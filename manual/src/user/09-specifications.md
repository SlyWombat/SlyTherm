# Specifications

## Wall unit

| Item | Specification |
| --- | --- |
| Display | 4.3-inch colour IPS touchscreen, 480 × 272, capacitive touch, landscape |
| Power source | 24 V AC from the furnace (no batteries; no wall adapter) |
| Power consumption | ≤ ~3 VA from the furnace transformer (about 2 W; measured for this class of display board, to be confirmed on the production unit) |
| Connectivity | Wi-Fi (2.4 GHz) to your home network; wired digital link to the furnace |
| Smart-home integration | Home Assistant (local network, MQTT); Home Assistant Companion app |
| Built-in backup sensor | Wired digital temperature sensor |
| Mounting | Replaces the existing thermostat at the same wall location (professional installation) |

## Control ranges and behaviour

| Item | Value |
| --- | --- |
| Setpoint range | 10–30 °C (the wall dial may display a wider range, but every request is validated by the controller; 10–30 °C is the effective control range) |
| Setpoint adjustment step | 0.5 °C |
| Minimum heat/cool setpoint gap | 2.8 °C default; never below 1.1 °C |
| Reaction threshold (hysteresis) | ~0.55 °C past setpoint |
| Gas furnace output range | 0 (off), or 40–100 % of capacity (modulating) |
| Cooling lockout | No cooling when indoor temperature is below 18 °C |
| Remote sensor accepted range | 5–40 °C (readings outside are rejected as faulty) |
| Remote sensor timeout | Dropped from the average after ~5 min without a fresh reading |

## Protection timers and dual-fuel defaults

These are factory defaults; your installer can tune them within safe bounds.

| Parameter | Default |
| --- | --- |
| Compressor minimum off time | 5 min |
| Compressor minimum run time | 5 min |
| Compressor starts per hour (max) | 3 |
| Compressor hold-off after power-up | ~5 min |
| Heat/cool changeover: sustained need required | 10 min |
| Heat/cool changeover: minimum time since opposite call | 30 min |
| Dual-fuel balance point (heat pump vs gas) | −8 °C outdoor (±2 °C switching band) |
| Compressor low-temperature lockout | −20 °C outdoor |
| Gas/aux heat lockout (mild weather) | above 10 °C outdoor |
| Heat-pump-to-gas escalation | room ≥1 °C below setpoint for 30 min at full heat-pump output |
| Gas heat maximum continuous run before alert/stop | 4 h |
| Defrost air tempering | a small amount of furnace heat during defrost (exact behaviour finalized during installation) |
| Network-loss fallback setpoints (after 30 min) | heat 18 °C / cool 27 °C, last mode kept |
| Backup-sensor-mode heating range | 16–18 °C (cooling disabled) |

> **⚠ To be confirmed during installation:** heat pump operating limits (including the exact
> low-temperature rating of the installed outdoor unit) are confirmed against
> the installed model during commissioning; the −20 °C lockout default is
> set conservatively until then.

## Compatible equipment

| Role | Equipment |
| --- | --- |
| Gas furnace | Dettson Chinook modulating gas furnace (communicating control) |
| Heat pump / air conditioning | Gree-built heat pump matched to the Chinook |
| Remote sensors | Zigbee and ESPHome temperature/occupancy sensors via Home Assistant (e.g. SONOFF SNZB-02D, Aqara, Apollo MSR-2, Aqara FP2) |

This thermostat is purpose-built for the equipment combination above. It is
not a general-purpose replacement thermostat for other furnaces or heat
pumps.
