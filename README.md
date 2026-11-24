# RinCast — hhhbwc 优化分支

这是 [TheReal-Flo/RinCast](https://github.com/TheReal-Flo/RinCast) 的 `next/capped-vbr` 分支，面向 Windows PC 串流到 Quest / PICO 头显。上游的桌面、PC VR、输入、音频、发现和账户功能继续保留；本分支主要优化视频码控、自动画质、帧生成、延迟诊断、稳定性和安装体验。

## 相对原项目的优化

- **码控与自动画质**：在 capped-VBR 基础上加入头显队列反馈刹车，按链路实际情况动态调整发送速率；画质档位改为自动调节，可向下压缩并在链路恢复后逐步回升，画质变化尽量在现有 VR 流上完成，减少重连和卡顿。
- **帧生成**：过滤静止画面中的块匹配噪声，只采用整幅画面一致的运动；修正运动时间戳和 discontinuities 处理。异步运动读back、cadence-budget 等实验默认关闭，仍需真机 A/B 验证。
- **延迟与可观测性**：补充游戏侧提交节奏、编码各阶段耗时、头显解码与显示延迟、队列和丢包等 1 Hz 双端日志，便于把“卡顿”定位到具体阶段，而不是只看总延迟。
- **稳定性与安全**：加固握手、FEC 重组、发现、控制消息、端口映射、设置读取和编码器异常路径；修复多个空指针、竞态、冻结和退出等待问题。
- **安装体验**：修复防火墙规则查询、作用域和失败重试问题；Setup 默认按当前用户安装，不再自动请求管理员权限。

## 安装

1. 从 [Releases](../../releases) 下载 `RinCast-<version>-Setup.exe` 和 `RinCast-<version>.apk`。
2. 直接运行 Setup，无需“以管理员身份运行”。默认安装到 `%LOCALAPPDATA%\Programs\RinCast`。
3. 用 `adb install -r RinCast-<version>.apk` 或 SideQuest 安装头显端。
4. 打开头显端 RinCast，选择 PC；首次连接时按 Windows 提示允许网络访问。

按用户安装时，系统级防火墙规则、全局 OpenXR 运行时和可选音频驱动不会自动修改，留待后续安装路径处理。SteamVR 驱动仍按当前用户注册。

## 当前验证边界

- 设置页已在真实 Chrome / CDP 下做过多视口布局与控件回归。
- PICO 4 已进行安装包哈希回读和 A104 真机验证；无线性能与画质结论仍以真机 A/B 为准。
- 默认关闭的实验功能不应被当作已验证优化。

## 构建

```powershell
scripts\build-all.ps1
```

详细构建、协议和架构说明见 [docs/BUILDING.md](docs/BUILDING.md)、[docs/PROTOCOL.md](docs/PROTOCOL.md) 和 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 许可证

沿用上游 [MIT 许可证](LICENSE)；`pc/audio-driver/src` 的 Microsoft Public License 例外和第三方声明保持不变。
