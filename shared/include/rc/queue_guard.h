#pragma once

#include <algorithm>
#include <cstdint>

#include "rc/stream_profile.h"

namespace rc {

// The fixed-bitrate guard: it holds the send rate just under what the path carries by
// watching the queueing delay the headset reports, so a fixed rate still rides the edge
// of the link instead of overrunning it.
//
// A target with a dead band, not a threshold. A threshold that lands on the queue the link
// runs at while it carries the stream fires on every report - measured on this link: 114 of
// 156 dips sat at 25-31 ms against a 25 ms threshold with a natural queue of 27-30 ms - and
// a burst of reports then walks the rate to the floor in steps. Inside the band the rate
// simply stays, which is what keeps the picture still.
//
// Dips are fast and climbs are slow: the cost of one dip too many is a soft frame, the cost
// of one climb too many is a queued second.
struct QueueGuardConfig {
    // The queue to hold. This is the latency budget: on this link 27-30 ms is what an idle
    // AP adds at full rate, so 45 leaves room for the burst of a keyframe without spending
    // a third of a second on latency.
    double targetMs = 45.0;
    double bandMs = 12.0;     // target +- band is the dead band: no dips, no climbs
    double floorExtraMs = 25.0;  // never dip under the device's own queue plus this
    uint32_t minDivisor = 8;  // hard floor = target / minDivisor
    // A rate that just caused a dip is not a rate to climb back to. The climb stops just
    // under it, and the ceiling only rises after this long without a dip - otherwise every
    // cycle ends with two dips and a visible pump (measured: 140 -> 113 -> 140 every ten
    // seconds, nine encoder reconfigures per cycle). Adaptive mode sets both fields so the
    // ceiling explores upward quickly, since there the aim is the maximum, not the setting.
    int64_t ceilingRiseNs = 60'000'000'000LL;
    uint32_t ceilingRisePercent = 3;
    // A stream's first keyframe request is the headset asking for an I-frame to start
    // decoding, not a lost frame. Without this grace every stream start / profile switch
    // cut the rate 12% for two seconds while the queue was still -1..4 ms and loss 0%
    // (measured 2026-11-14: "guard dip to 123200 kbps (queue -1 ms, floor 0, loss 0.0 %)"
    // on each of the three profile switches). Real loss after the grace still dips.
    int64_t startupGraceNs = 2'000'000'000LL;
};

// A stream profile's "how much delay do I accept" becomes the guard's brake
// line. Kept next to the guard so the sender, the tests and any future caller
// all start from the same numbers.
inline QueueGuardConfig GuardConfigFor(StreamProfile profile) {
    QueueGuardConfig cfg;
    const StreamProfileParams p = ProfileParams(profile);
    cfg.targetMs = p.targetQueueMs;
    cfg.bandMs = p.bandMs;
    cfg.floorExtraMs = p.floorExtraMs;
    return cfg;
}

class QueueGuard {
public:
    enum class Action { None, Dip, Climb, Released };

    struct Result {
        uint32_t capKbps;  // 0 = no cap: the stream runs at the target
        Action action;
    };

    void Configure(const QueueGuardConfig& cfg) { m_cfg = cfg; }
    const QueueGuardConfig& Config() const { return m_cfg; }
    uint32_t Cap() const { return m_cap; }
    void Reset() {
        m_cap = 0;
        m_ceiling = 0;
        m_ceilingNs = 0;
        m_lastDipNs = 0;
        m_lastClimbNs = 0;
        m_lastKeyDipNs = 0;
        m_firstReportNs = -1;
        m_lastDipQueueMs = 0;
        m_episodeDips = 0;
        m_recoverWaitNs = 0;
        m_recoverPermille = 0;
    }
    // Puts the guard at a cap without a dip (a settings change, a new session).
    void Seed(uint32_t capKbps) { m_cap = capKbps; }
    // Adaptive mode starts below the maximum and explores upward: seeding the ceiling makes
    // the first climb stop there instead of jumping to the maximum on the first report.
    void SeedCeiling(uint32_t kbps, int64_t nowNs) {
        if (!kbps) return;
        m_ceiling = kbps;
        m_ceilingNs = nowNs;
    }

    // One feedback report. targetKbps is what the user asked for; queueMs is the headset's
    // queueing delay estimate (relative to its own recent best, see DelayTracker), or < 0
    // when the headset does not measure it; queueFloorMs is the device's own idle queue.
    Result Update(uint32_t targetKbps, double queueMs, double queueFloorMs, bool keyframeRequested, int64_t nowNs) {
        Result r{0, Action::None};
        if (!targetKbps) return r;
        if (m_firstReportNs < 0) m_firstReportNs = nowNs;
        const bool starting = nowNs - m_firstReportNs < m_cfg.startupGraceNs;
        const uint32_t hardFloor = std::max<uint32_t>(targetKbps / m_cfg.minDivisor, 1000);
        const bool measured = queueMs >= 0;

        // The band, raised above the device's own queue so a link that simply has a deep
        // idle queue is not treated as congested for its whole life.
        const double climbUnder = std::max(m_cfg.targetMs - m_cfg.bandMs, queueFloorMs + 2.0);
        const double dipAt = std::max(m_cfg.targetMs + m_cfg.bandMs, queueFloorMs + m_cfg.floorExtraMs);

        // Back under the band means the episode the last dip was about is over: forget its
        // queue, so a new flood is allowed to dip again and a stale huge value cannot disable
        // the protection for the rest of the session. Under the band, not merely under the dip
        // line: a queue that hovers at the top of the band while the rate is already below
        // what the path carries is the tail of the drain the last dip caused, not a new
        // episode, and re-dipping on it is the pump test 5 measures. The dip count ends with
        // it, so the next flood starts the recovery ladder at the cheap rung again; the pair
        // the last dip latched is what the climb reads, so an episode that is over still pays
        // for the dips it took.
        if (measured && queueMs < climbUnder) {
            m_lastDipQueueMs = 0;
            m_episodeDips = 0;
        }

        // The ceiling only rises after a long clean stretch, so the climb stops just under
        // the level that failed instead of running back into it every few seconds.
        // "Clean" means no dip at all, not "no dip that was worth recording": a cascade of
        // dips leaves the ceiling timer alone, so a timer measured from the ceiling alone
        // expired in the middle of a window that was still failing, and the working point
        // creeps up risePercent every period whatever the path does (field 2026-11-22:
        // climbed to 114664 at 22:50:36 and to 118104 at 22:51:36 - exactly 1.0300, one climb
        // a minute, with dips still in the log between them).
        if (m_ceiling && nowNs - m_ceilingNs > m_cfg.ceilingRiseNs &&
            nowNs - m_lastDipNs > m_cfg.ceilingRiseNs) {
            const uint32_t step = std::max<uint32_t>(m_ceiling * m_cfg.ceilingRisePercent / 100, 1);
            const uint32_t raised = m_ceiling + step;
            m_ceiling = raised >= targetKbps ? 0 : raised;  // cleared: try the target again
            m_ceilingNs = nowNs;
        }

        const auto dip = [&](uint32_t permille, bool congestion) {
            // A congestion dip is evidence about the path only if the queue is worse than the
            // one the last dip was taken against. The headset reports its queue relative to the
            // best of its last four seconds, so a backlog that the previous dip is already
            // draining keeps reading "congested" for seconds while it is going down - and
            // dipping against it is what made one field flood cut 180000 to 38442 kbps in three
            // seconds (seven dips 400 ms apart, the last four of them against a falling queue),
            // which is far under what the path carries and costs seconds of soft frames to
            // climb back out of. The first dip of an episode is untouched by this: nothing on
            // record means nothing being drained.
            if (congestion && measured && m_lastDipQueueMs > 0 && queueMs <= m_lastDipQueueMs) return false;
            const uint32_t base = m_cap ? m_cap : targetKbps;
            const uint32_t next = std::max<uint32_t>(uint64_t(base) * permille / 1000, hardFloor);
            // The level that failed is the new ceiling: climbing straight back to it is what
            // made the stream pump between two levels every few seconds. Only a fresh episode
            // counts - while the queue from the last dip is still draining, every 400 ms looks
            // like another failure at a lower rate, and taking those as the ceiling ratcheted
            // it under the path's capacity (76 Mb/s on a 100 Mb/s link in the simulation).
            // And only congestion counts: a lost frame is not evidence about the path.
            // Nor does a dip that happens while already at the floor: that says the path is
            // gone, not that the floor is the rate it carries. Recording it there set the
            // ceiling to the floor itself, aim fell under the current cap, and the climb then
            // walked the rate *down* - the stream sat at 16 Mb/s and could not climb out.
            const bool meaningful = base > hardFloor + hardFloor / 20;
            if (congestion && meaningful && nowNs - m_lastDipNs > 2'000'000'000LL) {
                m_ceiling = base;
                m_ceilingNs = nowNs;
            }
            if (m_cap && next >= m_cap) return false;  // already at the floor
            m_cap = next;
            m_lastDipNs = nowNs;
            m_lastDipQueueMs = queueMs;  // both dip paths only fire on a measured queue past dipAt
            m_episodeDips++;             // how far into this episode this dip is
            // What getting back up costs is rung by how many dips this episode has taken, not
            // by how deep this one was. It has to be latched here rather than read by the
            // climb: the first report under the band both ends the episode (the reset above)
            // and is the first one the climb is allowed to answer, so a live counter would
            // always read zero - measured 2026-11-23, where the ladder written against
            // m_episodeDips directly left TestGuardRecoveryCost's cascade row at the isolated
            // dip's 14.9 s and made mutation V-a of this card unkillable.
            // An isolated cut is one event on the wire, and the stream has to be still again
            // within the second of dead time and three seconds back to the aim that
            // TestGuardRecoveryCost now holds rung 1 to (measured 0.9 s and 1.9 s on its
            // "isolated 900" row). That is also what test 5's settling chain needs - the
            // +3 % ceiling rise 60 s after its last dip is followed 2.2 s later by the dip
            // that samples the window, and with A73's 2.8-7.8 s depth-based dead time that
            // chain ended at 19.9 s instead of 10.2 s and put the pump at i=221, inside the
            // window, at 92436-102707 = 10.00 % against the hi/12 gate.
            // The rungs after it are the field flood (22:48:38-45, seven dips in three
            // seconds, the last four against a queue the previous dip was already draining),
            // where every extra press buys more silence, because the next climb is answering
            // a backlog that has not arrived in the report yet.
            const uint32_t rung = std::min(m_episodeDips, 5u);
            m_recoverWaitNs = 800'000'000LL + int64_t(rung - 1) * 1'000'000'000LL;
            m_recoverPermille = 1025u - (rung - 1) * 3u;
            r.action = Action::Dip;
            return true;
        };
        const auto climb = [&](uint32_t permille) {
            const uint32_t base = m_cap ? m_cap : targetKbps;
            uint32_t next = uint32_t(uint64_t(base) * permille / 1000);
            if (next <= base) next = base + 1;
            const uint32_t aim = AimKbps(targetKbps);
            if (next >= aim) {
                if (aim >= targetKbps) {
                    m_cap = 0;
                    r.action = Action::Released;
                } else {
                    // Never let a climb lower the rate: an aim under the current cap means the
                    // ceiling is stale or was recorded at the floor, and moving down is the one
                    // thing a climb must not do (it walked 18.7 -> 16.5 Mb/s and stayed there).
                    const uint32_t held = std::max(aim, base);
                    if (held == base) return;
                    m_cap = held;  // hold under the ceiling; no dip needed to stay there
                    r.action = Action::Climb;
                }
            } else {
                m_cap = next;
                r.action = Action::Climb;
            }
            m_lastClimbNs = nowNs;
        };

        // A keyframe request is one lost FRAME, not evidence that the path is full: the
        // recovery frame answers it, FEC covers the gap, and LossBrake handles actual
        // packet loss. Dipping for the request alone made a clean link pump 34 <-> 40
        // Mbps every two seconds with queue 0 ms and loss 0 % (headset-sim --drop-every 2,
        // 2026-11-15): the 2 s throttle only paced the sawtooth, it did not remove it.
        // Only let a request deepen a dip the queue already justifies. It used to dip 65%
        // on every report, and one burst walked 140 -> 30 Mb/s in ten seconds.
        if (keyframeRequested && !starting && measured && queueMs > dipAt &&
            nowNs - m_lastKeyDipNs > 2'000'000'000LL) {
            m_lastKeyDipNs = nowNs;
            if (dip(650, true)) {
                r.capKbps = m_cap;
                return r;
            }
        }

        if (measured && queueMs > dipAt && nowNs - m_lastDipNs > 400'000'000LL) {
            // How far past the target decides the step: a queue a second deep needs to go
            // now, a queue 10 ms past the band does not.
            const bool deep = queueMs > dipAt * 2.5;
            if (dip(deep ? 750 : 900, true)) {
                r.capKbps = m_cap;
                return r;
            }
        }

        if (m_cap) {
            const bool calm = !measured || queueMs < climbUnder;
            const bool pinned = m_cap <= hardFloor && nowNs - m_lastDipNs > 4'000'000'000LL;
            // Wait out the drain the dip started before the first climb (a dip's own effect on
            // the reported queue takes seconds to arrive, so a climb 0.8 s later is answering a
            // queue that is already on its way down), and come back at the latched step for the
            // rest of that window. The step is never the +4 %/400 ms sprint HEAD used: four of
            // those inside the 2 s window FastestGain measures is 1.04^4 = 1.1698, over the
            // 1.12 gate: mutation V-b of this card, set back on 2026-11-23, printed test 8 at
            // 1.125 and the recovery rows at 1.1698 (verify-a73b/mutation-Vb.log). 1.025 to the
            // fourth is 1.1038, and it is still 2.7 x faster than the 10 % cut it is undoing.
            // `pinned` keeps the plain 800 ms and the 1250 self-rescue: sitting on the floor is
            // not a recovery, it is a stream that has stopped. Inherited from A73 and left
            // standing, but NOT verified here - measured 2026-11-23, taking `!pinned` out of
            // `waiting` (so a floor-pinned stream waits the full rung too) changes no printed
            // number and fails no CHECK, because nothing in the suite ever satisfies
            // `m_cap <= hardFloor`: test 6, the collapse scenario, bottoms at 17755 kbps
            // against a 17500 kbps floor (verify-a73b/mutation-Vd.log). Treat the clause as
            // intent carried over, not as a rule the tests pay for.
            const int64_t sinceDipNs = nowNs - m_lastDipNs;
            const bool dipped = m_recoverWaitNs > 0;  // set by every dip, cleared by Reset()
            const bool waiting = dipped && !pinned && sinceDipNs < m_recoverWaitNs;
            const bool recovering = dipped && sinceDipNs < m_recoverWaitNs * 3;
            if (!waiting && sinceDipNs > 800'000'000LL && nowNs - m_lastClimbNs > 400'000'000LL &&
                (calm || pinned)) {
                climb(pinned ? 1250 : (recovering ? m_recoverPermille : 1025));
            }
        }
        r.capKbps = m_cap;
        return r;
    }

private:
    // Where the climb is allowed to stop: just under the last rate that failed.
    uint32_t AimKbps(uint32_t targetKbps) const {
        if (!m_ceiling) return targetKbps;
        const uint32_t under = m_ceiling * 97 / 100;
        return std::min(targetKbps, std::max<uint32_t>(under, 2000u));
    }

    QueueGuardConfig m_cfg{};
    uint32_t m_cap = 0;
    uint32_t m_ceiling = 0;    // the rate that last caused a dip; 0 = unknown
    int64_t m_ceilingNs = 0;
    int64_t m_lastDipNs = 0;
    int64_t m_lastClimbNs = 0;
    int64_t m_lastKeyDipNs = 0;
    int64_t m_firstReportNs = -1;  // the first feedback report of this stream; -1 = none yet
    double m_lastDipQueueMs = 0;   // the queue the last dip was taken against; 0 = none on record
    uint32_t m_episodeDips = 0;    // how many dips the current episode has taken; 0 = none
    int64_t m_recoverWaitNs = 0;   // how long the climb waits after the last dip of that episode
    uint32_t m_recoverPermille = 0;  // the step the climbs use on the way back
};

}  // namespace rc
