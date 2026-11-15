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

// The budget NVENC receives is averageBitRate / frameRateNum, so a rung BELOW the rate
// frames really arrive at inflates every frame by real/rung - the overshoot the comment
// above names, which the ladder alone cannot undo. This converts the user's ceiling into
// the rate to hand the encoder so that the per-frame budget is ceiling / realFps whatever
// rung was picked: rate = ceiling * rung / real.
//
// Only the overshoot direction is corrected (anything at or above 0.98 is left alone):
// the setting is a ceiling, so a rung ABOVE the real rate - which under-spends - must not
// be inflated past what the user asked for. 0.5 is the same floor the measured-rate clamp
// uses: one bad second may not quarter the target.
//
// Measured on the real device 2026-11-16 00:05-00:09: 38 fps frames at the 30 fps rung
// gave 1.27x - 124 Mbps against a 100 Mbps setting, 390 KB/frame. The pacer could not
// drain it (390 KB at 108 Mbps is 28.9 ms; the frame period is 27 ms), the receive queue
// grew to 74 ms, and the headset reported 190-215 ms pose-to-display.
inline uint32_t ScaleRateToRealFps(uint32_t kbps, uint32_t rcFps, double realFps) {
    if (!kbps || !rcFps || realFps < 1.0) return kbps;
    const double ratio = double(rcFps) / realFps;
    if (ratio >= 0.98) return kbps;
    const double bounded = ratio < 0.5 ? 0.5 : ratio;
    return uint32_t(double(kbps) * bounded);
}

}  // namespace rc
