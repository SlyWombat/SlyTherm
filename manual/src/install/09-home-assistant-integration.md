# 9. Home Assistant Integration

The DT-1 integrates with Home Assistant (HA) over **MQTT on the local
network only** — no cloud service is used or required. HA is a
**supervisory** layer: dashboards, schedules, remote sensors, history, and
phone access. Every control HA offers also works (or degrades safely) when
HA is down; **loss of HA or the broker never raises any demand**.

## 9.1 Prerequisites

| Requirement | Notes |
| --- | --- |
| MQTT broker | A local broker reachable from the controller and HA (e.g., the Mosquitto broker add-on commonly used with Home Assistant) |
| HA MQTT integration | Enabled, with **MQTT Discovery** active (default discovery prefix `homeassistant`) |
| Wi-Fi | The controller joins the local Wi-Fi; credentials are provisioned without pulling the unit from the wall (captive portal / Bluetooth provisioning) |
| Remote phone access (optional) | Self-hosted WireGuard or Tailscale with the HA Companion app; no cloud relay required |

## 9.2 Discovery and entities

The controller publishes a **device-based MQTT Discovery payload** on boot;
HA auto-creates one device with the following entities:

| Entity | Type | Purpose |
| --- | --- | --- |
| `climate.dettson_hvac` | climate | Modes off/heat/cool/heat_cool, single + dual setpoints, current temperature, action |
| `sensor.dettson_active_equipment` | sensor | `idle` / `hp_heat` / `gas_heat` / `cool` / `defrost` — distinguishes heat-pump heat from gas heat (HA's `hvac_action` cannot) |
| `sensor.dettson_modulation` | sensor (%) | Live gas modulation: 0 or **40–100 %** (there is no 1–39 % band) |
| `sensor.dettson_outdoor_temp` | sensor (°C) | Outdoor temperature used by the dual-fuel logic |
| `sensor.dettson_outdoor_source` | sensor | Which source supplied it: `bus` / `wired` / `ha` / `none` |
| `sensor.dettson_fusion` | sensor (JSON attributes) | Effective room temperature, participating sensors, occupancy weighting |
| `sensor.dettson_compressor_min_off_remaining` | sensor (s) | Compressor minimum-off countdown |
| `binary_sensor.dettson_compressor_locked_out` | binary_sensor | Compressor lockout active (timers, low outdoor temp, fault) |
| `sensor.dettson_changeover_reason` | sensor | Why the last heat↔cool / gas↔HP changeover happened |
| `sensor.dettson_blower` | sensor | Blower/inducer state |
| `sensor.dettson_fault` | sensor | Decoded equipment fault code/text |
| `binary_sensor.dettson_health` | binary_sensor (problem) | Controller health (Wi-Fi/MQTT/sensor/watchdog) |
| `sensor.dettson_last_error` | sensor (diagnostic) | Last error string |
| `sensor.dettson_hold` | sensor (diagnostic) | Active hold type (`none` / `until_next_preset` / `two_hours` / `four_hours` / `indefinite`); remaining seconds as an attribute |
| `sensor.dettson_lock` | sensor (diagnostic) | Wall-screen lock state (`unlocked` / `user_locked` / `installer_locked`); lock level and whether a PIN is set as attributes (Section 9.8) |
| `switch.dettson_em_heat` | switch | EM HEAT (gas-only emergency heat): `ON` engages, `OFF` restores the prior mode. Deliberately a switch — not an HVAC mode and not a preset, so a scheduled preset write can never silently disengage it |
| Per-remote-sensor diagnostics | sensor / binary_sensor | Per-sensor age and fusion participation (diagnostic category) |
| `number.dettson_sensor_<id>_offset` | number (config) | Per-sensor calibration offset, ±5 °C in 0.1 °C steps — includes the built-in fallback sensor (id `local`) |
| Configuration numbers | number | Every parameter in Section 8, range-clamped by firmware |

## 9.3 Topic map (integrator reference)

Commands (HA → controller), all under `dettson/cmd/`:

| Topic | Payload |
| --- | --- |
| `dettson/cmd/setpoint` | float °C (heat or cool mode) |
| `dettson/cmd/target_temp_low` / `target_temp_high` | float °C (heat_cool mode dual setpoints) |
| `dettson/cmd/mode` | `off` / `heat` / `cool` / `heat_cool` |
| `dettson/cmd/fan_mode` | `auto` / `on` / `circulate` |
| `dettson/cmd/preset` | a preset name from the configured roster (default `home` / `away` / `sleep`) |
| `dettson/cmd/hold` | `until_next_preset` / `two_hours` / `four_hours` / `indefinite` / `clear` |
| `dettson/cmd/em_heat` | `ON` / `OFF` |
| `dettson/cmd/sensor/<id>/offset` | float °C within ±5 (per-sensor calibration; `local` = built-in fallback sensor) |
| `dettson/cmd/outdoor_temp` | float °C — HA-weather bridge feeding the outdoor-temperature ladder's third rung (`ha` source); plausibility-gated −50…55 °C; republish at least every 30 min or the rung goes stale (Section 8.8 fail-cold policy applies) |
| `dettson/cmd/next_target` | JSON `{"temp": 21.0, "mode": "heat", "in_s": 5400}` — the next scheduled setpoint change, for smart recovery (Section 9.7); all three keys required, invalid payloads ignored |
| `dettson/cmd/lock_clear` | exactly `clear_user_pin` — forgotten-PIN recovery (Section 9.8); any other payload, including empty, is ignored |

State (controller → HA), all under `dettson/state/` and echoing the
**post-validation** values: `current_temp`, `setpoint`, `target_temp_low`,
`target_temp_high`, `mode`, `fan_mode`, `preset`, `hold` (JSON: type +
remaining seconds), `em_heat`, `action`
(`off`/`idle`/`heating`/`cooling`/`fan`/`defrosting`), `active_equipment`,
`modulation`, `outdoor_temp`, `outdoor_source`, `fusion`,
`compressor_min_off_remaining`, `compressor_locked_out`,
`sensor/<id>/offset`, `changeover_reason`, `fault`, `blower`, `health`,
`last_error`, `lock` (JSON: state, level, whether a PIN is set).

**Important contract points:**

- **Setpoint clamping is firmware-side.** The dual-setpoint deadband
  (cool ≥ heat + 2.8 °C default, 1.1 °C hard floor) is enforced on the
  controller: violating writes are clamped and the other setpoint is pushed;
  HA sees the corrected values echoed on the state topics.
- **Availability/LWT:** the controller publishes `dettson/availability` =
  `online`; its MQTT **Last Will** sets `offline` so HA marks every entity
  unavailable the moment the controller drops off the broker.
- A rejected or malformed command payload never mutates controller state.

## 9.4 Remote room sensors (temperature + occupancy)

Remote sensors (Zigbee/ESPHome devices known to HA) reach the controller via
an **HA bridge automation** that republishes them to MQTT:

- **Per-sensor state topic:** `dettson/sensors/<id>/state`, JSON payload
  `{"temp": <°C>, "occ": true|false|null, "bat": 0-100|null, "hum": <%>|null}`.
- **Publish non-retained**, on change **and on a 60-second heartbeat**. The
  firmware's per-sensor staleness timeout (default 300 s) requires the live
  cadence; non-retained publication ensures a dead broker cannot replay
  stale temperatures.
- **Roster/config topic:** retained JSON at `dettson/config/sensors` —
  sensor ids, which presets each sensor participates in, and per-sensor
  staleness overrides.

Sensor **fusion happens in the controller, not in HA templates** — the
control input must survive an HA outage, and per-sensor health must be
visible to the controller's safety layer. Do not build an averaging template
in HA and feed it as a single sensor.

**Sensor hardware note:** use Zigbee temperature sensors (e.g., SONOFF
SNZB-02D, Aqara) and mmWave occupancy sensors (e.g., Aqara FP2, Apollo
MSR-2) integrated through HA. **Ecobee remote sensors cannot be used** —
their 915 MHz protocol is proprietary and unreceivable by this hardware.

## 9.5 Schedules and presets

- **HA owns schedules.** The HA scheduler or automations drive the
  thermostat by writing presets and/or setpoints. The controller does not
  store a weekly schedule.
- **The preset roster is configuration, not firmware.** Publish retained
  JSON at `dettson/config/presets` —
  `{"presets":[{"name":"home","heat":21.0,"cool":25.0}, …]}` — up to
  **8 entries**, names ≤ 23 characters, setpoints within the climate limits;
  invalid entries are skipped, and the climate entity's preset list is
  rebuilt from the roster (default `home`/`away`/`sleep`).
- **Holds:** a manual setpoint/mode change creates a hold of the configured
  default type (factory: until next preset). While a hold is active,
  scheduled preset writes are ignored; timed holds expire by clock and the
  next scheduled preset write restores the schedule (expiry never reverts
  setpoints on its own). See Section 8.2 for the timer values.
- **Starter package:** a ready-made HA package (weekly schedule, vacation
  calendar, runtime-based filter reminder, temperature/humidity alerts,
  presence-based Away) ships in the project's `ha/` directory with its own
  install instructions (`ha/README.md`). Load it at commissioning rather
  than hand-building these automations.
- **The controller owns the outage fallback.** If MQTT goes stale
  (> 30 min): heat-to 18 °C / cool-to 27 °C, mode = last user mode — never
  escalated to OFF, and never a single bare setpoint.

## 9.6 Safety interactions (binding on the integrator)

- HA is for **comfort and visibility, never a safety layer**. All failsafes
  live on the controller, the furnace IFC, and the equipment protections.
  Do not build HA automations that assume they are load-bearing for safety.
- Loss of HA/MQTT never raises demand of any kind; recovery from
  MQTT/sensor faults honors the compressor minimum-off timer.
- Health and diagnostics are published so silent failures are visible in HA
  (and on the touchscreen and status indication).
- As a diagnostic fallback, the controller serves a local web page usable
  when HA or the broker is down. It is a fallback, not a primary UI.

## 9.7 Smart recovery — the `next_target` contract

Smart recovery (pre-heat/pre-cool, Section 8.11) needs to know the *next*
scheduled setpoint before it arrives. Because HA owns the schedule
(Section 9.5), HA supplies that look-ahead:

- **Publish the next scheduled target** to `dettson/cmd/next_target` as
  JSON `{"temp": 21.0, "mode": "heat", "in_s": 5400}` — the upcoming
  setpoint, which side it serves (`heat` / `cool`), and the seconds until
  it takes effect. Republish whenever the schedule or the remaining time
  changes (piggyback on the sensor-bridge heartbeat, Section 9.4).
- **The parse is tolerant:** all three keys are required (`in_s` capped at
  7 days); an invalid payload is ignored, and publishing nothing simply
  means no pre-start. HA stays optional, as always.
- **Advisory only:** the recovery estimator recommends an early start; the
  control logic decides, and the compressor timers, dual-fuel arbitration,
  and every other protection still gate the resulting demand. A bogus
  `next_target` can at worst start a normal, fully protected call early —
  it can never bypass a lockout or timer.

## 9.8 Screen lock and forgotten-PIN recovery

The wall screen's PIN lock (user-facing description in the User Manual;
parameters in Section 8.10) surfaces in HA two ways:

- **Visibility:** `dettson/state/lock` → `sensor.dettson_lock`
  (diagnostic) carries the lock state, lock level, and whether a PIN is
  set, so a locked-out (or installer-locked) thermostat is diagnosable
  remotely.
- **Forgotten-PIN recovery:** publish exactly `clear_user_pin` to
  `dettson/cmd/lock_clear`. The user PIN is cleared and a user lock
  released — **no PIN required**. Rationale: anyone who can publish to the
  broker already has full climate control (every setpoint, mode, and EM
  HEAT), so **HA/broker access is admin by definition** — a PIN gate on
  this topic would add no security, only a second thing to forget. The
  lock is tamper resistance, never a safety layer.
- **The installer code is *not* clearable over MQTT.** Recovery from a
  lost installer code is physical (USB reflash / NVS wipe at the wall). An
  NVS wipe fails *open* — lock disabled, no PINs — so a wipe can never
  brick the wall UI.
