// Splits encoded video frames into datagrams (with optional XOR parity FEC)
// and reassembles them on the other side.
//
// A "frame payload" is [uint16 metadataSize][metadata bytes][bitstream]. The
// metadata is VrFrameMetadata for the VR stream and DesktopFrameMetadata for
// monitor streams.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "rc/connection.h"
#include "rc/protocol.h"

namespace rc {

class VideoPacketizer {
   public:
    explicit VideoPacketizer(Connection* connection);
    ~VideoPacketizer();

    VideoPacketizer(const VideoPacketizer&) = delete;
    VideoPacketizer& operator=(const VideoPacketizer&) = delete;

    // fecPercent: 0 disables FEC; otherwise one parity packet per
    // round(100 / fecPercent) data fragments (clamped to [2, 64]).
    // A45: atomic store - SendFrame takes a single relaxed snapshot before doing
    // 100 / fecPct, so a concurrent SetFecPercent(0) can never turn the divide
    // into a SIGFPE (and the old plain int was a C++ data race).
    void SetFecPercent(int fecPercent) { m_fecPercent.store(fecPercent, std::memory_order_relaxed); }
    // Two roles. Legacy clock (frame budget 0): spreads the datagrams at this rate
    // instead of sending them in one burst, which shallow queues (Wi-Fi, the internet)
    // drop. Token-bucket burst mode (frame budget > 0): the AVERAGE ceiling the mode
    // keeps - each frame bills its wire bytes at this rate, and the next frame may not
    // start before the billing elapses. Blocks the caller. 0 = no pacing.
    void SetPacingKbps(uint32_t kbps) { m_pacingKbps.store(kbps, std::memory_order_relaxed); }
    // Frame-granularity token bucket (A10), nanoseconds. When armed (> 0), a frame's
    // datagrams leave BACK-TO-BACK - nothing inside the frame waits - and the budget is
    // kept BETWEEN frames: the next frame's first datagram may not leave before this
    // frame's start + its wire bytes at the pacing rate. The value is the burst window
    // the frame's own send is judged against (see BudgetOverruns). 0 = off (the legacy
    // continuous clock: fragments spread at the pacing rate). The remote path and the
    // desktop stream never arm it.
    void SetPacingFrameBudgetNs(int64_t budgetNs) { m_frameBudgetNs.store(budgetNs, std::memory_order_relaxed); }
    // Frames whose burst window exceeded their budget + ~1 ms of timer error: the
    // socket or the path backed the burst up (the token-bucket wait itself doesn't count).
    uint32_t BudgetOverruns() const { return m_budgetOverruns.load(std::memory_order_relaxed); }

    // A79 phase M (stale-flush attribution, reading only): the send-side pacing backlog,
    // max(m_nextSendNs - now, 0). m_nextSendNs is a plain send-thread-only int64_t (the
    // discipline note below), so reading it from the stats thread would be a data race;
    // SendFrame instead stores a relaxed snapshot at the one point where the burst clock
    // is re-anchored every frame, and this getter decays that snapshot against NowNs().
    // 0 = the billing has elapsed, no burst-mode frame anchored the clock yet, or the
    // stream runs the legacy clock (which the card leaves unsnapshotted). No new
    // synchronisation: a relaxed load of a single-writer snapshot.
    int64_t SendBacklogNs() const;

    // A26 (stale-frame flush, rc/stale_flush.h): re-baselines the pacing clock to
    // `nowNs`, wiping whatever the backlog billed (card §1.3: the debt is forgiven
    // so the next frame may start at once). Send-thread only - SendFrame owns this
    // clock on that thread, and the flush runs on the same thread between two
    // SendFrame calls, so no lock is needed (the same discipline as the setters
    // above). In the legacy modes the clock holds at most kPaceMaxDebtNs, so the
    // reset is a harmless re-baseline; in burst mode it is what turns the flush
    // into an instant recovery.
    void ResetPacingDebtNs(int64_t nowNs) { m_nextSendNs = nowNs; }

    // Sends one frame. Returns number of datagrams sent. May return early (count <
    // expected) once RequestStop() was called.
    size_t SendFrame(uint8_t streamId, VideoCodec codec, uint8_t frameFlags, uint32_t frameIndex,
                     const void* metadata, uint16_t metadataSize, const uint8_t* bitstream, size_t bitstreamSize);

    // Makes a SendFrame waiting in the pacer return at once (used by shutdown).
    void RequestStop();

    // Datagrams the socket refused (a full send buffer after SO_SNDTIMEO). They never
    // reach the path, so the headset counts them as loss while the sender's own stats and
    // the congestion guard see nothing - the one loss source neither can explain.
    uint32_t SendFailures() const { return m_sendFailures.load(std::memory_order_relaxed); }

    // A45 test-only observability (read-only; no wire-format or behaviour change).
    // The transport-robustness fixes bound internal pacing/loss state; these getters
    // let the core tests assert those bounds deterministically instead of guessing
    // from wall-clock timing. SendFrame owns this clock on the send thread, so the
    // plain reads match the existing setters' single-writer discipline.
    int64_t NextSendNsForTest() const { return m_nextSendNs; }
    uint64_t LastBilledWireBytesForTest() const { return m_lastBilledWireBytes; }
    uint32_t ModeSwitchResetsForTest() const { return m_modeSwitchResets; }

   private:
    // Waits until dueNs; false means a stop was requested: wrap up at once. precise=true
    // would place fragments at sub-ms intervals instead of the legacy 0.2 ms batch;
    // every current caller waits with precise=false (the frame budget mode no longer
    // shapes inside a frame).
    bool WaitDue(int64_t dueNs, bool precise);

    Connection* m_connection;
    std::atomic<int> m_fecPercent{5};
    std::atomic<uint32_t> m_pacingKbps{0};
    std::atomic<int64_t> m_frameBudgetNs{0};
    std::atomic<uint32_t> m_budgetOverruns{0};
    // Two clocks, one per mode. Legacy clock (frame budget 0): the earliest moment the
    // next PACKET may leave, accumulating across frames; idle over 50 ms resets it to
    // now (a hiccup would otherwise repay its debt as one burst). Token-bucket burst
    // mode: the earliest moment the next FRAME may START - the running frame's billing
    // at the average ceiling. Stored as void* to keep windows.h out of this header.
    int64_t m_nextSendNs = 0;
    // A79 phase M: relaxed snapshot of m_nextSendNs, stored at the burst-mode anchor
    // point in SendFrame (the only write point) so SendBacklogNs() can be read from
    // the stats/diag threads without a race and without any lock.
    std::atomic<int64_t> m_nextSendNsPublished{0};
    // A45 §1.7: wire bytes billed into m_nextSendNs by the last burst-mode frame
    // (test observability) and the mode-switch bookkeeping that re-baselines the
    // clock when leaving burst mode, so a future-dated start cannot stall the next frame.
    uint64_t m_lastBilledWireBytes = 0;
    uint32_t m_modeSwitchResets = 0;
    bool m_wasBurstMode = false;
    std::atomic<bool> m_stopRequested{false};
    // Timer and stop event for WaitDue. Stored as void* to keep windows.h out
    void* m_paceTimer = nullptr;
    void* m_stopEvent = nullptr;
    std::vector<uint8_t> m_frame;
    std::atomic<uint32_t> m_sendFailures{0};
    int64_t m_lastSendFailLogNs = 0;
};

struct ReceivedFrame {
    uint8_t streamId;
    VideoCodec codec;
    uint8_t flags;
    uint32_t frameIndex;
    std::vector<uint8_t> metadata;
    std::vector<uint8_t> bitstream;
    int64_t firstFragmentNs;
    int64_t completeNs;
};

struct ReassemblyStats {
    uint32_t fragmentsReceived = 0;
    uint32_t fragmentsRecovered = 0;
    uint32_t fragmentsLost = 0;
    uint32_t framesComplete = 0;
    uint32_t framesLost = 0;
    // A45 §1.5: packets rejected at the entry gate (oversize fragment count /
    // pending table full). A fixed-vector proof that the OOM guards actually fire.
    uint32_t packetsDropped = 0;
    uint64_t bytesReceived = 0;
    double spreadMsSum = 0;
};

class VideoReassembler {
   public:
    using FrameHandler = std::function<void(ReceivedFrame&& frame)>;

    explicit VideoReassembler(FrameHandler handler) : m_handler(std::move(handler)) {}

    void HandleFragment(const uint8_t* payload, size_t size);
    void HandleParity(const uint8_t* payload, size_t size);

    // Drops frames that have been pending for too long. Returns true if a frame
    // was lost since the last call (=> caller should request a keyframe).
    bool Expire(int64_t nowNs, int64_t timeoutNs);

    // Returns and resets the accumulated statistics.
    ReassemblyStats TakeStats();
    uint32_t LastCompleteFrame() const { return m_lastComplete; }
    void Reset();

   private:
    struct Assembly {
        uint8_t streamId = 0;
        VideoCodec codec = VideoCodec::HEVC;
        uint8_t flags = 0;
        uint16_t fragmentCount = 0;
        uint8_t fecGroupSize = 0;
        uint32_t frameSize = 0;
        // A45 §1.4: VideoParityHeader.lastFragmentSize (sent by the packetizer but
        // previously ignored on receive) - used to reject tail-length underflow.
        uint16_t lastFragmentSize = 0;
        size_t fragmentPayload = 0;  // bytes per full fragment
        std::vector<std::vector<uint8_t>> fragments;
        std::vector<bool> present;
        std::map<uint16_t, std::vector<uint8_t>> parity;  // groupIndex -> xor data
        uint16_t received = 0;
        int64_t firstNs = 0;
    };

    void TryRecover(uint32_t frameIndex, Assembly& a, uint16_t group);
    // A45 §1.1: the caller's unique_lock is passed in so the handler call unlocks /
    // relocks THROUGH the lock object. A null or throwing handler can no longer skip
    // the relock and leave the outer unique_lock to double-unlock on destruction (UB).
    void TryComplete(uint32_t frameIndex, std::unique_lock<std::mutex>& lock);
    // A45 §1.6: a single loss watermark - every frameIndex is counted lost at most
    // once, whether it was dropped by Expire, evicted by TryComplete, or seen as a gap.
    void MarkLost(uint32_t frameIndex, uint16_t fragmentCount = 0, uint16_t received = 0);
    // A45 §1.5: caller holds m_mutex; evicts the oldest pending frame when full.
    void EnsurePendingCapacity(uint32_t frameIndex);

    FrameHandler m_handler;
    std::map<uint32_t, Assembly> m_pending;
    uint32_t m_lastComplete = 0;
    bool m_hasComplete = false;
    uint32_t m_lastCountedFrame = 0;
    bool m_hasCountedFrame = false;
    bool m_lostSinceExpire = false;
    ReassemblyStats m_stats;
    std::mutex m_mutex;
};

}  // namespace rc
