#!/usr/bin/env python3
"""Offline robustness matrix (PLAN-ROBUST-2026-11-15, B group S8-S13 x 3 profiles).

Runs encode-bench once per case, keeps the raw stdout, and builds the result table
from parsing that stdout. No number in the report is typed by hand.

Usage:  python tools/diagnostics/run_robust_matrix.py
Output: glm-toolkit/out/robust/<case>.log  (raw stdout)
        ROBUST-RESULT-2026-11-15.md        (table + FAIL details)
"""
import re
import subprocess
import sys
from datetime import datetime
from pathlib import Path

WS = Path(__file__).resolve().parents[3]
REPO = WS / "RinCast"
BENCH = REPO / "build" / "pc" / "bin" / "Release" / "encode-bench.exe"
OUT = WS / "glm-toolkit" / "out" / "robust"
REPORT = WS / "ROBUST-RESULT-2026-11-15.md"

PROFILES = [("0", "延迟优先"), ("1", "平衡"), ("2", "画质优先")]
BASE = ["--codec", "hevc", "--size", "3488x1744", "--fps", "90", "--mbps", "100", "--out", "NUL"]

# (id, label, ceiling Mbps, extra args)
SCENARIOS = [
    ("S8",  "低帧率 24fps",        100, ["--pace", "24", "--frames", "360", "--flat"]),
    ("S9",  "帧率抖动 60<->15",    100, ["--swing", "--frames", "600", "--flat"]),
    ("S10", "每120帧卡800ms",      100, ["--pace", "90", "--stall-every", "120:800", "--frames", "360", "--flat"]),
    ("S11", "长时间静止",          100, ["--pace", "90", "--still", "--frames", "360", "--flat"]),
    ("S12a", "上限极值 20",         20, ["--pace", "90", "--mbps", "20", "--frames", "300", "--synthetic"]),
    ("S12b", "上限极值 500",       500, ["--pace", "90", "--mbps", "500", "--frames", "300", "--synthetic"]),
    ("S13", "只有丢包没有队列",    100, ["--pace", "90", "--net", "--fb-every", "100", "--fb-queue", "0:-1",
                                        "--fb-loss", "0:0.03", "--frames", "300", "--flat"]),
]


def parse(text):
    d = {"wire": None, "fps": None, "peak": None, "setting": None,
         "encAvg": None, "encP95": None, "dips": 0, "brakes": 0,
         "reconf": 0, "errors": 0, "ratio": None}
    m = re.search(r"wire:\s+([\d.]+) Mbps at ([\d.]+) fps", text)
    if m:
        d["wire"], d["fps"] = float(m.group(1)), float(m.group(2))
    m = re.search(r"paced: peak 1s throughput ([\d.]+) Mbps against (\d+) Mbps setting", text)
    if m:
        d["peak"], d["setting"] = float(m.group(1)), int(m.group(2))
        d["ratio"] = d["peak"] / d["setting"] if d["setting"] else None
    m = re.search(r"encode ms: avg ([\d.]+)\s+p50 [\d.]+\s+p95 ([\d.]+)", text)
    if m:
        d["encAvg"], d["encP95"] = float(m.group(1)), float(m.group(2))
    d["dips"] = len(re.findall(r"guard dip", text))
    d["brakes"] = len(re.findall(r"loss brake", text))
    d["reconf"] = len(re.findall(r"fps measured", text))
    d["errors"] = len(re.findall(r"\[E\]|\bERROR\b", text))
    return d


def main():
    if not BENCH.exists():
        print("missing", BENCH)
        return 2
    OUT.mkdir(parents=True, exist_ok=True)
    rows, fails = [], []
    total = len(SCENARIOS) * len(PROFILES)
    n = 0
    for sid, label, ceiling, extra in SCENARIOS:
        for pid, pname in PROFILES:
            n += 1
            case = f"{sid}-p{pid}"
            cmd = [str(BENCH)] + BASE + extra + ["--profile", pid]
            print(f"[{n}/{total}] {case} {label}", flush=True)
            try:
                p = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
                text = p.stdout + p.stderr
                rc = p.returncode
            except subprocess.TimeoutExpired as e:
                text = (e.stdout or b"").decode(errors="replace") + "\nTIMEOUT 300s"
                rc = -9
            (OUT / f"{case}.log").write_text(text, encoding="utf-8", errors="replace")
            d = parse(text)
            ok_m1 = rc == 0 and d["errors"] == 0
            ok_m3 = d["ratio"] is None or d["ratio"] <= 1.5
            # The user's bitrate is a CEILING, not a target to average above. A short
            # burst may exceed it (M3a), but the 3-second mean must not (M3b).
            ok_m3b = d["wire"] is None or d["wire"] <= ceiling * 1.05
            ok = ok_m1 and ok_m3 and ok_m3b
            rows.append((sid, label, ceiling, pid, pname, d, rc, ok))
            if not ok:
                why = []
                if not ok_m1:
                    why.append(f"M1 退出码 {rc} / ERROR {d['errors']} 条")
                if not ok_m3:
                    why.append(f"M3a 峰值 {d['ratio']:.2f}x > 1.5x")
                if not ok_m3b:
                    why.append(f"M3b 平均 {d['wire']:.1f} Mbps 超过 {ceiling} Mbps 上限 "
                               f"({d['wire'] / ceiling * 100:.0f}%)")
                fails.append((case, "; ".join(why), text[-1200:]))
            print(f"    rc={rc} wire={d['wire']} Mbps peak={d['ratio']} dips={d['dips']} "
                  f"brakes={d['brakes']} reconf={d['reconf']} -> {'PASS' if ok else 'FAIL'}", flush=True)

    lines = [
        "# ROBUST-RESULT · 离线极端矩阵（B 组 S8–S13 × 三档）",
        "",
        f"> 自动生成于 {datetime.now().strftime('%Y-%m-%d %H:%M')}，由 `glm-toolkit/run_robust_matrix.py`"
        " 解析每条 bench 的原始 stdout 得到，**没有一个数字是手抄的**。",
        f"> 仓库 HEAD：`{subprocess.run(['git', '-C', str(REPO), 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True).stdout.strip()}`；"
        f" 二进制：`{BENCH.name}`。",
        "",
        "## 判据",
        "",
        "- **M1 不崩**：退出码 0，日志无 `[E]`/`ERROR`。",
        "- **M3a 不发散（瞬时）**：`paced: peak 1s throughput` / 用户上限 ≤ 1.5×（允许短突发）。",
        "- **M3b 不发散（平均）**：`wire` 平均吞吐 ≤ 上限 × 1.05。**用户设的是上限，不是可以平均超过的目标**；"
        " 这一条是 09:2x 首轮跑完才补的——首轮 1.5× 判据把 136% 的平均吞吐放过去了。",
        "- 未取到的字段一律写 `—`（bench 不报 QP p50、loss%、恢复秒数，不编造）。",
        "",
        "## 结果表",
        "",
        "| 场景 | 档位 | 上限 Mbps | wire Mbps | fps | 峰值/上限 | encode avg/p95 ms | guard dip | loss brake | NVENC 重配 | 结论 | 证据 |",
        "|---|---|---|---|---|---|---|---|---|---|---|---|",
    ]
    for sid, label, ceiling, pid, pname, d, rc, ok in rows:
        wire = f"{d['wire']:.1f}" if d["wire"] is not None else "—"
        fps = f"{d['fps']:.0f}" if d["fps"] is not None else "—"
        ratio = f"{d['ratio']:.2f}x" if d["ratio"] is not None else "—"
        enc = (f"{d['encAvg']:.2f}/{d['encP95']:.2f}"
               if d["encAvg"] is not None and d["encP95"] is not None else "—")
        lines.append(
            f"| {sid} {label} | {pid} {pname} | {ceiling} | {wire} | {fps} | {ratio} | {enc} | "
            f"{d['dips']} | {d['brakes']} | {d['reconf']} | {'PASS' if ok else 'FAIL'} | "
            f"`glm-toolkit/out/robust/{sid}-p{pid}.log` |")
    lines += ["", f"合计 **{len(rows)}** 组，PASS **{sum(1 for r in rows if r[7])}**，FAIL **{len(fails)}**。", ""]
    if fails:
        lines += ["## FAIL 明细", ""]
        for case, why, tail in fails:
            lines += [f"### {case} — {why}", "", "```", tail.strip(), "```", ""]
    else:
        lines += ["## FAIL 明细", "", "无。", ""]
    lines += [
        "## 边界与说明",
        "",
        "- `--flat` 是平滑渐变，内容极简，wire Mbps 接近 0 属预期；它用来量**帧率/重配/卡顿恢复**，不量画质。",
        "- `--synthetic` 是每帧全变的噪声，用来把码率顶到上限，检验码控不越界（S12）。",
        "- S13 用合成反馈把 `queueDelayMs` 设为 -1、丢包率 3%，走「头显不测队列」的兜底分支。",
        "- A 组（S1–S7）和 C 组（S14–S17）需要真实 SteamVR/头显会话，本轮未跑。",
        "",
    ]
    REPORT.write_text("\n".join(lines), encoding="utf-8")
    print("report:", REPORT)
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
