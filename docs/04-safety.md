# 04 — Safety & Functional Safety

> **This controls a gas appliance and a heat-pump compressor. Read this document fully before energizing anything.**

## 1. The safety boundary (the premise the whole project rests on)

We **do not command the gas valve.** We send a CT-485 **heat *demand*** — a capacity/setpoint *request*. The furnace's certified **Integrated Furnace Control (IFC)** is what opens/modulates the gas valve, and it independently enforces **every** combustion safety:

**What the certified IFC guarantees regardless of anything we send over CT-485:**

- Flame sensing & **flame-failure lockout** (no proven flame → gas valve closes, lockout).
- **Primary high-limit** (overtemp → shutdown).
- **Rollout switch** and **pressure/vent (draft) switch** proving.
- **Ignition-trial limits / retries / hard lockout.**
- Internal modulation limits — never fires above its rated input regardless of demand.

These interlocks are hard-wired/firmware-locked inside the certified IFC and are **not on the RS-485 bus.** They cannot be disabled over CT-485, and a missing or malformed demand message does **not** open the gas valve.

**What *we* must guarantee (prime directive):**

1. **Fail to NO-DEMAND on any abnormal condition — *all* channels** (HEAT 0x64, COOL 0x65, FAN 0x66, BACKUP 0x67, DEFROST 0x68, AUX 0x69 — and, in the relay path, **all relays de-energized**) — **and never violate compressor minimum timers on the way down or back up.** A *silent* node / open relay = equipment sees the demand drop = it ramps down/off via its normal logic. Our failsafe is **"go quiet / de-energize,"** not "send an off command we hope arrives." Recovery from a failsafe trip must itself pass through the compressor timers (a flapping fault must not short-cycle the compressor via the failsafe path).
2. **Never babble on the bus** (don't corrupt furnace↔interface-board comms).

### 1a. What the heat-pump ODU guarantees (verify on the installed model) vs what we enforce

The compressor side has the same shape as the gas side — we request, the Gree inverter/coil-board executes — **but with a critical difference: the ODU's protections are equipment protections, not a certified life-safety chain.** Treat them as a backstop only; our timers and interlocks are primary.

**What the Gree-built ODU enforces itself (documented for FLEXX-class units — verify against the installed model's manual in Phase 0):**

- Refrigerant-circuit protections: high-pressure (E1), low-pressure (E3 — **smaller units have *no* low-pressure switch**), high discharge temperature (E4), compressor overload (H4), IPM/module protection (H5, P5, P8 class).
- An internal **~3-minute compressor restart delay** and a minimum run time.
- **Repeated protection trips latch a manual-reset fault** — i.e., abuse from a misbehaving controller doesn't just self-recover; it strands the system until someone power-cycles/services it.
- Operating envelope: heating to −30 °C, cooling to −15 °C (rated floors; confirm per submittal sheet).

**What *we* enforce regardless (primary, in firmware — see CompressorGuard in §3):**

- Compressor minimum OFF time, minimum ON time, max starts/hour, and post-boot hold-off — with timer state persisted so a reboot can't erase a pending min-off.
- Low-ambient compressor lockout and the dual-fuel balance-point logic.
- Mutual exclusion of gas heat and compressor heat (§2), with defrost as the sole sanctioned exception.

Relying on the ODU's 3-minute delay as the anti-short-cycle mechanism is **not acceptable** — repeated trips latch faults, and the split of protection responsibility between ODU, interface board, and thermostat in communicating mode is an **open question** (verify during Phase 2; see docs/05).

### 1b. Case B (relay path): "silence = safe" does NOT transfer to relays

On the CT-485 bus, a dead controller goes silent and the equipment drops the call. **A 24 V relay output has the opposite failure polarity:** a hung ESP32 with the Y relay energized runs the compressor indefinitely — there is no refresh timer on a closed contact. Therefore, in the relay architecture (Case B, see docs/03):

- All demand relays are **normally-open, de-energized = no demand**, and must idle de-energized at boot/reset (same discipline as DE/RE).
- The **external hardware watchdog must cut the common relay-coil feed** on timeout, in addition to forcing transceiver DE off. "Watchdog forces no-demand" means *both* actions.
- 24 V sense inputs (Y/G/W/D monitoring) remain permanently installed so a stuck-closed output contact is detectable.

### 1c. UI failure isolation (single-unit wall touchscreen)

The production controller is a wall-mounted ESP32-S3 touchscreen running LVGL + Wi-Fi alongside the CT-485 stack (see docs/01/03). This does **not** weaken the safety case, provided task separation is maintained:

- An LVGL/UI task crash is a **comfort outage, not a safety event**: the high-priority safety/protocol task and the external hardware watchdog are unchanged from the headless design. The safety task pets the watchdog **only when its own invariants hold** — a wedged UI cannot keep the watchdog fed.
- A whole-chip hang → external watchdog forces no-demand exactly as in the headless design: DE held off, **and relay-coil power cut in Case B**.
- The worst credible UI-load failure is a delayed/missed demand refresh — which degrades toward *no demand*, the safe direction. (Single-unit viability is still gated on the Phase 3 TX-turnaround jitter bench test — see docs/05.)

> 🚩 **Stop condition.** If sniffing ever shows the thermostat sending a *raw valve duty* that the IFC obeys **without independent flame/limit supervision**, **stop the project** — that would be an uncertified, unsafe design out of scope for a hobby build on a gas appliance. Verify the demand semantics before trusting any decode.

## 2. Failure modes → required failsafes

All rows apply **in every mode** (heat, cool, auto, emergency heat) unless stated; "demand = 0" always means *all channels and relays*, and recovery always honors compressor minimum timers.

| Failure mode | Risk | Required failsafe |
| --- | --- | --- |
| **Wi-Fi loss** | lose HA / remote setpoint | keep last good setpoints **locally**; control keeps running on local sensors; never let "no Wi-Fi" raise *any* demand |
| **MQTT broker loss / stale setpoints** | no setpoint updates | **dual-bounded** local fallback: heat-to **18 °C** / cool-to **27 °C**, mode = last user mode (never escalate to OFF, never a bare heat-only "18 °C" — that is a continuous *cool* call in summer) |
| **Temp-sensor loss/fault** (open, short, NaN, frozen, out-of-range, divergent) | garbage control input → **runaway demand** — *most dangerous software failure*, and it applies **identically to COOL** (runaway cooling = frozen coil + flooded condensate) | **mandatory fault detection** per §4 (range + CRC + staleness + stuck-value + divergence). On quorum loss → **demand = 0 on all channels** and alarm. Recovery honors compressor min-off — a flapping sensor must not short-cycle the compressor via the failsafe path itself. |
| **ESP32 crash / hang / deadlock** (incl. UI-induced) | last demand stuck **on**; in Case B a closed Y relay runs the compressor with no timeout | **external hardware watchdog** that on timeout **forces no-demand** (holds DE off **and cuts relay-coil power**, not merely resets). Bus silence also helps: thermostat lost → equipment drops the call (verify per-channel, §6). |
| **Brownout / power sag / power-cycle mid-call** | random reset, partial state, lost compressor timers | brownout detector **enabled**; bulk cap; **boot = no demand** until mode/setpoints/sensors validated; compressor timers persisted (NVS/RTC) — if state unknown, enforce **full min-off hold-off (300 s + jitter)** before any compressor demand |
| **Reset loop** (crash/brownout cycling) | each reboot could re-start the compressor | **≥3 watchdog/brownout resets within 30 min → latched NO-DEMAND**, manual clear required |
| **Bus contention / collision** | talk over the furnace/interface board, corrupt frames | **explicit DE/RE; TX only in your slot; release bus (DE low) at idle and boot/reset**; listen-before-talk; CRC every frame; can't get clean comms → **go passive** |
| **Demand conflict (invariant violation)** | heat and cool demands simultaneously nonzero | **invariant check at the single emission point** (DemandArbiter, §3): conflict → all channels zero + alarm. Changeover requires the outgoing channel zeroed **and** its refresh timer expired, plus dwell ≥ compressor min-off. |
| **Gas heat + compressor heat simultaneous** | **prohibited by the manufacturer** (defrost excepted): the HP coil sits on the furnace supply, *downstream* of the heat exchanger — gas-heated air entering the condensing coil drives head pressure toward the high-pressure cutout (trips, compressor stress, latched lockouts) | hard mutual-exclusion interlock in the arbiter — nothing at the bus level enforces this (see docs/02); it is **ours to enforce**. Defrost tempering is the *sole* sanctioned overlap, bounded per docs/05 (fixed demand, 15 min cap). |
| **Relay stuck closed / O-B flip under load** (Case B) | compressor runs uncommanded; reversing valve switched mid-run | sense inputs detect output-vs-command mismatch → alarm + watchdog coil-cut path; **O/B changes state only with the compressor proven idle** (Gree convention: **B energized = heating** — verify polarity, §6) |
| **Indoor coil freeze** (cooling without airflow) | iced coil, liquid slugging, water damage on thaw | **never close Y / send COOL without blower confirmation** (bus blower telemetry in Path A, or sensed G/Y feedback in Path B); drop the cool call if feedback disappears; cooling locked out below **18 °C indoor**; optional supply-air DS18B20 → drop cool if supply < ~5 °C |
| **Condensate overflow** | water damage | **hardwired float switch breaks the cool call independent of software** (in series with Y / sensed and enforced upstream of any firmware path) |
| **Runaway heat / overshoot** | too hot | first line = furnace high-limit; our line = **demand clamp + anti-windup + max-demand cap** (gas shaper never dithers below the 40% modulation floor — snap to 0/min-fire with hysteresis) |
| **Stuck-on demand / logic bug** | continuous operation | **per-equipment max-runtime policy:** gas call > **4 h** without progress → drop + alarm. **Heat pump: progress alarm ONLY — never auto-cycle** (continuous near-balance-point running is *normal* for an inverter HP; droop > 2 °C for > 60 min → alarm + allow gas staging, do not bounce the compressor as a "failsafe"). |

## 3. Required mechanisms (implement all — defence in depth)

- **Independent hardware watchdog with a fail-safe output.** The ESP32's internal WDT only reboots the chip — not enough. Add an external watchdog (TPL5010 / MAX6369 class, or an ATtiny) that the ESP32 must pet. **On timeout, the external part forces the no-demand state:** holds transceiver DE off **and (Case B) cuts the common relay-coil feed** so the ESP32 can't drive the bus *or* hold a contactor in. **Default-safe state of all hardware = NO DEMAND.**
- **Heartbeat / deadman via CT-485 presence — per channel.** Rely on the equipment interpreting *loss of periodic thermostat messages as "drop the call."* **Verify this per demand channel during commissioning** (pull the bus mid-call for heat, cool, HP heat, aux — see §6 matrix). ⚠️ Open question: with the coordinator/token topology, does our silence propagate to the **non-coordinator** unit (interface board / ODU side)? Do not assume it does — test it. If loss-of-comms does **not** drop a given call, that channel needs a *hardware* means of removal — and that materially raises project risk; reconsider.
- **CompressorGuard (new, primary compressor protection).** Enforces min-OFF (300 s), min-ON (300 s), max 3 starts/hour, and post-boot hold-off. **Timer state persisted in NVS/RTC**; if persisted state is missing/invalid at boot, assume the worst and enforce the full hold-off. No code path — including failsafe recovery and mode changeover — may bypass it.
- **DemandArbiter as the single emission point.** Every demand (bus message or relay state) is emitted by exactly one module, which checks the invariants (mutual exclusion, compressor timers, lockouts, blower-proven, float switch) on **every** emission. No other module may touch the bus TX path or a relay GPIO.
- **Layered watchdogs:** internal task WDT (hung control task) + external HW WDT (dead chip) + equipment comms-loss timeout (catch-all). The safety task pets the external watchdog only when its own invariants pass (§1c).
- **Software defaults / boot validation:** boot = no demand until **mode + dual setpoints + sensors** are validated: CRC/range-validate persisted config; **cross-check the restored mode against OAT lockouts before any demand** (don't resume COOL at −10 °C outdoor); enforce the setpoint deadband invariant (**cool ≥ heat + min delta, 2.8 °C default / 1.1 °C floor**) — reject/clamp violating writes. Every control cycle re-validates *(sensors OK? setpoints fresh? comms OK? timers satisfied?)* before any nonzero demand; clamp + anti-windup; per-equipment max-runtime policy; anti-short-cycle.
- **Alarming:** publish health/last-error to MQTT **and** a local status indication (LED / on-screen banner) so a silent failure is visible.
- **Demand refresh discipline — per channel:** each active demand channel (heat, cool, fan, aux) has its own protocol refresh timer; re-issue each well within its window so a sluggish loop never drops a call mid-cycle — and so a *deliberately* stopped refresh reliably ends it. (Exact timer byte offsets are **unconfirmed until sniffed** — see docs/02.)

### 3a. OTA update policy (wall unit)

Reflashing a live HVAC controller is a deliberate, bounded risk — bounded by these rules (all mandatory):

- **OTA is accepted only at zero demand / idle.** If any channel is active, the update is deferred or the firmware first ramps to no-demand (honoring compressor min-ON) before accepting the image.
- **The external hardware watchdog stays armed throughout the update.** There is no "disable watchdog for OTA" mode; a wedged updater is treated like any other hang → forced no-demand.
- **ESPHome `safe_mode` + IDF app rollback:** the new image must confirm itself within its validation window or the bootloader rolls back to the previous app automatically.
- **Physical recovery path:** USB access at the wall mount is preserved so a bricked unit can be reflashed in place without pulling wires.
- **Why the rollback window is safe:** boot = no-demand-until-validated (§3) means every boot of an unconfirmed image starts from the safe state — an OTA gone wrong costs comfort, not safety.

## 4. Sensors (safety-relevant)

**Multi-sensor fusion happens in firmware** (not in HA templates) so the control input and per-sensor health remain visible to the safety layer and survive HA loss.

- **Indoor temperature — fused remote sensors (via HA→MQTT) = primary comfort input; local DS18B20 = always-present safety/fallback.** Never a single source.
- **Per-sensor fault policy (each remote sensor, all mandatory):** staleness timeout (default 300 s; requires the 60 s HA heartbeat republish — see docs/06; sensor topics **non-retained** so a dead broker can't replay stale temps); range gate (5–40 °C); stuck-value (zero-variance) detection; **divergence check** — a sensor > 4 °C from the participant median is excluded + alarmed. Occupancy-driven re-weighting is rate-limited (slew ~0.1 °C/min on the effective input) so a sensor joining/leaving can't step the control input.
- **Quorum loss** (no valid participants) → fall back to DS18B20. **DS18B20 also bad → demand 0 on all channels + alarm.**
- **DS18B20 as independent sanity floor:** even while fusion is healthy, the DS18B20 cross-checks the aggregate — disagreement > 5 °C → alarm + use the conservative value.
- **DS18B20 fault detection (all mandatory):** CRC check; reject the **85 °C power-on default** and **−127 °C disconnect** sentinels; range gate; staleness/stuck-value check.
- **DS18B20 degraded mode (explicit mode, not a transparent failover):** when running on the fallback sensor alone — bounded setpoints (heat-to 16–18 °C floor), **cooling disabled** (or ≥ 29 °C ceiling only), demand capped, **loud persistent alarm**. The furnace-room sensor reads plenum-adjacent air, not living space; it is a floor-keeper, not a comfort input. **Do not** place it near the plenum if avoidable (long 1-Wire run option, docs/03).
- **Outdoor temperature (new — feeds balance point, lockouts, changeover):** sourced by rung — bus sensor (if present, open question) → wired outdoor DS18B20 (north wall, shaded) → HA weather. Identical fault treatment per rung (staleness 30 min → next rung; range; stuck-value); alarm on > 5 °C disagreement between rungs. **All rungs stale → fail cold: gas heat allowed, compressor locked out** (and cooling locked out under the indoor-18 °C policy). Never run the compressor on an unknown outdoor temperature.
- **No-local-fallback build (this wall unit — issue #73):** the installed SlyTherm wall unit has **no on-board DS18B20**; the local slot (fusion slot 0) is gated OFF (`DETTSON_LOCAL_SENSOR` default off, so `-DDETTSON_DS18B20` is also not built). The three DS18B20 bullets above (fallback, sanity floor, degraded mode) therefore **do not apply** to this build: there is no local sanity-floor cross-check and no local-vs-fusion disagreement alarm. **Consequence — the safety tradeoff:** if **every** MQTT room sensor goes stale (WiFi/broker/bridge loss or all sensors dead), there is no control temperature and the system **fails to no-demand** (0 on all channels + alarm), exactly as the quorum-loss rule requires — but with **no degraded local mode to keep a heat floor**. The wall unit's temperature availability now depends entirely on the MQTT bridge + WiFi; in heating season this makes the "loss of heat is a hazard" escalating alarm (below) the sole backstop until a room sensor returns or the OEM thermostat is swapped back in. The DS18B20/local paths remain **compiled-in behind the flag** for other hardware.
- **Config validation rule (hard, new):** reject any lockout/balance-point combination that leaves **any outdoor-temperature band with no permitted heat source.** A config that can silently produce "no heat at −25 °C" is itself a fault.
- **Loss of heat is itself a hazard** (frozen pipes, habitability): sustained forced-no-demand in heating season raises an **escalating alarm** (MQTT + local). The recovery path is rolling back to the OEM thermostat (§5) — design the wiring so that swap takes minutes.

## 5. Certification, code, warranty, insurance — honestly

- **CSA** certifies gas appliances in Canada/US. The furnace is certified **as a system with an approved control.**
- The OEM **R02P030 thermostat is discontinued**, but the OEM path is not closed: **successors R02P032/R02P034 ship today**, and an **R02P034 is the certified rollback device** (and the recommended Phase 1–2 reference bus master — see docs/05). Replacing the OEM control with a homemade controller is a **modification of a certified gas appliance's control system.** Riding the bus *as a thermostat* (IFC keeps all safeties) does **not** alter the certified combustion-safety path — but it **is outside the certified configuration.** The same logic applies on the HP side: we stay on the thermostat side of the coil board / interface board; we never touch the Gree-proprietary ODU link.
- **Honest implications:** this **can void the furnace and heat-pump warranties**, may **violate local gas/electrical code** (work on gas-appliance controls is often restricted to licensed technicians), and **could affect home insurance** after any fire/CO/water incident traced to the controller.

**Conservative requirements before/while running this:**

- **Do not remove or bypass any furnace or ODU safety wiring.** Tap CT-485 / thermostat terminals only; leave R/C/1/2 semantics intact; never open the Gree H1/H2 or COND links.
- **Install a CO alarm** in the home (you should have one regardless).
- **Keep an OEM thermostat** (R02P034 communicating, or R02P033 for a conventional install) available for instant rollback to the certified config.
- **Have a licensed HVAC tech review the final install** and confirm the comms-loss-drops-call behaviour per channel.
- **Document** that the design does not alter the combustion-safety chain or the refrigerant-circuit protections.
- Treat this as **experimental, at-your-own-risk, on a life-safety appliance.** If you're not willing to own that, use a Dettson-supported thermostat (R02P034) and integrate at the HA level instead.

## 6. Commissioning safety matrix & checklist

**Core matrix — every cell must reach zero demand.** For each equipment call {**furnace heat, HP heat, HP cool, aux/backup heat**}, induce each fault {**pull bus mid-call**, **ESP32 hang (HW watchdog)**, **sensor fault/flap**, **MQTT loss**, **brownout/power-cycle mid-call**} and confirm the call drops to zero (bus silent / relays open) and recovery honors compressor timers.

| ↓ fault \ call → | Furnace heat | HP heat | HP cool | Aux/backup |
| --- | --- | --- | --- | --- |
| Pull bus mid-call | ☐ | ☐ | ☐ | ☐ |
| ESP32 hang (HW WDT) | ☐ | ☐ | ☐ | ☐ |
| Sensor fault / flap | ☐ | ☐ | ☐ | ☐ |
| MQTT loss | ☐ | ☐ | ☐ | ☐ |
| Brownout / power-cycle mid-call | ☐ | ☐ | ☐ | ☐ |

> ⚠️ The pull-bus row also answers open question §3 above: confirm silence drops the call at **both** the furnace and the HP/interface-board side (coordinator topology may not propagate it). Any cell that fails = that channel needs a hardware call-removal path before field use.

**Checklist:**

- [ ] Phase 0 inventory complete: architecture (A/B), thermostat & ODU models, wiring map documented (docs/05).
- [ ] CO alarm installed and working.
- [ ] OEM thermostat retained and rollback procedure known (and rehearsed).
- [ ] Sniff-only validated; field dictionary complete; **demand payload offsets confirmed from real captures** (docs/02).
- [ ] Confirmed (by capture) the IFC enforces flame/limit/pressure independently of the bus.
- [ ] Full §6 matrix executed — every cell reaches zero demand.
- [ ] External hardware watchdog forces NO-DEMAND on hang: DE held off **and (Case B) relay-coil feed cut** (bench-tested).
- [ ] **Mutual-exclusion invariant bench-tested:** gas heat + compressor heat simultaneously commanded → arbiter zeros all + alarms.
- [ ] **Compressor min-off enforced across reboot** (power-cycle mid-min-off; confirm hold-off resumes/restarts).
- [ ] **O/B polarity verified on the installed equipment: B = energized in heating** (Gree convention) — a wrong guess inverts heat/cool.
- [ ] **Float switch kills the cool call** with firmware unaware (hardwired path verified).
- [ ] **Blower-proven interlock verified:** cool call refused/dropped without blower confirmation.
- [ ] **Defrost observed (forced cycle) and not disrupted** — controller holds steady through the D-signal / bus defrost signature.
- [ ] **Relay coils verified de-energized at boot/reset** (Case B; scope/sense-input-verified, same as DE/RE check).
- [ ] Sensor-fault → demand-0 path bench-tested per channel (unplug DS18B20, kill MQTT, flap a remote sensor) — recovery respects min-off.
- [ ] Reset-loop lockout tested (3 forced resets → latched NO-DEMAND).
- [ ] Brownout detector enabled; boots to no-demand; restored mode cross-checked against OAT lockouts.
- [ ] Demand clamp, per-equipment max-runtime policy, anti-short-cycle, per-channel demand-refresh all in place and tested.
- [ ] DE/RE pulls to idle at boot/reset (scope-verified bus stays silent during reset).
- [ ] OTA policy verified: update refused while any demand active; watchdog armed through update; rollback exercised once on the bench.
- [ ] Licensed HVAC tech reviewed the install.
