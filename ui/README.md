# ui/ — LVGL touchscreen UI (NOT in the build yet)

## Why this directory is not compiled

Decision **#38** (ESPHome + LVGL external-component vs custom PlatformIO +
LVGL — see `docs/05-firmware-plan.md`, "Framework — open decision") is still
**open**, and the two stacks supply LVGL, display/touch drivers, OTA and
provisioning in completely different ways. Committing to either binding now
would be rework later.

So the split is:

| Part | Where | Status |
|---|---|---|
| View-model + intent queue (`UiModel`, `DisplayState`, `UiCommands`) | `lib/UiModel/` | **Built + unit-tested** (`pio test -e native -f test_ui_model`) — pure C++17, survives #38 either way |
| LVGL v9 screens + binding | `ui/lvgl/` | Skeleton written against the plain LVGL v9 C API; **not compiled by any env** |

`ui/` sits outside PlatformIO's `src/`/`lib/`/`test/` scan, so nothing here
can leak into firmware or test builds by accident. The skeleton compiles
against nothing until a future env adds it deliberately.

Safety framing (binding either way, `docs/04-safety.md` §1c): the UI has **no
demand authority**. Screens render `DisplayState` snapshots and call
`UiCommands` only; commands enqueue bounded `UiIntent`s that the control task
pops and re-validates. A crashed or garbage UI can at worst enqueue rejected
intents.

## Files

- `lvgl/screens.h` / `lvgl/screens.cpp` — widget construction only: home dial
  (dual-setpoint heat/cool arcs + fused temp), mode/preset rows, sensors page
  (fusion table), diagnostics page (alarms + link health), settings page.
- `lvgl/binding.cpp` — the glue shape: `binding_render()` maps
  `DisplayState` → widgets using the dirty-group bits (renders only changed
  groups, then `clearDirty`), and LVGL events → `UiCommands`
  (`adjustSetpoint` / `setMode` / `setPreset`).

## Hardware target

**Guition JC4827W543C** — ESP32-S3, 4.3" 480×272 IPS, **NV3041A** panel over
**QSPI**, **GT911** capacitive touch (I²C). Landscape orientation assumed by
the layout.

## How to compile it later

### Option A — decision #38 picks ESPHome

The CT-485 stack becomes an external component; ESPHome supplies LVGL via its
`lvgl:` component plus the `display:`/`touchscreen:` platforms:

- `display:` platform `qspi_dbi` (NV3041A is supported there), 480×272,
  with the JC4827W543 QSPI pin map.
- `touchscreen:` platform `gt911` on I²C.
- Port `screens.cpp`/`binding.cpp` into the external component (or express
  the simpler screens in `lvgl:` YAML and keep only the binding in C++).
  `lib/UiModel` is included unchanged by the component.

### Option B — decision #38 picks custom PlatformIO

Add a display env to `platformio.ini` (do **not** touch `sniffer`/`native`):

```ini
[env:display]
platform = espressif32
board = esp32-s3-devkitc-1   ; JC4827W543C: 8 MB flash / PSRAM variant flags
framework = arduino
build_flags =
    -std=gnu++17
    -DLV_CONF_INCLUDE_SIMPLE
    -Iui                      ; picks up ui/lv_conf.h (create it from
                              ;  lvgl's lv_conf_template.h, v9.x)
    -DBOARD_HAS_PSRAM
lib_deps =
    lvgl/lvgl@^9.2
build_src_filter = +<*> +<../ui/lvgl/>   ; the deliberate opt-in
```

Driver layer: `esp_lcd` QSPI panel IO for the NV3041A (Arduino-GFX and the
JC4827W543 demo projects have the init sequence and pin map) + a GT911 I²C
driver, glued to LVGL v9 with `lv_display_create(480, 272)` +
`lv_display_set_flush_cb` and `lv_indev_create()` (`LV_INDEV_TYPE_POINTER`).

`lv_conf.h` knobs that matter on this board:

- `LV_COLOR_DEPTH 16` (RGB565; check whether the NV3041A path needs byte swap
  — `LV_COLOR_16_SWAP` equivalent is handled per-display in v9 via
  `lv_draw_sw_rgb565_swap` in the flush cb).
- Draw buffer ≈ 1/10 screen (480×27×2 ≈ 26 kB ×2) in internal RAM; full
  frame buffer in PSRAM optional.
- `LV_MEM_SIZE` ≥ 64 kB; `LV_USE_OS LV_OS_FREERTOS` if LVGL runs threaded.
- Enable: `LV_USE_ARC`, `LV_USE_BUTTONMATRIX`, `LV_USE_TABVIEW`,
  `LV_USE_TABLE`, `LV_USE_LIST`, `LV_USE_LABEL`, `LV_FONT_MONTSERRAT_28`.
- UI task pinned to **core 0**; CT-485 task stays high-priority on core 1
  (`docs/05` concurrency plan). Single-unit viability is still gated on the
  Phase 3 TX-turnaround jitter measurement.

### Native simulator (either option, develop screens without hardware)

LVGL v9 + SDL2 runs the exact same `screens.cpp`/`binding.cpp` on the PC:

1. Start from the upstream `lv_port_pc_eclipse` (CMake/SDL) project, or add a
   PlatformIO `[env:simulator]` on `platform = native` with `lvgl/lvgl@^9.2`,
   `-lSDL2`, and `-DLV_USE_SDL=1`.
2. Create the display with `lv_sdl_window_create(480, 272)` and the mouse
   with `lv_sdl_mouse_create()`.
3. Drive it with a stub control loop: instantiate `UiModel`, call
   `screens_create` + `binding_attach`, feed it fake `DisplayState` updates,
   and print intents popped via `popIntent()` to verify the event wiring.

This simulator path is also the cheapest way to iterate on layout before #38
is resolved, since it has zero dependency on the chosen firmware stack.
