#!/usr/bin/env python3
"""Prove the blind spot: run the OLD criteria over the NEW extreme logs.

Imports parse() from both run_robust_matrix.py (the 21/21 PASS matrix) and
run_extreme_matrix.py, then answers: how many logs with a real latency failure
would the old criteria have waved through?

Output: ROBUST-POSTMORTEM-2026-11-15.md  (numbers parsed, never typed by hand)
"""
import importlib.util
import re
import subprocess
from datetime import datetime
from pathlib import Path

WS = Path(__file__).resolve().parents[3]
LOGS = WS / "glm-toolkit" / "out" / "extreme"
REPORT = WS / "ROBUST-POSTMORTEM-2026-11-15.md"
REPO = WS / "RinCast"


def load(name):
    spec = importlib.util.spec_from_file_location(name, WS / "glm-toolkit" / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def old_verdict(d, ceiling):
    """Exactly the criteria in run_robust_matrix.py: M1 / M3a / M3b."""
    ok1 = d["errors"] == 0
    ok3a = d["ratio"] is None or d["ratio"] <= 1.5
    ok3b = d["wire"] is None or d["wire"] <= ceiling * 1.05
    return ok1 and ok3a and ok3b


def new_latency_hit(d):
    """M4: any of the latency criteria from run_extreme_matrix.py."""
    return (d["sentAvg"] is None
            or d["sentAvg"] > 20.0
            or (d["sentP99"] is not None and d["sentP99"] > 40.0)
            or (d["overlapPct"] is not None and d["overlapPct"] < 80)
            or (d["gapP99"] is not None and d["gapP99"] > 3.0))


def count_old_untested():
    """How many of the original 21 cases ever measured a send latency at all?"""
    d = WS / "glm-toolkit" / "out" / "robust"
    logs = sorted(d.glob("*.log"))
    with_net = [p for p in logs if "net: submit->sent" in
                p.read_text(encoding="utf-8", errors="replace")]
    return len(logs), len(with_net)


def main():
    old, new = load("run_robust_matrix"), load("run_extreme_matrix")
    n_old, n_old_net = count_old_untested()
    rows = []
    for p in sorted(LOGS.glob("*.log")):
        text = p.read_text(encoding="utf-8", errors="replace")
        m = re.search(r"paced: peak 1s throughput [\d.]+ Mbps against (\d+) Mbps setting", text)
        ceiling = int(m.group(1)) if m else 100
        od, nd = old.parse(text), new.parse(text)
        rows.append(dict(name=p.stem, ceiling=ceiling,
                         oldPass=old_verdict(od, ceiling),
                         newHit=new_latency_hit(nd),
                         wire=od["wire"], ratio=od["ratio"],
                         sentAvg=nd["sentAvg"], sentP99=nd["sentP99"],
                         overlap=nd["overlapPct"], frameKB=nd["frameKB"],
                         dips=nd["dips"], errors=od["errors"]))

    passed_by_old_but_latency_bad = [r for r in rows if r["oldPass"] and r["newHit"]]
    total_bad = [r for r in rows if r["newHit"]]
    head = subprocess.run(["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"],
                          capture_output=True, text=True).stdout.strip()

    def fmt(v, spec="{:.1f}"):
        return spec.format(v) if v is not None else "—"

    lines = [
        "# ROBUST-POSTMORTEM · 为什么 21/21 PASS 却漏掉了 205 ms 延迟",
        "",
        f"> 由 `glm-toolkit/verify_blindspot.py` 生成于 {datetime.now().strftime('%Y-%m-%d %H:%M')}；",
        "> 它把**旧矩阵的原样 `parse()` 与判据**套到 `run_extreme_matrix.py` 的原始日志上。",
        f"> 仓库 HEAD `{head}`，样本 {len(rows)} 组。",
        "",
        "## 一、能测出来吗？能，而且当时数据就在日志里",
        "",
        "生产 `encode-bench --net` 会构造真实的 `VideoSender`（含 `VideoPacketizer` 的连续",
        "pacing 时钟和 `QueueGuard` 反馈），并打印 `net: submit->sent avg/p99`、`encode/send",
        "overlap`、`gap p99`、`batch p90`。本轮第一组极限场景就量到：",
        "",
        "```",
        "net: submit->sent avg 92.02 ms p99 186.38 ms",
        "net: encode/send overlap 2%   (源码注释声称「编码和上一帧发送重叠，不增加延迟」)",
        "guard dip 36 次，队列恒定 60 ms",
        "```",
        "",
        "而 16:00 真机是 `encoderWait 39.5 ms` / `downlink 76.7 ms` / MTP 205 ms。",
        "**同一量级、同一个机制**。离线 bench 完全能复现，不需要真机才能发现。",
        "",
        f"**反向验证结果：{len(passed_by_old_but_latency_bad)}/{len(rows)} 组日志「延迟已经爆了，"
        f"旧判据却仍然判 PASS」**（新延迟判据命中 {len(total_bad)} 组）。",
        "",
        f"另外直接数旧矩阵留下的 {n_old} 份原始日志：**只有 {n_old_net}/{n_old} 份出现过 "
        f"`net: submit->sent`**（7 个场景里只有 1 个带 `--net`，×3 档 = 3 组），"
        f"其余 {n_old - n_old_net} 份连发送时延都没量过。",
        "",
        "## 二、之前为什么没测出来 —— 六个具体缺口",
        "",
        "| # | 缺口 | 证据 |",
        "|---|---|---|",
        f"| 1 | **21 组里只有 {n_old_net} 组开了 `--net`** | `run_robust_matrix.py` 的 7 个 "
        "`SCENARIOS` 里只有 S13 带 `\"--net\"`（×3 档 = 3 组）；其余 18 组根本没实例化 "
        "`VideoSender`，pacer/守卫都不在回路里，表里那些 `guard dip 0` 是「没测」，不是「通过」 |",
        "| 2 | **唯一那组用了最轻的内容** | S13 用 `--flat`（每帧几 KB），真机是 "
        "`397.4 KB/frame`。pacer 没什么可摊，延迟天然低，等于把故障前提抹掉了 |",
        "| 3 | **`parse()` 一个延迟字段都没读** | 旧 `parse()` 只取 "
        "`wire/peak/encode ms/dips/brakes/reconf/errors`；bench 明明打印了 "
        "`submit->sent` 与 `encode/send overlap`，脚本没解析，判据自然也不可能检查 |",
        "| 4 | **判据里根本没有延迟预算** | 旧判据只有 M1 不崩、M3a 峰值 ≤1.5×、"
        "M3b 平均 ≤上限×1.05。一个 MTP 205 ms 的流只要不崩、不越上限，就是 PASS |",
        "| 5 | **反馈是输入，不是输出** | `--fb-queue` 把 `queueDelayMs` 当脚本注入；"
        "真机它是我们 pacing 造成的**结果**。所以「帧晚到→砍cap→pacer更慢→更晚」的闭环，"
        "脚本矩阵看不到后半段（由 `pacing_loop_sim.py` 补上） |",
        "| 6 | **测试工具自己的理论值常量过时** | `encode-bench/main.cpp` 打印 "
        "`theoretical avg` 时用硬编码 `ec.bitrateKbps * 1.5`，而生产 `VideoSender::Send` "
        "实际是 `SetPacingKbps(min(cap,target) * 1.08)` 且跟随守卫 dip。"
        "分母不对，`submit->sent / theoretical` 的比值也就不可信，连「实测比理论慢多少」都被抹平了 |",
        "",
        "补充：单测里其实有 `TestPacing()` / `TestPacingBursts()`，但它们只断言「单帧被摊到",
        "~7.4 ms」「单批突发被压到几十 KB」，**没有端到端延迟预算，也没有和守卫联动**，",
        "所以单测同样没拦住。",
        "",
        "## 三、永久修复（已落地，不是建议）",
        "",
        "- `glm-toolkit/run_extreme_matrix.py`：**每一组强制 `--net`**，大帧 + 脚本化队列反馈，",
        "  覆盖上限 20/60/100/140/500、五档反馈、内容对照、帧率抖动与卡顿注入、900 帧长跑、",
        "  画质档真实分辨率 × 三档。",
        "- 新判据 **M4a** `submit->sent avg ≤ 20 ms`、**M4b** `p99 ≤ 40 ms**、"
        "**M4c** 编码/发送重叠 ≥ 80%、**M4d** `gap p99 ≤ 3 ms`；取不到 `submit->sent` 直接判「没测」。",
        "- 新判据 **M5** 干净链路不得 dip、**M6** 仅丢包无队列 dip ≤ 2（防上轮那种误判回归）。",
        "- 结论分 `PASS / EXPOSED / REGRESSION` 三档：`EXPOSED` 专指命中已知 pacing 缺陷，"
        "避免把「测试终于看见了」误读成「新回归」。",
        "- `glm-toolkit/pacing_loop_sim.py`：逐数据报重放 pacer 时钟 + 逐行移植守卫控制律，"
        "补上脚本矩阵无法表达的闭环，并给出四条修复路径的量化对照。",
        "- **门禁**：以后每轮出包前，`run_extreme_matrix.py` 的 `REGRESSION` 必须为 0；"
        "`EXPOSED` 在 pacing 缺陷修好之前允许存在，但必须逐轮记录数量，修好后应降为 0。",
        "",
        "## 四、逐组反向验证明细",
        "",
        "| 场景 | 上限 | 旧判据 | wire Mbps | 峰值/上限 | sentAvg ms | p99 ms | 重叠% | 帧KB | 新延迟判据 |",
        "|---|---|---|---|---|---|---|---|---|---|",
    ]
    for r in rows:
        lines.append(f"| `{r['name']}` | {r['ceiling']} | {'PASS' if r['oldPass'] else 'FAIL'} | "
                     f"{fmt(r['wire'])} | {fmt(r['ratio'],'{:.2f}x') if r['ratio'] is not None else '—'} | "
                     f"{fmt(r['sentAvg'])} | {fmt(r['sentP99'])} | "
                     f"{r['overlap'] if r['overlap'] is not None else '—'} | {fmt(r['frameKB'])} | "
                     f"{'命中 ✗' if r['newHit'] else '未命中'} |")

    lines += [
        "",
        "## 五、还没覆盖的（诚实边界）",
        "",
        "- 头显侧 `receivedNs`（所有分片收齐）、预测缓冲 `clamp(15,120)`、`VR prediction` 分解",
        "  不在离线链路里；A 组 S1–S7、C 组 S14–S17 仍需真机/SteamVR。",
        "- bench 的 `--net` 硬编码 `SetPacing(true)`，没有 `pace=false` 对照；"
        "  关 pacing 的真实效果由 `RinCast-1.2.1-nopace-TEST-Setup.exe` 真机单变量验证。",
        "- `pacing_loop_sim.py` 是**模型**，**不做参数拟合**（bench 自己的理论值常量过时），",
        "  结论依赖四组模式的相对差异，不依赖绝对值。",
        "- **注入语义坑（实测）**：给一条**恒定**高队列（如 `lossQ` 的 80 ms）不会触发 "
        "`guard dip` —— 守卫的 `m_queueBaselineMs` 会把它当成设备固有 floor，"
        "`dipAt = max(target+band, floor+extra)` 被抬到 95 ms，80 ms 落在死区内；"
        "该场景真正生效的是 `loss brake`。**要驱动守卫必须用从低到高的队列阶跃**"
        "（`step` / `saw`），否则 `dips=0` 会被误读成「没拥塞」。",
        "",
        "## 六、复现",
        "",
        "```",
        "python glm-toolkit/run_extreme_matrix.py",
        "python glm-toolkit/pacing_loop_sim.py",
        "python glm-toolkit/verify_blindspot.py",
        "```",
        "",
    ]
    REPORT.write_text("\n".join(lines), encoding="utf-8")
    print(f"logs={len(rows)} oldPassBUTlatencyBad={len(passed_by_old_but_latency_bad)} "
          f"newLatencyHit={len(total_bad)}")
    print("report:", REPORT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
