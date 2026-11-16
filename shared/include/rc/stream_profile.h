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
    // Quality keeps a mild floor (12): measured 2026-11-17 on a real device, no
    // floor at all swung the QP 3-25 and the user read it as strong "breathing" -
    // they want stable sharpness, not the smallest QP number. A floor that is too
    // high is equally bad: floor 20 pinned the QP at exactly 20.0 and left frames
    // at 150-168 KB when the per-frame budget was ~194 KB. Noisy synthetic clips
    // hide all of this (their QP is naturally above any floor), which is why the
    // offline A/B looked flat.
    uint32_t minQp = 20;
    // VBV size as a percentage of ONE frame at the ceiling (150 = 1.5 frames).
    // Small VBV = smaller bursts = less queueing; large VBV lets a keyframe breathe.
    // BUT one frame is too small for NVENC's VBR rate control to converge. Measured
    // 2026-11-15 (3488x1744 @90, synthetic noise): a 1-frame VBV sent 136 / 126 / 109 %
    // of a 20 / 30 / 40 Mbps ceiling and only 87 % of a 140 Mbps one, while 1.5 frames
    // brought every ceiling to 95-97 %. Never go below 150.
    uint32_t vbvFramesPercent = 150;
};

constexpr StreamProfileParams ProfileParams(StreamProfile p) {
    switch (p) {
        case StreamProfile::LowLatency:
            // 150, not 100: a one-frame VBV made capped VBR miss the ceiling in BOTH
            // directions (see the note on vbvFramesPercent). The 15 ms queue budget is
            // what makes this profile latency-first, not an undersized buffer.
            return {15.0, 6.0, 15.0, 100, 24, 150};
        case StreamProfile::Quality:
            // 2026-11-17 real device: with no floor this profile swung QP 3-25
            // (light load) / 9-29 (pressure map) and the user read it as strong
            // "breathing" - what they want is stable sharpness, not the lowest QP
            // number. 12 is a mild floor: it cuts the near-lossless spikes while
            // keeping the sharpness edge over Balanced (16). Initial guess from
            // the measured QP distribution, pending real-device re-measure.
            return {60.0, 12.0, 30.0, 100, 12, 200};
        case StreamProfile::Balanced:
        default:
            // Keep the guard's current, measured numbers: changing the default
            // brake line without a real-link test is how we got here before.
            // Latency-first (15) and quality-first (60) bracket it. The QP floor
            // went the way of Quality's (real device 2026-11-15: with the floor at
            // 20 the QP stopped exactly at 20.0 and the stream sat at 35 Mbps of a
            // 100 ceiling; floor off, the same content spent 100.8) - spending less
            // on simple frames is the latency profile's job now, and the queue
            // budget plus the guard stay responsible for the safety.
            return {45.0, 12.0, 25.0, 100, 16, 150};
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
