#!/usr/bin/env python3
import os, sys, hashlib, shutil
from pathlib import Path

repo = Path(r'C:\Users\wzy\.zcode\workspace\draft-f4c11c48-5ad3-4301-ac42-79f775847197\RinCast')
desktop = Path(r'C:\Users\wzy\Desktop')
dist = repo / 'dist'
artifact_dir = Path(r'C:\Users\wzy\.gemini\antigravity\brain\17589027-c1f1-4694-9104-918f7d5dd56b')

files_to_embed = [
    ('headset/app/src/main/cpp/xr/xr_input.cpp', 'cpp', '头显端 6DoF 控制器绑定与三级降级自适应'),
    ('headset/app/src/main/cpp/xr/xr_loader.cpp', 'cpp', '头显端 OpenXR 运行时加载器三级容灾回退'),
    ('headset/app/src/main/cpp/headsets/headset.h', 'cpp', '多品牌头显抽象基类定义与 Profile'),
    ('headset/app/src/main/cpp/headsets/headset.cpp', 'cpp', '多品牌头显工厂与通用 Controller 列表'),
    ('headset/app/src/main/cpp/media/video_decoder.cpp', 'cpp', 'AImageReader 显存缓冲降级与硬解流水线'),
    ('headset/app/src/main/java/app/rincast/vr/MainActivity.java', 'java', 'Android 10/11 USB Tethering 兼容'),
    ('headset/app/src/main/cpp/platform/jni_bridge.cpp', 'cpp', 'JNI 异常清除屏障保护'),
    ('headset/app/src/main/cpp/app/app.cpp', 'cpp', 'USB 网络自愈防开机死循环与主业务循环'),
    ('headset/app/src/main/cpp/app/screens.cpp', 'cpp', 'UI 设置面板 (已修复 nullptr 崩溃)'),
    ('headset/app/src/main/AndroidManifest.xml', 'xml', '全品牌 3D Launcher 与权限清单'),
    ('pc/driver-openvr/src/layer_reproj.h', 'cpp', 'SteamVR 菜单抖动修复：多图层反向姿态重投影纯数学'),
    ('pc/driver-openvr/src/driver.cpp', 'cpp', 'SteamVR DirectMode 驱动主实现与图层重投影合成'),
    ('shared/include/rc/protocol.h', 'cpp', '跨端协议与数据包线格式定义 (冻结契约)')
]

out_md = dist / 'RinCast-Source-Review-ALL-IN-ONE.md'

with open(out_md, 'w', encoding='utf-8') as out:
    out.write('# RinCast 全量核心源代码展开版 (供 Web 端 AI 直接审阅)\n\n')
    out.write('> 本文件专为 Web 端 AI（ChatGPT、Claude、Kimi 等）设计，已将所有近期重构与跨品牌通用化改造的**完整源代码正文、逐行代码、架构逻辑**全部平铺展开在单个文件中，直接上传本文件即可让 Web 端 AI 进行全文代码审查与诊断。\n\n')
    out.write('---\n\n')
    out.write('## 目录索引\n\n')
    for rel, lang, desc in files_to_embed:
        anchor = rel.replace('/', '').replace('.', '')
        out.write(f'- [{rel}](#{anchor}) ：{desc}\n')
    out.write('\n---\n\n')

    for rel, lang, desc in files_to_embed:
        fpath = repo / rel
        if not fpath.exists():
            continue
        content = fpath.read_text(encoding='utf-8', errors='replace')
        lines = content.splitlines()
        anchor = rel.replace('/', '').replace('.', '')
        out.write(f'<a id="{anchor}"></a>\n\n')
        out.write(f'## 源码文件：`{rel}`\n')
        out.write(f'**功能说明**：{desc}（共 {len(lines)} 行，SHA-256: `{hashlib.sha256(content.encode("utf-8")).hexdigest()}`）\n\n')
        out.write(f'```{lang}\n')
        out.write(content)
        if not content.endswith('\n'):
            out.write('\n')
        out.write('```\n\n---\n\n')

# Copy to Desktop & artifacts
desktop_md = desktop / 'RinCast-Source-Review-ALL-IN-ONE.md'
artifact_md = artifact_dir / 'RinCast-Source-Review-ALL-IN-ONE.md'
desktop_txt = desktop / 'RinCast-Source-Review-ALL-IN-ONE.txt'

shutil.copy2(out_md, desktop_md)
shutil.copy2(out_md, artifact_md)
shutil.copy2(out_md, desktop_txt)

sz = out_md.stat().st_size
print(f'Generated ALL-IN-ONE: {out_md} ({sz} bytes)')
print(f'Copied to Desktop: {desktop_md}')
print(f'Copied to Desktop (.txt): {desktop_txt}')
