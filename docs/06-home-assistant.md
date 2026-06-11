# 06 — Home Assistant Integration

Local-only, over MQTT, using **MQTT Discovery** so HA auto-creates the entities. No cloud.

**Division of labour:** the on-device touchscreen (wall-mounted ESP32-S3, see hardware docs) is the **primary local UI**; HA is **supervisory** — dashboards, schedules, remote sensors, history, notifications, and the phone UX. Every control HA offers must also work (or degrade safely) when HA is down.

## Entities

| Entity | Type | Purpose |
| --- | --- | --- |
| `climate.dettson_hvac` | `climate` (HVAC) | modes (off/heat/cool/heat_cool), single + dual setpoints, current temp, action |
| `switch.dettson_em_heat` | `switch` | EM HEAT (gas-only emergency heat) — `ON` engages `EMERGENCY_HEAT`, `OFF` restores the prior mode; see "Emergency heat" below (gap G15) |
| `sensor.dettson_active_equipment` | `sensor` | `idle` / `hp_heat` / `gas_heat` / `cool` / `defrost` — required because HA's `hvac_action` cannot distinguish HP heat from gas heat |
| `sensor.dettson_modulation` | `sensor` (%) | live gas modulation % — 0 (off) or **40–100** (the Chinook's documented low fire is 40%; there is no 1–39% band) |
| `sensor.dettson_outdoor_temp` | `sensor` (°C) | fused outdoor temperature used by the dual-fuel arbiter |
| `sensor.dettson_outdoor_source` | `sensor` | which rung supplied OAT: `bus` / `wired` / `ha` / `none` |
| `sensor.dettson_fusion` | `sensor` (JSON attrs) | effective room temp, source rung, participating sensors, occupancy weighting |
| `sensor.dettson_compressor_min_off_remaining` | `sensor` (s) | compressor minimum-off countdown |
| `binary_sensor.dettson_compressor_locked_out` | `binary_sensor` | compressor lockout active (timers, low-OAT, fault) |
| `sensor.dettson_changeover_reason` | `sensor` | enum: why the last heat↔cool / gas↔HP changeover happened (balance point, lockout, escalation, manual…) |
| `sensor.dettson_blower` | `sensor` (RPM/%) | blower/inducer state |
| `sensor.dettson_fault` | `sensor` | decoded fault code/text (from `Get Diagnostics 0x86`) |
| `binary_sensor.dettson_health` | `binary_sensor` (problem) | controller health (Wi-Fi/MQTT/sensor/watchdog) |
| `sensor.dettson_last_error` | `sensor` (diagnostic) | last error string |
| `sensor.dettson_hold` | `sensor` (diagnostic) | active hold type (`none` / `until_next_preset` / `two_hours` / `four_hours` / `indefinite`); remaining seconds as attribute |
| `sensor.dettson_sensor_<id>_age` / `binary_sensor.dettson_sensor_<id>_participating` | diagnostics | per-remote-sensor staleness and fusion participation (`entity_category: diagnostic`) |
| `number.dettson_sensor_<id>_offset` | `number` (`entity_category: config`) | per-sensor calibration offset, ±5 °C in 0.1 steps (gap G6) — includes the local DS18B20 fallback (id `local`) |

At ~16 entities, prefer **device-based discovery** (one discovery payload for the whole device) over per-entity discovery topics.

## Climate entity (MQTT Discovery sketch)

Published once on boot (device-based discovery payload shown abbreviated to the climate component):

```jsonc
{
  "name": "Dettson HVAC",
  "unique_id": "dettson_hvac",
  "modes": ["off", "heat", "cool", "heat_cool"],
  "fan_modes": ["auto", "on", "circulate"],     // circulate = duty-cycled FAN_DEMAND 0x66 (Path A)
  "preset_modes": ["home", "away", "sleep"],    // rebuilt from the retained dettson/config/presets roster (defaults shown)
  "min_temp": 10, "max_temp": 30, "temp_step": 0.5,
  "temperature_unit": "C",
  "current_temperature_topic": "dettson/state/current_temp",
  // single setpoint — used in heat or cool mode
  "temperature_command_topic":  "dettson/cmd/setpoint",
  "temperature_state_topic":    "dettson/state/setpoint",
  // dual setpoints — used in heat_cool mode
  "temperature_low_command_topic":  "dettson/cmd/target_temp_low",
  "temperature_low_state_topic":    "dettson/state/target_temp_low",
  "temperature_high_command_topic": "dettson/cmd/target_temp_high",
  "temperature_high_state_topic":   "dettson/state/target_temp_high",
  "mode_command_topic":         "dettson/cmd/mode",
  "mode_state_topic":           "dettson/state/mode",
  "fan_mode_command_topic":     "dettson/cmd/fan_mode",
  "fan_mode_state_topic":       "dettson/state/fan_mode",
  "preset_mode_command_topic":  "dettson/cmd/preset",
  "preset_mode_state_topic":    "dettson/state/preset",
  "action_topic":               "dettson/state/action",
  "availability_topic":         "dettson/availability",   // online/offline (LWT)
  "device": { "identifiers": ["dettson_esp32"], "name": "Dettson ClimateTalk Thermostat",
              "manufacturer": "ElectricRV", "model": "ESP32-S3 CT-485" }
}
```

Notes:

- `action` values are restricted to HA's real `HVACAction` enum: `off`, `idle`, `heating`, `cooling`, `drying`, `fan`, **`defrosting`**, `preheating`. We use `defrosting` for native defrost display; `active_equipment` (above) carries the HP-vs-gas distinction `action` cannot.
- The climate.mqtt documentation page claims `modes` must be a subset of the defaults; this is **contradicted by the source** — the schema validates `modes` with `cv.ensure_list` (see `homeassistant/components/mqtt/climate.py` in HA core). `heat_cool` works as listed.
- The firmware enforces the dual-setpoint deadband (cool ≥ heat + min delta, default 2.8 °C, hard floor 1.1 °C): violating writes are clamped and the other setpoint is pushed, Ecobee-style — HA sees the corrected values echoed on the state topics.
- `EMERGENCY_HEAT` is deliberately **not** in `modes` (HA's climate schema accepts only the standard hvac modes — `emergency_heat` would be rejected) and **not** in `preset_modes`; it is the dedicated `switch.dettson_em_heat` (see "Emergency heat" below). While engaged, `dettson/state/mode` keeps reporting `heat`; the switch and `active_equipment` carry the truth.

## Topic map

| Direction | Topic | Payload |
| --- | --- | --- |
| HA → ESP32 | `dettson/cmd/setpoint` | float °C (heat or cool mode) |
| HA → ESP32 | `dettson/cmd/target_temp_low` | float °C (heat side, heat_cool mode) |
| HA → ESP32 | `dettson/cmd/target_temp_high` | float °C (cool side, heat_cool mode) |
| HA → ESP32 | `dettson/cmd/mode` | `off` / `heat` / `cool` / `heat_cool` |
| HA → ESP32 | `dettson/cmd/fan_mode` | `auto` / `on` / `circulate` |
| HA → ESP32 | `dettson/cmd/preset` | preset name from the roster (default `home` / `away` / `sleep`) |
| HA → ESP32 | `dettson/cmd/hold` | `until_next_preset` / `two_hours` / `four_hours` / `indefinite` / `clear` |
| HA → ESP32 | `dettson/cmd/em_heat` | `ON` / `OFF` — engage/disengage EMERGENCY_HEAT (gap G15) |
| HA → ESP32 | `dettson/sensors/<id>/state` | remote-sensor JSON (see below) |
| HA → ESP32 | `dettson/config/sensors` | retained sensor roster/config (see below) |
| HA → ESP32 | `dettson/cmd/sensor/<id>/offset` | per-sensor calibration offset, float °C within ±5 (incl. the local DS18B20, id `local`) |
| HA → ESP32 | `dettson/config/presets` | retained preset roster JSON (see "Schedules and presets") |
| ESP32 → HA | `dettson/state/current_temp` | float °C (the fused control input) |
| ESP32 → HA | `dettson/state/setpoint` / `target_temp_low` / `target_temp_high` | echo (post-clamp) |
| ESP32 → HA | `dettson/state/mode` | `off` / `heat` / `cool` / `heat_cool` |
| ESP32 → HA | `dettson/state/fan_mode` / `preset` | echo |
| ESP32 → HA | `dettson/state/hold` | JSON `{"type": hold type or "none", "remaining": s}` |
| ESP32 → HA | `dettson/state/em_heat` | `ON` / `OFF` (reflects EMERGENCY_HEAT engagement from any source, incl. the wall UI) |
| ESP32 → HA | `dettson/state/action` | `off` / `idle` / `heating` / `cooling` / `fan` / `defrosting` |
| ESP32 → HA | `dettson/state/active_equipment` | `idle` / `hp_heat` / `gas_heat` / `cool` / `defrost` |
| ESP32 → HA | `dettson/state/modulation` | 0 or 40–100 % (gas) |
| ESP32 → HA | `dettson/state/outdoor_temp` | float °C |
| ESP32 → HA | `dettson/state/outdoor_source` | `bus` / `wired` / `ha` / `none` |
| ESP32 → HA | `dettson/state/fusion` | JSON: effective temp, source rung, participants, occupied |
| ESP32 → HA | `dettson/state/compressor_min_off_remaining` | seconds |
| ESP32 → HA | `dettson/state/compressor_locked_out` | `ON` / `OFF` |
| ESP32 → HA | `dettson/state/sensor/<id>/offset` | echo (post-clamp) |
| ESP32 → HA | `dettson/state/changeover_reason` | enum string |
| ESP32 → HA | `dettson/state/fault` | code/text |
| ESP32 → HA | `dettson/availability` | `online` / `offline` (MQTT **Last Will** = `offline`) |

All protection and dual-fuel parameters (balance point, lockouts, compressor timers, differentials — see the canonical defaults table in [`05-firmware-plan.md`](05-firmware-plan.md)) are **firmware-resident** and exposed as HA-editable MQTT `number` entities; HA edits them but never owns them.

## Remote sensors (room temp + occupancy)

Ecobee-class "follow me" comes from remote sensors, but **not Ecobee's own sensors** — those use a proprietary, undocumented 915 MHz FHSS protocol with no public decoder, and the ESP32 has no sub-GHz radio. Do not attempt. Recommended hardware: **SONOFF SNZB-02D** (Zigbee temp, ±0.2 °C), **Aqara** temp sensors, **Aqara FP2** or **Apollo MSR-2** (mmWave occupancy; the MSR-2 is ESPHome-native — same toolchain).

The sensors reach the firmware via an HA bridge automation:

- **Per-sensor state:** `dettson/sensors/<id>/state`, JSON `{"temp": C, "occ": bool|null, "bat": 0-100|null, "hum": %|null}`. **Non-retained.** The HA bridge automation republishes on change **and on a 60 s heartbeat** — the firmware's staleness timeout (default 300 s per sensor) needs a live cadence, and HA itself omits/flags sensors whose `last_updated` exceeds the device's expected interval.
- **Roster/config:** retained JSON at `dettson/config/sensors` — `{"sensors":[{"id":"kitchen","max_age_s":300,"offset":-0.5}, …]}`: sensor ids (required, unique), optional `max_age_s` (firmware-clamped 180–900 s) and optional calibration `offset` (default 0, clamped ±5 °C). Tolerant parse: invalid entries are skipped, ≤16 sensors. (Per-preset participation profiles: future work.)
- **Calibration offsets (gap G6):** each sensor — and the local DS18B20 fallback, id `local` — gets an HA `number` (`entity_category: config`, ±5 °C, 0.1 steps) at `dettson/cmd/sensor/<id>/offset`. Offsets are applied **before** the health gates (range/stuck/outlier judge corrected values), and an offset edit ramps the fused output through the slew limit — never a step.

**Sensor fusion happens in firmware, not in HA templates.** This is deliberate, not a style choice:

1. The control loop's input must not depend on HA uptime — follow-me must survive an HA outage.
2. Per-sensor health (staleness, range, stuck-value, divergence) must be visible to the firmware's safety layer, which acts on it (quorum loss → demand 0).
3. A template aggregate would double-smooth (HA averaging + firmware EMA) and hide which sensor failed.

The local DS18B20 remains the independent sanity floor and last-resort input (see [`04-safety.md`](04-safety.md) §4).

## Schedules and presets

- **HA owns schedules.** The HA scheduler (or any automation) drives the thermostat by writing `dettson/cmd/preset` and/or setpoints. The firmware does not store a weekly schedule.
- **Preset roster is config-driven** (gap G4): retained JSON at `dettson/config/presets` — `{"presets":[{"name":"home","heat":21.0,"cool":25.0}, …]}`. Up to **8** entries, names ≤23 chars, setpoints within the climate limits (10–30 °C); invalid entries are skipped. The climate discovery `preset_modes` list is rebuilt from the roster; default roster `home`/`away`/`sleep`. A preset pair violating the deadband resolves with the cool value winning (heat pushed down).
- **Holds (Ecobee semantics):** a manual setpoint/mode change creates a hold of the configured default type (`until_next_preset` default; `two_hours` = 7200 s, `four_hours` = 14400 s, `indefinite`); `dettson/cmd/hold` sets one explicitly. While a hold is active incoming presets are ignored — except an `until_next_preset` hold ends when the next valid preset arrives; timed holds expire by clock; `indefinite` ends only on `clear`. Timed-hold expiry does **not** revert setpoints — HA's next scheduled preset write restores the schedule. State: `dettson/state/hold`.
- A ready-made starter package (weekly schedule, vacation calendar, runtime-based filter reminder, temp/RH alerts, presence-Away) ships in [`../ha/`](../ha/README.md) — closes gap G3 ([`07-ecobee-gap-analysis.md`](07-ecobee-gap-analysis.md)).
- **Firmware owns the outage fallback.** On stale MQTT (no command/heartbeat traffic for >30 min): fall back to heat **18 °C** / cool **27 °C**, mode = **last user mode** — never escalate to OFF, and never to a single bare setpoint (a lone "18 °C" is a continuous cool call in summer). Stored in NVS.

## Emergency heat (EM HEAT)

EM HEAT (gas-only heat; the compressor is never requested) is exposed to HA as the dedicated `switch.dettson_em_heat` (gap G15) — **not** as an hvac mode and **not** as a preset:

- **Not a mode:** HA's MQTT climate schema accepts only `off`/`heat`/`cool`/`heat_cool`/`auto`/`dry`/`fan_only`; an `emergency_heat` mode would be rejected.
- **Not a preset:** the HA scheduler republishes comfort presets on schedule — a preset-based EM HEAT would be silently disengaged by the next scheduled write, exactly when the owner is away with a failed heat pump. The switch is orthogonal: comfort presets keep managing setpoints while EM HEAT pins the equipment choice, and `emergency_heat` never appears in `preset_modes`.
- **Both directions:** `ON` → `ModeStateMachine` `EMERGENCY_HEAT` (mode state topic keeps reporting `heat`; `active_equipment` shows `gas_heat`); `OFF` → restores the mode that was active at engagement, including a wall-UI engagement. `dettson/state/em_heat` reflects engagement from any source.
- **Safety:** exposing EM HEAT to the app is acceptable because `EMERGENCY_HEAT` is gas-only — the call is emitted `gasOnly` and the compressor path is de-energized (consistent with [`04-safety.md`](04-safety.md): gas/compressor mutual exclusion §2, fail-toward-no-demand §1). Engaging it can only *remove* compressor demand, never add it; disengaging re-enters the prior mode through the normal pipeline, where `CompressorGuard` owns any compressor restart.

## Mobile app

**Primary phone UX: the Home Assistant Companion app (iOS/Android)** against `climate.dettson_hvac`. Zero custom development — full thermostat card, schedules, history, and notifications come free, fully local on the LAN.

- **Remote access (no-cloud):** self-hosted **WireGuard or Tailscale**; the Companion app works over both using the internal URL.
- **Nabu Casa** (~US$6.50/mo) is a zero-config cloud relay — optional and noted as **not aligned with this project's no-cloud stance** (it does fund HA development).
- **A custom mobile app is explicitly rejected** for scope: months of work to replicate what Companion provides for free.
- **Diagnostic fallback:** the device serves an ESPHome `web_server` page directly — a local control/diagnostic page that works when HA or the MQTT broker is down (defence in depth for the broker-loss failure row). It is a fallback, not a primary UI; the wall touchscreen is the primary local UI.

## Safety interactions with HA

- **HA/MQTT loss must never raise any demand** — not heat, not cool, not fan, not compressor. MQTT **Last Will** sets `offline`; the firmware falls back to the dual-bounded local profile above and keeps running on local sensors — it never interprets "broker gone" as a demand of any kind.
- Recovery from MQTT/sensor faults honors the compressor minimum-off timer — the failsafe path itself must not short-cycle the compressor.
- Health/diagnostics published so a silent failure is visible in HA (and on the touchscreen and local status LED).
- HA is for **comfort and visibility**, not a safety layer — all failsafes live on the ESP32 + furnace IFC + equipment protections.
