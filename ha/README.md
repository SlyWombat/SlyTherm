# Dettson DT-1 — Home Assistant starter config

Ready-made HA config closing gap **G3** in [`../docs/07-ecobee-gap-analysis.md`](../docs/07-ecobee-gap-analysis.md) — the thermostat deliberately stores no weekly schedule, vacation object, reminders, or presence logic; HA owns those ([`../docs/06-home-assistant.md`](../docs/06-home-assistant.md), *Schedules and presets*) — plus the first step of gap **G1** (accessory coordination blueprints). This directory makes that real instead of "bring your own YAML".

```text
ha/
├── packages/dettson_starter.yaml          # one self-contained HA package
└── blueprints/
    ├── dettson_presence_away.yaml         # reusable presence -> Away automation
    ├── dettson_filter_reminder.yaml       # reusable runtime-hours reminder
    ├── dettson_accessory_humidity.yaml    # standalone humidifier/dehumidifier coordination
    └── dettson_accessory_hrv.yaml         # standalone HRV/ventilator duty cycling
```

## Install

### 1. The package

1. Enable packages in `configuration.yaml` (once):

   ```yaml
   homeassistant:
     packages: !include_dir_named packages
   ```

2. Copy `packages/dettson_starter.yaml` into `<config>/packages/`.
3. Create the vacation calendar (config-flow only, cannot ship as YAML): **Settings → Devices & Services → Add Integration → Local Calendar**, name it **Dettson Vacation** → `calendar.dettson_vacation`.
4. Search the package for `EDIT ME` (humidity sensor entity, optional phone notify), restart HA.
5. First-load housekeeping: the threshold sliders start at their minimum — set **low temp 7 °C, high temp 32 °C, low RH 20 %, high RH 65 %, filter threshold 300 h**, and press **Dettson filter changed (reset)** once to stamp the install date.

### 2. The blueprints

Copy the blueprint files into `<config>/blueprints/automation/dettson/` (or import via **Settings → Automations & Scenes → Blueprints → Import**, pointing at the raw file URL if this repo is hosted), then create automations from them in the UI.

## What each piece does

| Piece | Behavior |
| --- | --- |
| `schedule.dettson_occupied` + preset automation | Weekly blocks (weekdays 06:00–22:30, Fri/Sat to 23:00, weekends from 07:00). ON → `home`, OFF → `sleep`. Daytime Away comes from presence, not the schedule. Stands down during vacation or presence-Away. |
| Vacation automation | Any `calendar.dettson_vacation` event with "vacation" in the title → `away` preset for the whole span, then hands back to the schedule (home/sleep per schedule state). |
| Filter reminder | `history_stats` counts **real blower hours** (`sensor.dettson_active_equipment` in `gas_heat`/`hp_heat`/`cool`; `defrost` excluded as noise); a nightly automation banks the daily total into `input_number.dettson_filter_runtime_h` (the recorder purges history after ~10 days, so a months-long single window is impossible); crossing `input_number.dettson_filter_alert_hours` raises a persistent notification; `input_button.dettson_filter_reset` zeroes the counter, stamps the date, dismisses the notification. |
| Temp/RH alerts | Mirror Ecobee's user-settable alert semantics in °C: low temp settable 1.5–20 °C (35–68 °F), high 15.5–40 °C (60–104 °F), RH 5–95 %. Temp reads the climate entity's `current_temperature` (the firmware's fused control input). RH needs your own sensor (`EDIT ME`); the firmware publishes no fused indoor-humidity entity. The firmware's own freeze-protection floors remain independent of these. |
| Presence → Away | Package version uses `zone.home` (count of persons home): 0 for 10 min → `away`; first return → resume schedule. Blueprint version lets you select explicit `person` entities, delay, and presets. |
| Filter blueprint | Same reminder, parameterized by runtime-hours threshold and counter entity — instantiate again for UV lamp / HRV core intervals. |
| Humidity accessory blueprint | RH-setpoint control of a standalone humidifier **or** dehumidifier (mode input) with hysteresis deadband, min on/off cycle timers, blower interlock from `sensor.dettson_active_equipment`, and optional Ecobee-style frost-control ceiling from `sensor.dettson_outdoor_temp` (see below). |
| HRV blueprint | Ecobee-style ventilator duty: N minutes at the top of each hour, separate occupied/unoccupied rates (optional presence entity), outdoor temp/RH lockout that also aborts a run in progress. |

## Accessory coordination (humidifier / dehumidifier / HRV) — gap G1, step 1

The thermostat has **no accessory output and no humidity logic** (docs/07 G1). These two blueprints turn that from "Missing" into "Partial via HA" by coordinating **user-supplied** hardware — any smart plug/switch (or natively smart unit exposing a `switch`/`fan`/`humidifier` entity) powering a standalone humidifier, dehumidifier, or HRV/ERV.

**Honest scope:** this is automation-level coordination, **not certified equipment control.** Nothing here is a safety layer or a substitute for the accessory's own protections: the device must be safe to be switched off (or left off) at any moment and must keep its own float switches, compressor delays, and HRV core/defrost logic. If HA is down, the accessory simply doesn't run (or, after a mid-run HA restart, an HRV may run until the next top of the hour). Both blueprints fail toward OFF on bad sensor data — except the HRV outdoor lockout, which fails toward fresh air when the outdoor sensor is unavailable (comfort feature, documented in the input).

- `dettson_accessory_humidity.yaml` — pick `humidify` or `dehumidify`, point it at your RH sensor and an `input_number` setpoint. The **blower interlock** (default on) only runs the accessory while `sensor.dettson_active_equipment` is in `gas_heat` / `hp_heat` / `cool` — the exact wire states proving moving air (`idle` is no blower; `defrost` excluded, same as the filter counter) — mandatory for duct-mounted humidifiers, safe to disable for standalone room units. **Frost control** (optional, humidify only) mirrors Ecobee's Frost Control + Window Efficiency: a piecewise-linear ceiling on the RH target computed from `sensor.dettson_outdoor_temp` (45 % at ≥ +5 °C down to 15 % at −30 °C; curve table in the blueprint header), shifted ±2.5 %RH per window-efficiency step (1 = drafty single pane … 7 = high-efficiency triple pane; Ecobee's exact constants are unpublished — this is the standard cold-climate condensation guideline). Min on/off cycle timers protect dehumidifier compressors.
- `dettson_accessory_hrv.yaml` — minutes-per-hour duty at the top of each hour (Ecobee-style minimum runtime; default 20 occupied / 5 unoccupied via an optional presence entity), with a free-cooling-style lockout: no ventilation when outdoor temp leaves the configured band (default −25…+30 °C) or an optional outdoor RH sensor exceeds its limit (the thermostat publishes no outdoor RH — bring your own or use a weather integration sensor).

Instantiate the humidity blueprint twice (one humidifier, one dehumidifier on the same sensor) for year-round control; leave a gap between the two setpoints so they never fight.

## Entities expected from the thermostat (MQTT discovery)

Cross-checked against `lib/HaMqtt/HaMqtt.h` / `.cpp` discovery payloads (`unique_id` / `name`). If `lib/HaMqtt` changes, [`../docs/06-home-assistant.md`](../docs/06-home-assistant.md) is the contract this directory follows.

| Entity | Used for | Source (`HaMqtt`) |
| --- | --- | --- |
| `climate.dettson_hvac` | preset commands (`home`/`away`/`sleep`, exact lowercase wire strings), `current_temperature` attribute for alerts | `climateDiscoveryJson()`, `unique_id: dettson_hvac` |
| `sensor.dettson_active_equipment` | blower-hours counting; blower interlock in the humidity accessory blueprint; states `idle`/`hp_heat`/`gas_heat`/`cool`/`defrost` | `activeEquipmentDiscoveryJson()`, `unique_id: dettson_active_equipment` |
| `sensor.dettson_outdoor_temp` | frost-control ceiling (humidity blueprint), HRV outdoor lockout | docs/06 entity table (`dettson/state/outdoor_temp`; discovery builder not yet in `HaMqtt.cpp`) |

You provide: `calendar.dettson_vacation` (Local Calendar), `person.*` with phone trackers (HA Companion app), an indoor %RH sensor for the humidity alerts and the accessory blueprint (e.g. SONOFF SNZB-02D per docs/06 *Remote sensors*), and the accessory hardware itself (smart-switched humidifier/dehumidifier/HRV — see *Accessory coordination* above).

## Version requirements

Honest floor, per feature actually used: **HA ≥ 2023.1** (Local Calendar integration); everything else here is older and stable — `schedule` YAML helper (2022.9), calendar triggers and `today_at()` (2022.5), `history_stats` multi-state lists, `input_button` (2021.12). Developed against **HA 2024.x+** conventions and recommended to run there; no feature requiring newer than 2024.1 is used (the schedule helper's per-block `data:` attributes, which would need 2024.10, were deliberately avoided).

**Validation status:** these files are syntax-validated (YAML parse) only — they have not yet been loaded into a live Home Assistant instance. Run **Developer Tools → YAML → Check configuration** after install.

## Known edges (by design, kept simple)

- Vacation ending while everyone is out resumes the schedule (home/sleep) until the next presence departure re-applies Away.
- The nightly accumulator forfeits the final minute of each day's runtime — irrelevant at a 300 h threshold.
- A manual preset change holds until the next schedule edge (the firmware's documented "hold until next activity" behavior); the package adds no timed holds.
