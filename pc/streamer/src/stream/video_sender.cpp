#include "video_sender.h"

#include <algorithm>

#include "rc/platform.h"

namespace rcs {

VideoSender::VideoSender(rc::Connection* connection, uint8_t streamId, rc::VideoCodec codec, uint32_t maxBitrateKbps,
                         int fecPercent, bool adaptive, rc::StreamProfile profile)
    : m_connection(connection),
      m_packetizer(connection),
      m_streamId(streamId),
      m_codec(codec),
      m_targetKbps(maxBitrateKbps),
      m_maxKbps(maxBitrateKbps),
      m_adaptive(adaptive) {
    m_packetizer.SetFecPercent(fecPercent);
    m_baseFecPercent = fecPercent;
    SetProfile(profile);
    m_windowStartNs = rc::NowNs();
}

void VideoSender::SetProfile(rc::StreamProfile profile) {
    m_profile = profile;
    // The user's "how much delay do I accept" is the brake line, not an arbitrary
    // constant. The device's own idle queue is still added on top inside the guard.
    rc::QueueGuardConfig cfg = rc::GuardConfigFor(profile);
    // Adaptive explores up to the maximum, so its ceiling rises quickly (10% per clean 5 s);
    // fixed bitrate only re-tests the setting, so it creeps (3% a minute). Same controller.
    if (m_adaptive) {
        cfg.ceilingRiseNs = 5'000'000'000LL;
        cfg.ceilingRisePercent = 10;
    }
    m_guard.Configure(cfg);
}

VideoSender::~VideoSender() {
    m_packetizer.RequestStop();
}

void VideoSender::StartAt(uint32_t kbps) {
    if (m_adaptive) m_guard.SeedCeiling(std::min(kbps, m_maxKbps.load()), rc::NowNs());
}

void VideoSender::SetMaxBitrate(uint32_t kbps) {
    m_maxKbps = kbps;
    if (!m_adaptive || m_targetKbps > kbps) m_targetKbps = kbps;
}

void VideoSender::SetPacingMode(int mode, int64_t frameBudgetNs) {
    m_pacingMode.store(mode, std::memory_order_relaxed);
    m_pacingBudgetNs.store(frameBudgetNs, std::memory_order_relaxed);
    switch (mode) {
        case 1: SetPacing(false); m_packetizer.SetPacingFrameBudgetNs(0); break;
        case 3: SetPacing(true); m_packetizer.SetPacingFrameBudgetNs(frameBudgetNs); break;
        default: SetPacing(true); m_packetizer.SetPacingFrameBudgetNs(0); break;  // follow
    }
}

void VideoSender::Send(const EncodedFrame& frame, const void* metadata, uint16_t metadataSize, uint8_t extraFlags,
                       double encodeMs, bool lossAnswer) {
    uint8_t flags = extraFlags;
    if (frame.keyframe) flags |= rc::kVideoKeyframe;
    const bool recovery = lossAnswer && !frame.keyframe;
    if (recovery) flags |= rc::kVideoRecovery;
    const uint32_t index = ++m_frameIndex;
    {
        // 丢包恢复簿记：Send 按编码完成顺序被调用，每帧必发不丢。
        std::lock_guard lossLock(m_lossMutex);
        m_sent[index % std::size(m_sent)] = {index, frame.timestamp};
        if (frame.keyframe) m_lastKeyframe = index;
        if (frame.keyframe || lossAnswer) {
            m_lastRecovery = index;
            m_lastRecoveryNs = rc::NowNs();
        }
    }
    // Burst pacing follows the USER's ceiling, not the guard's cap: an encoded frame
    // must leave within its own frame period, otherwise the capacity-1 send slot
    // back-pressures the encoder and the queue climbs (the real device's 205 ms at a
    // 100 Mbps ceiling). The legacy follow mode keeps pacing from TargetBitrateKbps()
    // so remote paths and the follow control retain the old behavior. The encoder's
    // budget still obeys the guard in both modes.
    if (m_pace) {
        const int mode = m_pacingMode.load(std::memory_order_relaxed);
        const uint32_t paceBase = mode == 3 ? m_maxKbps.load() : TargetBitrateKbps();
        m_packetizer.SetPacingKbps(uint32_t(paceBase * 1.08));
        if (mode == 3) {
            int64_t budgetNs = m_pacingBudgetNs.load(std::memory_order_relaxed);
            if (budgetNs <= 0) {
                const double fps = m_measuredFps.load(std::memory_order_relaxed);
                budgetNs = fps > 1.0 ? int64_t(1e9 / fps) : 11'100'000LL;
            }
            m_packetizer.SetPacingFrameBudgetNs(budgetNs);
        }
    }
    m_packetizer.SendFrame(m_streamId, m_codec, flags, index, metadata, metadataSize, frame.data.data(),
                           frame.data.size());

    std::lock_guard lock(m_statsMutex);
    m_windowFrames++;
    m_windowBytes += frame.data.size();
    m_windowEncodeMs += encodeMs;
    const int64_t now = rc::NowNs();
    const double elapsed = double(now - m_windowStartNs) / 1e9;
    if (elapsed >= 1.0) {
        m_stats.fps = m_windowFrames / elapsed;
        m_measuredFps.store(m_stats.fps, std::memory_order_relaxed);
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
    // Only a request that actually starts a recovery is a new loss episode. Passing every
    // repetition straight to the guard bypassed the in-flight de-dup below and made one
    // lost frame look like sustained congestion (the 34 <-> 40 Mbps sawtooth, S3).
    bool keyframeEpisode = false;
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
            keyframeEpisode = true;
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
    const uint32_t aim = m_adaptive ? m_maxKbps.load() : m_targetKbps.load();
    const auto res = m_guard.Update(aim, q, queueFloor, keyframeEpisode, now);
    // The loss brake runs beside the guard: whichever cap is lower wins, and neither
    // side's climb can override the other's hold (the real link oscillated 88 <-> 99
    // Mbps for 12 minutes on loss-driven dips that the climb instantly undid).
    const auto brake = rc::LossBrakeStep(m_brake, m_lossAvg, aim, now);
    const uint32_t guardCap = res.capKbps ? res.capKbps : UINT32_MAX;
    const uint32_t brakeCap = brake.capKbps ? brake.capKbps : UINT32_MAX;
    const uint32_t finalCap = guardCap < brakeCap ? guardCap : brakeCap;
    m_congestionKbps.store(finalCap == UINT32_MAX ? 0 : finalCap);
    // Sustained heavy loss means the path is below the user's ceiling - say it once
    // every 30 s so the setting vs. reality mismatch shows up in the log.
    if (m_lossAvg > 0.05) {
        if (!m_lossHeavySinceNs) m_lossHeavySinceNs = now;
        else if (now - m_lossHeavySinceNs > 30'000'000'000LL) {
            RC_LOGW("Stream %u: loss still %.1f %% at the 70 %% brake (path below the user's ceiling)",
                    m_streamId, m_lossAvg * 100);
            m_lossHeavySinceNs = now;
        }
    } else {
        m_lossHeavySinceNs = 0;
    }
    if (brake.changed) {
        if (brake.released) {
            RC_LOGI("Stream %u: loss brake released, back to %u kbps (loss %.1f %%)", m_streamId, aim,
                    m_lossAvg * 100);
        } else {
            const int64_t holdSecs = (m_brake.holdUntilNs - now) / 1000000000LL;
            RC_LOGI("Stream %u: loss brake %s %u kbps (loss %.1f %%, hold %lld s)", m_streamId,
                    brake.capKbps < res.capKbps || res.capKbps == 0 ? "cut to" : "holds", brake.capKbps,
                    m_lossAvg * 100, (long long)holdSecs);
        }
    }
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
