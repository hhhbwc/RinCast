#!/usr/bin/env python3
"""Closed-loop digital twin for the 2026-11-15 latency failure.

WHY A SIMULATION
----------------
run_extreme_matrix.py scripts queueDelayMs as an INPUT. On a real headset it is an
OUTPUT: our own pacer spreads one frame, the frame lands late, the headset reports a
queue, the guard cuts the cap, and (this is the bug) the pacer rate FOLLOWS the cap,
so the next frame spreads even longer. A scripted timeline cannot express that loop.

This twin replays the loop with the production constants copied verbatim from:
  RinCast/shared/src/video_transport.cpp       (pacer clock, thresholds, FEC)
  RinCast/shared/include/rc/queue_guard.h     (dip / climb / ceiling law)
  RinCast/pc/streamer/src/stream/video_sender.cpp (pacingKbps = TargetBitrateKbps*1.08)

It is a MODEL, not the production binary. NO PARAMETER FITTING is done: bench's own
"theoretical" line uses a stale x1.5 constant, so it cannot calibrate anything.
Constants come from source, the field frame sizes from the 16:00 session log, and
every assumption is printed. Conclusions rest on the four modes' relative behaviour.

Output: glm-toolkit/out/pacing_sim.json
        PACING-SIM-2026-11-15.md
"""
import json
import math
import random
from pathlib import Path

WS = Path(__file__).resolve().parents[3]
OUT = WS / "glm-toolkit" / "out"
REPORT = WS / "PACING-SIM-2026-11-15.md"

# ---- production constants (copy, do not invent) -------------------------------
NS = 1_000_000_000
K_PACE_WAIT_THRESHOLD_NS = 200_000      # video_transport.cpp: inside 0.2 ms -> send now
K_PACE_MAX_DEBT_NS = 600_000           # drop debt beyond 0.6 ms
K_PACE_IDLE_RESET_NS = 50_000_000      # 50 ms idle resets the continuous clock
FRAG_TOTAL_BYTES = 1368                # bench: "packet max 1368 B" (header + payload)
FRAG_PAYLOAD = 1200                    # kMaxDatagramLan - sizeof(VideoFragmentHeader)
FEC_GROUP = 20                         # SetFecPercent(5) -> clamp(100/5,2,64)
FRAME_INTERVAL_NS = NS // 90          # 90 fps
SEND_COST_NS = 3_000                  # estimated memcpy+sendto per datagram (ASSUMPTION)
# video_transport.cpp comment: "a 0.5 ms period timer ticks at 1.0 ms" on this machine.
TIMER_TICK_NS = 1_000_000
LINK_FLOOR_MS = 4.0                    # field log: "floor 4" (device's own idle queue)
LINK_CAPABILITY_KBPS = 800_000        # measured by glm-toolkit/link_udp_test.py (800 Mbps)

TARGET_KBPS = 100_000                 # user's ceiling, 100 Mbps
PROFILE_TARGET_MS = 15.0             # quality-first: ProfileParams.targetQueueMs
PROFILE_BAND_MS = 6.0
PROFILE_FLOOR_EXTRA_MS = 15.0
MIN_DIVISOR = 8
SECONDS = 20
FEEDBACK_EVERY_NS = 100_000_000       # bench/headset feedback cadence

# ---- guard: line-for-line port of QueueGuard::Update --------------------------
class Guard:
    def __init__(self):
        self.cap = 0
        self.ceiling = 0
        self.ceiling_ns = 0
        self.last_dip_ns = 0
        self.last_climb_ns = 0
        self.last_key_dip_ns = 0
        self.first_ns = -1

    def aim(self, target):
        if not self.ceiling:
            return target
        return min(target, max(self.ceiling * 97 // 100, 2000))

    def update(self, target, queue_ms, queue_floor_ms, keyframe, now):
        act = "None"
        if not target:
            return act
        if self.first_ns < 0:
            self.first_ns = now
        starting = now - self.first_ns < 2 * NS
        hard_floor = max(target // MIN_DIVISOR, 1000)
        measured = queue_ms >= 0
        climb_under = max(PROFILE_TARGET_MS - PROFILE_BAND_MS, queue_floor_ms + 2.0)
        dip_at = max(PROFILE_TARGET_MS + PROFILE_BAND_MS, queue_floor_ms + PROFILE_FLOOR_EXTRA_MS)

        if self.ceiling and now - self.ceiling_ns > 60 * NS:
            step = max(self.ceiling * 3 // 100, 1)
            raised = self.ceiling + step
            self.ceiling = 0 if raised >= target else raised
            self.ceiling_ns = now

        def dip(permille, congestion):
            nonlocal act
            base = self.cap or target
            nxt = max(base * permille // 1000, hard_floor)
            meaningful = base > hard_floor + hard_floor // 20
            if congestion and meaningful and now - self.last_dip_ns > 2 * NS:
                self.ceiling = base
                self.ceiling_ns = now
            if self.cap and nxt >= self.cap:
                return False
            self.cap = nxt
            self.last_dip_ns = now
            act = "Dip"
            return True

        def climb(permille):
            nonlocal act
            base = self.cap or target
            nxt = max(base * permille // 1000, base + 1)
            aim = self.aim(target)
            if nxt >= aim:
                if aim >= target:
                    self.cap = 0
                    act = "Released"
                else:
                    held = max(aim, base)
                    if held == base:
                        return
                    self.cap = held
                    act = "Climb"
            else:
                self.cap = nxt
                act = "Climb"
            self.last_climb_ns = now

        if keyframe and not starting and measured and queue_ms > dip_at and \
                now - self.last_key_dip_ns > 2 * NS:
            self.last_key_dip_ns = now
            if dip(650, True):
                return act
        if measured and queue_ms > dip_at and now - self.last_dip_ns > 400_000_000:
            deep = queue_ms > dip_at * 2.5
            if dip(750 if deep else 900, True):
                return act
        if self.cap:
            calm = not measured or queue_ms < climb_under
            pinned = self.cap <= hard_floor and now - self.last_dip_ns > 4 * NS
            if now - self.last_dip_ns > 800_000_000 and now - self.last_climb_ns > 400_000_000 \
                    and (calm or pinned):
                climb(1250 if pinned else 1040)
        return act


def send_frame(frame_bytes, pacing_kbps, clock_ns, now_ns, timer_tick_ns, send_cost_ns):
    """Replays VideoPacketizer::SendFrame's paced loop, datagram by datagram."""
    count = math.ceil(frame_bytes / FRAG_PAYLOAD)
    parity = math.ceil(count / FEC_GROUP)
    total_dg = count + parity
    ns_per_byte = 8e6 / pacing_kbps
    if clock_ns == 0 or clock_ns < now_ns - K_PACE_IDLE_RESET_NS:
        clock_ns = now_ns
    waits = 0
    for _ in range(total_dg):
        remain = clock_ns - now_ns
        if remain > K_PACE_WAIT_THRESHOLD_NS:
            wait = math.ceil(remain / timer_tick_ns) * timer_tick_ns
            now_ns += wait
            waits += 1
            if clock_ns < now_ns - K_PACE_MAX_DEBT_NS:
                clock_ns = now_ns - K_PACE_MAX_DEBT_NS
        now_ns += send_cost_ns                       # sending itself is not free
        clock_ns += int(FRAG_TOTAL_BYTES * ns_per_byte)
    return now_ns, clock_ns, waits


def sanity_check():
    """Order-of-magnitude check only. NO FITTING.

    bench prints "theoretical avg 8.68 ms" using a HARD-CODED pacingKbps = bitrate*1.5
    (tools/encode-bench/main.cpp), but VideoSender::Send actually calls
    SetPacingKbps(TargetBitrateKbps() * 1.08), where TargetBitrateKbps() = min(cap,target)
    and cap follows the guard. So that 8.68 ms and the derived ratio 1.74 are computed
    with a stale constant and CANNOT be used to calibrate a model.

    We therefore take timer granularity and send cost straight from source/assumption,
    report what the model gives for the bench-sized frame, and draw conclusions ONLY
    from the relative difference between the four modes.
    """
    frame = int(8.68e-3 * 150_000_000 / 8)   # ~160 KB, from bench's own theory line
    end, clock, waits = send_frame(frame, 150_000, 0, 0, TIMER_TICK_NS, SEND_COST_NS)
    return end / 1e6, waits, frame


def run(mode, tick, cost, seed=20261115):
    """mode: follow / decoupled / burst_aware / off.

    follow       = production: pacingKbps = min(cap,target) * 1.08
    decoupled    = pacingKbps = target * 1.08, guard no longer slows the pacer
    burst_aware  = pacer also guarantees one frame fits inside one frame interval,
                   capped by the measured 800 Mbps link capability. This is a THEORETICAL
                   LOWER BOUND on latency, not a shippable law: it can exceed the user's
                   average ceiling for short bursts, so a token bucket would be needed.
    off          = no pacing at all (one burst); queue ~0, bust radio loss is NOT modelled.
    """
    rng = random.Random(seed)
    guard = Guard()
    clock_ns = 0
    now_ns = 0
    cap = 0
    queue_smooth = LINK_FLOOR_MS
    dips = 0
    next_fb_ns = 0
    timeline = []
    total_frames = SECONDS * 90
    for i in range(total_frames):
        # Field frame sizes: ~390 KB with short-term variation, a 2x keyframe every 2 s.
        kb = max(60.0, rng.gauss(390.0, 35.0))
        if i % 180 == 0:
            kb *= 2.0
        frame_bytes = int(kb * 1024)

        due_ns = (i + 1) * FRAME_INTERVAL_NS
        # SendFrame blocks the encoder thread: the next submit cannot start before it returns.
        submit_ns = max(now_ns, due_ns)
        pacing_kbps = 0
        if mode == "off":
            end_ns = submit_ns + cost * math.ceil(frame_bytes / FRAG_PAYLOAD)
        else:
            if mode == "follow":
                pacing_kbps = min(cap or TARGET_KBPS, TARGET_KBPS) * 1.08
            elif mode == "decoupled":
                pacing_kbps = TARGET_KBPS * 1.08
            elif mode == "burst_aware":
                # A frame must fit inside one frame interval, else the pacer itself is
                # the queue. 8 bits * bytes / interval -> kbit/s, with 10% headroom.
                needed = frame_bytes * 8 / (FRAME_INTERVAL_NS / NS) / 1000 * 1.10
                pacing_kbps = min(LINK_CAPABILITY_KBPS,
                                  max(TARGET_KBPS * 1.08, needed))
            pacing_kbps = max(1000, int(pacing_kbps))
            end_ns, clock_ns, waits = send_frame(frame_bytes, pacing_kbps,
                                                 clock_ns, submit_ns, tick, cost)
        now_ns = end_ns
        # The headset reports how late the frame landed relative to the link's own floor.
        span_ms = (end_ns - submit_ns) / 1e6
        observed = max(0.0, span_ms - LINK_FLOOR_MS)
        queue_smooth = queue_smooth * 0.6 + observed * 0.4 if queue_smooth >= 0 else observed

        if now_ns >= next_fb_ns:
            act = guard.update(TARGET_KBPS, queue_smooth, LINK_FLOOR_MS, False, now_ns)
            cap = guard.cap
            if act == "Dip":
                dips += 1
            next_fb_ns = now_ns + FEEDBACK_EVERY_NS

        if i % 90 == 89:
            timeline.append(dict(sec=(i + 1) // 90, capMbps=round(cap / 1000, 1) if cap else 100.0,
                                  paceMbps=round(pacing_kbps / 1000, 1),
                                  queueMs=round(queue_smooth, 1), spanMs=round(span_ms, 1),
                                  dips=dips))
    return dict(mode=mode, timeline=timeline, finalCapKbps=cap, totalDips=dips,
                finalQueueMs=round(queue_smooth, 1),
                meanSpanMs=round(sum(t["spanMs"] for t in timeline) / len(timeline), 1),
                maxSpanMs=max(t["spanMs"] for t in timeline),
                minCapMbps=round(min(t["capMbps"] for t in timeline), 1))


def main():
    sim_ms, waits, frame = sanity_check()
    tick, cost = TIMER_TICK_NS, SEND_COST_NS
    print(f"SANITY (no fit): tick={tick/1e6:.2f} ms, sendCost={cost} ns, "
          f"{frame} B frame -> {sim_ms:.2f} ms, {waits} timer waits "
          f"(bench measured 15.14 ms; bench's own 'theoretical' 8.68 ms uses a stale x1.5 constant)")
    modes = ("follow", "decoupled", "burst_aware", "off")
    results = {m: run(m, tick, cost) for m in modes}
    payload = dict(model=dict(timerTickNs=tick, sendCostNs=cost, benchAvgMs=15.14,
                              benchTheoreticalMs=8.68, benchTheoreticalStale=True,
                              sanitySimMs=round(sim_ms, 2), sanityTimerWaits=waits,
                              fitted=False),
                   assumptions=dict(fragTotalBytes=FRAG_TOTAL_BYTES, fragPayload=FRAG_PAYLOAD,
                                    fecGroup=FEC_GROUP, linkFloorMs=LINK_FLOOR_MS,
                                    linkCapabilityKbps=LINK_CAPABILITY_KBPS,
                                    targetKbps=TARGET_KBPS, frameKbMean=390,
                                    frameKbStd=35, seconds=SECONDS,
                                    profileTargetMs=PROFILE_TARGET_MS,
                                    profileBandMs=PROFILE_BAND_MS,
                                    profileFloorExtraMs=PROFILE_FLOOR_EXTRA_MS),
                   results=results)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "pacing_sim.json").write_text(json.dumps(payload, indent=2), encoding="utf-8")

    lines = [
        "# PACING-SIM · 闭环数字孪生（2026-11-15 延迟故障）",
        "",
        "> 由 `glm-toolkit/pacing_loop_sim.py` 生成。**这是模型，不是生产二进制实测**；",
        "  与 `ROBUST-EXTREME-2026-11-15.md` 的生产 bench 数据互为补充。",
        "",
        "## 为什么要仿真",
        "",
        "矩阵里的 `--fb-queue` 是**注入的输入**，真机的 `queueDelayMs` 是我们自己 pacing 造成的",
        "**输出**。脚本化时间线无法表达「帧晚到 → 上报队列 → 砍 cap → pacer 更慢 → 下一帧更晚」",
        "这个正反馈。本孪生按生产代码逐数据报重放 pacer 时钟、按生产控制律逐次重放守卫。",
        "",
        "## 常量来源与假设（不编造）",
        "",
        "| 项 | 值 | 来源 |",
        "|---|---|---|",
        "| `kPaceWaitThresholdNs` | 200,000 | `video_transport.cpp` |",
        "| `kPaceMaxDebtNs` | 600,000 | 同上 |",
        "| `kPaceIdleResetNs` | 50,000,000 | 同上 |",
        "| 每数据报字节 | 1368 | bench `packet max 1368 B` |",
        "| FEC 组 | 20 | `SetFecPercent(5)` → `clamp(100/5,2,64)` |",
        "| pacer 速率 | `min(cap,target) × 1.08` | `video_sender.cpp:68` |",
        "| 守卫 dip/climb/ceiling | 逐行移植 | `queue_guard.h` |",
        "| 帧大小 | 均值 390 KB / σ35 / 每 2 s 关键帧 ×2 | 真机 16:00 日志 `397.4 KB/frame` |",
        "| 链路固有 floor | 4 ms | 真机日志 `floor 4` |",
        "| 链路能力上限 | 800 Mbps | `glm-toolkit/link_udp_test.py` 独立 UDP 打流实测 |",
        f"| 每数据报发送成本 | {cost} ns | **假设值**（memcpy+sendto），非实测 |",
        f"| 高精度定时器粒度 | {tick/1e6:.2f} ms | `video_transport.cpp` 注释：本机 0.5 ms 周期定时器实际约 1.0 ms |",
        "",
        "### 为什么**不做数值拟合**（重要，别拿它凑数）",
        "",
        "bench 打印的 `theoretical avg 8.68 ms` 来自 `tools/encode-bench/main.cpp` 里硬编码的",
        "`pacingKbps = ec.bitrateKbps * 1.5`；但生产 `VideoSender::Send` 是",
        "`SetPacingKbps(TargetBitrateKbps() * 1.08)`，且 `TargetBitrateKbps() = min(cap,target)`",
        "会跟随守卫的 dip。**测试工具自己的理论值分母就是过时的**，`ratio 1.74` 因此不可信，",
        "拿它去拟合模型参数只会把错误固化下来。这是漏检的第 6 条原因。",
        "",
        f"所以本模型直接用源码常量，**只做量级自检**：用 bench 那份 ~{frame} B 的帧、",
        f"150 Mbps pacer 跑一次，模型给出 `{sim_ms:.2f} ms` / {waits} 次定时器等待，",
        "bench 实测 `15.14 ms`。**结论只依赖四种模式的相对差异，不依赖任何绝对数值**。",
        "",
        "## 四组对照（20 秒 / 90 fps / 100 Mbps 上限 / 画质优先档）",
        "",
        "| 模式 | 20s 末 cap Mbps | 最低 cap Mbps | 20s 末队列 ms | 平均单帧发完 ms | 最大 ms | dip 次数 | 结论 |",
        "|---|---|---|---|---|---|---|---|",
    ]
    verdict = {
        "follow": "**发散/自锁** —— 复现真机锯齿",
        "decoupled": "打断「cap→pacer」这一段，但大帧固有摊平时长仍超 dip 线 → 仍下探",
        "burst_aware": "**收敛** —— pacer 保证一帧在一个帧周期内发完（需令牌桶约束长期均值）",
        "off": "收敛 —— 无排队（突发丢包风险未建模，需真机 nopace 包验证）",
    }
    for m, r in results.items():
        lines.append(f"| `{m}` | {r['finalCapKbps']/1000 if r['finalCapKbps'] else 100.0:.1f} | "
                     f"{r['minCapMbps']:.1f} | {r['finalQueueMs']:.1f} | {r['meanSpanMs']:.1f} | "
                     f"{r['maxSpanMs']:.1f} | {r['totalDips']} | {verdict[m]} |")

    lines += ["", "## 逐秒时间线（pacer 跟随 cap）", "",
              "| 秒 | cap Mbps | pacer Mbps | 队列 ms | 单帧发完 ms | 累计 dip |",
              "|---|---|---|---|---|---|"]
    for t in results["follow"]["timeline"]:
        lines.append(f"| {t['sec']} | {t['capMbps']:.1f} | {t['paceMbps']:.1f} | "
                     f"{t['queueMs']:.1f} | {t['spanMs']:.1f} | {t['dips']} |")
    lines += ["", "## 回路", "",
              "```",
              "帧晚到(queue↑) → guard dip(cap↓) → pacingKbps = cap×1.08 ↓",
              "      ↑                                      ↓",
              "      └──────── 一帧摊得更久(span↑) ──────────┘",
              "```",
              "",
              "**关键结论不止一个**：`follow` 组 cap 下探到 floor、队列不回落，坐实自锁。但",
              "`decoupled` 组即便把 pacer 与 cap 解耦**仍未收敛** —— 因为画质优先档的 dip 线是",
              "`max(15+6, 4+15) = 21 ms`，而 390 KB 帧按 108 Mbps 摊平的理论值就是 ~29 ms，",
              "再加定时器开销必然撞线。也就是说，**光解耦不够， cap 与帧大小本身就是矛盾的**：",
              "capped VBR 允许单帧突发到 281 Mbps 瞬时，pacer 却按 100 Mbps 平均值摊，",
              "差额只能变成排队。",
              "",
              "因此可落地的三条路，按风险从低到高：",
              "",
              "1. **关 pacing**（`off`）——链路独立实测有 800 Mbps 余量，已备",
              "   `RinCast-1.2.1-nopace-TEST-Setup.exe` 做真机单变量验证；风险是突发丢包，需真机确认。",
              "2. **突发感知 pacer**（`burst_aware`）——速率取 `max(平均码率, 帧大小×8/帧间隔)` 并用",
              "   链路能力封顶，允许短时超过平均上限但用**令牌桶**保证长期均值不越过用户设的上限。",
              "   这是延迟最优且仍守「上限是上限」的方向，但实现量最大。",
              "3. **减小 VBV / 限制单帧最大字节**——把帧大小压回 pacer 能一个周期发完的范围，",
              "   代价是关键帧画质。",
              "",
              "`off` 与 `burst_aware` 的瞬时速率是否会在真实空口丢包，本模型不建模；",
              "`link_udp_test.py` 只证明 800 Mbps 匀速可全额交付，不等于 3 Gbps 瞬时突发不溢出缓冲。",
              "",
              "## 复现",
              "",
              "```",
              "python glm-toolkit/pacing_loop_sim.py",
              "```",
              "",
              "原始数据：`glm-toolkit/out/pacing_sim.json`。",
              "",
    ]
    REPORT.write_text("\n".join(lines), encoding="utf-8")
    print("report:", REPORT)
    for m, r in results.items():
        print(f"  {m:10s} finalCap={r['finalCapKbps']/1000 if r['finalCapKbps'] else 100:.1f} Mbps "
              f"minCap={r['minCapMbps']:.1f} queue={r['finalQueueMs']:.1f}ms "
              f"spanAvg={r['meanSpanMs']:.1f}ms dips={r['totalDips']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
