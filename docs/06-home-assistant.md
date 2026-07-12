# 06 — Home Assistant Integration

Local-only, over MQTT, using **MQTT Discovery** so HA auto-creates the entities. No cloud.

> **Migration — brand rename `dettson/` → `slytherm/` (issue #79, breaking).**
> The product topic namespace and HA entity/device ids moved from `dettson/*`
> and `dettson_*` to `slytherm/*` and `slytherm_*` (SlyTherm is the product;
> Dettson is a device profile). This is a one-time breaking change:
> 1. Flash the new firmware (it now publishes/subscribes only under `slytherm/`).
> 2. Clear the **old retained** topics at the broker, e.g.
>    `mosquitto_sub -t 'dettson/#' -v --retained-only` to see them, then publish
>    a retained empty payload to each to delete it. Do **not** re-publish under
>    the old prefix.
> 3. HA will **not** rename existing entities — delete the old MQTT device in
>    HA (Settings → Devices) and let Discovery re-create it as `slytherm_*`
>    (e.g. `climate.slytherm_hvac`). Update any dashboards/automations that
>    referenced the old entity_ids.
> Live retained topics are **not** auto-migrated by the firmware.

**Division of labour:** the on-device touchscreen (wall-mounted ESP32-S3, see hardware docs) is the **primary local UI**; HA is **supervisory** — dashboards, schedules, remote sensors, history, notifications, and the phone UX. Every control HA offers must also work (or degrade safely) when HA is down.

## Entities

| Entity | Type | Purpose |
| --- | --- | --- |
| `climate.slytherm_hvac` | `climate` (HVAC) | modes (off/heat/cool/heat_cool), single + dual setpoints, current temp, action |
| `switch.slytherm_em_heat` | `switch` | EM HEAT (gas-only emergency heat) — `ON` engages `EMERGENCY_HEAT`, `OFF` restores the prior mode; see "Emergency heat" below (gap G15) |
| `sensor.slytherm_active_equipment` | `sensor` | `idle` / `hp_heat` / `gas_heat` / `cool` / `defrost` — required because HA's `hvac_action` cannot distinguish HP heat from gas heat |
| `sensor.slytherm_modulation` | `sensor` (%) | live gas modulation % — 0 (off) or **40–100** (the Chinook's documented low fire is 40%; there is no 1–39% band) |
| `sensor.slytherm_outdoor_temp` | `sensor` (°C) | fused outdoor temperature used by the dual-fuel arbiter |
| `sensor.slytherm_outdoor_source` | `sensor` | which rung supplied OAT: `bus` / `wired` / `ha` / `none` |
| `sensor.slytherm_fusion` | `sensor` (JSON attrs) | effective room temp, source rung, participating sensors, occupancy weighting |
| `sensor.slytherm_compressor_min_off_remaining` | `sensor` (s) | compressor minimum-off countdown |
| `binary_sensor.slytherm_compressor_locked_out` | `binary_sensor` | compressor lockout active (timers, low-OAT, fault) |
| `sensor.slytherm_changeover_reason` | `sensor` | enum: why the last heat↔cool / gas↔HP changeover happened (balance point, lockout, escalation, manual…) |
| `sensor.slytherm_blower` | `sensor` (RPM/%) | blower/inducer state |
| `sensor.slytherm_fault` | `sensor` | decoded fault code/text (from `Get Diagnostics 0x86`) |
| `sensor.slytherm_bus_status` | `sensor` (diagnostic, JSON attrs) | CT-485 TX-stack state (Path A): join state + node address, TX-silent flag, last ACK/NAK code, and the latched bus alarms — pairing rejection (`NAK2 0x1B`), comms loss (response timeouts exhausted), demand-refresh starvation; each alarm ends in no-demand ([`04-safety.md`](04-safety.md) §1) |
| `binary_sensor.slytherm_health` | `binary_sensor` (problem) | controller health — `ON` while any SafetySupervisor alarm is active (sensor invalid, control-loop stall, demand-state insanity, bus deadman, MQTT/setpoint staleness, boot-validation overdue, reset-loop latch) |
| `sensor.slytherm_last_error` | `sensor` (diagnostic) | last error string (most recent SafetySupervisor alarm text, kept until the next alarm) |
| `sensor.slytherm_hold` | `sensor` (diagnostic) | active hold type (`none` / `until_next_preset` / `two_hours` / `four_hours` / `indefinite`); remaining seconds as attribute |
| `sensor.slytherm_lock` | `sensor` (diagnostic) | wall-screen lock state (`unlocked` / `user_locked` / `installer_locked`); lock level and `pin_set` as attributes — see "Screen lock" below (issue #45) |
| `sensor.slytherm_sensor_<id>_age` / `binary_sensor.slytherm_sensor_<id>_participating` | diagnostics | per-remote-sensor staleness and fusion participation (`entity_category: diagnostic`) |
| `number.slytherm_sensor_<id>_offset` | `number` (`entity_category: config`) | per-sensor calibration offset, ±5 °C in 0.1 steps (gap G6) — includes the local DS18B20 fallback (id `local`) |

At ~16 entities, prefer **device-based discovery** (one discovery payload for the whole device) over per-entity discovery topics.

## Climate entity (MQTT Discovery sketch)

Published once on boot (device-based discovery payload shown abbreviated to the climate component):

```jsonc
{
  "name": "SlyTherm HVAC",
  "unique_id": "slytherm_hvac",
  "modes": ["off", "heat", "cool", "heat_cool"],
  "fan_modes": ["auto", "on", "circulate"],     // circulate = duty-cycled FAN_DEMAND 0x66 (Path A)
  "preset_modes": ["home", "away", "sleep"],    // rebuilt from the retained slytherm/config/presets roster (defaults shown)
  "min_temp": 10, "max_temp": 30, "temp_step": 0.5,
  "temperature_unit": "C",
  "current_temperature_topic": "slytherm/state/current_temp",
  // single setpoint — used in heat or cool mode
  "temperature_command_topic":  "slytherm/cmd/setpoint",
  "temperature_state_topic":    "slytherm/state/setpoint",
  // dual setpoints — used in heat_cool mode
  "temperature_low_command_topic":  "slytherm/cmd/target_temp_low",
  "temperature_low_state_topic":    "slytherm/state/target_temp_low",
  "temperature_high_command_topic": "slytherm/cmd/target_temp_high",
  "temperature_high_state_topic":   "slytherm/state/target_temp_high",
  "mode_command_topic":         "slytherm/cmd/mode",
  "mode_state_topic":           "slytherm/state/mode",
  "fan_mode_command_topic":     "slytherm/cmd/fan_mode",
  "fan_mode_state_topic":       "slytherm/state/fan_mode",
  "preset_mode_command_topic":  "slytherm/cmd/preset",
  "preset_mode_state_topic":    "slytherm/state/preset",
  "action_topic":               "slytherm/state/action",
  "availability_topic":         "slytherm/availability",   // online/offline (LWT)
  "device": { "identifiers": ["slytherm_esp32"], "name": "SlyTherm ClimateTalk Thermostat",
              "manufacturer": "ElectricRV", "model": "ESP32-S3 CT-485" }
}
```

Notes:

- `action` values are restricted to HA's real `HVACAction` enum: `off`, `idle`, `heating`, `cooling`, `drying`, `fan`, **`defrosting`**, `preheating`. We use `defrosting` for native defrost display; `active_equipment` (above) carries the HP-vs-gas distinction `action` cannot.
- The climate.mqtt documentation page claims `modes` must be a subset of the defaults; this is **contradicted by the source** — the schema validates `modes` with `cv.ensure_list` (see `homeassistant/components/mqtt/climate.py` in HA core). `heat_cool` works as listed.
- The firmware enforces the dual-setpoint deadband (cool ≥ heat + min delta, default 2.8 °C, hard floor 1.1 °C): violating writes are clamped and the other setpoint is pushed, Ecobee-style — HA sees the corrected values echoed on the state topics.
- `EMERGENCY_HEAT` is deliberately **not** in `modes` (HA's climate schema accepts only the standard hvac modes — `emergency_heat` would be rejected) and **not** in `preset_modes`; it is the dedicated `switch.slytherm_em_heat` (see "Emergency heat" below). While engaged, `slytherm/state/mode` keeps reporting `heat`; the switch and `active_equipment` carry the truth.

## Topic map

| Direction | Topic | Payload |
| --- | --- | --- |
| HA → ESP32 | `slytherm/cmd/setpoint` | float °C (heat or cool mode) |
| HA → ESP32 | `slytherm/cmd/target_temp_low` | float °C (heat side, heat_cool mode) |
| HA → ESP32 | `slytherm/cmd/target_temp_high` | float °C (cool side, heat_cool mode) |
| HA → ESP32 | `slytherm/cmd/mode` | `off` / `heat` / `cool` / `heat_cool` |
| HA → ESP32 | `slytherm/cmd/fan_mode` | `auto` / `on` / `circulate` |
| HA → ESP32 | `slytherm/cmd/fan_circulate_min` | int minutes-per-hour the blower runs in `circulate` mode, clamped 0–60 (default 15). Set by the on-panel Fan sheet or the `number.slytherm_fan_circulate_min` entity (#128) |
| HA → ESP32 | `slytherm/cmd/fan_circulate_pct` | int circulate blower speed %, snapped to Low 25 / Med 50 / High 75 (default 25). Set by the on-panel Fan sheet or the `number.slytherm_fan_circulate_pct` entity (#128) |
| HA → ESP32 | `slytherm/cmd/preset` | preset name from the roster (default `home` / `away` / `sleep`) |
| HA → ESP32 | `slytherm/cmd/hold` | `until_next_preset` / `two_hours` / `four_hours` / `indefinite` / `clear` |
| HA → ESP32 | `slytherm/cmd/em_heat` | `ON` / `OFF` — engage/disengage EMERGENCY_HEAT (gap G15) |
| HA → ESP32 | `slytherm/cmd/lock_clear` | exactly `clear_user_pin` — forgotten-PIN recovery (see "Screen lock"); any other payload, including empty, is ignored |
| HA → ESP32 | `slytherm/cmd/next_target` | JSON `{"temp": 21.0, "mode": "heat", "in_s": 5400}` — the next scheduled setpoint change, for smart recovery (see "Smart recovery"); tolerant parse, all three keys required (`mode` = `heat`/`cool`, `in_s` ≤ 7 days), invalid payloads ignored |
| HA → ESP32 | `slytherm/cmd/energy_prices` | **retained** JSON `{"elecKwh": 0.15, "gasM3": 0.45}` — ALL-IN marginal electricity ($/kWh) and gas ($/m³) prices for the #143 economic switchover (publish from HA's energy config or a TOU automation; a $/therm gas price converts ÷2.778). Strict parse: both keys required, each in (0, 10]; invalid payloads ignored — a rejected payload never moves the switchover point. NVS-persisted on the device too, so prices survive broker-less reboots |
| HA → ESP32 | `slytherm/cmd/outdoor_temp` | float °C — HA-weather bridge feeding the outdoor-temperature ladder's third rung (`ha` source); plausibility-gated −50…55 °C, republish at least every 30 min (the rung staleness window) or the ladder demotes to `none` → fail-cold |
| HA → ESP32 | `slytherm/sensors/<id>/state` | remote-sensor JSON (see below) |
| HA → ESP32 | `slytherm/config/sensors` | retained sensor roster/config (see below) |
| HA → ESP32 | `slytherm/cmd/sensor/<id>/offset` | per-sensor calibration offset, float °C within ±5 (incl. the local DS18B20, id `local`) |
| HA → ESP32 | `slytherm/config/presets` | retained preset roster JSON (see "Schedules and presets") |
| ESP32 → HA | `slytherm/state/current_temp` | float °C (the fused control input) |
| ESP32 → HA | `slytherm/state/setpoint` / `target_temp_low` / `target_temp_high` | echo (post-clamp) |
| ESP32 → HA | `slytherm/state/mode` | `off` / `heat` / `cool` / `heat_cool` |
| ESP32 → HA | `slytherm/state/fan_mode` / `preset` | echo (`fan_mode` is **retained** so a reconnecting Remote/HA reads the truth — #128) |
| ESP32 → HA | `slytherm/state/fan_circulate_min` / `fan_circulate_pct` | **retained** int echo of the runtime circulate minutes-per-hour and speed % (#128) |
| ESP32 → HA | `slytherm/state/hold` | JSON `{"type": hold type or "none", "remaining": s}` |
| ESP32 → HA | `slytherm/state/em_heat` | `ON` / `OFF` (reflects EMERGENCY_HEAT engagement from any source, incl. the wall UI) |
| ESP32 → HA | `slytherm/state/action` | `off` / `idle` / `heating` / `cooling` / `fan` / `defrosting` |
| ESP32 → HA | `slytherm/state/active_equipment` | `idle` / `hp_heat` / `gas_heat` / `cool` / `defrost` |
| ESP32 → HA | `slytherm/state/modulation` | 0 or 40–100 % (gas) |
| ESP32 → HA | `slytherm/state/outdoor_temp` | float °C |
| ESP32 → HA | `slytherm/state/outdoor_source` | `bus` / `wired` / `ha` / `none` |
| ESP32 → HA | `slytherm/state/fusion` | JSON `{"temp": C, "tier": "fused_remotes"/"single_remote"/"local_degraded"/"none", "participants": [ids], "occupied": bool}` — state of `sensor.slytherm_fusion` is `temp`, the full payload is its attributes |
| ESP32 → HA | `slytherm/state/compressor_min_off_remaining` | seconds |
| ESP32 → HA | `slytherm/state/compressor_locked_out` | `ON` / `OFF` |
| ESP32 → HA | `slytherm/state/relays` | Case B diagnostic JSON: `{"y1":bool,"y2":bool,"ob":bool,"g":bool,"defrost":bool}` — commanded relay states + D-wire defrost sense; `ob` true = energized = **heating** (Gree B convention, `kObEnergizedIsHeat`) |
| ESP32 → HA | `slytherm/state/sensor/<id>/offset` | echo (post-clamp) |
| ESP32 → HA | `slytherm/state/changeover_reason` | enum string |
| ESP32 → HA | `slytherm/state/lock` | JSON `{"state":"user_locked","level":"settings","pin_set":true}` |
| ESP32 → HA | `slytherm/state/bus` | CT-485 TX-stack JSON: `{"join":"addressed","addr":1,"silent":false,"last_ack":"0x06","alarms":{"pairing":false,"comms_loss":false,"starvation":false}}` (alarms latched until explicitly cleared) |
| ESP32 → HA | `slytherm/state/cop_proxy` | **retained** record-only #143 telemetry JSON: `{"bucketC":3,"buckets":[{"oat":-12,"runS":8130,"degH":61.2,"ddph":1.13},…]}` — per-3 °C-OAT-bucket HP-heat runtime, indoor−outdoor degree-hours, and the degree-days-per-runtime-hour proxy ([`13-dual-fuel-control-research.md`](13-dual-fuel-control-research.md) §5); feeds nothing back into control this season |
| ESP32 → HA | `slytherm/state/fault` | code/text |
| ESP32 → HA | `slytherm/availability` | `online` / `offline` (MQTT **Last Will** = `offline`) |

All protection and dual-fuel parameters (balance point, lockouts, compressor timers, differentials — see the canonical defaults table in [`05-firmware-plan.md`](05-firmware-plan.md)) are **firmware-resident** and exposed as HA-editable MQTT `number` entities; HA edits them but never owns them.

## Remote sensors (room temp + occupancy)

Ecobee-class "follow me" comes from remote sensors, but **not Ecobee's own sensors** — those use a proprietary, undocumented 915 MHz FHSS protocol with no public decoder, and the ESP32 has no sub-GHz radio. Do not attempt. Recommended hardware: **SONOFF SNZB-02D** (Zigbee temp, ±0.2 °C), **Aqara** temp sensors, **Aqara FP2** or **Apollo MSR-2** (mmWave occupancy; the MSR-2 is ESPHome-native — same toolchain).

The sensors reach the firmware via an HA bridge automation:

- **Per-sensor state:** `slytherm/sensors/<id>/state`, JSON `{"temp": C, "occ": bool|null, "bat": 0-100|null, "hum": %|null}`. **Non-retained.** The HA bridge automation republishes on change **and on a 60 s heartbeat** — the firmware's staleness timeout (default 300 s per sensor) needs a live cadence, and HA itself omits/flags sensors whose `last_updated` exceeds the device's expected interval.
- **Roster/config:** retained JSON at `slytherm/config/sensors` — `{"sensors":[{"id":"kitchen","max_age_s":300,"offset":-0.5}, …]}`: sensor ids (required, unique), optional `max_age_s` (firmware-clamped 180–900 s) and optional calibration `offset` (default 0, clamped ±5 °C). Tolerant parse: invalid entries are skipped, ≤16 sensors. (Per-preset participation profiles: future work.)
- **Calibration offsets (gap G6):** each sensor — and the local DS18B20 fallback, id `local` — gets an HA `number` (`entity_category: config`, ±5 °C, 0.1 steps) at `slytherm/cmd/sensor/<id>/offset`. Offsets are applied **before** the health gates (range/stuck/outlier judge corrected values), and an offset edit ramps the fused output through the slew limit — never a step.

**Sensor fusion happens in firmware, not in HA templates.** This is deliberate, not a style choice:

1. The control loop's input must not depend on HA uptime — follow-me must survive an HA outage.
2. Per-sensor health (staleness, range, stuck-value, divergence) must be visible to the firmware's safety layer, which acts on it (quorum loss → demand 0).
3. A template aggregate would double-smooth (HA averaging + firmware EMA) and hide which sensor failed.

The local DS18B20 remains the independent sanity floor and last-resort input (see [`04-safety.md`](04-safety.md) §4).

**Outdoor temperature bridge:** the third OAT rung (`ha`) is fed the same way — an HA automation publishes the weather integration's outdoor temperature to `slytherm/cmd/outdoor_temp` (float °C, non-retained, republish ≤ every 30 min). The firmware plausibility-gates it (−50…55 °C) and the rung ladder/fail-cold policy in [`04-safety.md`](04-safety.md) §4 applies unchanged: HA going quiet only ever demotes the rung, never raises demand.

## Schedules and presets

- **HA owns schedules.** The HA scheduler (or any automation) drives the thermostat by writing `slytherm/cmd/preset` and/or setpoints. The firmware does not store a weekly schedule.
- **Preset roster is config-driven** (gap G4): retained JSON at `slytherm/config/presets` — `{"presets":[{"name":"home","heat":21.0,"cool":25.0}, …]}`. Up to **8** entries, names ≤23 chars, setpoints within the climate limits (10–30 °C); invalid entries are skipped. The climate discovery `preset_modes` list is rebuilt from the roster; default roster `home`/`away`/`sleep`. A preset pair violating the deadband resolves with the cool value winning (heat pushed down).
- **Holds (Ecobee semantics):** a manual setpoint/mode change creates a hold of the configured default type (`until_next_preset` default; `two_hours` = 7200 s, `four_hours` = 14400 s, `indefinite`); `slytherm/cmd/hold` sets one explicitly. While a hold is active incoming presets are ignored — except an `until_next_preset` hold ends when the next valid preset arrives; timed holds expire by clock; `indefinite` ends only on `clear`. Timed-hold expiry does **not** revert setpoints — HA's next scheduled preset write restores the schedule. State: `slytherm/state/hold`.
- A ready-made starter package (weekly schedule, vacation calendar, runtime-based filter reminder, temp/RH alerts, presence-Away) ships in [`../ha/`](../ha/README.md) — closes gap G3 ([`07-ecobee-gap-analysis.md`](07-ecobee-gap-analysis.md)).
- Accessory-coordination blueprints (standalone humidifier/dehumidifier with Ecobee-style frost-control ceiling from `sensor.slytherm_outdoor_temp` and blower interlock from `sensor.slytherm_active_equipment`; HRV minutes-per-hour duty with outdoor lockout) also ship in [`../ha/`](../ha/README.md) — gap G1 step 1: automation-level coordination of user-supplied smart accessories, not certified equipment control.
- **Firmware owns the outage fallback.** On stale MQTT (no command/heartbeat traffic for >30 min): fall back to heat **18 °C** / cool **27 °C**, mode = **last user mode** — never escalate to OFF, and never to a single bare setpoint (a lone "18 °C" is a continuous cool call in summer). Stored in NVS.

## Smart recovery (pre-heat / pre-cool)

Ecobee-style smart recovery (issue #50): start the equipment early so the room *arrives at* the scheduled setpoint at the scheduled time, instead of starting toward it then.

- **HA publishes the next scheduled target** to `slytherm/cmd/next_target` as JSON `{"temp": 21.0, "mode": "heat", "in_s": 5400}` — the upcoming setpoint, which side it serves (`heat`/`cool`), and the seconds until it takes effect. Republish whenever the schedule or the remaining time changes (piggyback on the sensor-bridge heartbeat). The parse is tolerant; an invalid payload is ignored, and publishing nothing simply means no pre-start — HA stays optional, as always.
- **The firmware learns ramp rates** (°C/h) per {heat/cool × HP/gas} channel from real run segments, seeded at heat **1.0 °C/h** / cool **0.8 °C/h** until learned (robust EMA with outlier rejection — gates in the [`05-firmware-plan.md`](05-firmware-plan.md) canonical defaults table). From the learned rate, the current fused temp, and the target it recommends an early-start lead, hard-capped at **2 h** (`kRecoveryMaxLookaheadS`).
- **Advisory only:** `RecoveryEstimator` recommends; `ModeStateMachine`/glue decides; `CompressorGuard` and `DualFuelArbiter` still gate every demand. A bogus `next_target` can at worst start a normal, fully-protected call early — it can never bypass a lockout, timer, or the deadband.
- **Disabled by default** (`kRecoveryEnabledDefault = false`) **until field-tuned**: leave it off until a few weeks of learned ramp rates look sane for the installed equipment, then enable via the HA-editable setting. Learning runs even while disabled, so enabling starts from real data.

## Emergency heat (EM HEAT)

EM HEAT (gas-only heat; the compressor is never requested) is exposed to HA as the dedicated `switch.slytherm_em_heat` (gap G15) — **not** as an hvac mode and **not** as a preset:

- **Not a mode:** HA's MQTT climate schema accepts only `off`/`heat`/`cool`/`heat_cool`/`auto`/`dry`/`fan_only`; an `emergency_heat` mode would be rejected.
- **Not a preset:** the HA scheduler republishes comfort presets on schedule — a preset-based EM HEAT would be silently disengaged by the next scheduled write, exactly when the owner is away with a failed heat pump. The switch is orthogonal: comfort presets keep managing setpoints while EM HEAT pins the equipment choice, and `emergency_heat` never appears in `preset_modes`.
- **Both directions:** `ON` → `ModeStateMachine` `EMERGENCY_HEAT` (mode state topic keeps reporting `heat`; `active_equipment` shows `gas_heat`); `OFF` → restores the mode that was active at engagement, including a wall-UI engagement. `slytherm/state/em_heat` reflects engagement from any source.
- **Safety:** exposing EM HEAT to the app is acceptable because `EMERGENCY_HEAT` is gas-only — the call is emitted `gasOnly` and the compressor path is de-energized (consistent with [`04-safety.md`](04-safety.md): gas/compressor mutual exclusion §2, fail-toward-no-demand §1). Engaging it can only *remove* compressor demand, never add it; disengaging re-enters the prior mode through the normal pipeline, where `CompressorGuard` owns any compressor restart.

## Screen lock & forgotten-PIN recovery (issue #45)

The wall touchscreen supports a 4-digit PIN lock (state machine in `lib/UiModel`): lock level **settings-only** (default — setpoints stay adjustable) or **settings+setpoints**; auto-relock after 120 s of inactivity; 5 wrong attempts → 60 s entry backoff (defaults table in [`05-firmware-plan.md`](05-firmware-plan.md)). Installer settings pages are gated by a **separate installer code**, and an **installer lockout** locks the screen so that only the installer code releases it. PINs are stored as salted hashes — tamper resistance against casual users, not cryptography.

- **Safety rule ([`04-safety.md`](04-safety.md) §1c/§3):** the lock blocks **change intents only**. Alarms, current temperature, and equipment status are never hidden at any lock level — alarm *visibility* is exempt from every lock; alarm *acknowledgement* stays locked.
- **HA visibility:** `slytherm/state/lock` (→ `sensor.slytherm_lock`, diagnostic) carries `{"state","level","pin_set"}` so a locked-out (or installer-locked) thermostat is diagnosable remotely.
- **Forgotten-PIN recovery:** publish exactly `clear_user_pin` to `slytherm/cmd/lock_clear`. The user PIN is cleared and a user lock released — **no PIN is required**. Rationale: anyone who can publish to the broker already has full climate control (every setpoint, mode, and EM HEAT) — **HA/broker access = admin by definition**; a PIN gate here would add no security, only a second thing to forget. HA stays a comfort/visibility layer either way — the lock is tamper resistance, not a safety mechanism.
- **Retained-safe by construction:** the payload is an exact magic string (never `ON`/`1`, so no generic retained switch payload can trigger it); an empty payload is ignored; after handling, the firmware publishes a **retained empty message** to the topic, which deletes any retained copy from the broker — a reboot/resubscribe can replay only the empty tombstone, a no-op.
- **The installer code is NOT clearable over MQTT** — recovery from a lost installer code is physical (USB reflash / NVS wipe at the wall, per the [`04-safety.md`](04-safety.md) §3a recovery path). An NVS wipe fails *open* (lock disabled, no PINs) — the lock must never brick the wall UI.

## Mobile app

**Primary phone UX: the Home Assistant Companion app (iOS/Android)** against `climate.slytherm_hvac`. Zero custom development — full thermostat card, schedules, history, and notifications come free, fully local on the LAN.

- **Remote access (no-cloud):** self-hosted **WireGuard or Tailscale**; the Companion app works over both using the internal URL.
- **Nabu Casa** (~US$6.50/mo) is a zero-config cloud relay — optional and noted as **not aligned with this project's no-cloud stance** (it does fund HA development).
- **A custom mobile app is explicitly rejected** for scope: months of work to replicate what Companion provides for free.
- **Diagnostic fallback:** the device serves an ESPHome `web_server` page directly — a local control/diagnostic page that works when HA or the MQTT broker is down (defence in depth for the broker-loss failure row). It is a fallback, not a primary UI; the wall touchscreen is the primary local UI.

## Safety interactions with HA

- **HA/MQTT loss must never raise any demand** — not heat, not cool, not fan, not compressor. MQTT **Last Will** sets `offline`; the firmware falls back to the dual-bounded local profile above and keeps running on local sensors — it never interprets "broker gone" as a demand of any kind.
- Recovery from MQTT/sensor faults honors the compressor minimum-off timer — the failsafe path itself must not short-cycle the compressor.
- Health/diagnostics published so a silent failure is visible in HA (and on the touchscreen and local status LED).
- HA is for **comfort and visibility**, not a safety layer — all failsafes live on the ESP32 + furnace IFC + equipment protections.
