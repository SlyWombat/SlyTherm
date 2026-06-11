# Dettson DT-1 — Home Assistant starter config

Ready-made HA config closing gap **G3** in [`../docs/07-ecobee-gap-analysis.md`](../docs/07-ecobee-gap-analysis.md): the thermostat deliberately stores no weekly schedule, vacation object, reminders, or presence logic — HA owns those ([`../docs/06-home-assistant.md`](../docs/06-home-assistant.md), *Schedules and presets*). This directory makes that real instead of "bring your own YAML".

```text
ha/
├── packages/dettson_starter.yaml          # one self-contained HA package
└── blueprints/
    ├── dettson_presence_away.yaml         # reusable presence -> Away automation
    └── dettson_filter_reminder.yaml       # reusable runtime-hours reminder
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

Copy both files into `<config>/blueprints/automation/dettson/` (or import via **Settings → Automations & Scenes → Blueprints → Import**, pointing at the raw file URL if this repo is hosted), then create automations from them in the UI.

## What each piece does

| Piece | Behavior |
| --- | --- |
| `schedule.dettson_occupied` + preset automation | Weekly blocks (weekdays 06:00–22:30, Fri/Sat to 23:00, weekends from 07:00). ON → `home`, OFF → `sleep`. Daytime Away comes from presence, not the schedule. Stands down during vacation or presence-Away. |
| Vacation automation | Any `calendar.dettson_vacation` event with "vacation" in the title → `away` preset for the whole span, then hands back to the schedule (home/sleep per schedule state). |
| Filter reminder | `history_stats` counts **real blower hours** (`sensor.dettson_active_equipment` in `gas_heat`/`hp_heat`/`cool`; `defrost` excluded as noise); a nightly automation banks the daily total into `input_number.dettson_filter_runtime_h` (the recorder purges history after ~10 days, so a months-long single window is impossible); crossing `input_number.dettson_filter_alert_hours` raises a persistent notification; `input_button.dettson_filter_reset` zeroes the counter, stamps the date, dismisses the notification. |
| Temp/RH alerts | Mirror Ecobee's user-settable alert semantics in °C: low temp settable 1.5–20 °C (35–68 °F), high 15.5–40 °C (60–104 °F), RH 5–95 %. Temp reads the climate entity's `current_temperature` (the firmware's fused control input). RH needs your own sensor (`EDIT ME`); the firmware publishes no fused indoor-humidity entity. The firmware's own freeze-protection floors remain independent of these. |
| Presence → Away | Package version uses `zone.home` (count of persons home): 0 for 10 min → `away`; first return → resume schedule. Blueprint version lets you select explicit `person` entities, delay, and presets. |
| Filter blueprint | Same reminder, parameterized by runtime-hours threshold and counter entity — instantiate again for UV lamp / HRV core intervals. |

## Entities expected from the thermostat (MQTT discovery)

Cross-checked against `lib/HaMqtt/HaMqtt.h` / `.cpp` discovery payloads (`unique_id` / `name`). If `lib/HaMqtt` changes, [`../docs/06-home-assistant.md`](../docs/06-home-assistant.md) is the contract this directory follows.

| Entity | Used for | Source (`HaMqtt`) |
| --- | --- | --- |
| `climate.dettson_hvac` | preset commands (`home`/`away`/`sleep`, exact lowercase wire strings), `current_temperature` attribute for alerts | `climateDiscoveryJson()`, `unique_id: dettson_hvac` |
| `sensor.dettson_active_equipment` | blower-hours counting; states `idle`/`hp_heat`/`gas_heat`/`cool`/`defrost` | `activeEquipmentDiscoveryJson()`, `unique_id: dettson_active_equipment` |

You provide: `calendar.dettson_vacation` (Local Calendar), `person.*` with phone trackers (HA Companion app), and an indoor %RH sensor for the humidity alerts (e.g. SONOFF SNZB-02D per docs/06 *Remote sensors*).

## Version requirements

Honest floor, per feature actually used: **HA ≥ 2023.1** (Local Calendar integration); everything else here is older and stable — `schedule` YAML helper (2022.9), calendar triggers and `today_at()` (2022.5), `history_stats` multi-state lists, `input_button` (2021.12). Developed against **HA 2024.x+** conventions and recommended to run there; no feature requiring newer than 2024.1 is used (the schedule helper's per-block `data:` attributes, which would need 2024.10, were deliberately avoided).

**Validation status:** these files are syntax-validated (YAML parse) only — they have not yet been loaded into a live Home Assistant instance. Run **Developer Tools → YAML → Check configuration** after install.

## Known edges (by design, kept simple)

- Vacation ending while everyone is out resumes the schedule (home/sleep) until the next presence departure re-applies Away.
- The nightly accumulator forfeits the final minute of each day's runtime — irrelevant at a 300 h threshold.
- A manual preset change holds until the next schedule edge (the firmware's documented "hold until next activity" behavior); the package adds no timed holds.
