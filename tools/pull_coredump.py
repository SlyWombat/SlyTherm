#!/usr/bin/env python3
"""pull_coredump.py - fetch a crash coredump from a fielded SlyTherm unit
over the LAN (issue #124; no USB required).

The firmware serves the coredump flash image on TCP :8082:
  (default)  "SLYCORE <size>\\n" + raw bytes   (size 0 = no dump waiting)
  ERASE      clears the dump after you've confirmed the pull ("OK\\n")

Usage:
  pull_coredump.py <ip> [out.bin]                pull the image
  pull_coredump.py <ip> --erase                  erase (after a confirmed pull)
  pull_coredump.py <ip> out.bin --decode <elf>   pull + symbolize

Decode uses espcoredump.py (ships with esp-idf / the PlatformIO toolchain):
  espcoredump.py info_corefile -t raw -c <out.bin> <matching.elf>
The matching ELF is the release asset <target>-<version>.elf.gz (#125) for
the EXACT version the device reported in its .../boot topic.
"""
import gzip
import os
import shutil
import socket
import subprocess
import sys

def main() -> int:
    args = [a for a in sys.argv[1:]]
    if not args:
        print(__doc__)
        return 2
    ip = args.pop(0)
    erase = "--erase" in args
    if erase:
        args.remove("--erase")
    elf = None
    if "--decode" in args:
        i = args.index("--decode")
        elf = args[i + 1]
        del args[i:i + 2]
    out = args[0] if args else "coredump.bin"

    s = socket.create_connection((ip, 8082), timeout=10)
    s.settimeout(10)
    if erase:
        s.sendall(b"ERASE\n")
        resp = s.recv(8).decode(errors="replace").strip()
        print(f"erase: {resp}")
        s.close()
        return 0 if resp == "OK" else 1

    s.sendall(b"GET\n")
    hdr = b""
    while b"\n" not in hdr:
        ch = s.recv(1)
        if not ch:
            break
        hdr += ch
    parts = hdr.decode(errors="replace").split()
    if len(parts) != 2 or parts[0] != "SLYCORE":
        print(f"bad header: {hdr!r}")
        return 1
    size = int(parts[1])
    if size == 0:
        print("no coredump waiting on the device")
        return 0

    data = b""
    while len(data) < size:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
    s.close()
    if len(data) != size:
        print(f"short read: {len(data)}/{size}")
        return 1
    with open(out, "wb") as f:
        f.write(data)
    print(f"saved {out} ({size} bytes)")

    if elf:
        if elf.endswith(".gz"):  # release asset form (#125)
            raw = elf[:-3]
            with gzip.open(elf, "rb") as fin, open(raw, "wb") as fout:
                shutil.copyfileobj(fin, fout)
            elf = raw
        tool = shutil.which("espcoredump.py") or "espcoredump.py"
        cmd = [tool, "info_corefile", "-t", "raw", "-c", out, elf]
        print("+", " ".join(cmd))
        return subprocess.call(cmd)
    print("decode with: espcoredump.py info_corefile -t raw -c", out, "<matching.elf>")
    print("then, after confirming:", f"{os.path.basename(sys.argv[0])} {ip} --erase")
    return 0

if __name__ == "__main__":
    sys.exit(main())
