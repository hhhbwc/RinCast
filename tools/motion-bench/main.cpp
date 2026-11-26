// Developer tool: how well does generating a frame from motion vectors predict the real next
// frame, compared with showing the last one again? Also times the estimator on this GPU.
//
//   motion-bench <image.jpg>
#define NOMINMAX
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <thread>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#include "rc/motion_field.h"
#include "rc/platform.h"
#include "../../pc/streamer/src/stream/motion_estimator.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr int W = 2496, H = 1312;

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    uint8_t* At(int x, int y) { return &rgba[size_t(y) * w * 4 + size_t(x) * 4]; }
    const uint8_t* At(int x, int y) const { return &rgba[size_t(y) * w * 4 + size_t(x) * 4]; }
};

Image Blank() {
    Image i;
    i.w = W;
    i.h = H;
    i.rgba.assign(size_t(W) * H * 4, 255);
    return i;
}

// `source` tiled, shifted by (ox, oy), drawn into the rectangle.
void Draw(Image& to, const Image& source, int rx, int ry, int rw, int rh, int ox, int oy) {
    for (int y = std::max(0, ry); y < std::min(H, ry + rh); y++) {
        for (int x = std::max(0, rx); x < std::min(W, rx + rw); x++) {
            int sx = (x + ox) % source.w, sy = (y + oy) % source.h;
            if (sx < 0) sx += source.w;
            if (sy < 0) sy += source.h;
            std::copy_n(source.At(sx, sy), 3, to.At(x, y));
        }
    }
}

struct Scene {
    const char* name;
    int pan;                     // the whole background, pixels per frame
    int spriteA, spriteB;        // moving objects, pixels per frame; 0 = absent
    bool noise;
    float yawDegrees = 0;        // head turn per frame
    bool expectedAccept = false;
    float minGainDb = 0.5f;
};

const float kFov[2][4] = {{1.376f, 0.839f, 0.966f, 1.191f}, {0.839f, 1.376f, 0.966f, 1.191f}};

Image Frame(const Scene& s, const Image& source, int t, std::mt19937& rng) {
    Image f = Blank();
    Draw(f, source, 0, 0, W, H, s.pan * t, 0);
    if (s.yawDegrees != 0) {
        // The image as a world around the head, seen through each eye after t turns.
        const float yaw = s.yawDegrees * t * 3.14159265f / 180.f, cy = std::cos(yaw), sy = std::sin(yaw);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const int eye = x >= W / 2;
                const float* c = kFov[eye];
                const float u = (x + 0.5f - eye * W / 2) / (W / 2), v = (y + 0.5f) / H;
                const float rx = -c[0] + u * (c[0] + c[1]), ry = c[2] - v * (c[2] + c[3]);
                const float wx = cy * rx - sy, wz = -sy * rx - cy;  // turned about y, looking along -z
                const float az = std::atan2(wx, -wz), el = std::atan2(ry, std::sqrt(wx * wx + wz * wz));
                int sx = int((az / 6.2831853f + 0.5f) * source.w * 3) % source.w;
                int sa = std::clamp(int((0.5f - el / 3.14159265f) * source.h), 0, source.h - 1);
                if (sx < 0) sx += source.w;
                std::copy_n(source.At(sx, sa), 3, f.At(x, y));
            }
        }
    }
    if (s.spriteA) Draw(f, source, 300 + s.spriteA * t, 250, 420, 340, 1200, 900 - (300 + s.spriteA * t));
    if (s.spriteB) Draw(f, source, 1800 - s.spriteB * t, 700, 300, 300, 400 + (1800 - s.spriteB * t), 100);
    if (s.noise) {
        std::uniform_int_distribution<int> d(-2, 2);
        for (auto& v : f.rgba) v = uint8_t(std::clamp(int(v) + d(rng), 0, 255));
        for (size_t i = 3; i < f.rgba.size(); i += 4) f.rgba[i] = 255;
    }
    return f;
}

double Psnr(const Image& a, const Image& b) {
    double sum = 0;
    int64_t n = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            for (int c = 0; c < 3; c++) {
                const double d = double(a.At(x, y)[c]) - b.At(x, y)[c];
                sum += d * d;
                n++;
            }
        }
    }
    return sum == 0 ? 99 : 10 * std::log10(255.0 * 255.0 / (sum / n));
}

// What the headset would do: pull each pixel from where the motion field says it is coming from.
Image Warp(const Image& cur, const std::vector<float>& field, int gw, int gh, bool blockCentered = true) {
    Image out = Blank();
    const int block = rcs::MotionEstimator::kBlockSize;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const float fx = (x + 0.5f) / block - (blockCentered ? 0.5f : 0.f);
            const float fy = (y + 0.5f) / block - (blockCentered ? 0.5f : 0.f);
            const int ix = std::clamp(int(std::floor(fx)), 0, gw - 1), iy = std::clamp(int(std::floor(fy)), 0, gh - 1);
            const int jx = std::min(ix + 1, gw - 1), jy = std::min(iy + 1, gh - 1);
            const float ax = std::clamp(fx - ix, 0.f, 1.f), ay = std::clamp(fy - iy, 0.f, 1.f);
            float mv[2];
            for (int c = 0; c < 2; c++) {
                const float a = field[(size_t(iy) * gw + ix) * 2 + c], b = field[(size_t(iy) * gw + jx) * 2 + c];
                const float d = field[(size_t(jy) * gw + ix) * 2 + c], e = field[(size_t(jy) * gw + jx) * 2 + c];
                mv[c] = (a + (b - a) * ax) * (1 - ay) + (d + (e - d) * ax) * ay;
            }
            const int sx = std::clamp(int(std::lround(x - mv[0])), 0, W - 1), sy = std::clamp(int(std::lround(y - mv[1])), 0, H - 1);
            std::copy_n(cur.At(sx, sy), 3, out.At(x, y));
        }
    }
    return out;
}

bool TestMotionMapping() {
    const int block = rcs::MotionEstimator::kBlockSize;
    const int gw = W / block, gh = H / block;
    Image cur = Blank();
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t* p = cur.At(x, y);
            p[0] = uint8_t(x);
            p[1] = uint8_t(x * 3);
            p[2] = uint8_t(y);
            p[3] = 255;
        }
    }
    std::vector<float> field(size_t(gw) * gh * 2, 0.f);
    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            float* v = &field[(size_t(y) * gw + x) * 2];
            v[0] = (x & 1) ? 16.f : 0.f;
            v[1] = (y & 1) ? 16.f : 0.f;
        }
    }
    const Image warped = Warp(cur, field, gw, gh);
    int failures = 0;
    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            const int px = x * block + block / 2, py = y * block + block / 2;
            const int sx = px - ((x & 1) ? 16 : 0), sy = py - ((y & 1) ? 16 : 0);
            if (std::memcmp(warped.At(px, py), cur.At(sx, sy), 3) != 0) failures++;
        }
    }
    std::printf("motion mapping: %d block-center samples %s\n", failures ? failures : gw * gh,
                failures ? "FAILED" : "matched exact source pixels");
    return failures == 0;
}

struct ReadbackStats {
    std::vector<double> activeMs, readMs, frameMs;
    uint32_t reads = 0, misses = 0, queueFailures = 0, overwritten = 0;
    uint32_t nonzeroReads = 0, minMovedBlocks = UINT32_MAX;
    uint32_t zeroReads = 0, warmupZeroReads = 0, firstZeroFrame = 0;
    uint32_t tagFailures = 0, lastTag = 0;
    uint32_t paired = 0, unpaired = 0, pairAttempts = 0;
    uint32_t lagSum = 0, lagMax = 0;
};

double Percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, size_t(fraction * (values.size() - 1)))];
}

double Mean(const std::vector<double>& values) {
    if (values.empty()) return 0;
    double sum = 0;
    for (double v : values) sum += v;
    return sum / values.size();
}

ReadbackStats BenchmarkReadback(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srvA,
                                ID3D11ShaderResourceView* srvB, bool async, int frames) {
    constexpr double periodMs = 1000.0 / 90.0;
    constexpr uint64_t timestampBaseNs = 1'000'000'000;
    constexpr uint64_t timestampStepNs = 11'111'111;
    const float turn[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    rcs::MotionEstimator motion;
    if (!motion.Init(device, W, H)) return {};
    motion.Process(ctx, srvA, turn, kFov);
    ReadbackStats stats;
    std::vector<float> field;
    int frameIndex = 0;
    uint32_t readbackSequence = 0;
    rc::MotionReadbackTiming queuedTiming;
    rc::MotionFieldMailbox mailbox;
    std::deque<std::pair<uint64_t, uint64_t>> encodingFrames;
    // Model the encoder behind the full four-slot readback pipeline; exact pairing must survive it.
    constexpr size_t encodeDelayFrames = rcs::MotionEstimator::kReadbackSlots - 1;
    auto process = [&] {
        const bool current = (frameIndex++ & 1) != 0;
        const bool valid = motion.Process(ctx, current ? srvB : srvA, turn, kFov);
        if (valid) {
            const uint64_t sequence = ++readbackSequence;
            const uint64_t intervalNs = (sequence & 1) ? 22'222'222 : 11'111'111;
            queuedTiming = {timestampBaseNs + sequence * timestampStepNs, intervalNs, uint64_t(frameIndex)};
        }
        return valid;
    };
    auto poll = [&] {
        const auto start = std::chrono::steady_clock::now();
        bool read = false;
        if (async) {
            if (!motion.QueueRead(ctx, queuedTiming)) stats.queueFailures++;
            ctx->Flush();
            bool consumed = false;
            rc::MotionReadbackTiming readTiming;
            read = motion.TryRead(ctx, field, readTiming, consumed);
            if (read) {
                const uint64_t sequence = readTiming.timestampNs >= timestampBaseNs
                                              ? uint32_t((readTiming.timestampNs - timestampBaseNs) / timestampStepNs)
                                              : 0;
                const uint64_t expectedNs = (sequence & 1) ? 22'222'222 : 11'111'111;
                if (!sequence || sequence <= stats.lastTag || readTiming.intervalNs != expectedNs ||
                    readTiming.timestampNs != timestampBaseNs + sequence * timestampStepNs)
                    stats.tagFailures++;
                stats.lastTag = sequence;
                const uint32_t lag = uint32_t(readbackSequence) - sequence;
                stats.lagSum += lag;
                stats.lagMax = std::max(stats.lagMax, lag);
                mailbox.Put(readTiming.frameIndex, readTiming.timestampNs, {uint8_t(sequence)}, uint64_t(rc::NowNs()));
            }
        } else {
            read = motion.Read(ctx, field);
        }
        stats.readMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        if (read) {
            stats.reads++;
            uint32_t moved = 0;
            for (size_t i = 0; i + 1 < field.size(); i += 2)
                if (field[i] != 0 || field[i + 1] != 0) moved++;
            if (moved) {
                stats.nonzeroReads++;
                stats.minMovedBlocks = std::min(stats.minMovedBlocks, moved);
            } else {
                stats.zeroReads++;
                if (frameIndex <= 30) stats.warmupZeroReads++;
                if (!stats.firstZeroFrame) stats.firstZeroFrame = uint32_t(frameIndex);
            }
        } else {
            stats.misses++;
        }
    };
    auto pairEncodedFrame = [&] {
        encodingFrames.push_back({queuedTiming.frameIndex, queuedTiming.timestampNs});
        if (encodingFrames.size() <= encodeDelayFrames) return;
        const auto [frameIndex, timestampNs] = encodingFrames.front();
        encodingFrames.pop_front();
        if (!async || !frameIndex || !timestampNs) return;
        stats.pairAttempts++;
        std::vector<uint8_t> packet;
        if (mailbox.TakeUnboundedForTest(frameIndex, timestampNs, packet)) {
            const uint64_t sequence = (timestampNs - timestampBaseNs) / timestampStepNs;
            if (packet.size() != 1 || packet[0] != uint8_t(sequence)) stats.tagFailures++;
            stats.paired++;
        } else {
            stats.unpaired++;
        }
    };
    for (int i = 0; i < 30; i++) {
        if (process()) poll();
        pairEncodedFrame();
    }
    const auto begin = std::chrono::steady_clock::now();
    auto deadline = begin + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(periodMs));
    for (int i = 0; i < frames; i++) {
        const auto frameStart = std::chrono::steady_clock::now();
        const auto workStart = std::chrono::steady_clock::now();
        if (process()) poll();
        pairEncodedFrame();
        stats.activeMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - workStart).count());
        deadline += std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(periodMs));
        const auto now = std::chrono::steady_clock::now();
        if (now > deadline + std::chrono::milliseconds(12)) deadline = now;
        std::this_thread::sleep_until(deadline);
        stats.frameMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count());
    }
    stats.overwritten = motion.OverwrittenReadbacks();
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    std::printf("%-7s %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f %8u %8u %8u %8u %8u %8.1f\n", async ? "async" : "sync",
                Mean(stats.activeMs), Percentile(stats.activeMs, 0.95), Percentile(stats.activeMs, 0.99), Mean(stats.readMs),
                Percentile(stats.readMs, 0.95), Percentile(stats.readMs, 0.99), Percentile(stats.frameMs, 0.99), stats.reads,
                stats.misses, stats.queueFailures, stats.overwritten, stats.tagFailures, frames / elapsed);
    std::printf("%-7s zero fields: %u total, %u during warmup, first at process %u\n", async ? "async" : "sync",
                stats.zeroReads, stats.warmupZeroReads, stats.firstZeroFrame);
    return stats;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return std::printf("usage: motion-bench <image> [--readback-frames N]\n"), 1;
    if (!TestMotionMapping()) return 2;
    int readbackFrames = 900;
    for (int i = 2; i < argc; i++) {
        if (std::string(argv[i]) == "--readback-frames" && i + 1 < argc) readbackFrames = std::max(1, std::atoi(argv[++i]));
    }
    Image source;
    int channels = 0;
    uint8_t* pixels = stbi_load(argv[1], &source.w, &source.h, &channels, 4);
    if (!pixels) return std::printf("can't read %s\n", argv[1]), 1;
    source.rgba.assign(pixels, pixels + size_t(source.w) * source.h * 4);
    stbi_image_free(pixels);

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx)))
        return std::printf("no device\n"), 1;
    ComPtr<IDXGIDevice> dxgi;
    device.As(&dxgi);
    ComPtr<IDXGIAdapter> adapter;
    dxgi->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC ad;
    adapter->GetDesc(&ad);
    std::printf("GPU: %ls\n", ad.Description);

    rcs::MotionEstimator me;
    if (!me.Init(device.Get(), W, H)) return std::printf("estimator failed\n"), 1;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W;
    td.Height = H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    device->CreateTexture2D(&td, nullptr, &tex);
    ComPtr<ID3D11ShaderResourceView> srv;
    device->CreateShaderResourceView(tex.Get(), nullptr, &srv);

    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ComPtr<ID3D11Query> disjoint, t0, t1;
    device->CreateQuery(&qd, &disjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    device->CreateQuery(&qd, &t0);
    device->CreateQuery(&qd, &t1);

    const Scene scenes[] = {
        {"object moving fast", 0, 24, 0, false, 0.f, true},
        {"two objects, different speeds", 0, 8, 40, false, 0.f, true},
        {"whole picture panning", 12, 0, 0, false, 0.f, true},
        {"objects over a pan", 6, 24, 16, false, 0.f, true},
        {"nothing moving, with noise", 0, 0, 0, true},
        // Noise and 4 px quantization leave less margin than the clean, faster scenes.
        {"slow motion, with noise", 0, 4, 2, true, 0.f, true, 0.25f},
        // Head-only motion is compensated before matching, so no generated warp is needed.
        {"head turning 1 degree a frame", 0, 0, 0, false, 1.f},
        {"head turning 3 degrees a frame", 0, 0, 0, false, 3.f},
    };
    std::printf("%-32s %10s %10s %10s %10s %8s %8s  %s\n", "scene", "repeat dB", "warp dB", "gated dB", "old-map dB", "GPU ms", "gate", "blocks moved");
    int sceneFailures = 0;
    for (const Scene& s : scenes) {
        std::mt19937 rng(7);
        // A direction from this frame's head, as the last frame's head saw it: one step less turned.
        const float back = s.yawDegrees * 3.14159265f / 180.f;
        const float turn[9] = {std::cos(back), 0, std::sin(back), 0, 1, 0, -std::sin(back), 0, std::cos(back)};
        Image prev = Frame(s, source, 0, rng), cur = Frame(s, source, 1, rng), next = Frame(s, source, 2, rng);
        ctx->UpdateSubresource(tex.Get(), 0, nullptr, prev.rgba.data(), W * 4, 0);
        me.Process(ctx.Get(), srv.Get(), turn, kFov);
        ctx->UpdateSubresource(tex.Get(), 0, nullptr, cur.rgba.data(), W * 4, 0);
        ctx->Begin(disjoint.Get());
        ctx->End(t0.Get());
        me.Process(ctx.Get(), srv.Get(), turn, kFov);
        ctx->End(t1.Get());
        ctx->End(disjoint.Get());
        std::vector<float> field;
        me.Read(ctx.Get(), field);
        std::vector<int8_t> grid;
        rc::ApplyMotionHysteresis(field, grid);
        const auto gate = rc::JudgeMotionField(grid, int(me.GridWidth()), int(me.GridHeight()));
        std::vector<float> gated(field.size(), 0.0f);
        if (gate.worth) {
            for (size_t i = 0; i < field.size(); i++) gated[i] = float(grid[i]) * rc::kMotionUnitPixels;
        }
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        while (ctx->GetData(disjoint.Get(), &dj, sizeof(dj), 0) != S_OK) {}
        UINT64 a = 0, b = 0;
        while (ctx->GetData(t0.Get(), &a, sizeof(a), 0) != S_OK) {}
        while (ctx->GetData(t1.Get(), &b, sizeof(b), 0) != S_OK) {}
        int moved = 0;
        for (size_t i = 0; i + 1 < field.size(); i += 2)
            if (field[i] != 0 || field[i + 1] != 0) moved++;
        const Image warped = Warp(cur, field, int(me.GridWidth()), int(me.GridHeight()));
        const Image gatedWarp = Warp(cur, gated, int(me.GridWidth()), int(me.GridHeight()));
        const Image oldMapWarp = Warp(cur, gated, int(me.GridWidth()), int(me.GridHeight()), false);
        const double repeatPsnr = Psnr(cur, next), warpPsnr = Psnr(warped, next), gatedPsnr = Psnr(gatedWarp, next);
        const double oldMapPsnr = Psnr(oldMapWarp, next);
        std::printf("%-32s %10.2f %10.2f %10.2f %10.2f %8.2f %8s  %d of %zu\n", s.name, repeatPsnr, warpPsnr,
                    gatedPsnr, oldMapPsnr, double(b - a) * 1000.0 / double(dj.Frequency), gate.worth ? "accept" : "reject", moved, field.size() / 2);
        if (gate.worth != s.expectedAccept ||
            (s.expectedAccept && gatedPsnr <= repeatPsnr + s.minGainDb)) {
            std::printf("FAIL: scene '%s': expected gate %s and accepted warp gain >%.2f dB; got %s, repeat %.2f, gated %.2f\n",
                        s.name, s.expectedAccept ? "accept" : "reject", s.minGainDb,
                        gate.worth ? "accept" : "reject", repeatPsnr, gatedPsnr);
            sceneFailures++;
        }
    }
    Image timingA = Blank(), timingB = Blank();
    Draw(timingA, source, 0, 0, W, H, -6, 0);
    Draw(timingB, source, 0, 0, W, H, 6, 0);
    D3D11_TEXTURE2D_DESC tdB = td;
    ComPtr<ID3D11Texture2D> texB;
    device->CreateTexture2D(&tdB, nullptr, &texB);
    ComPtr<ID3D11ShaderResourceView> srvB;
    device->CreateShaderResourceView(texB.Get(), nullptr, &srvB);
    ctx->UpdateSubresource(tex.Get(), 0, nullptr, timingA.rgba.data(), W * 4, 0);
    ctx->UpdateSubresource(texB.Get(), 0, nullptr, timingB.rgba.data(), W * 4, 0);
    std::printf("\n90 Hz readback benchmark (%d frames per mode; lower is better)\n", readbackFrames);
    std::printf("%-7s %9s %9s %9s %9s %9s %9s %9s %8s %8s %8s %8s %8s %8s\n", "mode", "active", "p95", "p99", "read", "read p95",
                "read p99", "frame p99", "reads", "misses", "qfail", "drop", "tagbad", "fps");
    const ReadbackStats sync = BenchmarkReadback(device.Get(), ctx.Get(), srv.Get(), srvB.Get(), false, readbackFrames);
    const ReadbackStats async = BenchmarkReadback(device.Get(), ctx.Get(), srv.Get(), srvB.Get(), true, readbackFrames);
    std::printf("async readback lag: %.2f frames average, %u maximum\n",
                async.reads ? double(async.lagSum) / async.reads : 0.0, async.lagMax);
    if (sceneFailures || sync.zeroReads != sync.warmupZeroReads || sync.minMovedBlocks <= 100 ||
        async.zeroReads != async.warmupZeroReads || async.reads < uint32_t(readbackFrames) * 0.8f ||
        async.minMovedBlocks <= 100 || async.queueFailures || async.tagFailures ||
        async.paired < async.pairAttempts * 0.8f || async.unpaired > async.overwritten ||
        async.paired + async.unpaired != async.pairAttempts) {
        std::printf("FAIL: %d scene gate/quality checks failed; readback missed fields, failed the queue/timing tag, returned zero motion, or paired the wrong frame (sync %u/%u, async %u/%u, paired %u, unpaired %u, min blocks %u, tag failures %u)\n",
                    sceneFailures, sync.nonzeroReads, sync.reads, async.nonzeroReads, async.reads,
                    async.paired, async.unpaired, async.minMovedBlocks == UINT32_MAX ? 0 : async.minMovedBlocks, async.tagFailures);
        return 2;
    }
    std::printf("PASS: async delivered %u nonzero fields with correct timing tags, paired %u exact encoded frames, dropped %u stale fields, %u unpaired frames used zero motion, no queue failure\n",
                async.reads, async.paired, async.overwritten, async.unpaired);
    return 0;
}
