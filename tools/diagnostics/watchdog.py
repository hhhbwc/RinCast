#!/usr/bin/env python3
"""Watchdog for the GLM hand-off.

Writes a heartbeat, and shouts when the executor goes quiet. The point is that a
quota-limited model can stop mid-plan without saying so; this makes the stop
visible within minutes instead of hours.

    python glm-toolkit/watchdog.py                # run in the foreground
    python glm-toolkit/watchdog.py --stall-min 12  # change the silence threshold

It watches, every 30 s:
  * PROGRESS.md          (the executor appends one line per finished task)
  * RinCast git HEAD and the number of dirty files
  * the newest mtime anywhere under RinCast/pc, RinCast/shared, RinCast/tools

Any change resets the idle timer. When the timer passes --stall-min it appends ONE
STALL line to PROGRESS.md and writes glm-toolkit/out/TAKEOVER.flag, which is the
signal for the main AI to take over that task number.

Never deletes anything; never touches the repo.
"""

import argparse
import datetime
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO = ROOT / "RinCast"
OUT = Path(__file__).resolve().parent / "out"
PROGRESS = ROOT / "PROGRESS.md"
FLAG = OUT / "TAKEOVER.flag"
LOG = OUT / "WATCH.log"
PID = OUT / "WATCH.pid"

WATCH_DIRS = ["pc", "shared", "tools"]


def now():
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def git(args):
    try:
        r = subprocess.run(["git", "-C", str(REPO)] + args, capture_output=True,
                           text=True, timeout=20, encoding="utf-8", errors="replace")
        return (r.stdout or "").strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def newest_mtime():
    newest, where = 0.0, ""
    for d in WATCH_DIRS:
        base = REPO / d
        if not base.is_dir():
            continue
        for p in base.rglob("*"):
            if not p.is_file() or p.suffix.lower() in (".obj", ".pdb", ".log"):
                continue
            try:
                m = p.stat().st_mtime
            except OSError:
                continue
            if m > newest:
                newest, where = m, str(p.relative_to(REPO))
    return newest, where


def progress_state():
    if not PROGRESS.exists():
        return 0, "", "PROGRESS.md 不存在"
    st = PROGRESS.stat()
    last = ""
    try:
        lines = [l.strip() for l in PROGRESS.read_text(encoding="utf-8", errors="replace").splitlines() if l.strip()]
        last = lines[-1][:120] if lines else ""
    except OSError:
        pass
    return st.st_size, last, ""


def snapshot():
    mtime, where = newest_mtime()
    size, last, missing = progress_state()
    return {
        "progress_size": size,
        "progress_last": last,
        "progress_missing": missing,
        "head": git(["rev-parse", "--short", "HEAD"]),
        "dirty": len([l for l in git(["status", "--porcelain"]).splitlines() if l.strip()]),
        "newest_mtime": mtime,
        "newest_file": where,
    }


def append_progress(line):
    with PROGRESS.open("a", encoding="utf-8") as f:
        f.write(line.rstrip() + "\n")


def log(line):
    OUT.mkdir(parents=True, exist_ok=True)
    with LOG.open("a", encoding="utf-8") as f:
        f.write("%s %s\n" % (now(), line))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stall-min", type=float, default=12.0, help="silence before shouting")
    ap.add_argument("--interval", type=float, default=30.0, help="poll period, seconds")
    ap.add_argument("--once", action="store_true", help="one snapshot and exit (for testing)")
    a = ap.parse_args()

    OUT.mkdir(parents=True, exist_ok=True)
    if a.once:
        print(json.dumps(snapshot(), ensure_ascii=False, indent=2))
        return 0

    PID.write_text(str(os.getpid()), encoding="utf-8")
    log("watchdog start pid=%d stall=%.0fmin interval=%.0fs" % (os.getpid(), a.stall_min, a.interval))

    prev = snapshot()
    idle_since = time.time()
    last_beat = 0.0
    shouted = False
    try:
        while True:
            time.sleep(a.interval)
            cur = snapshot()
            changed = (cur["progress_size"] != prev["progress_size"]
                       or cur["head"] != prev["head"]
                       or cur["dirty"] != prev["dirty"]
                       or cur["newest_mtime"] != prev["newest_mtime"])
            if changed:
                idle_since = time.time()
                if shouted:
                    log("activity resumed: %s" % cur["newest_file"])
                    append_progress("- [%s] 恢复活动（看门狗）" % now())
                    shouted = False
                    if FLAG.exists():
                        FLAG.unlink()
                prev = cur
            idle = (time.time() - idle_since) / 60.0
            if time.time() - last_beat > 300:
                log("idle %.1f min | head=%s dirty=%d newest=%s | last=%s"
                    % (idle, cur["head"], cur["dirty"], cur["newest_file"], cur["progress_last"][:70]))
                last_beat = time.time()
            if idle >= a.stall_min and not shouted:
                msg = ("- [%s] **STALL** 已 %.0f 分钟没有任何变化（head=%s dirty=%d，最后一条：%s）"
                       " → 主 AI 接手" % (now(), idle, cur["head"], cur["dirty"],
                                          cur["progress_last"][:80] or "（无）"))
                append_progress(msg)
                FLAG.write_text(json.dumps({"at": now(), "idle_min": round(idle, 1), **cur},
                                           ensure_ascii=False, indent=2), encoding="utf-8")
                log("STALL after %.1f min -> TAKEOVER.flag" % idle)
                shouted = True
    except KeyboardInterrupt:
        log("watchdog stop (Ctrl-C)")
    finally:
        try:
            PID.unlink()
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
