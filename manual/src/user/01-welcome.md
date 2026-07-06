# Welcome

Your SlyTherm Dual-Fuel Smart Thermostat controls two pieces of heating and
cooling equipment as one system:

- a **Dettson Chinook modulating gas furnace**, and
- a **Gree-built heat pump**, which provides both heating and air conditioning.

"Dual fuel" means the thermostat automatically chooses the best heat source
for the conditions. In mild weather it favours the heat pump, which moves heat
instead of burning fuel. In cold weather, when a heat pump becomes less
effective, it switches to gas heat. You set the temperature; the thermostat
decides how to get there.

## What makes it different

- **True modulating gas control.** Most thermostats can only switch a furnace
  on or off. This thermostat speaks the furnace's own digital language and
  asks for exactly the amount of heat needed — anywhere from 40 % to 100 % of
  the furnace's capacity. The result is longer, gentler, quieter heating
  cycles and steadier room temperatures, instead of blasts of full heat
  followed by cool-downs.

- **Automatic dual-fuel changeover.** The thermostat watches the outdoor
  temperature and switches between heat pump and gas heat around a
  configurable outdoor "balance point", with built-in protection so the
  switch never harms the compressor.

- **Heating and cooling in one.** Modes for heat, cool, automatic
  changeover between them, and a gas-only emergency heat mode.

- **A real touchscreen on the wall.** A 4.3-inch colour touchscreen shows the
  current temperature, both setpoints, what equipment is running, the outdoor
  temperature, and any alerts — and it keeps working even if your home
  network is down.

- **Remote room sensors.** Add small wireless temperature and occupancy
  sensors in the rooms you actually use. The thermostat blends their readings
  and gives more weight to occupied rooms ("follow me" comfort).

- **Local-first smart control.** The thermostat connects to **Home
  Assistant**, a free home-automation platform that runs on your own
  equipment in your own home. Schedules, phone control, history graphs, and
  notifications all work without any cloud service, subscription, or account.
  If Home Assistant or your network goes down, the thermostat keeps
  controlling temperature on its own.

- **Safety-first design.** If anything goes wrong — a sensor fails, the
  controller locks up, the network disappears — the thermostat's designed
  response is always to *stop asking for heating or cooling*, never to run
  equipment blindly. The furnace's and heat pump's own built-in protections
  remain fully in charge of their machinery at all times.

## What you need

| Item | Required? |
| --- | --- |
| Dettson Chinook furnace + Gree-built heat pump (professionally installed) | Yes |
| The SlyTherm wall thermostat (professionally installed and commissioned) | Yes |
| Home Assistant on your home network | For schedules, phone app, and remote sensors |
| Remote room sensors (Zigbee or ESPHome-based) | Optional |
| A working carbon monoxide (CO) alarm | **Yes — see the safety section** |

You can use the thermostat entirely from the wall screen if you wish. Home
Assistant adds scheduling, the phone app, and remote sensors, but day-to-day
temperature control never depends on it.
