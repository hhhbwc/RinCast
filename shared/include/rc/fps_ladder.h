// The frame-rate ladder the encoder reconfigures to. Real VRChat sessions swing
// 23-91 fps minute to minute; following every measured integer reconfigured NVENC
// 11.7-16.8 times per minute (peak 52), and each reconfigure resets rate control
// and doubles the VBV (1.11 <-> 2.22 Mbit). Quantizing onto a ladder keeps the
// budget meaningful while cutting the reconfigures to a few per minute.
//
// Pure arithmetic, simulated offline in the core tests; no locks, no clock.
#pragma once

#include <cmath>
#include <cstdint>

namespace rc {

constexpr int kFpsLadder[] = {24, 30, 45, 54, 60, 72, 90};

// Picks the reconfigure target for a measured frame rate:
//   - below 21 fps is treated as a stall (a load screen, a hang): the budget must
//     not inflate to the ladder's bottom rung because of one bad second;
//   - within 15% of the current setting: keep the current (no reconfigure);
//   - otherwise the largest ladder step at or under the measured rate. Down is
//     deliberate: the per-frame budget is ceiling / frameRateNum, so a rung ABOVE
//     the real rate understates every frame's budget and the stream undershoots,
//     while a rung below it overshoots into the queue. The 15% window bounds how
//     far the truth can drift before the ladder follows it.
constexpr int QuantizeFps(double measured, int current) {
    if (measured < 21.0) return 24;
    int rung = kFpsLadder[0];
    for (int step : kFpsLadder)
        if (step <= measured) rung = step;
    if (current > 0 && std::fabs(measured - double(current)) <= double(current) * 0.15) return current;
    return rung;
}

}  // namespace rc
