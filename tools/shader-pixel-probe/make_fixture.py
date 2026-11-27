from pathlib import Path
import subprocess
import sys

WIDTH, HEIGHT = 2880, 1600
OUT = Path(sys.argv[1] if len(sys.argv) > 1 else "build/black-screen-2026-11-27/shader-fixture")
OUT.mkdir(parents=True, exist_ok=True)
yuv = bytearray(WIDTH * HEIGHT * 3 // 2)
for row in range(HEIGHT):
    for x in range(WIDTH):
        padding = x < 16 or x >= 2844 or row < 80 or row >= 1520
        if padding:
            y, u, v = 83, 255, 255
        elif x < 1440:
            y = 65 + (x - 16) * 32 // 2827 + (row % 17)
            u, v = 90, 240
        else:
            y = 25 + (x - 1440) * 32 // 2827 + (row % 17)
            u, v = 240, 110
        yuv[row * WIDTH + x] = y
for row in range(HEIGHT // 2):
    for x in range(WIDTH // 2):
        source_x = x * 2 + 1
        source_y = row * 2 + 1
        padding = source_x < 16 or source_x >= 2844 or source_y < 80 or source_y >= 1520
        at = WIDTH * HEIGHT + row * (WIDTH // 2) + x
        yuv[at] = 255 if padding else (90 if source_x < 1440 else 240)
        yuv[WIDTH * HEIGHT * 5 // 4 + row * (WIDTH // 2) + x] = 255 if padding else (240 if source_x < 1440 else 110)
raw_path = OUT / "fixture.yuv"
bitstream_path = OUT / "fixture.hevc"
raw_path.write_bytes(yuv)
subprocess.run([
    "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
    "-f", "rawvideo", "-pixel_format", "yuv420p", "-video_size", f"{WIDTH}x{HEIGHT}",
    "-framerate", "1", "-i", str(raw_path), "-frames:v", "1", "-an",
    "-c:v", "libx265", "-preset", "ultrafast", "-crf", "0", "-pix_fmt", "yuv420p",
    "-x265-params", "keyint=1:min-keyint=1", "-f", "hevc", str(bitstream_path),
], check=True)
print(raw_path.resolve())
print(bitstream_path.resolve())
