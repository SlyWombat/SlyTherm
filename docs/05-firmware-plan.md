# 05 — Firmware Plan

**Framework:** Arduino for ESP32, **PlatformIO**. Modular: RS-485 PHY, CT-485 protocol, control logic, and integration each in their own unit. Safety-first throughout (see [`04-safety.md`](04-safety.md)).

## Module layout

```
src/
  main.cpp                 // setup/loop, top-level state machine, watchdog petting
  config.h                // pins, timings, build-time secrets (generated from .env)
include/
  Ct485Phy.{h,cpp}         // HardwareSerial wrapper: DE/RE, 3.5ms frame delimiter,
                           //   100ms inter-packet gap, 300us pre/post-drive hold; non-blocking RX
  Ct485Frame.{h,cpp}       // 10-byte header struct + payload + Fletcher-16 (seed 0xAA, mod 0xFF)
  Ct485Parser.{h,cpp}      // decode msgType (0x02/0x82, 0x03, 0x06/0x86, 0x1D/0x9D...),
                           //   field dictionary, status/diagnostics extraction
  Ct485Thermostat.{h,cpp}  // TX path: coordinator/R2R/token handling, HEAT_DEMAND(0x64),
                           //   demand-refresh watchdog
  PidController.{h,cpp}    // PID + integral anti-windup + output clamp + max-runtime
  TempSource.{h,cpp}       // MQTT primary + DS18B20 fallback + fault detection (CRC/range/staleness)
  HaMqtt.{h,cpp}           // MQTT discovery, climate entity, diagnostics publish
  Safety.{h,cpp}           // task WDT, external HW-WDT pet, fail-to-no-demand, heartbeat/deadman
```

## Phases

### Phase 1 — Passive bus sniffing & logging  *(bus risk: none)*

**Goal:** listen without transmitting, identify baud, dump raw hex over Wi-Fi.

- `Ct485Phy` in **permanent receive** — `DE`/`RE` held to receive; **never assert TX**.
- Initialize `HardwareSerial` (UART2) on the chosen RX/TX pins; **non-blocking reads** to avoid dropping frames.
- Auto-baud helper: try **9600** then **38400**; correct baud yields clean ≥ 3.5 ms frame gaps and passing Fletcher.
- Stream incoming bytes as HEX over **WebSocket or MQTT** to a PC.
- Optional cross-check with a PC-side USB-RS485 adapter + logic analyzer.

**Done when:** raw frames captured at the right baud, framed on the 3.5 ms gap, logged losslessly.

### Phase 2 — Packet decoding & node identification  *(bus risk: none)*

**Goal:** parse raw hex into CT-485 structures; build the field dictionary.

- `Ct485Frame` parse + **Fletcher validate** (reference impl in [`02-protocol-climatetalk.md`](02-protocol-climatetalk.md) §3); test against `kpishere/Net485` `diag/logs/*.log` and the openHAB sample frame.
- `Ct485Parser` decodes header fields and labels by `msgType`; for `0x03` decode command code (offset 4) + demand byte; for `0x82`/`0x9D` dump payload as a byte grid.
- Identify: furnace node ID, Gree HP node ID, coordinator, manufacturer IDs.
- **Stimulus-response:** walk the OEM thermostat low→high fire; diff successive `0x03`/`0x82` payloads to **localize the modulation byte**; cross-check demand÷2 vs displayed % and fire rate.
- Map faults (`0x86`/`0x05`) by triggering a known fault per OEM procedure.

**Done when:** a documented field dictionary (msgType, command, offset → meaning) exists, and the modulation byte is **confirmed**. *This is the gate before any TX.*

### Phase 3 — Active transmission ("virtual thermostat")  *(bus risk: high)*

**Pre-req:** field dictionary complete + **OEM communicating thermostat removed** (no two masters).

**Goal:** mimic a CT-485 thermostat and take the bus.

- Implement RS-485 **TX** honouring 300 µs DE pre/post-drive and the 100 ms inter-packet gap.
- Implement **coordinator/token** handling: respond to R2R (`0x00`) / Token Offer (`0x77`); only TX when granted.
- Network join: Version Announcement (`0x78`), Node Discovery reply (`0xF9`), Set Address (`0xFA`) → become node 1.
- Send a successful **`HEAT_DEMAND` (`0x64`, demand = % × 2)** at low fire first, with a hard software clamp on the demand byte, and **refresh within the demand-watchdog timer**.
- Verify ACK (`0x06`) / handle NAK (`0x15` bad CRC, `0x1B` invalid/pairing).

**Done when:** a clamped low-fire demand is accepted (ACK) and the furnace responds, with no bus corruption.

### Phase 4 — Control logic & Home Assistant  *(bus risk: high)*

**Goal:** PID loop + HA Climate entity + full failsafes.

- `PidController`: input = room temp (`TempSource`), setpoint = HA; output = clamped 0–100 % demand; anti-windup; max-runtime; anti-short-cycle.
- `TempSource`: MQTT primary + DS18B20 fallback + fault detection → demand 0 if both bad.
- `Safety`: task WDT + external HW-WDT pet + comms-loss deadman; boot to no-demand; brownout enabled.
- `HaMqtt`: MQTT discovery → HA recognizes an HVAC Climate entity; publish live modulation %, blower, fault codes, health. See [`06-home-assistant.md`](06-home-assistant.md).

**Done when:** the commissioning checklist in [`04-safety.md`](04-safety.md) §6 passes end-to-end.

## PlatformIO setup

`platformio.ini` skeleton is in the repo root. Secrets (Wi-Fi/MQTT) come from `.env` → generated `config.h` at build time; **never commit secrets** (`.env` is gitignored).

## Testing strategy

- **Offline unit tests** for `Ct485Frame` (Fletcher) and `Ct485Parser` against captured logs — runnable on the `native` PlatformIO env, no hardware.
- **Bench tests** for every failsafe (unplug sensor, kill MQTT, hang the task, brownout) before the controller ever drives a live furnace bus.
- **Scope/logic-analyzer** verification that the bus stays silent during ESP32 boot/reset.
