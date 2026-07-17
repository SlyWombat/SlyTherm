# Recreating the wall panel's "System" chart in Home Assistant

For the HA administrator. The wall panel's **System** tab shows a *Last 12 h*
trend with three lines — **Room** (averaged room temperature), **Setpoint**, and
**Outside** — matching the on-device colours (Room = white, Setpoint = orange,
Outside = blue).

On the panel that chart is fed by the SlyLog graph publisher over
`slytherm/graph/system` (issue #156), so the *panel* doesn't need HA. This doc is
for rebuilding the **same chart as an HA dashboard card**, straight from the
thermostat's own MQTT entities — no SlyLog and no custom integration required.

## Source entities

| Line | HA entity | Notes |
|---|---|---|
| **Room** (avg) | `sensor.slytherm_fusion` | the fused/averaged room temperature (°C) — auto-discovered |
| **Outside** | `sensor.slytherm_outdoor_temp` | outdoor temperature (°C) — auto-discovered |
| **Setpoint** | `sensor.slytherm_setpoint` | **you must add this** (below) — the active setpoint isn't a standalone sensor by default |

### Add the Setpoint sensor (one time)

The active setpoint is published on `slytherm/state/setpoint`, but it's consumed by
the `climate.slytherm_hvac` entity, not exposed as its own sensor. Add this to your
MQTT sensors (e.g. in `configuration.yaml` under `mqtt:` → `sensor:`, or drop it
into a package):

> **Firmware note:** `slytherm/state/setpoint` (and `.../target_temp_low` /
> `.../target_temp_high`) are published **retained** as of Controller firmware
> **≥ 1.2.8**. On earlier firmware they were transient (non-retained), so the MQTT
> sensor below would sit `unavailable` until the next setpoint change and
> `mosquitto_sub -W 3` would see nothing — that was expected, not a broker problem.
> On ≥ 1.2.8 the sensor populates immediately on (re)connect.

```yaml
mqtt:
  sensor:
    - name: "SlyTherm Setpoint"
      unique_id: slytherm_setpoint
      state_topic: "slytherm/state/setpoint"
      unit_of_measurement: "°C"
      device_class: temperature
      state_class: measurement
      availability_topic: "slytherm/availability"
      payload_available: "online"
      payload_not_available: "offline"
```

Reload MQTT (Developer Tools → YAML → *MQTT entities*, or restart HA). You now have
`sensor.slytherm_setpoint`.

## Option A — built-in `history-graph` card (no custom cards)

Simplest, works on any HA. Add this card to a dashboard (Edit dashboard → Add card
→ *Manual*):

```yaml
type: history-graph
title: System — last 12 h
hours_to_show: 12
refresh_interval: 30
entities:
  - entity: sensor.slytherm_fusion
    name: Room
  - entity: sensor.slytherm_setpoint
    name: Setpoint
  - entity: sensor.slytherm_outdoor_temp
    name: Outside
```

HA picks the line colours automatically; the shape and 12 h window match the panel.

## Option B — `ApexCharts` card (matches the panel's look)

Nicer, and lets you pin the exact colours. Requires the **apexcharts-card** custom
card (HACS → Frontend → *ApexCharts Card*). Then:

```yaml
type: custom:apexcharts-card
header:
  title: System — last 12 h
  show: true
graph_span: 12h
yaxis:
  - min: ~
    apex_config:
      title: { text: "°C" }
series:
  - entity: sensor.slytherm_fusion
    name: Room
    color: white
    stroke_width: 2
  - entity: sensor.slytherm_setpoint
    name: Setpoint
    color: orange
    stroke_width: 2
    curve: stepline        # setpoint changes are steps, like the panel
  - entity: sensor.slytherm_outdoor_temp
    name: Outside
    color: "#4da6ff"
    stroke_width: 2
```

## Notes

- **History depth:** the chart only shows as much past data as HA's recorder has
  retained for these entities. If your `recorder` `purge_keep_days` is short, older
  points won't appear until they accumulate. 12 h needs no special config.
- **Averaged room temp:** `sensor.slytherm_fusion` is already the multi-room fused
  value the control loop uses, so it matches the panel's "Room" line exactly —
  don't average the individual room sensors yourself.
- **Setpoint in Auto mode:** `slytherm/state/setpoint` is the single active
  setpoint the firmware is driving to. In heat_cool/Auto you may instead want the
  low/high pair — those are on `slytherm/state/target_temp_low` /
  `.../target_temp_high`; add them as two more MQTT sensors and series if desired.
```
