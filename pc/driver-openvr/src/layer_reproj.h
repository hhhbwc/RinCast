#pragma once

#include <cmath>
#include <cstdint>
#include <deque>
#include <vector>

#include "openvr/openvr_driver.h"
#include "vr_bridge.h"

namespace rcvr {

struct PoseMatchResult {
    bool matched = false;
    double score = 1e30;
    rcvr::TrackingSnapshot snapshot{};
};

inline PoseMatchResult MatchHistoryPose(
    const vr::HmdMatrix34_t& m,
    const std::deque<rcvr::TrackingSnapshot>& history,
    const rcvr::TrackingSnapshot& fallback = {}) {
    if (history.empty()) {
        return {false, 1e30, fallback};
    }
    const rcvr::TrackingSnapshot* best = nullptr;
    double bestScore = 1e30;
    for (const auto& t : history) {
        const double qx = t.hmd.orientation[0];
        const double qy = t.hmd.orientation[1];
        const double qz = t.hmd.orientation[2];
        const double qw = t.hmd.orientation[3];
        const double pos[3] = {t.hmd.position[0], t.hmd.position[1], t.hmd.position[2]};

        const double r[3][3] = {
            {1.0 - 2.0 * (qy * qy + qz * qz), 2.0 * (qx * qy - qz * qw), 2.0 * (qx * qz + qy * qw)},
            {2.0 * (qx * qy + qz * qw), 1.0 - 2.0 * (qx * qx + qz * qz), 2.0 * (qy * qz - qx * qw)},
            {2.0 * (qx * qz - qy * qw), 2.0 * (qy * qz + qx * qw), 1.0 - 2.0 * (qx * qx + qy * qy)}
        };

        double score = 0.0;
        for (int i = 0; i < 3; i++) {
            score += std::fabs(m.m[i][3] - pos[i]);
            for (int j = 0; j < 3; j++) {
                score += std::fabs(m.m[i][j] - r[i][j]);
            }
        }
        if (score <= bestScore) {
            bestScore = score;
            best = &t;
        }
    }
    const bool matched = (bestScore <= 0.01);
    return {matched, bestScore, best ? *best : fallback};
}

struct ValidLayerEntry {
    size_t originalIndex = 0;
    vr::HmdMatrix34_t pose{};
};

struct LayerSelectionResult {
    size_t targetIdx = 0;
    vr::HmdMatrix34_t targetPose{};
    rcvr::TrackingSnapshot targetTracking{};
};

inline LayerSelectionResult SelectCompositeTargetLayer(
    const std::vector<ValidLayerEntry>& validLayers,
    const std::deque<rcvr::TrackingSnapshot>& history,
    const rcvr::TrackingSnapshot& fallback = {}) {
    if (validLayers.empty()) {
        return {0, {}, fallback};
    }

    struct EvaluatedLayer {
        size_t originalIndex;
        vr::HmdMatrix34_t pose;
        PoseMatchResult match;
    };

    std::vector<EvaluatedLayer> evaluated;
    evaluated.reserve(validLayers.size());

    bool anyMatched = false;
    for (const auto& l : validLayers) {
        PoseMatchResult res = MatchHistoryPose(l.pose, history, fallback);
        if (res.matched) anyMatched = true;
        evaluated.push_back({l.originalIndex, l.pose, res});
    }

    if (anyMatched) {
        size_t bestIdx = 0;
        uint64_t bestSeq = 0;
        size_t bestOriginalIndex = 0;
        bool first = true;

        for (size_t i = 0; i < evaluated.size(); i++) {
            if (!evaluated[i].match.matched) continue;
            const uint64_t seq = evaluated[i].match.snapshot.sequence;
            const size_t origIdx = evaluated[i].originalIndex;
            if (first || seq > bestSeq || (seq == bestSeq && origIdx > bestOriginalIndex)) {
                bestIdx = i;
                bestSeq = seq;
                bestOriginalIndex = origIdx;
                first = false;
            }
        }
        return {evaluated[bestIdx].originalIndex, evaluated[bestIdx].pose, evaluated[bestIdx].match.snapshot};
    } else {
        const auto& last = evaluated.back();
        rcvr::TrackingSnapshot snap = last.match.snapshot;
        if (last.match.score > 0.05 && !history.empty()) {
            snap = history.back();
        }
        return {last.originalIndex, last.pose, snap};
    }
}

inline LayerSelectionResult SelectCompositeTargetLayer(
    const std::vector<vr::HmdMatrix34_t>& layerPoses,
    const std::deque<rcvr::TrackingSnapshot>& history,
    const rcvr::TrackingSnapshot& fallback = {}) {
    std::vector<ValidLayerEntry> entries;
    entries.reserve(layerPoses.size());
    for (size_t i = 0; i < layerPoses.size(); i++) {
        entries.push_back({i, layerPoses[i]});
    }
    return SelectCompositeTargetLayer(entries, history, fallback);
}

inline bool ComputeLayerReproj(
    const vr::HmdMatrix34_t& layerPose,
    const vr::HmdMatrix34_t& targetPose,
    float outRowMajor3x3[9]) {
    // M = R_layer^T * R_target
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            float sum = 0.0f;
            for (int k = 0; k < 3; k++) {
                sum += layerPose.m[k][r] * targetPose.m[k][c];
            }
            outRowMajor3x3[r * 3 + c] = sum;
        }
    }

    auto fillIdentity = [&]() {
        outRowMajor3x3[0] = 1.0f; outRowMajor3x3[1] = 0.0f; outRowMajor3x3[2] = 0.0f;
        outRowMajor3x3[3] = 0.0f; outRowMajor3x3[4] = 1.0f; outRowMajor3x3[5] = 0.0f;
        outRowMajor3x3[6] = 0.0f; outRowMajor3x3[7] = 0.0f; outRowMajor3x3[8] = 1.0f;
    };

    for (int i = 0; i < 9; i++) {
        if (!std::isfinite(outRowMajor3x3[i])) {
            fillIdentity();
            return false;
        }
    }

    const float det = outRowMajor3x3[0] * (outRowMajor3x3[4] * outRowMajor3x3[8] - outRowMajor3x3[5] * outRowMajor3x3[7])
                    - outRowMajor3x3[1] * (outRowMajor3x3[3] * outRowMajor3x3[8] - outRowMajor3x3[5] * outRowMajor3x3[6])
                    + outRowMajor3x3[2] * (outRowMajor3x3[3] * outRowMajor3x3[7] - outRowMajor3x3[4] * outRowMajor3x3[6]);

    if (std::fabs(det - 1.0f) > 0.05f) {
        fillIdentity();
        return false;
    }

    float maxDev = 0.0f;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            const float expected = (r == c) ? 1.0f : 0.0f;
            const float dev = std::fabs(outRowMajor3x3[r * 3 + c] - expected);
            if (dev > maxDev) maxDev = dev;
        }
    }

    if (maxDev < 1e-5f) {
        fillIdentity();
        return false;
    }

    return true;
}

inline bool ReprojectEyeUv(
    float u, float v,
    const float fovTan[4],
    const float m[9],
    float& outU, float& outV) {
    const float rx = -fovTan[0] + u * (fovTan[0] + fovTan[1]);
    const float ry =  fovTan[2] - v * (fovTan[2] + fovTan[3]);
    const float rz = -1.0f;
    const float lx = m[0] * rx + m[1] * ry + m[2] * rz;
    const float ly = m[3] * rx + m[4] * ry + m[5] * rz;
    const float lz = m[6] * rx + m[7] * ry + m[8] * rz;
    if (lz > -0.05f) return false;
    const float tx = lx / -lz;
    const float ty = ly / -lz;
    outU = (tx + fovTan[0]) / (fovTan[0] + fovTan[1]);
    outV = (fovTan[2] - ty) / (fovTan[2] + fovTan[3]);
    return outU >= 0.0f && outU <= 1.0f && outV >= 0.0f && outV <= 1.0f;
}

struct LayerConstants {
    float rect[4];     // uMin, vMin, uMax, vMax
    float fov[4];      // left, right, up, down
    float reproj0[4];  // row 0, w = enabled (0/1)
    float reproj1[4];  // row 1, w = isOverlay (0/1)
    float reproj2[4];  // row 2, w = unused
};
static_assert(sizeof(LayerConstants) == 80, "LayerConstants must be 80 bytes");

inline constexpr const char* kBlitShader = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);
cbuffer Layer : register(b0) {
    float4 rect;     // uMin, vMin, uMax, vMax
    float4 fov;      // left, right, up, down (positive tangents)
    float4 reproj0;  // row 0 of R_layer^T * R_target, w = reprojEnabled (0 or 1)
    float4 reproj1;  // row 1 of R_layer^T * R_target, w = isOverlay (0 or 1)
    float4 reproj2;  // row 2 of R_layer^T * R_target, w = unused
};
struct V { float4 pos : SV_Position; float2 eyeUv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    float2 c = float2((id << 1) & 2, id & 2);
    V o;
    o.pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1);
    o.eyeUv = c;
    return o;
}
float Lanczos2(float x) {
    x = abs(x);
    if (x < 1e-4) return 1.0;
    if (x >= 2.0) return 0.0;
    const float pi = 3.14159265;
    float px = pi * x;
    return (sin(px) / px) * (sin(px * 0.5) / (px * 0.5));
}
float3 LinearToSrgb(float3 c) {
    c = saturate(c);
    return lerp(c * 12.92, 1.055 * pow(max(c, 1e-6), 1.0 / 2.4) - 0.055, step(0.0031308, c));
}
float4 ps(V i) : SV_Target {
    float2 srcEyeUv = i.eyeUv;
    bool inBounds = true;
    if (reproj0.w > 0.5) {
        float3 ray = float3(-fov.x + i.eyeUv.x * (fov.x + fov.y),
                             fov.z - i.eyeUv.y * (fov.z + fov.w),
                            -1.0);
        float3 r = float3(dot(reproj0.xyz, ray), dot(reproj1.xyz, ray), dot(reproj2.xyz, ray));
        if (r.z > -0.05) {
            inBounds = false;
        } else {
            float tx = r.x / -r.z;
            float ty = r.y / -r.z;
            srcEyeUv = float2((tx + fov.x) / (fov.x + fov.y),
                              (fov.z - ty) / (fov.z + fov.w));
            if (any(srcEyeUv < 0.0) || any(srcEyeUv > 1.0)) {
                inBounds = false;
            }
        }
    }
    float2 uv = lerp(rect.xy, rect.zw, saturate(srcEyeUv));
    float w, h;
    tex.GetDimensions(w, h);
    const float2 covered = float2(abs(ddx(uv.x)) * w, abs(ddy(uv.y)) * h);
    if (!inBounds) {
        return float4(0, 0, 0, reproj1.w > 0.5 ? 0.0 : 1.0);
    }
    float4 res;
    if (covered.x <= 1.05 && covered.y <= 1.05) {
        res = tex.Sample(samp, uv);
    } else {
        const float2 stepUv = max(covered, 1.0) / (4.0 * float2(w, h));
        float4 sum = 0;
        float wSum = 0;
        float3 mn = float3(1e9, 1e9, 1e9);
        float3 mx = float3(-1e9, -1e9, -1e9);
        [unroll] for (int y = 0; y < 4; y++) {
            float dy = (float(y) - 1.5) * 0.5;
            float wy = Lanczos2(dy * 1.6);
            [unroll] for (int x = 0; x < 4; x++) {
                float dx = (float(x) - 1.5) * 0.5;
                float wt = Lanczos2(dx * 1.6) * wy;
                float4 s = tex.SampleLevel(samp, uv + (float2(x, y) - 1.5) * stepUv, 0);
                sum += s * wt;
                wSum += wt;
                if (x >= 1 && x <= 2 && y >= 1 && y <= 2) {
                    mn = min(mn, s.rgb);
                    mx = max(mx, s.rgb);
                }
            }
        }
        res = sum / max(wSum, 1e-5);
        float3 margin = (mx - mn) * 0.12;
        res.rgb = clamp(res.rgb, max(0.0, mn - margin), min(1.0, mx + margin));
    }
    if (reproj2.w > 0.5) res.rgb = LinearToSrgb(res.rgb);
    return res;
}
)";

}  // namespace rcvr
