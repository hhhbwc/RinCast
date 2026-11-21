// Where a frame's latency goes, stage by stage, in the order a frame passes through them.
// PC times are moved onto the headset's clock with the offset the pings measure.
#pragma once

#include <algorithm>
#include <cstdint>

namespace rv {

enum LatencyStage {
    kStageUplink,       // VR: head pose to the PC (half the round trip)
    kStageGame,         // VR: the game renders it and hands it to SteamVR
    kStageEncoderWait,  // VR: waiting for the encoder
    kStageEncode,       // VR: encoding
    kStageCapture,      // desktop: capture and encoding
    kStageDownlink,     // the frame to the headset
    kStageDecode,       // decoding
    kStageNextFrame,    // decoded, waiting for the headset's next frame to pick it up
    kStageCompositor,   // picked up: the headset's compositor until it's lit
    kStageCount
};

struct LatencyStageInfo {
    const char* key;    // in the stats sent to the PC
    const char* label;  // in the headset's performance overlay
};

inline constexpr LatencyStageInfo kLatencyStages[kStageCount] = {
    {"uplink", "Head movement to the PC"},
    {"game", "Game and SteamVR"},
    {"encoderWait", "Waiting for the encoder"},
    {"encode", "Encoding"},
    {"capture", "Capture and encoding"},
    {"downlink", "Video to the headset"},
    {"decode", "Decoding"},
    {"nextFrame", "Waiting for the next frame"},
    {"compositor", "Compositor and display"},
};

// TASK-LATENCY-METER-FIX-2026-11-22 §1.2: one frame's stage samples join the running
// sums. Every stage counts every frame, so on the same frames the stages add up to
// the total latency instead of falling short of it: a stage pushed below zero by a
// slightly wrong clock offset counts as zero (a frame is kept, only a small stage
// skews a little), and an extreme stage counts as a full second rather than
// silently disappearing. Header-only and dependency-free so the core tests can
// call it directly (same pattern as the settings helper they already include).
inline void AddStageSamples(double (&sums)[kStageCount], uint32_t (&frames)[kStageCount],
                            const double (&ms)[kStageCount]) {
    for (int i = 0; i < kStageCount; i++) {
        sums[i] += std::clamp(ms[i], 0.0, 1000.0);
        frames[i]++;
    }
}

}  // namespace rv
