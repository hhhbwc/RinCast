#!/usr/bin/env python3
"""logpair — pair PC streamer logs with headset logcat dumps on one timeline.

Pure stdlib; run with `python -I logpair.py --pc <streamer.log> --hmd <logcat-dump.log>`.
ROUND19 L3-L6-BUNDLE / T1 (原 L3-LOG-PAIRING 卡).

What it does:
  * parses the PC streamer log (YYYY-MM-DD HH:MM:SS LEVEL msg) and the headset logcat
    dump (`adb logcat -v time` format: MM-DD HH:MM:SS.mmm I/RinCast (PID): msg);
  * estimates the clock offset (headset time minus PC time) from same-source anchors
    (wire/received Mbps step changes), refined by an interpolated waveform match;
    reports `offset=unknown` honestly when anchors are insufficient — never invents one;
  * emits a markdown report: session list, per-segment stats (segmentation on
    display-rate / dt-probe change points), a side-by-side per-window latency table
    (PC game/enc/drop/wire/qp vs HMD decode/compositor/submit->lit/total), and an
    anomaly list with paired timestamps;
  * `--selftest` builds synthetic pairs with a known offset (+-2.7 s) and asserts
    recovery within +-0.3 s, including an asymmetric-coverage case and a
    no-anchor negative control (which must yield offset=unknown).
"""

import argparse
import datetime as dt
import os
import re
import statistics
import sys

# ---------------------------------------------------------------------------
# Line parsers
# ---------------------------------------------------------------------------

PC_LINE = re.compile(r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}) (INFO|WARN|ERROR)\s{1,2}(.*)$")
HMD_LINE = re.compile(r"^(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})\.(\d{3}) ([VDIWE])/(\S+)\s*\((\d+)\): (.*)$")

PC_THROUGHPUT = re.compile(
    r"Stream \d+ throughput: ([\d.]+) Mbps actual, (\d+) fps, target (\d+) kbps, effective (\d+) kbps, "
    r"wireCap (-?\d+) kbps, queue (\d+) ms, loss ([\d.]+)%")
PC_DIAG = re.compile(
    r"rc diag 1s: game (\d+)/s enc (\d+)/s drop (\d+)/s, halfRate (\w+), target (\d+) kbps, wire ([\d.]+) Mbps, "
    r"queue (\d+) ms, qp ([\d.]+), render (\d+)x(\d+) enc (\d+)x(\d+)"
    r"(?:, seq (\d+)-(\d+))?(?:, enc p50/p95 ([\d.]+)/([\d.]+) ms)?")
PC_NVENC = re.compile(r"NVENC \w+ \d+x\d+ @(\d+) fps")
PC_REQUEST = re.compile(r"stream requests (\d+) fps")
PC_CONNECTED = re.compile(r"Headset (\S+)(?: \(\S+\))? connected from (\S+)")
PC_CLOSED = re.compile(r"Session with (.+?) closed \((.+?)\)")

HMD_DIAG = re.compile(
    r"rc vr diag 1s: (\d+) shown, (\d+) repeated, (\d+) late of (\d+) displays \(flow window\), (\d+) generated, "
    r"(\d+) stale dropped; latency ([\d.]+) ms, pipeline ([\d.]+) ms, decode ([\d.]+) ms, rtt ([\d.]+) ms, "
    r"stages (\{.*?\})(?:, submit->lit ([\d.]+) ms)?(?:, dt-probe (-?\d+))?\s*$")
HMD_RX = re.compile(
    r"rc vr rx 1s: ([\d.]+) Mbps in, loss ([\d.]+)% \((\d+) frags lost of (\d+), FEC recovered (\d+)\), "
    r"(\d+) frames lost/s, queue ([\d.]+) ms, decode ([\d.]+) ms, net spread ([\d.]+) ms, total latency ([\d.]+) ms")
HMD_TIMING = re.compile(r"VR frame timing at (\d+) Hz")
HMD_CONFIG = re.compile(r"Stream config: ([\d.]+) Hz asked, ([\d.]+) Hz on the display")
HMD_DTPROBE = re.compile(r"^DisplayTime probe: delta=(-?\d+) frames")
HMD_SWAPCHAIN = re.compile(r"VR swapchain: (\d+) images")


def parse_pc(path):
    """[(event dict)] from a streamer log. t = naive datetime from the log stamp."""
    events, year_hint = [], None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            m = PC_LINE.match(raw.rstrip("\r\n"))
            if not m:
                continue
            stamp, _level, msg = m.groups()
            try:
                t = dt.datetime.strptime(stamp, "%Y-%m-%d %H:%M:%S")
            except ValueError:
                continue
            if year_hint is None:
                year_hint = t.year
            ev = {"t": t, "msg": msg, "kind": "note"}
            mp = PC_THROUGHPUT.search(msg)
            if mp:
                ev.update(kind="throughput", mbps=float(mp.group(1)), fps=int(mp.group(2)),
                          queue_ms=int(mp.group(6)), loss_pct=float(mp.group(7)))
            md = PC_DIAG.search(msg)
            if md:
                ev.update(kind="diag", game=int(md.group(1)), enc=int(md.group(2)), drop=int(md.group(3)),
                          wire=float(md.group(6)), queue_ms=int(md.group(7)), qp=float(md.group(8)),
                          seq=(int(md.group(13)), int(md.group(14))) if md.group(13) else None,
                          enc_p50=float(md.group(15)) if md.group(15) else None,
                          enc_p95=float(md.group(16)) if md.group(16) else None)
            mn = PC_NVENC.search(msg)
            if mn:
                ev.update(kind="enc_config", fps=int(mn.group(1)))
            mr = PC_REQUEST.search(msg)
            if mr:
                ev.update(kind="fps_request", fps=int(mr.group(1)))
            mc = PC_CONNECTED.search(msg)
            if mc:
                ev.update(kind="connected", who=mc.group(1), addr=mc.group(2))
            mx = PC_CLOSED.search(msg)
            if mx:
                ev.update(kind="closed", who=mx.group(1), why=mx.group(2))
            events.append(ev)
    return events


def parse_hmd(path, year_hint=2000):
    """[(event dict)] from a logcat -v time dump; year comes from the PC log."""
    events = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            m = HMD_LINE.match(raw.rstrip("\r\n"))
            if not m:
                continue
            mo, da, hh, mm, ss, ms, _lvl, _tag, pid, msg = m.groups()
            try:
                t = dt.datetime(year_hint, int(mo), int(da), int(hh), int(mm), int(ss), int(ms) * 1000)
            except ValueError:
                continue
            ev = {"t": t, "pid": int(pid), "msg": msg, "kind": "note"}
            md = HMD_DIAG.match(msg)
            if md:
                ev.update(kind="diag", shown=int(md.group(1)), repeated=int(md.group(2)), late=int(md.group(3)),
                          displays=int(md.group(4)), generated=int(md.group(5)), stale=int(md.group(6)),
                          latency=float(md.group(7)), pipeline=float(md.group(8)), decode=float(md.group(9)),
                          rtt=float(md.group(10)),
                          submit_lit=float(md.group(12)) if md.group(12) else None,
                          dt_probe=int(md.group(13)) if md.group(13) is not None else None)
            mr = HMD_RX.match(msg)
            if mr:
                ev.update(kind="rx", mbps=float(mr.group(1)), loss_pct=float(mr.group(2)),
                          frames_lost=int(mr.group(6)), queue_ms=float(mr.group(7)),
                          decode=float(mr.group(8)), total_latency=float(mr.group(10)))
            mt = HMD_TIMING.search(msg)
            if mt:
                ev.update(kind="timing", hz=int(mt.group(1)))
            mf = HMD_CONFIG.search(msg)
            if mf:
                ev.update(kind="config", asked=float(mf.group(1)), effective=float(mf.group(2)))
            mp = HMD_DTPROBE.match(msg)
            if mp:
                ev.update(kind="dtprobe", delta=int(mp.group(1)))
            ms2 = HMD_SWAPCHAIN.search(msg)
            if ms2:
                ev.update(kind="swapchain", images=int(ms2.group(1)))
            events.append(ev)
    return events


# ---------------------------------------------------------------------------
# Clock offset estimation: offset = headset_time - pc_time (seconds)
# Two methods, layered:
#   1. waveform fit (per-sample linear interpolation): sub-second precision, but only
#      usable when both sides sample the SAME quantity with the same window semantics
#      (synthetic tests, dense matching series). Gate: R^2 >= 0.5.
#   2. binned 1 s Pearson correlation: robust to window-semantics mismatch between the
#      PC wire average and the HMD receive average; precision +-1 s (bin resolution).
#      Gate: r >= 0.6 over >= 15 active bin pairs.
# If neither gate passes -> offset=unknown (never invented).
# ---------------------------------------------------------------------------

SEARCH_RANGE_S = 12.0
WAVE_STEP_S = 0.01
MIN_WAVE_SAMPLES = 30
MIN_WAVE_R2 = 0.5
MIN_BIN_PAIRS = 15
MIN_BIN_R = 0.6
BIN_UNCERTAINTY_S = 1.0


def _interp(series, t):
    """Linear interpolation on a [(datetime, float)] series sorted by t; None outside."""
    if not series or t <= series[0][0] or t >= series[-1][0]:
        return None
    lo, hi = 0, len(series) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if series[mid][0] <= t:
            lo = mid
        else:
            hi = mid
    (ta, va), (tb, vb) = series[lo], series[hi]
    if tb == ta:
        return va
    frac = (t - ta).total_seconds() / (tb - ta).total_seconds()
    return va + (vb - va) * frac


def _waveform_fit(pc_series, hmd_series):
    """Best offset + R^2 of the per-sample interpolated waveform match."""
    def err_at(off):
        sq, n = 0.0, 0
        shift = dt.timedelta(seconds=off)
        for th, vh in hmd_series:
            vpc = _interp(pc_series, th - shift)
            if vpc is None:
                continue
            sq += (vh - vpc) ** 2
            n += 1
        return (sq / n if n else float("inf")), n

    def scan(center, window, step):
        best_off, best_err = None, float("inf")
        n = max(1, int(round(2 * window / step)))
        for i in range(n + 1):
            off = center - window + i * step
            e, _ = err_at(off)
            if e < best_err:
                best_off, best_err = off, e
        return best_off, best_err

    if len(hmd_series) < MIN_WAVE_SAMPLES:
        return None, 0.0, 0, None
    off0, _ = scan(0.0, SEARCH_RANGE_S, 0.1)
    off1, _ = scan(off0, 0.6, WAVE_STEP_S)
    err, cnt = err_at(off1)
    var_h = statistics.pvariance([v for _t, v in hmd_series]) if len(hmd_series) > 1 else 0.0
    r2 = 1.0 - err / var_h if var_h > 1e-9 else 0.0
    rms = err ** 0.5 if err != float("inf") else None
    return (round(off1, 3) if cnt >= MIN_WAVE_SAMPLES else None), r2, cnt, rms


def _bin1s(series):
    bins = {}
    for t, v in series:
        key = int(t.timestamp())
        bins.setdefault(key, []).append(v)
    return {k: statistics.fmean(v) for k, v in sorted(bins.items())}


def _binned_fit(pc_series, hmd_series):
    """Best offset + Pearson r over 1 s mean bins (active bins only, >1 Mbps).

    Integer-second offsets only: the bins themselves are 1 s wide, so sub-bin shifts
    would just re-label the same pairing; the reported uncertainty covers the rest.
    """
    pb, hb = _bin1s(pc_series), _bin1s(hmd_series)
    best = (None, 0.0, 0)
    for off_s in range(-int(SEARCH_RANGE_S), int(SEARCH_RANGE_S) + 1):
        xs, ys = [], []
        for k, hv in hb.items():
            if hv <= 1.0:
                continue
            pk = k - off_s
            pv = pb.get(pk)
            if pv is None or pv <= 1.0:
                continue
            xs.append(pv)
            ys.append(hv)
        if len(xs) < MIN_BIN_PAIRS:
            continue
        mx, my = statistics.fmean(xs), statistics.fmean(ys)
        vx = sum((a - mx) ** 2 for a in xs)
        vy = sum((b - my) ** 2 for b in ys)
        cov = sum((a - mx) * (b - my) for a, b in zip(xs, ys))
        r = cov / (vx * vy) ** 0.5 if vx > 0 and vy > 0 else 0.0
        if abs(r) > abs(best[1]):
            best = (off_s, r, len(xs))
    return best


def estimate_offset(pc_series, hmd_series):
    """Estimate offset (headset - PC) in seconds. Returns (offset|None, detail)."""
    detail = {"method": "none", "uncertainty_s": None, "waveform_r2": None, "waveform_samples": 0,
              "waveform_rms": None, "binned_r": None, "binned_pairs": 0}

    off_w, r2_w, n_w, rms_w = _waveform_fit(pc_series, hmd_series)
    detail["waveform_r2"] = round(r2_w, 3) if r2_w is not None else None
    detail["waveform_samples"] = n_w
    detail["waveform_rms"] = round(rms_w, 2) if rms_w is not None else None

    off_b, r_b, n_b = _binned_fit(pc_series, hmd_series)
    detail["binned_r"] = round(r_b, 3)
    detail["binned_pairs"] = n_b

    if off_w is not None and r2_w >= MIN_WAVE_R2:
        if off_b is not None and n_b >= MIN_BIN_PAIRS and abs(off_b - off_w) > 3.0:
            detail["method"] = "disagreement(waveform vs binned)"
            return None, detail
        detail["method"] = "waveform"
        detail["uncertainty_s"] = 0.1
        return off_w, detail
    if off_b is not None and r_b >= MIN_BIN_R and n_b >= MIN_BIN_PAIRS:
        detail["method"] = "binned-correlation"
        detail["uncertainty_s"] = BIN_UNCERTAINTY_S
        return off_b, detail
    detail["method"] = "insufficient-anchors"
    return None, detail


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

ANOM = {"late_ratio": 0.30, "stale_per_win": 5, "loss_pct": 0.5, "queue_ms": 20.0,
        "mbps_jump": 20.0, "frames_lost": 0}


def _fmt(a, b):
    return f"{(b - a).total_seconds():.1f}s"


def _stats(values):
    vals = [v for v in values if v is not None and not (isinstance(v, float) and v != v)]
    if not vals:
        return "n/a"
    return (f"mean {statistics.fmean(vals):.1f}, min {min(vals):.1f}, max {max(vals):.1f} "
            f"({len(vals)} samples)")


def _compositor(e):
    m = re.search(r'"compositor":([0-9.]+)', e["msg"])
    return float(m.group(1)) if m else None


def build_report(pc_ev, hmd_ev, offset, detail, pc_path, hmd_path):
    align = offset is not None
    shift = dt.timedelta(seconds=offset) if align else dt.timedelta(0)
    L = []
    w = L.append

    w("# logpair report")
    w("")
    if pc_ev:
        w(f"- PC log: `{os.path.basename(pc_path)}` — {len(pc_ev)} parsed lines, "
          f"{pc_ev[0]['t']:%Y-%m-%d %H:%M:%S} .. {pc_ev[-1]['t']:%H:%M:%S}")
    else:
        w("- PC log: no parseable lines")
    if hmd_ev:
        w(f"- HMD log: `{os.path.basename(hmd_path)}` — {len(hmd_ev)} parsed lines, "
          f"{hmd_ev[0]['t']:%m-%d %H:%M:%S} .. {hmd_ev[-1]['t']:%H:%M:%S}")
    else:
        w("- HMD log: no parseable lines")
    w("")
    if not align:
        w(f"**clock offset: unknown** (detail: {detail}) — tables are NOT time-aligned; every timestamp "
          "is on its own side's clock. No offset was invented.")
    else:
        unc = detail.get("uncertainty_s")
        unc_s = f"±{unc:.1f} s" if unc is not None else "n/a"
        w(f"**clock offset (headset − PC): {offset:+.3f} s** (uncertainty {unc_s}) — method "
          f"`{detail['method']}`, waveform R² {detail['waveform_r2']} over {detail['waveform_samples']} "
          f"samples (RMS {detail['waveform_rms']} Mbps), binned Pearson r {detail['binned_r']} over "
          f"{detail['binned_pairs']} active bin pairs")
        if detail["method"] == "binned-correlation":
            w("")
            w("> Bin-resolution pairing: timestamps are aligned to ±1 s; read cross-side pairs with that margin.")
    w("")

    # ---- 1. sessions ----------------------------------------------------------
    w("## 1. Sessions")
    w("")
    w("| side | id | start | end | note |")
    w("|---|---|---|---|---|")
    open_ev = None
    for e in pc_ev:
        if e["kind"] == "connected":
            open_ev = e
        elif e["kind"] == "closed" and open_ev:
            w(f"| PC | {open_ev['who']} | {open_ev['t']:%H:%M:%S} | {e['t']:%H:%M:%S} | closed: {e['why']} |")
            open_ev = None
    if open_ev:
        w(f"| PC | {open_ev['who']} | {open_ev['t']:%H:%M:%S} | (log end) | never closed in log |")
    pids = {}
    for e in hmd_ev:
        a, b = pids.get(e["pid"], (e["t"], e["t"]))
        pids[e["pid"]] = (min(a, e["t"]), max(b, e["t"]))
    for pid, (a, b) in sorted(pids.items()):
        n = sum(1 for e in hmd_ev if e["pid"] == pid)
        w(f"| HMD | pid {pid} | {a:%H:%M:%S} | {b:%H:%M:%S} | {n} lines |")
    w("")

    # ---- 2. segments ----------------------------------------------------------
    marks = []
    for e in hmd_ev:
        if e["kind"] == "timing":
            marks.append((e["t"], "HMD", f"display {e['hz']} Hz"))
        elif e["kind"] == "config":
            marks.append((e["t"], "HMD", f"stream config {e['asked']:.0f} Hz"))
        elif e["kind"] == "dtprobe":
            marks.append((e["t"], "HMD", f"dt-probe delta {e['delta']}"))
    for e in pc_ev:
        if e["kind"] == "fps_request":
            marks.append((e["t"] + shift, "PC", f"stream requests {e['fps']} fps"))
        elif e["kind"] == "enc_config":
            marks.append((e["t"] + shift, "PC", f"encoder @{e['fps']} fps"))
    marks.sort(key=lambda x: x[0])
    segments, last = [], None
    for t, side, label in marks:
        if label != last:
            segments.append([t, side, label])
            last = label

    hmd_diag = [e for e in hmd_ev if e["kind"] == "diag"]
    pc_diag = [e for e in pc_ev if e["kind"] == "diag"]
    hmd_end = hmd_diag[-1]["t"] if hmd_diag else (hmd_ev[-1]["t"] if hmd_ev else None)
    pc_end = pc_diag[-1]["t"] if pc_diag else (pc_ev[-1]["t"] if pc_ev else None)
    timeline_end = max(x for x in (hmd_end, pc_end) if x is not None) if (hmd_end or pc_end) else None

    w("## 2. Segments (display-rate / encoder / probe change points)")
    w("")
    if not segments:
        w("(no change marks found)")
    for i, (t, side, label) in enumerate(segments):
        nxt = segments[i + 1][0] if i + 1 < len(segments) else timeline_end
        w(f"### segment {i + 1}: {label} ({side} clock, {t:%H:%M:%S}) — span {_fmt(t, nxt)}")
        hd = [e for e in hmd_diag if t <= e["t"] < nxt]
        pd = [e for e in pc_diag if (t - shift) <= e["t"] < (nxt - shift)]
        if hd:
            w(f"- HMD submit->lit: {_stats([e['submit_lit'] for e in hd])} ms")
            w(f"- HMD compositor stage: {_stats([_compositor(e) for e in hd])} ms; "
              f"decode {_stats([e['decode'] for e in hd])} ms; total-latency {_stats([e['total'] for e in hd])} ms")
            w(f"- HMD flow/win: shown {_stats([e['shown'] for e in hd])}, "
              f"late {_stats([e['late'] for e in hd])}, stale {_stats([e['stale'] for e in hd])}")
        else:
            w("- HMD: no diag windows in this span")
        if pd:
            w(f"- PC: game {_stats([e['game'] for e in pd])} fps, enc {_stats([e['enc'] for e in pd])} fps, "
              f"drop {_stats([e['drop'] for e in pd])} fps, wire {_stats([e['wire'] for e in pd])} Mbps, "
              f"qp {_stats([e['qp'] for e in pd])}")
        else:
            w("- PC: no diag windows in this span (logcat/PC coverage gap)")
        w("")

    # ---- 3. side-by-side windows ----------------------------------------------
    w("## 3. Side-by-side windows (HMD diag vs nearest PC diag, ≤3 s apart)")
    w("")
    w("| HMD time | submit->lit | compositor | decode | total | late/displays | PC time | game | enc | drop | wire | qp | seq | enc p50/p95 |")
    w("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for e in hmd_diag:
        pe, pt = None, "—"
        if pc_diag:
            th = e["t"] - shift
            cand = min(pc_diag, key=lambda p: abs((p["t"] - th).total_seconds()))
            gap = abs((cand["t"] - th).total_seconds())
            if gap <= 3.0:
                pe = cand
                pt = f"{cand['t']:%H:%M:%S} (Δ{gap:.1f}s)"
        comp = _compositor(e)
        cells = [
            f"{e['t']:%H:%M:%S}",
            f"{e['submit_lit']:.1f}" if e["submit_lit"] is not None else "n/a",
            f"{comp:.1f}" if comp is not None else "n/a",
            f"{e['decode']:.1f}",
            f"{e['total']:.1f}" if e["total"] is not None else "n/a",
            f"{e['late']}/{e['displays']}",
            pt,
            f"{pe['game']}" if pe else "—",
            f"{pe['enc']}" if pe else "—",
            f"{pe['drop']}" if pe else "—",
            f"{pe['wire']:.1f}" if pe else "—",
            f"{pe['qp']:.1f}" if pe else "—",
            (f"{pe['seq'][0]}-{pe['seq'][1]}" if pe and pe.get("seq") else "—"),
            (f"{pe['enc_p50']:.1f}/{pe['enc_p95']:.1f}" if pe and pe.get("enc_p95") is not None else "—"),
        ]
        w("| " + " | ".join(cells) + " |")
    w("")

    # ---- 4. anomalies ----------------------------------------------------------
    w("## 4. Anomalies")
    w("")
    hmd_rx = [e for e in hmd_ev if e["kind"] == "rx"]
    count = 0

    def pc_suffix(t_hmd):
        return f" (≈PC {(t_hmd - shift):%H:%M:%S})" if align else ""

    def hmd_suffix(t_pc):
        return f" (≈HMD {(t_pc + shift):%H:%M:%S})" if align else ""

    for e in hmd_diag:
        if e["displays"] and e["late"] / e["displays"] > ANOM["late_ratio"]:
            w(f"- late burst {e['late']}/{e['displays']} displays at HMD {e['t']:%H:%M:%S}{pc_suffix(e['t'])}")
            count += 1
    for e in hmd_diag:
        if e["stale"] >= ANOM["stale_per_win"]:
            w(f"- stale dropped {e['stale']}/win at HMD {e['t']:%H:%M:%S}{pc_suffix(e['t'])}")
            count += 1
    for e in hmd_rx:
        if e["loss_pct"] > ANOM["loss_pct"] or e["frames_lost"] > ANOM["frames_lost"]:
            w(f"- rx loss {e['loss_pct']}%, frames lost/s {e['frames_lost']} at HMD {e['t']:%H:%M:%S}"
              f"{pc_suffix(e['t'])}")
            count += 1
        if e["queue_ms"] > ANOM["queue_ms"]:
            w(f"- rx queue {e['queue_ms']} ms at HMD {e['t']:%H:%M:%S}{pc_suffix(e['t'])}")
            count += 1
    for i in range(1, len(hmd_rx)):
        if abs(hmd_rx[i]["mbps"] - hmd_rx[i - 1]["mbps"]) >= ANOM["mbps_jump"]:
            w(f"- rx bitrate {hmd_rx[i-1]['mbps']:.1f}→{hmd_rx[i]['mbps']:.1f} Mbps at HMD "
              f"{hmd_rx[i]['t']:%H:%M:%S}{pc_suffix(hmd_rx[i]['t'])}")
            count += 1
    for i in range(1, len(pc_diag)):
        if abs(pc_diag[i]["wire"] - pc_diag[i - 1]["wire"]) >= ANOM["mbps_jump"]:
            w(f"- PC wire {pc_diag[i-1]['wire']:.1f}→{pc_diag[i]['wire']:.1f} Mbps at PC "
              f"{pc_diag[i]['t']:%H:%M:%S}{hmd_suffix(pc_diag[i]['t'])}")
            count += 1
    for e in pc_diag:
        if e["drop"] > 0:
            w(f"- PC dropped {e['drop']} frames/s at PC {e['t']:%H:%M:%S}{hmd_suffix(e['t'])}")
            count += 1
    if count == 0:
        w("(none above thresholds: late>30%/win, stale≥5/win, loss>0.5%, rx queue>20ms, |jump|≥20 Mbps, drop>0)")
    w("")
    return "\n".join(L)


def estimate_offset_entry(pc_ev, hmd_ev):
    pc_series = [(e["t"], e["wire"]) for e in pc_ev if e["kind"] == "diag"]
    pc_series += [(e["t"], e["mbps"]) for e in pc_ev if e["kind"] == "throughput"]
    pc_series.sort(key=lambda x: x[0])
    hmd_series = [(e["t"], e["mbps"]) for e in hmd_ev if e["kind"] == "rx"]
    hmd_series.sort(key=lambda x: x[0])
    return estimate_offset(pc_series, hmd_series)


def attach_context(hmd_diag, hmd_rx):
    """Total latency + compositor stage: nearest rx line within 1.5 s of each diag window."""
    for e in hmd_diag:
        best = None
        for r in hmd_rx:
            gap = abs((r["t"] - e["t"]).total_seconds())
            if gap <= 1.5 and (best is None or gap < best[0]):
                best = (gap, r)
        e["total"] = best[1]["total_latency"] if best else None
    return hmd_diag


def run_selftest():
    """Synthetic pairs with known offset ±2.7 s; assert recovery within ±0.3 s."""
    import random

    def synth(offset, hmd_window=None, seed=7):
        rng = random.Random(seed)
        # A piecewise-linear waveform (like real wire/rx Mbps: ramps between plateaus), so the
        # interpolated waveform match carries information at every sample — a pure step+plateau
        # signal is MSE-flat between steps and the estimate degrades to grid quantization.
        keys = [(0, 5.0), (28, 70.0), (34, 72.0), (60, 12.0), (88, 60.0), (94, 64.0),
                (120, 8.0), (150, 40.0), (180, 42.0)]

        def wave(t):
            if t <= keys[0][0]:
                return keys[0][1]
            for i in range(1, len(keys)):
                t0, v0 = keys[i - 1]
                t1, v1 = keys[i]
                if t <= t1:
                    return v0 + (v1 - v0) * (t - t0) / (t1 - t0)
            return keys[-1][1]

        p0 = dt.datetime(2026, 11, 18, 0, 0, 0)
        h0 = dt.datetime(2026, 11, 18, 0, 0, 0)
        pc_ev = []
        for t in range(180):
            pc_ev.append({"t": p0 + dt.timedelta(seconds=t), "kind": "diag",
                          "wire": round(wave(t) + rng.uniform(-0.4, 0.4), 1), "game": 60, "enc": 60,
                          "drop": 0, "queue_ms": 0, "qp": 16.0, "msg": "rc diag 1s"})
        hmd_ev = []
        w0, w1 = hmd_window if hmd_window else (0, 180)
        k = w0 * 2
        while k <= w1 * 2:
            th_sec = k / 2
            hmd_ev.append({"t": h0 + dt.timedelta(seconds=th_sec), "pid": 999, "kind": "rx",
                           "mbps": round(wave(th_sec - offset) + rng.uniform(-0.4, 0.4), 1),
                           "loss_pct": 0.0, "frames_lost": 0, "queue_ms": 0.3, "decode": 5.0,
                           "total_latency": 60.0, "msg": "rc vr rx 1s"})
            k += 1
        return pc_ev, hmd_ev

    results = []
    a98_msg = ("rc diag 1s: game 60/s enc 60/s drop 0/s, halfRate OFF, target 100000 kbps, "
               "wire 80.0 Mbps, queue 2 ms, qp 20.0, render 2000x2000 enc 1800x1800, "
               "seq 1-60, enc p50/p95 5.0/7.0 ms, sendBacklog 3.0 ms, overruns 2, "
               "jobAge -1.0 ms, sendAge -1.0 ms, arrival1s 16.0-17.0 ms, submitGap 16.0-17.0 ms, "
               "submitToJob avg 4.0/max 5.0 ms, hold avg/max 0.2/0.4 ms, lock avg/max 0.1/0.2 ms, "
               "convertBlock avg/max 1.0/1.5 ms, motionRead n/a, jobRepl 0, "
               "sendWait avg/max 0.0/0.1 ms, overruns1s 1, motionPending 7")
    a98_match = PC_DIAG.search(a98_msg)
    a98_suffix = a98_msg[a98_match.end():] if a98_match else ""
    results.append(("A98/A102 appended diag fields preserve PC_DIAG parsing",
                    a98_match is not None and a98_match.group(1) == "60" and a98_match.group(9) == "2000"
                    and a98_match.group(11) == "1800" and a98_match.group(13) == "1"
                    and a98_match.group(14) == "60" and ", overruns1s 1" in a98_suffix
                    and ", motionPending 7" in a98_suffix))
    for label, off in (("positive +2.7 s", 2.7), ("negative -2.7 s", -2.7)):
        pc_ev, hmd_ev = synth(off)
        est, detail = estimate_offset_entry(pc_ev, hmd_ev)
        ok = est is not None and abs(est - off) <= 0.3
        results.append((f"{label}: est={est} s (method {detail['method']}, waveform R² {detail['waveform_r2']})", ok))

    pc_ev, hmd_ev = synth(2.7, hmd_window=(40, 120))
    est, detail = estimate_offset_entry(pc_ev, hmd_ev)
    ok = est is not None and abs(est - 2.7) <= 0.3
    results.append((f"asymmetric coverage (HMD 40-120s of PC 0-180s): est={est} s", ok))

    rng = random.Random(11)
    p0 = dt.datetime(2026, 11, 18, 0, 0, 0)
    pc_ev = [{"t": p0 + dt.timedelta(seconds=i), "kind": "diag",
              "wire": rng.uniform(0, 90), "game": 0, "enc": 0, "drop": 0, "queue_ms": 0, "qp": 16.0,
              "msg": "x"} for i in range(120)]
    hmd_ev = [{"t": p0 + dt.timedelta(seconds=i / 2), "pid": 1, "kind": "rx",
               "mbps": rng.uniform(0, 90), "loss_pct": 0.0, "frames_lost": 0, "queue_ms": 0.0,
               "decode": 0.0, "total_latency": 0.0, "msg": "y"} for i in range(240)]
    est, detail = estimate_offset_entry(pc_ev, hmd_ev)
    results.append((f"no-anchor control: est={est} (must be None; waveform R² {detail['waveform_r2']}, "
                    f"binned r {detail['binned_r']})", est is None))

    print("== logpair selftest ==")
    failed = 0
    for line, ok in results:
        print(("PASS " if ok else "FAIL ") + line)
        failed += 0 if ok else 1
    print(f"selftest: {len(results) - failed}/{len(results)} passed")
    return 1 if failed else 0


def main(argv=None):
    # Windows consoles default to a legacy codepage (e.g. GBK) that cannot print R², arrows
    # or the em dashes used below; force UTF-8 with a safe fallback instead of crashing.
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except AttributeError:
        pass
    ap = argparse.ArgumentParser(description="pair PC streamer log with HMD logcat dump")
    ap.add_argument("--pc", help="streamer.log path")
    ap.add_argument("--hmd", help="logcat -v time dump path")
    ap.add_argument("--out", help="write the markdown report here (default: stdout)")
    ap.add_argument("--year", type=int, default=None, help="logcat year hint (default: taken from the PC log)")
    ap.add_argument("--selftest", action="store_true", help="run the synthetic selftest and exit")
    args = ap.parse_args(argv)

    if args.selftest:
        return run_selftest()
    if not args.pc or not args.hmd:
        ap.error("--pc and --hmd are required unless --selftest")

    pc_ev = parse_pc(args.pc)
    year = args.year if args.year else (pc_ev[0]["t"].year if pc_ev else 2000)
    hmd_ev = parse_hmd(args.hmd, year_hint=year)
    if not pc_ev:
        print("logpair: PC log produced no parseable lines", file=sys.stderr)
        return 2
    if not hmd_ev:
        print("logpair: HMD log produced no parseable lines", file=sys.stderr)
        return 2

    offset, detail = estimate_offset_entry(pc_ev, hmd_ev)
    hmd_diag = attach_context([e for e in hmd_ev if e["kind"] == "diag"],
                              [e for e in hmd_ev if e["kind"] == "rx"])
    keep = [e for e in hmd_ev if e["kind"] != "diag"] + hmd_diag
    keep.sort(key=lambda e: e["t"])
    report = build_report(pc_ev, keep, offset, detail, args.pc, args.hmd)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(report)
        print(f"logpair: report written to {args.out} "
              f"(offset={'unknown' if offset is None else f'{offset:+.3f} s'})")
    else:
        print(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
