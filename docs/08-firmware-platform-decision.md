# 08 — Firmware platform decision: ESPHome external component vs custom PlatformIO + LVGL

**Status: DECIDED (issue #38, 2026-06-11) — custom PlatformIO + LVGL for the production wall unit; ESPHome optionally for non-safety accessory nodes.** This resolves the open decision at the top of [`05-firmware-plan.md`](05-firmware-plan.md).

## 1. Context

The production controller is the wall-mounted ESP32-S3 touchscreen ([`03-hardware-wiring.md`](03-hardware-wiring.md) §8, Guition JC4827W543C class) that is **also the CT-485 bus controller for a live gas furnace**. docs/05 left two stacks open:

1. **ESPHome + LVGL** with the CT-485 stack as an external component (esphome-econet precedent) — OTA + safe mode, provisioning, HA integration, LVGL glue "for free".
2. **Custom PlatformIO (Arduino/ESP-IDF) + LVGL** — maximum control of task pinning, ISR placement, and watchdog policy; more by-hand work.

This spike built **both legs with real evidence**: a real, compiling ESPHome external component wrapping the repo's protocol libs, and a verification build of the already-working custom application.

## 2. What the spike actually built and measured

### 2a. ESPHome leg — `spike/esphome/` (evidence, not product; see `spike/README.md`)

Built: `ct485_sniffer`, a real external component wrapping `lib/Ct485Frame` + `lib/Ct485Parser` for RX-only frame logging (UART bus, valid-frame-count sensor, last-decode text sensor), mounted by `spike/esphome/ct485-spike.yaml` (esp32dev, wifi/api/ota/captive-portal stubs).

| Measurement | Value |
|---|---|
| ESPHome version | 2026.5.3 (pip, Python 3.12) |
| Compile result | **SUCCESS** (`esphome compile`, 146 s on the second, uncontended attempt) |
| `firmware.bin` | **885,936 B** (48.3 % of the 1.75 MB app partition); static RAM 48,168 B (14.7 %) |
| Component LOC | **183** (50 py + 55 h + 78 cpp) + 62 YAML |
| Repo-lib reuse | `Ct485Frame.cpp` / `Ct485Parser.cpp` compiled **unmodified** from `lib/` via `platformio_options: lib_extra_dirs` + forcing `lib_ldf_mode: chain` (ESPHome generates `off`) |
| Generated surface | 384-line generated `main.cpp`; **206 framework C++ files across 22 components** for this minimal node |
| Toolchain | ESPHome 2026.x builds **`framework = arduino, espidf`** (Arduino 3.3.8 as an IDF 5.5.4 component, pioarduino platform 55.03.38) |

**Friction list (all actually hit):**

1. **First compile FAILED** (“Missing framework-arduinoespressif32-libs package”): ESPHome’s pioarduino platform and the repo’s vanilla `espressif32` platform share `~/.platformio/packages` and raced/clobbered each other while a `pio run` was running concurrently. The clobbering is **bidirectional** — after ESPHome installed its packages, the repo’s own `pio run -e thermostat` failed with `freertos/FreeRTOS.h: No such file` until rebuilt in an isolated `PLATFORMIO_CORE_DIR`. Running both stacks on one dev machine requires permanent core-dir separation.
2. ESPHome’s generated `lib_ldf_mode: off` had to be overridden for the pure-C++ `lib/` tree to resolve — undocumented, found by inspecting the generated `platformio.ini`.
3. The component API gives **no UART RX-timeout/gap event** — `uart::UARTDevice` is polled from `loop()`. The spike infers the 3.5 ms CT-485 frame gap from `micros()` at loop granularity (~1–16 ms cooperative scheduler): fine for sniffing (packets ≥ 100 ms apart), **wrong for the production token path** — back-to-back frames drained in one `loop()` pass merge. ESPHome’s `uart` does expose `flow_control_pin` (→ `UART_MODE_RS485_HALF_DUPLEX`) and a symbol-time `rx_timeout`, but **not** `uart_set_rs485_hd_opts()` (the 300 µs DE pre/post-drive spec) and not the RX-timeout interrupt. The production CT-485 stack would own the UART with raw ESP-IDF calls *inside* the component either way.
4. Wi-Fi credentials live in the YAML (or `secrets.yaml`) — manageable, but a second secrets mechanism beside the repo’s `*_secrets.h` pattern.

### 2b. Custom leg — what `src/main_thermostat.cpp` already proves

No new code was needed; the bootable application (issue #55) already demonstrates on real hardware patterns ESPHome cannot give us without bypassing it:

- **Deliberate dual-core layout**: `mqtt` (core 0, prio 2), `control` (core 1, prio 3), `ct485` (core 1, prio 4 — above control so a slow control cycle cannot starve bus timing). Exactly the docs/01 §4 / docs/05 concurrency plan.
- **Full Phase-4 control pipeline** at fixed cadence with SafetySupervisor invariant reporting gating the external-WDT pet, demands-disabled triple gate, reset-loop accounting, persisted monotonic clock, NVS persist-on-change.
- **HA integration already MQTT-by-design** (discovery, LWT availability, diff-publishing) — chosen in docs/06 for HA-outage tolerance; ESPHome’s native API would *replace* a working, deliberately-MQTT design, not fill a gap.
- 19 pure-C++ `lib/` modules, host-tested (`pio test -e native`), 6,909 LOC — the safety core is already framework-independent.

Verification build (this spike): `pio run -e thermostat` **SUCCESS** in an isolated core dir — `firmware.bin` **447,008 B** (Flash 34.1 %, static RAM 35,000 B / 10.7 %), i.e. **roughly half the ESPHome spike's footprint while carrying the entire control pipeline** the sniffer-only ESPHome node lacks. Build needed a real fix first: the unpinned `platform = espressif32` now resolves to 7.0.1 / Arduino core 3.x, which moved `esp_efuse_mac_get_default()` to `esp_mac.h`. Fixed with a guarded include in `main_thermostat.cpp`. **Pin the platform version** (review item below).

What custom must build that ESPHome gives free:

| ESPHome freebie | Custom equivalent | Honest effort |
|---|---|---|
| OTA + automatic IDF rollback (2026.1+, default-on) + safe mode | esp_https_ota / ArduinoOTA + IDF `app_update` rollback API + dual app partitions; boot-validation hook already exists (SafetySupervisor boot gate) | days, not weeks; rollback marries naturally to our boot gate |
| Provisioning (captive portal + Improv BLE built in) | Improv-BLE library or WiFiManager-class captive portal | days; BLE stack RAM cost applies to both stacks |
| Wi-Fi reconnect/logging | already in `mqttTask` (retry timers, LWT); serial + MQTT logging exist | mostly done |
| HA-native API | **not wanted** — MQTT is the design (docs/06) | n/a |
| Display driver glue (NV3041A QSPI + GT911 have working ESPHome configs) | esp_lcd/LovyanGFX + LVGL bring-up by hand | the real cost of custom: ~1–2 weeks of display/touch/LVGL plumbing before UI work starts |
| Web installer / dashboards | not required for a wall thermostat | n/a |

## 3. Comparison

| Criterion | ESPHome external component | Custom PlatformIO + LVGL |
|---|---|---|
| **Safety-path auditability** | Between SafetySupervisor and the hardware sit the ESPHome scheduler/main loop, its WDT policy, and 200+ generated framework files that **change monthly**. Demonstrated hazard: the 2026.4.0 main-loop/WDT rework made a popular RS-485-class component (emporia_vue) trip the task WDT and **OTA-rollback-loop** until patched. Pinning ESPHome forever restores auditability but forfeits the maintenance benefit. | Audit surface = 1,632-line main + 19 host-tested libs, all repo-pinned. The WDT pet, boot gate, and goSilent() path are first-party code reviewed in docs/04 terms. **Clear win.** |
| **CT-485 timing control** | Possible only by *bypassing* ESPHome: spawn our own pinned FreeRTOS task (supported; thread-safe bridges exist: `defer()`, `enable_loop_soon_any_context()`) and drive the UART with raw IDF calls — at which point ESPHome contributes nothing to this path but still owns the loop/WDT around it. No RX-timeout event, no `uart_set_rs485_hd_opts()`, IRAM ISR config via sdkconfig overrides. | `xTaskCreatePinnedToCore` layout already running; UART ISR-in-IRAM, RS-485 HD opts, RX-timeout interrupt all direct. The Phase-3 jitter bench gate measures exactly this stack. **Clear win.** |
| **LVGL fit** (custom arc setpoint, alarm overlays, PIN lock) | LVGL 9.5 built in (2026.4); arc widget binds to number/sensor; JC4827W543C has a published working config (display+touch driver glue free). But **custom widgets are not officially supported** (feature-request #3016 open); complex flows become YAML+lambda sprawl; `lib/UiModel` (lock/PIN/backoff state) would need bridging. | Full LVGL C API: custom arc widget, top-layer alarm overlays, UiModel binding are straightforward; cost is hand-rolling display/touch bring-up ESPHome would have given free. **Modest win for custom on our specific UI; ESPHome wins for stock UIs.** |
| **OTA / provisioning** | Best-in-class, free: OTA + IDF rollback + safe mode + captive portal + Improv BLE. | Must be built (see table above); rollback integrates with the existing boot-validation gate, which is arguably *safer* than ESPHome’s generic 60 s mark-valid window. **ESPHome wins on effort; custom matches on outcome with ~1–2 weeks work.** |
| **HA integration** | Native API instant; but our design is deliberately MQTT (HA-outage tolerance, docs/06) and `HaMqtt` + discovery is already built and running. | Already done. **Tie, leaning custom because the work exists.** |
| **Maintenance burden** | Monthly releases; framework reworks land under the component (2026.1 IDF-default switch, 2026.4 WDT/CPU-freq rework); toolchain collides with the repo’s PlatformIO setup unless core dirs are separated; two secrets mechanisms. Upside: community maintains drivers/OTA/provisioning. | We own everything we ship, but it is small, pinned, and host-tested; today’s espressif32 7.0.1 drift cost one guarded include. **Custom wins for a safety device; ESPHome wins for fleets of accessories.** |

## 4. Recommendation

**Build the production wall unit on the custom PlatformIO + LVGL stack.** The deciding argument is the safety-auditability + timing-control column: on ESPHome, every mechanism docs/04 cares about (WDT discipline, core pinning, IRAM ISR, DE timing, boot-to-no-demand) is either bypassed framework code or first-party code wrapped in a framework whose scheduler and watchdog semantics demonstrably change between monthly releases. The spike shows the CT-485 production path would be written as raw IDF-in-a-task *either way* — ESPHome would only ever host it, never help it. Meanwhile the things ESPHome genuinely gives free (OTA+rollback, Improv, display glue) are days-to-weeks of bounded, once-only work that integrates *better* with our existing SafetySupervisor boot gate.

**Hybrid corollary (honest part):** ESPHome earned its keep for everything that is *not* the wall unit. The spike’s 183-LOC component delivering a working HA-visible sniffer over our unmodified libs is a genuinely excellent power-to-effort ratio. Use ESPHome freely for:

- **Accessory room-sensor nodes** publishing `slytherm/sensors/<id>/state` (they are exactly its sweet spot; zero safety role).
- The **bench sniff-rig convenience build** (`spike/esphome/ct485-spike.yaml` already is one).
- An outdoor DS18B20 node for the `OutdoorTempSource` HA-weather rung, if wanted.

The component spike is retained under `spike/` as evidence (see `spike/README.md`), not productized.

## 5. Consequences

- **docs/05**: the framework open-decision paragraph collapses to “custom PlatformIO + LVGL (decided, docs/08)”. Phase 4 gains explicit work items custom no longer gets free: **(a)** OTA + IDF rollback wired to the SafetySupervisor boot gate, **(b)** Improv BLE / captive-portal provisioning (the docs/05 concurrency plan already names WiFiManager/Improv), **(c)** display/touch/LVGL bring-up for the NV3041A+GT911 board.
- **Issue #37 (LVGL thermostat UI skeleton)**: proceeds on the custom stack — LVGL v9 C API directly (custom arc setpoint widget and alarm overlays are unconstrained); display driver bring-up moves into its scope (or a predecessor issue).
- **Issue #39 (Wi-Fi provisioning + OTA policy)**: scope confirmed and slightly enlarged — it must deliver what ESPHome would have provided: OTA transport, IDF rollback policy (mark-valid only after the docs/04 §3 boot gate opens), reset-loop-aware safe mode, and Improv BLE.
- **Repo hygiene from spike findings (review items):** pin `platform = espressif32` to a known version in `platformio.ini` (today’s drift to 7.0.1 broke the build until a guarded `esp_mac.h` include was added — fix already applied to `src/main_thermostat.cpp`); developers running ESPHome on the same machine should use a separate `PLATFORMIO_CORE_DIR`.
- The **Phase 3 single-unit jitter bench gate** (docs/05) is unchanged and remains the fallback trigger to a split architecture — this decision selects the stack, not the topology.

## 6. Evidence index

- `spike/esphome/components/ct485_sniffer/` — the compiled external component (183 LOC).
- `spike/esphome/ct485-spike.yaml` — the esp32dev node config (placeholder credentials).
- Compile artifacts (not retained in repo): `firmware.bin` 885,936 B, RAM 48,168 B, ESPHome 2026.5.3, 2026-06-11.
- Custom verification: `pio run -e thermostat` SUCCESS (isolated core dir), `firmware.bin` 447,008 B / RAM 35,000 B, espressif32 7.0.1 (Arduino core 3.3.x), same date.
- External sources: ESPHome 2026.1 changelog (IDF default + OTA rollback), 2026.4 changelog (LVGL 9.5, loop/WDT rework), esphome feature-request #3016 (custom LVGL widgets, open), emporia-vue-local discussion #409 (2026.4 WDT → OTA rollback loop), esphome.io uart/esp32_improv/safe_mode docs, devices.esphome.io JC4827W543C page, esphome-econet (precedent architecture).
