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
    void SetFecPercent(int fecPercent) { m_fecPercent = fecPercent; }
    // Spreads the datagrams at this rate instead of sending them in one burst, which
    // shallow queues (Wi-Fi, the internet) drop. Blocks the caller. 0 = no pacing.
    // The rate only changes the spacing: the clock carries on across frames, so a frame
    // never starts with a burst of the previous frame's leftovers.
    void SetPacingKbps(uint32_t kbps) { m_pacingKbps.store(kbps, std::memory_order_relaxed); }

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

   private:
    // Waits until dueNs; false means a stop was requested: wrap up at once.
    bool WaitDue(int64_t dueNs);

    Connection* m_connection;
    int m_fecPercent = 5;
    std::atomic<uint32_t> m_pacingKbps{0};
    // Continuous send clock: always the earliest moment the next packet may leave. It
    // accumulates across frames; idle over 50 ms resets it to now (a hiccup would
    int64_t m_nextSendNs = 0;
    std::atomic<bool> m_stopRequested{false};
    // otherwise repay the debt as one burst). Stored as void* to keep windows.h out
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
        size_t fragmentPayload = 0;  // bytes per full fragment
        std::vector<std::vector<uint8_t>> fragments;
        std::vector<bool> present;
        std::map<uint16_t, std::vector<uint8_t>> parity;  // groupIndex -> xor data
        uint16_t received = 0;
        int64_t firstNs = 0;
    };

    void TryRecover(uint32_t frameIndex, Assembly& a, uint16_t group);
    void TryComplete(uint32_t frameIndex);

    FrameHandler m_handler;
    std::map<uint32_t, Assembly> m_pending;
    uint32_t m_lastComplete = 0;
    bool m_hasComplete = false;
    bool m_lostSinceExpire = false;
    ReassemblyStats m_stats;
    std::mutex m_mutex;
};

}  // namespace rc
