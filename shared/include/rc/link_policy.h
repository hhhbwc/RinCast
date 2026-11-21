#pragma once

// VD-OFFLINE (TASK 2026-11-20): the "full rate, or half rate + frame generation"
// policy, as a PURE function - state is held by the caller, time comes in as
// nowNs, no clock calls, no I/O, no globals. NOT wired to any behaviour yet:
// the thresholds need a real device before they may steer anything, so this
// file only feeds a log line (headset 5 s stats block) and the unit tests.
//
// The shape is inspired by VD's headset-side spacewarp policy (a smoothed game
// time judged against a window around the target rate, double-threshold
// hysteresis); everything here is re-derived for our own signals, no VD code.
//
// Documented deviations and limits (the task asks for these explicitly):
//  1. VD judges per frame from the game's own GameTime metadata; we only have
//     the 1 Hz gameFps average from kServerStats, so the ±0.5/1.5 ms window
//     semantics stretch with that much coarser time scale. Known limitation
//     until a real device tells us where the thresholds actually belong.
//  2. The 2 s anti-flap guard (rule 8) is RinCast's own; VD has none.
//  3. Rule 6 as written in the task ("gameFrameMs < targetMs - 1.5") is
//     unreachable by construction: the smoothing (rule 1) keeps
//     smoothedGameFps <= gameFps <= targetFps, so gameFrameMs >= targetMs
//     forever and the game can never look FASTER than the target. The task's
//     own test cases 4/5 (back to Full once the game sustains the target)
//     require the exit to fire at gameFrameMs < targetMs + 1.5, which is what
//     this implementation does; the hysteresis then lives in the entry/exit
//     shortfall gates (entry needs shortfall > 6, exit needs shortfall < 4,
//     mutually exclusive). Flagged for adjudication - not silently invented.
//  4. aer is reserved: the headset has no AER source today (grep confirmed),
//     callers pass false. The signature keeps it so a future source can fill
//     it in without changing either side.
//
// SEMANTICS REVISION (TASK-POLICY-FIX-TESTS-2026-11-20): the 07:56:08 device
// log caught this policy judging a healthy game (70 fps) as "game-slow". Two
// root causes: the entry condition was fed by the LINK rate (shortfall over
// recvFps - link drops read as a slow game), and the 0.25/s EMA lagged 6-7 s
// behind a recovering game, keeping the smoothed frame-window gate open so link
// signals decided everything. The smoothed frame-window gate (the window
// semantics in item 3 above) is RETIRED: entry/exit now count consecutive 1 Hz
// ticks of the RAW gameFps against kEnterGapFps/kExitGapFps, switching after
// kEnterTicks/kExitTicks. The EMA stays, feeding the log line only. recvFps no
// longer takes part in any switch decision: it names the reason (LinkSlow) and
// feeds slowSamples for that same naming. The deviation records above are kept
// as history of the pre-revision behaviour.

#include <cstdint>

#include "rc/fps_ladder.h"  // kFpsLadder (SuggestStartFps lands on a rung)

namespace rc {

// 待真机 A/B 校准，单位 fps（TASK-POLICY-FIX-TESTS-2026-11-20 §1.1）。
static constexpr float   kEnterGapFps = 12.f;  // 进入：gameFps < target - 12（72Hz 下 < 60）
static constexpr float   kExitGapFps  = 4.f;   // 退出/清零：gameFps >= target - 4（72Hz 下 >= 68）
static constexpr int     kEnterTicks  = 3;     // 连续 3 拍低于进入线才进入
static constexpr int     kExitTicks   = 3;     // 连续 3 拍达到退出线才退出

// 头显端的"该全速还是半速补帧"判定。纯函数：状态由调用方持有，时间由 nowNs 传入。
enum class PaceMode : uint8_t { Full = 0, HalfWithGeneration = 1 };

// 给日志用，不影响行为。
enum class PaceReason : uint8_t { Ok, GameSlow, LinkSlow, Recovered, AppRefusesGeneration, Dwelling };

struct LinkPolicyInput {
    float targetFps = 0;   // 头显当前显示刷新率
    float gameFps = 0;     // PC 实测游戏出帧（来自 kServerStats 的 vr.gameFps），<=0 表示未知
    float recvFps = 0;     // 头显实测每秒收到的视频帧数
    bool  aer = false;     // 预留：当前头显侧无 AER 来源，调用方传 false
};

struct LinkPolicyState {
    float smoothedGameFps = 0;
    int   slowSamples = 0;
    int   slowGameTicks = 0;   // 连续低于进入线的拍数（raw gameFps，进入 Half 的条件）
    int   recoveredTicks = 0;  // 连续达到退出线的拍数（raw gameFps，退回 Full 的条件）
    PaceMode mode = PaceMode::Full;
    int64_t lastSwitchNs = 0;       // 0 = 从未切换过（防抖门不生效）
    PaceReason lastReason = PaceReason::Ok;  // 供日志：本拍判定理由（任务卡枚举的用途）
};

inline const char* PaceModeName(PaceMode m) {
    return m == PaceMode::HalfWithGeneration ? "half+generate" : "full";
}

inline const char* PaceReasonName(PaceReason r) {
    switch (r) {
        case PaceReason::GameSlow: return "game-slow";
        case PaceReason::LinkSlow: return "link-slow";
        case PaceReason::Recovered: return "recovered";
        case PaceReason::AppRefusesGeneration: return "app-refuses-generation";
        case PaceReason::Dwelling: return "dwelling";
        case PaceReason::Ok: break;
    }
    return "ok";
}

inline PaceMode UpdateLinkPolicy(LinkPolicyState& s, const LinkPolicyInput& in, int64_t nowNs) {
    // Rule 1: smooth the game rate (only real samples update it). Kept for the
    // log line ONLY - no decision reads it any more (see the revision note).
    if (in.gameFps > 0)
        s.smoothedGameFps = s.smoothedGameFps <= 0 ? in.gameFps : s.smoothedGameFps + 0.25f * (in.gameFps - s.smoothedGameFps);
    // Rule 9: not enough signal - never act (also the NaN guard). An unknown
    // tick advances and clears nothing: both counters below are kept.
    if (in.targetFps <= 0 || s.smoothedGameFps <= 0) {
        s.lastReason = PaceReason::Ok;
        return PaceMode::Full;  // a guard, not a decision: s.mode is kept for when signals return
    }
    const float shortfall = in.targetFps - in.recvFps;  // reason naming only (see below)
    // Rule 4: sustained "half the target" receiving pressure. recvFps takes
    // part in NO switch decision - this and the reason naming are its only uses.
    if (in.recvFps < in.targetFps * 0.5f) ++s.slowSamples;
    else s.slowSamples = 0;
    // The raw-rate counters (revision): one if/else chain, mutually exclusive
    // per tick. At/above the exit line the recovered count builds and the slow
    // count clears; below the enter line the other way round; the 60-67 middle
    // band and an unknown (<= 0) game rate HOLD both counters.
    if (in.gameFps >= in.targetFps - kExitGapFps) {
        ++s.recoveredTicks;
        s.slowGameTicks = 0;
    } else if (in.gameFps > 0 && in.gameFps < in.targetFps - kEnterGapFps) {
        ++s.slowGameTicks;
        s.recoveredTicks = 0;
    }
    // Rule 7 (AER) first: the app cannot tolerate generation, full rate now.
    // This wins over the anti-flap guard too - it is a safety override.
    if (in.aer) {
        if (s.mode != PaceMode::Full) {
            s.mode = PaceMode::Full;
            s.lastSwitchNs = nowNs;
        }
        s.lastReason = PaceReason::AppRefusesGeneration;
        return PaceMode::Full;
    }
    // Into Half: the GAME ran below the enter line for kEnterTicks consecutive
    // ticks (raw rate - no smoothing lag, no link signal). Anti-flap: a switch
    // due within 2 s of the last real one dwells, counters kept, not cleared.
    if (s.mode == PaceMode::Full && s.slowGameTicks >= kEnterTicks) {
        if (s.lastSwitchNs && nowNs - s.lastSwitchNs < 2'000'000'000LL) {  // rule 8, RinCast's own
            s.lastReason = PaceReason::Dwelling;
            return s.mode;
        }
        s.mode = PaceMode::HalfWithGeneration;
        s.lastSwitchNs = nowNs;
        s.lastReason = PaceReason::GameSlow;
        return s.mode;
    }
    // Back to Full: the game held the exit line for kExitTicks consecutive
    // ticks. Same anti-flap: dwell, keep the counters.
    if (s.mode == PaceMode::HalfWithGeneration && s.recoveredTicks >= kExitTicks) {
        if (s.lastSwitchNs && nowNs - s.lastSwitchNs < 2'000'000'000LL) {  // rule 8
            s.lastReason = PaceReason::Dwelling;
            return s.mode;
        }
        s.mode = PaceMode::Full;
        s.lastSwitchNs = nowNs;
        s.lastReason = PaceReason::Recovered;
        return s.mode;
    }
    // No switch: explain the tick for the log line. The shortfall and the
    // slow samples name the observation (LinkSlow) - they steer nothing.
    if (s.mode == PaceMode::Full)
        s.lastReason = shortfall > 6 || s.slowSamples > 3 ? PaceReason::LinkSlow : PaceReason::Ok;
    else
        s.lastReason = PaceReason::GameSlow;
    return s.mode;
}

// 起流前：设备自报的"大尺寸下能跑多少帧"低于目标刷新率时，建议的编码帧率。
// 返回 0 表示不降（信号不足或目标本来就够低）。
//
// 吸附系数说明（对任务卡的一处已文档化偏差）：卡面写 0.98，但其期望用例
// (72,70)→0 与变异要求（系数→1.0 时 (72,72) 必须挂）只有「0.97 + 严格大于」
// 能同时满足——72×0.98=70.56，70 并不在 0.98 的吸附带内（70 < 70.56）。本实现
// 按用例为准取 0.97，请终验裁决。71.9 这类"面板少测两帧"仍被吸附（71.9 > 69.84）。
inline float SuggestStartFps(float targetFps, int maxFpsAtMaxRes) {
    if (targetFps <= 0 || maxFpsAtMaxRes <= 0) return 0;
    if (float(maxFpsAtMaxRes) > targetFps * 0.97f) return 0;
    // 低于阶梯最低档时落到最低档；否则取不高于该值的最大一档。
    for (int i = int(sizeof(kFpsLadder) / sizeof(kFpsLadder[0])) - 1; i >= 0; --i)
        if (float(kFpsLadder[i]) <= float(maxFpsAtMaxRes)) return float(kFpsLadder[i]);
    return float(kFpsLadder[0]);
}

}  // namespace rc
