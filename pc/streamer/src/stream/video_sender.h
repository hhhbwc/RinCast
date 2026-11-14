// Shared plumbing for desktop and VR streams: packetizing, keyframe requests,
// adaptive bitrate (AIMD on loss / queuing delay) and statistics.
#pragma once

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>
#include <thread>
#include <vector>

#include "../encode/encoder.h"
#include "rc/connection.h"
#include "rc/loss_brake.h"
#include "rc/queue_guard.h"
#include "rc/stream_profile.h"
#include "rc/video_transport.h"

namespace rcs {

struct VideoSenderStats {
    double fps = 0;
    double encodeMs = 0;
    double mbps = 0;
    uint32_t bitrateKbps = 0;
    uint32_t framesLost = 0;
    double clientDecodeMs = 0;
    double clientLatencyMs = 0;
    double queueMs = -1;  // queueing delay on the way to the headset, -1 = unknown
    float qp = -1;        // the encoder's rolling mean QP, -1 = unknown
    uint32_t encWidth = 0, encHeight = 0;  // what is actually being encoded, 0 = unknown
};

class VideoSender {
   public:
    VideoSender(rc::Connection* connection, uint8_t streamId, rc::VideoCodec codec, uint32_t maxBitrateKbps,
                int fecPercent, bool adaptive, rc::StreamProfile profile = rc::StreamProfile::Balanced);
    ~VideoSender();

    // Sends one encoded frame with metadata.
    void Send(const EncodedFrame& frame, const void* metadata, uint16_t metadataSize, uint8_t extraFlags,
              double encodeMs) {
        Send(frame, metadata, metadataSize, extraFlags, encodeMs, TakeRecovery());
    }
    // For sending on another thread than the encoding: whether the frame BeginFrame was just
    // called for answers a loss (kVideoRecovery). Take it right after encoding, pass it to Send.
    bool TakeRecovery() { return std::exchange(m_nextIsRecovery, false); }
    void Send(const EncodedFrame& frame, const void* metadata, uint16_t metadataSize, uint8_t extraFlags,
              double encodeMs, bool recovery);

    void RequestKeyframe() { m_keyframeRequested = true; }
    // True if the headset is waiting for a frame (keyframe or loss recovery), so one
    // should be encoded even if the picture didn't change.
    bool WantsFrame();
    // Call right before encoding; returns whether to force a keyframe. A loss the
    // headset reported is answered by invalidating the lost frames in the encoder when
    // it can, so the next frame is a normal-sized one flagged kVideoRecovery.
    bool BeginFrame(VideoEncoder& encoder);

    void OnFeedback(const rc::VideoFeedbackPayload& fb);
    // Bitrate the encoder should use now (changes when the guard caps).
    uint32_t TargetBitrateKbps() const {
        const uint32_t cap = m_congestionKbps.load();
        const uint32_t t = m_targetKbps.load();
        return cap ? std::min(cap, t) : t;
    }
    void SetMaxBitrate(uint32_t kbps);
    // The preference behind the automatic-bitrate switch. Maps to the guard's
    // latency budget before the first frame; the UI can call it at runtime too.
    void SetProfile(rc::StreamProfile profile);
    rc::StreamProfile Profile() const { return m_profile; }
    // 把每帧的包铺开在时间上发，而不是一口气打出去（局域网和互联网都用）：
    // Wi-Fi 的浅队列会被整帧突发打爆。必须在 Start 之前设好。
    void SetPacing(bool pace) { m_pace = pace; }
    // Adaptive only: begin below the maximum and let the guard explore upward. The ceiling
    // is where the climb stops, so seeding it is what makes the start point mean anything.
    void StartAt(uint32_t kbps);

    VideoSenderStats Stats();
    uint8_t StreamId() const { return m_streamId; }

   private:
    rc::Connection* m_connection;
    rc::VideoPacketizer m_packetizer;
    const uint8_t m_streamId;
    const rc::VideoCodec m_codec;
    std::atomic<bool> m_keyframeRequested{true};
    std::atomic<uint32_t> m_targetKbps;
    // Fixed-bitrate guard (OnFeedback): holds the rate under what the path carries by
    // watching the queue the headset reports. Its law and the measurements behind it live
    // in rc/queue_guard.h; this class only owns the timing and the logging around it.
    rc::QueueGuard m_guard;
    // Loss-side brake (loss_brake.h): the guard watches the queue, this watches
    // actual loss and HOLDS the rate down after a loss cut. Both caps are taken
    // together; neither one's climb may override the other's hold.
    rc::LossBrakeState m_brake;
    std::atomic<uint32_t> m_congestionKbps{0};  // mirror of m_guard.Cap() for the sender side
    int64_t m_lastDipNs = 0;         // network thread only
    int m_baseFecPercent = -1;       // the setting's FEC; network thread only
    std::atomic<int> m_dynamicFec{-1};  // what the packetizer currently runs; network thread only
    int64_t m_floorLogNs = 0;        // network thread only
    double m_queueBaselineMs = -1;   // the device's own queue floor; network thread only
    uint32_t m_maxKbps;
    bool m_adaptive;
    rc::StreamProfile m_profile = rc::StreamProfile::Balanced;
    std::atomic<bool> m_pace{false};
    double m_lossAvg = 0;
    uint32_t m_frameIndex = 0;

    // Loss recovery (OnFeedback runs on the network thread, the rest on the stream thread).
    std::mutex m_lossMutex;
    bool m_lossPending = false;
    uint32_t m_lossGoodFrame = 0;      // newest frame the headset decoded cleanly
    uint32_t m_lastKeyframe = 0;       // frame index of the newest keyframe sent
    uint32_t m_lastRecovery = 0;       // newest keyframe or recovery frame sent
    int64_t m_lastRecoveryNs = 0;
    bool m_nextIsRecovery = false;
    struct SentFrame {
        uint32_t index = 0;
        uint64_t timestamp = 0;  // EncodedFrame::timestamp
    };
    SentFrame m_sent[64];

    std::mutex m_statsMutex;
    int64_t m_windowStartNs = 0;
    uint32_t m_windowFrames = 0;
    uint64_t m_windowBytes = 0;
    double m_windowEncodeMs = 0;
    VideoSenderStats m_stats;
    int64_t m_lastStatsLogNs = 0;
    double m_queueMs = -1;            // smoothed queueing delay reported by the headset
    double m_receivedKbps = 0;        // smoothed rate that actually arrives
};

}  // namespace rcs
