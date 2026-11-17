#!/usr/bin/env python3
"""make_brief.py - 把工具包合并成「一个文件」，直接丢给执行 AI 读。

    python glm-toolkit/make_brief.py                     # 生成 GLM-BRIEF.md（含空白任务区）
    python glm-toolkit/make_brief.py --task my-task.md   # 把某张任务卡一起并进去
    python glm-toolkit/make_brief.py --out other.md

会先跑一遍 preflight（除非 --no-preflight），保证嵌进去的是当前实测值。
纯标准库，只写 glm-toolkit/GLM-BRIEF.md。
"""
import argparse
import re
import subprocess
import sys
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def clean(p):
    """去掉 YAML frontmatter 与 rule-doc 标记，降低噪音。"""
    if not p.exists():
        return "> （缺失：%s）\n" % p.name
    s = p.read_text(encoding="utf-8", errors="replace")
    m = re.match(r"^---\s*\n[\s\S]*?\n---\s*\n", s)
    if m:
        s = s[m.end():]
    s = s.replace("<!-- lint: rule-doc -->", "")
    return re.sub(r"\n{3,}", "\n\n", s).strip() + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--task", help="要并入的任务卡 md 路径")
    ap.add_argument("--out", default=None,
                   help="默认 GLM-BRIEF.md；带 --task 时默认 GLM-BRIEF-<任务名>.md")
    ap.add_argument("--no-preflight", action="store_true")
    a = ap.parse_args()

    if not a.no_preflight:
        try:
            r = subprocess.run([sys.executable, str(HERE / "preflight.py")],
                               capture_output=True, text=True, timeout=120)
            print((r.stdout or "").strip().splitlines()[0] if r.stdout else "preflight failed")
        except (subprocess.SubprocessError, OSError):
            print("preflight 跳过（异常）")

    stamp = datetime.now().strftime("%Y-%m-%d %H:%M")
    task = Path(a.task) if a.task else None
    if task and not task.exists():
        print("ERROR: 任务卡不存在 %s" % task)
        return 2

    L = [
        "# RinCast 执行简报（单文件版）",
        "",
        "<!-- lint: rule-doc -->",
        "",
        "> 生成 **%s**。这份文件是**给你（执行 AI）的唯一入口**，四部分按顺序读："
        % stamp,
        "> ① 工作流与铁律 ② 事实与反例 ③ 最新实测快照 ④ 本轮任务。",
        "> 与任何长交接文档冲突时，**以本文第③部分为准**，并把冲突写进 HANDOFF。",
        "> 你只有两种交付：`FACT-RECAP.md`（先复述）+ 补丁/HANDOFF。先复述，再动代码。",
        "",
        "---",
        "",
        "## 目录",
        "",
        "- ① 工作流、十条铁律、输出契约、质疑与偏离协议 → 本文件第 1 节",
        "- ② 事实表（F01–F30）、证据等级、六条反例 → 第 2 节",
        "- ③ 本次 preflight 实测快照 → 第 3 节",
        "- ④ 本轮任务卡（范围/验收/交付） → 第 4 节",
        "",
        "---",
        "",
    ]

    if a.no_preflight:
        snap = ["> ⚠️ 本次用了 `--no-preflight`，下面是**旧快照**；开工前必须自己重跑 "
                "`python glm-toolkit/preflight.py`。"]
    else:
        snap = ["> 这是刚才 preflight 的实测结果。**它会过期**（设备在线状态、安装包、日志都会变）；",
                "> 若与你的命令输出不符，以命令输出为准，并在 HANDOFF 写明冲突。"]
    snap.append("> 任何 `MISSING` / `MISMATCH` 一律写 `UNKNOWN`，不要补猜。")

    L += ["# ① 工作流与铁律", "", clean(HERE / "SKILL.md"), "", "---", "",
          "# ② 事实与反例", "", clean(HERE / "GROUND-TRUTH.md"), "", "---", "",
          "# ③ 最新实测快照（preflight）", ""] + snap + [
          "", clean(HERE / "out" / "CONTEXT.md"), "", "---", "",
          "# ④ 本轮任务", ""]

    if task:
        L += ["> 来源：`%s`" % task.name, "", clean(task)]
    else:
        L += [
            "> ⚠️ **这份 BRIEF 还没有任务卡。** 生成时加 `--task <卡片路径>`，",
            "> 或把填好的卡直接粘到本节下方。没有任务卡就**不要开工**。",
            "",
            "模板见第 ⑤ 节；主 AI 每次只给一张卡、一个新会话。",
        ]

    tc = clean(HERE / "TASK-CARD.md")
    if task:
        # 已带真实任务卡时，去掉模板里的样例，避免同一张卡出现两次
        tc = re.split(r"\n## 2\. 填好的样例", tc)[0] + \
            "\n> （样例已省略：本轮真实任务卡见第 ④ 节。）\n"

    L += ["", "---", "", "# ⑤ 任务卡模板（照这个颗粒度；本轮以第 ④ 节为准）", "",
          tc, "", "---", "",
          "# ⑥ 交稿前最后一遍", "",
          "1. `FACT-RECAP.md` 已交、必读事实逐条标了 `已验证 / UNKNOWN / 冲突`？",
          "2. 只改了任务卡列出的文件？改动用 `git diff` / `.patch` 交付，没有整文件重写？",
          "3. 有偏离/质疑 → `glm-toolkit/DEVIATIONS.md` 里 `D-xxx` 四要素齐全，HANDOFF 顶部引用了编号？",
          "   没有 → HANDOFF 写了「偏离记录: 无」？",
          "4. `python glm-toolkit/lint_output.py <你的产出>` 返回 PASS？WARN 逐条解释了？",
          "5. HANDOFF 六节齐全：改动 / 不变量与边界 / 验证 / 证据等级 / 未做与 UNKNOWN / 回滚？",
          ""]

    if a.out:
        out = Path(a.out)
    elif task:
        stem = re.sub(r"^TASK-", "", task.stem, flags=re.I)
        out = HERE / ("GLM-BRIEF-%s.md" % re.sub(r"[^\w.-]", "_", stem))
    else:
        out = HERE / "GLM-BRIEF.md"
    out.write_text("\n".join(L), encoding="utf-8")
    kb = len(out.read_text(encoding="utf-8").encode("utf-8")) / 1024
    print("brief -> %s (%.1f KB)%s" % (out, kb, "" if task else "  [无任务卡]"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
