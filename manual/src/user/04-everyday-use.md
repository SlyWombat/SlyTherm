# Everyday use

## Setting the temperature

From the wall screen:

1. On the **Home** screen, find the dial with the orange (heat) and blue
   (cool) arcs.
2. Drag the **orange knob** to change the heat setpoint — the temperature
   below which heating starts.
3. Drag the **blue knob** to change the cool setpoint — the temperature
   above which cooling starts.
4. The new values appear immediately in the setpoint readout. There is
   nothing to "save".

From your phone: open the thermostat card in the Home Assistant Companion
app and drag the same setpoints there (see *Mobile app*).

**The minimum gap.** The cool setpoint must always be at least a set amount
above the heat setpoint (factory default 2.8 °C). If you move one setpoint
too close to the other, the thermostat automatically pushes the other one
along to keep the gap. This prevents the system from heating and cooling the
same air in turns.

## Choosing a mode

Tap a button on the mode row: **OFF / HEAT / COOL / AUTO / EM HEAT**.

![Modes at a glance](diagrams/modes-at-a-glance.svg)

- **OFF** — no heating or cooling. The screen, sensors, and alerts stay
  active. Note: there is no "away for the winter" frost-protection in OFF —
  if you leave for an extended period in winter, use HEAT or AUTO with a low
  heat setpoint instead.

- **HEAT** — holds the heat setpoint. The thermostat automatically picks the
  heat source: the **heat pump** in milder weather, the **gas furnace** in
  colder weather (see "How the thermostat chooses" below). You don't manage
  this — it is what "dual fuel" means.

- **COOL** — holds the cool setpoint, using the heat pump as a central air
  conditioner. To protect the equipment, cooling will not run when the
  indoor temperature is below 18 °C.

- **AUTO** — uses **both** setpoints and changes over between heating and
  cooling by itself. Heats when the room drops below the heat setpoint,
  cools when it rises above the cool setpoint, does nothing in between.
  Ideal for spring and fall, when mornings need heat and afternoons need
  cooling.

- **EM HEAT (emergency heat)** — gas furnace only; the heat pump is not used
  at all. Use this if the heat pump is damaged, iced up, or awaiting
  service. Emergency heat is selected on the wall screen; in the phone app
  the mode list shows Off / Heat / Cool / Auto.

## Choosing a preset

Tap **Home**, **Away**, or **Sleep** on the preset row. Each preset is a
pair of setpoints you (or your installer) define in Home Assistant — for
example, Away might be heat 16 / cool 28 to save energy while nobody is in.
Schedules normally switch presets for you automatically (see *Schedules and
presets*).

## Why the thermostat sometimes waits

You will occasionally change a setting and hear… nothing. A short wait is
almost always deliberate protection, not a problem:

- **Compressor rest time.** The heat pump's compressor must rest for about
  5 minutes after it stops before it may start again, and once started it
  runs at least 5 minutes. Starting a compressor against pressure that
  hasn't equalized shortens its life dramatically — every quality thermostat
  enforces this. The home screen shows a countdown ("Heat pump ready in
  4:32") while it applies. The thermostat also limits the compressor to 3
  starts per hour and waits about 5 minutes after a power outage before the
  first compressor start.

- **Changeover wait (AUTO mode).** Switching between heating and cooling has
  its own patience built in: the system waits a sustained period (about 10
  minutes of genuine need, and at least 30 minutes since the opposite
  operation) before reversing direction. This stops a sunny hour from
  flipping your system from heat to cool and back.

- **Reaction threshold.** The thermostat doesn't chase every 0.1° wiggle —
  it acts when the room moves meaningfully past a setpoint (about 0.5 °C).

## How the thermostat chooses heat pump vs gas

In HEAT and AUTO modes the choice is automatic, based mainly on outdoor
temperature:

- **Milder than the balance point** (factory default −8 °C): the heat pump
  heats. It may run for long stretches near this temperature — for an
  inverter heat pump, long gentle runs are normal and efficient, not a
  fault.
- **Colder than the balance point**: the gas furnace heats, modulating
  smoothly between 40 % and 100 % of its capacity to match the need.
- **The heat pump falls behind**: if the room droops well below the setpoint
  for half an hour with the heat pump flat out, the thermostat steps over to
  gas until conditions improve.
- **Very cold weather**: below the compressor's safe operating limit
  (default −20 °C outdoor), the heat pump is locked out entirely and gas
  carries the load.
- **Mild weather**: above about 10 °C outdoor, gas heat is locked out and
  the heat pump handles any heating need.

Gas heat and heat pump heat never run at the same time — the equipment
manufacturer prohibits it, and the thermostat enforces it. The one designed
exception: during the heat pump's **defrost cycle** in winter, the furnace
briefly tempers the air so the vents don't blow cold (see below).

![How AUTO mode decides](diagrams/auto-mode-decision.svg)

## About defrost (winter, heat pump running)

When the outdoor unit frosts up in cold, damp weather it periodically runs a
defrost cycle: the outdoor fan stops, you may see steam rising from the
outdoor unit and hear a whoosh or hiss, and the screen shows **Defrosting**.
The system briefly adds a little furnace heat during the cycle so the indoor
air stays comfortable. Defrost lasts a few minutes and is completely normal.

## After a power outage

The thermostat restarts in a deliberately cautious state: it re-checks its
sensors and settings before asking anything to run, and holds the heat pump
off for about 5 minutes. Your modes, setpoints, and presets are remembered.
If the outage interrupted a compressor run, expect the heat pump to wait out
its full rest time before restarting.
