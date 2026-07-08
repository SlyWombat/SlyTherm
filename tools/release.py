#!/usr/bin/env python3
"""release.py — build release firmware artifacts: merged USB images, ESP Web
Tools manifests, and (for OTA targets, issues #59/#60) app-only images +
sha256 + optional ECDSA signature + the regenerated firmware/catalog.json.

Per target (see TARGETS):
  1. pio run -e <env>
  2. esptool merge_bin of bootloader/partitions/boot_app0/firmware at the
     chip's offsets -> web/installer/firmware/<env>-<version>.bin (USB, offset 0)
  3. web/installer/firmware/manifest_<env>.json (ESP Web Tools format)
  4. OTA targets only:
       web/installer/firmware/<target_id>-<version>.app.bin  (raw app image,
         what Update.h/esp_https_ota consumes)
       sha256 of the app image; ECDSA-P256 signature when OTA_SIGNING_KEY_PEM
         is set (issue #62 — CI provides it; unsigned builds are for bench)
       entry in firmware/catalog.json (schema #60; lib/OtaCatalog parses it)

Flash-parameter rule (hard lesson, 2026-07-07): merge with --flash_mode/freq/
size "keep" — forcing qio into the merged header corrupts the S3 bootloader
(it must stay DIO in its own header) and boot-loops the board. Note a merged
image spans the NVS region, so USB-flashing it factory-resets the device.

Version comes from the VERSION file at the repo root; when --require-tag is
given (CI), the tag must equal v<VERSION> (issue #113). Python 3.9+, stdlib
only (signing shells out to openssl). Honors PLATFORMIO_BUILD_DIR.

Usage:
  python3 tools/release.py [--skip-build] [--require-tag vX.Y.Z]
"""

import argparse
import base64
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "web" / "installer" / "firmware"
CATALOG = ROOT / "firmware" / "catalog.json"
CATALOG_SCHEMA = 1

# GitHub release asset URL template for catalog appUrl entries (#60).
REPO_SLUG = "SlyWombat/SlyTherm"
ASSET_URL = "https://github.com/{slug}/releases/download/v{ver}/{name}"
NOTES_URL = "https://github.com/{slug}/releases/tag/v{ver}"

# Release targets. ota=None -> USB-only bench env (no catalog entry).
# `optional` targets are skipped with a warning when the env is absent from
# platformio.ini (remote_p4 lives on feature/remote-p4 until it merges).
TARGETS = [
    {
        "env": "sniffer",
        "name": "Dettson CT-485 Sniffer (RX-only)",
        "chip": "esp32", "chipFamily": "ESP32",
        "offsets": ["0x1000", "0x8000", "0xe000", "0x10000"],
        "max_flash": 4 * 1024 * 1024,
        "ota": None, "optional": False,
    },
    {
        "env": "thermostat",
        "name": "Dettson Thermostat (bench, demands disabled)",
        "chip": "esp32", "chipFamily": "ESP32",
        "offsets": ["0x1000", "0x8000", "0xe000", "0x10000"],
        "max_flash": 4 * 1024 * 1024,
        "ota": None, "optional": False,
    },
    {
        "env": "thermostat_s3",
        "name": "SlyTherm Controller (Waveshare ESP32-S3-4.3B)",
        "chip": "esp32s3", "chipFamily": "ESP32-S3",
        "offsets": ["0x0", "0x8000", "0xe000", "0x10000"],
        "max_flash": 16 * 1024 * 1024,
        # default_8MB.csv ota_0/ota_1 slots are 0x330000 (#64); CI fails a
        # release whose app exceeds 90% of the slot.
        "ota": {"id": "wall-s3", "hwRev": "s3-43b-r1.1", "slot": 0x330000},
        "optional": False,
    },
    {
        "env": "remote_p4",
        "name": "SlyTherm Remote (Guition JC-ESP32P4-M3)",
        "chip": "esp32p4", "chipFamily": "ESP32-P4",
        # ESP32-P4 second-stage bootloader lives at 0x2000 (IDF 5.x).
        "offsets": ["0x2000", "0x8000", "0xe000", "0x10000"],
        "max_flash": 16 * 1024 * 1024,
        "ota": {"id": "remote-p4", "hwRev": "p4-m3-r1", "slot": 0x640000},
        "optional": True,
    },
]


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
    """Argv prefix that runs esptool. Preference order:
    1. `python -m esptool` when the module is importable by THIS interpreter
       (CI pip-installs it with its own deps — the pio-bundled esptool.py is
       v5 there and crashes on a bare interpreter: ModuleNotFoundError
       rich_click, the v0.4.2 release failure).
    2. The pio-bundled package run with pio's own penv python (local dev).
    3. Anything on PATH."""
    try:
        import esptool  # noqa: F401
        return [sys.executable, "-m", "esptool"]
    except ImportError:
        pass
    home = pio_home()
    candidates = sorted(home.glob("packages/tool-esptoolpy*/esptool.py"))
    if candidates:
        penv_py = home / "penv" / "bin" / "python"
        if penv_py.exists():
            return [str(penv_py), str(candidates[0])]
    exe = shutil.which("esptool.py") or shutil.which("esptool")
    if exe:
        return [exe]
    die(f"esptool not importable, not under {home}/packages, not on PATH"
        " (pip install esptool)")


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


def env_exists(env: str) -> bool:
    ini = (ROOT / "platformio.ini").read_text(encoding="utf-8")
    return f"[env:{env}]" in ini


def run(cmd: list, **kw) -> None:
    print(f"+ {' '.join(str(c) for c in cmd)}")
    subprocess.run(cmd, check=True, cwd=ROOT, **kw)


def sign_image(app: Path) -> str:
    """ECDSA-P256-SHA256 signature over the app image bytes, DER, base64.
    Key comes from OTA_SIGNING_KEY_PEM (the PEM text itself — a CI secret).
    Returns "" when unset (bench build; the Controller refuses to APPLY an
    unsigned catalog entry, issue #62 — publishing one is only for testing)."""
    pem = os.environ.get("OTA_SIGNING_KEY_PEM", "")
    if not pem.strip():
        print(f"release.py: WARNING: OTA_SIGNING_KEY_PEM unset — {app.name} unsigned")
        return ""
    openssl = shutil.which("openssl")
    if not openssl:
        die("OTA_SIGNING_KEY_PEM set but openssl not found on PATH")
    with tempfile.NamedTemporaryFile("w", suffix=".pem", delete=False) as kf:
        kf.write(pem)
        key_path = kf.name
    try:
        sig = subprocess.run(
            [openssl, "dgst", "-sha256", "-sign", key_path, str(app)],
            check=True, capture_output=True).stdout
    finally:
        os.unlink(key_path)
    return base64.b64encode(sig).decode("ascii")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--skip-build", action="store_true",
                    help="reuse existing pio build artifacts")
    ap.add_argument("--require-tag", metavar="TAG", default=None,
                    help="fail unless TAG == v<VERSION> (CI guard, #113)")
    args = ap.parse_args()

    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    if not version:
        die("VERSION file is empty")
    if args.require_tag is not None and args.require_tag != f"v{version}":
        die(f"tag {args.require_tag!r} != v{version} (VERSION file) — bump VERSION"
            " in the release PR (issue #113)")
    print(f"release version: {version}")

    pio = find_pio()
    # esptool + boot_app0 ship inside PlatformIO packages that only exist
    # AFTER the first `pio run` downloads the toolchain — resolve them lazily
    # (a cold CI runner has an empty ~/.platformio until the build).
    esptool = None
    boot_app0 = None
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    results = []
    catalog_targets = []
    for t in TARGETS:
        env = t["env"]
        if not env_exists(env):
            if t["optional"]:
                print(f"release.py: WARNING: [env:{env}] not in platformio.ini — skipped")
                continue
            die(f"[env:{env}] missing from platformio.ini")
        if not args.skip_build:
            run([pio, "run", "-e", env])
        if esptool is None:
            esptool = find_esptool()
            boot_app0 = find_boot_app0()

        bdir = build_dir_for(env)
        images = [bdir / "bootloader.bin", bdir / "partitions.bin",
                  boot_app0, bdir / "firmware.bin"]
        for img in images:
            if not img.is_file():
                die(f"missing build artifact: {img}")
        app = images[3]
        app_size = app.stat().st_size
        if app_size < 100 * 1024:
            die(f"{env}: firmware.bin suspiciously small ({app_size} bytes)")

        merged = OUT_DIR / f"{env}-{version}.bin"
        # "keep" preserves each image's own header flash params — see the
        # module docstring for why forcing them corrupts the S3 bootloader.
        cmd = esptool + ["--chip", t["chip"], "merge_bin", "-o", str(merged),
                         "--flash_mode", "keep", "--flash_freq", "keep",
                         "--flash_size", "keep"]
        for off, img in zip(t["offsets"], images):
            cmd += [off, str(img)]
        run(cmd)

        size = merged.stat().st_size
        if not (0x10000 + app_size <= size <= t["max_flash"]):
            die(f"{env}: merged image size {size} fails sanity check")

        manifest = {
            "name": t["name"],
            "version": version,
            "new_install_prompt_erase": True,
            "builds": [
                {
                    "chipFamily": t["chipFamily"],
                    "parts": [{"path": merged.name, "offset": 0}],
                }
            ],
        }
        mpath = OUT_DIR / f"manifest_{env}.json"
        mpath.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        json.loads(mpath.read_text(encoding="utf-8"))  # round-trip validate
        results.append((env, merged, size, mpath))

        ota = t["ota"]
        if ota:
            if app_size > 0.9 * ota["slot"]:
                die(f"{env}: app {app_size} B exceeds 90% of the {ota['slot']} B"
                    f" OTA slot (#64) — it would not be OTA-flashable")
            app_out = OUT_DIR / f"{ota['id']}-{version}.app.bin"
            shutil.copyfile(app, app_out)
            sha = hashlib.sha256(app_out.read_bytes()).hexdigest()
            entry = {
                "id": ota["id"],
                "name": t["name"],
                "chipFamily": t["chipFamily"],
                "hwRev": ota["hwRev"],
                "version": version,
                "appUrl": ASSET_URL.format(slug=REPO_SLUG, ver=version,
                                           name=app_out.name),
                "appSize": app_size,
                "sha256": sha,
                "sig": sign_image(app_out),
                "minVersion": "0.0.0",
                "mandatory": False,
                "notesUrl": NOTES_URL.format(slug=REPO_SLUG, ver=version),
            }
            catalog_targets.append(entry)
            results.append((f"{env} (ota)", app_out, app_size, None))

    if catalog_targets:
        CATALOG.parent.mkdir(parents=True, exist_ok=True)
        CATALOG.write_text(
            json.dumps({"schema": CATALOG_SCHEMA, "targets": catalog_targets},
                       indent=2) + "\n", encoding="utf-8")
        json.loads(CATALOG.read_text(encoding="utf-8"))  # round-trip validate
        print(f"catalog: {CATALOG.relative_to(ROOT)} ({len(catalog_targets)} targets)")

    print("\n=== release artifacts ===")
    for env, artifact, size, mpath in results:
        print(f"  {env:18s} {artifact.relative_to(ROOT)}  ({size / 1024:.0f} KiB)")
        if mpath:
            print(f"  {'':18s} {mpath.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
