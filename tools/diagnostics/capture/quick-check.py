#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ROUND15 快速验证：抓两端日志 -> 只查 3 个关键点 -> 打印结论。
用法: double-click quick-check.bat  (or: python quick-check.py)
不修改任何产品文件，只读 + 复制日志。
"""
import os, re, subprocess, sys, glob, shutil, tempfile

PC_LOG = os.path.join(os.environ.get("LOCALAPPDATA", ""), "RinCast", "logs", "streamer.log")
HERE = os.path.dirname(os.path.abspath(__file__))

def find_device():
    try:
        out = subprocess.run(["adb", "devices"], capture_output=True, text=True, timeout=20).stdout
    except Exception as e:
        return None, f"adb 不可用: {e}"
    for line in out.splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "device":
            return parts[0], None
    return None, "没有在线的 adb 设备（插 USB 或 adb connect <ip:5555>）"

def grab():
    tmp = tempfile.mkdtemp(prefix="rc-check-")
    pc = os.path.join(tmp, "pc.log")
    hmd = os.path.join(tmp, "hmd.log")
    if os.path.exists(PC_LOG):
        shutil.copy2(PC_LOG, pc)
    else:
        pc = None
    dev, err = find_device()
    if not dev:
        return pc, None, err, tmp
    r = subprocess.run(["adb", "-s", dev, "logcat", "-d", "-s", "RinCast"],
                       capture_output=True, text=True, errors="replace", timeout=120)
    with open(hmd, "w", encoding="utf-8") as f:
        f.write(r.stdout)
    return pc, hmd, None, tmp

def main():
    print("=" * 62)
    print(" ROUND15 快速验证（只查 3 个关键点）")
    print("=" * 62)
    pc, hmd, err, tmp = grab()

    # 1) A: rc diag 1s 是否有值
    print("\n[1] A 修复：rc diag 1s 的 fps/qp/尺寸")
    if not pc:
        print("    ! 没找到 PC 日志（程序没启动过？）")
    else:
        txt = open(pc, encoding="utf-8", errors="replace").read()
        diag = re.findall(r"^.*rc diag 1s: game ([\d.]+)/s enc ([\d.]+)/s drop ([\d.]+)/s.*?qp ([\d.-]+), (\d+)x(\d+)", txt, re.M)
        total = len(diag)
        good = [d for d in diag if float(d[1]) > 0 and float(d[3]) >= 0 and int(d[4]) > 0]
        print(f"    总条数 {total}，其中有值（enc>0 且 qp>=0 且尺寸>0）的 {len(good)} 条")
        if total == 0:
            print("    ✗ 一条都没有：程序没串流，或装的不是新包")
        elif good:
            d = good[-1]
            print(f"    最后一条样本: enc {d[1]}/s, qp {d[3]}, {d[4]}x{d[5]}")
            print("    ✓ A 修复生效（字段有值了）")
        else:
            print("    ✗ 全是 0/-1/0x0：A 的修复没生效（还是旧 exe？）")

    # 2) B: 平衡档 QP 是否被托住
    print("\n[2] B 修复：平衡档 QP 下限（应 ≥16）")
    if pc:
        txt = open(pc, encoding="utf-8", errors="replace").read()
        # 找最近的 balanced 档起点
        marks = [(m.start(), m.group(1)) for m in re.finditer(r"VR stream: fixed bitrate, ([\w-]+), ceiling", txt)]
        bal_start = None
        for pos, prof in marks:
            if prof == "balanced":
                bal_start = pos
        seg_txt = txt[bal_start:] if bal_start is not None else txt
        qs = [float(m.group(1)) for m in re.finditer(r"ENC 5s: [\d.]+ fps, qp ([\d.]+),", seg_txt)]
        ranges = [(float(m.group(1)), float(m.group(2))) for m in re.finditer(r"qpRange ([\d.]+)-([\d.]+)", seg_txt)]
        if not qs:
            print("    - 平衡档没采到 ENC 5s 样本（测得太短？切一次档至少要 6 秒）")
        else:
            lo = min(r[0] for r in ranges) if ranges else min(qs)
            print(f"    平衡档 QP: 均值 {sum(qs)/len(qs):.1f}，5s 内最低 {lo:.0f}，样本 {len(qs)} 条")
            if lo >= 15.5:
                print("    ✓ 下限守住了（≥16）")
            else:
                print("    ✗ 仍有低于 16 的样本：B 没生效，或还在旧包")

    # 3) 头显侧
    print("\n[3] 头显日志")
    if err:
        print(f"    ! {err}")
    elif hmd:
        t = open(hmd, encoding="utf-8", errors="replace").read()
        n = len(re.findall(r"rc vr rx 1s:", t))
        d = len(re.findall(r"rc vr diag 1s:", t))
        print(f"    rc vr rx 1s: {n} 条，rc vr diag 1s: {d} 条")
        print("    ✓ 头显日志正常" if n > 0 else "    ✗ 没抓到：APK 没装对，或没连上串流")

    print("\n" + "=" * 62)
    print(" 原始日志留在: " + tmp)
    print(" 若要我深看，把上面这个路径发我即可。")
    print("=" * 62)
    return 0

if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.TimeoutExpired:
        print("\n! adb 超时（无线 adb 很慢）。建议插 USB 后重试，或稍后再跑。")
        sys.exit(1)
