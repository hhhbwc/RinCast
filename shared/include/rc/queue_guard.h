#pragma once

#include <algorithm>
#include <cstdint>

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
};

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
        const uint32_t hardFloor = std::max<uint32_t>(targetKbps / m_cfg.minDivisor, 1000);
        const bool measured = queueMs >= 0;

        // The band, raised above the device's own queue so a link that simply has a deep
        // idle queue is not treated as congested for its whole life.
        const double climbUnder = std::max(m_cfg.targetMs - m_cfg.bandMs, queueFloorMs + 2.0);
        const double dipAt = std::max(m_cfg.targetMs + m_cfg.bandMs, queueFloorMs + m_cfg.floorExtraMs);

        // The ceiling only rises after a long clean stretch, so the climb stops just under
        // the level that failed instead of running back into it every few seconds.
        if (m_ceiling && nowNs - m_ceilingNs > m_cfg.ceilingRiseNs) {
            const uint32_t step = std::max<uint32_t>(m_ceiling * m_cfg.ceilingRisePercent / 100, 1);
            const uint32_t raised = m_ceiling + step;
            m_ceiling = raised >= targetKbps ? 0 : raised;  // cleared: try the target again
            m_ceilingNs = nowNs;
        }

        const auto dip = [&](uint32_t permille, bool congestion) {
            const uint32_t base = m_cap ? m_cap : targetKbps;
            const uint32_t next = std::max<uint32_t>(uint64_t(base) * permille / 1000, hardFloor);
            // The level that failed is the new ceiling: climbing straight back to it is what
            // made the stream pump between two levels every few seconds. Only a fresh episode
            // counts - while the queue from the last dip is still draining, every 400 ms looks
            // like another failure at a lower rate, and taking those as the ceiling ratcheted
            // it under the path's capacity (76 Mb/s on a 100 Mb/s link in the simulation).
            // And only congestion counts: a lost frame is not evidence about the path.
            if (congestion && nowNs - m_lastDipNs > 2'000'000'000LL) {
                m_ceiling = base;
                m_ceilingNs = nowNs;
            }
            if (m_cap && next >= m_cap) return false;  // already at the floor
            m_cap = next;
            m_lastDipNs = nowNs;
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
                    m_cap = aim;  // hold under the ceiling; no dip needed to stay there
                    if (m_cap == base) return;
                    r.action = Action::Climb;
                }
            } else {
                m_cap = next;
                r.action = Action::Climb;
            }
            m_lastClimbNs = nowNs;
        };

        // A keyframe request is a lost frame, not necessarily a full path: the recovery frame
        // answers it, so it dips once per episode and gently. It used to dip 65% on every
        // report, and one burst walked 140 -> 30 Mb/s in ten seconds.
        if (keyframeRequested && nowNs - m_lastKeyDipNs > 2'000'000'000LL) {
            m_lastKeyDipNs = nowNs;
            if (dip(measured && queueMs > dipAt ? 650 : 880, measured && queueMs > dipAt)) {
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
            if (nowNs - m_lastDipNs > 800'000'000LL && nowNs - m_lastClimbNs > 400'000'000LL && (calm || pinned)) {
                climb(pinned ? 1250 : 1040);
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
};

}  // namespace rc
