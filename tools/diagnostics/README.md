# RinCast diagnostic toolchain

<!-- FORK-DOC: fork-specific tool doc. Tools were sanitized and moved in from workspace scripts. Chinese: README.zh-CN.md -->

[English](README.md) | [中文](README.zh-CN.md)

Offline, read-only tools for figuring out what the video bitrate / pacing / latency is doing on this fork.
They don't install anything, they don't modify product code, and they don't touch a headset unless a tool
explicitly says it does. Almost everything is Python 3, standard library only — no pip installs needed.

## What's here

| File | What it does |
|---|---|
| `analyze_dual.py` | Lines up PC `streamer.log` and headset logcat on one timeline. |
| `analyze_session.py` | Summarizes one session: bitrate, fps, QP. |
| `analyze_wire.py` | Per-datagram wire timing. |
| `pacing_check.py` | Checks the pacing budget in a captured log. |
| `pacing_loop_sim.py` | Offline simulation of the pacing loop. |
| `run_extreme_matrix.py` | The 51-case extreme network matrix — this is the regression gate. |
| `run_robust_matrix.py` | Robustness matrix. |
| `run_burst_matrix.py` | Burst-shaping matrix. |
| `verify_blindspot.py` | Re-checks cases an earlier matrix marked PASS but never actually exercised. |
| `preflight.py` | Environment / device readiness checks. |
| `lint_output.py` | Lints handoff docs for evidence-contract compliance. |
| `make_brief.py`, `make_handoff.py`, `make_packages.py`, `build_target.py` | Workspace-side report / build helpers. |
| `watchdog.py` | Watches a long-running test. |
| `link_udp_test.py` | UDP throughput probe to the headset. |
| `capture/collect-logs.bat` | One-click capture of the PC log + headset logcat. |
| `capture/quick-check.py` / `.bat` | Quick verdict on three key fields after a short test. Grabs headset logcat via adb. |
| `build/pc/bin/Release/encode-bench.exe` | Not a Python script — a C++ build product (source in `tools/encode-bench/`, built by `pc/CMakeLists.txt`). `run_extreme_matrix.py` hard-depends on it; missing means exit code 2. |
| `testdata/` (README.md + two sample logs) | Sanitized sample logs for the parsers' self-tests. |

A note on `analyze_dual.py --selftest`: it validates new-format parsing against the shipped samples.
The old-format regression check needs an evidence log that does **not** ship with this fork, so it prints
SKIP and still reports `SELFTEST PASS` (exit 0). Old-format parsing is unverified here — the PASS doesn't cover it.

## Configuration

Device-specific values were replaced with placeholders / environment variables before publishing.
Set your own:

| Variable | Meaning | Example |
|---|---|---|
| `RINCAST_ADB_DEVICE` | headset adb serial | find it with `adb devices` |
| `RINCAST_HEADSET_IP` | headset IP on your LAN | `192.168.1.50` |
| `LOCALAPPDATA` | Windows only; where `RinCast/logs/streamer.log` lives | set by Windows |

One layout gotcha: seven scripts derive the **workspace root** — the clone's *parent* directory, `parents[3]` —
from their own path, then expect fixed subdirectory names. The clone must be named `RinCast` and sit next to a
`glm-toolkit` directory: the matrix scripts write output there, and `build_target.py` imports a helper from there.
A bare standalone clone won't satisfy this — create the sibling directories or edit the `WS` constant in the scripts.

Output goes to two places. Per-case raw logs land in `<workspace>/glm-toolkit/out/<matrix>/` (create it, or point
the `OUT` constant somewhere else). Each matrix also writes a summary report — `ROBUST-EXTREME-2026-11-15.md` /
`ROBUST-RESULT-2026-11-15.md`, fixed filenames — to the **workspace root**, overwriting it every run.

## A minimal workflow

1. Configure once — `build/` is gitignored, so a fresh clone has no build tree:
   `cmake -S pc -B build\pc -G "Visual Studio 17 2022" -A x64`
2. Build Release targets from the repo root: `cmake --build build/pc --config Release --parallel`
3. Unit tests: `build/pc/bin/Release/rincast-core-tests.exe` — expect `ALL TESTS PASSED`.
4. Regression gate: `python tools/diagnostics/run_extreme_matrix.py`
   Exit codes: 0 = pass, 1 = REGRESSION found, 2 = `encode-bench.exe` missing.
   This proves nothing regressed **offline**. It does not prove image quality.
5. On a real setup: play 30–60 s, then run `tools/diagnostics/capture/collect-logs.bat`.
6. Analyze the two captured files:
   `python tools/diagnostics/analyze_dual.py --pc <capture-dir>/pc-streamer.log --hmd <capture-dir>/hmd-logcat.txt`
   (add `--estimate-offset` if you want a suggested time shift).

## What these tools cannot prove

An all-green offline matrix means "no regression against known test cases", not "the picture is stable".
Bitrate breathing is a perceived-quality problem; only a real headset and a human observer can judge it.
The known limitations under load are in [`../../FORK-README.md`](../../FORK-README.md).