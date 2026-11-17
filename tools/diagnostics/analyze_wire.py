#!/usr/bin/env python3
"""Measure the real wire timeline from an encode-bench --csv dump.

This is a MEASUREMENT helper, not a gate/judge script (it changes no verdict and
is not in the taskbook's frozen-judge list). The bench's own "batch" metric groups
datagrams whose gap <= 0.15 ms, so a precisely-paced stream (uniform ~0.06-0.10 ms
spacing) collapses whole frames into one giant "batch" - that number is an artifact
of the metric, not a micro-burst. The physically meaningful measures are:

  * instant rate  = max bytes in any 1 ms sliding window  (the AP-buffer threat)
  * 1 s wire peak = max bytes in any 1 s sliding window    (the user's ceiling)
  * frame loss    = any submitted frame missing one or more fragments

Usage: python glm-toolkit/analyze_wire.py <csv> <ceiling_mbps>
"""
import sys
from pathlib import Path


def sliding_max(events, window_ns):
    """events: list of (ns, bytes) sorted by ns. Max byte-sum in any window_ns window."""
    best = 0
    cur = 0
    lo = 0
    for hi in range(len(events)):
        cur += events[hi][1]
        while events[hi][0] - events[lo][0] >= window_ns:
            cur -= events[lo][1]
            lo += 1
        if cur > best:
            best = cur
    return best


def main():
    csv = Path(sys.argv[1])
    ceiling = float(sys.argv[2])  # Mbps
    rows = []
    with csv.open() as f:
        next(f)  # header
        for line in f:
            ns, b, frame, frag = line.split(",")
            rows.append((int(ns), int(b), int(frame), int(frag)))
    rows.sort(key=lambda r: r[0])
    events = [(r[0], r[1]) for r in rows]
    span_s = (rows[-1][0] - rows[0][0]) / 1e9
    total = sum(r[1] for r in rows)

    inst_bytes = sliding_max(events, 1_000_000)        # 1 ms
    wire1s_bytes = sliding_max(events, 1_000_000_000)  # 1 s
    inst_mbps = inst_bytes * 8 / 1000.0                # bytes in 1 ms -> Mbps
    wire1s_mbps = wire1s_bytes * 8 / 1e6
    avg_mbps = total * 8 / 1e6 / span_s

    # Frame completeness: data fragments are frag != 0xFFFF; a frame is complete when
    # frag indices 0..fragCount-1 are all present. fragCount isn't in the CSV, so use
    # max frag index + 1 per frame as the expected count (parity rows carry fragCount
    # but frag=0xFFFF; the last data fragment index + 1 == fragCount).
    frames = {}
    for ns, b, frame, frag in rows:
        if frag == 0xFFFF:
            continue
        frames.setdefault(frame, set()).add(frag)
    incomplete = []
    for frame, frags in frames.items():
        expect = max(frags) + 1
        if len(frags) != expect or min(frags) != 0:
            incomplete.append((frame, len(frags), expect))
    # whole-frame gaps (a frame index never seen at all)
    seen = sorted(frames)
    missing_idx = []
    if seen:
        for i in range(seen[0], seen[-1] + 1):
            if i not in frames:
                missing_idx.append(i)

    quota = ceiling * 1.02
    print(f"file           : {csv.name}")
    print(f"datagrams      : {len(rows)} over {span_s:.2f} s, total {total/1e6:.2f} MB")
    print(f"avg wire       : {avg_mbps:.1f} Mbps")
    print(f"instant (1 ms) : {inst_mbps:.1f} Mbps  ({inst_bytes} B in worst 1 ms)")
    print(f"1 s wire peak  : {wire1s_mbps:.1f} Mbps  vs ceiling {ceiling:.0f} x1.02 = {quota:.1f}"
          f"  -> {'OK' if wire1s_mbps <= quota else 'OVER'}")
    # Per-second wire rate (fixed buckets from the first datagram) and where the peak
    # 1 s sliding window starts, to tell a startup/keyframe transient from a sustained breach.
    t0 = rows[0][0]
    buckets = {}
    for ns, b, frame, frag in rows:
        sec = int((ns - t0) / 1e9)
        buckets[sec] = buckets.get(sec, 0) + b
    per_sec = " ".join(f"s{s}:{buckets[s]*8/1e6:.0f}" for s in sorted(buckets))
    print(f"per-second Mbps: {per_sec}")
    over = sum(1 for s in buckets if buckets[s] * 8 / 1e6 > quota)
    print(f"seconds > {quota:.0f}: {over} of {len(buckets)}")
    print(f"frames seen    : {len(seen)} (index {seen[0]}..{seen[-1]})")
    print(f"incomplete     : {len(incomplete)} {incomplete[:5]}")
    print(f"missing index  : {len(missing_idx)} {missing_idx[:10]}")
    print(f"frame loss     : {'NONE' if not incomplete and not missing_idx else 'PRESENT'}")


if __name__ == "__main__":
    main()