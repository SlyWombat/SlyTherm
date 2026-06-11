# Safety information for users

Your thermostat controls a **gas appliance** and a **heat pump compressor**.
The thermostat never operates the gas valve or the compressor directly — it
sends requests, and the furnace's certified control board and the heat pump's
own electronics decide whether and how to run, with all of their built-in
safety systems (flame sensing, overheat limits, pressure switches, compressor
protections) always active. Even so, please read and follow this section.

## Carbon monoxide (CO) alarm — required

**Install and maintain a working CO alarm in your home.** Any home with a
gas-burning appliance should have one regardless of which thermostat is on
the wall. Test it monthly and replace batteries per the alarm manufacturer's
instructions.

**If your CO alarm sounds:** get everyone outside to fresh air immediately,
then call emergency services or your gas utility from outside. Do not
re-enter until told it is safe.

**If you smell gas:** do not touch any electrical switches (including the
thermostat). Leave the building immediately and call your gas utility from
outside.

## Rules for safe use

- **Never block or cover** the furnace's air intakes, exhaust vents, supply
  registers, or return grilles, and keep the area around the outdoor heat
  pump unit clear of snow, leaves, and debris.
- **Do not open** the thermostat, the furnace cabinet, or the heat pump.
  There are no user-serviceable parts inside any of them.
- **Do not press the thermostat's screen with sharp objects** or mount
  anything over it.
- The thermostat is powered from the furnace. **Do not switch off the
  furnace's power** to "reset" the system unless instructed by a service
  technician — in winter, a powered-down system cannot protect your home
  from freezing.

## Understanding safety-related alerts

The thermostat shows alerts in a banner on the home screen (and, if Home
Assistant is connected, as notifications on your phone). The full alert table
is in the *Alerts and troubleshooting* section. The safety-relevant ones, in
plain language:

| Alert | What it means | What to do |
| --- | --- | --- |
| **Furnace fault** (with a code) | The furnace's own control board has reported a problem and may have shut itself down. The thermostat displays the code; it cannot override the furnace. | Note the code and call your HVAC service company. |
| **Heating stopped — no progress** | Gas heat ran continuously for 4 hours without reaching the target, so the thermostat stopped the call and raised an alert. This can mean an open window or door, an undersized setting on an extreme day, or an equipment problem. | Check for open windows/doors and a clogged filter. If it recurs, call a professional. |
| **No heat — system in safe state** | The thermostat detected a fault it could not resolve and has stopped all requests. In winter this is itself urgent: an unheated home risks frozen pipes. The alert escalates the longer it persists. | Call your installer or HVAC service company promptly. Ask your installer about the backup thermostat changeover they prepared at installation. |
| **Backup sensor mode** | All room sensors became unavailable and the thermostat is running on its built-in backup sensor only. Heating is limited to a safe minimum range and cooling is disabled. | See the troubleshooting section; if it persists, call your installer. |

## When to call a professional

Call a licensed HVAC technician (do not attempt yourself) if:

- A furnace fault code appears repeatedly.
- The "no heat — system in safe state" alert appears.
- You hear unusual noises, smell burning or gas, or see water around the
  furnace or indoor coil.
- Heating or cooling performance changes suddenly without a weather
  explanation.

Routine actions you **can** do yourself: changing setpoints and modes,
replacing the furnace filter per the furnace manual, replacing remote-sensor
batteries, and clearing snow or debris from around the outdoor unit.

## A note on what this thermostat will never do

- It never disables or bypasses any furnace or heat pump safety device —
  those are physically out of its reach by design.
- It never runs gas heat and heat pump heat together (briefly tempering the
  air during the heat pump's defrost cycle is the one designed exception).
- It never restarts the compressor faster than the compressor's protection
  timers allow — even while recovering from a fault.
- When in doubt, it stops requesting heating and cooling and tells you.
