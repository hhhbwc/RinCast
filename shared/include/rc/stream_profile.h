#pragma once

// The "automatic bitrate" switch is a preference, not a number. The bitrate the
// user sets is a CEILING (how much bandwidth to allow); the profile decides how
// eagerly the encoder spends it and how much queueing delay is tolerable before
// the guard brakes. Simple pictures then cost little, complex ones can use the
// whole ceiling, and the user still chooses what to favour.
//
// Everything here is pure arithmetic: no GPU, no clock, no device, so the same
// numbers can be checked by the offline core tests and by encode-bench.

#include <algorithm>
#include <cstdint>

namespace rc {

enum class StreamProfile : uint8_t {
    LowLatency = 0,
    Balanced = 1,  // default: what the streamer used before profiles existed
    Quality = 2,
};

struct StreamProfileParams {
    // QueueGuard's latency budget. "How many ms of queue am I willing to pay for
    // a sharper picture" - the one number a user can actually name.
    double targetQueueMs = 45.0;
    double bandMs = 12.0;       // dead band around the target
    double floorExtraMs = 25.0; // headroom above the device's own idle queue
    // Capped VBR: keep the average target AT the ceiling so a busy scene can still
    // spend the whole budget. NVENC's averageBitRate is a long-term target, so
    // lowering it also starves complex scenes (measured: 60% average -> a noisy
    // moving scene fell from 142 to 92 KB/frame). What actually saves bits on a
    // still menu is a QP floor: rate control may not lower QP past it, so it stops
    // inflating an already-trivial frame to fill the budget.
    uint32_t vbrAveragePercent = 100;
    // QP floor, 0 = off (rate control may spend the whole ceiling on ANY picture).
    // Higher = fewer bits on easy content = lower latency. With the ceiling at
    // 110 Mbps, QP 20 took a still menu from 49 KB/frame to 2.3 KB/frame.
    // Quality uses 0: a picture the user paid 140 Mbps for should not be held back
    // by a floor. Measured on a real VRChat stream the floor left frames at
    // 150-168 KB when the per-frame budget was ~194 KB - the encoder wanted a
    // lower QP and was not allowed to. Noisy synthetic clips hide this (their QP
    // is naturally above any floor), which is why the offline A/B looked flat.
    uint32_t minQp = 20;
    // VBV size as a percentage of ONE frame at the ceiling (150 = 1.5 frames).
    // Small VBV = smaller bursts = less queueing; large VBV lets a keyframe breathe.
    uint32_t vbvFramesPercent = 150;
};

constexpr StreamProfileParams ProfileParams(StreamProfile p) {
    switch (p) {
        case StreamProfile::LowLatency:
            return {15.0, 6.0, 15.0, 100, 24, 100};
        case StreamProfile::Quality:
            // No QP floor at all. "Best picture" must actually be allowed to spend
            // the ceiling; the other two profiles keep their savings. Latency-first
            // and balanced still bracket it on the queue side (15 / 45 / 60 ms).
            return {60.0, 12.0, 30.0, 100, 0, 200};
        case StreamProfile::Balanced:
        default:
            // Keep the guard's current, measured numbers: changing the default
            // brake line without a real-link test is how we got here before.
            // Only the encoder's spending changes for existing users.
            // Latency-first (15) and quality-first (60) bracket it.
            return {45.0, 12.0, 25.0, 100, 20, 150};
    }
}

constexpr StreamProfile ProfileFromId(int id) {
    return id == int(StreamProfile::LowLatency)   ? StreamProfile::LowLatency
           : id == int(StreamProfile::Quality)    ? StreamProfile::Quality
                                                  : StreamProfile::Balanced;
}

// The three NVENC rate-control numbers for a ceiling. Kept separate from the
// encoder so the same arithmetic is used at Init, on SetBitrate and on every
// fps-driven VBV reconfigure - they must never disagree about the budget.
struct CappedVbrParams {
    uint32_t averageBitrate = 0;  // bits/second
    uint32_t maxBitrate = 0;      // bits/second (the hard ceiling)
    uint32_t vbvBufferSize = 0;   // bits
};

constexpr CappedVbrParams ComputeCappedVbr(uint32_t capKbps, uint32_t fps, uint32_t averagePercent,
                                           uint32_t vbvFramesPercent) {
    CappedVbrParams p{};
    if (!capKbps) return p;
    const uint64_t cap = uint64_t(capKbps) * 1000ull;
    const uint32_t avgPct = std::clamp<uint32_t>(averagePercent, 1u, 100u);
    const uint32_t vbvPct = std::clamp<uint32_t>(vbvFramesPercent, 25u, 400u);
    const uint32_t safeFps = std::max<uint32_t>(fps, 1u);
    p.maxBitrate = uint32_t(std::min<uint64_t>(cap, 0xFFFFFFFFull));
    p.averageBitrate = uint32_t(std::min<uint64_t>(cap * avgPct / 100u, 0xFFFFFFFFull));
    p.vbvBufferSize = uint32_t(std::min<uint64_t>(cap * vbvPct / 100u / safeFps, 0xFFFFFFFFull));
    return p;
}

constexpr CappedVbrParams ComputeCappedVbr(uint32_t capKbps, uint32_t fps, StreamProfile profile) {
    const StreamProfileParams q = ProfileParams(profile);
    return ComputeCappedVbr(capKbps, fps, q.vbrAveragePercent, q.vbvFramesPercent);
}

}  // namespace rc
