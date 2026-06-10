# 06 — Home Assistant Integration

Local-only, over MQTT, using **MQTT Discovery** so HA auto-creates the entities. No cloud.

## Entities

| Entity | Type | Purpose |
| --- | --- | --- |
| `climate.dettson_furnace` | `climate` (HVAC) | setpoint, mode (heat/off), current temp, action |
| `sensor.dettson_modulation` | `sensor` (%) | live gas-valve modulation % (from `Get Status`/DMA) |
| `sensor.dettson_blower` | `sensor` (RPM/%) | blower/inducer state |
| `sensor.dettson_fault` | `sensor` | decoded fault code/text (from `Get Diagnostics 0x86`) |
| `binary_sensor.dettson_health` | `binary_sensor` (problem) | controller health (Wi-Fi/MQTT/sensor/watchdog) |
| `sensor.dettson_last_error` | `sensor` (diagnostic) | last error string |

## Climate entity (MQTT Discovery sketch)

Published once on boot to `homeassistant/climate/dettson_furnace/config`:

```jsonc
{
  "name": "Dettson Furnace",
  "unique_id": "dettson_furnace",
  "modes": ["off", "heat"],
  "min_temp": 10, "max_temp": 30, "temp_step": 0.5,
  "temperature_unit": "C",
  "current_temperature_topic": "dettson/state/current_temp",
  "temperature_command_topic":  "dettson/cmd/setpoint",
  "temperature_state_topic":    "dettson/state/setpoint",
  "mode_command_topic":         "dettson/cmd/mode",
  "mode_state_topic":           "dettson/state/mode",
  "action_topic":               "dettson/state/action",   // idle/heating/off
  "availability_topic":         "dettson/availability",   // online/offline (LWT)
  "device": { "identifiers": ["dettson_esp32"], "name": "Dettson ClimateTalk Thermostat",
              "manufacturer": "ElectricRV", "model": "ESP32 CT-485" }
}
```

## Topic map

| Direction | Topic | Payload |
| --- | --- | --- |
| HA → ESP32 | `dettson/cmd/setpoint` | float °C |
| HA → ESP32 | `dettson/cmd/mode` | `off` / `heat` |
| ESP32 → HA | `dettson/state/current_temp` | float °C (the PID input source) |
| ESP32 → HA | `dettson/state/setpoint` | echo |
| ESP32 → HA | `dettson/state/mode` | `off` / `heat` |
| ESP32 → HA | `dettson/state/action` | `idle` / `heating` / `off` |
| ESP32 → HA | `dettson/state/modulation` | 0–100 % |
| ESP32 → HA | `dettson/state/fault` | code/text |
| ESP32 → HA | `dettson/availability` | `online` / `offline` (MQTT **Last Will** = `offline`) |

## Room-temperature source

The PID input is the **HA/MQTT room sensor** (a sensor where people actually are), with the local **DS18B20 as fallback** if the MQTT value is stale/out-of-range/absent. The ESP32 subscribes to the room sensor's state topic (or HA pushes it to `dettson/cmd/room_temp`). If both sources fail → demand 0 (see [`04-safety.md`](04-safety.md) §4).

## Safety interactions with HA

- **HA/MQTT loss must never raise heat.** MQTT **Last Will** sets `offline`; the firmware falls back to a bounded local setpoint and keeps the local sensor PID running — it never interprets "broker gone" as "max demand."
- Health/diagnostics published so a silent failure is visible in HA (and on the local status LED).
- HA is for **comfort and visibility**, not a safety layer — all failsafes live on the ESP32 + furnace IFC.
