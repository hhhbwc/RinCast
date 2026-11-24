// 编码器帧率阶梯、每帧预算及重配置门控。观测/请求与成功提交的状态分开保存。
// 纯计算，由编码线程传入时间；测试无需 GPU、设备或实际等待。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "rc/stream_profile.h"

namespace rc {

// 90 -> 120 for 120 Hz panels; 40/48/80 so the fill floor can land on a rung that
// keeps ratio = vbvPct/100 * rung / nominal >= 1 at every supported refresh tier.
constexpr int kFpsLadder[] = {24, 30, 40, 45, 48, 54, 60, 72, 80, 90, 120};
constexpr int kLadderLast = int(sizeof(kFpsLadder) / sizeof(kFpsLadder[0])) - 1;

// Picks the reconfigure target for a measured frame rate:
//   - below 21 fps is treated as a stall (a load screen, a hang): the budget must
//     not inflate to the ladder's bottom rung because of one bad second;
//   - within 15% of the current setting: keep the current (no reconfigure);
//   - otherwise the largest ladder step at or under the measured rate. Down is
//     deliberate: the per-frame budget is ceiling / frameRateNum, so a rung ABOVE
//     the real rate understates every frame's budget and the stream undershoots,
//     while a rung below it overshoots into the queue. The 15% window bounds how
//     far the truth can drift before the ladder follows it.
constexpr int QuantizeFps(double measured, int current) {
    if (!(measured >= 0.0 && measured <= 1e9)) return current > 0 ? current : 24;
    if (measured < 21.0) return 24;
    int rung = kFpsLadder[0];
    for (int step : kFpsLadder)
        if (step <= measured + 1e-6) rung = step;
    // S1b: a panel measures a couple of frames short of its top rate (117-119 on a
    // 120 Hz panel). Without this the ladder tops out one rung lower after a dip and
    // cannot climb back (|118-90| > 90*15%), leaving the fill rung at 90 and the wire
    // at 118/90 = 1.31x the ceiling. 2% is inside panel clock tolerance.
    if (measured >= double(kFpsLadder[kLadderLast]) * 0.98) rung = kFpsLadder[kLadderLast];
    if (current > 0 && std::fabs(measured - double(current)) <= double(current) * 0.15) return current;
    return rung;
}

// D-030：只向下修正，不能用比例下限或死区保留超额的每帧预算。
inline uint32_t ScaleRateToRealFps(uint32_t kbps, double rcFps, double realFps) {
    if (!kbps || !std::isfinite(rcFps) || !std::isfinite(realFps) || rcFps <= 0 || realFps < 1.0)
        return kbps;
    return uint32_t(double(kbps) * std::min(1.0, rcFps / realFps));
}

constexpr int64_t kRateStartupNs = 3'000'000'000LL;
constexpr int64_t kRateRetryNs = 1'000'000'000LL;
constexpr int64_t kRateGrowNs = 3'000'000'000LL;
constexpr double kCadenceMaxScale = 1.5;
constexpr double kCadenceVbvHeadroom = 0.95;

// L17 fill mode floor: the smallest ladder rung whose per-frame budget still fits the
// VBV bucket. ratio = (vbvPct/100) * rung / nominal >= 1  <=>  rung >= nominal*100/vbvPct.
// Derived, never hard-coded: 90 Hz + 200% -> 45 (ratio 1.00), 72 Hz + 150% -> 48 (1.00),
// 120 Hz + 200% -> 60 (1.00), 72 Hz + 200% -> 40 (1.11). A rung below this overflows the
// bucket (every frame larger than the bucket) - the D-034 QP-breathing root.
constexpr int FillFloorRung(int nominal, int vbvPct) {
    if (nominal <= 0 || vbvPct <= 0) return kFpsLadder[0];
    const double need = double(nominal) * 100.0 / double(vbvPct);
    for (int step : kFpsLadder)
        if (double(step) + 1e-9 >= need) return step;
    return kFpsLadder[kLadderLast];
}
constexpr bool LadderContainsStep(int step) {
    for (int s : kFpsLadder)
        if (s == step) return true;
    return false;
}
static_assert(LadderContainsStep(FillFloorRung(90, 200)), "the fill floor must be a ladder rung");
static_assert(FillFloorRung(90, 200) == 45, "90 Hz + 200% VBV must stay at 45 (the shipped default)");
static_assert(FillFloorRung(72, 150) == 48, "72 Hz + 150% VBV (CBR) must be 48");
static_assert(FillFloorRung(120, 200) == 60, "120 Hz + 200% VBV must be 60");

// Encoder-side hysteresis for the fill branch. Pressure must hold kHoldNs
// continuously before the branch turns on; once armed it stays armed for the
// session (only an encoder re-init or a settings change resets it). The rung
// itself still follows the slow EMA every call, clamped to [FillFloorRung(nominal, vbvPct), nominal]:
// recovery therefore raises the rung and shrinks the per-frame budget again. The
// cooldown is stamped once at arm time (Update's m_cooldownUntilNs = now +
// kCooldownNs) and never throttles anything again: while armed the m_active
// short-circuit returns before the cooldown is ever read, and Reset() clears
// m_cooldownUntilNs together with the rest, so a re-arm after a reset walks a
// fresh hold window with NO cooldown guard at all (A8 comment fix: the old
// "cooldown only guards a re-arm after a reset" wording contradicted Reset();
// behaviour pinned by the F4 case in core_tests.cpp TestFillModeGateEdges -
// armed at 6S, Reset() at 20S inside the old 36S window, re-armed at 25S).
// Pure state, so the core tests exercise it without the driver.
struct FillModeGate {
    static constexpr int64_t kHoldNs = 5'000'000'000LL;
    // re-arm guard only (armed state is sticky, so this does not throttle rung changes)
    static constexpr int64_t kCooldownNs = 30'000'000'000LL;

    // pressure: real fps below nominal*0.85, non-desktop, switch on, past startup.
    bool Update(bool pressure, int64_t now) {
        if (m_active) return true;  // armed for the session; the rung keeps following the slow EMA (see above)
        if (m_cooldownUntilNs && now < m_cooldownUntilNs) return false;
        if (!pressure) {
            m_holdSinceNs = 0;
            return false;
        }
        if (!m_holdSinceNs) m_holdSinceNs = now;
        if (now - m_holdSinceNs < kHoldNs) return false;
        m_active = true;
        m_cooldownUntilNs = now + kCooldownNs;
        m_holdSinceNs = 0;
        return true;
    }

    bool Active() const { return m_active; }
    bool InCooldown(int64_t now) const { return m_cooldownUntilNs && now < m_cooldownUntilNs; }
    void Reset() { m_active = false; m_holdSinceNs = 0; m_cooldownUntilNs = 0; }

   private:
    bool m_active = false;
    int64_t m_holdSinceNs = 0;
    int64_t m_cooldownUntilNs = 0;
};

// 重配置时候选桶可能小于已提交的 initial delay（降码率、升档都会缩小桶），
// 而 initial delay 必须落在桶内。夹紧而不是重置，避免重新灌满桶造成突发。
inline uint32_t ClampVbvInitialDelay(uint32_t initialDelay, uint32_t vbvBufferSize) {
    return vbvBufferSize ? std::min(initialDelay, vbvBufferSize) : initialDelay;
}

// 编码线程持有；快观测在加速时立即跟随，减速则平滑，单次长停顿不参与估计。
struct EncoderRateTracker {
    double fastIntervalNs = 0;
    double slowIntervalNs = 0;

    void Observe(double intervalNs) {
        if (!std::isfinite(intervalNs) || intervalNs < 1.0 || intervalNs > 1e12) return;
        if (fastIntervalNs > 0 && intervalNs > std::max(250'000'000.0, fastIntervalNs * 4.0)) return;
        fastIntervalNs = fastIntervalNs <= 0 || intervalNs < fastIntervalNs
                             ? intervalNs : fastIntervalNs * 0.4 + intervalNs * 0.6;
        slowIntervalNs = slowIntervalNs > 0 ? slowIntervalNs * 0.9 + intervalNs * 0.1 : intervalNs;
    }

    double RealFps(double nominal) const {
        return fastIntervalNs > 0 ? 1e9 / fastIntervalNs : nominal;
    }
};

struct EncoderRatePlan {
    uint32_t requestedKbps = 0;
    uint32_t rateNum = 0;  // 分母固定为 1000，保留标称小数帧率。
    CappedVbrParams budget{};
};

inline bool SameRatePlan(const EncoderRatePlan& a, const EncoderRatePlan& b) {
    return a.requestedKbps == b.requestedKbps && a.rateNum == b.rateNum &&
           a.budget.averageBitrate == b.budget.averageBitrate && a.budget.maxBitrate == b.budget.maxBitrate &&
           a.budget.vbvBufferSize == b.budget.vbvBufferSize;
}

inline EncoderRatePlan PlanEncoderRate(uint32_t kbps, double nominal, bool desktop, bool startup,
                                      const EncoderRateTracker& tracker, uint32_t currentRateNum,
                                      uint32_t averagePercent, uint32_t vbvFramesPercent,
                                      bool fillMode = false, bool cadenceFill = false) {
    // 帧率补偿已冻结（D-034）：不再让 frameRateNum 跟随实时 fps 降档。降档会把每帧预算放大
    // real/rung 倍（真机 38fps@30档 = 390KB/帧、124Mbps、190-215ms 的根因），再和标称 VBV 互相
    // 打架造成 QP「呼吸」。冻结在标称后：每帧预算 = kbps/nominal（恒定小帧），VBV = 标称线上
    // 时间上限，预算恒小于桶 → 既不爆队列也不泵。低帧率时码率自然降低，这是延迟优先的物理取舍。
    nominal = std::isfinite(nominal) && nominal >= 1.0 ? std::min(nominal, 1000.0) : 72.0;  // ROUND18 L4 fallback
    uint32_t rateNum = uint32_t(nominal * 1000.0);
    // L17 opt-in fill branch (default off = byte-identical to the frozen path): only
    // the frame-rate rung follows the slow fps EMA, floored at FillFloorRung(nominal, vbvPct) and never
    // above nominal. `scaled` and the VBV formula below stay on the frozen D-034
    // deep bucket - that is what keeps per-frame budget <= VBV at this rung.
    if (fillMode && !cadenceFill && !desktop && !startup) {
        const double slow = tracker.slowIntervalNs > 0 ? 1e9 / tracker.slowIntervalNs : nominal;
        int rung = QuantizeFps(slow, int(currentRateNum / 1000));
        const int lo = std::min(FillFloorRung(int(nominal + 0.5), int(vbvFramesPercent)), int(nominal));
        rung = std::clamp(rung, lo, int(nominal));
        rateNum = uint32_t(rung) * 1000u;
    }
    const uint32_t scaled = kbps;
    auto budget = ComputeCappedVbr(scaled, 1, averagePercent, vbvFramesPercent);
    // 桶按标称帧率算，与档位无关：固定的线上时间上限（1.5 帧 = 16.7ms，2 帧 = 22ms），
    // 且恒 ≥ 每帧预算（kbps/nominal），驱动不会被桶夹住而振荡。
    const uint32_t vbvPct = std::clamp(vbvFramesPercent, 25u, 400u);
    const uint32_t vbv = uint32_t(std::min(double(kbps) * 1000.0 * vbvPct / 100.0 / nominal, double(UINT32_MAX)));
    budget.vbvBufferSize = vbv;
    if (cadenceFill && !desktop && !startup && tracker.slowIntervalNs > 0 && vbv > 0) {
        const double slowFps = 1e9 / tracker.slowIntervalNs;
        if (std::isfinite(slowFps) && slowFps > 0 && slowFps < nominal * 0.85) {
            // Preserve 5% bucket headroom; the nominal denominator is never lowered.
            const double scale = std::min({kCadenceMaxScale, nominal / slowFps,
                                           double(vbvPct) / 100.0 * kCadenceVbvHeadroom});
            if (scale > 1.0) {
                const auto scaledRate = [scale](uint32_t value) {
                    return uint32_t(std::min(double(value) * scale, double(UINT32_MAX)));
                };
                budget.maxBitrate = scaledRate(budget.maxBitrate);
                budget.averageBitrate = std::min(budget.maxBitrate, scaledRate(budget.averageBitrate));
            }
        }
    }
    return {kbps, rateNum, budget};
}

// applied 只在 API 成功后改变；attempt 在失败时也记账，避免逐帧失败重试。
struct EncoderRateState {
    EncoderRatePlan applied{};
    int64_t lastAttemptNs = 0;
    int64_t growSinceNs = 0;
    bool failed = false;

    bool ShouldAttempt(const EncoderRatePlan& next, int64_t now) {
        if (SameRatePlan(applied, next)) {
            growSinceNs = 0;
            return false;
        }
        const double oldFrame = double(applied.budget.maxBitrate) / std::max(1u, applied.rateNum);
        const double newFrame = double(next.budget.maxBitrate) / std::max(1u, next.rateNum);
        const bool requested = next.requestedKbps != applied.requestedKbps;
        const bool grows = newFrame > oldFrame || next.budget.vbvBufferSize > applied.budget.vbvBufferSize;
        if (!requested && grows && newFrame >= oldFrame) {
            // 恢复至少变化 2% 且持续需要增加三秒；缩小每帧预算优先，不套这个死区。
            if (newFrame <= oldFrame * 1.02 &&
                next.budget.vbvBufferSize <= double(applied.budget.vbvBufferSize) * 1.02) {
                growSinceNs = 0;
                return false;
            }
            if (!growSinceNs) growSinceNs = now;
            if (now - growSinceNs < kRateGrowNs) return false;
        } else {
            growSinceNs = 0;
        }
        // 所有成功/失败尝试共用节流，频繁的外层请求也不能绕过它。
        return !lastAttemptNs || now - lastAttemptNs >= kRateRetryNs;
    }
};

template <typename Submit>
bool ApplyEncoderRateTransaction(EncoderRateState& state, const EncoderRatePlan& candidate,
                                 int64_t now, Submit submit) {
    state.lastAttemptNs = now;
    state.failed = !submit();
    if (state.failed) return false;
    state.applied = candidate;
    state.growSinceNs = 0;
    return true;
}

// QP telemetry validity. NVENC reports frameAvgQP as uint32_t. A real quantiser
// lives in a small codec-specific domain; a garbage value (adjacent-field leak,
// sign extension, uninitialised memory) is typically hundreds of millions. We
// do NOT guess why a value is wrong or clamp it into a plausible QP - we reject
// samples outside the codec's domain and keep the first raw value for diagnosis.
// This is display/telemetry only; it never feeds rate control.
//
// Domains are deliberately per-codec, not one shared ceiling: H.264/HEVC use
// 0..51, AV1 uses 0..255.
constexpr uint32_t kMaxValidQpH264Hevc = 51;
constexpr uint32_t kMaxValidQpAv1 = 255;

constexpr uint32_t MaxValidQp(int codec) {
    // rc::VideoCodec: H264=0, HEVC=1, AV1=2.
    return codec == 2 ? kMaxValidQpAv1 : kMaxValidQpH264Hevc;
}

struct QpStats {
    uint64_t sum = 0;           // sum of VALID samples only
    uint32_t valid = 0;
    uint32_t rejected = 0;
    uint32_t firstBadRaw = 0;   // first rejected raw value, kept for diagnosis
    // Min/max of VALID samples: the mean alone hides the swings that look like
    // "breathing". 0 when there is no usable sample (see Mean() for unknown).
    uint32_t min = 0, max = 0;

    void Add(uint32_t raw, uint32_t maxValid) {
        if (raw <= maxValid) {
            sum += raw;
            if (valid == 0 || raw < min) min = raw;
            if (valid == 0 || raw > max) max = raw;
            ++valid;
        } else {
            ++rejected;
            if (rejected == 1) firstBadRaw = raw;
        }
    }
    // -1 = unknown when there is no usable sample (matches VideoSender's qp=-1).
    float Mean() const { return valid ? float(double(sum) / valid) : -1.0f; }
    void Reset() { sum = 0; valid = 0; rejected = 0; firstBadRaw = 0; min = 0; max = 0; }
};

}  // namespace rc
