#include "vr_stream.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "foveator.h"
#include "motion_estimator.h"
#include "rc/fovea_placement.h"
#include "rc/motion_field.h"
#include "rc/stale_flush.h"

#include <avrt.h>

#include "../vr/vr_bridge_host.h"
#include "rc/platform.h"

namespace rcs {

namespace {

// Rotates a direction seen from `now` into what it was seen as from `before`.
void HeadTurn(const rcvr::BridgePose& before, const rcvr::BridgePose& now, float m[9]) {
    const float* a = before.orientation;  // x, y, z, w; conjugated below
    const float* b = now.orientation;
    const float ax = -a[0], ay = -a[1], az = -a[2], aw = a[3];
    const float x = aw * b[0] + ax * b[3] + ay * b[2] - az * b[1];
    const float y = aw * b[1] - ax * b[2] + ay * b[3] + az * b[0];
    const float z = aw * b[2] + ax * b[1] - ay * b[0] + az * b[3];
    const float w = aw * b[3] - ax * b[0] - ay * b[1] - az * b[2];
    m[0] = 1 - 2 * (y * y + z * z);
    m[1] = 2 * (x * y - z * w);
    m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w);
    m[4] = 1 - 2 * (x * x + z * z);
    m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w);
    m[7] = 2 * (y * z + x * w);
    m[8] = 1 - 2 * (x * x + y * y);
}

void AppendMotion(const MotionEstimator& motion, const std::vector<float>& field, uint64_t previousTimestampNs,
                  std::vector<int8_t>& grid, std::vector<uint8_t>& out) {
    // A18: `grid` is the quantizer's hysteresis state - the caller keeps it across frames
    // (all zero on the first frame and after every estimator reset), it comes in holding the
    // previous frame's units and leaves with this frame's. The unit, the wire format and
    // EncodeMotionRuns are untouched: old APKs decode this byte for byte as before.
    rc::ApplyMotionHysteresis(field, grid);
    rc::MotionHeader h{};
    h.gridWidth = uint16_t(motion.GridWidth());
    h.gridHeight = uint16_t(motion.GridHeight());
    h.blockSize = uint16_t(MotionEstimator::kBlockSize);
    h.previousTimestampNs = previousTimestampNs;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&h);
    out.insert(out.end(), bytes, bytes + sizeof(h));
    rc::EncodeMotionRuns(grid.data(), field.size() / 2, out);
}

// For what can go wrong on every frame: each reason at most once every 5 seconds, with how
// often it happened since it was last logged.
void WarnEvery5s(const char* reason) {
    static std::mutex mutex;
    static std::map<const char*, std::pair<int64_t, uint32_t>> seen;  // last logged, count since
    std::lock_guard lock(mutex);
    auto& [last, count] = seen[reason];
    count++;
    const int64_t now = rc::NowNs();
    if (last && now - last < 5'000'000'000LL) return;
    RC_LOGW("%s (%u times)", reason, count);
    last = now;
    count = 0;
}

// A26 (stale-frame flush): how long after the driver's submit the headset expects to
// show the frame, when the frame's exact prediction is unavailable. The same 40 ms
// client_session falls back to when no clock offset is known yet.
constexpr int64_t kStalePredictAheadNs = 40'000'000;
// A26: a prediction further before the driver's submit than this means the clock
// mapping behind it is off (a frame cannot be displayed before it was rendered);
// such a value is not trusted and the predict-ahead estimate is used instead.
constexpr int64_t kStaleClockGuardNs = 5'000'000;

}  // namespace

// The full-density square follows the eyes, but only moves once they look clearly elsewhere:
// while it stays put, the encoder can keep referring to the frames before. Without a recent
// gaze, it sits where each eye's lens is sharpest.
void VrStream::PlaceFovea(const float fovTan[2][4], float start[2][2]) {
    rc::GazePayload g;
    int64_t at;
    {
        std::lock_guard lock(m_gazeMutex);
        g = m_gaze;
        at = m_gazeNs;
    }
    const bool fresh = at && rc::NowNs() - at < 250'000'000LL;
    for (int eye = 0; eye < 2; eye++) {
        // The eye's own gaze, or the other one's while it blinks.
        const int from = (g.flags & (1 << eye)) ? eye : (g.flags & (1 << (1 - eye))) ? 1 - eye : -1;
        const float* gaze = fresh && from >= 0 ? g.gaze[from] : nullptr;
        rc::FoveaSquare(fovTan[eye], gaze, m_config.foveaSize, m_foveaPlaced, m_foveaStart[eye]);
        start[eye][0] = m_foveaStart[eye][0];
        start[eye][1] = m_foveaStart[eye][1];
    }
    m_foveaPlaced = true;
}

VrStream::VrStream(rc::Connection* connection, VrBridgeHost* bridge, const VrStreamConfig& config,
                   StateHandler onState)
    : m_connection(connection),
      m_bridge(bridge),
      m_config(config),
      m_sender(connection, rc::kVrStreamId, config.codec, config.bitrateKbps, config.fecPercent, config.adaptive,
               config.profile),
      m_onState(std::move(onState)) {
    if (config.startBitrateKbps) m_sender.StartAt(config.startBitrateKbps);
}

VrStream::~VrStream() { Stop(); }


bool VrStream::Start() {
    m_running = true;
    m_sendThread = std::thread([this] { RunSender(); });
    m_encodeThread = std::thread([this] { RunEncoder(); });
    m_thread = std::thread([this] { Run(); });
    return true;
}

void VrStream::Stop() {
    m_running = false;
    // Under the locks: a thread between checking m_running and waiting would miss the wake-up.
    { std::lock_guard lock(m_jobMutex); }
    { std::lock_guard lock(m_sendMutex); }
    m_jobReady.notify_all();
    m_jobTaken.notify_all();
    m_sendCv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    if (m_encodeThread.joinable()) m_encodeThread.join();
    if (m_sendThread.joinable()) m_sendThread.join();
}

static const char* ProducerName(rcvr::ProducerKind k) {
    switch (k) {
        case rcvr::ProducerKind::SteamVR: return "steamvr";
        case rcvr::ProducerKind::OpenXR: return "openxr";
        default: return "";
    }
}

void VrStream::LogFrameFlow(int64_t now) {
    // G10.1: a once-a-second line that puts every key quantity on ONE clock. The other
    // logs each run their own 5/10 s window with a different start, so they never line up;
    // this one is for correlating "when it pumped, what were target/queue/half-rate doing".
    if (!m_diagSinceNs) m_diagSinceNs = now;
    if (now - m_diagSinceNs >= 1'000'000'000LL) {
        m_diagSinceNs = now;
        const VideoSenderStats s = Stats();
        // ROUND19 T1: frame-level identity + encode-time distribution for the window just
        // ended (telemetry only; reads the counters the send path keeps above).
        const uint32_t end = m_diagEncIdx.load(std::memory_order_relaxed);
        const uint32_t count = std::min<uint32_t>(m_diagEncCount.exchange(0, std::memory_order_relaxed), uint32_t(kDiagRing));
        std::vector<std::pair<uint32_t, double>> frames;  // (seq, encodeMs), window order
        frames.reserve(count);
        for (uint32_t i = end - count; i < end; i++)
            frames.emplace_back(m_diagSeqRing[i % kDiagRing].load(std::memory_order_relaxed),
                                m_diagEncUs[i % kDiagRing].load(std::memory_order_relaxed) / 1000.0);
        const uint32_t seqMin = m_diagSeqMin.exchange(0, std::memory_order_relaxed);
        const uint32_t seqMax = m_diagSeqMax.exchange(0, std::memory_order_relaxed);
        std::vector<double> encSorted;
        encSorted.reserve(frames.size());
        for (auto& f : frames) encSorted.push_back(f.second);
        std::sort(encSorted.begin(), encSorted.end());
        const double p50 = encSorted.empty() ? 0.0 : encSorted[size_t((encSorted.size() - 1) * 50 / 100)];
        const double p95 = encSorted.empty() ? 0.0 : encSorted[size_t((encSorted.size() - 1) * 95 / 100)];
        const double periodMs = m_config.fps > 0 ? 1000.0 / m_config.fps : 0.0;
        char seqPart[32];
        if (seqMax) snprintf(seqPart, sizeof(seqPart), ", seq %u-%u", seqMin, seqMax);
        else snprintf(seqPart, sizeof(seqPart), ", seq -");
        RC_LOGI("rc diag 1s: game %.0f/s enc %.0f/s drop %.0f/s, halfRate %s, target %u kbps, wire %.1f Mbps, "
                "queue %.0f ms, qp %.1f, render %ux%u enc %ux%u%s, enc p50/p95 %.1f/%.1f ms",
                s.gameFps, s.encFps, s.droppedFps, m_halfRateActive.load(std::memory_order_relaxed) ? "ON" : "OFF",
                m_sender.TargetBitrateKbps(), s.mbps, s.queueMs, s.qp, s.srcWidth, s.srcHeight, s.encWidth, s.encHeight,
                seqPart, p50, p95);
        // ROUND19 T1: sparse slow-frame trace, only under pressure, at most 8 per window.
        if ((s.droppedFps > 0.0 || (periodMs > 0 && p95 > periodMs)) && !frames.empty()) {
            std::vector<std::pair<uint32_t, double>> worst(frames);
            std::sort(worst.begin(), worst.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            if (worst.size() > 8) worst.resize(8);
            std::string seqs, encs;
            char part[48];
            for (auto& f : worst) {
                snprintf(part, sizeof(part), "%s%u", seqs.empty() ? "" : ",", f.first);
                seqs += part;
                snprintf(part, sizeof(part), "%s%.1f", encs.empty() ? "" : ",", f.second);
                encs += part;
            }
            RC_LOGI("slow-frames seq=%s enc=%s ms", seqs.c_str(), encs.c_str());
        }
    }

    if (!m_flowSinceNs) m_flowSinceNs = now;
    if (now - m_flowSinceNs < 10'000'000'000LL) return;
    const double secs = double(now - m_flowSinceNs) / 1e9;
    m_flowSinceNs = now;
    const uint32_t given = m_flowGiven.exchange(0), bridge = m_flowReplacedBridge.exchange(0),
                   queue = m_flowReplacedQueue.exchange(0), encoded = m_flowEncoded.exchange(0);
    const uint32_t staleDrops = m_flowStaleDrops.exchange(0);  // A26: the window's flush count
    const int64_t convertNs = m_flowConvertNs.exchange(0), encodeNs = m_flowEncodeNs.exchange(0),
                  heldNs = m_flowHeldNs.exchange(0);
    if (!given) return;
    const uint32_t converted = given - bridge;
    // Live rates for the stats path (atomics: Stats() reads from another thread).
    m_lastGameFps.store(double(given) / secs, std::memory_order_relaxed);
    m_lastEncFps.store(double(encoded) / secs, std::memory_order_relaxed);
    m_lastDroppedFps.store(double(queue) / secs, std::memory_order_relaxed);
    // G10.3: arrival-gap and keyed-mutex wait spread, reset each 10 s window.
    const uint32_t gapMin = m_diagGapMin, gapMax = m_diagGapMax;
    const uint32_t lockN = m_diagLockSamples;
    const double lockAvgMs = lockN ? double(m_diagLockSumNs) / lockN / 1e6 : 0.0;
    const double lockMaxMs = double(m_diagLockMaxNs) / 1e6;
    m_diagGapMin = 0;
    m_diagGapMax = 0;
    m_diagLockSamples = 0;
    m_diagLockSumNs = 0;
    m_diagLockMaxNs = 0;
    RC_LOGI("VR frames: %.0f/s from the game, %.0f/s encoded; %.0f/s replaced before conversion, %.0f/s while "
            "the encoder was busy; conversion %.1f ms, encoding %.1f ms, held for the encoder %.1f ms, "
            "arrivalGap %u-%u ms, lockWait avg %.1f/max %.1f ms (%u)",
            given / secs, encoded / secs, bridge / secs, queue / secs,
            converted ? double(convertNs) / converted / 1e6 : 0.0, encoded ? double(encodeNs) / encoded / 1e6 : 0.0,
            converted ? double(heldNs) / converted / 1e6 : 0.0,
            gapMin, gapMax, lockAvgMs, lockMaxMs, lockN);
    // A26: the window's stale-flush count, on its own line and only when it fired -
    // the steady state's log stream stays byte-for-byte as it was.
    if (staleDrops)
        RC_LOGI("VR stale flush: %u frame(s) dropped past their display moment in this 10 s window", staleDrops);
}

void VrStream::WaitForEncoder(int64_t convertAvgNs) {
    const int64_t start = rc::NowNs();
    {
        std::unique_lock lock(m_jobMutex);
        // The frame before still waits for the encoder: one converted now would only replace it.
        if (m_job) m_jobTaken.wait_for(lock, std::chrono::milliseconds(100), [this] { return !m_running || !m_job; });
        if (!m_encoding || m_job) {
            m_flowHeldNs += rc::NowNs() - start;
            return;
        }
    }
    // The encoder is busy: take the game's next frame so its conversion ends about when the
    // encoder is free. Taken sooner, it would wait for the encoder and arrive older. Meanwhile the
    // frame stays in the bridge, and the driver holds the game until it's taken: the game renders
    // at the rate the encoder keeps up with, not frames that would be thrown away.
    const int64_t readyAt = m_encodeStartNs + m_encodeAvgNs - convertAvgNs - 1'000'000;
    const int64_t wait = std::min<int64_t>(readyAt - rc::NowNs(), 20'000'000);
    if (wait >= 1'000'000) rc::SleepMs(int(wait / 1'000'000));
    m_flowHeldNs += rc::NowNs() - start;
}

void VrStream::Run() {
    ScopedCoInit com;
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);  // highest MMCSS class
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);

    ID3D11Device* device = m_bridge->Device();
    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    rcvr::Shared* shared = m_bridge->Shared();
    // The conversion queues behind the game's GPU work otherwise; with a busy GPU it waits most
    // of a frame. A relative priority, within this process's class: no privilege needed.
    {
        static std::once_flag logged;
        ComPtr<IDXGIDevice> dxgi;
        const HRESULT hr = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) ? dxgi->SetGPUThreadPriority(7) : E_NOINTERFACE;
        std::call_once(logged, [hr] { RC_LOGI("VR: GPU priority for conversion %s", SUCCEEDED(hr) ? "raised" : "unchanged (refused)"); });
    }

    // Three YUV targets: one the encoder reads, one waiting for it, one being converted into.
    ColorConverter converters[3];
    Foveator foveator;
    const bool foveated = m_config.foveatedEyeWidth && m_config.foveatedEyeHeight;
    uint32_t srcWidth = 0, srcHeight = 0;  // the frame from the game
    uint32_t encWidth = 0, encHeight = 0;  // what's encoded: smaller when foveated
    bool convertersReady = false;
    constexpr double kHalfRateBelowBpp = 0.03, kFullRateAboveBpp = 0.05;
    bool halfRate = false, skipNext = false;
    m_sender.SetPacingMode(m_config.pacingMode, 0);
    uint64_t lastFrameIndex = 0;
    int64_t lastFrameNs = 0;
    bool streaming = false;
    rcvr::ProducerKind lastProducer = rcvr::ProducerKind::None;
    std::string lastApp;
    MotionEstimator motion;
    bool motionReady = false;
    std::map<ID3D11Texture2D*, ComPtr<ID3D11ShaderResourceView>> motionViews;
    rcvr::BridgePose motionPose{};
    uint64_t motionPreviousTimestamp = 0;
    std::vector<float> motionField;
    // A18: the quantized grid lives on in m_motionGridPrev (the hysteresis state) instead of
    // a local, per the card: the cross-frame state belongs to VrStream.
    uint32_t lastTextureGeneration = m_bridge->TextureGeneration();
    // A61: a producer that restarted starts its frame counter over (driver.cpp resets it
    // per vrserver process) and the textures may be a new generation. Everything derived
    // from the old producer's frames has to go with it - above all lastFrameIndex, which
    // otherwise rejects every new frame as stale for the rest of the session.
    const auto resetProducerCaches = [&] {
        lastFrameIndex = 0;
        m_diagArrivalPrevNs = 0;
        motionViews.clear();  // the SRVs pin the previous generation's textures
        motion.Reset();
        m_motionGridPrev.clear();
        motionPose = rcvr::BridgePose{};
        motionPreviousTimestamp = 0;
    };

    m_onState("waiting", "", "");
    bool firstFrameLogged = false;
    int64_t lastWaitReportNs = rc::NowNs();
    int64_t convertAvgNs = 0;
    while (m_running) {
        WaitForSingleObject(m_bridge->FrameEvent(), 50);
        // Not before the encoder is about to want it; the frame waits in the bridge, the game with it.
        if (streaming && !m_paused) WaitForEncoder(convertAvgNs);
        const int64_t now = rc::NowNs();
        // A61: dead-producer recovery and a resolution change both hand out new textures.
        if (const uint32_t generation = m_bridge->TextureGeneration(); generation != lastTextureGeneration) {
            RC_LOGI("VR: bridge textures recreated (generation %u -> %u), restarting frame accounting",
                    lastTextureGeneration, generation);
            lastTextureGeneration = generation;
            resetProducerCaches();
        }
        // No picture: say every 10 s what the bridge looks like, so a log shows whether the
        // driver never sent a frame or frames arrive and get dropped here.
        if (!streaming && now - lastWaitReportNs > 10'000'000'000LL) {
            lastWaitReportNs = now;
            RC_LOGW("VR: no frame sent for 10 s. Producer %s (pid %ld), latest slot %lld, slots %ld/%ld/%ld, "
                    "heartbeat %.1f s ago, encoder %s",
                    ProducerName(m_bridge->ActiveProducer()), long(shared->producerPid), (long long)shared->latestReadySlot,
                    long(shared->slots[0].state), long(shared->slots[1].state), long(shared->slots[2].state),
                    shared->producerHeartbeatNs ? double(rcvr::QpcNowNs() - shared->producerHeartbeatNs) / 1e9 : -1.0,
                    convertersReady ? "ready" : "not created");
        }
        if (streaming && !m_paused) LogFrameFlow(now);

        const rcvr::ProducerKind producer = m_bridge->ActiveProducer();
        const std::string app = m_bridge->ApplicationName();
        const bool fresh = now - lastFrameNs < 1'000'000'000LL;
        if (streaming && !fresh) {
            streaming = false;
            m_streaming = false;
            lastWaitReportNs = now;
            m_onState("waiting", ProducerName(producer), app);
        }
        if (producer != lastProducer || app != lastApp) {
            lastProducer = producer;
            lastApp = app;
            m_onState(streaming ? "running" : "waiting", ProducerName(producer), app);
        }

        const LONG64 latest = shared->latestReadySlot;
        if (latest < 0) continue;
        const int slot = int(latest & 0xFF);
        if (slot >= rcvr::kFrameSlots) continue;
        rcvr::FrameSlot& fs = shared->slots[slot];
        if (InterlockedCompareExchange(&fs.state, rcvr::kSlotReading, rcvr::kSlotReady) != rcvr::kSlotReady) continue;
        // A61: a restarted producer's counter runs backwards. Read only after a successful
        // CAS: before it, the slot's index is whatever the last wake already consumed.
        if (lastFrameIndex != 0 && fs.frameIndex <= lastFrameIndex) {
            RC_LOGI("VR: producer restarted (frame %llu -> %llu), restarting frame accounting",
                    (unsigned long long)lastFrameIndex, (unsigned long long)fs.frameIndex);
            resetProducerCaches();
        }
        // Frames the game finished that were replaced before this one was taken.
        if (lastFrameIndex && !m_paused) {
            const uint64_t given = fs.frameIndex - lastFrameIndex;
            m_flowGiven += uint32_t(given);
            m_flowReplacedBridge += uint32_t(given - 1);
            // G10.3: arrival gap distribution. A steady fps with a bimodal gap is
            // hidden by the mean; min..max in the 10 s line makes it visible.
            if (m_diagArrivalPrevNs) {
                const uint32_t gapMs = uint32_t((now - m_diagArrivalPrevNs) / 1'000'000);
                if (m_diagGapMin == 0 || gapMs < m_diagGapMin) m_diagGapMin = gapMs;
                if (gapMs > m_diagGapMax) m_diagGapMax = gapMs;
            }
            m_diagArrivalPrevNs = now;
        }

        if (m_paused) {
            lastFrameIndex = fs.frameIndex;
            lastFrameNs = now;
            InterlockedExchange(&fs.state, rcvr::kSlotFree);
            motion.Reset();
            // A18: the hysteresis state follows the estimator's - the next frame compares
            // against nothing, so every block starts over from still.
            m_motionGridPrev.clear();
            if (!streaming) {
                streaming = true;
                m_streaming = true;
                m_onState("running", ProducerName(producer), app);
            }
            continue;
        }

        ComPtr<ID3D11Texture2D> texRef = m_bridge->SlotTexture(slot);
        ID3D11Texture2D* tex = texRef.Get();
        if (!tex) {
            WarnEvery5s("VR frame dropped: the frame's texture isn't available");
            InterlockedExchange(&fs.state, rcvr::kSlotFree);
            continue;
        }
        D3D11_TEXTURE2D_DESC desc;
        tex->GetDesc(&desc);
        if (!firstFrameLogged) {
            firstFrameLogged = true;
            RC_LOGI("VR: first frame from %s (%ux%u, format %u)", ProducerName(producer), desc.Width, desc.Height,
                    unsigned(desc.Format));
        }
        if (!convertersReady || srcWidth != (desc.Width & ~1u) || srcHeight != (desc.Height & ~1u)) {
            srcWidth = desc.Width & ~1u;
            srcHeight = desc.Height & ~1u;
            // ROUND18 L2: encWidth/Height are what the ENCODER sees; srcWidth/Height
            // stay the render size (displayed on the headset, and logged separately).
            // encodeScale is applied to the final encode size, after foveation, so a
            // single rule covers both paths. NV12/P010 need even dimensions.
            {
                const uint32_t baseW = foveated ? m_config.foveatedEyeWidth * 2 : srcWidth;
                const uint32_t baseH = foveated ? m_config.foveatedEyeHeight : srcHeight;
                // ROUND18 L2-FIX2: same helper as the foveation/coded-size metadata,
                // so what we tell the headset is byte-for-byte what the encoder sees.
                encWidth = rc::ScaledEncodeSize(baseW, m_config.encodeScale);
                encHeight = rc::ScaledEncodeSize(baseH, m_config.encodeScale);
            }
            m_statSrcW.store(srcWidth, std::memory_order_relaxed);
            m_statSrcH.store(srcHeight, std::memory_order_relaxed);
            convertersReady = false;
            if (foveated && !foveator.Init(device, srcWidth / 2, srcHeight, encWidth / 2, encHeight, m_config.foveaSize)) {
                WarnEvery5s("VR frame dropped: foveated encoding didn't start");
                InterlockedExchange(&fs.state, rcvr::kSlotFree);
                continue;
            }
            bool ok = true;
            for (auto& c : converters) ok = ok && c.Init(device, encWidth, encHeight, m_config.tenBit);
            if (!ok) {
                WarnEvery5s("VR frame dropped: the colour converter didn't start");
                InterlockedExchange(&fs.state, rcvr::kSlotFree);
                continue;
            }
            convertersReady = true;
            motionViews.clear();
            motionReady = m_config.motionField && motion.Init(device, srcWidth, srcHeight);
            if (m_config.motionField && !motionReady) RC_LOGW("Frame generation: the motion estimator didn't start");
            // A18: a new estimator geometry invalidates the old grid's blocks - the hysteresis
            // state restarts from all-still with it (the next AppendMotion resizes it fresh).
            m_motionGridPrev.clear();
        }
        ComPtr<IDXGIKeyedMutex> mutex;
        tex->QueryInterface(IID_PPV_ARGS(&mutex));
        // G10.3: time the keyed-mutex acquire (observation only - timeout and the
        // success/failure decision are untouched). A long acquire is the direct sign
        // the driver still holds the shared texture, which used to be invisible.
        const int64_t lockWaitStart = rc::NowNs();
        const bool lockOk = !mutex || rcvr::AcquireKeyed(mutex.Get(), 100);
        if (mutex) {
            const int64_t waited = rc::NowNs() - lockWaitStart;
            ++m_diagLockSamples;
            m_diagLockSumNs += waited;
            if (waited > m_diagLockMaxNs) m_diagLockMaxNs = waited;
        }
        if (!lockOk) {
            WarnEvery5s("VR frame dropped: the driver kept the frame locked for over 100 ms");
            InterlockedExchange(&fs.state, rcvr::kSlotFree);
            continue;
        }
        // On a narrow path a clean image at half the frames beats a smeared one at all of
        // them: the headset keeps reprojecting the last frame to the current head pose.
        const uint32_t target = m_sender.TargetBitrateKbps();
        const double bitsPerPixel = target * 1000.0 / (double(encWidth) * encHeight * m_config.fps);
        const bool halfRateBefore = halfRate;
        if (bitsPerPixel < kHalfRateBelowBpp) halfRate = true;
        else if (bitsPerPixel > kFullRateAboveBpp) halfRate = false;
        if (halfRate != halfRateBefore)
            RC_LOGI("VR half-rate %s: effective target %u kbps, bpp %.4f (%ux%u @ %.0f nominal fps)",
                    halfRate ? "ON" : "OFF", target, bitsPerPixel, encWidth, encHeight, double(m_config.fps));
        // Publish for the 1 s timeline; this is a copy of the existing decision, not a new one.
        m_halfRateActive.store(halfRate, std::memory_order_relaxed);
        skipNext = halfRate && !skipNext;
        if (skipNext) {
            if (mutex) mutex->ReleaseSync(0);
            lastFrameIndex = fs.frameIndex;
            InterlockedExchange(&fs.state, rcvr::kSlotFree);
            continue;
        }

        // A target neither the encoder nor the frame waiting for it uses.
        ColorConverter* converter = nullptr;
        {
            std::lock_guard lock(m_jobMutex);
            for (auto& c : converters)
                if (c.Output() != m_encoding && (!m_job || c.Output() != m_job->yuv.Get())) converter = &c;
        }
        const int64_t convertStart = rc::NowNs();
        float foveaStart[2][2] = {};
        if (foveated) PlaceFovea(fs.fovTan, foveaStart);
        const bool converted =
            converter && (foveated ? foveator.Squeeze(ctx.Get(), tex, foveaStart) && converter->Convert(ctx.Get(), foveator.Output())
                                   : converter->Convert(ctx.Get(), tex));
        bool motionValid = false;
        // Only a frame the estimator saw becomes its previous one. A frame that failed
        // conversion, or could not be given a view, must not be named in the next field's
        // interval: the headset scales the field by that interval, so a pair that really
        // spans two frames would be bent as if it spanned one.
        bool motionConsumed = false;
        if (converted && motionReady) {
            ComPtr<ID3D11ShaderResourceView>& view = motionViews[tex];
            if (!view) {
                D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS   ? DXGI_FORMAT_R8G8B8A8_UNORM
                            : desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM
                                                                           : desc.Format;
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = 1;
                device->CreateShaderResourceView(tex, &sd, &view);
            }
            if (view) {
                float turn[9];
                HeadTurn(motionPose, fs.renderPose, turn);
                motionValid = motion.Process(ctx.Get(), view.Get(), turn, fs.fovTan);
                motionPose = fs.renderPose;
                motionConsumed = true;
            }
        }
        if (mutex) mutex->ReleaseSync(0);
        // The encoder is on another thread: hand the GPU the conversion now.
        ctx->Flush();

        const rcvr::FrameSlot slotCopy = fs;
        InterlockedExchange(&fs.state, rcvr::kSlotFree);
        lastFrameIndex = slotCopy.frameIndex;
        lastFrameNs = rc::NowNs();
        if (!converted) {
            WarnEvery5s("VR frame dropped: colour conversion failed");
            continue;
        }

        // A26 (stale-frame flush): recover this frame's predicted display moment in the
        // PC clock. The frame slot only carries the headset-clock echo
        // (FrameSlot::headsetTargetNs), so the moment comes from the tracking sample the
        // frame was rendered with - the streamer translated the headset's target into the
        // PC clock when it published it (client_session: RemoteToLocalNs, or its +40 ms
        // fallback). When the latest sample is still this frame's, the prediction is
        // exact; a newer one means the frame is at most one vsync old (the error stays
        // far below what the staleness gate judges), and the submit-time + predict-ahead
        // estimate covers the rest (seqlock hiccup, no tracking yet, a mapping that
        // failed the sanity guard above).
        rcvr::TrackingSnapshot flushTracking{};
        const bool flushTrackingRead = rcvr::ReadTracking(shared, flushTracking);
        const bool flushExact = flushTrackingRead && flushTracking.sequence == slotCopy.trackingSequence &&
                                flushTracking.targetDisplayPcNs > slotCopy.submitPcNs - kStaleClockGuardNs;

        EncodeJob job;
        job.yuv = converter->Output();
        job.format = converter->Format();
        job.width = encWidth;
        job.height = encHeight;
        job.srcWidth = srcWidth;
        job.srcHeight = srcHeight;
        job.halfRate = halfRate;
        job.foveated = foveated;
        std::memcpy(job.foveaStart, foveaStart, sizeof(foveaStart));
        job.frameIndex = slotCopy.frameIndex;
        job.headsetTargetNs = slotCopy.headsetTargetNs;
        job.submitPcNs = slotCopy.submitPcNs;
        job.predictedDisplayPcNs = flushExact ? flushTracking.targetDisplayPcNs
                                              : slotCopy.submitPcNs + kStalePredictAheadNs;
        std::memcpy(job.renderOrientation, slotCopy.renderPose.orientation, sizeof(job.renderOrientation));
        std::memcpy(job.renderPosition, slotCopy.renderPose.position, sizeof(job.renderPosition));
        std::memcpy(job.fovTan, slotCopy.fovTan, sizeof(job.fovTan));
        // Read now, before the next frame's estimate replaces it. This waits for the GPU, but
        // only with frame generation on.
        if (motionValid && motion.Read(ctx.Get(), motionField))
            AppendMotion(motion, motionField, motionPreviousTimestamp, m_motionGridPrev, job.motion);
        if (motionConsumed) motionPreviousTimestamp = slotCopy.headsetTargetNs;
        const int64_t convertNs = rc::NowNs() - convertStart;
        m_flowConvertNs += convertNs;
        convertAvgNs = convertAvgNs ? (convertAvgNs * 7 + convertNs) / 8 : convertNs;
        {
            std::lock_guard lock(m_jobMutex);
            // The encoder hasn't taken the one before: this newer frame replaces it.
            if (m_job) m_flowReplacedQueue++;
            m_job = std::move(job);
        }
        m_jobReady.notify_one();

        if (!streaming) {
            streaming = true;
            m_streaming = true;
            m_onState("running", ProducerName(m_bridge->ActiveProducer()), m_bridge->ApplicationName());
        }
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

void VrStream::RunEncoder() {
    ScopedCoInit com;
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
    // Same relative GPU priority the conversion thread uses: the sampler showed the
    // NVENC engine at 13% while frames waited ~7 ms in queue - the encode thread was
    // last in line for GPU time. Failure here is cosmetic (logged once).
    {
        static std::once_flag logged;
        ComPtr<IDXGIDevice> dxgi;
        const HRESULT hr = SUCCEEDED(m_bridge->Device()->QueryInterface(IID_PPV_ARGS(&dxgi))) ? dxgi->SetGPUThreadPriority(7) : E_NOINTERFACE;
        std::call_once(logged, [hr] { RC_LOGI("VR: GPU priority for encoding %s", SUCCEEDED(hr) ? "raised" : "unchanged (refused)"); });
    }

    ID3D11Device* device = m_bridge->Device();
    rcvr::Shared* shared = m_bridge->Shared();
    std::unique_ptr<VideoEncoder> encoder;
    uint32_t encWidth = 0, encHeight = 0;
    uint32_t srcW = 0, srcH = 0;
    DXGI_FORMAT encFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t appliedKbps = 0;
    EncodedFrame out;
    std::vector<uint8_t> frameMeta;
    while (m_running) {
        EncodeJob job;
        {
            std::unique_lock lock(m_jobMutex);
            m_encoding = nullptr;
            m_jobReady.wait(lock, [this] { return !m_running || m_job.has_value(); });
            if (!m_running) break;
            job = std::move(*m_job);
            m_job.reset();
            m_encoding = job.yuv.Get();
            m_encodeStartNs = rc::NowNs();
        }
        m_jobTaken.notify_one();
        // A62: the NVENC backend caches nvEncRegisterResource per input texture pointer.
        // Those registrations live exactly as long as the converter outputs do, so the
        // encoder must be rebuilt whenever the converters were rebuilt - and they are
        // rebuilt on any SOURCE size change, even when the encoded size stays the same
        // (foveated sizes are derived from the config and round to 16 px). Without this,
        // a rebuilt texture that lands on a freed address is handed to NVENC as its dead
        // predecessor's registration.
        if (!encoder || encWidth != job.width || encHeight != job.height || encFormat != job.format ||
            srcW != job.srcWidth || srcH != job.srcHeight) {
            encWidth = job.width;
            encHeight = job.height;
            encFormat = job.format;
            srcW = job.srcWidth;
            srcH = job.srcHeight;
            EncoderConfig ec;
            ec.codec = m_config.codec;
            ec.width = encWidth;
            ec.height = encHeight;
            ec.fps = m_config.fps;
            ec.bitrateKbps = m_sender.TargetBitrateKbps();
            ec.profile = m_config.profile;
            ec.tenBit = m_config.tenBit;
            ec.inputFormat = encFormat;
            // The lens-based bit budget assumes a picture that isn't squeezed.
            ec.foveated = m_config.foveated && !job.foveated;
            ec.adaptiveQuant = m_config.adaptiveQuant;
            ec.twoPass = m_config.twoPass;
            ec.cappedVbr = m_config.cappedVbr;
            ec.fillMode = m_config.fillMode;
            encoder = CreateEncoder(device, m_bridge->VendorId(), ec);
            // The stats path reads these from any thread; the encoder is local to
            // RunEncoder, so its QP and size travel out through atomics.
            m_statEncW.store(ec.width, std::memory_order_relaxed);
            m_statEncH.store(ec.height, std::memory_order_relaxed);
            appliedKbps = ec.bitrateKbps;
            m_sender.RequestKeyframe();
            if (!encoder) {
                WarnEvery5s("VR frame dropped: the encoder didn't start");
                rc::SleepMs(500);
                continue;
            }
        }

        // Half rate pays nothing extra here. The encoder tracks the interval between the
        // frames it actually sees (the slow EMA in nvenc_encoder reconfigures frameRateNum
        // to the real rate), so halving the frames already doubles the per-frame budget by
        // itself. Doubling the bitrate as well paid it twice: whenever the guard sat low
        // enough to trip half rate, the encoder produced twice what the pacer was allowed
        // to send (17.5 -> 35 Mb/s against a 26 Mb/s pacer) and the frame rate fell to
        // 29-33 fps. The handoff is bounded anyway - RunEncoder waits for the previous
        // SendJob to be taken - so 2x output would only queue against a 1x wire.
        const uint32_t target = m_sender.TargetBitrateKbps();
        const uint32_t encoderKbps = target;
        if (encoderKbps != appliedKbps) {
            encoder->SetBitrate(encoderKbps);
            appliedKbps = encoderKbps;
        }
        if (m_config.foveated && !job.foveated) encoder->SetFoveation(job.fovTan);
        const bool keyframe = m_sender.BeginFrame(*encoder);
        const int64_t encStart = rc::NowNs();
        if (!encoder->Encode(job.yuv.Get(), keyframe, out)) {
            WarnEvery5s("VR frame dropped: encoding failed; restarting the encoder");
            encoder.reset();
            continue;
        }
        const int64_t encEnd = rc::NowNs();
        const bool recovery = m_sender.TakeRecovery();
        m_flowEncoded++;
        m_statQp.store(encoder->AverageQp(), std::memory_order_relaxed);
        m_flowEncodeNs += encEnd - encStart;
        const int64_t encodeAvg = m_encodeAvgNs;
        m_encodeAvgNs = encodeAvg ? (encodeAvg * 7 + (encEnd - encStart)) / 8 : encEnd - encStart;

        rc::VrFrameMetadata meta{};
        meta.trackingTimestampNs = job.headsetTargetNs;
        std::memcpy(meta.renderPose.orientation, job.renderOrientation, sizeof(job.renderOrientation));
        std::memcpy(meta.renderPose.position, job.renderPosition, sizeof(job.renderPosition));
        std::memcpy(meta.fovTan, job.fovTan, sizeof(meta.fovTan));
        meta.ipd = shared->config.ipd;
        meta.eyeWidth = job.srcWidth / 2;
        meta.eyeHeight = job.srcHeight;
        meta.pcSubmitNs = job.submitPcNs;
        meta.pcEncodeStartNs = encStart;
        meta.pcEncodeEndNs = encEnd;
        frameMeta.assign(reinterpret_cast<const uint8_t*>(&meta), reinterpret_cast<const uint8_t*>(&meta) + sizeof(meta));
        if (job.foveated) {
            rc::VrFoveationMetadata fm{};
            std::memcpy(fm.foveaStart, job.foveaStart, sizeof(fm.foveaStart));
            frameMeta.insert(frameMeta.end(), reinterpret_cast<const uint8_t*>(&fm), reinterpret_cast<const uint8_t*>(&fm) + sizeof(fm));
        }
        frameMeta.insert(frameMeta.end(), job.motion.begin(), job.motion.end());
        SendJob send;
        send.frame = std::move(out);
        send.meta = std::move(frameMeta);
        send.flags = rc::kVideoHasPose | (m_config.tenBit ? rc::kVideoHdr10 : 0);
        send.encodeMs = rc::NsToMs(encEnd - encStart);
        // A55: the producer counts in 64 bits, the wire's diag seq field is 32 bits by design
        // (it is a wrapping sequence number). Say so instead of letting the compiler warn.
        send.frameIndex = static_cast<uint32_t>(job.frameIndex);  // ROUND19 T1: pass the existing counter through
        send.recovery = recovery;
        send.predictedDisplayPcNs = job.predictedDisplayPcNs;  // A26: the flush gate judges this
        {
            // The one before still going out (a paced send): wait, a dropped frame would break
            // the ones referencing it.
            std::unique_lock lock(m_sendMutex);
            m_sendCv.wait(lock, [this] { return !m_running || !m_send; });
            if (!m_running) break;
            m_send = std::move(send);
        }
        m_sendCv.notify_all();
        out = {};
        frameMeta.clear();
    }
    {
        std::lock_guard lock(m_jobMutex);
        m_encoding = nullptr;
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

void VrStream::RunSender() {
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
    bool firstSent = false;
    while (m_running) {
        SendJob send;
        {
            std::unique_lock lock(m_sendMutex);
            m_sendCv.wait(lock, [this] { return !m_running || m_send.has_value(); });
            if (!m_running) break;
            send = std::move(*m_send);
            m_send.reset();
        }
        m_sendCv.notify_all();
        // A26 (stale-frame flush) + A72 (the chain rule): a frame whose display moment has
        // already passed is late no matter how fast it goes out, and a frame whose
        // references were dropped with it decodes against a stale picture. So: drop the
        // expired one, and while the chain is broken let only repair frames (IDR /
        // recovery) leave - freezing on the last good picture beats one more artifact.
        // The debt wipe, the two throttles and the log line are A26's, unchanged. In the
        // steady state nothing here fires: normal frames' display moment sits in the future.
        const int64_t nowNs = rc::NowNs();
        const rc::FlushGate gate =
            rc::StaleFlushGate(send.frame.keyframe || send.recovery, send.predictedDisplayPcNs,
                               nowNs, m_staleFlush);
        if (!gate.send) {
            m_flowStaleDrops.fetch_add(1, std::memory_order_relaxed);
            if (gate.requestKeyframe) m_sender.RequestKeyframe();
            // The old billing lives in the packetizer and is unknown here; the wipe is
            // unconditional, so the argument is irrelevant (rc/stale_flush.h).
            if (gate.resetDebt) m_sender.ResetPacingDebt(rc::FlushSendDebtNs(0, nowNs));
            m_staleLogCount++;
            if (gate.log) {
                // A72 review: a drop is either late (A26's reason) or held back because the
                // frames it references were dropped with it. Calling the second one "display
                // moment had passed" sends the next reader chasing a clock fault that never
                // happened, and "keyframe requested" was a lie whenever the throttle held.
                const bool late = rc::FrameExpiredForSend(send.predictedDisplayPcNs, nowNs);
                RC_LOGI("VR stale flush: dropped %u frame(s) since the last line; last one %s "
                        "(%lld ms from its display moment); pacer debt wiped, repair %s",
                        m_staleLogCount, late ? "expired" : "held, chain broken",
                        (long long)((nowNs - send.predictedDisplayPcNs) / 1'000'000),
                        gate.requestKeyframe ? "requested" : "already pending");
                m_staleLogCount = 0;
            }
            if (gate.breaker)
                RC_LOGW("VR stale flush circuit breaker: chain broken, %u consecutive frame(s) "
                        "dropped; forcing a repair request instead of sending a broken frame",
                        rc::kStaleFlushBreakerDrops);
            continue;
        }
        m_sender.Send(send.frame, send.meta.data(), uint16_t(send.meta.size()), send.flags, send.encodeMs, send.recovery);
        // ROUND19 T1: feed the diag window (telemetry only; relaxed stores, the same cost
        // class as the flow counters). The CAS makes the window's first seq its minimum.
        {
            uint32_t expected = 0;
            m_diagSeqMin.compare_exchange_strong(expected, send.frameIndex, std::memory_order_relaxed);
            m_diagSeqMax.store(send.frameIndex, std::memory_order_relaxed);
            const uint32_t idx = uint32_t(m_diagEncIdx.fetch_add(1, std::memory_order_relaxed) % kDiagRing);
            m_diagEncUs[idx].store(uint32_t(send.encodeMs * 1000.0 + 0.5), std::memory_order_relaxed);
            m_diagSeqRing[idx].store(send.frameIndex, std::memory_order_relaxed);
            m_diagEncCount.fetch_add(1, std::memory_order_relaxed);
        }
        if (!firstSent) {
            firstSent = true;
            RC_LOGI("VR: frames are reaching the headset");
        }
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

}  // namespace rcs
