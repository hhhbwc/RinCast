#!/usr/bin/env python3
"""analyze_dual.py — RinCast 双端日志分段对齐分析工具（ROUND12/ROUND13 A-01）。

用法：
  python analyze_dual.py --pc <streamer.log> [--hmd <logcat.txt>] [--offset-ms N]
                         [--estimate-offset] [--all-seconds] [--json out.json]
                         [--hmd-year 2026] [--selftest]

只读解析：不修改任何文件；字段缺失给 None；解析失败计数。
报告中的"疑似…"均为启发式提示，不是结论。
"""
import argparse
import datetime as dt
import json
import re
import sys
from collections import defaultdict

PC_TS = re.compile(r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}) (INFO|WARN|ERROR|DEBUG)\s+(.*)$")
HMD_TS = re.compile(r"^(\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}) \d+ \d+ [VDIWE] (\S+)\s*: (.*)$")


def parse_pc_line(line):
    m = PC_TS.match(line)
    if not m:
        return None
    try:
        ts = dt.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S")
    except ValueError:
        return None
    return ts, m.group(3)


def parse_hmd_line(line, year):
    m = HMD_TS.match(line)
    if not m:
        return None
    try:
        ts = dt.datetime.strptime(year + "-" + m.group(1), "%Y-%m-%d %H:%M:%S.%f")
    except ValueError:
        return None
    return ts, m.group(3)


# ---------------------------------------------------------------- 行模式（PC）
RE_DIAG1S = re.compile(
    r"rc diag 1s: game ([\d.]+)/s enc ([\d.]+)/s drop ([\d.]+)/s, halfRate (\w+), target (\d+) kbps, "
    r"wire ([\d.]+) Mbps, queue ([\d.-]+) ms, qp ([\d.-]+), (\d+)x(\d+)")
RE_ENC5S = re.compile(
    r"ENC 5s: (\d+) fps, qp ([\d.]+), ([\d.]+) KB/frame, ([\d.]+) Mbps \(ceiling (\d+)\), "
    r"submit ([\d.]+) ms, gpu wait ([\d.]+) ms"
    r"(?:, qpValid (\d+)/(\d+), qpFirstBad (\d+), qpRange (\d+)-(\d+), kbRange ([\d.]+)-([\d.]+))?")
RE_VRFRAMES = re.compile(
    r"VR frames: ([\d.]+)/s from the game, ([\d.]+)/s encoded; ([\d.]+)/s replaced before conversion, "
    r"([\d.]+)/s while the encoder was busy; conversion ([\d.]+) ms, encoding ([\d.]+) ms, "
    r"held for the encoder ([\d.]+) ms"
    r"(?:, arrivalGap (\d+)-(\d+) ms, lockWait avg ([\d.]+)/max ([\d.]+) ms \((\d+)\))?")
RE_THROUGHPUT = re.compile(
    r"Stream (\d+) throughput: ([\d.]+) Mbps actual, (\d+) fps, target (\d+) kbps"
    r"(?:, (\d+) datagrams refused)?"
    r"(?:, effective (\d+) kbps, wireCap (\d+) kbps, queue ([\d.-]+) ms, loss ([\d.]+)%)?")
RE_PROFILE = re.compile(
    r"VR stream: (adaptive|fixed) bitrate, (latency-first|balanced|quality-first), "
    r"ceiling (\d+) kbps, (capped VBR|CBR) \(QP floor ([\w./]+)\)")
RE_RES = re.compile(r"VR stream: (\d+)x(\d+) per eye(?:, encoding (\d+)x(\d+) per eye)?")


# ---------------------------------------------------------------- 行模式（头显）
RE_RX1S = re.compile(
    r"rc vr rx 1s: ([\d.]+) Mbps in, loss ([\d.]+)% \((\d+) frags lost of (\d+), FEC recovered (\d+)\), "
    r"(\d+) frames lost/s, queue ([\d.]+) ms, decode ([\d.]+) ms, net spread ([\d.]+) ms, "
    r"total latency ([\d.]+) ms")
RE_VRDIAG = re.compile(
    r"rc vr diag 1s: (-?\d+) shown, (\d+) repeated, (\d+) late of (\d+) displays \(flow window\), "
    r"(\d+) generated; latency ([\d.]+) ms, pipeline ([\d.]+) ms, decode ([\d.]+) ms, rtt ([\d.]+) ms, stages (.*)")
RE_FRAMEFLOW = re.compile(r"VR frame flow over (\d+) displays: (.*)")
RE_PREDICTION = re.compile(r"VR prediction: (\d+) ms past.*?, (\d+) ms from.*?; (.*)")
RE_ARRIVAL = re.compile(r"VR frame arrival: (.*)")


def _f(v):
    return None if v is None else float(v)


def _i(v):
    return None if v is None else int(v)


# ---------------------------------------------------------------- 解析层（T1）
def parse_pc_log(path):
    records, total = [], 0
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            total += 1
            line = line.rstrip("\r\n")
            parsed = parse_pc_line(line)
            if not parsed:
                continue
            ts, msg = parsed
            m = RE_DIAG1S.search(msg)
            if m:
                records.append((ts, "diag1s", {
                    "gameFps": _f(m.group(1)), "encFps": _f(m.group(2)), "dropFps": _f(m.group(3)),
                    "halfRate": m.group(4), "targetKbps": _i(m.group(5)), "wireMbps": _f(m.group(6)),
                    "queueMs": _f(m.group(7)), "qp": _f(m.group(8)),
                    "encW": _i(m.group(9)), "encH": _i(m.group(10))}))
                continue
            m = RE_ENC5S.search(msg)
            if m:
                records.append((ts, "enc5s", {
                    "fps": _f(m.group(1)), "qp": _f(m.group(2)), "kbFrame": _f(m.group(3)),
                    "mbps": _f(m.group(4)), "ceilingKbps": _i(m.group(5)),
                    "submitMs": _f(m.group(6)), "gpuWaitMs": _f(m.group(7)),
                    "qpValid": _i(m.group(8)), "qpTotal": _i(m.group(9)), "qpFirstBad": _i(m.group(10)),
                    "qpMin": _i(m.group(11)), "qpMax": _i(m.group(12)),
                    "kbMin": _f(m.group(13)), "kbMax": _f(m.group(14))}))
                continue
            m = RE_VRFRAMES.search(msg)
            if m:
                records.append((ts, "vrframes", {
                    "gameFps": _f(m.group(1)), "encFps": _f(m.group(2)),
                    "replacedConv": _f(m.group(3)), "replacedBusy": _f(m.group(4)),
                    "convMs": _f(m.group(5)), "encMs": _f(m.group(6)), "heldMs": _f(m.group(7)),
                    "gapMinMs": _i(m.group(8)), "gapMaxMs": _i(m.group(9)),
                    "lockAvgMs": _f(m.group(10)), "lockMaxMs": _f(m.group(11)), "lockN": _i(m.group(12))}))
                continue
            m = RE_THROUGHPUT.search(msg)
            if m:
                records.append((ts, "throughput", {
                    "stream": _i(m.group(1)), "mbps": _f(m.group(2)), "fps": _f(m.group(3)),
                    "targetKbps": _i(m.group(4)), "refused": _i(m.group(5)),
                    "effectiveKbps": _i(m.group(6)), "wireCapKbps": _i(m.group(7)),
                    "queueMs": _f(m.group(8)), "lossPct": _f(m.group(9))}))
                continue
            m = RE_PROFILE.search(msg)
            if m:
                records.append((ts, "profile_switch", {
                    "rateMode": m.group(1), "profile": m.group(2), "ceilingKbps": _i(m.group(3)),
                    "rcMode": m.group(4), "qpFloor": m.group(5)}))
                continue
            m = RE_RES.search(msg)
            if m:
                records.append((ts, "res_line", {
                    "eyeW": _i(m.group(1)), "eyeH": _i(m.group(2)),
                    "encW": _i(m.group(3)), "encH": _i(m.group(4))}))
                continue
            records.append((ts, "other", {"raw": msg}))
    return records, {"path": path, "totalLines": total}


def parse_hmd_logcat(path, year=None):
    records, total = [], 0
    if year is None:
        year = str(dt.datetime.now().year)
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            total += 1
            line = line.rstrip("\r\n")
            parsed = parse_hmd_line(line, year)
            if not parsed:
                continue
            ts, msg = parsed
            m = RE_RX1S.search(msg)
            if m:
                records.append((ts, "rx1s", {
                    "mbpsIn": _f(m.group(1)), "lossPct": _f(m.group(2)),
                    "fragsLost": _i(m.group(3)), "fragTotal": _i(m.group(4)), "fecRecovered": _i(m.group(5)),
                    "framesLostPerS": _i(m.group(6)), "queueMs": _f(m.group(7)),
                    "decodeMs": _f(m.group(8)), "netSpreadMs": _f(m.group(9)), "totalLatencyMs": _f(m.group(10))}))
                continue
            m = RE_VRDIAG.search(msg)
            if m:
                records.append((ts, "vrdiag1s", {
                    "shown": _i(m.group(1)), "repeated": _i(m.group(2)), "late": _i(m.group(3)),
                    "displays": _i(m.group(4)), "generated": _i(m.group(5)),
                    "latencyMs": _f(m.group(6)), "pipelineMs": _f(m.group(7)),
                    "decodeMs": _f(m.group(8)), "rttMs": _f(m.group(9)), "stages": m.group(10)}))
                continue
            m = RE_FRAMEFLOW.search(msg)
            if m:
                records.append((ts, "frame_flow", {"displays": _i(m.group(1)), "raw": m.group(2)}))
                continue
            m = RE_PREDICTION.search(msg)
            if m:
                records.append((ts, "prediction", {"pastMs": _i(m.group(1)), "fromPoseMs": _i(m.group(2)),
                                                   "raw": m.group(3)}))
                continue
            m = RE_ARRIVAL.search(msg)
            if m:
                records.append((ts, "arrival", {"raw": m.group(1)}))
                continue
            records.append((ts, "other", {"raw": msg}))
    return records, {"path": path, "totalLines": total}


# ---------------------------------------------------------------- 档位分段（T2）
def segment_by_profile(pc_records):
    segs = []
    cur = {"start": None, "profile": "unknown", "ceiling": None}
    for ts, kind, f in pc_records:
        if kind == "profile_switch":
            if cur["start"] is not None:
                segs.append(dict(cur))
            cur = {"start": ts, "profile": f["profile"], "ceiling": f["ceilingKbps"]}
    if cur["start"] is not None:
        segs.append(dict(cur))
    if not segs:
        segs = [dict(cur)]
    return segs


def seg_for(ts, segs):
    best = segs[0]
    for sgm in segs:
        if sgm["start"] is None:
            continue
        if ts >= sgm["start"]:
            best = sgm
    return best


# ---------------------------------------------------------------- 汇总
def _mean(xs):
    xs = [x for x in xs if x is not None]
    return sum(xs) / len(xs) if xs else None


def _pctl(xs, p):
    xs = sorted(x for x in xs if x is not None)
    if not xs:
        return None
    return xs[min(len(xs) - 1, int(len(xs) * p))]


def _rng(series_fn, keys):
    out = {}
    for key in keys:
        vals = [v for _, v in series_fn(key) if v is not None]
        out[key] = {"mean": _mean(vals), "min": min(vals), "max": max(vals)} if vals else None
    return out


def summarize(pc_kinds):
    by = defaultdict(list)
    for kind, recs in pc_kinds.items():
        for ts, f in recs:
            by[kind].append((ts, f))
    d, t, e, v = by.get("diag1s", []), by.get("throughput", []), by.get("enc5s", []), by.get("vrframes", [])
    out = {"diag1s_count": len(d), "enc5s_count": len(e), "throughput_count": len(t)}
    wire = [f.get("wireMbps") for _, f in d if f.get("wireMbps") is not None]
    out["wire_mbps"] = {"mean": _mean(wire), "p95": _pctl(wire, 0.95), "max": max(wire)} if wire else None
    tp = [f.get("mbps") for _, f in t if f.get("mbps") is not None]
    out["throughput_mbps"] = {"mean": _mean(tp), "max": max(tp)} if tp else None
    tg = [f.get("targetKbps") for _, f in d if f.get("targetKbps") is not None]
    out["target_kbps"] = {"min": min(tg), "max": max(tg)} if tg else None
    qp_all = [f.get("qp") for _, f in e if f.get("qp") is not None]
    # HEVC QP is 0..51; garbage samples (raw rejected driver values, e.g. 432586644 in
    # the frozen R9 log) are counted separately instead of poisoning the mean.
    qp = [x for x in qp_all if 0 <= x <= 51]
    out["enc_qp"] = {"mean": _mean(qp), "min": min(qp), "max": max(qp)} if qp else None
    out["qp_invalid_count"] = len(qp_all) - len(qp)
    out.update(_rng(lambda key: [(ts_, f.get(key)) for ts_, f in e],
                    ["kbFrame", "submitMs", "gpuWaitMs", "kbMin", "kbMax"]))
    out.update(_rng(lambda key: [(ts_, f.get(key)) for ts_, f in v],
                    ["gapMinMs", "gapMaxMs", "lockAvgMs", "lockMaxMs"]))
    hr = [f.get("halfRate") for _, f in d if f.get("halfRate") is not None]
    out["halfRate_on_seconds"] = sum(1 for x in hr if x == "ON")
    out["halfRate_total_seconds"] = len(hr)
    out.update(_rng(lambda key: [(ts_, f.get(key)) for ts_, f in t],
                    ["effectiveKbps", "wireCapKbps", "lossPct"]))
    q = [f.get("queueMs") for _, f in t if f.get("queueMs") is not None]
    out["pc_queue_ms"] = {"mean": _mean(q), "max": max(q)} if q else None
    out["refused_total"] = sum(f.get("refused") or 0 for _, f in t)
    return out


def summarize_hmd(hmd_kinds):
    by = defaultdict(list)
    for kind, recs in hmd_kinds.items():
        for ts, f in recs:
            by[kind].append((ts, f))
    r, vd = by.get("rx1s", []), by.get("vrdiag1s", [])
    out = {"rx1s_count": len(r), "vrdiag1s_count": len(vd)}
    mbps = [f.get("mbpsIn") for _, f in r if f.get("mbpsIn") is not None]
    out["rx_mbps_in"] = {"mean": _mean(mbps), "max": max(mbps)} if mbps else None
    loss = [f.get("lossPct") for _, f in r if f.get("lossPct") is not None]
    out["loss_pct"] = {"mean": _mean(loss), "max": max(loss)} if loss else None
    q = [f.get("queueMs") for _, f in r if f.get("queueMs") is not None]
    out["hmd_queue_ms"] = {"mean": _mean(q), "max": max(q)} if q else None
    tl = [f.get("totalLatencyMs") for _, f in r if f.get("totalLatencyMs") is not None]
    out["total_latency_ms"] = {"mean": _mean(tl), "p95": _pctl(tl, 0.95)} if tl else None
    dec = [f.get("decodeMs") for _, f in r if f.get("decodeMs") is not None]
    out["hmd_decode_ms"] = {"mean": _mean(dec)} if dec else None
    ns = [f.get("netSpreadMs") for _, f in r if f.get("netSpreadMs") is not None]
    out["net_spread_ms"] = {"mean": _mean(ns)} if ns else None
    rep_t = sum(f.get("repeated") or 0 for _, f in vd)
    disp_t = sum(f.get("displays") or 0 for _, f in vd)
    out["repeated_ratio"] = (rep_t / disp_t) if disp_t else None
    for key in ("latencyMs", "pipelineMs", "decodeMs", "rttMs"):
        vals = [f.get(key) for _, f in vd if f.get(key) is not None]
        out["vrdiag_" + key] = {"mean": _mean(vals), "max": max(vals)} if vals else None
    return out


# ---------------------------------------------------------------- 对齐（T3）
def estimate_offset(pc_records, hmd_records, lo_s=-30.0, hi_s=30.0):
    pc = {t.replace(microsecond=0): f.get("wireMbps")
          for t, k, f in pc_records if k == "diag1s" and f.get("wireMbps") is not None}
    hm = {t.replace(microsecond=0): f.get("mbpsIn")
          for t, k, f in hmd_records if k == "rx1s" and f.get("mbpsIn") is not None}
    if len(pc) < 30 or len(hm) < 30:
        return None, "样本不足（PC %d / 头显 %d 秒，需各 ≥30）" % (len(pc), len(hm))
    best_n, best_off, best_dev = 0, None, None
    off = lo_s
    while off <= hi_s:
        shift = dt.timedelta(seconds=off)
        matched, dev = 0, 0.0
        for ts, v in hm.items():
            t2 = ts + shift
            if t2 in pc:
                matched += 1
                dev += abs(pc[t2] - v)
        if matched > best_n or (matched == best_n and matched > 0 and (best_dev is None or dev / matched < best_dev)):
            best_n, best_off, best_dev = matched, off, (dev / matched if matched else None)
        off += 1.0
    if best_n < 10:
        return None, "重合秒太少（%d）" % best_n
    return best_off, "重合 %d 秒，平均 |PC wire − 头显 rx| = %.2f Mbps" % (best_n, best_dev)


# ---------------------------------------------------------------- 报告（T4）
def fmt(x, nd=1):
    return "?" if x is None else ("%.*f" % (nd, x))


def build_report(pc_meta, pc_records, hmd_meta, hmd_records, offset_ms, segs, all_seconds):
    L = []
    shift = dt.timedelta(milliseconds=offset_ms or 0)
    segs = [dict(s) for s in segs]
    for i, sgm in enumerate(segs):
        sgm["id"] = i

    def seg_id_for(ts):
        best = 0
        for sgm in segs:
            if sgm.get("start") is not None and ts >= sgm["start"]:
                best = sgm["id"]
        return best

    pc_by, hmd_by = defaultdict(lambda: defaultdict(list)), defaultdict(lambda: defaultdict(list))
    for ts, kind, f in pc_records:
        pc_by[seg_id_for(ts)][kind].append((ts, f))
    for ts, kind, f in hmd_records:
        hmd_by[seg_id_for(ts + shift)][kind].append((ts + shift, f))

    seg_infos = []
    for i, sgm in enumerate(segs):
        start, end = sgm.get("start"), None
        for j in range(i + 1, len(segs)):
            if segs[j].get("start") is not None:
                end = segs[j]["start"]
                break
        in_seg = (lambda t: True) if start is None else (lambda t, s=start, e=end: t >= s and (e is None or t < e))
        pc_k, hmd_k = defaultdict(list), defaultdict(list)
        for kind, recs in pc_by.get(i, {}).items():
            pc_k[kind] = [(t, f) for t, f in recs if in_seg(t)]
        for kind, recs in hmd_by.get(i, {}).items():
            hmd_k[kind] = [(t, f) for t, f in recs if in_seg(t)]
        seg_infos.append((sgm, summarize(pc_k), summarize_hmd(hmd_k)))

    L.append("== 元信息 ==")
    L.append("PC 日志: %s（%d 行）" % (pc_meta.get("path"), pc_meta.get("totalLines", 0)))
    if hmd_meta.get("path"):
        L.append("头显日志: %s（%d 行）" % (hmd_meta["path"], hmd_meta["totalLines"]))
    L.append("应用偏移: %d ms" % (offset_ms or 0))
    if pc_records:
        L.append("PC 时间范围: %s … %s" % (pc_records[0][0], pc_records[-1][0]))
    if hmd_records:
        L.append("头显时间范围(平移后): %s … %s" % (hmd_records[0][0] + shift, hmd_records[-1][0] + shift))
    L.append("")

    L.append("== 档位分段 ==")
    for idx, (sgm, pc_sum, hmd_sum) in enumerate(seg_infos):
        s0, dur = sgm.get("start"), "?"
        if s0 is not None:
            e = None
            for j in range(idx + 1, len(seg_infos)):
                if seg_infos[j][0].get("start") is not None:
                    e = seg_infos[j][0]["start"]
                    break
            if e is not None:
                dur = "%.0f s" % (e - s0).total_seconds()
        L.append("  %s 起 | profile=%s ceiling=%s kbps | 时长 %s | PC diag=%d enc=%d tp=%d | 头显 rx=%d diag=%d"
                 % (s0 or "日志头", sgm["profile"], sgm["ceiling"], dur,
                    pc_sum["diag1s_count"], pc_sum["enc5s_count"], pc_sum["throughput_count"],
                    hmd_sum["rx1s_count"], hmd_sum["vrdiag1s_count"]))
    L.append("")

    L.append("== 每段 PC 汇总 ==")
    for sgm, pc_sum, _ in seg_infos:
        L.append("[%s / %s kbps]" % (sgm["profile"], sgm["ceiling"]))
        w = pc_sum.get("wire_mbps") or {}
        tg = pc_sum.get("target_kbps") or {}
        L.append("  wire Mbps: mean %s / p95 %s / max %s | target %s..%s kbps | halfRate ON %d/%d s"
                 % (fmt(w.get("mean")), fmt(w.get("p95")), fmt(w.get("max")),
                    tg.get("min"), tg.get("max"),
                    pc_sum["halfRate_on_seconds"], pc_sum["halfRate_total_seconds"]))
        e = pc_sum.get("enc_qp") or {}
        kb = pc_sum.get("kbFrame") or {}
        sub = pc_sum.get("submitMs") or {}
        gw = pc_sum.get("gpuWaitMs") or {}
        L.append("  ENC qp: mean %s / %s..%s | KB/frame mean %s | submit %s ms | gpu wait %s ms"
                 % (fmt(e.get("mean")), fmt(e.get("min")), fmt(e.get("max")),
                    fmt(kb.get("mean")), fmt(sub.get("mean")), fmt(gw.get("mean"))))
        g = pc_sum.get("gapMinMs") or {}
        gm = pc_sum.get("gapMaxMs") or {}
        la = pc_sum.get("lockAvgMs") or {}
        if g.get("mean") is not None:
            L.append("  arrivalGap %s..%s ms | lockWait avg %s ms"
                     % (fmt(g.get("mean")), fmt(gm.get("mean")), fmt(la.get("mean"))))
        tp = pc_sum.get("throughput_mbps") or {}
        pq = pc_sum.get("pc_queue_ms") or {}
        L.append("  throughput: mean %s / max %s Mbps | pc queue max %s ms | refused 累计 %d"
                 % (fmt(tp.get("mean")), fmt(tp.get("max")), fmt(pq.get("max")), pc_sum["refused_total"]))
        lp = pc_sum.get("lossPct") or {}
        if lp.get("mean") is not None:
            L.append("  loss mean %s%% / max %s%% | effective %s kbps | wireCap %s kbps"
                     % (fmt(lp.get("mean")), fmt(lp.get("max")),
                        fmt((pc_sum.get("effectiveKbps") or {}).get("mean")),
                        fmt((pc_sum.get("wireCapKbps") or {}).get("mean"))))
    L.append("")

    if hmd_records:
        L.append("== 每段头显汇总 ==")
        for sgm, _, hmd_sum in seg_infos:
            L.append("[%s / %s kbps]" % (sgm["profile"], sgm["ceiling"]))
            mi = hmd_sum.get("rx_mbps_in") or {}
            lp = hmd_sum.get("loss_pct") or {}
            hq = hmd_sum.get("hmd_queue_ms") or {}
            L.append("  rx Mbps in: mean %s / max %s | loss: mean %s%% / max %s%% | hmd queue: mean %s / max %s ms"
                     % (fmt(mi.get("mean")), fmt(mi.get("max")),
                        fmt(lp.get("mean")), fmt(lp.get("max")),
                        fmt(hq.get("mean")), fmt(hq.get("max"))))
            tl = hmd_sum.get("total_latency_ms") or {}
            L.append("  total latency: mean %s / p95 %s ms | decode mean %s ms | net spread mean %s ms"
                     % (fmt(tl.get("mean")), fmt(tl.get("p95")),
                        fmt((hmd_sum.get("hmd_decode_ms") or {}).get("mean")),
                        fmt((hmd_sum.get("net_spread_ms") or {}).get("mean"))))
            rr = hmd_sum.get("repeated_ratio")
            L.append("  repeated 比例: %s%%" % (fmt(rr * 100) if rr is not None else "?"))
            vd = hmd_sum.get("vrdiag_latencyMs") or {}
            if vd.get("mean") is not None:
                L.append("  vrdiag latency mean %s / max %s ms | rtt mean %s ms"
                         % (fmt(vd.get("mean")), fmt(vd.get("max")),
                            fmt((hmd_sum.get("vrdiag_rttMs") or {}).get("mean"))))
        L.append("")

    L.append("== 同秒对齐视图 ==" + ("（全部秒）" if all_seconds else "（只列异常秒；--all-seconds 全打）"))
    pc_sec, hmd_sec = {}, {}
    for ts, kind, f in pc_records:
        if kind == "diag1s":
            pc_sec[ts.replace(microsecond=0) + shift] = f
    for ts, kind, f in hmd_records:
        if kind in ("rx1s", "vrdiag1s"):
            hmd_sec.setdefault(ts.replace(microsecond=0) + shift, {}).update(f)
    shown = 0
    for key in sorted(set(pc_sec) | set(hmd_sec)):
        p = pc_sec.get(key, {})
        h = hmd_sec.get(key, {})
        wire, target = p.get("wireMbps"), p.get("targetKbps")
        loss, queue = h.get("lossPct"), h.get("queueMs")
        repeated, displays = h.get("repeated"), h.get("displays")
        hr = p.get("halfRate")
        bad = []
        if loss is not None and loss > 1.0:
            bad.append("loss %.1f%%" % loss)
        if queue is not None and queue > 30.0:
            bad.append("hmd queue %.0f ms" % queue)
        if repeated is not None and displays and repeated / max(1, displays) > 0.2:
            bad.append("repeated %d/%d" % (repeated, displays))
        if hr == "ON":
            bad.append("halfRate ON")
        if wire is not None and target and wire < target * 0.7:
            bad.append("wire %.1f << target %d" % (wire, target))
        pq = p.get("queueMs")
        if pq is not None and pq > 45.0:
            bad.append("pc queue %.0f ms" % pq)
        if all_seconds or bad:
            shown += 1
            L.append("  %s | PC wire %s / target %s | hmd rx %s loss %s queue %s | rep %s/%s | %s"
                     % (key, fmt(wire), target, fmt(h.get("mbpsIn")), fmt(loss), fmt(queue),
                        repeated if repeated is not None else "-", displays if displays is not None else "-",
                        "; ".join(bad) if bad else "ok"))
    L.append("  （列出 %d 秒）" % shown)
    L.append("")

    L.append("== 启发式提示（非结论） ==")
    hints = []
    both_q = [k for k in sorted(set(pc_sec) | set(hmd_sec))
              if (pc_sec.get(k, {}).get("queueMs") or -1) > 45 and (hmd_sec.get(k, {}).get("queueMs") or -1) > 30]
    if both_q:
        hints.append("PC 与头显队列同时偏高的秒有 %d 个 → 疑似网络排队（对比 loss 与 gpu wait 排除编码侧）" % len(both_q))
    rep_spikes = sum(1 for k, h in hmd_sec.items()
                     if h.get("displays") and h.get("repeated") is not None
                     and h["repeated"] / max(1, h["displays"]) > 0.2)
    if rep_spikes:
        hints.append("头显 repeated 比例 >20%% 的秒有 %d 个 → 疑似显示侧重复帧周期" % rep_spikes)
    hr_on = [k for k, p in pc_sec.items() if p.get("halfRate") == "ON"]
    if hr_on:
        hints.append("halfRate ON 的秒：%s … %s（共 %d 秒）" % (hr_on[0], hr_on[-1], len(hr_on)))
    low_spend = [k for k, p in pc_sec.items()
                 if p.get("wireMbps") is not None and p.get("targetKbps")
                 and p["wireMbps"] < p["targetKbps"] * 0.5]
    if low_spend:
        hints.append("wire < 50%% target 的秒有 %d 个 → 编码器没花满（QP 下限/内容简单/编码器限制）" % len(low_spend))
    if not hints:
        hints.append("未发现异常模式")
    for h in hints:
        L.append("  - " + h)
    return "\n".join(L)


# ---------------------------------------------------------------- 自测（T5）
def selftest(args):
    import os
    ok = True
    results = []

    def CHECK(cond, label):
        results.append(("PASS " if cond else "FAIL ") + label)
        if not cond:
            ok = False

    here = os.path.dirname(os.path.abspath(__file__))
    td = os.path.join(here, "testdata")

    pc_rec, _ = parse_pc_log(os.path.join(td, "pc-new-format-sample.log"))
    kinds = [k for _, k, _ in pc_rec]
    CHECK("diag1s" in kinds, "新格式 PC: rc diag 1s 被识别")
    d1 = next((f for _, k, f in pc_rec if k == "diag1s"), None)
    CHECK(d1 is not None and d1["gameFps"] == 47 and d1["halfRate"] == "OFF"
          and d1["targetKbps"] == 100000 and abs(d1["wireMbps"] - 96.4) < 0.01
          and abs(d1["queueMs"] - 3.0) < 0.01 and abs(d1["qp"] - 20.5) < 0.01
          and d1["encW"] == 2416 and d1["encH"] == 2416, "新格式 PC: rc diag 1s 字段值")
    e5 = [f for _, k, f in pc_rec if k == "enc5s" and f.get("qpMin") is not None]
    CHECK(bool(e5), "新格式 PC: ENC 5s 新字段（qpRange/kbRange）被解析")
    if e5:
        f = e5[0]
        CHECK(abs(f["qp"] - 20.5) < 0.01 and f["qpValid"] == 90 and f["qpTotal"] == 92
              and f["qpFirstBad"] == 47 and f["qpMin"] == 12 and f["qpMax"] == 41
              and abs(f["kbMin"] - 44.6) < 0.01 and abs(f["kbMax"] - 210.3) < 0.01,
              "新格式 PC: ENC 5s 新字段值")
    tp = [f for _, k, f in pc_rec if k == "throughput" and f.get("effectiveKbps") is not None]
    CHECK(bool(tp) and tp[0]["effectiveKbps"] == 98000 and tp[0]["wireCapKbps"] == 108000
          and tp[0]["lossPct"] == 0.1 and tp[0]["queueMs"] == 2.0,
          "新格式 PC: throughput effective/wireCap/queue/loss")
    vf = [f for _, k, f in pc_rec if k == "vrframes" and f.get("gapMinMs") is not None]
    CHECK(bool(vf) and vf[0]["gapMinMs"] == 9 and vf[0]["gapMaxMs"] == 22
          and abs(vf[0]["lockAvgMs"] - 0.9) < 0.01 and vf[0]["lockN"] == 89,
          "新格式 PC: VR frames arrivalGap/lockWait")

    hmd_rec, _ = parse_hmd_logcat(os.path.join(td, "hmd-new-format-sample.logcat"), year="2026")
    rx = [f for _, k, f in hmd_rec if k == "rx1s"]
    CHECK(bool(rx) and abs(rx[0]["mbpsIn"] - 95.8) < 0.01 and abs(rx[0]["lossPct"] - 0.2) < 0.01
          and rx[0]["fragsLost"] == 3 and rx[0]["fragTotal"] == 1500 and rx[0]["fecRecovered"] == 3
          and rx[0]["queueMs"] == 3.5 and abs(rx[0]["totalLatencyMs"] - 61.0) < 0.01,
          "新格式 HMD: rc vr rx 1s 字段值")
    vd = [f for _, k, f in hmd_rec if k == "vrdiag1s"]
    CHECK(bool(vd) and vd[0]["shown"] == 88 and vd[0]["repeated"] == 2 and vd[0]["displays"] == 90
          and vd[0]["generated"] == 89 and abs(vd[0]["latencyMs"] - 62.5) < 0.01
          and ('"downlinkMs": 8.1' in vd[0]["stages"]), "新格式 HMD: rc vr diag 1s 字段值 + stages")

    old = os.path.normpath(os.path.join(here, "..", "ROUND9-EVIDENCE-2026-11-16", "streamer.log"))
    if os.path.exists(old):
        old_rec, old_meta = parse_pc_log(old)
        CHECK(len(old_rec) > 100, "旧格式：解析出 %d 条记录（不崩）" % len(old_rec))
        enc = [f for _, k, f in old_rec if k == "enc5s"]
        CHECK(all(f.get("qpRange") is None for f in enc), "旧格式：ENC 5s 无 qpRange → 优雅降级 None")
        max_tp = max((f.get("mbps") for _, k, f in old_rec if k == "throughput" and f.get("mbps") is not None),
                     default=None)
        max_fps = max((f.get("fps") for _, k, f in old_rec if k == "enc5s" and f.get("fps") is not None),
                      default=None)
        CHECK(max_tp is not None and max_tp >= 90.0,
              "旧格式：throughput 峰值 %s Mbps（实测；ROUND9 公布的 92.1 为满帧段均值口径）" % fmt(max_tp))
        CHECK(max_fps is not None and abs(max_fps - 90) < 0.5,
              "旧格式：ENC fps 峰值 %s ≈ ROUND9 公布 89.9 满帧" % fmt(max_fps, 0))
        segs = segment_by_profile(old_rec)
        CHECK(any(sg["profile"] == "quality-first" for sg in segs), "旧格式：档位锚点识别")
        rep = build_report(old_meta, old_rec, {"path": None}, [], 0, segs, False)
        CHECK("档位分段" in rep, "旧格式：报告生成（不崩）")
    else:
        results.append("SKIP 旧格式证据文件不存在: " + os.path.basename(old))

    for line in results:
        print(line)
    print("SELFTEST " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description="RinCast 双端日志分段对齐分析（ROUND13 A-01）")
    ap.add_argument("--pc", help="PC streamer.log 路径")
    ap.add_argument("--hmd", help="头显 logcat 文本路径")
    ap.add_argument("--offset-ms", type=int, default=0, help="头显→PC 时间平移（毫秒，默认 0）")
    ap.add_argument("--estimate-offset", action="store_true", help="互相关估算偏移（建议值，不自动采用）")
    ap.add_argument("--all-seconds", action="store_true", help="对齐视图打印全部秒")
    ap.add_argument("--json", help="额外输出机器可读 JSON")
    ap.add_argument("--hmd-year", default=None, help="logcat 年份（默认当前年）")
    ap.add_argument("--since", default=None, help="只分析此时间之后的记录（YYYY-MM-DD HH:MM:SS）")
    ap.add_argument("--until", default=None, help="只分析此时间之前的记录（同上）")
    ap.add_argument("--csv", help="导出 1Hz 对齐序列为 CSV")
    ap.add_argument("--selftest", action="store_true", help="跑内置断言 + testdata 样例")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest(args)
    if not args.pc and not args.hmd:
        ap.print_help()
        return 2

    pc_records, pc_meta = parse_pc_log(args.pc) if args.pc else ([], {"path": None})
    hmd_records, hmd_meta = parse_hmd_logcat(args.hmd, args.hmd_year) if args.hmd else ([], {"path": None})
    if args.since or args.until:
        since = dt.datetime.strptime(args.since, "%Y-%m-%d %H:%M:%S") if args.since else None
        until = dt.datetime.strptime(args.until, "%Y-%m-%d %H:%M:%S") if args.until else None
        pc_records = [(t, k, f) for t, k, f in pc_records
                      if (since is None or t >= since) and (until is None or t <= until)]
        hmd_records = [(t, k, f) for t, k, f in hmd_records
                       if (since is None or t >= since) and (until is None or t <= until)]
    segs = segment_by_profile(pc_records)
    if args.estimate_offset and pc_records and hmd_records:
        off, note = estimate_offset(pc_records, hmd_records)
        print("== 偏移估算（建议值，未自动采用）==")
        print("  " + (("%d ms（%s）" % (int(off * 1000), note)) if off is not None else note))
    if args.csv and pc_records:
        pc_sec, hmd_sec2 = {}, {}
        shift2 = dt.timedelta(milliseconds=args.offset_ms or 0)
        for ts, kind, f in pc_records:
            if kind == "diag1s": pc_sec[ts.replace(microsecond=0) + shift2] = f
        for ts, kind, f in hmd_records:
            if kind == "rx1s": hmd_sec2.setdefault(ts.replace(microsecond=0) + shift2, {}).update(f)
        with open(args.csv, "w", encoding="utf-8") as f2:
            f2.write("second,pc_wire_mbps,pc_target_kbps,hmd_rx_mbps,hmd_loss_pct,hmd_queue_ms\n")
            for key in sorted(set(pc_sec) | set(hmd_sec2)):
                pp = pc_sec.get(key, {}); hh = hmd_sec2.get(key, {})
                f2.write("%s,%s,%s,%s,%s,%s\n" % (key, pp.get("wireMbps", ""), pp.get("targetKbps", ""),
                    hh.get("mbpsIn", ""), hh.get("lossPct", ""), hh.get("queueMs", "")))
        print("CSV 已写：%s" % args.csv)
    report = build_report(pc_meta, pc_records, hmd_meta, hmd_records, args.offset_ms, segs, args.all_seconds)
    print(report)
    if args.json:
        out = {"pc": {"meta": pc_meta,
                      "records": [[t.isoformat(" "), k, f] for t, k, f in pc_records]},
               "hmd": {"meta": hmd_meta,
                       "records": [[(t + dt.timedelta(milliseconds=args.offset_ms or 0)).isoformat(" "), k, f]
                                   for t, k, f in hmd_records]},
               "offsetMs": args.offset_ms or 0}
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(out, f, ensure_ascii=False, indent=1)
        print("JSON 已写：%s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
