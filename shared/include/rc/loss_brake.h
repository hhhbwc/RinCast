// A loss-driven brake that lives next to the queue guard: the guard watches the
// headset's queue, this one watches actual loss. The real link proved why both are
// needed (2026-11-15, 227 ENC samples): at the capacity edge loss arrives while the
// queue still reads 5-25 ms, the guard dips, and the climb logic walks straight
// back into the loss - 12 dips in 12 minutes, 88 <-> 99 Mbps, the user sees the
// picture blur in waves. The brake holds the rate down long enough for the path
// to settle, then releases at 2% per second.
//
// Pure arithmetic on plain state, simulated offline in the core tests; no locks,
// no clock of its own (nowNs is passed in). The queue guard is untouched - the
// sender takes whichever cap is lower.
#pragma once

#include <cstdint>

namespace rc {

struct LossBrakeState {
    uint32_t capKbps = 0;      // 0 = not braking
    int64_t holdUntilNs = 0;   // no climbing before this, whatever the loss says
    int64_t lastCutNs = 0;     // last change; also paces the 2%/s climb
};

struct LossBrakeResult {
    uint32_t capKbps = 0;      // 0 = not braking
    bool changed = false;
    bool released = false;     // the brake came off entirely this step
};

// One feedback step. lossAvg is the smoothed loss fraction (0..1), userKbps the
// ceiling the user set - the brake never holds the stream below 60% of it (the
// user's number is a contract, and a brake this deep means the path is gone).
constexpr LossBrakeResult LossBrakeStep(LossBrakeState& state, double lossAvg, uint32_t userKbps,
                                        int64_t nowNs) {
    LossBrakeResult r{state.capKbps, false, false};
    const uint32_t floorKbps = userKbps * 3 / 5;
    const int64_t second = 1'000'000'000LL;

    if (lossAvg > 0.05) {
        // Heavy loss: 70% of the user's number (never through the floor), and a
        // long hold. From an existing brake it only deepens if that is deeper.
        uint32_t target = userKbps * 7 / 10;
        if (state.capKbps && state.capKbps < target) target = state.capKbps;
        if (target < floorKbps) target = floorKbps;
        if (target != state.capKbps) {
            state.capKbps = target;
            state.holdUntilNs = nowNs + 15 * second;
            state.lastCutNs = nowNs;
            r.capKbps = target;
            r.changed = true;
        }
    } else if (lossAvg > 0.01) {
        // Light loss: 85% once, and each NEW cut (after the hold expired with loss
        // still above 1%) tightens by another 10%, down to the floor. During the
        // hold nothing happens - that is what the hold is.
        if (nowNs >= state.holdUntilNs) {
            uint32_t target = (state.capKbps && state.capKbps < userKbps) ? state.capKbps * 9 / 10
                                                                          : userKbps * 17 / 20;
            if (target < floorKbps) target = floorKbps;
            if (target != state.capKbps) {
                state.capKbps = target;
                state.holdUntilNs = nowNs + 8 * second;
                state.lastCutNs = nowNs;
                r.capKbps = target;
                r.changed = true;
            }
        }
    } else if (state.capKbps) {
        // The air looks clean. Inside the hold: nothing climbs, even at zero loss -
        // the real link showed loss arriving again within 1-2 s of the old climb.
        // After the hold: 2% of the user's number per second, and the brake comes
        // off once it is back at the ceiling.
        if (nowNs >= state.holdUntilNs && lossAvg < 0.003 && nowNs - state.lastCutNs >= second) {
            uint32_t next = state.capKbps + uint32_t(userKbps * 2 / 100);
            if (next >= userKbps) {
                state.capKbps = 0;
                state.holdUntilNs = 0;
                state.lastCutNs = nowNs;
                r.capKbps = userKbps;   // the result reads "back to the ceiling"
                r.changed = true;
                r.released = true;
            } else {
                state.capKbps = next;
                state.lastCutNs = nowNs;
                r.capKbps = next;
                r.changed = true;
            }
        }
    }
    return r;
}

}  // namespace rc
