# Getting to know your thermostat

The thermostat is a 4.3-inch colour touchscreen mounted on the wall where
your old thermostat was. Everything you need day to day is on the **Home**
screen; five more pages (Presets, Sensors, System, Settings, Diagnostics)
are a tap away — tap the SlyTherm logo in the top-left to drop down the page
menu, or swipe left/right.

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
| 10 | **Alert banner** | The most recent alert, in plain language. Hidden when there is nothing to report. Shows the "no room temperature" notice if every room sensor is lost. |
| 11 | **Connection icons** | Status of **Wi-Fi**, the link to your **Home Assistant app**, and the link to the **furnace**. Green = good. The thermostat keeps controlling temperature even when Wi-Fi or the app link is down. |
| 12 | **Hold pill** | Shows the active hold and its time remaining ("Hold 1:59", "Hold until next schedule", "Hold until you change it"). Tap it to choose a hold length or to **Resume schedule** (see *Everyday use*). Reads "Set a hold" when nothing is held. |
| 13 | **Page menu** | Tap the logo (top-left) for the page menu — Home, Presets, Sensors, System, Settings, Diagnostics — or swipe. |

## The other pages

- **Presets** — the comfort presets (Home, Away, Sleep, and any your
  household has added) as cards. Tap one to apply it; the active preset is
  outlined. Same as the preset row on Home, with a bit more room.

- **Sensors** — a table of every room sensor: its name, temperature, whether
  the room is occupied, how fresh the reading is, whether it is currently
  included in the room average, and its health. Tap a sensor's button to
  include or exclude it. Useful for checking a sensor battery or seeing why a
  room is being ignored.

- **System** — a plain-language summary of what the equipment is doing right
  now (running equipment, the room average, outdoor temperature, compressor
  status, gas output), plus a **12-hour trend graph** on the right showing the
  room temperature against your heat and cool setpoints — handy for seeing at
  a glance how the house held overnight. The graph starts filling in after the
  thermostat has been running a while and resets on a power cycle.

- **Settings** — set and lock a **PIN** (see *Everyday use*), choose the clock
  format (**12-hour / 24-hour**), and open **Wi-Fi setup** and **Home system**
  (Home Assistant link). Each of those two shows a status word that turns
  **green when it is connected and working**. Detailed tuning (schedules,
  balance point, sensor options) is done in the Home Assistant app, not on the
  wall.

- **Diagnostics** — the alert history plus connection health (Wi-Fi, app
  link, furnace link), the compressor countdown, and the furnace's current
  output. This is the page a service technician will ask you to read out.

## Things worth knowing

- **The screen is the boss's messenger, not the boss.** Everything you tap
  is double-checked by the thermostat's control core before any equipment is
  asked to run. Out-of-range requests are corrected automatically.
- **It works without the network.** Wi-Fi loss never stops heating or
  cooling. You will see the Wi-Fi icon turn grey; local control continues.
- **Temperatures are in °C** in 0.5° steps.
