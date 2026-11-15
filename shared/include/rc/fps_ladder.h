// 编码器帧率阶梯、每帧预算及重配置门控。观测/请求与成功提交的状态分开保存。
// 纯计算，由编码线程传入时间；测试无需 GPU、设备或实际等待。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "rc/stream_profile.h"

namespace rc {

constexpr int kFpsLadder[] = {24, 30, 45, 54, 60, 72, 90};

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
                                      uint32_t averagePercent, uint32_t vbvFramesPercent) {
    // 防止非有限值转整数；不改用户配置，非法标称值使用保守的 90fps 预算。
    nominal = std::isfinite(nominal) && nominal >= 1.0 ? std::min(nominal, 1000.0) : 90.0;
    const uint32_t nominalNum = uint32_t(nominal * 1000.0);
    double real = nominal;
    uint32_t rateNum = nominalNum;
    if (!desktop) {
        real = tracker.RealFps(nominal);
        // 追赶/补帧可能让两次提交间隔远小于标称周期（1e9/real 变成上千 fps），
        // 那会把每帧预算压到十分之一以下。超过标称 1.25 倍的估计不采信。
        real = std::min(real, nominal * 1.25);
        if (!startup && tracker.slowIntervalNs > 0) {
            const double slow = 1e9 / tracker.slowIntervalNs;
            int rung = QuantizeFps(slow, int(currentRateNum / 1000));
            // 慢 EMA/保持窗口不能把 38fps 锁在 45fps 而欠用；不用抬高 kbps 补偿。
            rung = std::min(rung, QuantizeFps(real, 0));
            rateNum = std::min(nominalNum, uint32_t(rung) * 1000u);
        }
    }
    const double rateFps = double(rateNum) / 1000.0;
    const uint32_t scaled = desktop ? kbps : ScaleRateToRealFps(kbps, rateFps, real);
    auto budget = ComputeCappedVbr(scaled, 1, averagePercent, vbvFramesPercent);
    // 桶按标称帧率算，不按当前档位：档位掉到 30fps 时按「30fps 的 2 帧」算会给出 66ms 的桶，
    // 实测单帧涨到 364KB、submit->sent p99 41ms（90fps 档是 13ms），这正是排队和尖峰的来源。
    // 固定成标称帧周期后，桶就是一个与档位无关的线上时间上限（1.5 帧 = 16.7ms，2 帧 = 22ms）。
    const uint32_t vbvPct = std::clamp(vbvFramesPercent, 25u, 400u);
    const double vbv = double(scaled) * 1000.0 * vbvPct / 100.0 / nominal;
    budget.vbvBufferSize = uint32_t(std::min(vbv, double(UINT32_MAX)));
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

}  // namespace rc
