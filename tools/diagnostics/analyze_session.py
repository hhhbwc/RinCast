import os, re, statistics, io, sys

hmd_path = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("RINCAST_HMD_LOG", "hmd-logcat.txt")
pc_path = os.path.join(os.environ.get("LOCALAPPDATA", ""), "RinCast", "logs", "streamer.log")
try:
    log = open(hmd_path, encoding='utf-8', errors='replace').read().splitlines()
    pc  = open(pc_path, encoding='utf-8', errors='replace').read().splitlines()
except OSError as e:
    sys.exit(f"cannot open log: {e}\nusage: python analyze_session.py [hmd-logcat.txt]  (PC log: %LOCALAPPDATA%/RinCast/logs/streamer.log)")

# --- PC 侧档位切换点 ---
seg = []
for l in pc:
    m = re.search(r'^(\d{4}-\d\d-\d\d (\d\d:\d\d:\d\d)).*VR stream: (?:fixed|adaptive) bitrate, (latency-first|balanced|quality-first), ceiling (\d+) kbps, (capped VBR|CBR)', l)
    if m:
        seg.append((m.group(2), m.group(3), int(m.group(4))//1000, m.group(5)))
print('=== 档位分段（PC 日志）===')
for s in seg: print('  ', s)

def bucket(t):
    for i,(ts,name,ceil,rc) in enumerate(seg):
        if t >= ts: cur = (i,name,ceil,rc)
    return cur if 'cur' in dir() else (0,'?',0,'?')

# --- 头显侧延迟分解 ---
pat = re.compile(r'^(\d\d-\d\d (\d\d:\d\d:\d\d)\.\d+).*VR prediction: ([\d.]+) ms past the display time, ([\d.]+) ms from the pose to its display; uplink ([\d.]+), game ([\d.]+), encoderWait ([\d.]+), encode ([\d.]+), capture ([\d.]+), downlink ([\d.]+), decode ([\d.]+), nextFrame ([\d.]+), compositor ([\d.]+)')
rows=[]
for l in log:
    m = pat.match(l.strip())
    if m:
        t=m.group(2)
        rows.append(dict(t=t, seg=bucket(t), past=float(m.group(3)), mtp=float(m.group(4)),
            uplink=float(m.group(5)), game=float(m.group(6)), encwait=float(m.group(7)),
            encode=float(m.group(8)), capture=float(m.group(9)), downlink=float(m.group(10)),
            decode=float(m.group(11)), nextframe=float(m.group(12)), compositor=float(m.group(13))))
if not rows:
    sys.exit('no VR prediction samples in the headset log — nothing to summarize')
print(f'\n=== VR prediction 样本 {len(rows)} 条，{rows[0]["t"]} → {rows[-1]["t"]} ===')

fields=['past','mtp','uplink','game','encwait','encode','downlink','decode','nextframe','compositor']
for idx in range(len(seg)):
    sub=[r for r in rows if r['seg'][0]==idx]
    if not sub: continue
    name=seg[idx][1]; print(f'\n--- 段 {idx}: {name} ({seg[idx][0]}起, {seg[idx][2]} Mbps, {seg[idx][3]})  样本 {len(sub)} ---')
    for f in fields:
        v=[r[f] for r in sub]
        print(f'  {f:10s} mean {statistics.mean(v):7.1f}  p95 {sorted(v)[int(len(v)*0.95)-1]:7.1f}  max {max(v):7.1f}')

print('\n=== 时间线（每 30s，看延迟何时恶化）===')
print(f'{"time":9s} {"seg":14s} {"past":>6s} {"MTP":>7s} {"down":>6s} {"dec":>6s} {"encWait":>8s} {"game":>6s}')
last=''
for r in rows:
    if r['t'][:5] != last and r['t'][-2:] in ('00','01','02','03','04','05','06','07','08','09','30','31','32','33','34','35','36','37','38','39'):
        last=r['t'][:5]
        print(f'{r["t"]:9s} {r["seg"][1]:14s} {r["past"]:6.1f} {r["mtp"]:7.1f} {r["downlink"]:6.1f} {r["decode"]:6.1f} {r["encwait"]:8.1f} {r["game"]:6.1f}')

# --- PC 侧 ENC 5s / throughput 分段 ---
print('\n=== PC 侧每段 ENC 5s 汇总 ===')
enc=[]
for l in pc:
    m=re.search(r'^(\d{4}-\d\d-\d\d (\d\d:\d\d:\d\d)).*ENC 5s: ([\d.]+) fps, qp ([\d.]+), ([\d.]+) KB/frame, ([\d.]+) Mbps \(ceiling (\d+)\), submit ([\d.]+) ms, gpu wait ([\d.]+) ms', l)
    if m: enc.append((m.group(2), *[float(x) for x in m.groups()[2:]]))
for idx in range(len(seg)):
    sub=[e for e in enc if bucket(e[0])[0]==idx]
    if not sub: continue
    print(f'  段{idx} {seg[idx][1]:14s} n={len(sub):3d} fps {statistics.mean(e[1] for e in sub):5.1f} qp {statistics.mean(e[2] for e in sub):5.1f} Mbps {statistics.mean(e[4] for e in sub):6.1f} (max {max(e[4] for e in sub):6.1f}) gpuwait {statistics.mean(e[7] for e in sub):4.1f}')

# --- guard 事件分段计数 ---
print('\n=== guard / loss 事件分段 ===')
for idx in range(len(seg)):
    lo=seg[idx][0]; hi=seg[idx+1][0] if idx+1<len(seg) else '99:99:99'
    dips=[l for l in pc if re.match(r'^'+r'\d{4}-\d\d-\d\d '+lo[:2],l) is None]  # unused
    c=sum(1 for l in pc if 'guard dip' in l and lo<=re.search(r'(\d\d:\d\d:\d\d)',l).group(1)<hi)
    br=sum(1 for l in pc if 'loss brake' in l and lo<=re.search(r'(\d\d:\d\d:\d\d)',l).group(1)<hi)
    print(f'  段{idx} {seg[idx][1]:14s} guard dip {c:3d} 次, loss brake {br:2d} 次')
