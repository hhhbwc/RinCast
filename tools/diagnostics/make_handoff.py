#!/usr/bin/env python3
"""make_handoff.py - 生成一个「丢给执行 AI 就能读」的目录（带编号，按顺序读）。

    python glm-toolkit/make_handoff.py                     # 生成 <工作区根>/GLM-HANDOFF/
    python glm-toolkit/make_handoff.py --task glm-toolkit/tasks/TASK-WP1a.md
    python glm-toolkit/make_handoff.py --out D:/somewhere   # 换输出位置

每次生成会先跑 preflight，保证 03-CONTEXT.md 是当前实测值。
"""
import argparse
import re
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def clean(p):
    if not p.exists():
        return "> （缺失：%s）\n" % p.name
    s = p.read_text(encoding="utf-8", errors="replace")
    m = re.match(r"^---\s*\n[\s\S]*?\n---\s*\n", s)
    if m:
        s = s[m.end():]
    s = s.replace("<!-- lint: rule-doc -->", "")
    return re.sub(r"\n{3,}", "\n\n", s).strip() + "\n"


def readme(stamp, task_name, root):
    task_line = ("`04-TASK.md`（本轮：**%s**）" % task_name) if task_name \
        else "`04-TASK.md`（**还没有任务卡** → 拿不到就别开工）"
    return """# 00 · 先读我

> 生成 **%s**。你是执行 AI。**这个目录就是你需要知道的全部**，不要再翻长交接文档。
> 按文件编号顺序读，读完 01–04 再动手。

## 阅读顺序

| 顺序 | 文件 | 读它干什么 |
|---|---|---|
| 1 | `01-RULES.md` | 五步工作流、十条铁律、输出契约、质疑与偏离协议（L0/L1/L2） |
| 2 | `02-FACTS.md` | 事实表 F01–F30（每条带验证命令）、证据等级、反例 N1–N7 |
| 3 | `03-CONTEXT.md` | 刚才 preflight 的实测快照。**会过期**，与你的命令输出冲突时以命令为准 |
| 4 | %s | 本轮范围 / 验收 / 交付。**卡外的事不做** |
| 5 | `05-DEVIATIONS.md` | 偏离与质疑留痕（append-only）。改了就记，没记就打回 |

## 工作区

- 代码仓库：`%s/RinCast`（改前先 `git -C RinCast status --short`，应干净）
- 工具：`%s/preflight.py`（取证）、`%s/lint_output.py`（交稿门禁）
- 本目录 `01`–`04` 由主 AI 每次重新生成，**不要改**；你只往 `05-DEVIATIONS.md` 追加
- `05-DEVIATIONS.md` 里的 `D-001` 是格式示例、`D-002` 是主 AI 自查记录（供参考）；
  **你的第一条真实记录从 `D-003` 开始**，编号别复用

## 你的第一次回复

**只交 `FACT-RECAP.md`**：把 `04-TASK.md` 的「必读事实 ID」逐条复述，标 `已验证 / UNKNOWN / 冲突`。
不要动代码。等确认；若无人确认，在顶部写「自行复核，未获主 AI 确认」再继续。

## 交稿

```bash
python %s/lint_output.py HANDOFF.md FACT-RECAP.md patches/*.patch
```
必须 `PASS`；WARN 逐条写进 HANDOFF 的「未做 / UNKNOWN」。
HANDOFF 六节齐全：改动 / 不变量与边界 / 验证 / 证据等级 / 未做与 UNKNOWN / 回滚。
""" % (stamp, task_line, root, HERE, HERE, HERE)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--task", help="本轮任务卡 md 路径")
    ap.add_argument("--out", default=str(ROOT / "GLM-HANDOFF"))
    ap.add_argument("--no-preflight", action="store_true")
    a = ap.parse_args()

    if not a.no_preflight:
        try:
            r = subprocess.run([sys.executable, str(HERE / "preflight.py")],
                               capture_output=True, text=True, timeout=120)
            print((r.stdout or "").strip().splitlines()[0] if r.stdout else "preflight failed")
        except (subprocess.SubprocessError, OSError):
            print("preflight 跳过（异常）")

    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y-%m-%d %H:%M")

    task = Path(a.task) if a.task else None
    if task and not task.exists():
        print("ERROR: 任务卡不存在 %s" % task)
        return 2

    (out / "00-READ-ME-FIRST.md").write_text(
        readme(stamp, task.stem if task else "", ROOT), encoding="utf-8")
    (out / "01-RULES.md").write_text(clean(HERE / "SKILL.md"), encoding="utf-8")
    (out / "02-FACTS.md").write_text(clean(HERE / "GROUND-TRUTH.md"), encoding="utf-8")
    (out / "03-CONTEXT.md").write_text(
        "> 快照时间 %s（preflight）。任何 MISSING/MISMATCH 一律写 UNKNOWN。\n\n" % stamp
        + clean(HERE / "out" / "CONTEXT.md"), encoding="utf-8")

    if task:
        (out / "04-TASK.md").write_text(clean(task), encoding="utf-8")
    else:
        (out / "04-TASK.md").write_text(
            "# 04 · 本轮任务\n\n> ⚠️ 还没有任务卡。让主 AI 运行：\n"
            "> `python glm-toolkit/make_handoff.py --task glm-toolkit/tasks/TASK-WP1a.md`\n\n"
            "> 或把填好的卡直接覆盖本文件。没有任务卡不要开工。\n", encoding="utf-8")

    dev = HERE / "DEVIATIONS.md"
    if dev.exists():
        shutil.copyfile(dev, out / "05-DEVIATIONS.md")
    else:
        (out / "05-DEVIATIONS.md").write_text(
            "# DEVIATIONS\n\n> 有偏离就追加 `## D-001`，四要素：类型 / 查了什么+证据 / 决定 / 影响与回滚。\n",
            encoding="utf-8")

    print("handoff -> %s" % out)
    for p in sorted(out.glob("*.md")):
        print("  %-22s %6.1f KB" % (p.name, p.stat().st_size / 1024))
    print("总计 %.1f KB" % (sum(p.stat().st_size for p in out.glob("*.md")) / 1024))
    return 0


if __name__ == "__main__":
    sys.exit(main())
