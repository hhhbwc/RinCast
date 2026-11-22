#include "video_sender.h"

#include <algorithm>
#include <cstdio>

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
        case 3:
            SetPacing(true);
            m_packetizer.SetPacingFrameBudgetNs(frameBudgetNs);
            break;
        default: SetPacing(true); m_packetizer.SetPacingFrameBudgetNs(0); break;  // follow
    }
    // The wire ceiling belongs to the burst mode only: leaving it behind would change
    // the follow modes' encoder target, and their behaviour must stay as it was.
    if (mode != 3) {
        m_wireCeiling.Reset();
        m_wireCapKbps.store(0, std::memory_order_relaxed);
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
    // A33 (TASK-PACE-FOLLOW-2026-11-22, 2026-11-22): burst pacing bills min(the USER's
    // ceiling, the ADAPTIVE target), i.e. the billing rate follows the adaptive target
    // when it is pushed under the ceiling. The old rule - bill the user's ceiling
    // unconditionally - predates the A10 frame-granularity token bucket: back then an
    // encoded frame had to leave within its own frame period or the capacity-1 send
    // slot back-pressured the encoder and the queue climbed (205 ms at a 100 Mbps
    // ceiling). Under burst + token-bucket billing a frame no longer must, so keeping
    // the inflated rate only mattered when it hurt: with rate adaptation holding the
    // target under the ceiling on a collapsed link, the pacer kept releasing frames the
    // link could not carry - the real device's 479 ms send-side queue spike. Adaptive
    // off is a no-op: the target then IS the user's setting, so min(max, target) == max
    // == exactly the old bill. The selection lives in PaceBaseKbps (video_sender.h,
    // pure + tested); the legacy follow modes keep pacing from TargetBitrateKbps() so
    // remote paths and the follow control retain the old behavior. The encoder's
    // budget still obeys the guard in both modes.
    if (m_pace) {
        const int mode = m_pacingMode.load(std::memory_order_relaxed);
        const uint32_t paceBase = PaceBaseKbps(mode, m_maxKbps.load(), TargetBitrateKbps());
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
    // R4.2: 1 s sliding-window wire-byte ceiling (burst mode only). The window counts wire
    // bytes (headers + FEC parity), exactly what the packetizer puts on the path.
    const int paceMode = m_pacingMode.load(std::memory_order_relaxed);
    const uint32_t capKbps = m_maxKbps.load();
    size_t frameWireBytes = 0;
    if (paceMode == 3 && capKbps > 0) {
        const size_t fragPayload = m_connection->MaxPayload() - sizeof(rc::VideoFragmentHeader);
        const size_t total = 2 + metadataSize + frame.data.size();
        int fecPct = m_dynamicFec.load(std::memory_order_relaxed);
        if (fecPct < 0) fecPct = m_baseFecPercent;
        // The one wire-byte formula, shared with the offline sweep (video_sender.h).
        frameWireBytes = FrameWireBytes(fragPayload, total, fecPct);
    }
    m_packetizer.SendFrame(m_streamId, m_codec, flags, index, metadata, metadataSize, frame.data.data(),
                           frame.data.size());
    // R4.2 (D-026): the ceiling acts on the ENCODER TARGET, never on already-encoded
    // frames. An earlier revision delayed a frame whose wire bytes would push the
    // trailing second over the ceiling: that held the wire ceiling but walked bench
    // submit->sent 5.88 -> 8.25 ms avg / 11.4 -> 49.0 ms p99 (G4.3 FAIL). The target
    // route keeps the latency untouched: the encoder is simply told what the wire
    // ceiling means in encoded bytes, and the NEXT frames are smaller.
    if (paceMode == 3 && capKbps > 0) {
        rc::WireCeiling::Snapshot snap;
        if (m_wireCeiling.OnFrame(rc::NowNs(), frameWireBytes, frame.data.size(), capKbps, snap))
            m_wireCapKbps.store(snap.encoderTargetKbps, std::memory_order_relaxed);
    }

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
            // G10.4: the trailing network triad makes the picture complete on one
            // timeline - effective encoder target, queue the headset reports, and
            // measured loss. Leading fields stay byte-for-byte for the log parsers.
            const uint32_t refused = m_packetizer.SendFailures();
            const uint32_t effectiveTarget = TargetBitrateKbps();
            const uint32_t wireCap = m_wireCapKbps.load(std::memory_order_relaxed);
            const double queueMs = m_queueMs.load(std::memory_order_relaxed);
            const double lossAvg = m_lossAvg.load(std::memory_order_relaxed);
            // A79 phase M (reading only): the send-side pacing backlog and the burst
            // overrun counter on the same line - the pair that separates "the clock was
            // pushed ahead by debt" (backlog high, overruns flat) from "the socket held
            // the burst back" (overruns climb). Appended after the leading fields, which
            // stay byte-for-byte for the log parsers (tools/logpair.py PC_THROUGHPUT,
            // tools/diagnostics/analyze_dual.py RE_THROUGHPUT - neither is end-anchored).
            const double backlogMs = double(m_packetizer.SendBacklogNs()) / 1e6;
            const uint32_t overruns = m_packetizer.BudgetOverruns();
            if (refused) {
                RC_LOGI("Stream %u throughput: %.1f Mbps actual, %.0f fps, target %u kbps, %u datagrams refused, effective %u kbps, wireCap %u kbps, queue %.0f ms, loss %.1f%%, sendBacklog %.1f ms, overruns %u",
                        m_streamId, m_stats.mbps, m_stats.fps, m_targetKbps.load(), refused,
                        effectiveTarget, wireCap, queueMs, lossAvg * 100, backlogMs, overruns);
            } else {
                RC_LOGI("Stream %u throughput: %.1f Mbps actual, %.0f fps, target %u kbps, effective %u kbps, wireCap %u kbps, queue %.0f ms, loss %.1f%%, sendBacklog %.1f ms, overruns %u",
                        m_streamId, m_stats.mbps, m_stats.fps, m_targetKbps.load(),
                        effectiveTarget, wireCap, queueMs, lossAvg * 100, backlogMs, overruns);
            }
            // A81 M1+M2 (TASK-A81-LATENCY-BUDGET-UNITS-2026-11-23): what the profile trade
            // costs, and where the brake line actually sits. clientDecodeMs/clientLatencyMs
            // were collected into m_stats in OnFeedback and never printed; the capability
            // line lies (streamer.log 2026-11-23 01:43:33 pc.streamProfile=2 while
            // latency-first was in effect from 01:44:55 - 1407 dips against a 21 ms line).
            // dipAt mirrors queue_guard.h Update() from the LIVE config and the floor the
            // guard last saw - never a cached copy. cap 0 kbps = uncapped. n/a, not 0, when
            // the headset does not report: an old APK zero-fills decodeMs/totalLatencyMs
            // (client_session.cpp memcpys the truncated payload over the -1 defaults).
            const rc::QueueGuardConfig& cfg = m_guard.Config();
            const double floorMs = m_queueFloorMs.load(std::memory_order_relaxed);
            const double dipAt = std::max(cfg.targetMs + cfg.bandMs,
                                          (floorMs < 0 ? 0.0 : floorMs) + cfg.floorExtraMs);
            const char* profileName = m_profile == rc::StreamProfile::LowLatency ? "LowLatency"
                                    : m_profile == rc::StreamProfile::Quality    ? "Quality"
                                                                                 : "Balanced";
            char decBuf[16], latBuf[16];
            if (m_stats.clientDecodeMs > 0) snprintf(decBuf, sizeof(decBuf), "%.1f", m_stats.clientDecodeMs);
            else snprintf(decBuf, sizeof(decBuf), "n/a");
            if (m_stats.clientLatencyMs > 0) snprintf(latBuf, sizeof(latBuf), "%.1f", m_stats.clientLatencyMs);
            else snprintf(latBuf, sizeof(latBuf), "n/a");
            RC_LOGI("Stream %u latency: clientDec %s ms, clientLat %s ms, queue %.1f ms, "
                    "profile=%s dipAt %.1f ms cap %u kbps aim %u kbps",
                    m_streamId, decBuf, latBuf, queueMs, profileName, dipAt,
                    m_congestionKbps.load(std::memory_order_relaxed),
                    m_adaptive ? m_maxKbps.load() : m_targetKbps.load());
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
    if (measured) {
        const double prev = m_queueMs.load(std::memory_order_relaxed);
        m_queueMs.store(prev < 0 ? fb.queueDelayMs : prev * 0.6 + fb.queueDelayMs * 0.4,
                        std::memory_order_relaxed);
    }
    // Without frames in this report (a still desktop) there is nothing to judge by.
    const double q = measured ? m_queueMs.load(std::memory_order_relaxed) : -1;
    // The device's own floor: some decode queues run 10-15 ms even when the link is
    // idle. Absolute thresholds would ratchet the guard down forever on those. The
    // baseline jumps down instantly with q, and creeps up very slowly if a queue
    // stays high (a sustained floor raise is real, just slow to accept).
    if (q >= 0 || m_queueBaselineMs >= 0) {
        const double qq = q >= 0 ? q : m_queueBaselineMs;
        m_queueBaselineMs = qq < m_queueBaselineMs || m_queueBaselineMs < 0 ? qq : m_queueBaselineMs + 0.01;
    }
    const double queueFloor = m_queueBaselineMs < 0 ? 0 : m_queueBaselineMs;
    // A81 M2: publish the exact value Update() below receives, for the send thread's
    // latency readout. Record only - the guard's input and law are untouched.
    m_queueFloorMs.store(queueFloor, std::memory_order_relaxed);
    if (fb.receivedKbps) m_receivedKbps = m_receivedKbps ? m_receivedKbps * 0.7 + fb.receivedKbps * 0.3 : fb.receivedKbps;
    {
        std::lock_guard lock(m_statsMutex);
        m_stats.queueMs = m_queueMs.load(std::memory_order_relaxed);
    }
    // Wi-Fi and mobile links lose the odd packet without being full: only sustained or
    // heavy loss counts. (Headsets that measure no delay have nothing better than lost frames.)
    const double lossAvg = m_lossAvg.load(std::memory_order_relaxed) * 0.8 + lossRate * 0.2;
    m_lossAvg.store(lossAvg, std::memory_order_relaxed);
    // Loss-coupled parity: at the edge of a real link the steady state includes small
    // losses. The recovery frames cover single-frame gaps; a little more parity covers
    // the rest. Cheaper than the keyframe waits that unrecoverable frames cost. The
    // extra parity costs wire time, so it steps back down when the link calms.
    if (m_baseFecPercent >= 0 && measured) {
        const int fec = lossAvg > 0.10 ? 12 : lossAvg > 0.04 ? 8 : m_baseFecPercent;
        if (fec != m_dynamicFec.exchange(fec)) {
            m_packetizer.SetFecPercent(fec);
            RC_LOGI("Stream %u: FEC %d%% (loss %.1f %%)", m_streamId, fec, lossAvg * 100);
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
    const auto brake = rc::LossBrakeStep(m_brake, lossAvg, aim, now);
    const uint32_t guardCap = res.capKbps ? res.capKbps : UINT32_MAX;
    const uint32_t brakeCap = brake.capKbps ? brake.capKbps : UINT32_MAX;
    const uint32_t finalCap = guardCap < brakeCap ? guardCap : brakeCap;
    // R4.2 (D-026): the wire ceiling is NOT folded in here. The congestion cap stays
    // exactly what it was - the guard's cap and the brake's cap - so the follow modes
    // and the guard's own law are untouched. The wire ceiling rides on top of it in
    // TargetBitrateKbps() instead: that way it does not depend on the headset's
    // feedback cadence (the feedback-gated version never reached the encoder when
    // the reports were slow or absent - measured appliedKbps stayed at 100000).
    m_congestionKbps.store(finalCap == UINT32_MAX ? 0 : finalCap);
    // Sustained heavy loss means the path is below the user's ceiling - say it once
    // every 30 s so the setting vs. reality mismatch shows up in the log.
    if (lossAvg > 0.05) {
        if (!m_lossHeavySinceNs) m_lossHeavySinceNs = now;
        else if (now - m_lossHeavySinceNs > 30'000'000'000LL) {
            RC_LOGW("Stream %u: loss still %.1f %% at the 70 %% brake (path below the user's ceiling)",
                    m_streamId, lossAvg * 100);
            m_lossHeavySinceNs = now;
        }
    } else {
        m_lossHeavySinceNs = 0;
    }
    if (brake.changed) {
        if (brake.released) {
            RC_LOGI("Stream %u: loss brake released, back to %u kbps (loss %.1f %%)", m_streamId, aim,
                    lossAvg * 100);
        } else {
            const int64_t holdSecs = (m_brake.holdUntilNs - now) / 1000000000LL;
            RC_LOGI("Stream %u: loss brake %s %u kbps (loss %.1f %%, hold %lld s)", m_streamId,
                    brake.capKbps < res.capKbps || res.capKbps == 0 ? "cut to" : "holds", brake.capKbps,
                    lossAvg * 100, (long long)holdSecs);
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
    s.lossAvg = m_lossAvg.load(std::memory_order_relaxed);  // PLAN-AUTO-TUNE: the same read the throughput log already makes
    m_stats.framesLost = 0;
    return s;
}

}  // namespace rcs
