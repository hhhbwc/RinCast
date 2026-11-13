#include "video_sender.h"

#include <algorithm>

#include "rc/platform.h"

namespace rcs {

VideoSender::VideoSender(rc::Connection* connection, uint8_t streamId, rc::VideoCodec codec, uint32_t maxBitrateKbps,
                         int fecPercent, bool adaptive)
    : m_connection(connection),
      m_packetizer(connection),
      m_streamId(streamId),
      m_codec(codec),
      m_targetKbps(maxBitrateKbps),
      m_maxKbps(maxBitrateKbps),
      m_adaptive(adaptive) {
    m_packetizer.SetFecPercent(fecPercent);
    m_windowStartNs = rc::NowNs();
    m_worker = std::thread([this] { WorkerLoop(); });
}

VideoSender::~VideoSender() { Stop(); }

void VideoSender::Stop() {
    {
        std::lock_guard lock(m_slotMutex);
        if (m_stopped) return;
        m_stopped = true;
    }
    m_slotCv.notify_all();       // 唤醒阻塞在 Submit 里的编码线程，让它看到 false 退出
    m_packetizer.RequestStop();  // 唤醒 pacing 等待，worker 不用等到下一个包到期
    if (m_worker.joinable()) m_worker.join();
    // 流正在结束，槽里还没发出的那一帧直接丢掉（头显也已经不会再等它）。
    std::lock_guard lock(m_slotMutex);
    m_slot.frame = EncodedFrame{};
    m_slot.metadata.clear();
    m_slot.occupied = false;
}

void VideoSender::WorkerLoop() {
    for (;;) {
        Slot local;
        {
            std::unique_lock lock(m_slotMutex);
            m_slotCv.wait(lock, [this] { return m_slot.occupied || m_stopped; });
            if (!m_slot.occupied) return;  // 已停止且槽是空的
            local.frame = std::move(m_slot.frame);
            local.metadata = std::move(m_slot.metadata);
            local.flags = m_slot.flags;
            local.index = m_slot.index;
            m_slot.occupied = false;
        }
        // 槽已经空出来，立刻放编码线程去编下一帧：编码和这一帧的 paced 发送重叠，
        // 这是端到端延迟不增反降的来源。
        m_slotCv.notify_all();
        if (m_pace.load(std::memory_order_relaxed))
            m_packetizer.SetPacingKbps(uint32_t(TargetBitrateKbps() * 1.5));
        m_packetizer.SendFrame(m_streamId, m_codec, local.flags, local.index, local.metadata.data(),
                               uint16_t(local.metadata.size()), local.frame.data.data(),
                               local.frame.data.size());
    }
}

void VideoSender::SetMaxBitrate(uint32_t kbps) {
    m_maxKbps = kbps;
    if (!m_adaptive || m_targetKbps > kbps) m_targetKbps = kbps;
}

bool VideoSender::Submit(EncodedFrame&& frame, const void* metadata, uint16_t metadataSize, uint8_t extraFlags,
                         double encodeMs) {
    uint8_t flags = extraFlags;
    if (frame.keyframe) flags |= rc::kVideoKeyframe;
    const bool recovery = m_nextIsRecovery && !frame.keyframe;
    if (recovery) flags |= rc::kVideoRecovery;
    m_nextIsRecovery = false;
    const size_t frameBytes = frame.data.size();

    {
        std::unique_lock lock(m_slotMutex);
        // 槽被占用就等：上一帧发完之前不编下一帧。这一等不会持有 m_lossMutex / m_statsMutex。
        m_slotCv.wait(lock, [this] { return !m_slot.occupied || m_stopped; });
        if (m_stopped) return false;
        const uint32_t index = ++m_frameIndex;
        {
            // 丢包恢复簿记留在编码侧：提交顺序就是发送顺序，每帧必发不丢（上一帧此刻已被 worker 取走）。
            std::lock_guard lossLock(m_lossMutex);
            m_sent[index % std::size(m_sent)] = {index, frame.timestamp};
            if (frame.keyframe) m_lastKeyframe = index;
            if (frame.keyframe || recovery) {
                m_lastRecovery = index;
                m_lastRecoveryNs = rc::NowNs();
            }
        }
        m_slot.frame = std::move(frame);
        if (metadataSize) {
            const auto* b = static_cast<const uint8_t*>(metadata);
            m_slot.metadata.assign(b, b + metadataSize);
        } else {
            m_slot.metadata.clear();
        }
        m_slot.flags = flags;
        m_slot.index = index;
        m_slot.occupied = true;
    }
    m_slotCv.notify_all();  // 叫醒 worker

    std::lock_guard lock(m_statsMutex);
    m_windowFrames++;
    m_windowBytes += frameBytes;
    m_windowEncodeMs += encodeMs;
    const int64_t now = rc::NowNs();
    const double elapsed = double(now - m_windowStartNs) / 1e9;
    if (elapsed >= 1.0) {
        m_stats.fps = m_windowFrames / elapsed;
        m_stats.mbps = double(m_windowBytes) * 8 / 1e6 / elapsed;
        m_stats.encodeMs = m_windowFrames ? m_windowEncodeMs / m_windowFrames : 0;
        m_stats.bitrateKbps = m_targetKbps;
        if (now - m_lastStatsLogNs > 3'000'000'000LL) {
            m_lastStatsLogNs = now;
            RC_LOGI("Stream %u throughput: %.1f Mbps actual, %.0f fps, target %u kbps", m_streamId, m_stats.mbps,
                    m_stats.fps, m_targetKbps.load());
        }
        m_windowFrames = 0;
        m_windowBytes = 0;
        m_windowEncodeMs = 0;
        m_windowStartNs = now;
    }
    return true;
}

bool VideoSender::WantsFrame() {
    if (m_keyframeRequested) return true;
    std::lock_guard lock(m_lossMutex);
    return m_lossPending;
}

bool VideoSender::BeginFrame(VideoEncoder& encoder) {
    m_nextIsRecovery = false;
    bool keyframe = m_keyframeRequested.exchange(false);
    uint32_t good;
    uint64_t timestamp = 0;
    bool known = false;
    {
        std::lock_guard lock(m_lossMutex);
        if (!m_lossPending) return keyframe;
        m_lossPending = false;
        good = m_lossGoodFrame;
        const SentFrame& s = m_sent[good % std::size(m_sent)];
        // Frames before the newest keyframe can't be referenced any more.
        known = good != 0 && s.index == good && good >= m_lastKeyframe;
        timestamp = s.timestamp;
    }
    if (keyframe) return true;
    if (known && encoder.InvalidateAfter(timestamp)) {
        m_nextIsRecovery = true;
        return false;
    }
    return true;
}

void VideoSender::OnFeedback(const rc::VideoFeedbackPayload& fb) {
    if (fb.requestKeyframe) {
        std::lock_guard lock(m_lossMutex);
        // The headset asks again until it gets something to resume from. If a keyframe
        // or recovery frame is already on its way (it hasn't completed anything that
        // new yet), this request predates it: answering again would only send another
        // one. If that frame got lost, the headset completes newer frames, or the
        // timeout passes, and the request counts again.
        const bool inFlight = m_lastRecovery != 0 && int32_t(fb.lastCompleteFrame - m_lastRecovery) < 0 &&
                              rc::NowNs() - m_lastRecoveryNs < 300'000'000LL;
        if (!inFlight) {
            m_lossPending = true;
            m_lossGoodFrame = fb.lastGoodFrame;
        }
    }
    {
        std::lock_guard lock(m_statsMutex);
        m_stats.framesLost += fb.framesLost;
        m_stats.clientDecodeMs = fb.decodeMs;
        m_stats.clientLatencyMs = fb.totalLatencyMs;
    }
    // Signals for both modes: the queueing delay the headset measures is the main one
    // (a busy AP or an internet hop queues packets; the delay grows to hundreds of ms
    // before anything drops - loss alone is a late signal). Loss and the arrival rate
    // back it up (headsets that don't measure the delay have only lost frames).
    const int64_t now = rc::NowNs();
    const uint32_t total = fb.fragmentsReceived + fb.fragmentsLost;
    const double lossRate = total ? double(fb.fragmentsLost) / total : 0;
    const bool measured = fb.queueDelayMs >= 0;
    if (measured) m_queueMs = m_queueMs < 0 ? fb.queueDelayMs : m_queueMs * 0.6 + fb.queueDelayMs * 0.4;
    // Without frames in this report (a still desktop) there is nothing to judge by.
    const double q = measured ? m_queueMs : -1;
    // The device's own floor: some decode queues run 10-15 ms even when the link is
    // idle. Absolute thresholds would ratchet the guard down forever on those. The
    // baseline jumps down instantly with q, and creeps up very slowly if a queue
    // stays high (a sustained floor raise is real, just slow to accept).
    if (q >= 0 || m_queueBaselineMs >= 0) {
        const double qq = q >= 0 ? q : m_queueBaselineMs;
        m_queueBaselineMs = qq < m_queueBaselineMs || m_queueBaselineMs < 0 ? qq : m_queueBaselineMs + 0.01;
    }
    const double queueFloor = m_queueBaselineMs < 0 ? 0 : m_queueBaselineMs;
    if (fb.receivedKbps) m_receivedKbps = m_receivedKbps ? m_receivedKbps * 0.7 + fb.receivedKbps * 0.3 : fb.receivedKbps;
    if (m_baselineNetworkMs < 0 || fb.networkMs < m_baselineNetworkMs) m_baselineNetworkMs = fb.networkMs;
    m_baselineNetworkMs += 0.01;  // slowly forget
    {
        std::lock_guard lock(m_statsMutex);
        m_stats.queueMs = m_queueMs;
    }
    const bool queueHigh = q > queueFloor + 40;  // well past what the path carries
    // Wi-Fi and mobile links lose the odd packet without being full: only sustained or
    // heavy loss counts. (Headsets that measure no delay have nothing better than lost frames.)
    m_lossAvg = m_lossAvg * 0.8 + lossRate * 0.2;
    const bool lossy = m_lossAvg > 0.02 || lossRate > 0.2;
    const bool congested = q > queueFloor + 15 || lossy ||
                           (!measured && (fb.framesLost > 0 || fb.networkMs > m_baselineNetworkMs + 8.0));

    if (!m_adaptive) {
        // Fixed bitrate: the user asked for a constant rate. Chronic small losses are
        // FINE here - the recovery frames cover them - so they must not ratchet the
        // rate down. Only real HARM backs the rate off: the headset asking for a
        // keyframe (the freeze precursor), or the queue piling past the device's own
        // floor. Gentle steps - a harsh step per 300 ms swung the rate from 140 to 71
        // and back every couple of seconds. The ride hovers at the path's edge.
        const bool keyframeDip = fb.requestKeyframe;
        const bool queueDip = q > queueFloor + 25;
        const bool dipGate = now - m_lastDipNs > 500'000'000LL;
        if (keyframeDip || (queueDip && dipGate)) {
            m_lastDipNs = now;
            const uint32_t base = m_congestionKbps.load() ? m_congestionKbps.load() : m_targetKbps.load();
            uint32_t cap = keyframeDip ? base * 4 / 5 : base * 85 / 100;
            if (queueHigh && m_receivedKbps > 0) cap = std::min(cap, uint32_t(m_receivedKbps * 0.8));
            m_congestionKbps.store(std::max(cap, m_targetKbps.load() / 8));
            RC_LOGI("Stream %u: congestion guard dip to %u kbps (queue %.0f ms, floor %.0f, loss %.1f %%)",
                    m_streamId, m_congestionKbps.load(), q, queueFloor, lossRate * 100);
        } else if (m_congestionKbps.load() && now > m_congestedUntilNs &&
                   now - m_lastDipNs > 1'500'000'000LL) {
            // No harm for 1.5 s: climb gently. Small losses on the way are tolerated
            // (recovery frames) and do not reset this - only harm does.
            if (now - m_lastClimbNs > 500'000'000LL) {
                m_lastClimbNs = now;
                const uint32_t target = m_targetKbps.load();
                const uint32_t up = std::min(m_congestionKbps.load() * 21 / 20, target);
                m_congestionKbps.store(up);
                if (m_congestionKbps.load() >= target) RC_LOGI("Stream %u: congestion guard released, back to %u kbps",
                                                               m_streamId, target);
            }
        }
        return;
    }

    // Congestion control. When the stream is faster than the path (an internet upload,
    // weak Wi-Fi), the excess waits in some buffer on the way and every frame arrives
    // later than the one before - that queueing delay above is the AIMD's main signal.
    const uint32_t minKbps = std::max<uint32_t>(m_maxKbps / 20, 2000);
    uint32_t target = m_targetKbps;
    // A queue takes a moment to drain after a cut: only cut again once that had time,
    // or if the queue is still growing.
    const bool mayDecrease = now - m_lastDecreaseNs > 500'000'000LL ||
                             (now - m_lastDecreaseNs > 150'000'000LL && q > m_queueAtDecreaseMs * 1.5);
    // Already sending less than arrives: the backlog is draining, so a still high queue
    // is no reason to cut further (only new loss is).
    const bool draining = m_receivedKbps > 0 && target <= m_receivedKbps * 0.85 && !lossy;
    if (congested && mayDecrease && !draining) {
        double next = target * 0.85;
        // What arrives is what the path carries right now: go below it so the queue empties
        // (but at most halve per step: the arrival rate lags behind and starts out low).
        if (queueHigh && m_receivedKbps > 0) next = std::max(target * 0.5, std::min(target * 0.7, m_receivedKbps * 0.8));
        target = std::max(minKbps, uint32_t(next));
        m_lastDecreaseNs = now;
        m_lastIncreaseNs = now;
        m_queueAtDecreaseMs = std::max(q, 1.0);
        RC_LOGI("Stream %u: %s, bitrate %u kbps (queue %.0f ms, loss %.1f %%, arriving %.0f kbps)", m_streamId,
                queueHigh ? "path overloaded" : "congestion", target, q, lossRate * 100, m_receivedKbps);
    } else if (!congested && q < 5 && now - m_lastIncreaseNs > 500'000'000LL &&
               now - m_lastDecreaseNs > 2'000'000'000LL) {
        target = std::min(m_maxKbps, uint32_t(target * 1.05) + 500);
        m_lastIncreaseNs = now;
    }
    m_targetKbps = target;
}

VideoSenderStats VideoSender::Stats() {
    std::lock_guard lock(m_statsMutex);
    VideoSenderStats s = m_stats;
    m_stats.framesLost = 0;
    return s;
}

}  // namespace rcs
