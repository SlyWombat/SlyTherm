# Getting to know your thermostat

The thermostat is a 4.3-inch colour touchscreen mounted on the wall where
your old thermostat was. Everything you need day to day is on the **Home**
screen; three more pages (Sensors, Diagnostics, Settings) are a tap away.

## The home screen

![DT-1 home screen with numbered callouts](diagrams/home-screen.svg)

| # | Element | What it shows / does |
| --- | --- | --- |
| 1 | **Current temperature** | The room temperature the thermostat is controlling to — a blend of your room sensors (see *Remote sensors*). Shows `--.-` if no valid reading is available. |
| 2 | **Setpoint dial** | Two coloured arcs around the dial: orange for the **heat** setpoint, blue for the **cool** setpoint. Drag an arc's knob to change that setpoint. The two setpoints always keep a minimum gap (see *Everyday use*). |
| 3 | **Setpoint readout** | The two setpoints as numbers, e.g. `19.0 / 24.0` (heat / cool). |
| 4 | **Activity** | What the system is doing right now: *Idle*, *Heating*, *Cooling*, *Fan*, or *Defrosting*. |
| 5 | **Mode row** | Tap to choose **OFF / HEAT / COOL / AUTO / EM HEAT**. The active mode is highlighted. |
| 6 | **Preset row** | Tap **Home / Away / Sleep** comfort presets (see *Schedules and presets*). |
| 7 | **Outdoor temperature** | The outside temperature the thermostat is using, with its source (heating equipment, outdoor sensor, or weather service). |
| 8 | **Running now** | Which equipment is active: gas heat (with its current output, 40–100 %), heat pump heat, cooling, fan, or defrost. |
| 9 | **Compressor protection countdown** | When the heat pump is resting between runs, a countdown shows when it may start again. Hidden when not relevant. This is normal protection, not a fault. |
| 10 | **Alert banner** | The most recent alert, in plain language. Hidden when there is nothing to report. Also shows the "backup sensor mode" notice when applicable. |
| 11 | **Connection icons** | Status of **Wi-Fi**, the link to your **Home Assistant app**, and the link to the **furnace**. Green = good. The thermostat keeps controlling temperature even when Wi-Fi or the app link is down. |
| 12 | **Page tabs** | Switch between Home, Sensors, Diagnostics, and Settings. |

## The other pages

- **Sensors** — a table of every room sensor: its name, temperature, whether
  the room is occupied, how fresh the reading is, whether it is currently
  included in the room average, and its health. Useful for checking a sensor
  battery or seeing why a room is being ignored.

- **Diagnostics** — the alert history plus connection health (Wi-Fi, app
  link, furnace link), the compressor countdown, and the furnace's current
  output. This is the page a service technician will ask you to read out.

- **Settings** — mostly informational on the wall unit: the active minimum
  gap between heat and cool setpoints, and software version information.
  Detailed tuning (schedules, balance point, sensor options) is done in the
  Home Assistant app, not on the wall.

## Things worth knowing

- **The screen is the boss's messenger, not the boss.** Everything you tap
  is double-checked by the thermostat's control core before any equipment is
  asked to run. Out-of-range requests are corrected automatically.
- **It works without the network.** Wi-Fi loss never stops heating or
  cooling. You will see the Wi-Fi icon turn grey; local control continues.
- **Temperatures are in °C** in 0.5° steps.
