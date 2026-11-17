import os
import socket, subprocess, time, sys
DEV = os.environ.get('RINCAST_ADB_DEVICE', '')  # your headset adb id
TARGET = (os.environ.get('RINCAST_HEADSET_IP', '192.168.1.100'), 9999)
PKT = b'x' * 1200

def sh(cmd):
    try:
        return subprocess.check_output(['adb','-s',DEV,'shell',cmd], text=True,
                                       stderr=subprocess.DEVNULL, timeout=8)
    except Exception as e:
        return f'ERR {e}'

def rx_bytes():
    out = sh('cat /proc/net/dev')
    for line in out.splitlines():
        if 'wlan0' in line:
            p = line.split()
            return int(p[1])
    return -1

print('=== 头显状态 ===')
print(sh('cat /sys/class/net/wlan0/address').strip(), '/', sh('cat /proc/net/dev | grep wlan0').strip()[:90])
print('toybox nc:', sh('toybox nc --help 2>&1 | head -3').strip().replace('\n',' '))
# absorb the datagrams so no ICMP flood is generated
sh('toybox nc -u -l -p 9999 > /dev/null 2>&1 &')
time.sleep(0.5)

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try: s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4*1024*1024)
except Exception: pass

print('\n=== PC 有线 -> 6.1 -> 5G 空口 -> PICO：UDP 交付速率 ===')
print(f'{"目标":>8s} {"头显实收":>10s} {"交付率":>8s}  备注')
for mbps in (50, 100, 200, 400):
    r0 = rx_bytes()
    secs = 5.0
    start = time.perf_counter(); sent = 0
    while time.perf_counter() - start < secs:
        due = start + sent * 8 / (mbps * 1e6)
        if time.perf_counter() < due: continue
        try: s.sendto(PKT, TARGET)
        except OSError: pass
        sent += len(PKT)
    elapsed = time.perf_counter() - start
    time.sleep(0.4)   # let the last datagrams land
    r1 = rx_bytes()
    got = (r1 - r0) * 8 / elapsed / 1e6
    print(f'{mbps:7.0f}M {got:9.1f}M {got/mbps*100:7.1f}%  sent {sent*8/elapsed/1e6:.1f}M')
s.close()
sh('pkill -f "nc -u -l" 2>/dev/null')
print('\n注：交付率 <100% 且随目标速率饱和 = 这条链路的吞吐天花板；若 400M 也能交付 = 链路没问题，瓶颈在软件。')
