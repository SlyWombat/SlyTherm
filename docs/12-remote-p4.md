# The Remote: ESP32-P4 wall unit — implementation notes

Status: **P0–P2 core landed** (epic #94: #95–#102, #114 done; #103 partially —
see "Link behavior"). The plan/architecture document is
`docs/11-remote-node-plan.md` (lands with the Controller-side branch); this
file records what was actually **built** on `feature/remote-p4` and the
decisions/gotchas future work must not re-learn.

## Hardware

GUITION **JC4880P443C_I_W**: ESP32-P4 (rev v1.3, 32MB PSRAM, 16MB flash) +
onboard ESP32-C6 WiFi co-processor over SDIO (`esp_hosted`), 4.3" 480×800
portrait IPS (ST7701 over 2-lane MIPI-DSI), GT911 touch @0x5D (raw I2C,
SDA=7 SCL=8; no reset line), backlight PWM on GPIO23, LCD reset GPIO5.
Native USB-Serial-JTAG (enumerated COM6 during bring-up; **the port resets
the board when a serial monitor detaches** — a "mystery reboot" after a
capture script exits is tooling, not firmware).

The module has an **IPEX (u.FL) connector** besides the ceramic antenna;
bench 2.4GHz was marginal (AUTH_EXPIRE/NO_AP_FOUND at −75..−90dBm) and an
external antenna materially improved association (−59dBm).

**Factory C6 firmware is stale** (esp_hosted slave 2.3.2 vs the 2.12.8 the
Arduino core expects): scan works, association always fails. One-time repair:
`tools/fetch_c6_hosted_fw.sh` + `env:remote_p4_c6_fw_update` pushes the
updated slave firmware over SDIO (no WiFi needed). Any new unit of this board
likely needs this once.

## Build & flash

- Build: `tools/pio_remote.sh run -e remote_p4` — the wrapper isolates
  `PLATFORMIO_CORE_DIR` (pioarduino P4 platform must never touch the shared
  `~/.platformio` the Controller's stock platform lives in) and puts build
  output on native disk.
- Flash (Windows COM port): `python3 -m esptool --chip esp32p4 --port COMx
  --baud 921600 write_flash 0x0 firmware.factory.bin`. Confirm the port by
  MAC first — the Remote is `80:F1:B2:D1:2A:C1`.
- Partition table: `default_16MB.csv` (single 6.25MB app; the default 1.25MB
  slot doesn't fit the UI + fonts).

## The shared wall UI (#114 split + #101 persona)

The Remote compiles the **same** screen modules as the Controller
(`src/ui/ui_{shared,main,overlays,modes}.cpp` + slim `slytherm_ui.cpp`
orchestrator); only the board port differs:

- `src/ui/ui_port.h` — the contract: `portInit()` presents a **logical
  800×480 landscape** LVGL display regardless of glass, `portBacklight()`,
  press-edge `uiNoteTouch()`.
- `src/ui/ui_port_s3.cpp` — Controller (LovyanGFX RGB + CH422G + GT911).
- `src/ui/ui_port_p4.cpp` — this board. Hard-won specifics:
  - Rotation is done **in the port's flushCb** (`drv.rotated=ROT_90`,
    `sw_rotate=0`) through a static SRAM bounce buffer. LVGL's own
    `sw_rotate=1` path allocates its chunk buffer from the LVGL pool — PSRAM
    here — and the pixel-wise rotate into PSRAM starved touch sampling.
  - Touch feeds **raw native GT911 coords**: LVGL v8 rotates pointer input
    itself when `drv.rotated` is set (`lv_indev.c`). Adding a manual
    transform double-rotates.
  - The LVGL pool is **PSRAM-backed** (`LV_MEM_CUSTOM=ps_malloc`, env
    flags). A bigger static pool starved the internal heap (OOM reboots);
    the Controller-sized pool overflowed — and a failed LVGL alloc is
    `while(1)` (task-WDT boot loop), not an error.
  - DSI `draw_bitmap` is async: flush blocks on a done-ISR semaphore with a
    100ms bail-out. Immediate `flush_ready` drops draws; ISR-only ready can
    deadlock LVGL's rotate busy-wait.
- Persona: `DisplayState.hasBus=false` (additive bit, default true) hides
  the Diag RS-485 LISTEN button, CT-485 sections, and Bus link words.
- Entrypoint `src/main_remote.cpp` (#100): pinned `uiTask` + loop-task
  WiFi/MQTT; provides all `extern "C"` UI hooks (sniffer hooks are stubs —
  dead code under hasBus=false). No CT-485/actuator/demand code links
  (verified by map-file grep).

Screenshot verification: the shared screenshot server runs on the Remote too
— `tools/slyshot.py <remote-ip> out.png <tab 0-5>` captures any tab remotely.

## Remote↔Controller link (#102, wire contract fixed by #104)

Codec: `lib/RemoteLink` (pure C++17, host-tested `test/test_remote_link`).
It holds the **Remote-side halves** (echo/status parsers + intent builders);
`lib/HaMqtt` holds the Controller halves (echo builder + intent parser).
Deliberately separate libs while both branches are in flight; consolidate
after merge. Full contract in `RemoteLink.h` (authoritative: `HaMqtt.h`).

| Dir | Topic | Retained | Payload |
|---|---|---|---|
| ↓ | `slytherm/remote/state` | yes | authoritative echo: heatC/coolC/mode/emHeat/hold/holdRemainS/activePreset/fusedTempC/fusedTempValid |
| ↓ | `slytherm/controller/status` | yes | `{"cid","status","version"}` — identity only; **liveness is `slytherm/availability`** (single-Will limit) |
| ↓ | `slytherm/config/presets` | yes | preset roster (parsed with HaMqtt's `parsePresetRosterJson`) |
| ↑ | `slytherm/remote/<id>/intent` | no | `{"id":>0,"type":"setpoints\|mode\|preset\|hold\|clear_hold",...}` |

Link behavior (in `src/remote_mqtt.cpp`):
- Echo → UiModel reconcile, **Controller wins**; a **2s suppression window**
  after a locally-published intent keeps a stale in-flight echo from yanking
  a value mid-adjust (#103's first half).
- Intent `id` is a **monotonic counter persisted in NVS** (`rlink/iid`): the
  Controller's dedupe (`id <= lastId` → drop) survives Remote reboots, so a
  counter restarting at 1 would mute the Remote.
- Vacation/alarm-ack intents have no wire type yet: logged and dropped,
  never banked ("NO intent queuing").
- Remote LWT: retained `slytherm/remote/<id>/status` online/offline.
- Broker discovery: same mDNS chain as the Controller (note: core 3.x renamed
  `MDNS.IP()` → `MDNS.address()`).

Still open from the epic: #103's offline-survival replica, #105-107 (local
SHT31/BME688 + Rd-03D presence, sensor publishing), #108-109 (Discovering-
Controller splash + 5-min degraded-UX state machine), #110 (multi-remote).
WiFi credentials are compile-time (`src/remote_secrets.h`, gitignored) until
provisioning reuses `wifi_prov` + the shared WiFi-setup screen (both already
compile on the P4).
