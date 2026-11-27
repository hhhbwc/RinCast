#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "rc/link_policy.h"
#include "rc/motion_field.h"
#include "rc/protocol.h"

namespace rc {

struct HalfRateState {
    bool active = false;
    bool skipNext = false;
};

inline HalfRateState StepHalfRate(HalfRateState state, double bitsPerPixel) {
    if (bitsPerPixel < 0.03) state.active = true;
    else if (bitsPerPixel > 0.05) state.active = false;
    state.skipNext = state.active && !state.skipNext;
    return state;
}

inline float ResolveVrUpscale(bool enabled, uint32_t recommendedWidth, uint32_t eyeWidth, bool shaderReady) {
    const float scale = std::min(2.f, float(recommendedWidth) / float(std::max(1u, eyeWidth)));
    return enabled && scale > 1.05f && shaderReady ? scale : 1.f;
}

inline double SmoothMotionInterval(double previousNs, int64_t currentNs) {
    if (currentNs <= 0 || double(currentNs) < double(kMinMotionIntervalNs) ||
        double(currentNs) > double(kMaxMotionIntervalNs)) return previousNs;
    const bool validPrevious = previousNs >= double(kMinMotionIntervalNs) && previousNs <= double(kMaxMotionIntervalNs);
    return validPrevious ? previousNs * 0.8 + double(currentNs) * 0.2 : double(currentNs);
}

enum class GenerationPath : uint8_t { None, Kept, External };

enum class GenerationRejectReason : uint8_t {
    None,
    InconsistentMotion,
    NoMotion,
    MissingFrame,
    MissingSwapchain,
    InvalidMotionTiming,
    InvalidField,
    AmountTooSmall,
    AmountAboveCap,
    TextureUnavailable,
    DrawFailed,
    Count
};

inline const char* GenerationRejectReasonName(GenerationRejectReason reason) {
    switch (reason) {
        case GenerationRejectReason::InconsistentMotion: return "inconsistent-motion";
        case GenerationRejectReason::NoMotion: return "no-motion";
        case GenerationRejectReason::MissingFrame: return "missing-frame";
        case GenerationRejectReason::MissingSwapchain: return "missing-swapchain";
        case GenerationRejectReason::InvalidMotionTiming: return "invalid-motion-timing";
        case GenerationRejectReason::InvalidField: return "invalid-field";
        case GenerationRejectReason::AmountTooSmall: return "amount-too-small";
        case GenerationRejectReason::AmountAboveCap: return "amount-above-cap";
        case GenerationRejectReason::TextureUnavailable: return "texture-unavailable";
        case GenerationRejectReason::DrawFailed: return "draw-failed";
        default: return "none";
    }
}

struct GenerationInput {
    bool enabled = false;
    bool vr = false;
    bool latched = false;
    bool hasFrame = false;
    bool swapchain = false;
    bool motionMoves = false;
    bool motionConsistent = false;
    int64_t motionFrameNs = 0;
    int64_t sinceFrameNs = 0;
    double displayNs = 0;
    double motionIntervalSmoothNs = 0;
    bool upscaled = false;
    bool externalReady = false;
    float fieldWidth = 0;
    float fieldHeight = 0;
    // Steady-cadence mode: keep the hard dependencies, but stop treating "the field isn't
    // worth it" as a veto.  Without this the gate skips most slow-motion frames, so a
    // 30 fps stream lands at ~38 Hz instead of a predictable 2x.
    bool force = false;
};

struct GenerationDecision {
    bool attempt = false;
    bool quotaBlocked = false;
    bool draw = false;
    bool inconsistent = false;
    GenerationRejectReason reason = GenerationRejectReason::None;
    GenerationPath path = GenerationPath::None;
    float amount = 0;
    float cap = 0;
    float scaleX = 0;
    float scaleY = 0;
};

inline int64_t ElapsedSinceLatchedFrameNs(int64_t displayNs, int64_t latchedDisplayNs, int64_t trackingTimestampNs) {
    return latchedDisplayNs > 0 ? displayNs - latchedDisplayNs : displayNs - trackingTimestampNs;
}

struct HeadsetMotionState {
    bool moves = false;
    bool consistent = false;
    int width = 0;
    int height = 0;
    int block = 0;
    int64_t frameNs = 0;
    double intervalSmoothNs = 0;
    std::vector<int8_t> grid;
};

inline constexpr uint8_t kMotionHeaderZeroWidth = 1 << 0;
inline constexpr uint8_t kMotionHeaderZeroHeight = 1 << 1;
inline constexpr uint8_t kMotionHeaderZeroBlock = 1 << 2;
inline constexpr uint8_t kMotionHeaderNonAdvancingTimestamp = 1 << 3;
inline constexpr uint8_t kMotionHeaderFailureMask = kMotionHeaderZeroWidth | kMotionHeaderZeroHeight |
                                                   kMotionHeaderZeroBlock | kMotionHeaderNonAdvancingTimestamp;

struct MotionDecodeResult {
    bool decoded = false;
    uint8_t failureStage = 0;  // 0: ok/disabled, 1: short-header, 2: invalid-header, 3: invalid-runs
    uint8_t headerFailure = 0;
    bool uploadMotion = false;
    MotionHeader header{};
    MotionGate gate{};
};

// Shared decoder + consistency evaluator for headset VR motion metadata packets.
// Clears stale validity and interval on every call so missing/invalid packets never leak prior state.
inline MotionDecodeResult DecodeVrMotionMetadata(bool enabled, bool foveated, uint64_t trackingTimestampNs,
                                                 const std::vector<uint8_t>& metadata, HeadsetMotionState& state) {
    state.moves = false;
    state.consistent = false;
    state.frameNs = 0;
    MotionDecodeResult res;
    if (!enabled) return res;
    const size_t at = sizeof(VrFrameMetadata) + (foveated ? sizeof(VrFoveationMetadata) : 0);
    if (metadata.size() < at + sizeof(MotionHeader)) {
        res.failureStage = 1;
        return res;
    }
    std::memcpy(&res.header, metadata.data() + at, sizeof(res.header));
    const size_t head = at + sizeof(res.header);
    if (!res.header.gridWidth) res.headerFailure |= kMotionHeaderZeroWidth;
    if (!res.header.gridHeight) res.headerFailure |= kMotionHeaderZeroHeight;
    if (!res.header.blockSize) res.headerFailure |= kMotionHeaderZeroBlock;
    if (trackingTimestampNs <= res.header.previousTimestampNs)
        res.headerFailure |= kMotionHeaderNonAdvancingTimestamp;
    if (res.headerFailure) {
        res.failureStage = 2;
        return res;
    }
    if (!DecodeMotionRuns(metadata.data() + head, metadata.size() - head,
                          size_t(res.header.gridWidth) * res.header.gridHeight, state.grid)) {
        res.failureStage = 3;
        return res;
    }
    res.decoded = true;
    state.width = res.header.gridWidth;
    state.height = res.header.gridHeight;
    state.block = res.header.blockSize;
    state.frameNs = int64_t(trackingTimestampNs - res.header.previousTimestampNs);
    state.intervalSmoothNs = SmoothMotionInterval(state.intervalSmoothNs, state.frameNs);
    res.gate = JudgeMotionField(state.grid, state.width, state.height);
    state.consistent = res.gate.worth;
    // A rejected field keeps whatever scattered blocks block matching produced; no gate cleaned
    // them. Filter before anything reaches the GPU, so forcing the cadence cannot upload noise.
    if (!state.consistent)
        FilterMotionFieldLocalConsensus(state.grid, state.width, state.height);
    else
        CleanMotionOutliers(state.grid, state.width, state.height);
    state.moves = std::any_of(state.grid.begin(), state.grid.end(), [](int8_t v) { return v != 0; });
    // Upload whenever the field still carries motion, even if the gate calls it inconsistent:
    // cadence-forced generation needs the real field on the GPU.  Skipping is the
    // decision's job (DecideFrameGeneration), not the upload's.
    res.uploadMotion = state.moves;
    return res;
}

// Forcing a generated frame is only worth its risk when the stream cannot fill the panel:
// a source interval of 1.2 display periods is 60 fps at 72 Hz. Above that the real frames
// already cover the slots, and a forced frame would only steal quota from a good stream.
inline bool ShouldForceFrameGeneration(double motionIntervalSmoothNs, int64_t motionFrameNs, double displayNs) {
    if (!(displayNs > 0)) return false;
    const double frameNs = motionFrameNs > 0 ? double(motionFrameNs) : 0.0;
    return std::max(motionIntervalSmoothNs, frameNs) >= displayNs * 1.2;
}

inline GenerationDecision DecideFrameGeneration(FrameGenerationQuota& quota, const GenerationInput& in) {
    GenerationDecision out;
    if (!in.enabled || !in.vr || in.latched) return out;
    const double displayNs = in.displayNs > 0 ? in.displayNs : 1e9 / 90.0;
    out.cap = float(std::clamp(in.motionIntervalSmoothNs > 0 ? in.motionIntervalSmoothNs / displayNs + 0.3 : 1.2, 1.0, 2.5));
    if (!quota.HasCredit()) {
        out.quotaBlocked = true;
        return out;
    }
    out.attempt = true;
    out.inconsistent = in.motionMoves && !in.motionConsistent;
    if (in.force) {
        // A rejected field is now only locally cleaned, not trusted: extrapolate at most one
        // source interval. A field the gate accepted gets 1.5. The old 2.5 stays for the
        // strict path, where a frame only draws when the gate already agreed with it.
        out.cap = std::min(out.cap, out.inconsistent ? 1.0f : 1.5f);
    }
    if (!in.force && out.inconsistent) out.reason = GenerationRejectReason::InconsistentMotion;
    else if (!in.motionMoves) out.reason = GenerationRejectReason::NoMotion;
    else if (!in.hasFrame) out.reason = GenerationRejectReason::MissingFrame;
    else if (!in.swapchain) out.reason = GenerationRejectReason::MissingSwapchain;
    else if (in.motionFrameNs <= 0) out.reason = GenerationRejectReason::InvalidMotionTiming;
    else if (!(in.fieldWidth > 0) || !(in.fieldHeight > 0)) out.reason = GenerationRejectReason::InvalidField;
    if (out.reason != GenerationRejectReason::None) return out;
    const float amount = float(double(in.sinceFrameNs) / double(in.motionFrameNs));
    out.amount = amount;
    if (!std::isfinite(amount) || amount <= 0.05f) {
        // Almost no time since the last picture: drawing it again would be a copy of it, so the
        // slot keeps the latched picture either way. Force does not change that.
        out.reason = GenerationRejectReason::AmountTooSmall;
    } else if (amount > out.cap) {
        // Overshoot: force holds at the cap instead of dropping the slot; the strict path still
        // rejects, so its 2.5 cap remains a ceiling nobody crosses.
        if (in.force) out.amount = out.cap;
        else out.reason = GenerationRejectReason::AmountAboveCap;
    }
    if (out.reason == GenerationRejectReason::None) {
        if (in.upscaled) out.path = GenerationPath::Kept;
        else if (in.externalReady) out.path = GenerationPath::External;
        else out.reason = GenerationRejectReason::TextureUnavailable;
    }
    if (out.reason != GenerationRejectReason::None) return out;
    // out.amount, not the raw ratio: the force path holds an overshoot at the cap, and the
    // shader must receive exactly what the log reports.  The strict path only gets here
    // with amount <= cap, so both are the same number there.
    quota.Spend();
    const float perPixel = out.amount * 0.85f * 127.f * kMotionUnitPixels;
    out.scaleX = perPixel / in.fieldWidth;
    out.scaleY = perPixel / in.fieldHeight;
    out.draw = true;
    return out;
}

inline bool NeedsTextureResize(int currentWidth, int currentHeight, int width, int height) {
    return width != currentWidth || height != currentHeight;
}

struct FrameGenerationStats {
    uint32_t source = 0;
    uint32_t generated = 0;
    uint32_t attempts = 0;
    uint32_t quotaBlocked = 0;
    std::array<uint32_t, size_t(GenerationRejectReason::Count)> rejected{};

    void Attempt() { ++attempts; }
    void Generated() { ++generated; }
    void Rejected(GenerationRejectReason reason) {
        if (reason != GenerationRejectReason::None) rejected[size_t(reason)]++;
    }
    void Record(const GenerationDecision& decision, bool drawSucceeded = true) {
        if (decision.quotaBlocked) ++quotaBlocked;
        if (decision.attempt) ++attempts;
        if (decision.draw && drawSucceeded) ++generated;
        else if (decision.attempt)
            Rejected(decision.draw && !drawSucceeded ? GenerationRejectReason::DrawFailed : decision.reason);
    }
    double GeneratedFps(double seconds) const { return seconds > 0 ? generated / seconds : 0; }
    double RejectedFps(double seconds) const { return seconds > 0 ? (attempts - generated) / seconds : 0; }
    void Reset() {
        source = generated = attempts = quotaBlocked = 0;
        rejected.fill(0);
    }
};

}  // namespace rc
