#!/usr/bin/env python3
"""slyshot.py - pull a live screenshot from the SlyTherm wall unit over WiFi.

The firmware serves the current LVGL screen on TCP :8081 as a
"SLYSHOT <w> <h>\\n" header followed by raw little-endian RGB565.

Usage: slyshot.py [ip] [out.png]   (defaults: 192.168.10.13, slyshot.png)
Requires Pillow.
"""
import socket
import sys
from PIL import Image

ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.10.13"
out = sys.argv[2] if len(sys.argv) > 2 else "slyshot.png"

s = socket.create_connection((ip, 8081), timeout=10)
s.settimeout(10)

hdr = b""
while b"\n" not in hdr:
    ch = s.recv(1)
    if not ch:
        break
    hdr += ch
parts = hdr.decode(errors="replace").split()
if not parts or parts[0] != "SLYSHOT":
    print("unexpected header:", hdr)
    sys.exit(1)
w, h = int(parts[1]), int(parts[2])

need = w * h * 2
data = bytearray()
while len(data) < need:
    d = s.recv(65536)
    if not d:
        break
    data += d
s.close()
if len(data) < need:
    print(f"short read: {len(data)}/{need}")
    sys.exit(1)

# RGB565 (LE) -> RGB888
im = Image.new("RGB", (w, h))
px = im.load()
i = 0
for y in range(h):
    for x in range(w):
        v = data[i] | (data[i + 1] << 8)
        i += 2
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        px[x, y] = ((r * 527 + 23) >> 6, (g * 259 + 33) >> 6, (b * 527 + 23) >> 6)
im.save(out)
print(f"saved {out}  ({w}x{h})")
