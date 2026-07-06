# Schedules and presets

## How scheduling works — an honest explanation

The wall thermostat itself **does not store a weekly schedule**. Scheduling
lives in **Home Assistant**, the home-automation system the thermostat
connects to. Home Assistant changes the thermostat's preset (or setpoints)
at the times you choose, and the thermostat carries them out.

This split is deliberate:

- Home Assistant's scheduler is far more capable than anything that would
  fit on a wall screen — different weekday/weekend programs, presence-based
  rules ("switch to Away when both phones leave"), calendar integration, and
  easy editing from your phone or computer.
- The thermostat stays simple and dependable: it always knows its current
  setpoints and keeps controlling temperature even if Home Assistant goes
  quiet.

**What this means for you:** to set up or change a schedule, you use the
Home Assistant app or web page — not the wall screen. If you never set up
Home Assistant, the thermostat simply holds whatever setpoints and mode you
set by hand, like a classic non-programmable thermostat.

## Presets: Home, Away, Sleep — and your own

A preset is a named pair of setpoints. The three standard presets are:

| Preset | Typical use | Example setpoints (heat / cool) |
| --- | --- | --- |
| **Home** | Normal occupied comfort | 21 / 24 °C |
| **Away** | Nobody in for hours — save energy | 16 / 28 °C |
| **Sleep** | Overnight | 18 / 26 °C |

The example values are only suggestions — the actual setpoints behind each
preset are configured in Home Assistant to suit your household.

You are not limited to these three. The preset list itself is configured in
Home Assistant: you (or your installer) can rename presets or add your own —
**Workout**, **Guests**, **Movie night** — up to **eight** in total, each
with its own heat/cool pair. Whatever list is configured appears on the
wall screen's preset row and in the phone app. If a custom preset's two
setpoints are set too close together, the thermostat repairs the pair
automatically, keeping the cool value and lowering the heat value to
restore the minimum gap (see *Everyday use*, "The minimum gap").

You can switch presets three ways:

1. **Automatically by schedule** — the usual way: e.g. Sleep at 22:30, Home
   at 06:30, Away on weekday work hours.
2. **By tapping the preset row** on the wall screen — handy when the
   schedule guessed wrong ("we're home early").
3. **From the phone app** — the preset selector on the thermostat card.

A manual preset or setpoint change normally stays in effect until the next
scheduled change comes along — unless you place a timed or indefinite
**hold**, which protects your change from the schedule for longer (see
*Everyday use*, "Holding a temperature").

Presets can also influence which room sensors are used — for example, Sleep
can be configured to follow the bedroom sensors only. See *Remote sensors*.

## Setting a vacation on the wall unit

If you are going away, you can set a **vacation** right on the thermostat —
no phone or Home Assistant needed. On the **Presets** page, tap the
**Vacation** button to open the vacation planner:

- **Starts** — *Today* or a number of days from now.
- **Length** — how many nights you will be away.
- **Eco heat / Eco cool** — the energy-saving setpoints to hold while you are
  gone (the planner keeps them a safe gap apart). The defaults are a mild
  16 °C heat / 28 °C cool, roughly the Away preset.

Tap **Start vacation** and the thermostat takes over for that date range. On
the Home screen you will see a **"Vacation until <date>"** banner. While the
vacation is running, the eco setpoints hold **regardless of the schedule or
who is home**, so a schedule change or a returning phone won't cancel it. At
the end date the thermostat **automatically resumes** normal operation and
puts your previous setpoints back. To end a vacation early, open the planner
again and tap **Cancel vacation**.

A vacation is remembered through a power cut, and — if you also use the Home
Assistant *vacation calendar* from the starter package — both work the same
way; use whichever is handier. When the thermostat is PIN-locked, setting or
cancelling a vacation asks for the PIN, like any other comfort change.

## Smart recovery (pre-heat / pre-cool)

A schedule normally *starts* heating or cooling at the programmed time —
which means the house only *reaches* the new temperature some time later.
Smart recovery fixes that: the thermostat quietly learns how fast your
home warms up and cools down with your equipment, and starts the system
early so the home is **at** the scheduled temperature **at** the scheduled
time. Set Home for 06:30, and 06:30 is when it's warm — not when the
furnace lights.

Smart recovery is **off out of the box**: it needs a connected Home
Assistant schedule and a little tuning, so your installer enables it once
the system has been commissioned and has had time to learn. All of the
usual equipment protections apply to an early start, just like any other.

## A starter schedule

You don't have to build any of this from scratch: the thermostat ships with
a **ready-made Home Assistant starter package** that your installer can load
in a few minutes. It sets up a sensible weekly schedule (editable to taste),
a **vacation calendar** (add an event named "vacation" and the house drops
to Away for exactly that span), a **filter-change reminder** keyed to real
blower running hours, low/high **temperature and humidity alerts**, and
**presence-based Away** (everyone's phone leaves → Away; first one home →
back on schedule). See the *Mobile app* chapter and the Installation
Manual, Section 9. The package also includes optional ready-made
blueprints that coordinate a standalone humidifier, dehumidifier, or
HRV/ventilator with the thermostat — ask your installer if you have one
of these.

If you would rather build your own schedule, this pattern serves most
households well:

| Time | Weekdays | Weekends |
| --- | --- | --- |
| 06:30 | Home | Home (or 07:30) |
| 08:30 | Away (if the house empties) | — |
| 17:00 | Home | — |
| 22:30 | Sleep | Sleep |

Set it up in Home Assistant with a Scheduler card/automation targeting the
thermostat's preset. Your installer may have already created one for you.

## If Home Assistant goes down

Nothing dramatic happens. The thermostat keeps its last setpoints and mode
and continues controlling. If it hears nothing from Home Assistant for more
than 30 minutes, it falls back to a conservative safety net — heat to
18 °C, cool above 27 °C, in your last chosen mode — so the home can neither
freeze nor swelter while the network is out. When Home Assistant returns,
schedules resume on their own.
