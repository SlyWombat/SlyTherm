# 05 — Firmware Plan

**Framework — DECIDED (issue #38, 2026-06-11): custom PlatformIO + LVGL** — see [`08-firmware-platform-decision.md`](08-firmware-platform-decision.md) for the full decision record (ESPHome retained only as an option for non-safety accessory nodes). Original framing kept for context: the production controller is the wall-mounted ESP32-S3 touchscreen ([`03-hardware-wiring.md`](03-hardware-wiring.md)), and two stacks were considered for it:

1. **ESPHome + LVGL**, with the CT-485 stack written as an ESPHome **external component** (C++) — the `esphome-econet` project is the precedent and template. Buys OTA + safe-mode/rollback, Wi-Fi provisioning, HA integration, and the LVGL cookbook thermostat UI patterns essentially for free.
2. **Custom PlatformIO (Arduino/ESP-IDF) + LVGL** — maximum control over task pinning and ISRs; more UI work by hand.

**Pure-YAML ESPHome is not sufficient** — the CT-485 token/half-duplex timing requires real code either way. The CT-485 external component was prototyped (issue #38 spike, `spike/esphome/`) and the decision went to **option 2, custom PlatformIO + LVGL** — see [`08-firmware-platform-decision.md`](08-firmware-platform-decision.md). The Phase 1–2 **sniff rig stays Arduino-for-ESP32 + PlatformIO on the ESP32-DevKitC** regardless. Modular either way: RS-485 PHY, CT-485 protocol, control logic, UI, and integration each in their own unit. Safety-first throughout (see [`04-safety.md`](04-safety.md)).

## Module layout

```
src/
  main.cpp                 // setup/loop, top-level supervisor, watchdog petting
  config.h                // pins, timings, build-time secrets (generated from .env)
include/
  Ct485Phy.{h,cpp}         // UART wrapper: DE/RE, 3.5ms frame delimiter,
                           //   100ms inter-packet gap, 300us pre/post-drive hold; non-blocking RX
  Ct485Frame.{h,cpp}       // 10-byte header struct + payload + Fletcher-16 (seed 0xAA, mod 0xFF)
  Ct485Parser.{h,cpp}      // decode msgType (0x02/0x82, 0x03, 0x06/0x86, 0x1D/0x9D...),
                           //   field dictionary, status/diagnostics extraction
  Ct485Thermostat.{h,cpp}  // TX path: coordinator/R2R/token handling, demand messages,
                           //   demand-refresh watchdog; the GAS ACTUATOR behind DemandShaper
  ModeStateMachine.{h,cpp} // OFF / HEAT / COOL / AUTO / EMERGENCY_HEAT (bus BACKUP_HEAT naming,
                           //   gas-only); dual setpoints + deadband; auto-changeover dwell
  DualFuelArbiter.{h,cpp}  // balance point, OAT lockouts, comp-to-aux escalation,
                           //   gas/compressor mutual exclusion (defrost = sole exception)
  CompressorGuard.{h,cpp}  // min-off / min-on / starts-per-hour timers, NVS/RTC persistence,
                           //   post-boot hold-off when state unknown
  DemandArbiter.{h,cpp}    // SOLE emitter of any demand (bus message or relay); enforces
                           //   never-heat-and-cool and changeover sequencing invariants
  DemandShaper.{h,cpp}     // one interface, three impls: GasShaper (40% floor),
                           //   HpInverterShaper, HpRelayShaper — the Phase 0–2 answer selects
                           //   the implementation without rework
  PidController.{h,cpp}    // PID + anti-windup + clamp; survives ONLY as GasShaper input;
                           //   per-call gain sets, integrator reset on mode change
  SensorFusion.{h,cpp}     // (was TempSource) multi-sensor MQTT fusion + occupancy weighting +
                           //   DS18B20 fallback + per-sensor fault detection (staleness/range/stuck)
  OutdoorTempSource.{h,cpp}// OAT rungs: bus sensor → wired outdoor DS18B20 → HA weather;
                           //   staleness + cross-rung disagreement alarm; fail-cold policy
  RelayOutputs.{h,cpp}     // Case B: Y1/Y2/O-B/G dry contacts; de-energized at boot/reset;
                           //   common coil feed behind the HW watchdog cut
  SenseInputs.{h,cpp}      // Case B (and instrumentation in Case A): opto-isolated 24 V sense
                           //   (D/W defrost, Y/G/W monitoring), condensate float switch
  Ui.{h,cpp}               // LVGL touchscreen UI task (core 0): display/touch drivers, screens,
                           //   setpoint/mode input → requests to ModeStateMachine; NO demand authority
  HaMqtt.{h,cpp}           // MQTT discovery, climate entity, diagnostics publish
  Safety.{h,cpp}           // task WDT, external HW-WDT pet, fail-to-no-demand, heartbeat/deadman
```

## Concurrency plan (single-chip UI + protocol)

The production board runs the UI and the CT-485 stack on one ESP32-S3. The plan (from the HMI review):

- **Core pinning:** Wi-Fi / LVGL / MQTT on **core 0**; the CT-485 task **high-priority on core 1**.
- **UART ISR in IRAM** (`CONFIG_UART_ISR_IN_IRAM`) so flash-cache stalls (OTA, NVS) can't delay it.
- **3.5 ms inter-frame gap detected in hardware** via the UART **RX-timeout interrupt** (symbol-time configured, the standard Modbus pattern) — no busy-waiting.
- **DE/RE timing in the driver:** ESP-IDF `UART_MODE_RS485_HALF_DUPLEX` asserts/de-asserts RTS→DE around TX in the ISR; `uart_set_rs485_hd_opts()` sets the assert/de-assert delays to meet the 300 µs pre/post-drive spec. The external HW watchdog can still force DE off independently (retained from [`03-hardware-wiring.md`](03-hardware-wiring.md)/[`04-safety.md`](04-safety.md)).
- **Wi-Fi provisioning:** captive portal + **Improv BLE** (ESPHome has both built in; WiFiManager for custom firmware) — Wi-Fi credential changes must not require USB at the wall.
- The UI task crashing must not take down the protocol/safety tasks; the safety task pets the watchdog only when its own invariants hold. Whole-chip hang → external watchdog forces no-demand.

Whether this is good enough is **measured, not assumed** — see the Phase 3 bench gate below.

## Phases

### Phase 0 — Installed-equipment inventory  *(bus risk: none)*

**Goal:** determine which control architecture is actually installed — the entire design forks on it (Path A communicating Alizé + K03085 interface board vs Path B conventional 24 V FLEXX; see [`01-architecture.md`](01-architecture.md) §3).

- Record the **wall thermostat model** (R02P032/R02P034 = communicating; R02P033/other = conventional).
- Open the Chinook blower door: is a **K03085/K03081 interface board** present (RJ-11 cable A00443 to the IFC + COND-1/2/3 wires to the ODU)?
- Read the **ODU nameplate** (MHD/Alizé vs GUD/FXD FLEXX) and rated minimum ambient.
- **Photograph every low-voltage conductor** at the furnace terminal strip (R, C, W1, Y1, Y/Y2, G, V/W2, B), the coil board (Y1in/Y2in/W1in/W2in/Gin, B, D, H1/H2, SA1 DIP), and the ODU.
- **Count the conductors at the wall plate** — the single-unit wall-mounted touchscreen driving Case B relays (Y/G/O-B/W) over the existing thermostat cable needs enough conductors; too few → split architecture fallback.
- Record IFC DIP switches **S4-2/S4-3** (ON/ON = 2-stage stat; OFF/OFF = modulating/communicating).
- ⚠️ During probing: **never apply 24 VAC to V/W2** — documented damage hazard.

**Done when:** architecture (A/B), thermostat model, ODU model, IFC DIP states, and the full low-voltage wiring map (including the wall-plate conductor count) are documented. **This is the decision gate for every BOM purchase beyond the sniff rig** (see [`ORDERING.md`](ORDERING.md)).

### Phase 1 — Passive bus sniffing & logging  *(bus risk: none)*

**Goal:** listen without transmitting, identify baud, dump raw hex over Wi-Fi.

- `Ct485Phy` in **permanent receive** — `DE`/`RE` held to receive; **never assert TX**.
- Initialize `HardwareSerial` (UART2) on the chosen RX/TX pins; **non-blocking reads** to avoid dropping frames.
- Auto-baud helper: try **9600** then **38400**; correct baud yields clean ≥ 3.5 ms frame gaps and passing Fletcher.
- Stream incoming bytes as HEX over **WebSocket or MQTT** to a PC.
- Optional cross-check with a PC-side USB-RS485 adapter + logic analyzer.
- **Caveat (Phase 0 dependent):** a conventional install (R02P033) has **no ClimateTalk traffic to capture** — the bus will be near-silent. Add an RX-activity LED/counter as the diagnostic. Options then: buy an **R02P034 as a reference bus master** (also the certified rollback device), or accept building TX from protocol reconstruction alone with far more conservative bring-up.

**Done when:** raw frames captured at the right baud, framed on the 3.5 ms gap, logged losslessly.

### Phase 2 — Packet decoding & node identification  *(bus risk: none)*

**Goal:** parse raw hex into CT-485 structures; build the field dictionary.

- `Ct485Frame` parse + **Fletcher validate** (reference impl in [`02-protocol-climatetalk.md`](02-protocol-climatetalk.md) §3); test against `kpishere/Net485` `diag/logs/*.log` and the openHAB sample frame.
- `Ct485Parser` decodes header fields and labels by `msgType`; for `0x03` decode command code + demand byte; for `0x82`/`0x9D` dump payload as a byte grid.
- Identify: furnace node ID, coordinator, manufacturer IDs — and whether a **HP/interface-board node** exists at all (node type 0x05 HP vs 0x09 crossover/OBBI is an open question, never assume).
- **Stimulus-response:** walk the OEM thermostat low→high fire; diff successive `0x03`/`0x82` payloads to **localize the modulation byte**; cross-check demand÷2 vs displayed % and fire rate.
- Map faults (`0x86`/`0x05`) by triggering a known fault per OEM procedure.
- Run the new capture campaigns (see [`../captures/README.md`](../captures/README.md)): full Node Discovery enumeration; OEM **cooling call** (does the stat send COOL_DEMAND/FAN_DEMAND to the furnace for blower CFM, or is Y/G hardwired?); SYSTEM_SWITCH_MODIFY on mode change; **forced defrost** ("FO 3 Force Cycle"); Get Sensor Data for outdoor temp; **simultaneous 24 V terminal-state logging** (Y/O-B/W/G/D) against bus traffic.
- **Season dependency:** cooling and defrost captures need the matching season — the schedule is now weather-gated.

**Done when** all of:
1. (a) HP/interface-board node ID/type and command path identified (**bus vs relay**),
2. (b) outdoor temp located on the bus (sensor MDI id 0 via 0x07/0x87) **or ruled out**,
3. (c) defrost signature captured (forced cycle) and tempering ownership observed,
4. (d) cooling-call blower path mapped (bus vs Y/G),
5. (e) **demand payload offsets confirmed from real `0x03` frames** (prior art disagrees: [12] vs [13]/[14] — see [`02-protocol-climatetalk.md`](02-protocol-climatetalk.md) §5a),
6. (f) a bus-vs-24 V-terminal correlation log exists,
7. (g) the **R2R/token-offer → response window timing measured** (the slot budget for the Phase 3 single-unit jitter gate),
8. (h) **mixed-mode IFC behavior tested**: does the IFC honor hardwired Y/G/W while in communicating mode, or do bus comms disable the 24 V terminals? (S3 cooling-CFM DIPs are documented as ignored in communicating mode — this decides whether the hybrid CT-485-gas + relay-HP path is viable),

and a documented field dictionary (msgType, command, offset → meaning) exists, with the modulation byte **confirmed**. *This is the gate before any TX.*

### Phase 3 — Active transmission ("virtual thermostat")  *(bus risk: high)*

**Pre-req:** field dictionary complete + **OEM communicating thermostat removed** (no two masters).

**Goal:** mimic a CT-485 thermostat and take the bus.

- Implement RS-485 **TX** honouring 300 µs DE pre/post-drive and the 100 ms inter-packet gap (via `UART_MODE_RS485_HALF_DUPLEX` + `uart_set_rs485_hd_opts()`, per the concurrency plan above).
- Implement **coordinator/token** handling: respond to R2R (`0x00`) / Token Offer (`0x77`); only TX when granted.
- Network join: Version Announcement (`0x78`), Node Discovery reply (`0xF9`), Set Address (`0xFA`) → become node 1.
- Send a successful **`HEAT_DEMAND` (`0x64`, demand = % × 2)** at **low fire (40%) first** — the Chinook's modulation range is **40–100%** (40% floor; valid demand is 0 or 40–100, never in between) — with a hard software clamp on the demand byte, and **refresh within the demand-watchdog timer**.
- Verify ACK (`0x06`) / handle NAK (`0x15` bad CRC, `0x1B` invalid/pairing).
- **Cooling/HP milestone — Path A (communicating):** a clamped low **COOL_DEMAND (`0x65`)** is ACKed and the ODU responds. **Path B (hybrid/relay):** bench-verified relay sequencing (Y1/Y2/O-B/G) against `CompressorGuard` timers before any live 24 V wiring.

**Phase 3 bench gate — single-unit viability:** on the production ESP32-S3 touchscreen board, **measure token-response / TX-turnaround jitter with LVGL + Wi-Fi + MQTT all active**. If jitter exceeds the CT-485 token slot budget (itself measured in Phase 2 — an open number), **fall back to the split architecture** (headless controller at the furnace + display panel or HA dashboard over MQTT). This is a measurement gate, not a judgment call.

**Done when:** a clamped low-fire demand is accepted (ACK) and the furnace responds, with no bus corruption — and the bench gate above has a recorded pass/fail.

### Phase 4 — Control logic, UI & Home Assistant  *(bus risk: high)*

**Goal:** the full supervisory stack + touchscreen UI + HA Climate entity + full failsafes.

The control path is a pipeline, in this order — no module bypasses the one after it:

```
dual setpoints + fused room temp (SensorFusion) + OAT (OutdoorTempSource)
  → ModeStateMachine (OFF/HEAT/COOL/AUTO/EMERGENCY_HEAT; deadband enforcement)
  → effective call
  → DualFuelArbiter (balance point, lockouts, escalation, gas/compressor exclusion)
  → CompressorGuard (min-off/min-on/starts-per-hour; persisted)
  → DemandShaper (GasShaper | HpInverterShaper | HpRelayShaper)
  → DemandArbiter (sole emitter) → Ct485Thermostat or RelayOutputs
```

- `PidController`: survives **only as the GasShaper input** — output clamped to the **40–100% gas band** (demand below the 40% floor snaps to 0/minFire with hysteresis — never dither below the floor). Per-call gain sets; **integrator reset on mode change**; **suppressed during DEFROST_TEMPER**. HP paths are staged/slew-limited, not PID-driven.
- **DEFROST_TEMPER state:** fixed **35% demand (never PID), 15 min hard cap**, separately configurable fan % and heat %. **Expected default outcome (verify in Phase 2): the interface board handles tempering autonomously**, in which case our policy collapses to "detect, hold steady, ignore the temperature dip." Both branches are documented as a Phase 2 deliverable — ownership of tempering is an open question until captured.
- `SensorFusion`: multi-sensor MQTT fusion (occupancy-weighted) + DS18B20 fallback + per-sensor fault detection → demand 0 if quorum lost (recovery honours compressor min-off).
- **UI (`Ui`/LVGL):** thermostat screens (current temp, dual setpoints, mode, equipment state, alarms) on core 0; the UI **requests** mode/setpoint changes through `ModeStateMachine` — it has no demand authority and its crash must not affect the protocol/safety tasks.
- **Schedules:** **HA owns schedules and profiles**; firmware owns preset pass-through (`home`/`away`/`sleep`) plus an **outage fallback profile in NVS** (stale-MQTT setpoints per the table below). All protection and dual-fuel parameters are **firmware-resident**, exposed as HA-editable MQTT numbers.
- `Safety`: task WDT + external HW-WDT pet + comms-loss deadman; boot to no-demand; brownout enabled; reset-loop lockout per the table below.
- `HaMqtt`: MQTT discovery → HA Climate entity (`heat_cool`, dual setpoints, presets, `active_equipment` diagnostic); publish live modulation %, blower, fault codes, health. See [`06-home-assistant.md`](06-home-assistant.md).

**Done when:** the commissioning matrix in [`04-safety.md`](04-safety.md) §6 passes end-to-end.

## Canonical default parameters

This table is the **single source of truth** for control/protection defaults (mirrored from the design-change plan; [`04-safety.md`](04-safety.md) and [`06-home-assistant.md`](06-home-assistant.md) defer to it). All are firmware-resident, HA-editable within the stated ranges.

| Parameter | Default | Range / notes |
|---|---|---|
| Compressor min OFF time | **300 s** | 240–900 s (Ecobee); FLEXX internal delay is 3 min — ours is the primary protection |
| Compressor min ON time | **300 s** | 60–1200 s |
| Max compressor starts/hour | **3** | — |
| Post-boot/power-restore compressor hold-off | **300 s + 0–60 s jitter** | min-off persisted in NVS/RTC; if unknown, enforce full hold-off |
| Heat/cool differential (call hysteresis) | **0.55 °C (1 °F)** | Ecobee default is 0.5 °F but community-verified to short-cycle; 0.3 °C enter / 0.2 °C exit acceptable alternative |
| Auto-mode heat/cool setpoint deadband (min delta) | **2.8 °C (5 °F)** | hard floor 1.1 °C (2 °F); firmware rejects/clamps HA writes violating it, pushes other setpoint Ecobee-style |
| Auto-changeover dwell | **30 min since opposite call** + trigger sustained ≥10 min + comp min-off satisfied | tunable |
| Manual-change hold (presets) | **until next preset change** | types: until-next-preset / 2 h (7200 s) / 4 h (14400 s) / indefinite; presets blocked while held; preset roster config-driven, ≤8 entries (retained `slytherm/config/presets`) |
| Balance point (economic changeover) | **−8 °C** | mirror R02P034 P124 range −30…15 °C; 2 °C hysteresis |
| Compressor heating lockout (low OAT) | **−20 °C** | set from installed model's submittal sheet (FLEXX rated to −30 °C); range disabled…−30 °C |
| Aux/gas lockout (high OAT) | **+10 °C** | mirror P125-style aux diff −1…−4 °C as staging trigger |
| Config validation | reject lockouts leaving any OAT band with **no permitted heat source** | hard rule |
| Comp-to-aux escalation | droop > **1.0 °C** below setpoint after ≥ **30 min** at ≥95% HP demand → gas; stage back after 60 min + OAT above balance + hysteresis | maps Ecobee Comp-to-Aux Delta/Runtime onto demand saturation |
| Gas modulation floor (minFire) | **40%** | Chinook documented low fire; demand <40% snaps to 0/minFire with hysteresis — never dither below floor |
| Gas min ON / min OFF time | **300 s / 300 s** | 60–900 s each (G14); min-on gates **comfort** stops only (burner held at minFire) — safety stops (sensor fault, invariant trip, max-runtime, watchdog) are always immediate; boot starts the off-timer fresh unless persisted state proves it served |
| Defrost tempering heat demand | **35%** fixed (never PID) | hard cap 15 min; separate configurable fan % and heat % (generalizes, does not "mirror", the OEM single Defrost Fan % param) |
| Sensor staleness timeout | **300 s/sensor** (configurable `max_age_s`, 180–900) | requires 60 s HA heartbeat republish; sensor temps **non-retained** |
| Sensor range gate | 5–40 °C | + stuck-value detection (peer-disagreement policy, next row) + outlier >4 °C from median → exclude + alarm |
| Stuck-value policy (#153 redesign) | suspect window **3600 s** (`setStuckWindowS`, ≥300) · peer delta **0.5 °C** (`kStuckPeerDeltaC`, clamp 0.2–5) · absolute ceiling **36 h** (`kStuckCeilingS`, ≥300 s) | flat ≠ stuck: 0.1 °C-resolution sensors legitimately repeat bit-identically for hours when the room drifts <0.1 °C/h. Declared stuck only when flatness is IMPLAUSIBLE — (a) flat past the suspect window while a **strict majority** of comparable peers (fresh, in-range, participating remotes that were reporting when the flat run began; judged raw-vs-raw so offset edits are invisible) moved ≥ the peer delta since its last change, or (b) flat past the ceiling regardless of peers (sole trigger for single-participant installs). Field-derived (slylog room_temps 07-09..11): all sensors were mutually flat through all three 07-10 overnight dropouts; longest healthy flat run 16 h 15 m (dining) → ceiling ≈ 2× that; lone-peer drifts up to 1.0 °C across such a run → majority, not any. The most-recently-changed participant can never trip (a), so the peer rule alone can never force all-bad |
| Fused-temp coast grace (#153) | **120 s** (`kFusionCoastMaxS`, range 0–600; 0 disables) | fusion JUST lapsed (all participants stale/stuck — original field case: the pre-redesign zero-variance window expiring on flat overnight temps) + last-good younger than the grace + inside the 5–40 °C plausibility band → keep reporting last-good, flagged `coasting` (tier/degraded carried, so a local-degraded coast keeps its cooling lockout), instead of forcing a safety stop; hard-invalid beyond the grace or at boot (no last-good) — a real prolonged failure still fails to no-demand |
| Per-sensor calibration offset | **0 °C** | clamp ±5 °C (`kSensorOffsetMaxC`); applied before the health gates (range/stuck/outlier judge corrected values); edits ramp via the fusion slew limit; includes the local DS18B20 (id `local`) — gap G6 |
| Occupancy window ("follow me") | **30 min** | weight ramps in/out with tau **10–30 min** (tunable; Ecobee constants unpublished) |
| Occupied sensor weight | 2.0 (vs 1.0) | EMA smoothing tau 2–5 min; slew limit ~0.1 °C/min on participant-set changes |
| OAT staleness | 30 min → next rung; all stale → **gas allowed, compressor locked out** (cooling: locked out per the indoor-18 °C policy) | rungs: bus → wired outdoor DS18B20 → HA weather; alarm on >5 °C rung disagreement |
| MQTT-stale fallback setpoints | heat **18 °C** / cool **27 °C**, mode = last user mode (never escalate OFF) | 27–28 °C acceptable; dual-bounded; replaces heat-only "18 °C or OFF" |
| DS18B20 degraded mode | heat-to 16–18 °C floor; cooling **disabled** (or ≥29 °C ceiling); demand capped; loud alarm | explicit mode, not transparent failover |
| Gas max continuous runtime | 4 h → drop + alarm | **HP: no hard runtime cap** — progress alarm only (droop >2 °C for >60 min → alarm + allow gas staging); never cycle the HP as a "failsafe" |
| Reset-loop lockout | ≥3 watchdog/brownout resets in 30 min → latched NO-DEMAND, manual clear | protects compressor from reboot cycling |
| Bus comms-loss deadman | **30 s** (`kBusDeadmanS`) | range 10–120 s; continuous CT-485 silence → SafetySupervisor demand-drop request + critical alarm ([`04-safety.md`](04-safety.md) §3); watchdog petting continues — a chip reset cannot revive a dead bus, and our own silence already drops the calls equipment-side |
| Boot validation grace | **120 s** (`kBootValidationGraceS`) | range 60–600 s; boot gate (sensor OK + setpoint present + config CRC ok, [`04-safety.md`](04-safety.md) §3) still closed after this → critical alarm (loss of heat is itself a hazard, §4); the gate stays closed — the alarm is visibility, not a bypass |
| HP inverter demand shaping (if % path exists) | slew 10%/min, 5% steps, ~30% floor (verify) | HpRelayShaper (likely default): PID→duty at 3 CPH with CompressorGuard timers |
| Staged cooling demand shaping (#140) | stage **30%** (`kCoolStagePct`), min run **780 s** / min rest **480 s** (`kCoolMinOnS`/`kCoolMinOffS`), starts cap **2/h** (`kCoolMaxStartsPerH`), base duty period **4500 s** (`kCoolCyclePeriodS`), proportional band top **0.45 °C** (`kCoolFullDutyErrC`) | field-confirmed single stage engaged at CT-485 demand 30% — request is a RUNTIME fraction (StagedCoolShaper): error→duty→slow cycling; min-run gates **comfort** stops only (invalid input drops immediately, gate still consulted); CompressorGuard timers stay downstream; defaults derived from the 2026-07-09/10 shadow-vs-OEM data (test/test_cool_replay) |
| CT-485 demand refresh-timer byte | **`0x10` = 60 s** (`kDemandRefreshTimerByte`) | per-channel equipment watchdog written into every demand frame (high nibble minutes + low nibble 3.75 s units, [`02-protocol-climatetalk.md`](02-protocol-climatetalk.md) §5a); equipment reverts the channel to off on expiry — the protocol's own deadman |
| CT-485 demand refresh fraction | **0.5** (`kDemandRefreshFraction`) | range 0.1–0.9; re-emit each active demand within this fraction of its refresh window; a **full window** elapsing with no successful re-send (token starved) → starvation alarm **+ go-silent** — never a retry storm ([`04-safety.md`](04-safety.md) §1/§3). Protocol-fixed companions live in `Ct485Core.h`: AutoNet discovery slot 6–30 s, response timeout 3000 ms × 3 attempts (Net485) |
| Relay min inter-transition spacing (Case B) | **500 ms** (`kRelayMinTransitionMs`) | RelaySequencer defers any relay-state change closer than this to the previous one (contact-chatter guard); the emergency drop path (`goSilent()` / HW watchdog coil-cut) is never spaced |
| O/B reversing-valve polarity | **B energized = HEATING** (`kObEnergizedIsHeat`) | Gree convention, opposite the common O=cool default — a wrong guess inverts heat/cool; not HA-editable; verify on installed equipment ([`04-safety.md`](04-safety.md) §6). O/B changes state only with the compressor proven idle (min-off served) |
| UI screen lock level | **settings-only** | or settings+setpoints; the lock blocks **change intents only** — alarms, current temp, and status are never hidden at any level ([`04-safety.md`](04-safety.md) §1c); alarm *ack* stays locked; installer settings pages gated by a separate installer code; installer lockout releases only to the installer code |
| UI auto-relock timeout | **120 s** (`kUiAutoRelockS`) | range 30–600 s; inactivity relocks the screen and expires installer-settings access |
| UI PIN attempt limit / backoff | **5 attempts → 60 s backoff** (`kUiPinMaxAttempts` / `kUiPinBackoffS`) | attempts 3–10, backoff 30–600 s; 4-digit PINs stored as salted FNV hashes (casual tamper resistance, not crypto); forgotten user PIN cleared from HA via `slytherm/cmd/lock_clear` ([`06-home-assistant.md`](06-home-assistant.md) "Screen lock") |
| Smart recovery (pre-heat/pre-cool) | **disabled** (`kRecoveryEnabledDefault`) | ADVISORY only — RecoveryEstimator recommends an early start for the next scheduled target (`slytherm/cmd/next_target`); ModeStateMachine/glue decides, CompressorGuard + DualFuelArbiter still gate every demand; enable after field tuning ([`06-home-assistant.md`](06-home-assistant.md) "Smart recovery") |
| Recovery seed ramp rates | heat **1.0 °C/h** / cool **0.8 °C/h** (`kRecoverySeedHeatCPerH` / `kRecoverySeedCoolCPerH`) | per-{mode, equipment} channel default until that channel has learned from real run segments |
| Recovery max lookahead | **7200 s** (`kRecoveryMaxLookaheadS`) | hard cap on any early-start recommendation |
| Recovery learning gates | segment ≥ **900 s** (`kRecoveryMinSegmentS`) and ≥ **0.2 °C** moved (`kRecoveryMinSegmentDeltaC`) | robust EMA (alpha 0.3, `kRecoveryEmaAlpha`) over per-segment °C/h; plausibility band 0.1–10 °C/h (`kRecoveryRateMinCPerH`/`kRecoveryRateMaxCPerH`); after 3 accepted segments (`kRecoveryOutlierMinSamples`), a rate > 3× off the estimate (`kRecoveryOutlierRatio`) is rejected as an outlier |
| Fused-temp trend (TrendEstimator, #141) | EMA τ **600 s** (`kTrendTauS`), reset gap > **600 s** (`kTrendMaxGapS`), warm-up **300 s** (`kTrendWarmupS`), per-sample clamp **±10 °C/h** (`kTrendMaxSlopeCPerH`) | EMA'd slope of the fused temperature ([`13-dual-fuel-control-research.md`](13-dual-fuel-control-research.md) §2); invalid/dropout samples skipped, long blind gaps discard the trend; ADVISORY input to crossing prediction only |
| Cooling crossing prediction (#141) | **enabled** (`kCoolPredictEnabledDefault`), horizon **900 s** (`kCoolPredictHorizonS`), bias max **0.10 °C** (`kCoolPredictBiasMaxC`), pre-action floor **50 %** (`kCoolPredictMinReqPct`) | when the fused slope projects a deadband crossing within the horizon, a bias (0→max as the crossing nears) is added to the StagedCoolShaper error→duty band so the cycle starts early and gently instead of on the deadband slam; requests below the floor are dropped (a weaker pre-run could duty-cycle off before the call opens and fragment the cycle); COOL mode only (no pre-cool around AUTO changeover); advisory REQUEST shaping — min-run/min-off/starts-cap + CompressorGuard unchanged; validated on the 2026-07-09/10 trace (test/test_cool_replay) |
| Two-ramp recovery fallback (#141) | **disabled** (`kRecoveryTwoRampEnabledDefault`), gas-ramp margin **0.85** (`kRecoveryFallbackMargin`) | Honeywell US 5,622,310 two-ramp scheme ([`13-dual-fuel-control-research.md`](13-dual-fuel-control-research.md) §2): recovery rides the HP-alone ramp; beneath it sits the fallback line at the **derated** learned gas rate arriving at the same target — gas is advised only when the measured temp falls below that line (deep cold ⇒ gas from the start falls out of the same rule); ADVISORY (glue picks the source, GasShaper/DualFuelArbiter/CompressorGuard still gate); **heating validation is a winter task** — enable after field tuning |
| Blower-first pre-circulation (#142) | heat **enabled** / cool **disabled** (`kBlowerFirstHeatEnabledDefault` / `kBlowerFirstCoolEnabledDefault`), fan **25 %** (`kBlowerFirstFanPct` = CT-485 fan **Low**, wire `0x32`), lead **120 s** (`kBlowerFirstLeadS`, range 60–180), run cap **600 s** (`kBlowerFirstMaxRunS`) | [`13-dual-fuel-control-research.md`](13-dual-fuel-control-research.md) §3: when the fused slope projects the deadband crossing within the lead (crossingBias with horizon = lead), PreCirculator requests the blower LOW so the space destratifies and SensorFusion reads whole-space truth **before** the stage commits; a normal FAN-channel request through SafetySupervisor + DemandArbiter (no demand authority); a real call always overrides, prediction loss cancels, the cap parks a hovering prediction; pre-run seconds **count toward** the circulate duty window (§3 / #53 ledger — never runtime on top); explicit HEAT/COOL modes only (no pre-runs around AUTO changeover); **cool side OFF by default per §8** — a pre-run before a cool call re-evaporates the previous cycle's coil condensate (latent penalty); **no post-heat blower tail** — verified 2026-07-11: the furnace control owns fan-off dissipation internally (R02P032 System menu AC/HP ON/OFF fan delays 5–120 s / 5–240 s; heat-side blower profile is the IFC's, [`02-protocol-climatetalk.md`](02-protocol-climatetalk.md) §5a), and cooling fan-off delay stays **0 s** per §8 |
| Economic switchover (#143) | **disabled** (`kDualFuelEconomicEnabledDefault`); elec **$0.15/kWh** / gas **$0.45/m³** placeholders (`kElecPricePerKwhDefault`/`kGasPricePerM3Default`), AFUE **0.95** (`kAfueDefault`) | [`13-dual-fuel-control-research.md`](13-dual-fuel-control-research.md) §1: the balance point becomes COMPUTED — break-even `COP* = elec$/kWh × 10.55 × AFUE ÷ gas$/m³` (`kGasKwhPerM3` = 10.55 kWh/m³ HHV; identical to the per-therm form via 1 therm = 29.307 kWh = `kGasM3PerTherm` ≈ 2.778 m³), switchover = the OAT where the COP(OAT) curve crosses COP*, **clamped to [capacity balance point, aux lockout]** — economics only ever moves switchover WITHIN the thermally safe band, and both hard lockouts stay untouched downstream; same hysteresis latch as the fixed balance point. Seed COP curve (`kCopCurveSeed`): (−30, 1.4) (−20, 1.8) (−10, 2.3) (0, 2.9) (+8.3, 3.6) — nameplate-shaped ccASHP **placeholders** pending the installed unit's submittal + the CopLearner field record. Prices arrive via retained `slytherm/cmd/energy_prices` JSON `{"elecKwh":0.15,"gasM3":0.45}` (ALL-IN marginal rates; strict parse, NVS-persisted); prices/AFUE/curve are hard-rule validated even while disabled. **Winter validation task** — enable after field data |
| COP proxy learning (#143) | **record-only** — always on, never steers | [`13-dual-fuel-control-research.md`](13-dual-fuel-control-research.md) §5: per **3 °C** OAT bucket (`kCopBucketWidthC`, −33…+18 °C, 17 buckets) CopLearner accumulates HP-heat runtime vs indoor−outdoor degree-time; the degree-days-per-runtime-hour ratio is the hardware-free capacity proxy that corrects the seed COP curve **after a season** (deliberate seam in `CopLearner.h` — telemetry first, optimization later). Gates: heating regime only (indoor > OAT), both temps valid, per-tick gap cap **60 s** (`kCopTickMaxGapS`); NVS blob saved ≤ every **900 s** (`kCopSaveMinS`); telemetry = retained `slytherm/state/cop_proxy` JSON (≤ every 300 s, `kCopPublishMinS`) + `[copx]` telnet line (≤ every 600 s, `kCopLogMinS`) |

## PlatformIO setup

`platformio.ini` skeleton is in the repo root (sniff-rig / custom-firmware path). Secrets (Wi-Fi/MQTT) come from `.env` → generated `config.h` at build time; **never commit secrets** (`.env` is gitignored). If the ESPHome-external-component route wins the framework decision, the component is still developed and unit-tested as plain C++ under PlatformIO's `native` env; ESPHome then supplies provisioning (captive portal / Improv BLE), OTA + safe-mode/rollback, and the LVGL glue.

## Testing strategy

- **Offline unit tests** for `Ct485Frame` (Fletcher) and `Ct485Parser` against captured logs — runnable on the `native` PlatformIO env, no hardware.
- **Bench tests** for every failsafe (unplug sensor, kill MQTT, hang the task, brownout) before the controller ever drives a live furnace bus.
- **Bench tests for every guard timer:** compressor min-off/min-on/starts-per-hour, including **min-off enforced across a reboot** (NVS/RTC persistence + boot hold-off when unknown).
- **Mutual-exclusion invariant:** heat and cool demands simultaneously nonzero must be unreachable — fault-inject at the `DemandArbiter` boundary.
- **Relay boot state (Case B):** scope-verify all relay coils de-energized through boot/reset/brownout, and the watchdog coil-feed cut.
- **Sensor flap during cooling:** a flapping sensor must not short-cycle the compressor via the failsafe path itself (recovery honours min-off).
- **Phase 3 bench gate:** TX-turnaround jitter under LVGL + Wi-Fi + MQTT load, recorded against the measured token slot budget.
- **Scope/logic-analyzer** verification that the bus stays silent during ESP32 boot/reset.
- The full **commissioning matrix** in [`04-safety.md`](04-safety.md) §6 ({equipment} × {failure}) is the final acceptance test.
