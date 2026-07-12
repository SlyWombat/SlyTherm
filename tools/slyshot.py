#!/usr/bin/env python3
"""slyshot.py - pull a live screenshot from the SlyTherm wall unit over WiFi.

The firmware serves the current LVGL screen on TCP :8081 as a
"SLYSHOT <w> <h>\\n" header followed by raw little-endian RGB565.

Usage: slyshot.py [ip] [out.png] [screen]
  screen: optional screen to drive to before the snapshot (view-only; never
          changes control state). Accepts:
            bare index   0 Home  1 Presets  2 Sensors  3 System  4 Settings  5 Diag
            tab:NAME     tab:home|presets|sensors|system|settings|diag
            sheet:NAME   sheet:fan|networking|display|security|system   (Settings group)
                         sheet:wifi|home|hold|vacation
          Unknown/empty -> snapshots whatever screen is currently up.
Defaults: 192.168.10.13, slyshot.png. Requires Pillow.
"""

# Accepted `screen` argument vocabulary (mirrors navScreen() in
# src/ui/ui_overlays.cpp). Kept for --list / validation; the firmware is the
# source of truth and silently ignores anything it doesn't recognize.
SCREENS = [
    "0", "1", "2", "3", "4", "5",
    "tab:home", "tab:presets", "tab:sensors", "tab:system", "tab:settings", "tab:diag",
    "sheet:fan", "sheet:networking", "sheet:display", "sheet:security", "sheet:system",
    "sheet:wifi", "sheet:home", "sheet:hold", "sheet:vacation",
]
import socket
import sys
from PIL import Image

if len(sys.argv) > 1 and sys.argv[1] in ("--list", "-l"):
    print("screens:", " ".join(SCREENS))
    sys.exit(0)

ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.10.13"
out = sys.argv[2] if len(sys.argv) > 2 else "slyshot.png"

s = socket.create_connection((ip, 8081), timeout=10)
s.settimeout(10)
if len(sys.argv) > 3:
    if sys.argv[3] not in SCREENS:
        print(f"note: '{sys.argv[3]}' not a known screen; firmware will keep the current one",
              file=sys.stderr)
    s.sendall((sys.argv[3] + "\n").encode())

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
