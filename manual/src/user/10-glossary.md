# Glossary

**Auto-changeover (AUTO mode)** — the thermostat switches between heating
and cooling by itself, using both setpoints, with built-in waits so brief
temperature swings don't flip the system back and forth.

**Backup sensor mode** — a degraded-but-safe state entered when all remote
room sensors are unavailable: the thermostat runs on its built-in sensor,
limits heating to a modest range, disables cooling, and alerts you.

**Balance point** — the outdoor temperature (default −8 °C) below which the
thermostat prefers gas heat over the heat pump.

**Compressor** — the heart of the heat pump; the motor-driven pump that
moves refrigerant. It needs rest periods between starts, which is why the
thermostat sometimes shows a countdown before the heat pump runs.

**Defrost** — a normal winter cycle in which the heat pump briefly melts
frost off its outdoor coil. Steam from the outdoor unit and a whooshing
sound are normal; the furnace tempers the indoor air meanwhile.

**Dual fuel** — a system with two heat sources (here: heat pump and gas
furnace) where a controller picks the better one for the conditions.

**Emergency heat (EM HEAT)** — a mode that uses the gas furnace only,
bypassing the heat pump entirely. For when the heat pump is faulty or being
serviced.

**ESPHome** — an open-source system for Wi-Fi smart-home sensors and
devices that integrate with Home Assistant. Some supported room sensors are
ESPHome devices.

**Follow-me** — comfort weighting that gives occupied rooms more influence
on the controlled temperature, using occupancy-capable sensors.

**Heat pump** — equipment that heats or cools by moving heat between
indoors and outdoors. Very efficient in mild weather; less effective in
severe cold, which is why this system pairs it with a gas furnace.

**Home Assistant** — free, open-source home-automation software that runs
on a small computer in your home. Provides the thermostat's schedules,
phone app, sensor bridge, and history — all locally, with no cloud.

**Modulating furnace** — a furnace that can run anywhere between 40 % and
100 % of its capacity instead of just on/off, giving longer, quieter, more
even heating. This thermostat requests the exact firing rate (40–100 %) and
the furnace's certified control carries it out.

**Occupancy sensor** — a sensor that detects whether people are present in
a room. mmWave types (radar-based) detect even motionless people.

**Preset** — a named pair of setpoints (Home / Away / Sleep) switched by
schedule, by tap, or from the app.

**Setpoint** — a target temperature. This thermostat keeps two: the heat
setpoint (heating starts below it) and the cool setpoint (cooling starts
above it).

**Short-cycle protection** — the timers that prevent the compressor from
restarting too soon or too often, protecting it from damage.

**Zigbee** — a low-power wireless standard used by many battery-powered
smart-home sensors; connects to Home Assistant through a Zigbee radio.
