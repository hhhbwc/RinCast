# RinCast 诊断工具链

<!-- FORK-DOC: fork 专属工具文档（中文版）。工具已从工作区脚本脱敏后搬入。English: README.md -->

[English](README.md) | [中文](README.zh-CN.md)

一套离线、只读的工具，用来搞清楚这个分支的码率 / 发送节奏 / 延迟到底在干什么。
不装任何东西，不改产品代码，除非某个工具明确说了，否则不碰头显。
几乎全是 Python 3，只用标准库——不用 pip 装依赖。

## 这里有什么

| 文件 | 干什么用 |
|---|---|
| `analyze_dual.py` | 把 PC `streamer.log` 和头显 logcat 拉到同一条时间线上。 |
| `analyze_session.py` | 汇总单次会话：码率、帧率、QP。 |
| `analyze_wire.py` | 逐包的线上时序。 |
| `pacing_check.py` | 检查抓到的日志里的发送预算。 |
| `pacing_loop_sim.py` | 离线模拟发送循环。 |
| `run_extreme_matrix.py` | 51 组极端网络矩阵——回归门禁就是它。 |
| `run_robust_matrix.py` | 鲁棒性矩阵。 |
| `run_burst_matrix.py` | 突发整形矩阵。 |
| `verify_blindspot.py` | 复查早先矩阵标了 PASS、实际却没跑到的用例。 |
| `preflight.py` | 环境 / 设备就绪检查。 |
| `lint_output.py` | 检查交付文档符不符合证据契约。 |
| `make_brief.py`、`make_handoff.py`、`make_packages.py`、`build_target.py` | 工作区侧的报告 / 构建辅助脚本。 |
| `watchdog.py` | 盯着长时间跑的测试。 |
| `link_udp_test.py` | 到头显的 UDP 吞吐探测。 |
| `capture/collect-logs.bat` | 一键抓 PC 日志 + 头显 logcat。 |
| `capture/quick-check.py` / `.bat` | 短测后快速判断三个关键字段。会经 adb 抓头显 logcat。 |
| `build/pc/bin/Release/encode-bench.exe` | 不是 Python 脚本，是 C++ 构建产物（源码在 `tools/encode-bench/`，由 `pc/CMakeLists.txt` 编出）。`run_extreme_matrix.py` 硬依赖它，缺了就是退出码 2。 |
| `testdata/`（README.md + 两个样例日志） | 脱敏样例日志，供解析器自测。 |

关于 `analyze_dual.py --selftest` 多说一句：它用随附样例验证新格式解析。
旧格式回归项需要一份**没有**随本 fork 分发的证据日志，所以会打 SKIP，然后照样报
`SELFTEST PASS`（退出码 0）。旧格式解析在这里是没验证过的——那个 PASS 不覆盖它。

## 配置

设备专属值在发布前已换成占位符 / 环境变量，自己设一下：

| 变量 | 含义 | 示例 |
|---|---|---|
| `RINCAST_ADB_DEVICE` | 头显 adb 序列号 | 用 `adb devices` 查 |
| `RINCAST_HEADSET_IP` | 头显在你局域网里的 IP | `192.168.1.50` |
| `LOCALAPPDATA` | 仅 Windows；`RinCast/logs/streamer.log` 所在位置 | 系统自动设置 |

有个布局上的坑：7 个脚本从自身路径推导的是**工作区根**——也就是 clone 的*父目录*（`parents[3]`），
然后再拼固定的子目录名。clone 必须叫 `RinCast`，且同级要有一个 `glm-toolkit` 目录：
矩阵脚本往那里写输出，`build_target.py` 从那里导入辅助脚本。单独裸 clone 满足不了这个布局——
要么把同级目录建出来，要么改脚本里的 `WS` 常量。

输出在两个地方。逐案原始日志落在 `<workspace>/glm-toolkit/out/<矩阵名>/`（目录自己建，
或者把 `OUT` 常量指到别处）。每个矩阵还会写一份汇总报告——`ROBUST-EXTREME-2026-11-15.md` /
`ROBUST-RESULT-2026-11-15.md`，固定文件名——写到**工作区根**，每跑一次覆盖一次。

## 最小工作流

1. 先 configure 一次——`build/` 被 gitignore，全新 clone 没有构建树：
   `cmake -S pc -B build\pc -G "Visual Studio 17 2022" -A x64`
2. 在仓库根编 Release 目标：`cmake --build build/pc --config Release --parallel`
3. 单元测试：`build/pc/bin/Release/rincast-core-tests.exe`，期望 `ALL TESTS PASSED`。
4. 回归门禁：`python tools/diagnostics/run_extreme_matrix.py`
   退出码：0 = 过，1 = 有 REGRESSION，2 = 缺 `encode-bench.exe`。
   这只证明**离线**没回归，不证明画质。
5. 真机上玩 30–60 秒，然后跑 `tools/diagnostics/capture/collect-logs.bat`。
6. 分析抓到的两个文件：
   `python tools/diagnostics/analyze_dual.py --pc <抓取目录>/pc-streamer.log --hmd <抓取目录>/hmd-logcat.txt`
   （想要时间平移建议值就加 `--estimate-offset`）。

## 这些工具证明不了什么

离线矩阵全绿的意思是"已知用例没回归"，不是"画面稳了"。
码率呼吸是观感问题，只有真头显加人眼能判。
高负载下的已知限制写在 [`../../FORK-README.zh-CN.md`](../../FORK-README.zh-CN.md)。