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
    m_baseFecPercent = fecPercent;
    // Adaptive explores up to the maximum, so its ceiling rises quickly (10% per clean 5 s);
    // fixed bitrate only re-tests the setting, so it creeps (3% a minute). Same controller.
    if (m_adaptive) {
        rc::QueueGuardConfig cfg;
        cfg.ceilingRiseNs = 5'000'000'000LL;
        cfg.ceilingRisePercent = 10;
        m_guard.Configure(cfg);
    }
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
            // Pace at the rate we actually allow, not 1.5x it: at 140 Mb/s a frame is
            // ~190 KB and lands every 11 ms, so a 1.5x pacer (210 Mb/s) shipped it in
            // 7.4 ms and sat idle for the rest - the pacer shaped nothing, and every frame
            // went out as one 140-datagram burst. A Wi-Fi queue that overflows mid-burst
            // then loses a contiguous run of exactly one frame, which no FEC group this
            // size can repair. 8% of headroom absorbs the per-frame variance and the
            // timer's ~1 ms wake error without turning the stream back into bursts.
            m_packetizer.SetPacingKbps(uint32_t(TargetBitrateKbps() * 1.08));
        m_packetizer.SendFrame(m_streamId, m_codec, local.flags, local.index, local.metadata.data(),
                               uint16_t(local.metadata.size()), local.frame.data.data(),
                               local.frame.data.size());
    }
}

void VideoSender::StartAt(uint32_t kbps) {
    if (m_adaptive) m_guard.SeedCeiling(std::min(kbps, m_maxKbps), rc::NowNs());
}

void VideoSender::SetMaxBitrate(uint32_t kbps) {    m_maxKbps = kbps;
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
            // Datagrams the socket refused never reached the path: they look like loss on
            // the headset and like nothing at all here. Worth showing in the regular line.
            const uint32_t refused = m_packetizer.SendFailures();
            if (refused) {
                RC_LOGI("Stream %u throughput: %.1f Mbps actual, %.0f fps, target %u kbps, %u datagrams refused",
                        m_streamId, m_stats.mbps, m_stats.fps, m_targetKbps.load(), refused);
            } else {
                RC_LOGI("Stream %u throughput: %.1f Mbps actual, %.0f fps, target %u kbps", m_streamId, m_stats.mbps,
                        m_stats.fps, m_targetKbps.load());
            }
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
    {
        std::lock_guard lock(m_statsMutex);
        m_stats.queueMs = m_queueMs;
    }
    // Wi-Fi and mobile links lose the odd packet without being full: only sustained or
    // heavy loss counts. (Headsets that measure no delay have nothing better than lost frames.)
    m_lossAvg = m_lossAvg * 0.8 + lossRate * 0.2;
    // Loss-coupled parity: at the edge of a real link the steady state includes small
    // losses. The recovery frames cover single-frame gaps; a little more parity covers
    // the rest. Cheaper than the keyframe waits that unrecoverable frames cost. The
    // extra parity costs wire time, so it steps back down when the link calms.
    if (m_baseFecPercent >= 0 && measured) {
        const int fec = m_lossAvg > 0.10 ? 12 : m_lossAvg > 0.04 ? 8 : m_baseFecPercent;
        if (fec != m_dynamicFec.exchange(fec)) {
            m_packetizer.SetFecPercent(fec);
            RC_LOGI("Stream %u: FEC %d%% (loss %.1f %%)", m_streamId, fec, m_lossAvg * 100);
        }
    }

    // One controller for both modes. Fixed bitrate aims at what the user asked for and the
    // guard only goes under it; adaptive aims at the maximum and lets the same guard find
    // what the path carries. The law, and why it is a target with a dead band rather than a
    // threshold, is in rc/queue_guard.h - it is simulated there too.
    //
    // The old adaptive branch (an AIMD with "cut when the queue passes floor+15, grow when
    // it drops under 5") is gone: on a device whose own queue runs at 10-30 ms the grow
    // condition never came true, so it cut forever and sank to its floor, and its cuts were
    // sized by the arrival rate - the spiral that walked 119 Mb/s to 17.5 in four steps.
    const uint32_t aim = m_adaptive ? m_maxKbps : m_targetKbps.load();
    const auto res = m_guard.Update(aim, q, queueFloor, fb.requestKeyframe != 0, now);
    m_congestionKbps.store(res.capKbps);
    if (res.action == rc::QueueGuard::Action::Dip) {
        m_lastDipNs = now;
        RC_LOGI("Stream %u: guard dip to %u kbps (queue %.0f ms, floor %.0f, loss %.1f %%)", m_streamId,
                res.capKbps, q, queueFloor, lossRate * 100);
    } else if (res.action == rc::QueueGuard::Action::Climb) {
        RC_LOGD("Stream %u: guard climbed to %u kbps", m_streamId, res.capKbps);
    } else if (res.action == rc::QueueGuard::Action::Released) {
        RC_LOGI("Stream %u: congestion guard released, back to %u kbps", m_streamId, aim);
    }
    // A guard that sits at the floor otherwise leaves nothing behind. The queue alone
    // cannot say whether the path is full or the estimate is stale - the arrival rate
    // can, so log both and let the next test answer it.
    if (res.capKbps && res.capKbps <= aim / m_guard.Config().minDivisor &&
        now - m_floorLogNs > 5'000'000'000LL) {
        m_floorLogNs = now;
        RC_LOGI("Stream %u: guard at the floor, queue %.0f ms, sending %u of %u kbps, arriving %.0f kbps",
                m_streamId, q, res.capKbps, aim, m_receivedKbps);
    }
}

VideoSenderStats VideoSender::Stats() {
    std::lock_guard lock(m_statsMutex);
    VideoSenderStats s = m_stats;
    m_stats.framesLost = 0;
    return s;
}

}  // namespace rcs
