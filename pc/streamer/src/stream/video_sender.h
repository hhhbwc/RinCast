// Shared plumbing for desktop and VR streams: packetizing, keyframe requests,
// adaptive bitrate (AIMD on loss / queuing delay) and statistics.
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "../encode/encoder.h"
#include "rc/connection.h"
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
};

class VideoSender {
   public:
    VideoSender(rc::Connection* connection, uint8_t streamId, rc::VideoCodec codec, uint32_t maxBitrateKbps,
                int fecPercent, bool adaptive);
    ~VideoSender();

    VideoSender(const VideoSender&) = delete;
    VideoSender& operator=(const VideoSender&) = delete;

    // 把一帧交给发送线程。槽被占用时阻塞（这就是编码侧的节流），返回时保证：
    // 上一帧已被发送线程取走（正在/即将发出）、这一帧刚进槽。返回 false 表示发送端已停止，调用方应退出。
    // 发送顺序即提交顺序、且每帧必发不丢，所以丢包恢复簿记（m_sent 环 / BeginFrame）留在编码侧仍然正确。
    bool Submit(EncodedFrame&& frame, const void* metadata, uint16_t metadataSize, uint8_t extraFlags,
                double encodeMs);

    // 停发送线程：唤醒阻塞的 Submit 和 pacing 等待，join，然后丢弃槽里没发出的帧。
    // 幂等；析构也会调。
    void Stop();

    void RequestKeyframe() { m_keyframeRequested = true; }
    // True if the headset is waiting for a frame (keyframe or loss recovery), so one
    // should be encoded even if the picture didn't change.
    bool WantsFrame();
    // Call right before encoding; returns whether to force a keyframe. A loss the
    // headset reported is answered by invalidating the lost frames in the encoder when
    // it can, so the next frame is a normal-sized one flagged kVideoRecovery.
    bool BeginFrame(VideoEncoder& encoder);

    void OnFeedback(const rc::VideoFeedbackPayload& fb);
    // Bitrate the encoder should use now (changes when adaptive).
    uint32_t TargetBitrateKbps() const {
        const uint32_t cap = m_congestionKbps.load();
        const uint32_t t = m_targetKbps.load();
        return cap ? std::min(cap, t) : t;
    }
    void SetMaxBitrate(uint32_t kbps);
    // 把每帧的包铺开在时间上发，而不是一口气打出去（局域网和互联网都用）：
    // Wi-Fi 的浅队列会被整帧突发打爆。必须在 Start/第一次 Submit 之前设好。
    void SetPacing(bool pace) { m_pace = pace; }
    // Adaptive only: begin below the maximum and probe upward.
    void StartAt(uint32_t kbps) {
        if (m_adaptive) m_targetKbps = std::min(kbps, m_maxKbps);
    }

    VideoSenderStats Stats();
    uint8_t StreamId() const { return m_streamId; }

   private:
    rc::Connection* m_connection;
    rc::VideoPacketizer m_packetizer;
    const uint8_t m_streamId;
    const rc::VideoCodec m_codec;
    std::atomic<bool> m_keyframeRequested{true};
    std::atomic<uint32_t> m_targetKbps;
    // Fixed-bitrate congestion guard (OnFeedback): dips below the target while the link
    // saturates, climbs back when the path looks calm. 0 = no dip active. Quality-adaptive
    // mode leaves this alone - its AIMD owns m_targetKbps.
    std::atomic<uint32_t> m_congestionKbps{0};
    int64_t m_lastDipNs = 0;         // network thread only
    int64_t m_lastClimbNs = 0;       // network thread only
    int64_t m_floorLogNs = 0;        // network thread only
    double m_queueBaselineMs = -1;   // the device's own queue floor; network thread only
    uint32_t m_maxKbps;
    bool m_adaptive;
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

    // 一帧缓冲（1-slot）+ 发送线程：编码和上一帧的发送重叠。Packetizer 非线程安全，
    // 只由 worker 线程碰；Submit 只碰槽，不碰 packetizer。
    struct Slot {
        EncodedFrame frame;
        std::vector<uint8_t> metadata;
        uint8_t flags = 0;
        uint32_t index = 0;
        bool occupied = false;
    };
    void WorkerLoop();
    Slot m_slot;
    std::mutex m_slotMutex;
    std::condition_variable m_slotCv;
    std::thread m_worker;
    bool m_stopped = false;  // 由 m_slotMutex 保护

    std::mutex m_statsMutex;
    int64_t m_windowStartNs = 0;
    uint32_t m_windowFrames = 0;
    uint64_t m_windowBytes = 0;
    double m_windowEncodeMs = 0;
    VideoSenderStats m_stats;
    int64_t m_lastStatsLogNs = 0;
    int64_t m_lastIncreaseNs = 0;
    int64_t m_lastDecreaseNs = 0;
    double m_baselineNetworkMs = -1;
    double m_queueMs = -1;            // smoothed queueing delay reported by the headset
    double m_queueAtDecreaseMs = 0;
    double m_receivedKbps = 0;        // smoothed rate that actually arrives
};

}  // namespace rcs
