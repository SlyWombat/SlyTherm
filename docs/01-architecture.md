# 01 — System Architecture

## 1. Objective

Replace the OEM Dettson communicating thermostat (R02P030, **now discontinued**) with an open-source ESP32 controller that:

- Speaks **ClimateTalk / CT-485** over the furnace's RS-485 bus.
- Runs a local **PID** loop for true modulating control.
- Integrates with **Home Assistant** over MQTT as a Climate entity.
- Is **local-only** (no cloud), and **fails to no-heat** on any fault.

## 2. The one correction that shapes everything

> **We do not "command the gas valve." We send a *heat demand* — a capacity request — over CT-485.**

On a ClimateTalk system, the thermostat is a network node that sends an application-layer **heat demand**. The furnace's certified **Integrated Furnace Control (IFC)** is what actually sequences ignition and modulates the gas valve to its own commanded firing rate, while enforcing **every** combustion safety interlock (flame sense / flame-failure lockout, primary high-limit, rollout, pressure/vent switch, ignition-trial limits). Those interlocks are hard-wired/firmware-locked inside the certified IFC and are **not on the RS-485 bus** — they cannot be disabled over CT-485, and a missing or malformed message does **not** open the gas valve.

This is the architecture that makes the project tractable *and* keeps it on the safe side of the certification boundary. Project language, code comments, and entity names should all reflect "demand," never "valve control." See [`04-safety.md`](04-safety.md).

## 3. Hardware block diagram

```
   Dettson furnace control board (24VAC transformer + certified IFC)
        R    C        1 (A+)     2 (B-)
        │    │         │          │
        │    │         └────┬─────┘  RS-485 bus (also reaches Gree FLEXX HP)
        ▼    ▼              │ short stub tap (no added 120Ω until measured)
   ┌──────────────┐   ┌─────▼───────────┐
   │ Isolated     │   │ 3.3V RS-485     │  TVS + series-R on A/B
   │ AC/DC        │   │ transceiver     │
   │ 24VAC→5V     │   │ (THVD14xx /     │
   │ +fuse +MOV   │   │  MAX3485)       │
   └──────┬───────┘   └──┬───┬───┬──────┘
          │ 5V           RO  DI  DE/RE
          ▼              │   │   │ (pull-down to RX/idle at boot)
   ┌──────────────────────────────────────┐
   │ ESP32 (Arduino/PlatformIO)            │
   │  • CT-485 framer + Fletcher parser    │
   │  • PID loop                           │
   │  • MQTT / HA discovery (Wi-Fi)        │
   │  • layered watchdogs → fail-to-0%     │
   └───────┬───────────────────┬───────────┘
           │ GPIO13/14         │ Wi-Fi
           ▼                   ▼
   DS18B20 (local fallback)   Home Assistant / MQTT broker
                              (primary room-temp sensor, setpoint, control)

   External hardware watchdog ──► forces transceiver silent / NO-DEMAND on hang
```

Full wiring, pin map, and component specs: [`03-hardware-wiring.md`](03-hardware-wiring.md). Parts + links: [`ORDERING.md`](ORDERING.md).

## 4. Software module layout

Modular per the original constraint — RS-485 HW, CT-485 protocol, and control logic are separate units. See [`05-firmware-plan.md`](05-firmware-plan.md) for the phased plan.

```
src/
  main.cpp                 // setup/loop, state machine, watchdog petting
  config.h                // pins, timings, build-time secrets (from .env)
include/
  Ct485Phy.{h,cpp}         // HardwareSerial + DE/RE, 3.5ms frame delimiter, 100ms gap, 300us pre/post
  Ct485Frame.{h,cpp}       // 10-byte header + payload + Fletcher-16 (seed 0xAA, mod 0xFF)
  Ct485Parser.{h,cpp}      // decode msgType/command, status/diagnostics, field dictionary
  Ct485Thermostat.{h,cpp}  // TX path: token/R2R, HEAT_DEMAND, demand refresh watchdog
  PidController.{h,cpp}    // PID + anti-windup + output clamp + max-runtime
  TempSource.{h,cpp}       // MQTT primary + DS18B20 fallback + fault detection
  HaMqtt.{h,cpp}           // MQTT discovery, climate entity, diagnostics publish
  Safety.{h,cpp}           // watchdogs, fail-to-no-demand, heartbeat/deadman
```

## 5. Data flow (steady state, after Phase 3)

1. **Setpoint** arrives from HA over MQTT (or local fallback default if MQTT stale).
2. **Room temp** comes from the HA/MQTT room sensor (primary) or the local DS18B20 (fallback). Both are fault-checked (range / CRC / staleness). If both are bad → demand 0.
3. **PID** computes a 0–100 % demand from (setpoint − temp), with anti-windup and an output clamp.
4. **CT-485 TX** issues `HEAT_DEMAND` (command `0x64`, demand byte = % × 2) when granted the token, and **re-issues it within the demand-refresh watchdog** so the furnace holds the call.
5. **CT-485 RX** continuously decodes furnace `Get Status` / diagnostics → publishes live modulation %, blower state, and fault codes back to HA.
6. **Watchdogs** (software task WDT + external HW WDT + furnace comms-loss timeout) force **no demand** on any failure.

## 6. Scope & phasing (summary)

| Phase | Goal | Bus risk |
| --- | --- | --- |
| 1 | Passive sniff + log raw hex over Wi-Fi | **None** (RX only, DE never asserted) |
| 2 | Decode frames → build field dictionary (find the modulation byte) | None |
| 3 | Active TX: token handling + `HEAT_DEMAND` write | High — requires replacing OEM thermostat |
| 4 | PID + HA/MQTT integration + full failsafes | High |

Decision gate before Phase 3: the field dictionary must be complete and verified, and the OEM communicating thermostat must be **removed** (two masters on the bus is undefined behaviour — see [`02-protocol-climatetalk.md`](02-protocol-climatetalk.md) §risk).

## 7. Key design decisions

- **3.3 V transceiver, not MAX485.** MAX485 is a 5 V part; its `RO` output overstresses the ESP32's 3.3 V GPIO. Use THVD14xx or MAX3485.
- **Isolated AC-input power**, not a bare buck. 24 VAC is AC and is frequently half-wave-rectified with `C` shared on the furnace board; isolation eliminates the shared-`C` short hazard.
- **Sniff-only first.** Zero bus risk; build the whole decoder before transmitting a single byte.
- **Fail-to-no-demand** is the prime directive, enforced in hardware and software (defence in depth).
- **MQTT room sensor primary + DS18B20 fallback**, never a single sensor for a heat-producing loop.

## 8. Honest risk note

This is a **permanent modification to a certified gas appliance**. It can affect warranty, may be regulated work in your jurisdiction, and could complicate insurance after any incident. Keep the OEM thermostat for instant rollback, install a CO alarm, and have a licensed HVAC tech review the final install. Details in [`04-safety.md`](04-safety.md).
