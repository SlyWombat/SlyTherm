# Alerts and troubleshooting

The thermostat reports problems in the alert banner on the home screen, on
the Diagnostics page, and (when connected) as notifications in the Home
Assistant app. This section explains what each message means and what to do.

> **First, the golden rule:** when this thermostat is unsure, it *stops
> asking for heating and cooling* and tells you. Most alerts therefore mean
> "comfort may suffer until this is fixed", not "something dangerous is
> happening". The dangerous-condition protections live inside the furnace
> and heat pump themselves and are always active.

Alerts that describe a passing condition — a room sensor gone quiet, the
Wi-Fi or app link dropping, the outdoor temperature briefly unknown — **clear
themselves automatically** the moment the condition recovers (a sensor
reports again, the link reconnects). You do not have to acknowledge them.
A handful of serious faults stay listed until acknowledged so a brief problem
you didn't see isn't lost.

## "Waiting" states (normal — not faults)

| What you see | What it means | What to do |
| --- | --- | --- |
| **"Heat pump ready in M:SS"** countdown | The compressor is resting between runs (about 5 minutes), or the thermostat is limiting it to 3 starts per hour, or the system just powered up. | Nothing — it starts by itself when the countdown ends. |
| Changed AUTO direction but nothing happens | Switching between heating and cooling requires the need to persist about 10 minutes, and at least 30 minutes since the opposite operation. | Wait. If you need cooling *right now*, switch the mode to COOL manually. |
| **"Defrosting"** with steam/whoosh at the outdoor unit | Normal winter defrost of the heat pump; the furnace briefly tempers the air. | Nothing — a few minutes, then normal operation resumes. |
| Heat pump runs for hours without stopping | Normal for an inverter heat pump near its balance point — long, low-power runs are its most efficient behaviour. | Nothing, if the room is holding temperature. |
| Cooling refuses to start in a cold room | Cooling is locked out when the indoor temperature is below 18 °C, to protect the equipment. | Nothing — this is by design. |

## Alert table

| Alert / symptom | What it means | What to do |
| --- | --- | --- |
| **Sensor "name" offline / excluded** | That room sensor stopped reporting (likely a dead battery) or disagreed wildly with the others. The system continues on the remaining sensors. | Check the sensor's battery and placement; see the Sensors page for details. |
| **No room temperature — heating/cooling paused** (persistent banner) | *All* room sensors are unavailable and this wall unit has no sensor of its own, so there is no temperature to control to. The thermostat safely pauses all heating and cooling. | Restore the sensors: check sensor batteries, then whether Home Assistant and your network are running (this alert usually points at the network or bridge). If it persists, call your installer. |
| **Heat pump locked out — outdoor temperature** | It is colder outside than the compressor's safe limit (default −20 °C). Gas heat carries the load automatically. | Nothing — normal cold-weather behaviour. |
| **Heating switched to gas — heat pump couldn't keep up** | The heat pump ran flat out but the room kept drooping, so gas took over. | Nothing — this is the dual-fuel design working. Frequent occurrences in mild weather are worth mentioning to your installer. |
| **Gas heating stopped — ran 4 hours without progress** | A safety limit: gas heat ran continuously for 4 hours without gaining on the setpoint, so the thermostat dropped the call and alerted you. | Look for open windows/doors, a clogged filter, or an extreme-weather day. Clear/acknowledge and let it retry; if it recurs, call a professional. |
| **Furnace fault (code …)** | The furnace's own control board reported a problem. The thermostat displays it but cannot fix or override it. | Note the code from the Diagnostics page and call your HVAC service company. |
| **Wi-Fi / App icon grey** | Network or Home Assistant connection lost. Temperature control continues locally; after 30 minutes the fallback comfort range (heat 18 / cool 27 °C) applies until reconnection. | Check your router and the Home Assistant computer. The wall screen keeps full control meanwhile. |
| **Furnace link lost** | The thermostat cannot talk to the furnace over its communication wiring. Heating requests cannot be delivered. | Power-cycling your router won't help — this is the wired link. If it doesn't clear within minutes, call your installer. In winter treat this as urgent (heat is unavailable). |
| **No heat — system in safe state** (escalating) | The thermostat hit a fault it could not resolve and stopped all requests. The alert repeats and escalates because an unheated home in winter risks frozen pipes. | Call your installer or HVAC service promptly. Your installer prepared a backup-thermostat changeover for exactly this case. |
| **System restarted repeatedly — locked safe** (SAFE MODE screen) | The controller restarted several times in a short period and locked itself in the no-demand state. To stay usable it also boots a **simplified screen** — just the temperature, mode, and alerts, with the graph and other extras switched off — instead of risking another restart. | Heating/cooling stays paused until cleared. Tap **"Restore full screen"** on that screen to return to the normal display and clear the lock; if it happens again, call your installer. Do not repeatedly power-cycle it yourself. |
| **Blank screen** | The thermostat has no power. It is powered from the furnace, so the furnace likely has no power either. | Check the furnace's switch and breaker. If power is on and the screen stays dark, call your installer. |
| Room comfortable but a far room is cold/hot | The thermostat controls to its sensor average; rooms without sensors (or with closed doors/vents) drift. | Add a sensor to that room, or check vents and doors. |
| Vents blow cool-ish air in heat-pump heating | Heat pump supply air (often 30–40 °C) feels cooler than gas-furnace air, especially on your hand — but it is still heating. | Nothing, if the room holds temperature. |

## Reading the Diagnostics page to a technician

When you call for service, open **Diagnostics** and have ready:

1. The current alerts (and any furnace fault code).
2. The connection health rows (Wi-Fi / App / Furnace).
3. What was running at the time ("Running now" on the home screen).

> **⚠ To be confirmed during installation:** the exact wording of furnace fault codes shown on the
> Diagnostics page depends on the installed equipment configuration and will
> be finalized after on-site verification. Treat any fault code as "call for
> service" until this manual is updated.
