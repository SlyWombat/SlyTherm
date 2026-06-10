# 04 — Safety & Functional Safety

> **This controls a gas appliance. Read this document fully before energizing anything.**

## 1. The safety boundary (the premise the whole project rests on)

We **do not command the gas valve.** We send a CT-485 **heat *demand*** — a capacity/setpoint *request*. The furnace's certified **Integrated Furnace Control (IFC)** is what opens/modulates the gas valve, and it independently enforces **every** combustion safety:

**What the certified IFC guarantees regardless of anything we send over CT-485:**

- Flame sensing & **flame-failure lockout** (no proven flame → gas valve closes, lockout).
- **Primary high-limit** (overtemp → shutdown).
- **Rollout switch** and **pressure/vent (draft) switch** proving.
- **Ignition-trial limits / retries / hard lockout.**
- Internal modulation limits — never fires above its rated input regardless of demand.

These interlocks are hard-wired/firmware-locked inside the certified IFC and are **not on the RS-485 bus.** They cannot be disabled over CT-485, and a missing or malformed demand message does **not** open the gas valve.

**What *we* must guarantee:**

1. **Fail to NO-HEAT (demand = 0 / no message) on any abnormal condition.** A *silent* ESP32 = furnace sees the thermostat drop demand = furnace ramps down/off via its normal logic. Our failsafe is **"go quiet,"** not "send an off command we hope arrives."
2. **Never babble on the bus** (don't corrupt furnace↔heat-pump comms).

> 🚩 **Stop condition.** If sniffing ever shows the thermostat sending a *raw valve duty* that the IFC obeys **without independent flame/limit supervision**, **stop the project** — that would be an uncertified, unsafe design out of scope for a hobby build on a gas appliance. Verify the demand semantics before trusting any decode.

## 2. Failure modes → required failsafes

| Failure mode | Risk | Required failsafe |
| --- | --- | --- |
| **Wi-Fi loss** | lose HA / remote setpoint | keep last good setpoint **locally**; PID keeps running on local sensor; never let "no Wi-Fi" mean max demand |
| **MQTT broker loss** | no setpoint updates | local fallback setpoint, **bounded**; stale-setpoint timeout reverts to a safe default (e.g. 18 °C or OFF) |
| **Temp-sensor loss/fault** (open, short, NaN, frozen, out-of-range) | garbage PID input → **runaway demand** — *most dangerous software failure* | **mandatory fault detection:** range check + DS18B20 CRC + staleness/"stuck value" check. On fault → **demand = 0** and alarm. Never feed an invalid temp into the PID. |
| **ESP32 crash / hang / deadlock** | PID stops; last demand could be stuck **on** | **external hardware watchdog** that on timeout **forces demand OFF** (not merely resets). CT-485 also helps: stop sending thermostat messages → furnace sees thermostat lost → drops the call. |
| **Brownout / power sag** | random reset, partial state | **keep brownout detector enabled**; bulk cap; **boot state = no demand** until sensor + setpoint validated |
| **Bus contention / collision** | talk over the furnace, corrupt frames | **explicit DE/RE; TX only in your slot; release bus (DE low) at idle and boot/reset**; listen-before-talk; CRC every frame; can't get clean comms → **go passive** |
| **Runaway heat / overshoot** | too hot | first line = furnace high-limit; our line = **PID output clamp + anti-windup + max-demand cap** |
| **Stuck-on modulation / logic bug** | continuous heating | **max-runtime limiter:** heat call persisting beyond N minutes without reaching setpoint → **drop demand + alarm**; plus **min-off / anti-short-cycle** timer |

## 3. Required mechanisms (implement all — defence in depth)

- **Independent hardware watchdog with a fail-safe output.** The ESP32's internal WDT only reboots the chip — not enough. Add an external watchdog (TPL5010 / MAX6369 class, or an ATtiny) that the ESP32 must pet. **On timeout, the external part forces the no-demand state** (holds transceiver DE off / cuts a relay so the ESP32 can't drive the bus → furnace sees thermostat-lost → idles). **Default-safe state of all hardware = NO HEAT.**
- **Heartbeat / deadman via CT-485 presence.** Rely on the furnace interpreting *loss of periodic thermostat messages as "drop the heat call."* **Verify this on your unit during commissioning** (pull the bus mid-call, confirm it ramps down). If loss-of-comms does **not** drop the call on your furnace, you need a *hardware* means to remove the call — and that materially raises project risk; reconsider.
- **Layered watchdogs:** internal task WDT (hung PID task) + external HW WDT (dead chip) + furnace comms-loss timeout (catch-all).
- **Software defaults:** boot = OFF/no-demand; every PID cycle re-validates *(sensor OK? setpoint fresh? comms OK?)* before any nonzero demand; clamp + anti-windup; max-runtime; anti-short-cycle.
- **Alarming:** publish health/last-error to MQTT **and** a local status LED so a silent failure is visible.
- **Demand refresh discipline:** re-issue `HEAT_DEMAND` well within the protocol's refresh timer (high nibble minutes / low nibble 3.75 s units) so a sluggish loop never drops fire mid-cycle.

## 4. Sensors (safety-relevant)

- **MQTT room sensor = primary comfort input; DS18B20 = always-present local safety/fallback.** Never a single source.
- If MQTT value is **stale / out-of-range / absent → fall back to DS18B20**. If **both bad → demand 0 + alarm.**
- **DS18B20 fault detection (all mandatory):** CRC check; reject the **85 °C power-on default** and **−127 °C disconnect** sentinels; range gate (e.g. 5–40 °C plausible); staleness/stuck-value check.
- Optional **2nd DS18B20** for true redundancy/voting. **Do not** place the PID room sensor near the furnace (reads plenum heat, not room).

## 5. Certification, code, warranty, insurance — honestly

- **CSA** certifies gas appliances in Canada/US. The furnace is certified **as a system with an approved control.**
- The OEM **R02P030 thermostat is discontinued**. Replacing it with a homemade controller is a **modification of a certified gas appliance's control system.** Riding the bus *as a thermostat* (IFC keeps all safeties) does **not** alter the certified combustion-safety path — but it **is outside the certified configuration.**
- **Honest implications:** this **can void the furnace warranty**, may **violate local gas/electrical code** (work on gas-appliance controls is often restricted to licensed technicians), and **could affect home insurance** after any fire/CO incident traced to the controller.

**Conservative requirements before/while running this:**

- **Do not remove or bypass any furnace safety wiring.** Tap CT-485 only; leave R/C/1/2 semantics intact.
- **Install a CO alarm** in the home (you should have one regardless).
- **Keep the OEM thermostat** available for instant rollback to the certified config.
- **Have a licensed HVAC tech review the final install** and confirm the comms-loss-drops-call behaviour.
- **Document** that the design does not alter the combustion-safety chain.
- Treat this as **experimental, at-your-own-risk, on a life-safety appliance.** If you're not willing to own that, use a Dettson-supported communicating thermostat and integrate at the HA level instead.

## 6. Commissioning safety checklist

- [ ] CO alarm installed and working.
- [ ] OEM thermostat retained and rollback procedure known.
- [ ] Sniff-only validated; field dictionary complete; modulation byte confirmed.
- [ ] Confirmed (by capture) the IFC enforces flame/limit/pressure independently of the bus.
- [ ] Confirmed comms-loss drops the heat call (pull-bus test).
- [ ] External hardware watchdog forces NO-DEMAND on hang (bench-tested).
- [ ] Sensor-fault → demand-0 path bench-tested (unplug DS18B20, kill MQTT).
- [ ] Brownout detector enabled; boots to no-demand.
- [ ] PID output clamp, max-runtime, anti-short-cycle, demand-refresh all in place and tested.
- [ ] DE/RE pulls to idle at boot/reset (scope-verified bus stays silent during reset).
- [ ] Licensed HVAC tech reviewed the install.
