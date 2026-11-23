#pragma once

// PLAN-AUTO-TUNE (2026-11-19): the bottleneck classifier and the auto-tune
// decision layer behind Settings::autoTune (A84: default ON; with the switch off
// nothing here runs at all, which is exactly the branch every pre-A84 build had).
// Design authority: AUTO-TUNE-2026-11-19/
// DECISION-TABLE.md, where every threshold carries its evidence (a code cite
// or a frozen real-device log line). The house rules are the L7 ladder's
// (rc/autoscale.h): missing signals are never evidence, pressure must hold
// continuously before anything happens, any two actions sit a cooldown apart
// (the L7 ladder's own steps count - the session feeds its last step time in).
// A84 (TASK-A84-PROFILE-FLOOR-TWO-WAY-AUTOTUNE-2026-11-23) deliberately breaks
// the last two of those rules for the PROFILE dimension and only that one: the
// profile moves both ways now (the picture climbs back once the link has room),
// and a manual profile write no longer locks the dimension forever - the slider
// is a FLOOR (the lowest picture the user accepts), so writing it re-floors and
// hands the dimension back. The classifier below (ClassifyBottleneck, R1..R4 and
// the A8 validity gates) is untouched by A84: only the action layer moved.

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
// A88: the classifier has no Unknown member on purpose - Ok is its silent "nothing
// fired" state (see the note below). But "nothing fired" is not the same as "the
// link is clean": when the VR stream is paused or has no frames, every field sits
// at its unknown sentinel (the 2026-11-23 10:17 log: fps=0 game=0 qp=-1 wire=0
// queue=-1) and the periodic line still read `signals ok`. Feeding that to Update
// resets the climb streak and makes a black screen look like headroom. The caller
// skips the whole tick when this says false. One field reporting is enough - the
// test is deliberately an AND, not a vote.
inline bool HasUsableSignals(const BottleneckSignal& s) {
    return !(s.qp < 0 && s.mbps <= 0 && s.queueMs < 0 && s.gameFps <= 0);
}

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
// A84 appends ProfileUp (the values above keep the numbers they had - old log
// readers and the sweep tests bind on them): the profile climbs back over the
// user's floor and towards the sharpest picture when the link has room.
enum class AutoAction : uint8_t { None, LogOnly, CbrOff, FoveaOn, ProfileDown, ProfileUp };

// The dimensions auto may act on (DECISION-TABLE §3.1). They carry no sentinel
// value (bools), so "the user or the headset wrote it since autoTune was
// enabled" is the only honest manual-priority test; enabling autoTune is the
// hand-over, and ClearLocks on re-enable starts a fresh one.
// A84: this is still true for Cbr and Fovea. For Profile it is REVERSED - a
// manual write is the slider stating a new floor, and Update drops this lock the
// moment the floor it is fed moves (see profileFloorId).
enum class AutoDim : uint8_t { Cbr, Fovea, Profile };

// The three profiles are a ladder 0 (most responsive) / 1 / 2 (sharpest); nothing
// auto does may name a rung outside it, whatever a caller passes in.
inline int ClampProfileId(int id) { return id < 0 ? 0 : (id > 2 ? 2 : id); }

// A84b §4 (TASK-A84b): the rule that turns the slider (a FLOOR) plus what auto
// last chose into the rung a stream is BUILT at, as a pure function. It is the
// product rule ClientSession::EffectiveProfileLocked applies - moved here because
// rincast-core-tests links the shared headers only, so a rule that lives in
// client_session.cpp has no test that can go red on it, and the periodic log
// reading of §4 is exactly this pair of numbers. `autoRung` is the caller's stored
// choice, -1 = auto has not chosen yet. A84c added LiftRungToNewFloor beside it for
// the one thing a pure read cannot do: notice that the floor itself just moved.
//
// Invariants the floor semantics are built on, each one asserted in core_tests:
//   - off  -> the floor, always (the pre-A84 branch: the slider is an absolute
//     choice again, so effective == floor and never anything else);
//   - on, nothing chosen yet -> the floor, so auto starts from the user's choice;
//   - on -> the rung auto chose, clamped to the ladder and NOTHING else. It may sit
//     under the floor: that is the escape a link-bound stream needs when the floor
//     itself is what the wire cannot carry (the 5 s + 30 s brake at AutoAction::
//     ProfileDown), and the climb is then obliged to undo the deviation. A max() with
//     the floor stood here until A84c and made that whole branch a no-op: the tuner
//     proposed the cut, the caller stored it, and this line answered with the floor,
//     so the rebuild fired while nothing moved. A floor is where auto RETURNS, which
//     is what the slider copy in both UIs says; it is not a floor it cannot break.
//   - both inputs clamped, so a caller can never leave the 0/1/2 ladder.
// Because nothing here lifts, the caller owes one thing instead: see LiftRungToNewFloor.
inline int EffectiveProfileRung(int autoRung, int floorId, bool autoTuneOn) {
    if (!autoTuneOn || autoRung < 0) return ClampProfileId(floorId);
    return ClampProfileId(autoRung);
}

// A84c: the other half of the floor rule, and the half `EffectiveProfileRung` used to
// do by accident. When the user RAISES the floor above the rung the streams run at,
// the stream moves to the new floor on the spot - the slider is the one control in
// this page that has to look alive the moment it is touched, and auto's own climb
// waits 30 s of headroom plus a 45 s cooldown. Lowering the floor (or leaving it
// alone) changes nothing here: a dip already in progress stays dipped until the link
// calms, and a rung above the old floor is above the new one too.
// `autoRung` -1 (auto has not chosen) and `prevFloorId` -1 (floor never read) pass
// through untouched so the caller can feed this unconditionally on every read.
inline int LiftRungToNewFloor(int autoRung, int prevFloorId, int newFloorId) {
    if (autoRung < 0 || prevFloorId < 0) return autoRung;
    const int floor = ClampProfileId(newFloorId);
    return ClampProfileId(prevFloorId) < floor ? std::max(ClampProfileId(autoRung), floor) : autoRung;
}

struct AutoTuneInput {
    bool cbrOn = false;    // the live stream's effective CBR (QualityProfileCbr)
    bool foveaOn = false;  // the foveation the live stream was built with
    // Foveation needs the headset's gazeFoveation too (client_session.cpp gates
    // the fovea path on it); without it the PC's flag alone does nothing, so
    // auto must not spend a rebuild on it. Auto never writes synced keys.
    bool foveaPossible = true;
    int profileId = 1;     // the profile the LIVE stream is built at (A84: the
                           // effective one - the user's slider is profileFloorId)
    // A84 §2: the user's slider - the LOWEST picture profile they accept, not the
    // one they asked to run. Writing it is the user speaking about this dimension,
    // so a change here clears the Profile manual lock (the reversal).
    int profileFloorId = 0;
    // A84 §2 / A81 §M1-M2: the brake line the CURRENT profile pays, i.e. the
    // `dipAt` the latency log line prints (queue_guard.h's formula, read from the
    // live config - never a copy of the guard's state). 0 = unknown: an unknown
    // signal is never evidence, so an unknown line means no climb.
    double dipAtMs = 0;
};

class AutoTune {
   public:
    struct Config {
        // A84b §1 (TASK-A84b): the ONE enable decision. It lives here, in the
        // layer, because a gate only at the call site (the old `if (!pc.autoTune)
        // return;`) is unreachable from core_tests - the case that meant to cover
        // it replayed the gate as a lambda that returned the answer itself, so it
        // could never go red. ClientSession::RunAutoTune feeds Settings::autoTune
        // in through SetEnabled and reads it back with Enabled(); it does not
        // derive the decision a second time from the settings snapshot.
        bool enabled = true;
        int64_t overNs = 5'000'000'000LL;       // continuous pressure before an action (L7 precedent)
        int64_t cooldownNs = 30'000'000'000LL;  // between ANY two actions, ladder included
        // A84 §2: climbing is the direction the user is sensitive to (it trades
        // latency for picture), so its evidence is deliberately stricter than the
        // brake's and its cooldown is 1.5x the brake's. The two numbers above keep
        // the values they had - nothing about going DOWN changed.
        int64_t climbOkNs = 30'000'000'000LL;       // continuous Ok (classifier) before a climb
        int64_t climbCooldownNs = 45'000'000'000LL; // between a climb and any other action
    };

    struct Result {
        Bottleneck cls = Bottleneck::Ok;
        AutoAction action = AutoAction::None;
        int profileId = 0;  // for ProfileDown/ProfileUp: the profile to switch to
        bool recovered = false;
        const char* reason = "";  // set for every non-None result
        // A84 §6 / A84b §3: how long the evidence behind THIS action had been
        // continuous when it fired. It is carried by the two DIRECTIONAL profile
        // actions only - the pressure streak for ProfileDown, the clean Ok streak
        // for ProfileUp - because those are the moves A84 §6's log line has to
        // justify ("was this a 5 s blip or a 5 min flood?"). CbrOff/FoveaOn fire on
        // the same pressure streak but their pre-A84 log lines name no duration, so
        // filling this field for them would be data nothing reads. LogOnly
        // (recovered / ceiling / game-bound) explains; it has no streak to report.
        int64_t evidenceNs = 0;
    };

    void Configure(const Config& c) {
        std::lock_guard lock(m_mutex);
        m_cfg = c;
    }

    // A84b §1: the session's feed for Settings::autoTune. A setter narrow enough
    // that the call site cannot reset the timing numbers by accident, and the only
    // other writer of Config::enabled.
    void SetEnabled(bool on) {
        std::lock_guard lock(m_mutex);
        m_cfg.enabled = on;
    }

    // A84b §1: the enable decision read back from the single place it is stored.
    // RunAutoTune uses it only to skip gathering signals and the 30 s signals line
    // (DECISION-TABLE §4: an opted-out install sees nothing new) - the behaviour
    // itself is gated in Update().
    bool Enabled() const {
        std::lock_guard lock(m_mutex);
        return m_cfg.enabled;
    }

    // The user (PC UI) or the headset wrote this dimension: auto steps aside
    // until autoTune is turned off and back on - EXCEPT for Profile, where A84
    // reversed the rule: the slider states a floor, so the next tick that carries
    // a different floor than the last one clears this lock by itself.
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
        // A84b §1: the enable decision, enforced HERE. Disabled is INERT, not merely
        // silent - the return is above everything that reads or writes state, so the
        // off branch classifies nothing, accumulates no Ok streak, no pressure
        // streak, no floor, no cooldown anchor and no wire peak. The first tick after
        // a re-enable therefore behaves exactly like a session that never ran auto,
        // which is the half the old call-site gate could not be made to prove.
        if (!m_cfg.enabled) return r;
        r.cls = ClassifyBottleneck(s);
        // A84 §2: the slider moving IS the manual write reaching this layer. It
        // re-floors and hands the profile dimension back - the opposite of the
        // pre-A84 "one write locks it forever". A caller that never passes a floor
        // (the default 0, e.g. the sweep's BuildAutoTuneInput) never triggers this
        // and keeps the old lock semantics.
        // A84b §3: the comparison is clamped on BOTH sides, so m_profileFloor is the
        // rung the layer actually works to rather than a echo of whatever the caller
        // sent. An out-of-range floor (5, then 6 - both clamped to 2) states the same
        // floor and must not clear the lock once per tick; an unclamped comparison did.
        const int floorId = ClampProfileId(in.profileFloorId);
        if (floorId != m_profileFloor) {
            m_profileFloor = floorId;
            m_manual.erase(AutoDim::Profile);
        }
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
            // A84: the climb's own streak - it starts on the first Ok tick, which
            // is this one whenever the class has just calmed down. A84b §3: one
            // field, not a bool beside it (m_okSinceNs < 0 is "no streak"; the old
            // 0 sentinel collided with a legitimate t = 0, which is what the extra
            // bool existed to paper over).
            m_okSinceNs = m_over ? -1 : nowNs;
            m_loggedGameBound = false;
            m_loggedCeilingHint = false;
            return r;
        }
        if (r.cls == Bottleneck::Ok) {
            if (m_okSinceNs < 0) m_okSinceNs = nowNs;
            ClimbProfile(s, in, nowNs, r);  // A84: the way back up, and on past the floor
            return r;
        }
        m_okSinceNs = -1;
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
                // A84: `profileId` is the EFFECTIVE one, so this is the same rule
                // with the same numbers; what changed is that the value it moves is
                // no longer the user's slider. Going below the floor is allowed here
                // (a link-bound stream must still be able to save itself) and is the
                // deviation the climb below is obliged to undo.
                if (!m_manual.count(AutoDim::Profile) && in.profileId > 0) {
                    m_lastActionNs = nowNs;
                    r.action = AutoAction::ProfileDown;
                    r.profileId = in.profileId - 1;
                    r.evidenceNs = nowNs - m_overSinceNs;
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
    // A84 §2: the only way back up. Stricter than the brake on purpose (climbing
    // spends the latency the user asked for), and it never stops at the floor:
    // passing it on the way to 2 IS the point of this card. The caller holds the
    // lock and has already established this tick classifies Ok.
    void ClimbProfile(const BottleneckSignal& s, const AutoTuneInput& in, int64_t nowNs, Result& r) {
        if (m_manual.count(AutoDim::Profile)) return;  // locked without a floor write since
        if (ClampProfileId(in.profileId) >= 2) return;  // 2 is the ceiling: never past it
        if (nowNs - m_okSinceNs < m_cfg.climbOkNs) return;  // 30 s of continuous Ok first
        // The same anchor the brake uses, so a step down can never follow a climb
        // (or a ladder step) with no hysteresis - see the oscillation test in core_tests.
        if (m_lastActionNs && nowNs - m_lastActionNs < m_cfg.climbCooldownNs) return;
        // A81's readings, and the L7 house rule: an UNREPORTED queue is not a
        // clean queue. dipAt is the line the CURRENT profile pays, so a stream
        // sitting at 40 ms on a latency-first profile (line 21) may not climb.
        if (!(s.queueMs >= 0 && in.dipAtMs > 0 && s.queueMs < in.dipAtMs)) return;
        // The headset's decode inside the L7 budget - same pair of fields R1
        // accuses with, so the climb can never disagree with the classifier. An
        // unreported decode is not evidence of pressure (it never blocked the
        // Ok class either), so only a measured over-budget one vetoes the climb.
        if (s.decodeMs > 0 && s.decodeBudgetMs > 0 && s.decodeMs > s.decodeBudgetMs) return;
        m_lastActionNs = nowNs;
        r.action = AutoAction::ProfileUp;
        r.profileId = ClampProfileId(in.profileId) + 1;
        r.evidenceNs = nowNs - m_okSinceNs;
        r.reason = "headroom: profile up one rung (back towards the sharpest picture)";
    }

    void ResetEpisodesLocked() {
        m_class = Bottleneck::Ok;
        m_over = false;
        m_overSinceNs = 0;
        m_okSinceNs = -1;
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
    int64_t m_okSinceNs = -1;      // A84: the clean streak the climb reads, -1 = none
    int m_profileFloor = 0;      // A84: the last floor the slider stated (see Update)
    int64_t m_lastActionNs = 0;  // any action by this layer or the ladder
    bool m_loggedGameBound = false;
    bool m_loggedCeilingHint = false;
    double m_peakMbps = 0;       // session peak of the measured wire rate
};

}  // namespace rc
