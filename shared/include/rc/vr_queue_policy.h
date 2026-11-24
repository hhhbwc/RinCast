#pragma once

#include <cstdint>

namespace rc {

inline double VrQueueLimitNs(int64_t periodNs, int currentWanted) {
    const double period = double(periodNs);
    return period * (currentWanted == 0 ? 0.8 : 0.75);
}

inline int SelectVrQueueWanted(bool known, bool pcSlips, bool forceNoWait, int64_t periodNs,
                               double neededNs, int currentWanted, int64_t nowNs, int64_t keptUntilNs) {
    if (periodNs <= 0 || nowNs < keptUntilNs) return 1;
    return known && pcSlips && (forceNoWait || neededNs <= VrQueueLimitNs(periodNs, currentWanted)) ? 0 : 1;
}

}  // namespace rc
