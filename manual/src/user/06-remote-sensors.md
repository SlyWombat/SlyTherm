# Remote room sensors

## What they do

A thermostat that only measures the hallway keeps the *hallway* comfortable.
Remote sensors let the thermostat see the rooms you actually live in:

- **Room averaging.** The thermostat blends readings from all healthy
  sensors into one "effective" room temperature and controls to that.
- **Follow-me comfort.** Sensors that detect occupancy get roughly double
  the influence of empty rooms, shifting comfort toward where people
  actually are. The shift is gradual (over tens of minutes), so walking
  through a room doesn't yank the temperature around.
- **Per-preset rooms.** Presets can use different sensor groups — typically
  Sleep follows the bedrooms, Home follows the living areas.

The blending happens inside the thermostat itself, so an established sensor
average keeps working even during a network or Home Assistant hiccup.

## Supported sensors

Sensors connect through Home Assistant — they are standard smart-home
devices, not proprietary thermostat accessories:

| Type | Examples | Notes |
| --- | --- | --- |
| **Zigbee temperature** | SONOFF SNZB-02D, Aqara temperature sensors | Battery powered, small, cheap; needs a Zigbee radio on your Home Assistant box |
| **Occupancy (mmWave)** | Aqara FP2, Apollo MSR-2 | Detects presence even when you sit still — better than simple motion sensors for follow-me |
| **ESPHome-based** | Apollo MSR-2 and other ESPHome devices | Wi-Fi devices managed from Home Assistant |

Any temperature or occupancy sensor that Home Assistant can read can be
bridged to the thermostat. Your installer sets up the bridge and the sensor
roster. (Ecobee's own wireless sensors are **not** compatible — they use a
proprietary radio protocol that only Ecobee thermostats can receive.)

**Where the room temperature comes from.** On this wall unit the room
temperature comes **entirely from your Home Assistant / MQTT room sensors** —
there is no temperature sensor built into the wall thermostat itself. That
keeps the reading in the rooms you live in rather than at the hallway wall,
but it also means the thermostat depends on Home Assistant and your network
to know the temperature. If every room sensor stops reporting, the thermostat
has no temperature to control to and safely **pauses heating and cooling**
(see below) — it never guesses.

## Where to put sensors

![Sensor placement do and don't](diagrams/sensor-placement.svg)

**Do:**

- Mount on an **interior wall** of a room you actually use, about
  **1.2–1.5 m above the floor** (chest height), with open air around it.
- Put one in the **main living area** and one in the **bedroom(s)** you want
  comfortable at night.
- Give occupancy sensors a clear view of the space they watch.

**Avoid:**

- Direct sunlight, and spots above or near **heating/cooling vents,
  radiators, lamps, or electronics** — these read the heat source, not the
  room.
- **Kitchens and bathrooms** (cooking and showers swing the reading).
- Exterior doors, drafty hallways, and uninsulated exterior walls.
- Behind curtains or furniture, or inside cabinets.

## What happens when a sensor misbehaves

The thermostat checks every sensor continuously and protects itself from bad
data:

- **Sensor goes quiet** (dead battery, out of range): after about 5 minutes
  without a fresh reading, the sensor is dropped from the average and listed
  as stale on the Sensors page. The remaining sensors carry on.
- **Sensor reads nonsense** (frozen value, impossible temperature, or more
  than 4 °C away from what the other sensors agree on): it is excluded and
  an alert names the sensor.
- **A sensor joins or leaves the average**: the effective temperature glides
  to the new value rather than jumping, so you won't get a sudden burst of
  heating or cooling because a battery died.
- **All room sensors lost**: with no sensor built into the wall unit, the
  thermostat has no temperature to control to. It **stops all heating and
  cooling requests** (the safe direction — it never runs equipment on data
  it can't trust) and raises a persistent alert reminding you to restore the
  sensors. This depends on the Home Assistant bridge and your network being
  up, so this alert usually points at those. (Your equipment's own built-in
  safety limits stay active regardless.)

Routine care: replace sensor batteries when the Sensors page (or Home
Assistant) shows them low — typically once a year or two for Zigbee sensors.
