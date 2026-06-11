#!/usr/bin/env python3
"""release.py — build merged single-image firmware binaries + ESP Web Tools manifests.

For each release env (sniffer, thermostat):
  1. pio run -e <env>
  2. esptool merge_bin of the four esp32dev images:
       0x1000  bootloader.bin
       0x8000  partitions.bin
       0xe000  boot_app0.bin   (from the Arduino framework package)
       0x10000 firmware.bin
     -> web/installer/firmware/<env>-<version>.bin  (flashable at offset 0)
  3. web/installer/firmware/manifest_<env>.json (ESP Web Tools format)

Version comes from the VERSION file at the repo root. Python 3.9+, stdlib only.
Honors PLATFORMIO_BUILD_DIR (the same variable PlatformIO itself uses).

Usage:
  python3 tools/release.py [--skip-build]
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "web" / "installer" / "firmware"

# env -> ESP Web Tools manifest "name"
ENVS = {
    "sniffer": "Dettson CT-485 Sniffer (RX-only)",
    "thermostat": "Dettson Thermostat (bench, demands disabled)",
}

# esp32dev flash layout (matches what `pio run -t upload` flashes)
MERGE_OFFSETS = ["0x1000", "0x8000", "0xe000", "0x10000"]
FLASH_ARGS = ["--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "4MB"]


def die(msg: str) -> None:
    print(f"release.py: ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def find_pio() -> str:
    pio = shutil.which("pio") or shutil.which("platformio")
    if not pio:
        die("pio not found on PATH (pip install platformio)")
    return pio


def pio_home() -> Path:
    return Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))


def find_esptool() -> list:
    """Return the argv prefix that runs esptool (it ships inside the
    PlatformIO tool-esptoolpy package, not on PATH)."""
    home = pio_home()
    candidates = sorted(home.glob("packages/tool-esptoolpy*/esptool.py"))
    if candidates:
        # Prefer PlatformIO's own interpreter (has esptool's deps installed).
        penv_py = home / "penv" / "bin" / "python"
        py = str(penv_py) if penv_py.exists() else sys.executable
        return [py, str(candidates[0])]
    exe = shutil.which("esptool.py") or shutil.which("esptool")
    if exe:
        return [exe]
    die(f"esptool not found under {home}/packages/tool-esptoolpy* or on PATH")


def find_boot_app0() -> Path:
    home = pio_home()
    candidates = sorted(
        home.glob("packages/framework-arduinoespressif32*/tools/partitions/boot_app0.bin")
    ) or sorted(home.glob("packages/**/boot_app0.bin"))
    if not candidates:
        die(f"boot_app0.bin not found under {home}/packages")
    return candidates[0]


def build_dir_for(env: str) -> Path:
    base = os.environ.get("PLATFORMIO_BUILD_DIR")
    return (Path(base) if base else ROOT / ".pio" / "build") / env


def run(cmd: list, **kw) -> None:
    print(f"+ {' '.join(str(c) for c in cmd)}")
    subprocess.run(cmd, check=True, cwd=ROOT, **kw)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--skip-build", action="store_true",
                    help="reuse existing pio build artifacts")
    args = ap.parse_args()

    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    if not version:
        die("VERSION file is empty")
    print(f"release version: {version}")

    pio = find_pio()
    esptool = find_esptool()
    boot_app0 = find_boot_app0()
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    results = []
    for env, name in ENVS.items():
        if not args.skip_build:
            run([pio, "run", "-e", env])

        bdir = build_dir_for(env)
        images = [bdir / "bootloader.bin", bdir / "partitions.bin",
                  boot_app0, bdir / "firmware.bin"]
        for img in images:
            if not img.is_file():
                die(f"missing build artifact: {img}")
        app_size = images[3].stat().st_size
        if app_size < 100 * 1024:
            die(f"{env}: firmware.bin suspiciously small ({app_size} bytes)")

        merged = OUT_DIR / f"{env}-{version}.bin"
        cmd = esptool + ["--chip", "esp32", "merge_bin", "-o", str(merged)] + FLASH_ARGS
        for off, img in zip(MERGE_OFFSETS, images):
            cmd += [off, str(img)]
        run(cmd)

        size = merged.stat().st_size
        # Merged image starts at flash offset 0; must cover the app at 0x10000
        # and fit the 4 MB esp32dev flash.
        if not (0x10000 + app_size <= size <= 4 * 1024 * 1024):
            die(f"{env}: merged image size {size} fails sanity check")

        manifest = {
            "name": name,
            "version": version,
            "new_install_prompt_erase": True,
            "builds": [
                {
                    "chipFamily": "ESP32",
                    "parts": [{"path": merged.name, "offset": 0}],
                }
            ],
        }
        mpath = OUT_DIR / f"manifest_{env}.json"
        mpath.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        json.loads(mpath.read_text(encoding="utf-8"))  # round-trip validate
        results.append((env, merged, size, mpath))

    print("\n=== release artifacts ===")
    for env, merged, size, mpath in results:
        print(f"  {env:10s} {merged.relative_to(ROOT)}  ({size / 1024:.0f} KiB)")
        print(f"  {'':10s} {mpath.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
