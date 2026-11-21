#pragma once

// PLAN-AUTO-TUNE (2026-11-19): the bottleneck classifier and the auto-tune
// decision layer behind Settings::autoTune (default off - with the switch off,
// nothing here runs at all). Design authority: AUTO-TUNE-2026-11-19/
// DECISION-TABLE.md, where every threshold carries its evidence (a code cite
// or a frozen real-device log line). The house rules are the L7 ladder's
// (rc/autoscale.h): missing signals are never evidence, pressure must hold
// continuously before anything happens, any two actions sit a cooldown apart
// (the L7 ladder's own steps count - the session feeds its last step time in),
// everything moves one way only, and the user's manual writes win forever.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <set>

namespace rc {

// The five classes, evaluated in this order (DECISION-TABLE §2): first match
// wins, and the classifier itself is stateless - persistence lives below.
enum class Bottleneck : uint8_t {
    Ok = 0,
    DecodeBound,
    LinkBound,
    RateControl,
    GameBound,
};

inline const char* BottleneckName(Bottleneck b) {
    switch (b) {
        case Bottleneck::DecodeBound: return "decode-bound";
        case Bottleneck::LinkBound: return "link-bound";
        case Bottleneck::RateControl: return "rate-control";
        case Bottleneck::GameBound: return "game-bound";
        case Bottleneck::Ok: break;
    }
    return "ok";
}

// One classification tick's inputs. Every field keeps the sentinel its source
// already uses for "unknown" (DECISION-TABLE §1); an unknown signal can never
// fire its rule, so an old headset or a stats gap stays silent.
struct BottleneckSignal {
    double decodeMs = -1;        // headset decode average (kStats, ~1 Hz), <0 unknown
    double decodeBudgetMs = 0;   // 0.8 x 1000/maxFpsAtMaxRes (the L7 budget), 0 = no hint
    double droppedFps = -1;      // PC-side 10 s dropped average, <0 unknown
    double gameFps = 0;          // upstream game's 10 s average, 0 unknown
    double nominalFps = 0;       // the rate the stream requests, >0 known
    double qp = -1;              // encoder rolling QP mean (valid samples only), <0 unknown
    double mbps = 0;             // measured wire rate, >=0
    uint32_t ceilingKbps = 0;    // the user's ceiling (pre-guard), 0 unknown
    double queueMs = -1;         // queue the headset reports, <0 unknown
    double loss = 0;             // measured loss fraction 0..1
};

// A8 validity domains (TASK-AUTOTUNE-ROBUST-2026-11-20 §1c). loss is a
// fraction, valid only in [0, 1] (the :54 field comment). QP is codec-specific:
// H.264/HEVC quantisers live in 0..51, AV1 in 0..255 - the same domains
// QpStats::Add (fps_ladder.h kMaxValidQpH264Hevc/kMaxValidQpAv1, :251-261)
// enforces per frame when it rejects garbage ("typically hundreds of
// millions"). BottleneckSignal carries no codec tag, so the classifier's
// validity ceiling is the union of the two families: a rolling mean above 255
// cannot come from any supported codec.
constexpr double kClassifyLossMax = 1.0;
constexpr double kClassifyQpMaxH264Hevc = 51.0;
constexpr double kClassifyQpMaxAv1 = 255.0;

// Pure, ordered (DECISION-TABLE §2). The qp [16, 20) band with low fill is a
// deliberate dead zone: no real-device evidence for it yet, so no action.
inline Bottleneck ClassifyBottleneck(const BottleneckSignal& s) {
    // A8b: non-finite decodeMs/droppedFps/queueMs/loss/qp (+inf / NaN / -inf)
    // is garbage telemetry -> Unknown (the enum has no Unknown member; Ok is
    // the classifier's silent "nothing fired" state, the :41-43 unknown-signal
    // intent). The gate is unconditional - one non-finite field voids the
    // whole tick, so valid pressure in another field cannot fire through it.
    // NaN/-inf were already silent by comparison; +inf passed every bare >
    // guard (the A3 exposure) - the gate aligns all three, matching the
    // EncoderRateTracker::Observe precedent (fps_ladder.h:125). droppedFps is
    // in the gate too (A8 increment: it feeds R1's DecodeBound exactly like
    // decodeMs, so an infinite dropped average must not accuse either).
    if (!std::isfinite(s.decodeMs) || !std::isfinite(s.droppedFps) || !std::isfinite(s.queueMs) ||
        !std::isfinite(s.loss) || !std::isfinite(s.qp))
        return Bottleneck::Ok;
    // A8c: out-of-domain loss/qp is invalid -> Unknown, NOT clamped into range
    // (garbage telemetry must not be "repaired"). loss outside [0, 1] is a
    // broken fraction; qp above the codec ceiling is a leak/garbage mean no
    // codec can produce. qp < 0 is NOT garbage: it is the field's documented
    // unknown sentinel (:50 "<0 unknown", same contract as decodeMs/queueMs) -
    // it stays per-rule silent below and must not veto the other fields'
    // evidence (e.g. a valid decode over budget still classifies with qp
    // unknown).
    if (s.loss < 0.0 || s.loss > kClassifyLossMax || s.qp > kClassifyQpMaxAv1) return Bottleneck::Ok;
    // R1 decode-bound: the L7 budget rule, verbatim.
    if ((s.decodeMs > 0 && s.decodeBudgetMs > 0 && s.decodeMs > s.decodeBudgetMs) || s.droppedFps > 3.0)
        return Bottleneck::DecodeBound;
    // R2 link-bound: the loss brake's heavy threshold, and a queue above every
    // profile's dip line (the highest is Quality's 60 + 12 = 72 ms).
    if ((s.queueMs >= 0 && s.queueMs > 100.0) || s.loss > 0.05)
        return Bottleneck::LinkBound;
    // R3 rate-control: starving signature - wire well under the ceiling while
    // the QP is high (the frozen per-frame budget at low fps with CBR and no
    // QP floor; the 02:29 log line reads fill 0.53 / qp 23.8).
    if (s.ceilingKbps > 0 && s.mbps >= 0 && s.qp >= 20.0) {
        const double fill = s.mbps / (double(s.ceilingKbps) / 1000.0);
        if (fill < 0.70) return Bottleneck::RateControl;
    }
    // R4 game-bound: the game is slow but the picture is cheap - the encoder
    // has room, so no PC-side action can help.
    if (s.gameFps > 0 && s.nominalFps > 0 && s.gameFps < 0.75 * s.nominalFps && s.qp >= 0 && s.qp < 16.0)
        return Bottleneck::GameBound;
    return Bottleneck::Ok;
}

// What the decision layer may emit. The encode scale is NOT one of them: the
// L7 ladder (rc/autoscale.h) owns it and already runs under the autoTune gate;
// the session feeds its last step time in as part of the shared cooldown.
enum class AutoAction : uint8_t { None, LogOnly, CbrOff, FoveaOn, ProfileDown };

// The dimensions auto may act on (DECISION-TABLE §3.1). They carry no sentinel
// value (bools), so "the user or the headset wrote it since autoTune was
// enabled" is the only honest manual-priority test; enabling autoTune is the
// hand-over, and ClearLocks on re-enable starts a fresh one.
enum class AutoDim : uint8_t { Cbr, Fovea, Profile };

struct AutoTuneInput {
    bool cbrOn = false;    // the live stream's effective CBR (QualityProfileCbr)
    bool foveaOn = false;  // the foveation the live stream was built with
    // Foveation needs the headset's gazeFoveation too (client_session.cpp gates
    // the fovea path on it); without it the PC's flag alone does nothing, so
    // auto must not spend a rebuild on it. Auto never writes synced keys.
    bool foveaPossible = true;
    int profileId = 1;     // 0 latency, 1 balanced, 2 quality (the live stream's; 0 = not auto's to move)
};

class AutoTune {
   public:
    struct Config {
        int64_t overNs = 5'000'000'000LL;       // continuous pressure before an action (L7 precedent)
        int64_t cooldownNs = 30'000'000'000LL;  // between ANY two actions, ladder included
    };

    struct Result {
        Bottleneck cls = Bottleneck::Ok;
        AutoAction action = AutoAction::None;
        int profileId = 0;  // for ProfileDown: the profile to switch to
        bool recovered = false;
        const char* reason = "";  // set for every non-None result
    };

    void Configure(const Config& c) {
        std::lock_guard lock(m_mutex);
        m_cfg = c;
    }

    // The user (PC UI) or the headset wrote this dimension: auto steps aside
    // until autoTune is turned off and back on.
    void MarkManual(AutoDim d) {
        std::lock_guard lock(m_mutex);
        m_manual.insert(d);
    }

    // autoTune re-enabled (or the user's manual autoscale reset): a fresh
    // episode - locks stay (only a re-enable clears those), timers restart.
    void ResetEpisodes() {
        std::lock_guard lock(m_mutex);
        ResetEpisodesLocked();
    }

    void ClearLocks() {
        std::lock_guard lock(m_mutex);
        ResetEpisodesLocked();
        m_manual.clear();
    }

    // The L7 ladder stepped: it counts against the shared cooldown (one
    // variable at a time - DECISION-TABLE §3.3).
    void NoteLadderStep(int64_t nowNs) {
        std::lock_guard lock(m_mutex);
        m_lastActionNs = nowNs;
    }

    Result Update(const BottleneckSignal& s, const AutoTuneInput& in, int64_t nowNs) {
        std::lock_guard lock(m_mutex);
        Result r;
        r.cls = ClassifyBottleneck(s);
        // Session-level peak of what the wire actually carried: the closest
        // thing to an observed link capacity without new telemetry.
        m_peakMbps = std::max(m_peakMbps, s.mbps);
        if (r.cls != m_class) {
            if (r.cls == Bottleneck::Ok && m_class != Bottleneck::Ok) {
                // One explanation line on recovery, never an automatic undo.
                r.recovered = true;
                r.action = AutoAction::LogOnly;
                r.reason = "recovered";
            }
            m_class = r.cls;
            // The first tick of a bad class already counts as pressure (the L7
            // ladder counts from the first over-threshold sample too).
            m_over = r.cls != Bottleneck::Ok;
            m_overSinceNs = m_over ? nowNs : 0;
            m_loggedGameBound = false;
            m_loggedCeilingHint = false;
            return r;
        }
        if (r.cls == Bottleneck::Ok) return r;
        if (!m_over) {
            m_over = true;
            m_overSinceNs = nowNs;
        }
        if (nowNs - m_overSinceNs < m_cfg.overNs) return r;
        // The shared cooldown anchor: this layer's own actions and the L7
        // ladder's steps (fed in via NoteLadderStep) both count.
        if (m_lastActionNs && nowNs - m_lastActionNs < m_cfg.cooldownNs) return r;
        switch (r.cls) {
            case Bottleneck::DecodeBound:
                // The L7 ladder acts on this class, not this layer.
                break;
            case Bottleneck::RateControl:
                // Priority 2: CBR off (only when it is effectively on - writing
                // the key while capped VBR already runs would rebuild for nothing).
                if (!m_manual.count(AutoDim::Cbr) && in.cbrOn) {
                    m_lastActionNs = nowNs;
                    r.action = AutoAction::CbrOff;
                    r.reason = "rate-control: CBR off (capped VBR, QP floor and deep VBV return)";
                    break;
                }
                // Priority 3: foveation on - one way, never auto-disabled.
                if (!m_manual.count(AutoDim::Fovea) && in.foveaPossible && !in.foveaOn) {
                    m_lastActionNs = nowNs;
                    r.action = AutoAction::FoveaOn;
                    r.reason = "rate-control persists after CBR off: foveation on";
                    break;
                }
                // Priority 6 (exhausted): the ceiling hint, once per episode.
                if (!m_loggedCeilingHint && s.ceilingKbps > 0 && m_peakMbps > 0 &&
                    double(s.ceilingKbps) / 1000.0 > 2.0 * m_peakMbps) {
                    m_loggedCeilingHint = true;
                    r.action = AutoAction::LogOnly;
                    r.reason = "ceiling above twice the observed wire peak - consider lowering vrBitrateMbps";
                }
                break;
            case Bottleneck::LinkBound:
                // Priority 4: profile down one rung (queue budget 60 -> 45 -> 15 ms).
                if (!m_manual.count(AutoDim::Profile) && in.profileId > 0) {
                    m_lastActionNs = nowNs;
                    r.action = AutoAction::ProfileDown;
                    r.profileId = in.profileId - 1;
                    r.reason = "link-bound: profile down one rung (the guard brakes earlier)";
                }
                break;
            case Bottleneck::GameBound:
                // Priority 5: explain once per episode, act never.
                if (!m_loggedGameBound) {
                    m_loggedGameBound = true;
                    r.action = AutoAction::LogOnly;
                    r.reason = "game-bound: the upstream game is slow, no PC-side action helps";
                }
                break;
            case Bottleneck::Ok:
                break;
        }
        return r;
    }

   private:
    void ResetEpisodesLocked() {
        m_class = Bottleneck::Ok;
        m_over = false;
        m_overSinceNs = 0;
        m_lastActionNs = 0;
        m_loggedGameBound = false;
        m_loggedCeilingHint = false;
        m_peakMbps = 0;
    }

    Config m_cfg{};
    mutable std::mutex m_mutex;  // Update (worker) vs MarkManual/ClearLocks (app thread)
    std::set<AutoDim> m_manual;
    Bottleneck m_class = Bottleneck::Ok;
    bool m_over = false;         // the current class has held continuously
    int64_t m_overSinceNs = 0;
    int64_t m_lastActionNs = 0;  // any action by this layer or the ladder
    bool m_loggedGameBound = false;
    bool m_loggedCeilingHint = false;
    double m_peakMbps = 0;       // session peak of the measured wire rate
};

}  // namespace rc
