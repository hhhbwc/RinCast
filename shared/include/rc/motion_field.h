// Frame generation: how the picture moved, carried after VrFrameMetadata in a VR frame.
//
//   [MotionHeader][runs: uint8 still blocks, uint8 moving blocks, then x, y per moving block]
//
// x and y are int8, in kMotionUnitPixels per frame. Head turns are already taken out, because
// the Quest's compositor handles those itself. Most blocks don't move, so this stays small.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace rc {

inline constexpr int kMotionUnitPixels = 4;
static_assert(kMotionUnitPixels == 4);
inline constexpr uint64_t kMinMotionIntervalNs = 4'000'000ULL;
inline constexpr uint64_t kMaxMotionIntervalNs = 200'000'000ULL;

inline float MotionFieldExtent(uint32_t cells, uint32_t blockSize) {
    return float(cells) * float(blockSize);
}

// Picture UV to normalized motion-texture coordinates; GL maps texel centers itself.
inline float MotionFieldUvScale(uint32_t cells, uint32_t blockSize, uint32_t outputPixels) {
    const float extent = MotionFieldExtent(cells, blockSize);
    return extent > 0.f && outputPixels ? float(outputPixels) / extent : 0.f;
}

// A18 (TASK-MOTION-HYSTERESIS-2026-11-22): block matching is at its worst on static text -
// repeated high-contrast glyphs mismatch into their neighbours, and a still block comes out
// with a bogus +-1 unit vector, +-3.4 px of displacement on the generated frame (x0.85xamount
// in the headset's shader). That is the "static text jitters" report. A Schmidt hysteresis on
// the quantized grid, per block and per component, keeps that noise out: a still block only
// starts moving on real motion of kMotionEnterPx or more, and a +-1 unit block only settles
// back to still once the motion falls under kMotionExitPx. Blocks already at 2+ units just
// follow quantization - the hysteresis only guards the 0 <-> 1 boundary, where the noise lives.
// The accepted trade: a slow pan of 2-4 px/frame can stay frozen out of the generated frame
// (the real frames still pan correctly), so on slow pans the generated frame may disagree
// with the real one by 2-4 px. Static text's 3.4 px jitter was the worse evil; a real-device
// A/B decides.
inline constexpr float kMotionEnterPx = 4.0f;  // 0 -> +-1 unit needs real motion >= 4 px/frame
inline constexpr float kMotionExitPx = 2.0f;   // +-1 unit -> 0 once real motion < 2 px/frame

// Quantizes `field` (pixels per frame) into `grid` (int8 units of kMotionUnitPixels) with that
// hysteresis. `grid` doubles as the state: on the way in it holds the previous frame's values
// (all zero on the first frame, after a reset, and wherever the grid was resized), on the way
// out this frame's. Per component:
//   prev == 0    : |field| >= kMotionEnterPx -> candidate; otherwise stays 0
//   |prev| == 1  : |field| <  kMotionExitPx  -> 0; otherwise candidate
//   |prev| >= 2  : candidate (large motion follows quantization, no hysteresis)
//   candidate = clamp(lround(field / kMotionUnitPixels), -127, 127)
// The wire format and the unit are untouched (old APKs decode this byte for byte): a block the
// hysteresis keeps at exactly 0 simply falls into a still run, so the noise blocks that used
// to shred the runs into moving ones disappear and the runs get longer.
inline void ApplyMotionHysteresis(const std::vector<float>& field, std::vector<int8_t>& grid) {
    const auto candidate = [](float px) {
        const float bounded = std::clamp(px / kMotionUnitPixels, -127.f, 127.f);
        return int8_t(std::lround(bounded));
    };
    grid.resize(field.size());  // new entries start at 0: the first frame / a reset sees an all-still grid
    for (size_t i = 0; i < field.size(); i++) {
        const float px = field[i];
        if (!std::isfinite(px)) {
            grid[i] = 0;
            continue;
        }
        const int prev = grid[i];
        if (prev == 0)
            grid[i] = std::fabs(px) >= kMotionEnterPx ? candidate(px) : int8_t(0);
        else if (prev == 1 || prev == -1)
            grid[i] = std::fabs(px) >= kMotionExitPx ? candidate(px) : int8_t(0);
        else
            grid[i] = candidate(px);
    }
}

struct MotionGate {
    bool worth = false;
    int moving = 0;
    int mean = 0;
    int spread = 0;
    int farRejected = 0;
    int coherent = 0;
    int global = 0;
};

// Static background must not veto a connected local object; moving blocks need directional and spatial consensus.
inline MotionGate JudgeMotionField(std::vector<int8_t>& grid, int w, int h) {
    MotionGate g;
    if (w < 1 || h < 1 || size_t(h) > std::numeric_limits<size_t>::max() / size_t(w)) return g;
    const size_t blocks = size_t(w) * size_t(h);
    if (blocks > std::numeric_limits<size_t>::max() / 2) return g;
    const size_t expected = blocks * 2;
    if (grid.size() < expected) return g;
    const auto mag = [](int v) { return v < 0 ? -v : v; };
    constexpr int kFarUnits = 12;
    for (size_t i = 0; i < expected; i += 2) {
        if (mag(grid[i]) > kFarUnits || mag(grid[i + 1]) > kFarUnits) {
            g.farRejected++;
            grid[i] = grid[i + 1] = 0;
        }
    }
    long sx = 0, sy = 0;
    for (size_t i = 0; i < expected; i += 2) {
        if (!grid[i] && !grid[i + 1]) continue;
        g.moving++;
        sx += grid[i];
        sy += grid[i + 1];
    }
    if (g.moving < 2) return g;
    const int mx = int(sx / g.moving), my = int(sy / g.moving);
    g.mean = mag(mx) + mag(my);

    int histX[2 * kFarUnits + 1]{};
    int histY[2 * kFarUnits + 1]{};
    for (size_t i = 0; i < expected; i += 2) {
        if (!grid[i] && !grid[i + 1]) continue;
        histX[int(grid[i]) + kFarUnits]++;
        histY[int(grid[i + 1]) + kFarUnits]++;
    }
    const auto median = [moving = g.moving](const int* hist) {
        size_t count = 0;
        for (int i = 0; i <= 2 * kFarUnits; i++) {
            count += size_t(hist[i]);
            if (count * 2 >= size_t(moving)) return i - kFarUnits;
        }
        return 0;
    };
    const int gx = median(histX), gy = median(histY);
    g.global = mag(gx) + mag(gy);
    const int tolerance = 2 + g.global / 2;
    int aligned = 0;
    for (size_t i = 0; i < expected; i += 2) {
        if (!grid[i] && !grid[i + 1]) continue;
        if (int(grid[i]) * gx + int(grid[i + 1]) * gy > 0) aligned++;
        if (mag(int(grid[i]) - gx) + mag(int(grid[i + 1]) - gy) > tolerance)
            g.spread++;
        else
            g.coherent++;
    }
    const size_t required = std::max<size_t>({size_t(2), blocks / 100, size_t(g.moving) / 2});
    if (g.global == 0 || size_t(g.coherent) < required || size_t(g.spread) > size_t(g.moving) / 2 ||
        size_t(aligned) < size_t(g.moving) * 3 / 5)
        return g;

    const auto eligible = [&](size_t block) {
        const int x = grid[block * 2], y = grid[block * 2 + 1];
        if ((!x && !y) || mag(x - gx) + mag(y - gy) > tolerance) return false;
        return x * gx + y * gy > 0;
    };
    std::vector<uint8_t> seen(blocks, 0);
    std::vector<size_t> pending;
    pending.reserve(g.coherent);
    const size_t minPatch = std::max<size_t>(4, blocks / 500);
    size_t supported = 0;
    for (size_t start = 0; start < blocks && supported < required; start++) {
        if (seen[start] || !eligible(start)) continue;
        size_t component = 0;
        pending.push_back(start);
        seen[start] = 1;
        while (!pending.empty()) {
            const size_t at = pending.back();
            pending.pop_back();
            component++;
            const int x = int(at % size_t(w)), y = int(at / size_t(w));
            const size_t neighbours[4] = {
                x > 0 ? at - 1 : blocks,
                x + 1 < w ? at + 1 : blocks,
                y > 0 ? at - size_t(w) : blocks,
                y + 1 < h ? at + size_t(w) : blocks,
            };
            for (size_t next : neighbours) {
                if (next == blocks || seen[next] || !eligible(next)) continue;
                if (!(w & 1) && ((next == at + 1 && x + 1 == w / 2) ||
                                (next == at - 1 && x == w / 2))) continue;
                seen[next] = 1;
                pending.push_back(next);
            }
        }
        if (component >= minPatch) supported += component;
    }
    g.worth = supported >= required;
    if (g.worth) {
        for (size_t block = 0; block < blocks; block++) {
            if (!eligible(block)) grid[block * 2] = grid[block * 2 + 1] = 0;
        }
    }
    return g;
}

// JudgeMotionField only clears outlying blocks when it ACCEPTS the field (worth == true). A
// rejected field therefore still carries every scattered block matching invented, and a forced
// cadence uploads it. Dropping blocks with no local agreement keeps real moving objects (a 2x2
// patch already has three agreeing neighbours) and turns the rest into a still field, which the
// shader then repeats instead of tearing. Eyes never support each other: the side-by-side frame
// splits at w/2, the same seam JudgeMotionField's connected-component walk refuses to cross.
inline void FilterMotionFieldLocalConsensus(std::vector<int8_t>& grid, int w, int h) {
    // Same guards as JudgeMotionField: this runs on header-supplied geometry, so the
    // multiplication must not be able to wrap before the size comparison.
    if (w < 1 || h < 1 || size_t(h) > std::numeric_limits<size_t>::max() / size_t(w)) return;
    const size_t blocks = size_t(w) * size_t(h);
    if (blocks > std::numeric_limits<size_t>::max() / 2) return;
    if (grid.size() < blocks * 2) return;
    std::vector<int8_t> cleaned(blocks * 2, 0);  // never read and write the same buffer
    const bool splitEyes = (w & 1) == 0;
    const int mid = w / 2;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const size_t at = (size_t(y) * size_t(w) + size_t(x)) * 2;
            const int vx = grid[at], vy = grid[at + 1];
            if (!vx && !vy) continue;
            const int x0 = splitEyes && x >= mid ? mid : 0;
            const int x1 = splitEyes && x < mid ? mid : w;
            int agree = 0;
            for (int dy = -1; dy <= 1 && agree < 3; dy++) {
                const int ny = y + dy;
                if (ny < 0 || ny >= h) continue;
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy) continue;
                    const int nx = x + dx;
                    if (nx < x0 || nx >= x1) continue;
                    const size_t neighbour = (size_t(ny) * size_t(w) + size_t(nx)) * 2;
                    const int nvx = grid[neighbour], nvy = grid[neighbour + 1];
                    if (!nvx && !nvy) continue;
                    if (nvx * vx + nvy * vy <= 0) continue;  // opposite or perpendicular
                    if (std::abs(nvx - vx) + std::abs(nvy - vy) <= 2) agree++;
                }
            }
            if (agree >= 3) {
                cleaned[at] = int8_t(vx);
                cleaned[at + 1] = int8_t(vy);
            }
        }
    }
    grid.swap(cleaned);
}

#pragma pack(push, 1)
struct MotionHeader {
    uint16_t gridWidth;
    uint16_t gridHeight;
    uint16_t blockSize;           // pixels of the side-by-side frame per grid cell
    uint16_t reserved;
    uint64_t previousTimestampNs;  // current frame timestamp minus the source interval these vectors span
};
#pragma pack(pop)
static_assert(sizeof(MotionHeader) == 16);

// The field's endpoint is the picture it measured, which can be older than the frame being encoded.
inline uint64_t MotionPreviousTimestamp(uint64_t endpointTimestampNs, uint64_t intervalNs) {
    return intervalNs && intervalNs <= endpointTimestampNs ? endpointTimestampNs - intervalNs : endpointTimestampNs;
}

// Interval between consecutive captured frames. A shared or rewound headset target is not a
// shorter capture period: keep the last valid interval, and only use the host submit gap when
// no target-derived period exists yet.
inline uint64_t ResolveMotionIntervalNs(uint64_t currentTargetNs, uint64_t previousTargetNs,
                                        int64_t currentSubmitNs, int64_t previousSubmitNs,
                                        uint64_t lastIntervalNs) {
    if (!previousTargetNs) return 0;
    const bool validLast = lastIntervalNs >= kMinMotionIntervalNs && lastIntervalNs <= kMaxMotionIntervalNs;
    if (currentTargetNs > previousTargetNs) {
        const uint64_t delta = currentTargetNs - previousTargetNs;
        if (delta >= kMinMotionIntervalNs && delta <= kMaxMotionIntervalNs) return delta;
        if (validLast) return lastIntervalNs;
        return std::clamp(delta, kMinMotionIntervalNs, kMaxMotionIntervalNs);
    }
    if (validLast) return lastIntervalNs;
    if (previousSubmitNs && currentSubmitNs > previousSubmitNs) {
        return std::clamp(uint64_t(currentSubmitNs - previousSubmitNs), kMinMotionIntervalNs, kMaxMotionIntervalNs);
    }
    return 0;
}

inline bool ShouldResetMotionPrevious(uint64_t currentFrameIndex, uint64_t previousFrameIndex,
                                      bool deliberateSkipPending) {
    if (!previousFrameIndex) return false;
    if (currentFrameIndex <= previousFrameIndex) return true;
    return currentFrameIndex - previousFrameIndex != 1 &&
           !(deliberateSkipPending && currentFrameIndex - previousFrameIndex == 2);
}

struct MotionReadbackTiming {
    uint64_t timestampNs = 0;
    uint64_t intervalNs = 0;
    uint64_t frameIndex = 0;
};

struct MotionMailboxStats {
    uint64_t produced = 0;
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t expired = 0;
    uint64_t evicted = 0;
    uint64_t cleared = 0;
    uint64_t invalid = 0;
    uint64_t pending = 0;
    uint64_t oldestTimestampNs = 0;
    uint64_t newestTimestampNs = 0;
    uint64_t oldestEnqueuedNs = 0;
    uint64_t newestEnqueuedNs = 0;
};

inline bool MotionMailboxHasActivity(const MotionMailboxStats& current, const MotionMailboxStats& previous) {
    return current.pending || current.produced != previous.produced || current.hits != previous.hits ||
           current.misses != previous.misses || current.expired != previous.expired ||
           current.evicted != previous.evicted || current.cleared != previous.cleared ||
           current.invalid != previous.invalid;
}

inline bool ShouldLogMotionDiagnostics(bool enabled, bool hasMailboxActivity,
                                      bool stateKnown, bool previousEnabled) {
    return enabled || hasMailboxActivity || !stateKnown || enabled != previousEnabled;
}

// Readback and encoding run on different threads; only the field whose frame and endpoint match
// the encoded frame may be attached to it. frameIndex is PC-internal: the headset target can be
// shared or rewound without collapsing two distinct fields. Enqueue time uses the producer's
// monotonic clock, not the headset endpoint clock carried by timestampNs.
class MotionFieldMailbox {
   public:
    static constexpr size_t kCapacity = 8;

    void Put(uint64_t frameIndex, uint64_t timestampNs, std::vector<uint8_t> data, uint64_t enqueuedNs) {
        if (!frameIndex || !timestampNs) {
            std::lock_guard lock(m_mutex);
            ++m_stats.invalid;
            return;
        }
        std::lock_guard lock(m_mutex);
        m_pending[frameIndex] = {timestampNs, enqueuedNs, std::move(data)};
        ++m_stats.produced;
        while (m_pending.size() > kCapacity) {
            m_pending.erase(m_pending.begin());
            ++m_stats.evicted;
        }
    }

    // Unbounded test-only accessor: no byte-size gate.  Production code MUST use
    // TakeIfFits() so the wire's uint16 metadata length can never be exceeded.
    bool TakeUnboundedForTest(uint64_t frameIndex, uint64_t timestampNs, std::vector<uint8_t>& out) {
        std::lock_guard lock(m_mutex);
        const auto it = m_pending.find(frameIndex);
        if (it == m_pending.end()) {
            ++m_stats.misses;
            return false;
        }
        if (it->second.timestampNs != timestampNs) {
            ++m_stats.invalid;
            return false;
        }
        return TakeMatchedLocked(frameIndex, timestampNs, out);
    }

    bool TakeIfFits(uint64_t frameIndex, uint64_t timestampNs, size_t maxBytes,
                    std::vector<uint8_t>& out, bool* rejectedOversize = nullptr) {
        if (rejectedOversize) *rejectedOversize = false;
        std::lock_guard lock(m_mutex);
        const auto it = m_pending.find(frameIndex);
        if (it == m_pending.end()) {
            ++m_stats.misses;
            return false;
        }
        if (it->second.timestampNs != timestampNs) {
            ++m_stats.invalid;
            return false;
        }
        if (it->second.data.size() > maxBytes) {
            if (rejectedOversize) *rejectedOversize = true;
            return false;
        }
        return TakeMatchedLocked(frameIndex, timestampNs, out);
    }

    void Clear() {
        std::lock_guard lock(m_mutex);
        m_stats.cleared += m_pending.size();
        m_pending.clear();
    }

    bool HasTimestamp(uint64_t frameIndex, uint64_t timestampNs) const {
        std::lock_guard lock(m_mutex);
        const auto it = m_pending.find(frameIndex);
        return it != m_pending.end() && it->second.timestampNs == timestampNs;
    }

    MotionMailboxStats Stats() const {
        std::lock_guard lock(m_mutex);
        auto stats = m_stats;
        stats.pending = m_pending.size();
        if (!m_pending.empty()) {
            stats.oldestTimestampNs = m_pending.begin()->second.timestampNs;
            stats.newestTimestampNs = m_pending.rbegin()->second.timestampNs;
            stats.oldestEnqueuedNs = m_pending.begin()->second.enqueuedNs;
            stats.newestEnqueuedNs = m_pending.rbegin()->second.enqueuedNs;
        }
        return stats;
    }

   private:
    bool TakeMatchedLocked(uint64_t frameIndex, uint64_t timestampNs, std::vector<uint8_t>& out) {
        const auto it = m_pending.find(frameIndex);
        out = std::move(it->second.data);
        m_stats.expired += uint64_t(std::distance(m_pending.begin(), it));
        m_pending.erase(m_pending.begin(), std::next(it));
        ++m_stats.hits;
        return true;
    }

    struct PendingField {
        uint64_t timestampNs;
        uint64_t enqueuedNs;
        std::vector<uint8_t> data;
    };
    mutable std::mutex m_mutex;
    std::map<uint64_t, PendingField> m_pending;
    MotionMailboxStats m_stats;
};

// grid: x, y per block, row by row.
inline void EncodeMotionRuns(const int8_t* grid, size_t blocks, std::vector<uint8_t>& out) {
    size_t i = 0;
    while (i < blocks) {
        size_t still = 0, moving = 0;
        while (i + still < blocks && still < 255 && grid[(i + still) * 2] == 0 && grid[(i + still) * 2 + 1] == 0) still++;
        const size_t start = i + still;
        while (start + moving < blocks && moving < 255 && (grid[(start + moving) * 2] != 0 || grid[(start + moving) * 2 + 1] != 0))
            moving++;
        out.push_back(uint8_t(still));
        out.push_back(uint8_t(moving));
        const auto* bytes = reinterpret_cast<const uint8_t*>(grid + start * 2);
        out.insert(out.end(), bytes, bytes + moving * 2);
        i = start + moving;
    }
}

// False if the data doesn't describe exactly `blocks` blocks.
inline bool DecodeMotionRuns(const uint8_t* data, size_t size, size_t blocks, std::vector<int8_t>& grid) {
    size_t i = 0, at = 0;
    while (at < size) {
        if (size - at < 2) return false;
        const size_t still = data[at], moving = data[at + 1];
        at += 2;
        if (still > blocks - i) return false;
        i += still;
        if (moving > blocks - i || moving > (size - at) / 2) return false;
        at += moving * 2;
        i += moving;
    }
    if (i != blocks) return false;
    grid.assign(blocks * 2, 0);
    i = at = 0;
    while (at < size) {
        const size_t still = data[at], moving = data[at + 1];
        at += 2;
        i += still;
        if (moving) std::memcpy(&grid[i * 2], data + at, moving * 2);
        at += moving * 2;
        i += moving;
    }
    return true;
}

inline void AppendMotionPacket(uint16_t gridWidth, uint16_t gridHeight, uint16_t blockSize,
                               const std::vector<float>& field, uint64_t endpointTimestampNs,
                               uint64_t intervalNs, std::vector<int8_t>& gridPrev, std::vector<uint8_t>& out) {
    ApplyMotionHysteresis(field, gridPrev);
    MotionHeader h{};
    h.gridWidth = gridWidth;
    h.gridHeight = gridHeight;
    h.blockSize = blockSize;
    h.previousTimestampNs = MotionPreviousTimestamp(endpointTimestampNs, intervalNs);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&h);
    out.insert(out.end(), bytes, bytes + sizeof(h));
    EncodeMotionRuns(gridPrev.data(), field.size() / 2, out);
}

inline bool PublishMotionField(MotionFieldMailbox& mailbox, bool motionRead, const MotionReadbackTiming& timing,
                               uint16_t gridWidth, uint16_t gridHeight, uint16_t blockSize,
                               const std::vector<float>& field, std::vector<int8_t>& gridPrev,
                               uint64_t enqueuedNs) {
    if (!motionRead || !timing.frameIndex || !timing.timestampNs || !timing.intervalNs ||
        !gridWidth || !gridHeight || !blockSize || timing.intervalNs > timing.timestampNs ||
        field.size() != size_t(gridWidth) * size_t(gridHeight) * 2)
        return false;
    std::vector<uint8_t> packet;
    AppendMotionPacket(gridWidth, gridHeight, blockSize, field, timing.timestampNs, timing.intervalNs, gridPrev, packet);
    mailbox.Put(timing.frameIndex, timing.timestampNs, std::move(packet), enqueuedNs);
    return true;
}

inline bool AttachMotionField(bool motionFieldEnabled, MotionFieldMailbox& mailbox, uint64_t frameIndex,
                              uint64_t headsetTargetNs, std::vector<uint8_t>& frameMeta,
                              bool* rejectedOversize = nullptr) {
    if (rejectedOversize) *rejectedOversize = false;
    if (!motionFieldEnabled) return false;
    constexpr size_t wireLimit = std::numeric_limits<uint16_t>::max();
    const size_t baseSize = frameMeta.size();
    if (baseSize > wireLimit) {
        if (rejectedOversize) *rejectedOversize = true;
        return false;
    }
    std::vector<uint8_t> motion;
    if (!mailbox.TakeIfFits(frameIndex, headsetTargetNs, wireLimit - baseSize, motion, rejectedOversize)) return false;
    frameMeta.insert(frameMeta.end(), std::make_move_iterator(motion.begin()), std::make_move_iterator(motion.end()));
    return true;
}

}  // namespace rc
