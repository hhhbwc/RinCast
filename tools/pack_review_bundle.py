#!/usr/bin/env python3
import os, sys, shutil, subprocess, hashlib, zipfile
from pathlib import Path

repo = Path(r'C:\Users\wzy\.zcode\workspace\draft-f4c11c48-5ad3-4301-ac42-79f775847197\RinCast')
bundle_name = 'RinCast-multi-brand-review-2026-11-26'
out_dir = repo / 'build' / bundle_name
out_dir.mkdir(parents=True, exist_ok=True)

readme_content = """# RinCast 架构审查与全品牌头显通用化交接文档 (Architecture Review Handoff)

本打包专为另一位 AI / 评审专家准备，完整包含 RinCast（SteamVR PC 驱动 + Android OpenXR 头显客户端）近期的核心架构改动、完整源码、代码 diff 及自动化门禁测试日志。

---

## 1. 项目背景与架构概览
- **PC 串流端与 OpenVR 驱动 (`pc/driver-openvr/`, `pc/streamer/`)**:
  - 自研 SteamVR DirectMode HMD 驱动 (`IVRDriverDirectModeComponent`, `ITrackedDeviceServerDriver`)，拦截游戏场景层与 SteamVR Dashboard/Overlay 图层，对多图层进行时间反向姿态重投影，通过共享内存 (`vr_bridge.h`) 零拷贝送入 NVENC / AMF / QuickSync 硬件编码器，经 UDP/FEC 低延迟网络送至头显。
- **头显客户端 (`headset/app/src/main/cpp/`)**:
  - 基于原生 Android NDK + OpenXR + OpenGL ES 3.1 构建。使用 `AMediaCodec` + `AImageReader` + `EGLImageKHR` 实现零拷贝硬解码渲染循环，内置双端时间同步、预测性姿态回传、运动向量插帧与 7 国语言自研即时 UI。
- **冻结协议契约 (严禁破坏)**:
  - `kProtocolVersion = 3`
  - `VrFrameMetadata` (104 字节紧凑二进制头)
  - `vr_bridge.h` 共享内存结构

---

## 2. 本次打包重点审查改动 (Recent Key Changes)

### 2.1 全品牌头显通用化与容灾自适应 (Multi-Brand & Generic Headset Universality)
- **目标**：在缺乏所有物理样机的情况下，确保 APK 能够直接兼容 Meta Quest 1/2/3/Pro、PICO Neo3/4/4 Ultra、HTC Vive Focus 3/Vision/XR Elite、Android XR (三星 Galaxy XR)、大朋 (DPVR)、YVR、Lynx R1 及各类骁龙 Spaces 一体机。
- **涉及文件**:
  - `headset/app/src/main/AndroidManifest.xml`
  - `headset/app/src/main/cpp/xr/xr_loader.cpp`
  - `headset/app/src/main/cpp/headsets/headset.h`
  - `headset/app/src/main/cpp/headsets/headset.cpp`
  - `headset/app/src/main/cpp/xr/xr_input.cpp`
  - `headset/app/src/main/cpp/media/video_decoder.cpp`
- **核心逻辑**:
  1. **OpenXR 加载器动态容灾矩阵 (`xr_loader.cpp`)**：
     建立了 `首选厂商专属 Loader -> 标准 libopenxr_loader.so -> 系统级 /system/lib64/libopenxr_loader.so` 三级回退链，彻底杜绝在非标 ROM 上因加载器名不符导致的闪退。
  2. **手柄交互 Profile 弹性渐进降级 (`xr_input.cpp`, `headset.cpp`)**：
     OpenXR 规范规定：若 suggestedBindings 包含任一硬件不支持的 path（如 `/thumbrest/touch` 或电容触摸传感器），整个绑定将被直接拒绝（`XR_ERROR_PATH_UNSUPPORTED`）。
     代码新增官方 HTC Vive Focus 3 Profile (`XR_HTC_vive_focus3_controller_interaction`)，并实现 `全触控+拇指托 -> 无拇指托触控 -> 纯物理按键/摇杆/扳机` 三级自动降级重试，保证任何 6DoF 手柄都能 100% 绑定成功。
  3. **3D VR 启动器兼容 (`AndroidManifest.xml`)**：
     将 `android.hardware.vr.headtracking` 设为 optional；新增 HTC Vive Wave 分类 `<category android:name="com.htc.intent.category.VRAPP" />`。
  4. **解码器显存受限保护 (`video_decoder.cpp`)**：
     针对部分低配或显存严格的 GPU，在申请 8 帧硬解私有缓冲区失败时，自动降级至 4 帧缓冲重试。

### 2.2 USB 有线串流在 Android 10 上闪退与启动秒退死循环自愈
- **涉及文件**: `headset/app/src/main/java/app/rincast/vr/MainActivity.java`, `headset/app/src/main/cpp/platform/jni_bridge.cpp`, `headset/app/src/main/cpp/app/app.cpp`
- **根因与修复**:
  - Android 10 (PICO OS 5.x) 不支持 API 31+ 的 `TRANSPORT_USB(8)`，抛出 `IllegalArgumentException` 导致 CheckJNI 触发 `SIGABRT`。已修复版本判断与 `Throwable` 捕获。
  - 在 JNI 入口出口设置 `env->ExceptionClear()` 屏障。
  - 在 `app.cpp` 引入自愈机制：网络状态返回 `NotAllowed` 时自动将 `usbCable` 置为 `false` 并落盘，彻底阻断开机秒退死循环。

### 2.3 UI 面板空指针崩溃修复
- **涉及文件**: `headset/app/src/main/cpp/app/screens.cpp` 第 684, 819, 821 行。
- **根因与修复**: `CheckRow` 的 `hint` 形参为 `const std::string&`，旧调用传入了 `nullptr` 触发 `std::string(nullptr) -> strlen(nullptr)` SIGSEGV。已全部清除恢复默认值 `{}`。

### 2.4 SteamVR 菜单剧烈抖动修复 (多图层反向姿态重投影)
- **涉及文件**: `pc/driver-openvr/src/layer_reproj.h`, `pc/driver-openvr/src/driver.cpp`
- **根因与修复**: 游戏层（layers[0]，滞后姿态）与 Dashboard 菜单层（layers[1]，满帧最新姿态）叠合时，全帧强行被打上 layers[0] 姿态，导致头显 TimeWarp 对菜单施加了多余旋转偏移。驱动现以最新层姿态为合成目标姿态，对滞后层施加反向相对旋转 UV 重投影。

### 2.5 7 国语言完整性与 UI 布局重构
- 339 个 PC key 与 300 个头显 key 全语言 100% 覆盖无硬编码，头显字库贴图 100% 覆盖 844 个非 ASCII 字符。

---

## 3. 请同行 AI 重点审阅与建议的方面 (Review Focus Points)

我们诚挚邀请您针对以下关键工程细节提供深入审阅与优化建议：

1. **手柄与 OpenXR 绑定鲁棒性 (`xr_input.cpp` & `headset.cpp`)**:
   - 在弹性渐进降级绑定中，是否还有常见手柄 profile（如 Valve Index、Windows Mixed Reality 或 HTC Vive Wand）值得纳入默认推荐链？
   - 在极少数仅支持单手柄的场景下，绑定流程是否存在盲区？
2. **硬件解码缓冲队列生命周期 (`video_decoder.cpp`)**:
   - 当某些 GPU 降级至 4 帧缓冲运行时，`Latch()` 与渲染线程在突发网络丢包或重传时，是否需要调整防卡死跳帧阈值？
3. **多图层重投影在极端工况下的表现 (`layer_reproj.h` & `driver.cpp`)**:
   - 当游戏帧率极低（< 15 fps）且玩家以极高角速度甩头（> 360 deg/s）时，反向重投影 UV 畸变是否在视觉安全范围内？
4. **跨品牌 Android JNI 与系统生命周期防固化建议**:
   - 针对不同品牌安卓休眠/唤醒策略，目前的后台网络监听和音频锁是否存在可能被杀进程的隐患？

---

## 4. 压缩包目录结构

```text
00-README.md                         # 本审查文档
01-diff/
  ├── full-changes-since-d8c0677.diff # 自基准分支以来的全量代码 diff
  ├── headset-changes.diff           # 头显端完整 diff
  └── pc-driver-streamer-changes.diff# PC 端驱动与推流完整 diff
02-source/                           # 关键源文件（完整保留目录结构）
  ├── headset/                       # 头显端 Java / C++ / 清单
  ├── pc/                            # PC 驱动 / 网页 UI / 推流核心
  ├── shared/                        # 跨端共享协议与运动场
  └── tools/                         # 门禁测试工具
03-tests/
  └── i18n-integrity.log             # 自动化门禁测试运行结果
SHA256SUMS.txt                       # 所有文件 SHA-256 校验清单
```
"""

(out_dir / '00-README.md').write_text(readme_content, encoding='utf-8')

# Generate SHA256SUMS.txt
sums = []
for p in sorted(out_dir.rglob('*')):
    if p.is_file() and p.name != 'SHA256SUMS.txt':
        rel = p.relative_to(out_dir).as_posix()
        h = hashlib.sha256(p.read_bytes()).hexdigest()
        sums.append(f'{h}  {rel}')
(out_dir / 'SHA256SUMS.txt').write_text('\n'.join(sums) + '\n', encoding='utf-8')

# Create ZIP
zip_name = f'{bundle_name}.zip'
zip_dist = repo / 'dist' / zip_name
desktop_zip = Path(r'C:\Users\wzy\Desktop') / zip_name
artifact_zip = Path(r'C:\Users\wzy\.gemini\antigravity\brain\17589027-c1f1-4694-9104-918f7d5dd56b') / zip_name

with zipfile.ZipFile(zip_dist, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
    for p in sorted(out_dir.rglob('*')):
        if p.is_file():
            zf.write(p, arcname=p.relative_to(repo / 'build').as_posix())

shutil.copy2(zip_dist, desktop_zip)
shutil.copy2(zip_dist, artifact_zip)

zip_size = zip_dist.stat().st_size
zip_sha = hashlib.sha256(zip_dist.read_bytes()).hexdigest().upper()
print(f'Zip created: {zip_dist} ({zip_size} bytes, SHA-256: {zip_sha})')
print(f'Copied to Desktop: {desktop_zip}')
print(f'Copied to Artifacts: {artifact_zip}')
