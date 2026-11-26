#pragma once

#include <algorithm>
#include <cstdint>

namespace rcs {

// Visits the last `count` entries written before monotonic index `end` in a ring
// of `capacity` slots. Uses unsigned subtraction (`end - count`) and modular offset
// arithmetic so uint32_t index wraparound never causes early loop exit.
template <typename Fn>
inline void VisitDiagRing(uint32_t end, uint32_t count, uint32_t capacity, Fn&& fn) {
    if (capacity == 0 || count == 0) return;
    count = std::min(count, capacity);
    const uint32_t start = end - count;
    for (uint32_t n = 0; n < count; ++n) {
        fn((start + n) % capacity);
    }
}

// Computes positive elapsed nanoseconds `(nowNs - sinceNs)` in the integer domain
// before converting to double, preserving single-nanosecond precision above 2^53
// and clamping invalid/skewed timestamps to 0.0.
inline double ElapsedPositiveNs(int64_t nowNs, uint64_t sinceNs) {
    if (!sinceNs || nowNs <= 0) return 0.0;
    const uint64_t nowU = uint64_t(nowNs);
    return nowU > sinceNs ? double(nowU - sinceNs) : 0.0;
}

// Computes `given - bridgeReplaced` with a lower bound of 0 so separate atomic
// counter exchanges on a 10 s boundary cannot underflow uint32_t.
inline uint32_t BoundedConvertedCount(uint32_t given, uint32_t bridgeReplaced) {
    return given > bridgeReplaced ? (given - bridgeReplaced) : 0u;
}

}  // namespace rcs
