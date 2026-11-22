// A62: the QP offset a foveated frame gets at one angle off the optical axis.
// Pure so the tests can pin the two ends and the NaN rule; the caller feeds it the
// angle computed from a headset/game-reported fov, which is not validated upstream.
#pragma once

#include <algorithm>
#include <cmath>

namespace rc {

// Full quality out to `innerDeg`, smoothstep ramp to `maxDelta` at `outerDeg` and
// beyond. A non-finite angle is not an angle: it used to slip through std::clamp
// (clamp(NaN) is NaN) into std::lround and poison every block of the eye's QP map.
// 0 means "no offset", i.e. the picture is encoded evenly - the safe degradation.
inline int FoveaQpDelta(float deg, int maxDelta, float innerDeg = 25.0f, float outerDeg = 50.0f) {
    if (!std::isfinite(deg)) return 0;
    const float span = outerDeg > innerDeg ? outerDeg - innerDeg : 1.0f;
    const float t = std::clamp((deg - innerDeg) / span, 0.0f, 1.0f);
    return int(std::lround(t * t * (3.0f - 2.0f * t) * float(maxDelta)));
}

}  // namespace rc