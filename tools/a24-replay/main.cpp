// a24-replay: TASK-TRACE-REPLAY-2026-11-22 (overnight A24) - an adversarial timing
// replay: REAL components x a SYNTHETIC timeline. Measurement only - no production
// behaviour lives in here and nothing is tuned to look good.
//
// Real components on the real wall clock (NowNs):
//   - VideoPacketizer in the production burst mode (token bucket: pacing = cap x 1.08,
//     frame budget = frame period, FEC 5%) over a REAL loopback UDP Connection pair
//     (the SessionLink shape core_tests.cpp / a14-matrix use).
//   - VideoReassembler (fragment -> frame) on the receive side, exactly the production
//     path (HandleFragment / HandleParity / ReceivedFrame with completeNs).
//   - the A23 decode-queue gate (rc/frame_queue_bound.h) judged at the decode submission
//     point, the way App::OnVideoFrame does it (headset/app/src/main/cpp/app/app.cpp:1050).
//   - the A26 stale flush (rc/stale_flush.h) judged at the send pickup, the way
//     VrStream::RunSender does it (pc/streamer/src/stream/vr_stream.cpp:756), including
//     the real pacer-debt wipe (ResetPacingDebtNs) and the 300 ms keyframe throttle.
//
// Synthetic parts (the declared model, documented in the receipt):
//   - the timeline generator: frames of calibrated sizes on a 72 Hz cadence. Steady
//     frame 120-280 KB (mean ~200 KB: AB10's 113 Mbps p50 at 72 fps), the card's big
//     frames (T1 360 KB, T6 keyframe 500 KB + 300 KB giants).
//   - the send queue: a FIFO without the production capacity-1 producer replacement
//     (VrStream's m_job slot) - the unbounded queue is the worst case the flush was
//     designed for; production's replace would cap it earlier. Declared, not hidden.
//   - the consumer: a FIFO decode queue drained at the display beat (72 fps; the
//     production decoder submits at the display rate when output surfaces gate it).
//     The A23 gate judges each pop (its Submit point). A scripted consumer stall is
//     the T2/T5 attack.
//   - the keyframe response: client gate requests and PC flush requests make the next
//     produced frame a 500 KB keyframe (production: the encoder forces the next frame
//     key); a client request lands after kKfResponseDelayNs (feedback window + half
//     RTT), a flush request acts at once. No content is decoded - the replay measures
//     timing, not bitstream correctness.
//
//   a24-replay --attack t1|t2|t3|t4|t5|t6 [--cap Mbps] [--jitter ms] [--gate 0|1]
//              [--out csv] [--dump-timeline path] [--timeline path] [model knobs]
//
// Attacks (verbatim from the task card §2):
//   T1  burst of 10 x 360 KB back-to-back (released at half cadence)   cap {100,152,200}
//   T2  one 50 ms consumer stall, 300-frame recovery window            cap 152
//   T3  sawtooth throughput 120 -> 60 -> 120 Mbps, 5 s x 3             cap 152
//   T4  arrival-gap jitter 13.9 ms mean +/- {2,5,10} ms, 300 frames    cap 152
//   T5  pause 2 s (client consumer) -> resume backlog burst, 600 f     cap 152
//   T6  scene change: 500 KB keyframe + 30 x 300 KB giants             cap {100,152}
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "rc/connection.h"
#include "rc/crypto.h"
#include "rc/frame_queue_bound.h"
#include "rc/handshake.h"
#include "rc/platform.h"
#include "rc/protocol.h"
#include "rc/socket.h"
#include "rc/stale_flush.h"
#include "rc/video_transport.h"

// ---------------------------------------------------------------------------
// Model constants (declared in the receipt; every one has a knob on the CLI)
// ---------------------------------------------------------------------------

// The A26 predicted-display model: predictedDisplayPcNs = produced + lead. Production
// uses the tracking target (measured submit->lit 36.9-37.3 ms in AB9) or the
// kStalePredictAheadNs fallback of 40 ms (vr_stream.cpp:81). 40 ms is the model.
constexpr int64_t kDefaultFlushLeadMs = 40;
// Client keyframe request -> the PC's next frame is a keyframe: feedback window (~25 ms)
// + half RTT (~5 ms). The flush's own request acts on the PC immediately.
constexpr int64_t kDefaultKfResponseMs = 30;
// Keyframes are big (the card's T6 value; production keyframes of a scene change).
constexpr size_t kKeyframeBytes = 500 * 1024;
// Production VR stream defaults: FEC 5% (vr_stream.h), burst pacing = cap x 1.08
// (video_sender.cpp:96), frame budget = the frame period (video_sender.cpp:99-102).
constexpr int kFecPercent = 5;
constexpr uint32_t kDefaultCapMbps = 152;
// 72 Hz base cadence (the card's 13.9 ms mean arrival interval).
constexpr double kDefaultConsumeFps = 72.0;
// Decode cost inside a beat (AB dec p50 8.5-9.6 ms) - recorded, the beat governs drain.
constexpr double kDefaultDecodeMs = 8.6;
// Steady frame size spread: [120, 280] KB, mean ~200 KB (AB10: 113 Mbps p50 / 72 fps).
constexpr double kSteadyMinKb = 120.0, kSteadySpanKb = 160.0;

// A frame the flush let through but whose transport queue still held it back is judged
// recovered when its transport wait falls under this (production downlink p95 25.5 ms).
constexpr double kTransportRecoveredMs = 25.0;
// A gate storm is over when a latched frame is again younger than this end to end.
constexpr double kLatchRecoveredMs = 50.0;

struct Options {
    std::string attack = "t1";
    uint32_t capMbps = kDefaultCapMbps;
    int jitterMs = 0;      // T4
    int gate = 1;          // 1 = the A23 gate is live, 0 = baseline (everything decoded)
    uint64_t seed = 0xA24BEEF;
    std::string out = "a24-attacks.csv", dumpTimeline, timelinePath;
    double consumeFps = kDefaultConsumeFps;
    int64_t flushLeadNs = kDefaultFlushLeadMs * 1'000'000;
    int64_t kfResponseNs = kDefaultKfResponseMs * 1'000'000;
    double decodeMs = kDefaultDecodeMs;
    std::string label;  // run label for the CSV (the driver fills pass numbers)
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed | 1) {}
    uint64_t Next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    double Next01() { return double(Next() >> 11) / 9007199254740992.0; }
    size_t SteadyBytes() { return size_t((kSteadyMinKb + Next01() * kSteadySpanKb) * 1024.0); }
};

// nearest-rank percentile on a sorted copy (the same rule rc/percentile.h uses)
static double Pct(std::vector<double> v, double p) {
    if (v.empty()) return -1.0;
    std::sort(v.begin(), v.end());
    const size_t rank = size_t(std::ceil(p / 100.0 * double(v.size())));
    return v[std::min(rank, v.size()) - 1];
}

// Absolute-deadline wait: timer for the bulk, spin for the tail (~1 ms timer accuracy).
struct PreciseWaiter {
    HANDLE timer;
    PreciseWaiter() {
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    ~PreciseWaiter() {
        if (timer) CloseHandle(timer);
    }
    void Until(int64_t absNs) {
        for (;;) {
            const int64_t remain = absNs - rc::NowNs();
            if (remain <= 0) return;
            if (remain > 2'000'000 && timer) {
                LARGE_INTEGER li;
                li.QuadPart = -remain / 100;
                if (SetWaitableTimer(timer, &li, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(timer, INFINITE);
                    continue;
                }
            }
            YieldProcessor();
        }
    }
    void SleepNs(int64_t ns) { Until(rc::NowNs() + ns); }
};

static void EnsureHeader(const std::string& path, const std::string& header) {
    std::error_code ec;
    const bool haveData = std::filesystem::exists(path, ec) && !ec &&
                          std::filesystem::file_size(path, ec) > 0 && !ec;
    if (haveData) return;
    std::ofstream f(path, std::ios::app);
    f << header << "\n";
}

// ---------------------------------------------------------------------------
// The synthetic timeline (the corpus format; see evidence/A24-timeline-format.md)
// ---------------------------------------------------------------------------

struct FrameEvent {
    int64_t tNs = 0;       // production time (pipeline entry), relative to the run start
    size_t sizeBytes = 0;  // bitstream size
    uint8_t flags = 0;     // rc::kVideoKeyframe and friends
};
struct StallEvent {
    int64_t tNs = 0, durNs = 0;  // consumer-side pause (T2/T5)
};
struct PhaseEvent {
    int64_t tNs = 0;
    uint32_t capMbps = 0;  // pacing cap from here on (T3's sawtooth)
};
struct Timeline {
    double consumeFps = kDefaultConsumeFps;
    uint32_t capMbps = kDefaultCapMbps;
    int64_t flushLeadNs = kDefaultFlushLeadMs * 1'000'000;
    uint64_t seed = 0;
    std::vector<FrameEvent> frames;
    std::vector<StallEvent> stalls;
    std::vector<PhaseEvent> phases;
};

static bool FrameEventAscending(const FrameEvent& a, const FrameEvent& b) { return a.tNs < b.tNs; }
// priority_queue pops the LARGEST element under its comparator: with this one the top is
// the EARLIEST event (a min-heap by time).
static bool FrameEventEarlierFirst(const FrameEvent& a, const FrameEvent& b) { return a.tNs > b.tNs; }

static void AppendSteady(Timeline& tl, Rng& rng, int count, int64_t& t, int64_t periodNs,
                         bool markFirst = false) {
    for (int i = 0; i < count; ++i) {
        FrameEvent ev;
        ev.tNs = t;
        ev.sizeBytes = rng.SteadyBytes();
        if (markFirst && i == 0 && tl.frames.empty()) ev.flags |= rc::kVideoKeyframe;
        tl.frames.push_back(ev);
        t += periodNs;
    }
}

// The card's §2 grid, verbatim. T1's "back-to-back" releases at half cadence (the
// encoder's catch-up pace: between 0 and the 13.9 ms cadence; the billing dominates
// the dynamics either way) - declared in the receipt.
static Timeline BuildAttack(const Options& opt) {
    Timeline tl;
    tl.consumeFps = opt.consumeFps;
    tl.capMbps = opt.capMbps;
    tl.flushLeadNs = opt.flushLeadNs;
    tl.seed = opt.seed;
    Rng rng(opt.seed);
    const int64_t periodNs = int64_t(1e9 / opt.consumeFps);
    int64_t t = 0;
    if (opt.attack == "t1") {
        // 40 steady lead-in, 10 x 360 KB back-to-back (consecutive, at the cadence), 80
        // steady recovery. Cadence spacing reproduces the real session's 12-frame scene
        // change losing 1+2 frames to the flush (AB10 10:12).
        AppendSteady(tl, rng, 40, t, periodNs);
        for (int i = 0; i < 10; ++i) {
            tl.frames.push_back({t, 360 * 1024, 0});
            t += periodNs;
        }
        AppendSteady(tl, rng, 80, t, periodNs);
    } else if (opt.attack == "t2") {
        // 120 steady, one 50 ms consumer stall, 300-frame recovery window.
        AppendSteady(tl, rng, 120, t, periodNs);
        tl.stalls.push_back({t, 50'000'000});
        AppendSteady(tl, rng, 300, t, periodNs);
    } else if (opt.attack == "t3") {
        // Sawtooth throughput: 120 -> 60 -> 120 Mbps, 5 s per phase, x 3 phases.
        AppendSteady(tl, rng, 1080, t, periodNs);  // 15 s at 72 fps
        tl.phases.push_back({0, 120});
        tl.phases.push_back({5'000'000'000LL, 60});
        tl.phases.push_back({10'000'000'000LL, 120});
    } else if (opt.attack == "t4") {
        // 300 frames, arrival gap = 13.9 ms mean +/- jitter ms (uniform).
        AppendSteady(tl, rng, 1, t, periodNs, true);
        for (int i = 1; i < 300; ++i) {
            const int64_t jitterNs =
                int64_t((rng.Next01() * 2.0 - 1.0) * double(opt.jitterMs) * 1e6);
            t += periodNs + jitterNs;
            FrameEvent ev;
            ev.tNs = t;
            ev.sizeBytes = rng.SteadyBytes();
            tl.frames.push_back(ev);
        }
    } else if (opt.attack == "t5") {
        // 120 steady, the client consumer pauses 2 s (the sender keeps streaming, so the
        // resume burst is the queued backlog itself), 600-frame recovery window.
        AppendSteady(tl, rng, 120, t, periodNs);
        tl.stalls.push_back({t, 2'000'000'000LL});
        AppendSteady(tl, rng, 600, t, periodNs);
    } else if (opt.attack == "t6") {
        // Scene change: 60 steady, 500 KB keyframe, 30 x 300 KB giants, 120 steady.
        AppendSteady(tl, rng, 60, t, periodNs);
        tl.frames.push_back({t, kKeyframeBytes, rc::kVideoKeyframe});
        t += periodNs;
        for (int i = 0; i < 30; ++i) {
            tl.frames.push_back({t, 300 * 1024, 0});
            t += periodNs;
        }
        AppendSteady(tl, rng, 120, t, periodNs);
    } else {
        std::printf("unknown attack %s\n", opt.attack.c_str());
        exit(2);
    }
    return tl;
}

// The corpus format (v1): '#' comment lines carry the model knobs, stalls and phases;
// data lines are "tMs,sizeKB,flags" (the frame's production time, bitstream size and
// flags). A real-machine log converts into exactly this shape.
static void DumpTimeline(const Timeline& tl, const std::string& path) {
    std::ofstream f(path);
    f << "# a24-timeline v1\n";
    f << "# fps=" << tl.consumeFps << " capMbps=" << tl.capMbps
      << " flushLeadMs=" << tl.flushLeadNs / 1'000'000 << " seed=" << tl.seed << "\n";
    for (const auto& s : tl.stalls)
        f << "# stall tMs=" << s.tNs / 1e6 << " durMs=" << s.durNs / 1e6 << "\n";
    for (const auto& p : tl.phases)
        f << "# phase tMs=" << p.tNs / 1e6 << " capMbps=" << p.capMbps << "\n";
    f << "tMs,sizeKB,flags\n";
    for (const auto& e : tl.frames)
        f << e.tNs / 1e6 << "," << e.sizeBytes / 1024.0 << "," << int(e.flags) << "\n";
}

static bool LoadTimeline(const std::string& path, Timeline& tl) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    bool inData = false;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (line[0] == '#') {
            double leadMs = 0;
            if (sscanf_s(line.c_str(), "# fps=%lf capMbps=%u flushLeadMs=%lf seed=%llu",
                         &tl.consumeFps, &tl.capMbps, &leadMs,
                         (unsigned long long*)&tl.seed) == 4) {
                tl.flushLeadNs = int64_t(leadMs * 1e6);
                continue;
            }
            double stMs = 0, durMs = 0;
            if (sscanf_s(line.c_str(), "# stall tMs=%lf durMs=%lf", &stMs, &durMs) == 2) {
                tl.stalls.push_back({int64_t(stMs * 1e6), int64_t(durMs * 1e6)});
                continue;
            }
            double ptMs = 0;
            uint32_t pcap = 0;
            if (sscanf_s(line.c_str(), "# phase tMs=%lf capMbps=%u", &ptMs, &pcap) == 2) {
                tl.phases.push_back({int64_t(ptMs * 1e6), pcap});
                continue;
            }
            continue;
        }
        if (!inData) {
            if (line.rfind("tMs,sizeKB,flags", 0) == 0) inData = true;
            continue;
        }
        double tMs = 0, kb = 0;
        int flags = 0;
        if (sscanf_s(line.c_str(), "%lf,%lf,%d", &tMs, &kb, &flags) != 3) return false;
        FrameEvent ev;
        ev.tNs = int64_t(tMs * 1e6);
        ev.sizeBytes = size_t(kb * 1024.0);
        ev.flags = uint8_t(flags);
        tl.frames.push_back(ev);
    }
    std::sort(tl.frames.begin(), tl.frames.end(), FrameEventAscending);
    return !tl.frames.empty();
}

// ---------------------------------------------------------------------------
// The real loopback (SessionLink shape, core_tests.cpp:3204 / a14-matrix part 2) plus
// the client-side bookkeeping the replay metrics need.
// ---------------------------------------------------------------------------

struct HandlerRecord {  // written by the reassembly handler (the reader thread)
    uint32_t wireIndex = 0;
    int64_t firstFragmentNs = 0, completeNs = 0, pcEncodeEndNs = 0;
};
struct ConsumerRecord {  // written by the consumer (the decode submission point)
    uint32_t wireIndex = 0;
    int64_t submitNs = 0, latchNs = 0;
    bool gated = false, decoded = false;
};

struct Loopback {
    rc::UdpSocket tx, rx;
    std::unique_ptr<rc::Connection> sendConn, recvConn;
    std::unique_ptr<rc::VideoReassembler> reasm;
    std::thread reader;
    std::atomic<bool> stop{false};

    // The decode queue: the production decoder's input side (App::OnVideoFrame blocks at
    // Submit, so frames queue in front of the decoder). The reader keeps draining the
    // socket while the consumer stalls - socket backpressure is out of scope here.
    std::mutex qMutex;
    std::deque<rc::ReceivedFrame> queue;
    std::atomic<int64_t> lastEventNs{0};

    std::mutex recMutex;
    std::vector<HandlerRecord> handlerRecs;
    rc::ReassemblyStats reasmStats;

    bool Open(const Options& opt, std::vector<ConsumerRecord>* consumerRecs,
              std::mutex* consumerMutex, std::atomic<int64_t>* lastLatchNs) {
        if (!tx.Open(0) || !rx.Open(0)) return false;
        tx.SetBuffers(16 << 20, 1 << 20);
        rx.SetBuffers(16 << 20, 16 << 20);
        auto client = rc::crypto::KeyPair::Generate();
        auto server = rc::crypto::KeyPair::Generate();
        const std::string ctx = rc::SessionContext("cn", "sn", "a24");
        auto ck = rc::crypto::DeriveSessionKeys(client.secretKey, server.publicKey, client.publicKey, server.publicKey, ctx);
        auto sk = rc::crypto::DeriveSessionKeys(server.secretKey, client.publicKey, client.publicKey, server.publicKey, ctx);
        sendConn = std::make_unique<rc::Connection>(&tx, rc::NetAddress{0x7F000001, rx.LocalPort()}, 7, sk, true,
                                                    rc::kMaxDatagramLan);
        recvConn = std::make_unique<rc::Connection>(&rx, rc::NetAddress{0x7F000001, tx.LocalPort()}, 7, ck, false,
                                                    rc::kMaxDatagramLan);
        m_consumerRecs = consumerRecs;
        m_consumerMutex = consumerMutex;
        m_lastLatchNs = lastLatchNs;
        reasm = std::make_unique<rc::VideoReassembler>([this](rc::ReceivedFrame&& f) {
            HandlerRecord hr;
            hr.wireIndex = f.frameIndex;
            hr.firstFragmentNs = f.firstFragmentNs;
            hr.completeNs = f.completeNs;
            // The transport queue anchor, read the way stream_client.cpp does it.
            if (f.streamId == rc::kVrStreamId && f.metadata.size() >= sizeof(rc::VrFrameMetadata)) {
                rc::VrFrameMetadata m;
                std::memcpy(&m, f.metadata.data(), sizeof(m));
                hr.pcEncodeEndNs = m.pcEncodeEndNs;
            }
            {
                std::lock_guard lock(recMutex);
                handlerRecs.push_back(hr);
            }
            {
                std::lock_guard lock(qMutex);
                queue.push_back(std::move(f));
            }
            lastEventNs.store(rc::NowNs(), std::memory_order_relaxed);
        });
        recvConn->SetPacketHandler([this](rc::PacketType t, const uint8_t* d, size_t n) {
            if (t == rc::PacketType::Video)
                reasm->HandleFragment(d, n);
            else if (t == rc::PacketType::VideoParity)
                reasm->HandleParity(d, n);
        });
        reader = std::thread([this] {
            std::vector<uint8_t> buf(2048);
            rc::NetAddress from;
            while (!stop.load()) {
                const int n = rx.RecvFrom(buf.data(), buf.size(), from, 5);
                if (n <= 0) {
                    recvConn->Tick(rc::NowNs());
                    continue;
                }
                recvConn->HandleDatagram(buf.data(), size_t(n), from);
            }
        });
        (void)opt;
        return true;
    }
    void Close() {
        stop = true;
        if (reader.joinable()) reader.join();
        {
            std::lock_guard lock(recMutex);
            reasmStats = reasm->TakeStats();
        }
        reasm.reset();
        recvConn.reset();
        sendConn.reset();
        tx.Close();
        rx.Close();
    }

   private:
    std::vector<ConsumerRecord>* m_consumerRecs = nullptr;
    std::mutex* m_consumerMutex = nullptr;
    std::atomic<int64_t>* m_lastLatchNs = nullptr;
};

// ---------------------------------------------------------------------------
// One run: the sender (A26 flush + the real packetizer), the consumer (the A23 gate),
// then the join and the report.
// ---------------------------------------------------------------------------

struct SenderRecord {
    uint32_t seq = 0, wireIndex = 0;  // wireIndex 0 = flushed (never sent)
    int64_t producedNs = 0, pickupNs = 0, sendEndNs = 0;
    bool flushed = false, flushKf = false;
};

struct RunResult {
    uint32_t scripted = 0, flushed = 0, flushKfReq = 0, flushLogs = 0;
    uint32_t gateDrops = 0, gateKfReq = 0, gateStreakMax = 0, decoded = 0, pcKeyframes = 0;
    uint32_t reasmLost = 0, framesComplete = 0;
    double tqP50 = 0, tqP95 = 0, tqMax = 0;        // transport queue: firstFragment - pcEncodeEnd
    double swP50 = 0, swP95 = 0, swMax = 0;        // sender wait: pickup - produced
    double ageP50 = 0, ageP95 = 0, ageMax = 0;     // client queue age: submit - completeNs
    double latP50 = 0, latP95 = 0, latMax = 0;     // latch staleness: latch - pcEncodeEnd
    uint32_t tqOver100 = 0, ageOver100 = 0, latOver100 = 0;
    uint32_t blindZone = 0;      // decoded stale >100 ms with client age <=100 ms (the gate is blind to it)
    uint32_t gateShouldCatch = 0;  // decoded frames with client age >100 ms (0 when the gate is on)
    int64_t flushRecoveryFrames = -1, gateRecoveryFrames = -1;  // -1 = no trigger to recover from
    double flushRate = 0;
    double spanSec = 0;
};

static int RunReplay(const Options& opt, const Timeline& tl, RunResult& r) {
    rc::NetInit();
    const int64_t periodNs = int64_t(1e9 / tl.consumeFps);
    const int64_t startNs = rc::NowNs() + 200'000'000;  // let the sockets settle

    Loopback net;
    std::vector<ConsumerRecord> consumerRecs;
    std::mutex consumerMutex;
    std::atomic<int64_t> lastLatchNs{0};
    if (!net.Open(opt, &consumerRecs, &consumerMutex, &lastLatchNs)) {
        std::printf("loopback open failed\n");
        return 1;
    }
    net.lastEventNs.store(startNs, std::memory_order_relaxed);  // the drain's quiet-clock anchor

    rc::VideoPacketizer pkt(net.sendConn.get());
    pkt.SetFecPercent(kFecPercent);
    pkt.SetPacingKbps(uint32_t(tl.capMbps * 1000 * 1.08));
    pkt.SetPacingFrameBudgetNs(periodNs);

    // The bitstream content does not matter (no decoder); one pattern, sliced per frame.
    std::vector<uint8_t> bitstream(600 * 1024);
    for (size_t i = 0; i < bitstream.size(); ++i) bitstream[i] = uint8_t(i * 131 + (i >> 8));
    rc::VrFrameMetadata meta{};
    meta.ipd = 0.0686f;
    meta.eyeWidth = 2688;
    meta.eyeHeight = 2688;

    // Sender state
    std::vector<SenderRecord> senderRecs;
    std::atomic<bool> pendingKeyframe{false};
    std::atomic<uint32_t> pcKeyframes{0};
    std::atomic<bool> senderDone{false};
    int64_t lastKfReqNs = 0, lastLogNs = 0;
    uint32_t flushKfReq = 0, flushLogs = 0;
    uint32_t wireSeq = 0;
    // The client's keyframe requests, landing after the feedback window + half RTT.
    std::mutex schedMutex;
    std::vector<int64_t> scheduledKf;

    // ---- the sender thread (mirrors VrStream::RunSender's A26 branch) ----
    std::thread sender([&] {
        // One timer PER THREAD: a waitable timer is a shared kernel object - resetting it
        // from a second thread would cancel the first thread's pending wait for good.
        PreciseWaiter waiter;
        std::priority_queue<FrameEvent, std::vector<FrameEvent>, bool (*)(const FrameEvent&,
                                                                          const FrameEvent&)>
            heap(FrameEventEarlierFirst);
        {
            // A short tail pad so a pending keyframe response always has a carrier.
            const FrameEvent& last = tl.frames.back();
            FrameEvent pad1, pad2;
            pad1.tNs = last.tNs + periodNs;
            pad1.sizeBytes = 200 * 1024;
            pad2.tNs = last.tNs + 2 * periodNs;
            pad2.sizeBytes = 200 * 1024;
            heap.push(pad1);
            heap.push(pad2);
        }
        for (const auto& e : tl.frames) heap.push(e);
        size_t phaseIdx = 0;
        uint32_t seq = 0;
        for (;;) {
            FrameEvent ev;
            {
                // Single-threaded pop (insertions never preempt the next scripted frame).
                if (heap.empty()) break;
                ev = heap.top();
                heap.pop();
            }
            const int64_t producedNs = startNs + ev.tNs;
            waiter.Until(producedNs);
            // T3's pacing phases are applied live (the real atomic setter).
            while (phaseIdx < tl.phases.size() && startNs + tl.phases[phaseIdx].tNs <= rc::NowNs()) {
                pkt.SetPacingKbps(uint32_t(tl.phases[phaseIdx].capMbps * 1000 * 1.08));
                phaseIdx++;
            }
            const int64_t nowNs = rc::NowNs();
            // The client's keyframe request lands here after its RTT.
            {
                std::lock_guard lock(schedMutex);
                for (size_t i = 0; i < scheduledKf.size();) {
                    if (scheduledKf[i] <= nowNs) {
                        pendingKeyframe.store(true, std::memory_order_relaxed);
                        scheduledKf.erase(scheduledKf.begin() + i);
                    } else {
                        ++i;
                    }
                }
            }
            SenderRecord rec;
            rec.seq = seq;
            rec.producedNs = producedNs;
            rec.pickupNs = nowNs;
            // A26: the frame's predicted display moment (production model), judged at the
            // pickup exactly like VrStream::RunSender.
            const rc::FlushDecision flush =
                rc::StaleFlushDecide(producedNs + tl.flushLeadNs, nowNs, lastKfReqNs, lastLogNs);
            if (flush.drop) {
                rec.flushed = true;
                if (flush.requestKeyframe) {
                    rec.flushKf = true;
                    lastKfReqNs = nowNs;
                    pendingKeyframe.store(true, std::memory_order_relaxed);
                    flushKfReq++;
                }
                // The debt wipe is unconditional (rc/stale_flush.h) and hits the REAL pacer.
                pkt.ResetPacingDebtNs(rc::FlushSendDebtNs(0, nowNs));
                if (flush.log) {
                    lastLogNs = nowNs;
                    flushLogs++;
                }
                senderRecs.push_back(rec);
                seq++;
                continue;
            }
            uint8_t flags = ev.flags;
            size_t size = ev.sizeBytes;
            if (pendingKeyframe.exchange(false, std::memory_order_relaxed)) {
                flags |= rc::kVideoKeyframe;
                size = kKeyframeBytes;
                pcKeyframes.fetch_add(1, std::memory_order_relaxed);
            }
            meta.trackingTimestampNs = ev.tNs;
            meta.pcSubmitNs = producedNs;
            meta.pcEncodeStartNs = producedNs;
            meta.pcEncodeEndNs = producedNs;
            rec.wireIndex = ++wireSeq;
            pkt.SendFrame(rc::kVrStreamId, rc::VideoCodec::HEVC, flags, rec.wireIndex, &meta,
                          sizeof(meta), bitstream.data(), size);
            rec.sendEndNs = rc::NowNs();
            senderRecs.push_back(rec);
            seq++;
        }
        senderDone.store(true, std::memory_order_relaxed);
    });

    // ---- the consumer thread (the decode submission point, App::OnVideoFrame's gate) ----
    // Production model: a frame is submitted to the decoder AT ARRIVAL when a decode
    // slot is free (App::OnVideoFrame -> Submit on the network thread); it queues only
    // while the decoder is genuinely behind (one frame per decodeMs frees a slot). A
    // scripted stall (T2/T5) is the decoder being unable to take buffers. When
    // backlogged, the queue drains at the decode rate, not the display rate.
    std::atomic<bool> running{true};
    std::atomic<uint32_t> gateDrops{0}, gateKfReq{0}, gateStreakMax{0}, decodedCount{0};
    const int64_t decodeNs = int64_t(opt.decodeMs * 1e6);
    std::thread consumer([&] {
        // One timer PER THREAD (see the sender note above).
        PreciseWaiter waiter;
        uint32_t streak = 0;
        int64_t busyUntilNs = 0;  // the decoder frees a slot at this moment
        size_t stallIdx = 0;
        for (;;) {
            int64_t now = rc::NowNs();
            while (stallIdx < tl.stalls.size() && now >= tl.stalls[stallIdx].tNs + startNs) {
                // The stall: the decoder takes no buffers, frames queue up (T2/T5).
                waiter.Until(tl.stalls[stallIdx].tNs + tl.stalls[stallIdx].durNs + startNs);
                now = rc::NowNs();
                stallIdx++;
                busyUntilNs = 0;  // a resumed decoder starts idle
            }
            const bool stopping = !running.load(std::memory_order_relaxed);
            {
                std::lock_guard lock(net.qMutex);
                if (stopping && net.queue.empty() && now >= busyUntilNs) break;
            }
            if (now >= busyUntilNs) {
                rc::ReceivedFrame f;
                bool have = false;
                {
                    std::lock_guard lock(net.qMutex);
                    if (!net.queue.empty()) {
                        f = std::move(net.queue.front());
                        net.queue.pop_front();
                        have = true;
                    }
                }
                if (have) {
                    const int64_t submitNs = rc::NowNs();
                    ConsumerRecord cr;
                    cr.wireIndex = f.frameIndex;
                    cr.submitNs = submitNs;
                    // A23: the decode submission point, judged exactly like
                    // App::OnVideoFrame judges it.
                    const bool expired =
                        opt.gate != 0 && rc::FrameQueueExpired(submitNs, f.completeNs);
                    if (expired) {
                        cr.gated = true;
                        streak++;
                        gateDrops.fetch_add(1, std::memory_order_relaxed);
                        gateStreakMax.store(
                            std::max(gateStreakMax.load(std::memory_order_relaxed), streak),
                            std::memory_order_relaxed);
                        if (rc::QueueDropRequestKeyframe(streak)) {
                            gateKfReq.fetch_add(1, std::memory_order_relaxed);
                            std::lock_guard lock(schedMutex);
                            scheduledKf.push_back(submitNs + opt.kfResponseNs);
                        }
                    } else {
                        cr.decoded = true;
                        cr.latchNs = submitNs + decodeNs;  // shown after its decode completes
                        streak = 0;
                        decodedCount.fetch_add(1, std::memory_order_relaxed);
                        lastLatchNs.store(submitNs + decodeNs, std::memory_order_relaxed);
                    }
                    {
                        std::lock_guard lock(consumerMutex);
                        consumerRecs.push_back(cr);
                    }
                    net.lastEventNs.store(submitNs, std::memory_order_relaxed);
                    busyUntilNs = submitNs + decodeNs;  // the slot is busy for one decode
                }
            }
            // Sleep to the next interesting moment: the decoder freeing, the next stall,
            // or a short idle poll so an arrival is submitted promptly (production
            // submits at arrival; the poll bounds the added latency, not the queue).
            int64_t wake = std::max(busyUntilNs, now + 2'000'000);
            if (stallIdx < tl.stalls.size())
                wake = std::min(wake, tl.stalls[stallIdx].tNs + startNs);
            waiter.Until(std::min(wake, now + 20'000'000));
        }
    });

    // ---- run to completion, then drain ----
    while (!senderDone.load(std::memory_order_relaxed)) rc::SleepMs(10);
    const int64_t drainDeadline = rc::NowNs() + 20'000'000'000LL;
    for (;;) {
        const bool queueEmpty = [&] {
            std::lock_guard lock(net.qMutex);
            return net.queue.empty();
        }();
        const int64_t lastEvent = std::max(net.lastEventNs.load(std::memory_order_relaxed),
                                           lastLatchNs.load(std::memory_order_relaxed));
        if (queueEmpty && rc::NowNs() - lastEvent > 500'000'000LL) break;
        if (rc::NowNs() > drainDeadline) break;
        rc::SleepMs(10);
    }
    running.store(false, std::memory_order_relaxed);
    sender.join();
    consumer.join();
    // Stop the reader and take the reassembly stats BEFORE touching the records it wrote.
    net.Close();
    const int64_t endNs = rc::NowNs();
    r.spanSec = double(endNs - startNs) / 1e9;

    // ---- the join and the metrics ----
    std::map<uint32_t, const SenderRecord*> byWire;
    for (const auto& s : senderRecs)
        if (s.wireIndex) byWire[s.wireIndex] = &s;
    uint32_t flushed = 0;
    std::vector<double> senderWait, tq, age, lat;
    uint32_t sentCount = 0;
    int64_t lastFlushSeq = -1;
    for (const auto& s : senderRecs) {
        if (s.flushed) {
            flushed++;
            lastFlushSeq = int64_t(s.seq);
            continue;
        }
        sentCount++;
        senderWait.push_back(rc::NsToMs(s.pickupNs - s.producedNs));
    }
    // Client-side: the handler records give the transport queue; the consumer records the
    // gate verdicts. Joined by wireIndex.
    std::map<uint32_t, const HandlerRecord*> hr;
    for (const auto& h : net.handlerRecs) hr[h.wireIndex] = &h;
    // Consumer records in submission order (for the recovery scan).
    std::vector<ConsumerRecord> ordered;
    {
        std::lock_guard lock(consumerMutex);
        ordered = consumerRecs;
    }
    std::sort(ordered.begin(), ordered.end(), [](const ConsumerRecord& a, const ConsumerRecord& b) {
        return a.submitNs < b.submitNs;
    });
    int64_t lastGateSeq = -1;
    for (const auto& c : ordered) {
        auto it = hr.find(c.wireIndex);
        const HandlerRecord* h = it != hr.end() ? it->second : nullptr;
        if (c.gated) {
            lastGateSeq = int64_t(c.wireIndex);
            if (h) age.push_back(rc::NsToMs(c.submitNs - h->completeNs));
            continue;
        }
        if (!c.decoded || !h) continue;
        const double a = rc::NsToMs(c.submitNs - h->completeNs);
        const double l = h->pcEncodeEndNs > 0 ? rc::NsToMs(c.latchNs - h->pcEncodeEndNs) : -1;
        const double t = h->pcEncodeEndNs > 0 ? rc::NsToMs(h->firstFragmentNs - h->pcEncodeEndNs) : -1;
        age.push_back(a);
        if (l >= 0) lat.push_back(l);
        if (t >= 0) tq.push_back(t);
        if (a > 100.0) r.ageOver100++;
        if (l > 100.0) r.latOver100++;
        if (t > 100.0) r.tqOver100++;
        if (a > 100.0) r.gateShouldCatch++;
        if (l > 100.0 && a <= 100.0) r.blindZone++;
    }
    r.scripted = uint32_t(tl.frames.size());
    r.flushed = flushed;
    r.flushKfReq = flushKfReq;
    r.flushLogs = flushLogs;
    r.gateDrops = gateDrops.load();
    r.gateKfReq = gateKfReq.load();
    r.gateStreakMax = gateStreakMax.load();
    r.decoded = decodedCount.load();
    r.pcKeyframes = pcKeyframes.load();
    r.reasmLost = net.reasmStats.framesLost;
    r.framesComplete = net.reasmStats.framesComplete;
    r.swP50 = Pct(senderWait, 50);
    r.swP95 = Pct(senderWait, 95);
    r.swMax = Pct(senderWait, 100);
    r.tqP50 = Pct(tq, 50);
    r.tqP95 = Pct(tq, 95);
    r.tqMax = Pct(tq, 100);
    r.ageP50 = Pct(age, 50);
    r.ageP95 = Pct(age, 95);
    r.ageMax = Pct(age, 100);
    r.latP50 = Pct(lat, 50);
    r.latP95 = Pct(lat, 95);
    r.latMax = Pct(lat, 100);
    r.flushRate = sentCount + flushed > 0 ? double(flushed) / double(sentCount + flushed) : 0;
    // Flush recovery: frames SENT after the last flush until the transport queue is
    // absorbed again (< kTransportRecoveredMs).
    if (lastFlushSeq >= 0) {
        int64_t frames = 0;
        bool recovered = false;
        for (const auto& s : senderRecs) {
            if (int64_t(s.seq) <= lastFlushSeq || s.flushed) continue;
            frames++;
            auto it = hr.find(s.wireIndex);
            if (it == hr.end() || it->second->pcEncodeEndNs <= 0) continue;
            const double t = rc::NsToMs(it->second->firstFragmentNs - it->second->pcEncodeEndNs);
            if (t < kTransportRecoveredMs) {
                recovered = true;
                break;
            }
        }
        r.flushRecoveryFrames = recovered ? frames : -2;  // -2 = never recovered in-run
    }
    // Gate recovery: frames latched after the last gate drop until one is again younger
    // than kLatchRecoveredMs end to end.
    if (lastGateSeq >= 0) {
        int64_t frames = 0;
        bool recovered = false;
        for (const auto& c : ordered) {
            if (int64_t(c.wireIndex) <= lastGateSeq || !c.decoded) continue;
            auto it = hr.find(c.wireIndex);
            if (it == hr.end() || it->second->pcEncodeEndNs <= 0) continue;
            frames++;
            const double l = rc::NsToMs(c.latchNs - it->second->pcEncodeEndNs);
            if (l < kLatchRecoveredMs) {
                recovered = true;
                break;
            }
        }
        r.gateRecoveryFrames = recovered ? frames : -2;
    }

    return 0;
}

static void PrintResult(const Options& opt, const Timeline& tl, const RunResult& r) {
    std::printf(
        "[%s cap%u gate%d] scripted %u flushed %u (%.1f%%) gateDrops %u decoded %u lost %u | "
        "sendWait p50/p95/max %.1f/%.1f/%.1f ms | tQ p50/p95/max %.1f/%.1f/%.1f ms (t>100: %u) | "
        "cAge p50/p95/max %.1f/%.1f/%.1f ms (>100: %u) | latchStale p50/p95/max %.1f/%.1f/%.1f ms (>100: %u, blind %u, gateShouldCatch %u) | "
        "flushKf %u flushLog %u gateKf %u streakMax %u pcKf %u | recovery flush %lld f, gate %lld f | flushRate %.2f span %.1f s\n",
        opt.attack.c_str(), tl.capMbps, opt.gate, r.scripted, r.flushed, r.flushRate * 100.0,
        r.gateDrops, r.decoded, r.reasmLost, r.swP50, r.swP95, r.swMax, r.tqP50, r.tqP95, r.tqMax,
        r.tqOver100, r.ageP50, r.ageP95, r.ageMax, r.ageOver100, r.latP50, r.latP95, r.latMax,
        r.latOver100, r.blindZone, r.gateShouldCatch, r.flushKfReq, r.flushLogs, r.gateKfReq,
        r.gateStreakMax, r.pcKeyframes,
        r.flushRecoveryFrames == -1 ? 0LL : (r.flushRecoveryFrames == -2 ? -2LL : r.flushRecoveryFrames),
        r.gateRecoveryFrames == -1 ? 0LL : (r.gateRecoveryFrames == -2 ? -2LL : r.gateRecoveryFrames),
        r.flushRate, r.spanSec);
}

static void AppendCsv(const Options& opt, const Timeline& tl, const RunResult& r) {
    EnsureHeader(opt.out,
                 "label,attack,capMbps,jitterMs,gate,seed,consumeFps,flushLeadMs,decodeMs,"
                 "scripted,flushed,flushRate,flushKfReq,flushLogs,gateDrops,gateKfReq,gateStreakMax,"
                 "decoded,reasmLost,framesComplete,pcKeyframes,"
                 "sendWaitP50,sendWaitP95,sendWaitMax,tqP50,tqP95,tqMax,tqOver100,"
                 "clientAgeP50,clientAgeP95,clientAgeMax,clientAgeOver100,gateShouldCatch,"
                 "latchStaleP50,latchStaleP95,latchStaleMax,latchStaleOver100,blindZone,"
                 "flushRecoveryFrames,gateRecoveryFrames,spanSec");
    std::ofstream f(opt.out, std::ios::app);
    f << opt.label << "," << opt.attack << "," << tl.capMbps << "," << opt.jitterMs << "," << opt.gate
      << "," << tl.seed << "," << tl.consumeFps << "," << tl.flushLeadNs / 1'000'000 << ","
      << opt.decodeMs << "," << r.scripted << "," << r.flushed << "," << r.flushRate << ","
      << r.flushKfReq << "," << r.flushLogs << "," << r.gateDrops << "," << r.gateKfReq << ","
      << r.gateStreakMax << "," << r.decoded << "," << r.reasmLost << "," << r.framesComplete << ","
      << r.pcKeyframes << "," << r.swP50 << "," << r.swP95 << "," << r.swMax << "," << r.tqP50 << ","
      << r.tqP95 << "," << r.tqMax << "," << r.tqOver100 << "," << r.ageP50 << "," << r.ageP95 << ","
      << r.ageMax << "," << r.ageOver100 << "," << r.gateShouldCatch << "," << r.latP50 << ","
      << r.latP95 << "," << r.latMax << "," << r.latOver100 << "," << r.blindZone << ","
      << r.flushRecoveryFrames << "," << r.gateRecoveryFrames << "," << r.spanSec << "\n";
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--attack") opt.attack = next();
        else if (a == "--cap") opt.capMbps = uint32_t(std::stoi(next()));
        else if (a == "--jitter") opt.jitterMs = std::stoi(next());
        else if (a == "--gate") opt.gate = std::stoi(next());
        else if (a == "--seed") opt.seed = std::stoull(next());
        else if (a == "--out") opt.out = next();
        else if (a == "--dump-timeline") opt.dumpTimeline = next();
        else if (a == "--timeline") opt.timelinePath = next();
        else if (a == "--consume-fps") opt.consumeFps = std::stod(next());
        else if (a == "--flush-lead-ms") opt.flushLeadNs = std::stoll(next()) * 1'000'000;
        else if (a == "--kf-response-ms") opt.kfResponseNs = std::stoll(next()) * 1'000'000;
        else if (a == "--decode-ms") opt.decodeMs = std::stod(next());
        else if (a == "--label") opt.label = next();
        else {
            std::printf("unknown option %s\n", a.c_str());
            return 2;
        }
    }

    Timeline tl;
    if (!opt.timelinePath.empty()) {
        if (!LoadTimeline(opt.timelinePath, tl)) {
            std::printf("timeline %s not found or empty\n", opt.timelinePath.c_str());
            return 2;
        }
        opt.attack = "corpus:" + opt.timelinePath;
        std::printf("a24-replay: corpus %s loaded (%zu frames, %zu stalls, %zu phases, cap %u)\n",
                    opt.timelinePath.c_str(), tl.frames.size(), tl.stalls.size(), tl.phases.size(),
                    tl.capMbps);
    } else {
        tl = BuildAttack(opt);
        std::printf("a24-replay: attack %s built (%zu frames, %zu stalls, %zu phases, cap %u, jitter %d ms, gate %d)\n",
                    opt.attack.c_str(), tl.frames.size(), tl.stalls.size(), tl.phases.size(),
                    tl.capMbps, opt.jitterMs, opt.gate);
    }
    if (!opt.dumpTimeline.empty()) DumpTimeline(tl, opt.dumpTimeline);

    RunResult r;
    const int exitCode = RunReplay(opt, tl, r);
    PrintResult(opt, tl, r);
    AppendCsv(opt, tl, r);
    std::printf("a24-replay done, exit %d\n", exitCode);
    return exitCode;
}
