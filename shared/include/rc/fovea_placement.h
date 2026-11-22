// A61: where the full-density square goes in one eye. Pure so the tests can pin the
// arithmetic - the two failure modes it exists for (a zero-sum tangent pair and a
// non-finite gaze sample) both used to produce a NaN corner that the 0.04 move
// threshold then kept forever.
#pragma once

#include <algorithm>
#include <cmath>

#include "stream_profile.h"

namespace rc {

// `halves` is the eye's fov tangents (left, right, up, down); `gaze` is the eye's look
// vector (x right, y up, z forward as a negative), or nullptr. `s` is the remembered
// top-left corner and doubles as the hysteresis state. `placed` is false on the first
// frame and after a reset.
//
// An unusable pair falls back to the eye's centre: the squeezed size and the
// VrFoveationMetadata sent with the frame must describe the same square, and the centre
// needs no fov. The square is geometrically correct wherever it sits - a wrong place
// only puts the sharp region away from the lens centre.
inline void FoveaSquare(const float halves[4], const float* gaze, float f, bool placed, float s[2]) {
    float u = 0.5f, v = 0.5f;
    if (PlausibleFovPair(halves[0], halves[1]) && PlausibleFovPair(halves[2], halves[3])) {
        u = halves[0] / (halves[0] + halves[1]);  // straight ahead
        v = halves[2] / (halves[2] + halves[3]);
        if (gaze && gaze[2] < -0.2f) {
            u = (gaze[0] / -gaze[2] + halves[0]) / (halves[0] + halves[1]);
            v = (halves[2] - gaze[1] / -gaze[2]) / (halves[2] + halves[3]);
        }
        // A non-finite gaze sample is as unusable as a missing fov.
        if (!std::isfinite(u)) u = 0.5f;
        if (!std::isfinite(v)) v = 0.5f;
    }
    const float x = std::clamp(u - f / 2, 0.f, 1.f - f);
    const float y = std::clamp(v - f / 2, 0.f, 1.f - f);
    // Move only on a real move, so the encoder can keep referring to earlier frames.
    // A remembered corner that is not finite (from before this rule) is replaced.
    if (!placed || !std::isfinite(s[0]) || !std::isfinite(s[1]) || std::fabs(x - s[0]) > 0.04f ||
        std::fabs(y - s[1]) > 0.04f) {
        s[0] = x;
        s[1] = y;
    }
}

}  // namespace rc