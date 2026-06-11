# 01 — System Architecture

## 1. Objective

Replace the OEM Dettson thermostat (R02P030 is **discontinued**; the current successors are **R02P032/R02P034** — the R02P034's P122–P125/P49 parameter set is the behaviour spec we clone) with an open-source, **wall-mounted ESP32-S3 touchscreen controller** for the full dual-fuel system — Chinook modulating gas furnace **plus** Gree-built heat pump, heating *and* cooling — that:

- Speaks **ClimateTalk / CT-485** over the furnace's RS-485 bus for **40–100 %** modulating gas demand, and commands the heat pump via whichever path the **Phase 0 installed-equipment inventory** establishes (bus demands to an Alizé interface-board node, **or** 24 V relay outputs — see §3).
- Runs a supervisory **mode state machine** (OFF/HEAT/COOL/AUTO/EMERGENCY_HEAT) with dual setpoints, auto-changeover, **dual-fuel balance-point arbitration**, and **compressor protection timers**.
- Fuses multiple remote room sensors (Zigbee/ESPHome via HA→MQTT) Ecobee-style, plus a local outdoor-temperature source.
- Integrates with **Home Assistant** over MQTT as a Climate entity; mobile control via the **HA Companion app**.
- Presents a local **touchscreen UI (LVGL)** at the wall plate where the OEM thermostat lived.
- Is **local-only** (no cloud), and **fails to no-demand** (all channels — heat, cool, fan, compressor — and all relays de-energized) on any fault, while **never violating compressor minimum timers** on the way down or back up.

## 2. The one correction that shapes everything

> **We do not "command the gas valve." We send a *heat demand* — a capacity request — over CT-485.**

On a ClimateTalk system, the thermostat is a network node that sends an application-layer **heat demand**. The furnace's certified **Integrated Furnace Control (IFC)** is what actually sequences ignition and modulates the gas valve to its own commanded firing rate, while enforcing **every** combustion safety interlock (flame sense / flame-failure lockout, primary high-limit, rollout, pressure/vent switch, ignition-trial limits). Those interlocks are hard-wired/firmware-locked inside the certified IFC and are **not on the RS-485 bus** — they cannot be disabled over CT-485, and a missing or malformed message does **not** open the gas valve.

The same boundary applies on the compressor side: **we request capacity, the Gree inverter/controls execute it**. But there is a critical asymmetry — Gree's protections (high/low-pressure trips, ~3-min restart delay, manual-reset latching on repeated faults) are **equipment protections, not a certified safety chain** like the gas IFC's. Our own compressor minimum-on/off timers and outdoor-temperature lockouts are therefore the **primary** protection; the equipment's are backstop only.

This is the architecture that makes the project tractable *and* keeps it on the safe side of the certification boundary. Project language, code comments, and entity names should all reflect "demand," never "valve control." See [`04-safety.md`](04-safety.md).

## 3. Hardware block diagram

The production controller is a **single wall-mounted unit**: an ESP32-S3 touchscreen board (primary pick: Guition JC4827W543C — QSPI display with internal GRAM, which avoids the RGB-parallel PSRAM-contention hazard) installed at the OEM thermostat wall location, where **R/C/1/2 already terminate**. The isolated AC/DC supply, the 3.3 V RS-485 transceiver (explicit DE/RE, pull-down to idle), and the **external hardware watchdog** all live behind the display in the wall enclosure — retained unchanged from the headless design. The **ESP32-DevKitC is no longer the production controller**; it remains the Phase 1/2 sniff rig and a spare.

> **Single-unit vs split is gated, not assumed:** a Phase 3 bench test must show TX-turnaround/token-response jitter inside the CT-485 slot budget with LVGL + Wi-Fi + MQTT all running. If it fails, the documented fallback is a **split** build: headless controller at the furnace, with the display (openHASP/ESPHome panel or HA dashboard) talking MQTT only.

Which of the two variants below is installed is decided by the **Phase 0 physical inventory** (open question — never assume; the design forks here):

**Variant A — communicating (Dettson Alizé ODU + K03085 interface board):** the HP is reached *via the bus* — the interface board inside the furnace (RJ-11 to the IFC; COND-1/2/3 + Gree-proprietary link to the ODU) is a CT-485 node we send HP/cool demands to (node type 0x05 vs 0x09, and modulating vs staged demand, are open questions for sniffing).

```
   Dettson Chinook furnace (24VAC transformer + certified IFC)
   [Variant A only: K03085 interface board inside furnace → Alizé ODU,
    enumerating as a CT-485 node on the same bus]
        │ R / C / 1 / 2 — existing thermostat cable
   ═════╪═══ wall plate (OEM thermostat location) ═══════════════════
        R    C        1 (A+)     2 (B-)
        │    │         └────┬─────┘  CT-485 bus (furnace IFC; + interface-board
        ▼    ▼              │         node in Variant A)
   ┌──────────────┐   ┌─────▼───────────┐  short stub tap
   │ Isolated     │   │ 3.3V RS-485     │  (no added 120Ω until measured)
   │ AC/DC        │   │ transceiver     │  TVS + series-R on A/B
   │ 24VAC→5V     │   │ (THVD14xx /     │
   │ +fuse +MOV   │   │  MAX3485)       │
   └──────┬───────┘   └──┬───┬───┬──────┘
          │ 5V           RO  DI  DE/RE (pull-down to RX/idle at boot)
          ▼              │   │   │
   ┌─────────────────────────────────────────────────┐
   │ ESP32-S3 touchscreen board (wall-mounted)       │
   │  core 0: Wi-Fi / MQTT / LVGL touch UI           │
   │  core 1: CT-485 task (high prio, UART ISR IRAM) │
   │  mode SM • dual-fuel arbiter • CompressorGuard  │
   │  • layered watchdogs → fail-to-NO-DEMAND        │
   └───────┬──────────────────────┬──────────────────┘
           │ 1-Wire               │ Wi-Fi
           ▼                      ▼
   DS18B20s (indoor fallback   Home Assistant / MQTT broker
   + outdoor sensor, north     (remote Zigbee/ESPHome room + occupancy sensors
   wall — OAT rung 2; rung 1    → HA → MQTT; setpoints, schedules,
   is the bus if Variant A,     HA Companion mobile app)
   rung 3 is HA weather)

   External hardware watchdog ──► forces transceiver DE off / NO-DEMAND on hang
```

**Variant B — hybrid (true Gree FLEXX):** a real FLEXX is **not a CT-485 node** — it is commanded by conventional 24 V signals at the cased-coil board (its H1/H2 RS-485 is Gree-proprietary). Gas demand stays on CT-485 (or the 24 V "V" modulation signal — a third path; **never apply raw 24 VAC to V/W2**), and the wall unit adds a relay/sense stage for the HP:

```
   ┌─────────────────────────────────────────────────┐
   │ ESP32-S3 (same wall unit as above)              │
   └──┬──────────────────────────────┬───────────────┘
      │ GPIOs (idle de-energized     │ 24V opto-isolated SENSE inputs:
      │ at boot, like DE/RE)         │ D/W defrost from ODU + Y/G/W
      ▼                              ▼ terminal monitoring
   24V relay / optotriac outputs (normally open, demand = energized):
   Y1, Y2 (if supported), O/B (Gree convention: B = energized in HEATING), G
      │
      └── common relay-coil feed interrupted by the external hardware
          watchdog → relays fail OPEN = no demand even with the MCU hung

   Condensate float switch ──► breaks the cool call independent of software
```

Variant B is only possible as a single wall unit **if enough conductors exist at the wall plate** to carry Y1/Y2/O-B/G plus sense lines — counting them is part of the Phase 0 inventory. If conductors are insufficient, fall back to the split build (relay stage at the furnace/coil end).

Full wiring, pin map, and component specs: [`03-hardware-wiring.md`](03-hardware-wiring.md). Parts + links: [`ORDERING.md`](ORDERING.md).

## 4. Software module layout

Modular per the original constraint — RS-485 HW, CT-485 protocol, and control logic are separate units. See [`05-firmware-plan.md`](05-firmware-plan.md) for the phased plan.

```
src/
  main.cpp                  // setup/loop, task creation + core pinning, watchdog petting
  config.h                  // pins, timings, build-time secrets (from .env)
include/
  Ct485Phy.{h,cpp}          // HardwareSerial + DE/RE, 3.5ms frame delimiter, 100ms gap, 300us pre/post
  Ct485Frame.{h,cpp}        // 10-byte header + payload + Fletcher-16 (seed 0xAA, mod 0xFF)
  Ct485Parser.{h,cpp}       // decode msgType/command, status/diagnostics, field dictionary
  Ct485Thermostat.{h,cpp}   // TX path: token/R2R, demand messages, per-channel refresh watchdog
                            //   — the gas actuator, kept behind the DemandShaper abstraction
  ModeStateMachine.{h,cpp}  // OFF/HEAT/COOL/AUTO/EMERGENCY_HEAT, dual setpoints + deadband,
                            //   auto-changeover dwell
  DualFuelArbiter.{h,cpp}   // balance point, OAT lockouts, comp-to-aux escalation
  CompressorGuard.{h,cpp}   // min on/off, starts/hour, boot hold-off; timers persisted NVS/RTC
  DemandArbiter.{h,cpp}     // SOLE emitter of any demand (bus or relay); heat/cool
                            //   mutual-exclusion invariant lives here
  DemandShaper.{h,cpp}      // interface: GasShaper (40% floor) / HpInverterShaper /
                            //   HpRelayShaper — Phase 0–2 answer selects the implementation
  PidController.{h,cpp}     // survives only as the GasShaper input (anti-windup, clamp, max-runtime)
  SensorFusion.{h,cpp}      // (was TempSource) multi-sensor fusion, occupancy weighting,
                            //   staleness/range/stuck/divergence checks, DS18B20 sanity floor
  OutdoorTempSource.{h,cpp} // rungs: bus (Variant A) → wired DS18B20 → HA weather; fail-cold policy
  RelayOutputs.{h,cpp}      // Variant B: 24V relay outputs, de-energized at boot/reset
  SenseInputs.{h,cpp}       // Variant B: opto-isolated D/W defrost + Y/G/W monitoring
  HaMqtt.{h,cpp}            // MQTT discovery, climate entity, diagnostics publish
  Ui.{h,cpp}                // LVGL touchscreen UI — core 0 task, isolated from the protocol task
  Safety.{h,cpp}            // watchdogs, fail-to-no-demand, heartbeat/deadman
```

**Task placement (dual-core S3):** Wi-Fi / MQTT / LVGL run on **core 0**; the CT-485 protocol task runs **high-priority on core 1** with the UART ISR placed in IRAM (`CONFIG_UART_ISR_IN_IRAM`). A UI/LVGL crash is therefore not a protocol crash; the safety task pets the watchdog only when its own invariants hold, and a whole-chip hang still ends at the external hardware watchdog forcing no-demand.

## 5. Data flow (steady state, after Phase 3)

1. **Dual setpoints** (heat/cool, minimum-delta deadband enforced in firmware) arrive from HA over MQTT or the local touchscreen (dual-bounded local fallback profile if MQTT goes stale).
2. **Fused room temp** comes from `SensorFusion` (remote sensors via HA→MQTT, occupancy-weighted, DS18B20 sanity floor) and **outdoor temp** from `OutdoorTempSource`. All inputs are fault-checked (range / staleness / stuck-value / divergence). Quorum failure → demand 0 — with recovery still honouring compressor min-off.
3. The **mode state machine** (OFF/HEAT/COOL/AUTO/EMERGENCY_HEAT — "EMERGENCY_HEAT" follows the bus BACKUP_HEAT `0x04` naming and is gas-only) turns setpoints + temps into an **effective call**.
4. The **DualFuelArbiter** picks the equipment for that call (balance point, OAT lockouts, comp-to-aux escalation); **CompressorGuard** gates every compressor transition; the **DemandArbiter** is the single point that emits anything.
5. The per-equipment **DemandShaper** converts the call to an actuator command. **PID survives only as the gas shaper's input**, producing a **40–100 %** demand (40 % low-fire floor — demand below the floor snaps to 0/minFire with hysteresis, never dithering below it). HP paths are staged or slew-limited, never raw PID.
6. **CT-485 TX** issues the demand (e.g. `HEAT_DEMAND 0x64`) when granted the token and **re-issues it within the per-channel demand-refresh watchdog** so the equipment holds the call. (Demand payload offsets are unconfirmed until sniffed — see [`02-protocol-climatetalk.md`](02-protocol-climatetalk.md).) In Variant B, HP channels are relay outputs instead, sequenced by the same arbiter/guard.
7. **CT-485 RX** continuously decodes furnace `Get Status` / diagnostics → publishes live modulation %, blower state, and fault codes back to HA (plus `active_equipment` and compressor-timer diagnostics).
8. **Watchdogs** (software task WDT + external HW WDT + comms-loss timeout) force **no demand on every channel** — and cut the relay-coil feed in Variant B — on any failure.

## 6. Scope & phasing (summary)

| Phase | Goal | Bus risk |
| --- | --- | --- |
| 0 | **Installed-equipment inventory**: thermostat + ODU model numbers, interface board present?, IFC DIP states, full low-voltage wiring map, **conductor count at the wall plate** → decides Variant A vs B | **None** (no electronics) |
| 1 | Passive sniff + log raw hex over Wi-Fi (DevKitC sniff rig). Caveat: a conventional install yields a near-silent bus | **None** (RX only, DE never asserted) |
| 2 | Decode frames → build field dictionary — now including **cooling-call, HP-heat, and forced-defrost captures**, demand payload offsets, interface-board node type, and outdoor temp on the bus | None |
| 3 | Active TX: token handling + demand writes; **single-unit bench gate** — measure TX-turnaround jitter with LVGL + Wi-Fi + MQTT running against the CT-485 slot budget | High — requires replacing OEM thermostat |
| 4 | Supervisory stack (mode SM → arbiter → guard → shapers) + HA/MQTT integration + full failsafes | High |

Decision gates: Phase 0 gates the BOM beyond the sniff rig (the design forks on the installed architecture). Before Phase 3, the field dictionary must be complete and verified — including demand payload offsets from real frames — and the OEM communicating thermostat must be **removed** (two masters on the bus is undefined behaviour — see [`02-protocol-climatetalk.md`](02-protocol-climatetalk.md) §risk). Note the project is now **season-dependent**: cooling and defrost captures require the corresponding season.

## 7. Key design decisions

- **Wall-mounted single unit as production controller.** The ESP32-S3 touchscreen lives at the OEM thermostat wall plate (R/C/1/2 already there), with the isolated AC/DC, external transceiver, and hardware watchdog behind the display. Gated by the **Phase 3 bench test** (TX-turnaround jitter with LVGL + Wi-Fi + MQTT running); the documented fallback is a split build (headless controller at the furnace, MQTT-only display). The DevKitC is the sniff rig and spare, not the product.
- **Dual-core task split.** Wi-Fi/LVGL/MQTT on core 0; CT-485 high-priority task on core 1, UART ISR in IRAM — a UI stall must never miss a demand refresh, and if it does, the miss degrades toward *no demand* (the safe direction).
- **3.3 V transceiver, not MAX485.** MAX485 is a 5 V part; its `RO` output overstresses the ESP32's 3.3 V GPIO. Use THVD14xx or MAX3485. External transceiver with **explicit DE/RE** (no auto-direction parts) so the hardware watchdog can force DE off — retained even on a display board that offers onboard RS-485.
- **Isolated AC-input power**, not a bare buck. 24 VAC is AC and is frequently half-wave-rectified with `C` shared on the furnace board; isolation eliminates the shared-`C` short hazard.
- **Sniff-only first.** Zero bus risk; build the whole decoder before transmitting a single byte.
- **Fail-to-no-demand** is the prime directive (all channels, all relays de-energized), enforced in hardware and software (defence in depth) — and it must **never violate compressor minimum timers** on the way down or back up.
- **Reversing-valve polarity default: B = energized in HEATING** (Gree convention — opposite the common O=cool default). Misconfiguring this inverts heat and cool; it is verified at commissioning and only ever switched with the compressor idle.
- **Gas and compressor heat are mutually exclusive** (manufacturer requirement — the coil sits downstream of the heat exchanger). Defrost tempering is the sole sanctioned exception. The interlock is enforced in the DemandArbiter, because nothing at the bus level enforces it.
- **Sensor fusion lives in firmware, not HA templates.** The PID/arbiter input must not depend on HA uptime, and per-sensor health must be visible to the safety layer; follow-me weighting survives an HA outage.
- **Relays fail open** (Variant B): outputs idle de-energized at boot/reset, and the external watchdog cuts the common relay-coil feed — "silence = safe" does not transfer to relays without this.
- **Multi-sensor fusion primary + DS18B20 sanity floor**, never a single sensor for a heat- or cold-producing loop; outdoor temp gets the same fault treatment with a fail-cold policy.

## 8. Honest risk note

This is a **permanent modification to a certified gas appliance and a heat-pump system**. It can affect warranty, may be regulated work in your jurisdiction, and could complicate insurance after any incident. Keep the OEM thermostat for instant rollback (if the original is gone, the current **R02P034** is the certified rollback device), install a CO alarm, and have a licensed HVAC tech review the final install.

Note also that fail-to-no-demand has a flip side: **loss of heat in winter is itself a hazard** (frozen pipes, cold-weather risk). Sustained forced-no-demand raises an escalating alarm, and the OEM thermostat rollback is the recovery path. Details in [`04-safety.md`](04-safety.md).
