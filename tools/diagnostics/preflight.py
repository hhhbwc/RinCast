#!/usr/bin/env python3
"""preflight.py - 开工前自动抽取当前事实，生成短上下文供执行 AI 阅读。

纯标准库，无网络、无管理员权限、不改任何源码。
    python glm-toolkit/preflight.py [--root <工作区根>] [--no-net]

产出:
    glm-toolkit/out/context.json   机器可读
    glm-toolkit/out/CONTEXT.md     <=3KB, 直接贴给执行 AI
"""
import argparse
import json
import os
import re
import subprocess
import sys
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEF_ROOT = HERE.parent


def txt(p):
    try:
        return Path(p).read_text(encoding="utf-8", errors="replace")
    except (OSError, ValueError):
        return None


def n(p, pattern, flags=0):
    s = txt(p)
    if s is None:
        return None
    return len(re.findall(pattern, s, flags))


def has(p, pattern, flags=0):
    c = n(p, pattern, flags)
    return None if c is None else c > 0


def run(cmd, timeout=12, enc="utf-8"):
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=timeout, shell=True)
        return (r.stdout or b"").decode(enc, errors="replace").strip()
    except (subprocess.SubprocessError, OSError):
        return ""


def file_version(p, timeout=8):
    try:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "(Get-Item '%s').VersionInfo.FileVersion" % str(p).replace("'", "''")],
            capture_output=True, timeout=timeout)
        return (r.stdout or b"").decode("utf-8", errors="replace").strip()
    except (subprocess.SubprocessError, OSError):
        return ""


def run_ping(ip, timeout=8):
    """中文 Windows 的 ping 输出是 GBK；且 '0% 丢失' 可能是收到 ICMP unreachable，
    只有 TTL= 才是真的通。"""
    try:
        r = subprocess.run(["ping", "-n", "2", ip], capture_output=True, timeout=timeout)
        return (r.stdout or b"").decode("gbk", errors="replace")
    except (subprocess.SubprocessError, OSError):
        return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(DEF_ROOT))
    ap.add_argument("--no-net", action="store_true", help="跳过 ping / adb 探测")
    a = ap.parse_args()

    R = Path(a.root).resolve()
    repo = R / "RinCast"
    mv = R / "measure-vd"
    logs = Path(os.environ.get("RINCAST_CAPTURE_DIR", "C:/tmp/rincast-test-20261114"))
    # Prefer the live PC log (what the streamer is writing now): the archived session
    # folder goes stale the moment a new test runs. Fall back only when the live log
    # has no VR stream in it yet (fresh start, nothing streamed).
    # LOCALAPPDATA is not always exported in this shell, so try the usual roots too.
    _roots = [os.environ.get("LOCALAPPDATA", ""), str(Path.home() / "AppData" / "Local")]
    _cands = [Path(r) / "RinCast" / "logs" / "streamer.log" for r in _roots if r]
    slog = logs / "streamer.log"
    for c in _cands:
        if c.exists() and "VR stream:" in (txt(c) or ""):
            slog = c
            break
    lcat = logs / "live-logcat.txt"
    vdcfg = Path("C:/ProgramData/Virtual Desktop/StreamerSettings.json")
    pccfg = Path(os.environ.get("LOCALAPPDATA", "")) / "RinCast" / "settings.json"
    cs = repo / "pc/streamer/src/session/client_session.cpp"
    prof = repo / "shared/include/rc/stream_profile.h"
    fov = repo / "pc/streamer/src/stream/foveator.cpp"
    pcs = repo / "pc/streamer/src/settings.h"
    hss = repo / "headset/app/src/main/cpp/app/settings.h"

    C = []  # (id, claim, value, level, status, verify)

    def add(i, claim, value, level, status, verify):
        C.append(dict(id=i, claim=claim, value=value, level=level,
                      status=status, verify=verify))

    def st(ok, missing=False):
        if missing:
            return "MISSING"
        return "OK" if ok else "MISMATCH"

    # ---- A. 仓库与现场 ----
    head = run('git -C "%s" rev-parse --short HEAD' % repo)
    branch = run('git -C "%s" branch --show-current' % repo)
    dirty = run('git -C "%s" status --short' % repo)
    add("F01", "仓库 HEAD / 分支 / 是否干净",
        {"head": head or "UNKNOWN", "branch": branch or "UNKNOWN",
         "dirty_entries": len(dirty.splitlines()) if dirty else 0},
        "SRC", "OK" if head else "MISSING",
        'git -C RinCast rev-parse --short HEAD; git -C RinCast status --short')

    exe = Path("C:/Program Files/RinCast/RinCast Streamer.exe")
    if exe.exists():
        try:
            stt = exe.stat()
            add("F02", "已安装 Streamer 的时间/大小/版本（是否 HEAD 产物需另比 sha）",
                {"mtime": datetime.fromtimestamp(stt.st_mtime).strftime("%Y-%m-%d %H:%M"),
                 "size_mb": round(stt.st_size / 1e6, 1),
                 "file_version": file_version(exe) or "UNKNOWN"},
                "CFG", "OK",
                "Get-Item 'C:\\Program Files\\RinCast\\RinCast Streamer.exe' | % VersionInfo,LastWriteTime; Get-FileHash")
        except OSError:
            add("F02", "已安装 Streamer 状态", "stat failed", "CFG", "MISSING", "")
    else:
        add("F02", "已安装 Streamer（未找到安装路径）", str(exe), "CFG", "MISSING", "")

    cfg = txt(pccfg)
    live = {}
    if cfg:
        try:
            j = json.loads(cfg)
            for k in ("tenBit", "adaptiveBitrate", "streamProfile",
                      "vrBitrateMbps", "desktopBitrateMbps", "foveatedEncoding",
                      "twoPassEncoding", "adaptiveQuantization", "codec"):
                live[k] = j.get(k, "<absent>")
        except ValueError:
            live = {"<parse>": "INVALID JSON"}
    add("F03", "用户现场 settings.json（PC 端键；upscaling/frameGeneration 在头显侧，见 F04）",
        live or "UNKNOWN: 未找到 " + str(pccfg), "CFG",
        "OK" if cfg else "MISSING",
        'powershell "Get-Content $env:LOCALAPPDATA\\RinCast\\settings.json -Raw | ConvertFrom-Json"')

    defaults = {}
    for p, keys in ((hss, ["vrScale", "gazeFoveation", "upscaling", "frameGeneration"]),
                    (pcs, ["tenBit", "foveatedEncoding", "adaptiveBitrate",
                           "twoPassEncoding", "adaptiveQuantization", "codec"])):
        s = txt(p) or ""
        for k in keys:
            m = re.search(r"\b%s\s*=\s*([^;]+);" % k, s)
            defaults[k] = m.group(1).strip() if m else "<not found>"
    add("F04", "源码默认值（≠ 现场值）", defaults, "SRC",
        "OK" if txt(pcs) or txt(hss) else "MISSING",
        'grep -n "= true\\|= false\\|0.75" RinCast/pc/streamer/src/settings.h RinCast/headset/app/src/main/cpp/app/settings.h')

    # ---- B. 画质链路 ----
    # 不依赖 grep：直接扫源码树（跳过 .git / build / dist，避免遍历海量对象）
    found_pc = []
    # os.walk 而不是 rglob：gradle 的 intermediates 目录会被构建并发删除，
    # rglob 在 scandir 与下钻之间撞上 TOCTOU 直接抛 FileNotFoundError（WinError 3）。
    # 先按目录名剪枝（不下钻 build/dist/.git/out），再逐文件容错读取。
    for dirpath, dirnames, filenames in os.walk(repo):
        dirnames[:] = [d for d in dirnames
                       if d not in (".git", "build", "dist", "out")]
        rel = os.path.relpath(dirpath, repo)
        if rel == ".":
            rel = ""
        for name in filenames:
            if not name.lower().endswith((".cpp", ".h", ".hpp", ".c", ".js", ".py")):
                continue
            full = Path(dirpath) / name
            try:
                if "pc.tenBit" in full.read_text(encoding="utf-8", errors="replace"):
                    found_pc.append((Path(rel) / name).as_posix())
            except OSError:
                pass
    cst = txt(cs)
    st_ten = None if cst is None else {
        "m_settings.tenBit": len(re.findall(r"m_settings\.tenBit", cst)),
        "cfg.tenBit = st.tenBit": len(re.findall(r"cfg\.tenBit\s*=\s*st\.tenBit", cst)),
    }
    add("F06", "ChooseCodec 用头显下发 tenBit；PC 开关是死开关",
        {"client_session.cpp": st_ten, "pc.tenBit 命中文件": found_pc},
        "SRC", "OK" if cst and not found_pc else ("MISSING" if not cst else "MISMATCH"),
        'grep -n "tenBit" RinCast/pc/streamer/src/session/client_session.cpp; grep -rn "pc.tenBit" RinCast/')

    ten = n(slog, r"10-bit") if slog.exists() else None
    add("F07", "真机日志里 10-bit 出现次数（>0 表示早就在跑 Main10）",
        ten, "LOG", st(ten is not None and ten > 0, ten is None),
        'grep -c "10-bit" $LOCALAPPDATA/RinCast/logs/streamer.log')

    eye = n(slog, r"2688x2688 per eye") if slog.exists() else None
    add("F08", "真机 2688x2688 per eye 次数（>0 表示现场 vrScale=1.0）",
        eye, "LOG", st(eye is not None and eye > 0, eye is None),
        'grep -c "2688x2688 per eye" $LOCALAPPDATA/RinCast/logs/streamer.log')

    fs = txt(fov)
    formula_ok = bool(fs and re.search(
        r"\*\s*\(\s*\w+\s*\+\s*\(1(?:\.0)?\s*-\s*\w+\)\s*\*\s*0\.5\s*\)", fs))
    add("F10", "Foveator::EncodedSize 公式仍在（数字表前提）",
        {"formula_present": formula_ok,
         "table": {"0.6": 2144, "0.8": 2416, "0.9": 2560, "1.0": 2688}},
        "SRC", st(formula_ok, fs is None),
        'sed -n "58,70p" RinCast/pc/streamer/src/stream/foveator.cpp')

    # 三种 decode 语义完全不同，必须分开取，别用一个正则混着算：
    #   VR prediction 里的 "decode 8.2"  = 解码耗时（要的就是这个）
    #   "slowest decode 16.2 ms"         = 该窗口内最慢一帧，不是均值
    #   "decoded 1.1 ms before the latch"= 解码早于 latch 的提前量，也不是耗时
    def stats(v):
        if not v:
            return None
        v = sorted(v)
        n = len(v)
        return {"n": n, "mean": round(sum(v) / n, 2), "p50": v[n // 2],
                "p90": v[int(n * 0.9)], "max": v[-1],
                "over_11_1ms": sum(1 for x in v if x > 11.1)}

    dec, slow, lead = [], [], []
    if lcat.exists():
        ls = txt(lcat) or ""
        for line in ls.splitlines():
            if "VR prediction:" in line:
                m = re.search(r"\bdecode\s+([0-9]+\.?[0-9]*)", line)
                if m:
                    dec.append(float(m.group(1)))
        slow = [float(x) for x in re.findall(r"slowest decode\s+([0-9]+\.?[0-9]*)\s*ms", ls)]
        lead = [float(x) for x in re.findall(
            r"decoded\s+(-?[0-9]+\.?[0-9]*)\s*ms before the latch", ls)]

    add("F12", "真机解码耗时（90fps 预算 11.1ms）；三块语义不同，别混用",
        {"prediction_decode": stats(dec), "slowest_decode": stats(slow),
         "latch_lead_ms": stats(lead),
         "note": "prediction_decode=每 5s 一条的解码耗时；slowest=窗口内最慢帧；lead=提前量（越大越好）"},
        "LOG", "OK" if dec else "MISSING",
        "grep 'VR prediction:' <RINCAST_CAPTURE_DIR>/live-logcat.txt | "
        "grep -o 'decode [0-9.]*'   （不要裸 grep decode，会混进 slowest/decoded）")

    # ---- C. 码控 ----
    ps = txt(prof)
    qmin = None
    if ps:
        # struct 里的 minQp = 20 是默认值；要看 Quality case 的 return 第 5 个字段
        m = re.search(r"case\s+StreamProfile::Quality\s*:[\s\S]{0,700}?return\s*\{([^}]*)\}", ps)
        if m:
            fields = [x.strip() for x in m.group(1).split(",")]
            qmin = int(fields[4]) if len(fields) >= 5 and fields[4].isdigit() else "<parse fail>"
        else:
            qmin = "<Quality case not found>"
    add("F14", "Quality 档 minQp（0 = 已取消 QP 下限）",
        {"quality_minQp": qmin}, "SRC", st(qmin == 0, ps is None),
        'grep -n "Quality\\|minQp" RinCast/shared/include/rc/stream_profile.h')

    ad = n(slog, r"adaptive bitrate") if slog.exists() else None
    fx = n(slog, r"fixed bitrate") if slog.exists() else None
    add("F15", "真机日志 adaptive / fixed bitrate 次数",
        {"adaptive": ad, "fixed": fx}, "LOG", st(ad == 0, ad is None),
        'grep -c "adaptive bitrate" $LOCALAPPDATA/RinCast/logs/streamer.log; grep -c "fixed bitrate" ...')

    dips = n(slog, r"guard dip") if slog.exists() else None
    climbs = n(slog, r"guard climb") if slog.exists() else None
    add("F16", "guard dip / climb 事件总数（真机 06:43 后 12 次 dip 伴随丢包，不是启动假 dip）",
        {"dip": dips, "climb": climbs}, "LOG",
        "OK" if dips is not None else "MISSING",
        'grep -c "guard dip" $LOCALAPPDATA/RinCast/logs/streamer.log; grep -c "guard climb" ...')

    # ---- D. VD 侧 ----
    vs = txt(vdcfg)
    vinfo = {}
    if vs:
        try:
            vj = json.loads(vs)
            vinfo = {"keys": sorted(vj.keys())[:20],
                     "has_bitrate_key": any("bitrate" in k.lower() for k in vj),
                     "FoveaSize": vj.get("FoveaSize"),
                     "CodecName": vj.get("CodecName", "<absent>"),
                     "PreferredCodec": vj.get("PreferredCodec", "<absent>")}
        except ValueError:
            vinfo = {"<parse>": "INVALID JSON"}
    add("F21", "VD StreamerSettings：无码率字段 / FoveaSize 语义 UNK",
        vinfo or "UNKNOWN", "CFG+UNK", "OK" if vs else "MISSING",
        'type "C:\\ProgramData\\Virtual Desktop\\StreamerSettings.json"')

    # ---- E. 工具边界 ----
    pp = mv / "profile-paired.py"
    pp_s = txt(pp)
    hardcoded = bool(pp_s and "C:/tmp/rincast-test-20261114" in pp_s)
    rin_only = bool(pp_s and ("VR prediction" in pp_s or "RinCast" in pp_s))
    add("F26", "profile-paired.py 硬编码路径 / 只吃 RinCast 日志 → 不能量 VD",
        {"hardcoded_path": hardcoded, "rinCast_only": rin_only},
        "SRC", st(hardcoded and rin_only, pp_s is None),
        'head -35 measure-vd/profile-paired.py')

    tools = {p: (mv / p).exists() for p in
             ["vd_capture.py", "vd_headset_dump.py", "analyze_pcap.py",
              "profile-paired.py", "README.md"]}
    add("F28", "measure-vd 工具文件存在性", tools, "SRC",
        "OK" if all(tools.values()) else "MISMATCH",
        'ls -1 measure-vd/')

    # ---- F. 设备（可跳过） ----
    if not a.no_net:
        ping = run_ping(os.environ.get("RINCAST_HEADSET_IP", "192.168.1.100"))
        online = "TTL=" in ping
        unreachable = "无法访问目标" in ping
        adb = run("adb devices", timeout=8)
        add("F27", "PICO 4 在线 / 无线 adb 是否可用",
            {"ping_online": online, "icmp_unreachable": unreachable,
             "adb_wireless": (os.environ.get("RINCAST_HEADSET_IP", "192.168.1.100") + ":5555") in adb and "device" in adb},
            "CFG", "OK", "ping -n 2 <headset-ip> (须有 TTL=); adb devices")
    else:
        add("F27", "PICO 4 在线 / 无线 adb（本次跳过 --no-net）", "SKIPPED",
            "CFG", "MISSING", "ping -n 2 <headset-ip>; adb devices")

    # ---- 输出 ----
    out = HERE / "out"
    out.mkdir(exist_ok=True)
    meta = {"generated": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "root": str(R), "count": len(C),
            "ok": sum(1 for c in C if c["status"] == "OK"),
            "mismatch": sum(1 for c in C if c["status"] == "MISMATCH"),
            "missing": sum(1 for c in C if c["status"] == "MISSING")}
    (out / "context.json").write_text(
        json.dumps({"meta": meta, "facts": C}, ensure_ascii=False, indent=2),
        encoding="utf-8")

    L = ["# CONTEXT - 开工前事实快照", "",
         "> 生成 %s | 来源 `preflight.py`，**不是**长文档摘要。" % meta["generated"],
         "> 与长交接文档冲突时以本表为准并报告冲突。事实 ID 见 `GROUND-TRUTH.md`。", "",
         "| ID | 断言 | 实测值 | 等级 | 状态 |", "|---|---|---|---|---|"]
    for c in C:
        v = json.dumps(c["value"], ensure_ascii=False)
        if len(v) > 150:
            v = v[:147] + "..."
        claim = c["claim"].replace("|", "/")
        L.append("| %s | %s | `%s` | %s | %s |" %
                 (c["id"], claim, v, c["level"], c["status"]))
    L += ["", "OK %d / MISMATCH %d / MISSING %d" %
          (meta["ok"], meta["mismatch"], meta["missing"]), "",
          "## 复核命令（引用任何事实前先跑一次）", ""]
    for c in C:
        L.append("- **%s** `%s`" % (c["id"], c["verify"].replace("`", "'")))
    L += ["", "## 规则", "",
          "1. 本表是**事实**，`GROUND-TRUTH.md` 是**解释**，长文档只是背景。",
          "2. `MISSING` / `MISMATCH` 的项一律写 `UNKNOWN`，禁止补猜。",
          "3. 引用工具前读源码；拟新增标识符必须标「当前仓库不存在」。",
          "4. 每条「实测/已验证」的句子必须带出处与验证命令。", ""]
    (out / "CONTEXT.md").write_text("\n".join(L), encoding="utf-8")

    print("preflight: OK=%d MISMATCH=%d MISSING=%d  -> %s" %
          (meta["ok"], meta["mismatch"], meta["missing"], out / "CONTEXT.md"))
    for c in C:
        if c["status"] != "OK":
            print("  %-5s %-8s %s" % (c["id"], c["status"], c["claim"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
