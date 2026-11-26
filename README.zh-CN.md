# RinCast

[English](README.md) | [简体中文](README.zh-CN.md)

> **项目独立迁移与重构说明**  
> 本仓库最初作为 [TheReal-Flo/RinCast](https://github.com/TheReal-Flo/RinCast) 的优化 Fork 进行开发。随着对视频传输层、SteamVR 驱动管线、跨品牌 OpenXR 运行时等等的深入改造，累计变更已超过 150+ 个文件、35,000+ 行代码。如此庞大且涉及底层协议与架构的改动，已远远超出了向原项目提交 PR 的合理范畴。本项目已正式转为单独的开源项目进行维护。  
> 
> **核心改进**：
> - **画质优化**：改良了动态码率与编码调度，优化了画面锐化算法，画面细节更丰富、更清晰细腻。
> - **延迟降低**：优化了网络数据包发送策略与调度平滑度，有效降低端到端串流延迟，大幅减少卡顿。
> - **Bug 修复与稳定性增强**：修复了 SteamVR 多图层画面撕裂与重影、多品牌手柄键位与追踪兼容性异常、多语言本地化显示等多项已知 Bug。
> - **更老旧 VR 设备适配尝试**：尝试对更老的 VR 设备进行兼容适配（试验性支持，未充分实机测试）。

---

RinCast 是一款免费、开源的 VR 串流工具，支持在 Meta Quest（3 / 3S / Pro / 2）以及 PICO（4 / 4 Pro / 4 Ultra）头显上畅玩 Windows PC 上的 SteamVR 游戏，或将电脑桌面投射到虚拟空间中使用。无任何订阅费用，无需在应用商店付费购买。

- **虚拟桌面**：支持将电脑的每个显示器作为独立的浮动屏幕显示（支持曲面或平面），支持手柄控制与虚拟键盘输入。
- **PC VR 游戏**：在 SteamVR 中注册为头显和控制器，可直接在头显内启动 Steam 上的 VR 游戏。
- **硬件编码**：支持 NVENC（N卡）、AMF（A卡）或 Media Foundation 的 H.264、HEVC（H.265）及 AV1 硬件编码加速。
- **私密安全**：串流数据全部端到端加密，传输流量仅在头显与 PC 之间直接建立连接。
- **本地免登录**：同一局域网内的电脑会自动发现并显示在设备列表中；跨网络连接时才需要使用可选的账号服务。

## 运行要求

- **操作系统**：Windows 10 或 Windows 11 64 位系统，配备支持视频硬件编码的 NVIDIA、AMD 或 Intel 独立显卡。
- **运行库**：[Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/)（Win11 及大部分 Win10 已自带）。
- **VR 设备支持**：
  - **Meta Quest 系列**：Quest 3、Quest 3S、Quest Pro、Quest 2。安装前需开启[开发者模式](https://developers.meta.com/horizon/documentation/native/android/mobile-device-setup/)。Quest 1 需使用专用包 `RinCast-<版本号>-quest1.apk`（未实机测试）。
  - **PICO 系列**：PICO 4、PICO 4 Pro、PICO 4 Ultra。开启方法：在头显内打开 **设置 > 通用 > 关于本机**，连续点击“软件版本号”7 次开启开发者选项，进入 **开发者 > USB 调试** 开启调试权限。
  - **其他试验性设备（未实机测试）**：
    - **Samsung Galaxy XR**：使用标准版安装包，开启开发者模式与 USB 调试，并在应用请求时授权手势追踪。
    - **Steam Frame**：提供原生源码构建支持，详见 [headset/linux](headset/linux/README.md)。
- **游戏运行组件**：Steam 以及 SteamVR。
- **推荐网络环境**：PC 使用千兆有线网线连接路由器，头显连接 5GHz 或 6GHz（Wi-Fi 6 / 6E）高速无线网络。

## 快速上手

1. 在仓库的 [Releases 发布页面](../../releases) 下载最新版的安装程序 `RinCast-<版本号>-Setup.exe` 与头显端安装包 `RinCast.apk`。
2. 在电脑上运行 `RinCast-<版本号>-Setup.exe` 完成安装（初次安装会弹出管理员权限申请）。
3. 将 APK 安装至头显：使用终端命令 `adb install -r RinCast.apk`，或使用 SideQuest 等第三方安装工具将 APK 侧载安装到头显中。
4. 戴上头显，在应用库中启动 **RinCast**（Quest 用户可在“未知来源”列表中找到）。
5. 在头显内找到你的 PC 并点击连接，首次连接时在电脑端弹出的提示框中点击 **允许 (Allow)**。

> **防火墙提示**：安装程序会自动添加专用网络与域网络的防火墙放行规则。如果你的 Windows 当前网络被识别为“公用网络”，RinCast Streamer 首页会给出提示：建议将 Windows 网络属性更改为“专用网络”，或在 Streamer 设置的“通用”中勾选“允许公用网络上的头显连接”。

## 音频设置

RinCast 支持将电脑声音串流至头显，并将头显麦克风声音传输回电脑。Streamer 中的 **音频设备** 设置提供三种工作模式：

| 模式 | 适用场景 | 说明 |
|---|---|---|
| **Steam 串流设备** | 默认推荐 | 通过 Steam 自带的虚拟音频驱动传输电脑声音与麦克风（"Steam Streaming Speakers" / "Steam Streaming Microphone"）。 |
| **RinCast 独立驱动** | 仅限已签名驱动 | 需已安装 RinCast 虚拟音频驱动（"RinCast Speaker" / "RinCast Microphone"）。 |
| **系统默认设备** | 免虚拟音频设备 | 直接捕获电脑当前的默认音频输出设备（仅支持下行声音，不支持麦克风回传）。 |

## 有线连接 (USB 串流)

在 Quest 或 PICO 头显上，可以使用 USB-C 数据线（推荐 USB 3.0 数据线）替代 Wi-Fi 串流，彻底避免无线信道干扰：

1. 头显端进入设置开启 USB 串流连接选项，并使用数据线将头显插入电脑。
2. 头显弹出连接权限请求时选择允许。
3. 电脑端 RinCast 会识别到有线网络连接，头显内的电脑列表将显示 "USB cable"，点击即可进入。

## PICO 体感追踪器 (PICO Motion Trackers)

使用 PICO 设备时，在串流设置中开启 **VR > PICO 体感追踪器**，RinCast 会自动将绑在脚踝上的体感追踪数据转发至 SteamVR 中，在 SteamVR 里直接识别为标准的 Vive Trackers 追踪器。

## 虚拟局域网 (VPN / 异地组网)

如果你在电脑与头显上同时配置了 Tailscale、WireGuard 或其他异地组网工具，无需注册账号也可跨网直连：
1. 电脑与头显同时开启 VPN。
2. 电脑端 RinCast 首页会显示该虚拟网卡的 IP 地址。
3. 头显端选择“手动输入 PC IP”，填入该地址即可发起连接。

## 跨公网远程串流

在电脑端 Streamer 与头显端同时登录同一个 RinCast 账号。
头显端会显示一组临时配对码，在电脑或手机上确认后即可建立配对。中继服务器仅协助两端打洞握手，串流音视频数据将通过 IPv6 或 UDP 打洞在两端之间直接传输。

## 源码编译

详细编译指南请参阅 [docs/BUILDING.md](docs/BUILDING.md)。简要编译命令如下：

```powershell
scripts\build-all.ps1           # 自动编译全部组件并打包至 dist\ 目录
```

## 核心组件与文档索引

- [docs/FEATURES.md](docs/FEATURES.md)：功能清单与支持状态
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)：系统架构设计
- [docs/PROTOCOL.md](docs/PROTOCOL.md)：传输协议规范
- [docs/BUILDING.md](docs/BUILDING.md)：工具链与编译指南

| 组件模块 | 源码路径 | 功能说明 |
|---|---|---|
| RinCast Streamer | `pc/streamer` | Windows 托盘程序与 WebView2 界面：屏幕采集、硬件编码、输入输出、设备配对与 SteamVR 控制。 |
| SteamVR 驱动 | `pc/driver-openvr` | SteamVR OpenVR 驱动插件，注册头显显示屏与控制器。 |
| 虚拟音频驱动 | `pc/audio-driver` | 可选的 Windows 内核级虚拟扬声器与麦克风音频驱动。 |
| 头显客户端 | `headset` | Quest 与 PICO 适用的原生 OpenXR Android 客户端程序。 |
| 账号调度服务 | `server` | 基于 Go 语言的账号、配对码与节点打洞中继服务。 |
| rincast-core 核心库 | `shared` | PC 与头显共用的网络协议、加密、FEC 前向纠错与自发现核心逻辑。 |

## 开源协议

RinCast 整体遵循 [MIT 开源协议](LICENSE)。  
其中 `pc/audio-driver/src` 源自微软示例代码，遵循 Microsoft Public License。其余第三方开源库遵循其各自协议，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 免责声明

RinCast 为独立开源项目，与 Meta、Oculus、PICO、ByteDance（字节跳动）、Virtual Desktop Inc.、Valve 或 Microsoft 等公司均无附属或合作关系。文中出现的相关商标与产品名称仅用于技术兼容性说明。
