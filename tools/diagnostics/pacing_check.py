import os, re, statistics, sys

pc_path = os.path.join(os.environ.get("LOCALAPPDATA", ""), "RinCast", "logs", "streamer.log")
hmd_path = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("RINCAST_HMD_LOG", "hmd-logcat.txt")
try:
    pc = open(pc_path, encoding='utf-8', errors='replace').read().splitlines()
    hm = open(hmd_path, encoding='utf-8', errors='replace').read().splitlines()
except OSError as e:
    sys.exit(f"cannot open log: {e}\nusage: python pacing_check.py [hmd-logcat.txt]  (PC log: %LOCALAPPDATA%/RinCast/logs/streamer.log)")

# PC: "ENC 5s: 41 fps, qp 10.5, 306.3 KB/frame, 102.2 Mbps (ceiling 97), ..."
enc = {}
for l in pc:
    m = re.search(r'^\d{4}-\d\d-\d\d (\d\d:\d\d:\d\d).*ENC 5s: [\d.]+ fps, qp [\d.]+, ([\d.]+) KB/frame, ([\d.]+) Mbps \(ceiling (\d+)\)', l)
    if m:
        t, kb, mbps, ceil = m.group(1), float(m.group(2)), float(m.group(3)), int(m.group(4))
        # pacer = ceiling * 1.08 Mbps; time to clock out one frame
        delay = kb * 8.192 / (ceil * 1.08)
        enc[t] = (kb, mbps, ceil, delay)

# HMD: "VR prediction: ... downlink X, decode ..."
pat = re.compile(r'^\d\d-\d\d (\d\d:\d\d:\d\d)\.\d+.*VR prediction:.*downlink ([\d.]+), decode')
dl = {}
for l in hm:
    m = pat.match(l.strip())
    if m: dl.setdefault(m.group(1), []).append(float(m.group(2)))

print('=== 同秒对齐：帧大小÷pacer 速率  vs  实测 downlink ===')
print(f'{"time":9s} {"KB/frame":>9s} {"ceil":>5s} {"产出Mbps":>8s} {"预测pacer排队":>13s} {"实测downlink":>12s}')
pairs=[]
for t in sorted(enc):
    kb, mbps, ceil, d = enc[t]
    if t in dl:
        obs = statistics.mean(dl[t]); pairs.append((d, obs))
        print(f'{t:9s} {kb:9.1f} {ceil:5d} {mbps:8.1f} {d:13.1f} {obs:12.1f}')
print(f'\n对齐样本 {len(pairs)} 组')
if len(pairs) > 3:
    xs=[a for a,b in pairs]; ys=[b for a,b in pairs]
    mx,my=statistics.mean(xs),statistics.mean(ys)
    cov=sum((a-mx)*(b-my) for a,b in pairs)
    den=(sum((a-mx)**2 for a in xs)*sum((b-my)**2 for b in ys))**0.5
    print(f'预测均值 {mx:.1f} ms / 实测均值 {my:.1f} ms / 相关系数 r={cov/den:.3f}')

print('\n=== 编码器产出 vs 守卫 cap（失配即自锁）===')
over=[(t,v) for t,v in enc.items() if v[1] > v[2]*1.15]
print(f'产出超 cap 15% 以上的 5s 窗口：{len(over)}/{len(enc)}')
for t,v in over[:8]: print(f'  {t}  产出 {v[1]:6.1f} Mbps  cap {v[2]:3d} Mbps  比值 {v[1]/v[2]:.2f}x')
