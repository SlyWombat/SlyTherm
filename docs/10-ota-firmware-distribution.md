# OTA Firmware Distribution & Device Catalog

Plan for over-the-air (OTA) firmware updates across the SlyTherm device family,
distributed via **GitHub Releases** with a **versioned catalog of supported
platforms** — modelled on the parallel **SlyLED** project (firmware attached to
GitHub releases; per-target versioned binaries built by CI).

> This is a **plan**; execution is tracked in the linked GitHub issues (the OTA
> epic + children). Nothing here ships without the safety gates in §5.

## 1. Goal

Push new firmware to deployed devices without a USB cable, safely, with a clear
catalog of which hardware targets exist and what versions are available.

## 2. Where we are

- `tools/release.py` already builds **merged single-image binaries** + **ESP Web
  Tools manifests** (`web/installer/`) for **USB** browser-flashing.
- **Missing:** OTA (pull-update over WiFi), GitHub-release CI, a multi-target
  catalog, OTA-capable partition tables, and the on-device update UI.

## 3. Device catalog (targets)

| Target id | Hardware | Role | Furnace-connected |
| --- | --- | --- | --- |
| `wall-s3` | Waveshare ESP32-S3-4.3B | main thermostat + wall UI (`thermostat_s3`) | **yes** (CT-485/24V) |
| `sniffer-s3` | ESP32-S3-4.3B / DevKitC | RX-only CT-485 sniffer | no (listen-only) |
| `satellite-s3` | ESP32-S3 touchscreen | **remote room panel** — display/adjust over MQTT, report room temp+occupancy as a sensor; **no CT-485/actuator** | **no** |

Each target has a hardware-revision tag so the catalog can gate incompatible
images. Satellite panels are the driver for a clean control-vs-UI split (the
`ui/lvgl` binding + `wifi_prov` are already reusable without the control app).

## 4. Architecture (GitHub → catalog → device)

```
 git tag vX.Y.Z ──► GitHub Actions
                     ├─ pio test -e native
                     ├─ build each target (app image + merged USB image)
                     ├─ create GitHub Release vX.Y.Z, attach assets
                     └─ generate firmware/catalog.json (targets → latest ver + url + sha256)
                                     │
 device (OTA client) ◄──────────────┘  fetch catalog.json (raw/release asset)
   compare its target+hwrev+version  →  download app image (HTTPS, GitHub asset)
   verify sha256 + signature         →  esp_https_ota to inactive A/B partition
   mark-for-rollback → reboot        →  self-test → confirm or roll back
```

- **CI** extends `tools/release.py`: adds the OTA **app-only** image per target
  (for `esp_https_ota`) alongside the merged USB image, and emits `catalog.json`.
- **Catalog** (`catalog.json`, served from GitHub): per target
  `{id, name, chipFamily, hwRev, version, appUrl, sha256, minVersion,
  mandatory, notesUrl}`. Superset-compatible with the ESP Web Tools manifests.
- **Device OTA client**: manual "check now" + optional scheduled check; downloads
  over HTTPS from the release asset; **A/B (dual-app) partitions** with rollback.

## 5. Safety (the wall unit controls a gas appliance — docs/04)

Non-negotiable gates for `wall-s3` (satellite/sniffer are exempt):
- **Update only in a safe state:** no active demand, compressor idle, no pending
  call — never mid heat/cool cycle. A staged update waits for idle.
- **A/B partitions + automatic rollback:** boot the new image; if it fails to
  validate/boot, roll back to the previous partition.
- **Signed images:** verify a signature (not just sha256) before apply; reject
  unsigned/mismatched images.
- **Fail-to-no-demand:** an aborted/failed OTA leaves the device in no-demand;
  the certified IFC keeps all combustion interlocks regardless.
- **OEM rollback preserved:** the physical OEM-thermostat fallback path is
  unaffected by any firmware state.

## 6. Issue breakdown (execution)

1. **CI + GitHub Releases** — Actions build all targets on tag, attach assets,
   emit `catalog.json`; extend `tools/release.py`.
2. **Catalog format** — define/publish `catalog.json` (targets, versions, urls,
   hashes, hwRev, mandatory).
3. **On-device OTA client** — esp_https_ota, A/B partitions, version check,
   verify + apply + rollback.
4. **OTA safety gates (wall unit)** — idle-only apply, signing, rollback,
   fail-to-no-demand (§5).
5. **Satellite room panel target** — `satellite-s3` env: UI + `wifi_prov` + MQTT
   sensor/remote, no control app/CT-485; first-class OTA target.
6. **OTA partition table** — dual-app OTA layout on 16 MB flash for all S3 envs
   (the app is ~1.2 MB with LVGL).
7. **Firmware update UI** — Settings → Firmware: current version, check, update
   (with the §5 gate on the wall unit), progress/result.
