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
#include <vector>

namespace rc {

inline constexpr int kMotionUnitPixels = 4;

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
        return int8_t(std::clamp<long>(std::lround(px / kMotionUnitPixels), -127, 127));
    };
    grid.resize(field.size());  // new entries start at 0: the first frame / a reset sees an all-still grid
    for (size_t i = 0; i < field.size(); i++) {
        const float px = field[i];
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

// Whole-picture translation needs majority support; moving-only means hide symmetric noise.
inline MotionGate JudgeMotionField(std::vector<int8_t>& grid, int w, int h) {
    MotionGate g;
    if (w < 1 || h < 1 || grid.size() < size_t(w) * size_t(h) * 2) return g;
    const auto mag = [](int v) { return v < 0 ? -v : v; };
    constexpr int kFarUnits = 12;
    for (size_t i = 0; i < grid.size(); i += 2) {
        if (mag(grid[i]) > kFarUnits || mag(grid[i + 1]) > kFarUnits) {
            g.farRejected++;
            grid[i] = grid[i + 1] = 0;
        }
    }
    const size_t blocks = size_t(w) * size_t(h);
    long sx = 0, sy = 0;
    for (size_t i = 0; i < grid.size(); i += 2) {
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
    for (size_t i = 0; i < grid.size(); i += 2) {
        histX[int(grid[i]) + kFarUnits]++;
        histY[int(grid[i + 1]) + kFarUnits]++;
    }
    const auto median = [blocks](const int* hist) {
        size_t count = 0;
        for (int i = 0; i <= 2 * kFarUnits; i++) {
            count += size_t(hist[i]);
            if (count * 2 >= blocks) return i - kFarUnits;
        }
        return 0;
    };
    const int gx = median(histX), gy = median(histY);
    g.global = mag(gx) + mag(gy);
    const int tolerance = 2 + g.global / 2;
    for (size_t i = 0; i < grid.size(); i += 2) {
        if (!grid[i] && !grid[i + 1]) continue;
        if (mag(int(grid[i]) - gx) + mag(int(grid[i + 1]) - gy) > tolerance)
            g.spread++;
        else
            g.coherent++;
    }
    const size_t required = std::max<size_t>(2, blocks * 2 / 5);
    g.worth = g.global > 0 && g.coherent >= required && g.spread <= g.moving / 4;
    return g;
}

#pragma pack(push, 1)
struct MotionHeader {
    uint16_t gridWidth;
    uint16_t gridHeight;
    uint16_t blockSize;           // pixels of the side-by-side frame per grid cell
    uint16_t reserved;
    uint64_t previousTimestampNs;  // VrFrameMetadata::trackingTimestampNs of the frame compared with
};
#pragma pack(pop)

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
    grid.assign(blocks * 2, 0);
    size_t i = 0, at = 0;
    while (at < size) {
        if (size - at < 2) return false;
        const size_t still = data[at], moving = data[at + 1];
        at += 2;
        i += still;
        if (i + moving > blocks || size - at < moving * 2) return false;
        if (moving) std::memcpy(&grid[i * 2], data + at, moving * 2);
        at += moving * 2;
        i += moving;
    }
    return i == blocks;
}

}  // namespace rc
