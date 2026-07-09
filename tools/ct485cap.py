#!/usr/bin/env python3
"""ct485cap.py — capture the SlyTherm RS-485 LISTEN stream (issue #71).

While LISTEN capture is active (Diag -> LISTEN -> Start, `slytherm/cmd/sniff on`
over MQTT, or restored from NVS at boot — the capture runs regardless of which
screen is showing), the wall unit mirrors the CT-485 bus to its telnet log on
TCP :23 as:

    [ct485] <millis> <src>><dst> t<msgType> l<len> <hex...>   decoded frame
                                                              (first 16 payload B)
    [ct485+] <millis> <chunk#> <hex...>                       payload continuation
    [ct485-rej] <millis> <chunk#> <hex...>                    salvaged torn/merged
                                                              burst (raw bytes)
    [ct485-stats] <millis> ok=N badLen=N badCk=N over=N       accumulator counters

This connects to that port, keeps only the `[ct485]` lines, appends them to a
log file AND prints them live. Pure stdlib (socket) — no dependencies.

Usage:
    python3 tools/ct485cap.py <ip> [outfile]

    <ip>       the wall unit's IP (see its System/Diag screen or your router)
    outfile    capture file to append to (default: ct485-capture.log)

Stop with Ctrl-C. Reconnects are not automatic — rerun if the link drops.
"""
import socket
import sys
import time


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        return 1
    ip = sys.argv[1]
    outfile = sys.argv[2] if len(sys.argv) > 2 else "ct485-capture.log"
    print(f"[ct485cap] connecting to {ip}:23 -> {outfile} (Ctrl-C to stop)")
    try:
        sock = socket.create_connection((ip, 23), timeout=10)
    except OSError as e:
        print(f"[ct485cap] connect failed: {e}")
        return 2
    sock.settimeout(1.0)
    n = 0
    buf = b""
    last_data = time.time()  # [ct485-stats] beats every 30s while capturing —
    idle_limit = 120         # 2 min of silence = dead link (unit rebooted:
                             # no FIN, recv would block forever). Exit so a
                             # wrapper loop can reconnect.
    with open(outfile, "a", buffering=1) as f:
        f.write(f"# ct485cap {ip} @ {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
        try:
            while True:
                try:
                    chunk = sock.recv(4096)
                except socket.timeout:
                    if time.time() - last_data > idle_limit:
                        print(f"[ct485cap] no data for {idle_limit}s — "
                              "assuming dead link, exiting for reconnect")
                        break
                    continue
                if not chunk:
                    print("[ct485cap] connection closed by unit")
                    break
                last_data = time.time()
                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("utf-8", "replace").rstrip("\r")
                    if "[ct485" not in line:  # [ct485] / [ct485+] / [ct485-rej] / [ct485-stats]
                        continue
                    n += 1
                    stamped = time.strftime("%Y-%m-%dT%H:%M:%S") + " " + line
                    print(stamped)
                    f.write(stamped + "\n")
        except KeyboardInterrupt:
            print()
        finally:
            sock.close()
    print(f"[ct485cap] {n} frame lines written to {outfile}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
