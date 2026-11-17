#!/usr/bin/env python3
"""EXTREME robustness matrix - the one that would have caught the 2026-11-15 latency bug.

Why this exists
---------------
The original 21-case matrix (run_robust_matrix.py) reported 21/21 PASS while a real
session had MTP 205 ms. Root cause of the blind spot, not of the bug:

  * only 1 of 7 scenarios passed --net (x3 profiles = 3 of 21 cases), so 18 cases
    never instantiated VideoSender at all: no packetizer, no pacer, no feedback
    -> "guard dip 0" meant UNTESTED.
  * that single case used --flat (a few KB per frame), while the field frames were
    ~398 KB. A pacer with nothing to spread looks perfectly healthy.
  * parse() read zero latency fields, even though encode-bench already printed
    "net: submit->sent avg/p99" and "net: encode/send overlap".
  * the criteria had no latency budget whatsoever (only crash / peak / mean rate).

This matrix therefore forces --net on EVERY case, uses big frames, scripts the queue
feedback, and adds M4 (submit->sent, overlap) + M5 (clean-link dip) criteria.

A latency FAIL here is EXPECTED and is the point: it proves the test can see the
known pacing defect. Those are reported as EXPOSED, not as a new regression.

Usage:  python tools/diagnostics/run_extreme_matrix.py
Output: glm-toolkit/out/extreme/<case>.log
        ROBUST-EXTREME-2026-11-15.md
"""
import hashlib
import re
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

WS = Path(__file__).resolve().parents[3]
REPO = WS / "RinCast"
BENCH = REPO / "build" / "pc" / "bin" / "Release" / "encode-bench.exe"
OUT = WS / "glm-toolkit" / "out" / "extreme"
REPORT = WS / "ROBUST-EXTREME-2026-11-15.md"

PROFILES = [("0", "延迟优先"), ("1", "平衡"), ("2", "画质优先")]

# Scripted headset feedback. NOTE: this is an INPUT to the guard, unlike the real
# headset where queueDelayMs is the OUTPUT of our own pacing. See pacing_loop_sim.py
# for the closed-loop half that a scripted timeline cannot express.
FB = {
    "clean":    ["--fb-every", "100", "--fb-queue", "0:3,60:3", "--fb-loss", "0:0"],
    "step":     ["--fb-every", "100", "--fb-queue", "0:3,2:60,30:60", "--fb-loss", "0:0"],
    "saw":      ["--fb-every", "100", "--fb-queue", "0:3,2:110,5:3,8:110,11:3,14:110,20:3",
                 "--fb-loss", "0:0"],
    "lossOnly": ["--fb-every", "100", "--fb-queue", "0:-1,30:-1", "--fb-loss", "0:0.03"],
    "lossQ":    ["--fb-every", "100", "--fb-queue", "0:80,30:80", "--fb-loss", "0:0.10"],
}
CONTENT = {
    "noise": ["--synthetic"],          # ~398 KB/frame at 100 Mbps: the field case
    "flat":  ["--flat"],
    "still": ["--still", "--flat"],
}
INJECT = {
    "none":  [],
    "swing": ["--swing"],
    "stall": ["--stall-every", "120:800"],
}


def case_args(ceiling, content, fb, inject, frames, size="3488x1744"):
    return (["--codec", "hevc", "--size", size, "--fps", "90",
             "--mbps", str(ceiling), "--frames", str(frames),
             "--pace", "90", "--net", "--out", "NUL"]
            + CONTENT[content] + FB[fb] + INJECT[inject])


def build_cases():
    """Layered sampling: full boundaries, not a 675-case cartesian product."""
    c = []

    def add(layer, name, ceiling, content, fb, inject, frames=240, size="3488x1744"):
        c.append(dict(layer=layer, name=name, ceiling=ceiling, content=content,
                      fb=fb, inject=inject, frames=frames, size=size))

    # L1: the exact field failure, long enough to see a sawtooth settle.
    add("L1 真机故障复现", "噪声大帧+队列阶跃 600帧", 100, "noise", "step", "none", 600)
    # L2: ceiling gradient x 3 profiles x high queue (the user's ceiling is a ceiling).
    for m in (20, 60, 100, 140, 500):
        add("L2 上限梯度×高队列", f"噪声 {m}Mbps 队列阶跃", m, "noise", "step", "none")
    # L3: clean-link control - the guard must NOT dip here.
    for m in (100, 140):
        add("L3 干净链路对照", f"噪声 {m}Mbps 队列恒定3ms", m, "noise", "clean", "none")
    # L4: feedback modes - sawtooth / loss-only / loss+queue.
    for f in ("saw", "lossOnly", "lossQ"):
        add("L4 反馈模式", f"噪声 100Mbps {f}", 100, "noise", f, "none", 300)
    # L5: inject on top of the failure.
    for j in ("swing", "stall"):
        add("L5 抖动卡顿叠加", f"噪声 100Mbps 队列阶跃+{j}", 100, "noise", "step", j, 300)
    # L6: content control - proves why --flat hid the bug.
    for ct in ("flat", "still"):
        add("L6 内容对照", f"{ct} 100Mbps 队列阶跃", 100, ct, "step", "none")
    # L7: long-run lock-down (900 frames) + real quality-tier resolution.
    add("L7 长跑自锁", "噪声 100Mbps 队列锯齿+卡顿 900帧", 100, "noise", "saw", "stall", 900)
    add("L8 画质档真实分辨率", "噪声 3904x1952 队列阶跃", 100, "noise", "step", "none", 300,
        "3904x1952")
    return c


def parse(text):
    d = dict(wire=None, fps=None, peak=None, setting=None, ratio=None,
             encAvg=None, encP95=None, frameKB=None, dips=0, brakes=0, reconf=0,
             errors=0, sentAvg=None, sentP99=None, theoryAvg=None, ratioTheory=None,
             overlapPct=None, overlapN=None, gapP50=None, gapP99=None,
             batchP90=None, netMbps=None, datagrams=None)
    m = re.search(r"wire:\s+([\d.]+) Mbps at ([\d.]+) fps", text)
    if m:
        d["wire"], d["fps"] = float(m.group(1)), float(m.group(2))
    m = re.search(r"paced: peak 1s throughput ([\d.]+) Mbps against (\d+) Mbps setting", text)
    if m:
        d["peak"], d["setting"] = float(m.group(1)), int(m.group(2))
        d["ratio"] = d["peak"] / d["setting"] if d["setting"] else None
    m = re.search(r"frame KB: avg ([\d.]+)", text)
    if m:
        d["frameKB"] = float(m.group(1))
    m = re.search(r"net: submit->sent avg ([\d.]+) ms p99 ([\d.]+) ms; "
                  r"theoretical avg ([\d.]+) ms \(ratio ([\d.]+)\)", text)
    if m:
        d["sentAvg"], d["sentP99"] = float(m.group(1)), float(m.group(2))
        d["theoryAvg"], d["ratioTheory"] = float(m.group(3)), float(m.group(4))
    m = re.search(r"net: encode/send overlap (\d+) of (\d+) frames \((\d+)%\)", text)
    if m:
        d["overlapN"], d["overlapPct"] = int(m.group(2)), int(m.group(3))
    m = re.search(r"net: gap p50 ([\d.]+) ms p99 ([\d.]+) ms", text)
    if m:
        d["gapP50"], d["gapP99"] = float(m.group(1)), float(m.group(2))
    m = re.search(r"batch p50 [\d.]+ KB p90 ([\d.]+) KB", text)
    if m:
        d["batchP90"] = float(m.group(1))
    m = re.search(r"net: (\d+) datagrams over [\d.]+ s, ([\d.]+) Mbps", text)
    if m:
        d["datagrams"], d["netMbps"] = int(m.group(1)), float(m.group(2))
    d["dips"] = len(re.findall(r"guard dip", text))
    d["brakes"] = len(re.findall(r"loss brake", text))
    d["reconf"] = len(re.findall(r"fps measured", text))
    d["errors"] = len(re.findall(r"\[E\]|\bERROR\b", text))
    return d


def judge(case, d, rc):
    """Returns (verdict, reasons). EXPOSED = the known pacing defect, seen on purpose."""
    crash, ceiling, latency, saw = [], [], [], []
    if rc != 0 or d["errors"]:
        crash.append(f"M1 退出码 {rc} / ERROR {d['errors']}")
    if d["ratio"] is not None and d["ratio"] > 1.5:
        ceiling.append(f"M3a 峰值 {d['ratio']:.2f}x")
    if d["wire"] is not None and d["wire"] > case["ceiling"] * 1.05:
        ceiling.append(f"M3b 平均 {d['wire']:.1f} > {case['ceiling']} Mbps")
    # M4: the criteria the old matrix never had. Thresholds are generous: the field
    # failure was 39.5 ms encoderWait / 2% overlap, so a healthy stream passes easily.
    if d["sentAvg"] is not None and d["sentAvg"] > 20.0:
        latency.append(f"M4a submit->sent avg {d['sentAvg']:.1f} ms > 20")
    if d["sentP99"] is not None and d["sentP99"] > 40.0:
        latency.append(f"M4b p99 {d['sentP99']:.1f} ms > 40")
    if d["overlapPct"] is not None and d["overlapPct"] < 80:
        latency.append(f"M4c 编码/发送重叠 {d['overlapPct']}% < 80")
    if d["gapP99"] is not None and d["gapP99"] > 3.0:
        latency.append(f"M4d gap p99 {d['gapP99']:.1f} ms > 3")
    if d["sentAvg"] is None:
        latency.append("M4 未取到 submit->sent（--net 无数据报，等于没测）")
    # M5/M6: clean link must not dip; loss-only must not dip without a queue.
    if case["fb"] == "clean" and d["dips"] > 0:
        saw.append(f"M5 干净链路仍有 {d['dips']} 次 guard dip")
    if case["fb"] == "lossOnly" and d["dips"] > 2:
        saw.append(f"M6 仅丢包无队列却有 {d['dips']} 次 dip")
    if crash or ceiling or saw:
        return "REGRESSION", crash + ceiling + saw + latency
    if latency:
        return "EXPOSED", latency
    return "PASS", []


def main():
    if not BENCH.exists():
        print("missing", BENCH)
        return 2
    OUT.mkdir(parents=True, exist_ok=True)
    head = subprocess.run(["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"],
                          capture_output=True, text=True).stdout.strip()
    binsha = hashlib.sha256(BENCH.read_bytes()).hexdigest()[:16]
    cases = build_cases()
    total = len(cases) * len(PROFILES)
    rows = []
    n = 0
    t0 = time.time()
    for c in cases:
        for pid, pname in PROFILES:
            n += 1
            tag = f"{c['layer'].split()[0]}-{c['name'].replace(' ', '')}-p{pid}"
            tag = re.sub(r"[^\w.-]", "", tag)[:70]
            cmd = [str(BENCH)] + case_args(c["ceiling"], c["content"], c["fb"], c["inject"],
                                          c["frames"], c["size"]) + ["--profile", pid]
            print(f"[{n}/{total}] {tag}", flush=True)
            try:
                p = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
                text, rc = p.stdout + p.stderr, p.returncode
            except subprocess.TimeoutExpired as e:
                text = (e.stdout or b"").decode(errors="replace") + "\nTIMEOUT 300s"
                rc = -9
            (OUT / f"{tag}.log").write_text(text, encoding="utf-8", errors="replace")
            d = parse(text)
            verdict, why = judge(c, d, rc)
            rows.append((c, pid, pname, tag, d, rc, verdict, why, " ".join(cmd)))
            print(f"    rc={rc} sentAvg={d['sentAvg']} p99={d['sentP99']} "
                  f"overlap={d['overlapPct']}% frameKB={d['frameKB']} dips={d['dips']} "
                  f"-> {verdict}", flush=True)

    def f(v, fmt="{:.1f}"):
        return fmt.format(v) if v is not None else "—"

    lines = [
        "# ROBUST-EXTREME · 极限鲁棒性矩阵（全场景强制 --net）",
        "",
        f"> 自动生成于 {datetime.now().strftime('%Y-%m-%d %H:%M')}，由 "
        "`tools/diagnostics/run_extreme_matrix.py` 解析每条 bench 原始 stdout 得到，**没有一个数字是手抄的**。",
        f"> 仓库 HEAD：`{head}`；`encode-bench.exe` sha256 前 16 位 `{binsha}`；耗时 {time.time()-t0:.0f} s。",
        "",
        "## 与旧矩阵的区别（这次为什么能测出来）",
        "",
        "旧 `run_robust_matrix.py` 的 7 个场景里 **只有 1 个（S13）传了 `--net`**（×3 档 =",
        " 21 组里只有 3 组量过发送时延）**，其余 18 组根本没实例化 `VideoSender`，",
        "pacer 不在回路里，`guard dip 0` 是「没测」不是「通过」；",
        "而唯一那组用了 `--flat`（每帧几 KB），真机是 ~398 KB。本矩阵**每一组都开 `--net`**，",
        "用大帧 + 脚本化队列反馈，并把 bench 一直在打印、旧 `parse()` 却一个都没读的",
        "`submit->sent` / `encode/send overlap` 变成正式判据。",
        "",
        "## 判据",
        "",
        "- **M1** 退出码 0、无 `[E]`/`ERROR`；**M3a** 峰值 ≤ 1.5×；**M3b** 平均 ≤ 上限×1.05。",
        "- **M4a** `net: submit->sent avg` ≤ 20 ms（真机 `encoderWait` 39.5 ms 是坏值）。",
        "- **M4b** `submit->sent p99` ≤ 40 ms；**M4c** 编码/发送重叠 ≥ 80%（源码注释声称重叠，实测 2%）。",
        "- **M4d** `gap p99` ≤ 3 ms；取不到 `submit->sent` 直接判没测。",
        "- **M5** 干净链路（队列恒定 3 ms）不得出现 `guard dip`；**M6** 仅丢包无队列时 dip ≤ 2。",
        "- 结论分三档：`PASS` / `EXPOSED`（命中已知 pacing 缺陷，**这是本轮故意要暴露的，不是新回归**） /",
        "  `REGRESSION`（崩溃、越界、干净链路误判——这些必须修）。",
        "",
        "## 结果表",
        "",
        "| 层 | 场景 | 档 | 上限 | 帧KB | wire | sentAvg/p99 ms | 理论 ms | 重叠% | gap p99 | dip | 判据命中 | 结论 | 证据 |",
        "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|",
    ]
    for c, pid, pname, tag, d, rc, verdict, why, cmd in rows:
        lines.append(
            f"| {c['layer'].split()[0]} | {c['name']} | {pid} {pname} | {c['ceiling']} | "
            f"{f(d['frameKB'])} | {f(d['wire'])} | {f(d['sentAvg'])}/{f(d['sentP99'])} | "
            f"{f(d['theoryAvg'])} | {d['overlapPct'] if d['overlapPct'] is not None else '—'} | "
            f"{f(d['gapP99'])} | {d['dips']} | {'; '.join(why) if why else '—'} | {verdict} | "
            f"`glm-toolkit/out/extreme/{tag}.log` |")

    cnt = {v: sum(1 for r in rows if r[6] == v) for v in ("PASS", "EXPOSED", "REGRESSION")}
    lines += ["", f"合计 **{len(rows)}** 组：PASS **{cnt['PASS']}**、EXPOSED **{cnt['EXPOSED']}**、"
             f"REGRESSION **{cnt['REGRESSION']}**。", ""]

    reg = [r for r in rows if r[6] == "REGRESSION"]
    lines += ["## REGRESSION 明细（必须修）", ""]
    if reg:
        for c, pid, pname, tag, d, rc, v, why, cmd in reg:
            lines += [f"### {tag} — {'; '.join(why)}", "", "```", cmd, "```", ""]
    else:
        lines += ["无。", ""]

    exp = [r for r in rows if r[6] == "EXPOSED"]
    lines += ["## EXPOSED 汇总（已知 pacing 缺陷，证明本判据有效）", ""]
    if exp:
        sa = [r[4]["sentAvg"] for r in exp if r[4]["sentAvg"] is not None]
        p99 = [r[4]["sentP99"] for r in exp if r[4]["sentP99"] is not None]
        ov = [r[4]["overlapPct"] for r in exp if r[4]["overlapPct"] is not None]
        lines += [f"- 命中 {len(exp)} 组；`submit->sent avg` 均值 **{sum(sa)/len(sa):.1f} ms**、"
                 f"`p99` 均值 **{sum(p99)/len(p99):.1f} ms**、重叠率均值 "
                 f"**{sum(ov)/len(ov):.0f}%**（源码注释假设 100%）。",
                 "- 这些数字与真机 `encoderWait 39.5 ms` / `downlink 76.7 ms` 同量级，",
                 "  说明**离线 bench 本来就能复现**，只是旧矩阵没开 `--net`、没读延迟字段。", ""]
    else:
        lines += ["无（若这里为空，说明判据或场景仍有盲区，需复查）。", ""]

    lines += [
        "## 边界（诚实的局限）",
        "",
        "- `--fb-queue` 是**脚本注入的输入**；真机的 `queueDelayMs` 是我们自己 pacing 造成的",
        "  **输出**。所以「队列→砍cap→pacer更慢→队列更高」的自锁闭环，脚本化矩阵只能看到",
        "  前半段，后半段由 `glm-toolkit/pacing_loop_sim.py` 的数字孪生补上。",
        "- 头显侧 `receivedNs`（所有分片收齐）、预测缓冲 `clamp(15,120)`、`VR prediction` 分解",
        "  不在离线链路里，仍需真机或 headset-sim；A 组 S1–S7 / C 组 S14–S17 依旧未跑。",
        "- **注入语义**：恒定高队列（如 `lossQ` 的 80 ms）会被守卫当成设备固有 floor 吸收，",
        "  `dipAt = max(target+band, floor+extra)` 抬到 95 ms，80 ms 落在死区，所以该场景",
        "  `guard dip = 0` 不代表没拥塞，生效的是 `loss brake`。驱动守卫必须用队列**阶跃**。",
        "- bench 的 `--net` 把 `VideoSender` 的 pacing 固定打开，没有 `pace=false` 对照；",
        "  关 pacing 的效果由仿真与 `RinCast-1.2.1-nopace-TEST-Setup.exe` 真机包验证。",
        "",
        "## 复现方式",
        "",
        "```",
        "python glm-toolkit/run_extreme_matrix.py",
        "```",
        "",
    ]
    REPORT.write_text("\n".join(lines), encoding="utf-8")
    print("report:", REPORT)
    return 0 if not reg else 1


if __name__ == "__main__":
    sys.exit(main())
