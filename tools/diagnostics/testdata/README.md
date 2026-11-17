# testdata · analyze_dual.py 的测试样例

| 文件 | 内容 | 来源 |
|---|---|---|
| `pc-new-format-sample.log` | 9 行手工样例：`rc diag 1s`、带 qpRange/kbRange 的 `ENC 5s`、带 effective/wireCap/loss 的 `throughput`、带 arrivalGap/lockWait 的 `VR frames`、`VR stream:` 锚点 | 按 `pc/streamer/src/stream/vr_stream.cpp`、`nvenc_encoder.cpp`、`video_sender.cpp` 当前格式字符串手工构造（2026-11-17），字段值人工核定 |
| `hmd-new-format-sample.logcat` | 5 行手工样例：`rc vr rx 1s`、`rc vr diag 1s`（含 stages JSON）、`VR frame flow`、`VR prediction`、`VR frame arrival` | 按 `headset/app/src/main/cpp/net/stream_client.cpp:475`、`app/app.cpp:2159` 当前格式构造 |
| （运行时引用）`../ROUND9-EVIDENCE-2026-11-16/streamer.log` | 旧格式冻结证据（19.7 KB，无新字段），`--selftest` 用它验证"不崩 + 优雅降级 + 旧字段数字与 FACT-RECAP-ROUND9 一致" | 主 AI 冻结，未复制（避免重复 19 KB） |

用法：`python tools/diagnostics/analyze_dual.py --selftest`
