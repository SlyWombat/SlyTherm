# OTA Firmware Distribution & Device Catalog

Plan for over-the-air (OTA) firmware updates across the SlyTherm device family,
distributed via **GitHub Releases** with a **versioned catalog of supported
platforms** — modelled on the parallel **SlyLED** project (firmware attached to
GitHub releases; per-target versioned binaries built by CI).

> Execution is tracked in the OTA epic #58 + children (concrete per-issue plans
> live in the issue comments, revised 2026-07-07). Nothing here ships without
> the safety gates in §6.

## 1. Goal

Push new firmware to deployed devices without a USB cable, safely, with a clear
catalog of which hardware targets exist and what versions are available. The
**Controller and the Remote fetch their firmware through the same mechanism.**

## 2. Where we are

- `tools/release.py` builds **merged single-image binaries** + **ESP Web Tools
  manifests** (`web/installer/`) for **USB** browser-flashing;
  `.github/workflows/release.yml` runs it on `v*` tags. No release cut yet.
- **Both deployed targets already run dual-app OTA partition tables** (§5) —
  the on-flash layout needs no change.
- `tools/version_flag.py` injects `SLYTHERM_FW_VERSION`/`SLYTHERM_FW_BUILD`
  from the root `VERSION` file (§3); `lib/OtaCatalog` parses the catalog and
  resolves update decisions (host-tested).
- **Missing:** CI extensions for OTA artifacts (#59), the on-device client
  (#61), signing + safety gating (#62), and the update UI (#65, #112).

## 3. Versioning (issue #113)

- The root **`VERSION` file is the single source of truth** (semver
  `MAJOR.MINOR.PATCH`). PATCH = fixes; MINOR = features; MAJOR = breaking
  contract change (topic map, catalog schema, partition expectations). Bump in
  the PR that completes the work being released.
- `tools/version_flag.py` (PlatformIO pre-script, wired via `extra_scripts`)
  defines `SLYTHERM_FW_VERSION` ("0.3.0") and `SLYTHERM_FW_BUILD`
  ("0.3.0+g1a2b3c4[-dirty]") in every firmware env. Consumers: boot banner,
  Settings screen, HA discovery `sw_version`, the `slytherm/controller/status`
  heartbeat, and the OTA version compare (`lib/OtaCatalog::parseSemver`,
  which ignores `+build` metadata).
- A release tag is always `v<VERSION>`; CI fails the release if they differ.
- **Releases are cut from `main` only** (decided 2026-07-08): both platform
  tracks converge to `main` before any tag, and the workflow refuses a tag
  whose commit is not on `main` — a feature-branch tag can't ship one
  track's in-flight work to the whole fleet. One release train: every tag
  rebuilds all targets from the same tree (per-target trains are a possible
  later refinement; the catalog schema already supports per-target versions).
- **One release train**: Controller (`wall-s3`) and Remote (`remote-p4`)
  binaries share the version of the release that built them.

## 4. Device catalog (targets)

| Target id | Env | Hardware | hwRev | Furnace | OTA |
| --- | --- | --- | --- | --- | --- |
| `wall-s3` | `thermostat_s3` | Waveshare ESP32-S3-4.3B **Controller** | `s3-43b-r1.1` | **yes** | gated (§6) |
| `remote-p4` | `remote_p4` | Guition JC-ESP32P4-M3 **Remote** | `p4-m3-r1` | no | ungated |

The esp32dev bench envs (`sniffer`, `thermostat`) stay **USB-only** — no
catalog entries. A new board revision is a **new `{id, hwRev}` catalog entry**,
never a mutation of an existing one, so old hardware keeps resolving its own
image. (The former `satellite-s3` target was superseded by the Remote — #63.)

`catalog.json` lives at `firmware/catalog.json`, committed to `main` by CI on
each release; devices fetch
`https://raw.githubusercontent.com/SlyWombat/SlyTherm/main/firmware/catalog.json`.
Schema (v1) and parsing semantics: issue #60 + `lib/OtaCatalog`. Per target:
`{id, name, chipFamily, hwRev, version, appUrl, appSize, sha256, sig,
minVersion, mandatory, notesUrl}`.

**Distribution prerequisite:** the repo goes **public when OTA testing starts**
(decided 2026-07-07). Until then devices cannot fetch the catalog or assets;
all other OTA work proceeds (bench-testable against a local HTTPS server).

## 5. Partition tables (issue #64 — verified, no redesign)

- `thermostat_s3` → framework `default_8MB.csv`: `nvs@0x9000`,
  `otadata@0xe000`, `ota_0@0x10000` + `ota_1@0x340000` (3.34 MB each),
  `spiffs@0x670000`, `coredump@0x7F0000`. App ≈1.38 MB → 2.4× headroom.
  Deliberately kept on the 8 MB table despite 16 MB physical flash: it is the
  deployed layout, and **a partition table is never changeable via OTA**
  (USB-only, forever).
- `remote_p4` → `default_16MB.csv`: `ota_0`/`ota_1` @ 6.25 MB each,
  `spiffs@0xc90000`, `coredump@0xFF0000`.
- Per-target coredump offsets (0x7F0000 vs 0xFF0000) matter to the crash-debug
  tooling.
- Boot logs OTA capability (`running=ota_0 next=ota_1`); the client
  hard-disables with a visible reason if no second slot exists.

## 6. Architecture (GitHub → catalog → device)

```
 git tag vX.Y.Z ──► GitHub Actions (#59)
                     ├─ assert tag == VERSION file
                     ├─ pio test -e native
                     ├─ build each target: app-only image + merged USB image
                     ├─ sha256 + ECDSA-P256 signature per app image
                     ├─ create Release vX.Y.Z, attach assets
                     └─ regenerate + commit firmware/catalog.json
                                     │
 device (#61) ◄─────────────────────┘  fetch catalog.json (raw URL)
   lib/OtaCatalog: resolve target+hwRev+version → update decision
   HTTPClient (redirect-follow) streams the asset into Update.h → inactive slot
   sha256 (streamed) + signature verified BEFORE activation
   NVS otaPending → reboot (Controller: deferred to idle, §7)
   boot self-test → confirm, or roll back to the previous slot
```

**One shared client for both roles:** `lib/OtaCatalog` (pure, host-tested) +
`src/ota_client.cpp` (Arduino glue; compiles on core 2.x/S3 and 3.x/P4 — the
TLS class is the single `#if` seam). `HTTPClient` + `Update.h` is used instead
of raw `esp_https_ota`: same underlying `esp_ota_*` machinery, plus streaming
hash verify and GitHub 302-redirect handling that works identically on both
cores.

**Rollback is app-level** (stock Arduino cores don't enable bootloader
rollback): boot with `otaPending` set runs a self-test (control boot gate +
WiFi + broker within ~5 min); failure or a #80 reset-loop-latch trip while
pending → `esp_ota_set_boot_partition(previous)` + reboot. Result recorded to
NVS for the UI.

## 7. Safety (the wall unit controls a gas appliance — docs/04)

Non-negotiable gates for `wall-s3` (`remote-p4` exempt; it has no furnace):
- **Download/verify/stage to the inactive slot: allowed anytime** — the
  running image keeps controlling the furnace throughout. **The REBOOT is the
  gated action**: only at `action == idle`, no active equipment, no pending
  demand, sustained ≥5 min. A staged update waits for idle.
- **A/B partitions + automatic rollback** (§6) — a bad image self-recovers.
- **Signed images:** ECDSA-P256 over the app sha256, signed by CI
  (`OTA_SIGNING_KEY` secret), public key baked into firmware. The Controller
  rejects unsigned/mismatched images; sha256 alone is integrity, not
  authenticity. Key rotation = MAJOR bump + dual-key window (not built until
  needed).
- **Fail-to-no-demand:** boot-to-no-demand is already the codebase invariant
  (docs/04 §3); an aborted/failed OTA leaves the running image untouched. The
  certified IFC keeps all combustion interlocks regardless.
- **OEM rollback preserved:** the physical OEM-thermostat fallback path is
  wiring (docs/03), unaffected by any firmware state.

## 8. Issue breakdown (execution — see each issue for the concrete plan)

1. **#113 versioning** — VERSION → build flag → topics/UI/HA (start-now item).
2. **#60 catalog** — schema + `lib/OtaCatalog` parser/resolve (host-tested).
3. **#59 CI + Releases** — build all targets on tag, sign, attach, commit catalog.
4. **#64 partitions** — verified dual-app already; boot assert + size check in CI.
5. **#61 on-device client** — shared download/verify/apply/rollback.
6. **#62 safety gates** — idle-gated reboot + signing (Controller).
7. **#65 firmware UI** — shared Settings → Firmware screen; #112 Remote persona.
8. **#111 Remote client integration** — P4/C6 specifics, ungated apply.
