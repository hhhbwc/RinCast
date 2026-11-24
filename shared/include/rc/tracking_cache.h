#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rc {

class TrackingTargetCache {
   public:
    static constexpr size_t kCapacity = 64;

    void Put(uint64_t sequence, int64_t targetDisplayPcNs, bool clockValid) {
        if (!sequence) return;
        for (size_t i = 0; i < m_count; i++) {
            if (m_entries[i].sequence == sequence) {
                m_entries[i].targetDisplayPcNs = targetDisplayPcNs;
                m_entries[i].clockValid = clockValid;
                return;
            }
        }
        m_entries[m_next] = {sequence, targetDisplayPcNs, clockValid};
        m_next = (m_next + 1) % kCapacity;
        if (m_count < kCapacity) m_count++;
    }

    bool Get(uint64_t sequence, int64_t& targetDisplayPcNs, bool& clockValid) const {
        if (!sequence) return false;
        for (size_t i = 0; i < m_count; i++) {
            if (m_entries[i].sequence == sequence) {
                targetDisplayPcNs = m_entries[i].targetDisplayPcNs;
                clockValid = m_entries[i].clockValid;
                return true;
            }
        }
        return false;
    }

    void Clear() {
        m_entries = {};
        m_next = 0;
        m_count = 0;
    }

   private:
    struct Entry {
        uint64_t sequence = 0;
        int64_t targetDisplayPcNs = 0;
        bool clockValid = false;
    };
    std::array<Entry, kCapacity> m_entries{};
    size_t m_next = 0;
    size_t m_count = 0;
};

struct PredictedDisplay {
    int64_t pcNs = 0;
    bool exact = false;
    bool clockValid = false;
    bool trackHit = false;
};

inline PredictedDisplay SelectPredictedDisplay(bool trackHit, int64_t targetDisplayPcNs, bool clockValid,
                                                int64_t submitPcNs, int64_t clockGuardNs,
                                                int64_t predictAheadNs) {
    PredictedDisplay out{submitPcNs + predictAheadNs, false, clockValid, trackHit};
    if (trackHit && clockValid && targetDisplayPcNs > submitPcNs - clockGuardNs) {
        out.pcNs = targetDisplayPcNs;
        out.exact = true;
    }
    return out;
}

}  // namespace rc
