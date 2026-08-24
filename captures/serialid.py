import sys, time, subprocess, serial
port=sys.argv[1]
# reset into app so it prints the boot banner
subprocess.run([sys.executable,"-m","esptool","--port",port,"--after","hard_reset","--before","default_reset","flash_id"],capture_output=True,timeout=40)
t0=time.time(); ser=None
while time.time()-t0<12:
    try: ser=serial.Serial(port,115200,timeout=0.3); break
    except Exception: time.sleep(0.5)
if not ser: print(port,"could not open"); sys.exit(0)
end=time.time()+14; buf=b""
while time.time()<end:
    try: d=ser.read(2048)
    except Exception: break
    if d: buf+=d
ser.close()
txt=buf.decode(errors="replace")
import re
for ln in txt.splitlines():
    if re.search(r"id=|version|\[link\]|remote|panic|sdio_drv|assert|SlyTherm|1\.[0-9]\.[0-9]", ln, re.I):
        print(port, "|", ln.strip()[:120])
