# Getting to know your thermostat

The thermostat is a 4.3-inch colour touchscreen mounted on the wall where
your old thermostat was. Everything you need day to day is on the **Home**
screen; five more pages (Presets, Sensors, System, Settings, Diagnostics)
are a tap away — tap the SlyTherm logo in the top-left to drop down the page
menu, or swipe left/right.

## The home screen

![SlyTherm home screen with numbered callouts](diagrams/home-screen.svg)

| # | Element | What it shows / does |
| --- | --- | --- |
| 1 | **Top bar** | Across the top: the **SlyTherm logo** (tap it for the page menu), the **Wi-Fi dot**, the **outdoor temperature**, and the **clock** (day + time once the thermostat has the time). |
| 2 | **NOW temperature** | The large temperature on the left is the room temperature the thermostat is controlling to — a blend of your room sensors (see *Remote sensors*). Shows `--.-°` if no valid reading is available. Its colour warms to amber when heating, cools to blue when cooling. |
| 3 | **Presence line** | Just under the big temperature: a short plain-language line such as which room is driving the reading, or whether the home is showing as occupied. |
| 4 | **HEAT / COOL cards** | On the right, one or two cards show your **heat** setpoint (amber) and **cool** setpoint (blue) as big numbers, each with **– and +** buttons to change it in 0.5° steps. In HEAT you see the heat card, in COOL the cool card, in AUTO both stacked. The two setpoints always keep a minimum gap (see *Everyday use*). |
| 5 | **Mode bar** | The segmented bar across the middle: tap **OFF / HEAT / COOL / AUTO / EM HEAT**. The active mode is highlighted. When the system is **OFF**, a "System off" note appears where the cards would be. |
| 6 | **Hold pill** | Below the presence line: shows the active hold and its time remaining ("Hold • 1h 59m left", "Hold until next schedule", "Hold until you change it"). Tap it to choose a hold length or to **Resume schedule** (see *Everyday use*). Hidden when nothing is held and when the system is OFF. |
| 7 | **Vacation banner** | A pill across the top of Home reading **"Vacation until <date>"** whenever a vacation is set (see *Schedules and presets*). It disappears on its own when the vacation ends. |
| 8 | **Outdoor temperature** | In the top bar: the outside temperature the thermostat is using (from the heating equipment, an outdoor sensor, or a weather service). |
| 9 | **Compressor protection countdown** | When the heat pump is resting between runs, the System page shows a countdown for when it may start again. This is normal protection, not a fault. |
| 10 | **Alert banner** | The most recent alert, in plain language, appears when there is something to report — including the "no room temperature" notice if every room sensor is lost. Hidden when all is well. |
| 11 | **Wi-Fi dot** | The small dot in the top bar shows the **Wi-Fi** link (green = connected). The thermostat keeps controlling temperature even when Wi-Fi or the app link is down; deeper link status lives on **Diagnostics**. |
| 12 | **Page menu** | Tap the **SlyTherm logo** (top-left) to drop down the page menu — Home, Presets, Sensors, System, Settings, Diagnostics — or swipe left/right. |

## The other pages

- **Presets** — the comfort presets (Home, Away, Sleep, and any your
  household has added) as cards. Tap one to apply it; the active preset is
  outlined. Same as tapping a preset elsewhere, with a bit more room. At the
  bottom is a **Vacation** button that opens the on-device vacation planner
  (see *Schedules and presets*).

- **Sensors** — a table of every room sensor in aligned columns: **Room**
  (its friendly name), **Temp** (its temperature), and **Status** (In use,
  Following — the sensor currently driving the system, Away with how long, or
  *stale* if the reading is old), with an **On/Off** button on the right to
  include or exclude that room from the average. Useful for checking a sensor
  battery or seeing why a room is being ignored.

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
  output, including the CT-485 furnace-bus frame count when the bus is wired.
  This is the page a service technician will ask you to read out.

## Screens you will see now and then

- **Ambient (screensaver)** — after a few minutes untouched, the screen dims
  to a calm clock-and-temperature view so it isn't glaring in a dark room.
  Touch anywhere to wake it back to Home; nothing about your heating changes
  while it is dimmed.

- **Welcome (first-time setup)** — the very first time the thermostat powers
  up (or after a factory reset) it walks you through joining Wi-Fi and linking
  the Home Assistant app. You will not normally see this again.

- **Safe mode** — if the thermostat detects a problem with itself it falls
  back to a stripped-down safe screen that still shows the temperature and
  keeps the equipment's own safety systems in charge. If you ever see it,
  note any message shown and contact your installer.

## Things worth knowing

- **The screen is the boss's messenger, not the boss.** Everything you tap
  is double-checked by the thermostat's control core before any equipment is
  asked to run. Out-of-range requests are corrected automatically.
- **It works without the network.** Wi-Fi loss never stops heating or
  cooling. You will see the Wi-Fi icon turn grey; local control continues.
- **Temperatures are in °C** in 0.5° steps.
