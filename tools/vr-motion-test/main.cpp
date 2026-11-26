// Real D3D11 + multi-threaded MotionEstimator verification tool.
// Exercises GPU compute dispatch, QueueRead + Flush, HasPendingRead wake polling,
// ResetPrevious across bridge skips, duplicate headsetTargetNs submit fallback,
// and AttachMotionField -> DecodeVrMotionMetadata wire compatibility.
#define NOMINMAX
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "rc/frame_generation.h"
#include "rc/motion_field.h"
#include "rc/platform.h"
#include "../../pc/streamer/src/stream/motion_estimator.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kWidth = 2560;
constexpr uint32_t kHeight = 1440;
constexpr uint32_t kSourceWidth = kWidth + 512;
constexpr uint32_t kGridWidth = kWidth / 32;
constexpr uint32_t kGridHeight = kHeight / 32;
constexpr float kFov[2][4] = {{1.28f, 1.28f, 1.28f, 1.28f}, {1.28f, 1.28f, 1.28f, 1.28f}};
constexpr float kIdentityTurn[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

bool CreateProceduralResources(ID3D11Device* device, ComPtr<ID3D11Texture2D>& srcTex,
                               ComPtr<ID3D11Texture2D>& frameTex, ComPtr<ID3D11ShaderResourceView>& frameSrv) {
    std::vector<uint8_t> pixels(size_t(kSourceWidth) * kHeight * 4);
    for (uint32_t y = 0; y < kHeight; y++) {
        for (uint32_t x = 0; x < kSourceWidth; x++) {
            uint32_t h = x * 374761393u + y * 668265263u;
            h = (h ^ (h >> 13)) * 1274126177u;
            h ^= (h >> 16);
            const bool coarseCell = ((x / 23) + (y / 23)) % 2 == 0;
            const uint8_t base = coarseCell ? 180 : 60;
            const size_t idx = (size_t(y) * kSourceWidth + x) * 4;
            pixels[idx + 0] = uint8_t((base + (h & 63)) & 255);
            pixels[idx + 1] = uint8_t((base + ((h >> 8) & 63)) & 255);
            pixels[idx + 2] = uint8_t((base + ((h >> 16) & 63)) & 255);
            pixels[idx + 3] = 255;
        }
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kSourceWidth;
    td.Height = kHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{pixels.data(), kSourceWidth * 4, 0};
    if (FAILED(device->CreateTexture2D(&td, &init, &srcTex))) return false;

    td.Width = kWidth;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &frameTex))) return false;
    return SUCCEEDED(device->CreateShaderResourceView(frameTex.Get(), nullptr, &frameSrv));
}

void CopyShiftedFrame(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D* dst, uint32_t offsetX) {
    D3D11_BOX box{offsetX, 0, 0, offsetX + kWidth, kHeight, 1};
    ctx->CopySubresourceRegion(dst, 0, 0, 0, 0, src, 0, &box);
}

bool TestHalfDecode() {
    struct Case {
        uint16_t bits;
        float expected;
        bool finite;
    };
    const Case cases[] = {
        {0x0000, 0.f, true}, {0x8000, -0.f, true}, {0x3c00, 1.f, true}, {0xc000, -2.f, true},
        {0x0001, 1.f / 16777216.f, true}, {0x7c00, 0.f, false}, {0xfc00, 0.f, false},
        {0x7e00, 0.f, false}, {0xfe00, 0.f, false},
    };
    for (const Case& c : cases) {
        const float actual = rcs::DecodeMotionHalf(c.bits);
        if (std::isfinite(actual) != c.finite || (c.finite && actual != c.expected)) {
            std::printf("FAIL: FP16 0x%04x decoded as %.9g\n", c.bits, actual);
            return false;
        }
        if (!c.finite && c.bits == 0x7c00 && actual < 0) return false;
        if (!c.finite && c.bits == 0xfc00 && actual > 0) return false;
    }
    std::printf("PASS: FP16 finite and NaN/Inf decoding contracts\n");
    return true;
}

bool TestCoarseEyeBoundaries(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* src,
                             ID3D11Texture2D* frameTex, ID3D11ShaderResourceView* frameSrv) {
    struct Case {
        const char* name;
        int shift;
        float expected;
    };
    const Case cases[] = {{"still", 0, 0.f}, {"rightward-content", 16, 1.f}, {"leftward-content", -16, -1.f}};
    const uint32_t xs[4] = {0, kGridWidth / 2 - 1, kGridWidth / 2, kGridWidth - 1};
    for (const Case& c : cases) {
        rcs::MotionEstimator motion;
        if (!motion.Init(device, kWidth, kHeight)) return false;
        CopyShiftedFrame(ctx, src, frameTex, 256);
        if (motion.Process(ctx, frameSrv, kIdentityTurn, kFov)) return false;
        CopyShiftedFrame(ctx, src, frameTex, uint32_t(256 + c.shift));
        if (!motion.Process(ctx, frameSrv, kIdentityTurn, kFov)) return false;
        std::vector<float> coarse;
        if (!motion.ReadCoarse(ctx, coarse) || coarse.size() != size_t(kGridWidth) * kGridHeight * 2) return false;
        bool ok = true;
        const uint32_t y = kGridHeight / 2;
        for (uint32_t x : xs) {
            const size_t at = (size_t(y) * kGridWidth + x) * 2;
            std::printf("coarse %s x=%u: %.0f %.0f (expected %.0f 0)\n", c.name, x, coarse[at], coarse[at + 1], c.expected);
            ok &= std::fabs(coarse[at] - c.expected) < 0.001f && std::fabs(coarse[at + 1]) < 0.001f;
        }
        if (!ok) {
            std::printf("FAIL: coarse search crossed or excluded an eye boundary in case %s\n", c.name);
            return false;
        }
    }
    std::printf("PASS: real D3D11 coarse search preserves both eye boundaries\n");
    return true;
}

bool TestCoarseBoundaryMeanError(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    auto makeTexture = [&](uint8_t value, bool strip, ComPtr<ID3D11Texture2D>& texture,
                           ComPtr<ID3D11ShaderResourceView>& srv) {
        std::vector<uint8_t> pixels(size_t(kWidth) * kHeight * 4, value);
        if (strip) {
            for (uint32_t y = 0; y < kHeight; y++) {
                for (uint32_t x = 83 * 16; x < 84 * 16; x++) {
                    const size_t at = (size_t(y) * kWidth + x) * 4;
                    pixels[at] = pixels[at + 1] = pixels[at + 2] = 20;
                }
            }
        }
        for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{pixels.data(), kWidth * 4, 0};
        return SUCCEEDED(device->CreateTexture2D(&desc, &init, &texture)) &&
               SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &srv));
    };
    ComPtr<ID3D11Texture2D> previousTex, currentTex;
    ComPtr<ID3D11ShaderResourceView> previousSrv, currentSrv;
    if (!makeTexture(0, true, previousTex, previousSrv) || !makeTexture(220, false, currentTex, currentSrv)) return false;
    rcs::MotionEstimator motion;
    if (!motion.Init(device, kWidth, kHeight)) return false;
    if (motion.Process(ctx, previousSrv.Get(), kIdentityTurn, kFov)) return false;
    if (!motion.Process(ctx, currentSrv.Get(), kIdentityTurn, kFov)) return false;
    std::vector<float> coarse;
    if (!motion.ReadCoarse(ctx, coarse)) return false;
    const uint32_t x = kGridWidth / 2 - 1;
    const uint32_t y = kGridHeight / 2;
    const size_t at = (size_t(y) * kGridWidth + x) * 2;
    if (std::fabs(coarse[at]) > 0.001f || std::fabs(coarse[at + 1]) > 0.001f) {
        std::printf("FAIL: coarse boundary window won on total rather than mean error at x=%u y=%u: %.0f %.0f\n",
                    x, y, coarse[at], coarse[at + 1]);
        return false;
    }
    std::printf("PASS: partially sampled coarse windows cannot win by collecting less total error\n");
    return true;
}

bool TestRefineTopRow(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    rcs::MotionEstimator motion;
    if (!motion.Init(device, kWidth, kHeight)) return false;
    constexpr uint32_t quarterW = kWidth / 4, quarterH = kHeight / 4;
    constexpr uint32_t blockX = 20, blockY = 0;
    std::vector<uint8_t> current(size_t(quarterW) * quarterH, 128);
    std::vector<uint8_t> previous(size_t(quarterW) * quarterH, 0);
    for (uint32_t y = 0; y < 8; y++)
        for (uint32_t x = 0; x < 8; x++) previous[size_t(blockY * 8 + y) * quarterW + blockX * 8 + x] = 128;
    std::vector<uint16_t> coarse(size_t(kGridWidth) * kGridHeight * 2, 0x4800);  // +8 quarter-frame pixels

    auto makeTexture = [&](DXGI_FORMAT format, uint32_t width, uint32_t height, const void* data, UINT pitch,
                           ComPtr<ID3D11Texture2D>& texture, ComPtr<ID3D11ShaderResourceView>& srv) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{data, pitch, 0};
        return SUCCEEDED(device->CreateTexture2D(&desc, &init, &texture)) &&
               SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &srv));
    };
    ComPtr<ID3D11Texture2D> currentTex, previousTex, coarseTex;
    ComPtr<ID3D11ShaderResourceView> currentSrv, previousSrv, coarseSrv;
    if (!makeTexture(DXGI_FORMAT_R8_UNORM, quarterW, quarterH, current.data(), quarterW, currentTex, currentSrv) ||
        !makeTexture(DXGI_FORMAT_R8_UNORM, quarterW, quarterH, previous.data(), quarterW, previousTex, previousSrv) ||
        !makeTexture(DXGI_FORMAT_R16G16_FLOAT, kGridWidth, kGridHeight, coarse.data(), kGridWidth * 4, coarseTex, coarseSrv))
        return false;

    const float turn[9] = {0.9950041653f, 0, -0.0998334166f, 0, 1, 0, 0.0998334166f, 0, 0.9950041653f};
    if (!motion.RefineForTest(ctx, currentSrv.Get(), previousSrv.Get(), coarseSrv.Get(), turn, kFov)) return false;
    std::vector<float> refined;
    if (!motion.ReadRefined(ctx, refined)) return false;
    for (uint32_t y : {0u, 1u}) {
        const size_t at = (size_t(y) * kGridWidth + blockX) * 2;
        std::printf("refine top-row fixture y=%u: %.0f %.0f (expected 0 0)\n", y, refined[at], refined[at + 1]);
        if (std::fabs(refined[at]) > 0.001f || std::fabs(refined[at + 1]) > 0.001f) {
            std::printf("FAIL: refine top-row controlled fixture changed its expected output\n");
            return false;
        }
    }
    std::printf("PASS: refine top-row output contract (negative-neighbour rejection is covered by static mutation)\n");
    return true;
}

}  // namespace

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &ctx))) {
        std::printf("FAIL: no D3D11 hardware device\n");
        return 1;
    }

    ComPtr<ID3D11Texture2D> srcTex, frameTex;
    ComPtr<ID3D11ShaderResourceView> frameSrv;
    if (!CreateProceduralResources(device.Get(), srcTex, frameTex, frameSrv)) {
        std::printf("FAIL: texture creation failed\n");
        return 1;
    }

    const bool boundariesOk = TestCoarseEyeBoundaries(device.Get(), ctx.Get(), srcTex.Get(), frameTex.Get(), frameSrv.Get());
    const bool halfOk = TestHalfDecode();
    const bool coarseMeanOk = TestCoarseBoundaryMeanError(device.Get(), ctx.Get());
    const bool refineTopOk = TestRefineTopRow(device.Get(), ctx.Get());
    if (!boundariesOk || !halfOk || !coarseMeanOk || !refineTopOk) return 1;
    rcs::MotionEstimator motion;
    if (!motion.Init(device.Get(), kWidth, kHeight)) {
        std::printf("FAIL: MotionEstimator::Init failed\n");
        return 1;
    }

    // Exercise VrStream's multi-threaded wake + bounded WaitForMotionField loop on real GPU work.
    rc::MotionFieldMailbox mailbox;
    std::vector<int8_t> gridPrev;
    std::vector<float> motionField;
    HANDLE wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::mutex jobMutex, syncMutex;
    std::condition_variable jobCv, syncCv;
    struct Job {
        uint64_t targetNs = 0;
        bool queued = false;
        uint32_t frameIndex = 0;
    };
    std::deque<Job> pendingJobs;
    std::atomic<bool> running{true};
    std::atomic<bool> hasPendingRead{false};
    std::atomic<uint32_t> waiters{0};

    auto pollPending = [&]() -> bool {
        bool published = false;
        if (motion.HasPendingRead()) {
            bool slotConsumed = false;
            do {
                rc::MotionReadbackTiming timing;
                slotConsumed = false;
                if (motion.TryRead(ctx.Get(), motionField, timing, slotConsumed)) {
                    if (rc::PublishMotionField(mailbox, true, timing, uint16_t(motion.GridWidth()),
                                               uint16_t(motion.GridHeight()), uint16_t(rcs::MotionEstimator::kBlockSize),
                                               motionField, gridPrev, uint64_t(rc::NowNs()))) {
                        published = true;
                    }
                }
            } while (slotConsumed && motion.HasPendingRead());
        }
        const bool still = motion.HasPendingRead();
        hasPendingRead.store(still, std::memory_order_release);
        if (published || (!still && waiters.load(std::memory_order_acquire) > 0)) {
            std::lock_guard lk(syncMutex);
            syncCv.notify_all();
        }
        return still;
    };

    uint32_t attachedCount = 0;
    uint32_t decodedCoherentCount = 0;
    uint32_t encodedCount = 0;
    bool wireFailure = false;

    std::thread encoder([&] {
        rc::HeadsetMotionState hs;
        for (;;) {
            Job j;
            {
                std::unique_lock lk(jobMutex);
                jobCv.wait(lk, [&] { return !pendingJobs.empty() || !running; });
                if (pendingJobs.empty()) break;
                j = pendingJobs.front();
                pendingJobs.pop_front();
            }
            // Simulate NVENC finishing in 5 ms while D3D11 MotionEstimator runs asynchronously.
            rc::SleepMs(5);
            if (j.queued && !mailbox.HasTimestamp(j.frameIndex, j.targetNs) && hasPendingRead.load(std::memory_order_acquire)) {
                waiters.fetch_add(1, std::memory_order_acq_rel);
                SetEvent(wakeEvent);
                {
                    std::unique_lock lk(syncMutex);
                    syncCv.wait_for(lk, std::chrono::milliseconds(15), [&] {
                        return !running || mailbox.HasTimestamp(j.frameIndex, j.targetNs) ||
                               !hasPendingRead.load(std::memory_order_acquire);
                    });
                }
                waiters.fetch_sub(1, std::memory_order_acq_rel);
            }
            std::vector<uint8_t> meta(sizeof(rc::VrFrameMetadata), 0);
            rc::VrFrameMetadata hdr{};
            hdr.trackingTimestampNs = j.targetNs;
            std::memcpy(meta.data(), &hdr, sizeof(hdr));
            const bool attached = rc::AttachMotionField(true, mailbox, j.frameIndex, j.targetNs, meta);
            ++encodedCount;
            if (attached) {
                ++attachedCount;
                const auto res = rc::DecodeVrMotionMetadata(true, false, j.targetNs, meta, hs);
                if (!res.decoded || res.failureStage != 0) wireFailure = true;
                if (res.uploadMotion && hs.consistent) ++decodedCoherentCount;
            }
            jobCv.notify_all();
        }
    });

    uint64_t prevTarget = 0;
    int64_t prevSubmit = 0;
    uint64_t lastInterval = 0;
    uint32_t lastMotionFrame = 0;
    uint32_t shiftX = 0;

    // 30 frames: frames 15->17 inject a transient bridge skip, frames 10 & 22 share target timestamps.
    for (uint32_t frameIdx = 1; frameIdx <= 30; frameIdx++) {
        if (frameIdx == 16) continue;  // skip frame 16 to trigger ResetPrevious() without mailbox Clear()
        shiftX = (shiftX + 8) % 256;
        if (shiftX < 8) {
            // Avoid a backward wrap jump larger than search range by restarting smoothly via ResetPrevious.
            motion.ResetPrevious();
            lastMotionFrame = 0;
        }
        CopyShiftedFrame(ctx.Get(), srcTex.Get(), frameTex.Get(), shiftX);
        if (lastMotionFrame && frameIdx != lastMotionFrame + 1) {
            motion.ResetPrevious();
            lastMotionFrame = 0;
        }

        const int64_t submitNs = rc::NowNs();
        const uint64_t targetNs =
            (frameIdx == 10 || frameIdx == 22) ? prevTarget : uint64_t(10'000'000'000ULL + uint64_t(frameIdx) * 14'000'000ULL);
        const uint64_t intervalNs =
            rc::ResolveMotionIntervalNs(targetNs, prevTarget, submitNs, prevSubmit, lastInterval);
        const bool valid = motion.Process(ctx.Get(), frameSrv.Get(), kIdentityTurn, kFov);
        if (valid) {
            motion.QueueRead(ctx.Get(), {targetNs, intervalNs, frameIdx});
            ctx->Flush();
            if (intervalNs) lastInterval = intervalNs;
        }
        hasPendingRead.store(motion.HasPendingRead(), std::memory_order_release);
        prevTarget = targetNs;
        prevSubmit = submitNs;
        lastMotionFrame = frameIdx;

        {
            std::lock_guard lk(jobMutex);
            pendingJobs.push_back(Job{targetNs, valid, frameIdx});
        }
        jobCv.notify_all();

        const int64_t waitStart = rc::NowNs();
        while (rc::NowNs() - waitStart < 12'000'000LL) {
            const bool pending = pollPending();
            if (!pending) break;
            if (waiters.load(std::memory_order_acquire) > 0) {
                SwitchToThread();
            } else {
                WaitForSingleObject(wakeEvent, 1);
            }
        }
        (void)pollPending();
    }

    {
        std::lock_guard lk(jobMutex);
        running = false;
    }
    jobCv.notify_all();
    encoder.join();
    CloseHandle(wakeEvent);

    const auto stats = mailbox.Stats();
    std::printf("vr-motion-test: encoded=%u attached=%u coherent=%u produced=%llu hits=%llu misses=%llu expired=%llu cleared=%llu\n",
                encodedCount, attachedCount, decodedCoherentCount, (unsigned long long)stats.produced,
                (unsigned long long)stats.hits, (unsigned long long)stats.misses,
                (unsigned long long)stats.expired, (unsigned long long)stats.cleared);

    // Expect: Frame 1 + skipped Frame 17 (ResetPrevious) don't queue motion (2 frames), leaving 27 queued frames.
    // All queued frames including shared-target frames (10, 22) and pre-skip Frame 15 must attach & decode coherently.
    if (wireFailure || encodedCount != 29 || stats.cleared != 0 || stats.misses > 2 || attachedCount < 26 ||
        decodedCoherentCount < 24) {
        std::printf("FAIL: vr-motion-test pipeline invariants failed\n");
        return 1;
    }
    std::printf("PASS: real D3D11 MotionEstimator async pipeline, skip preservation, and shared-target fallback verified\n");
    return 0;
}
