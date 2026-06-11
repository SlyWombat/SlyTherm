# 07 — Gap Analysis: ElectricRV Dual-Fuel Smart Thermostat (DT-1) vs Ecobee Smart Thermostat Premium

**Document status:** v0.1, 2026-06-11. Compares the DT-1 **as documented in the v0.1 draft manuals** against the Ecobee Smart Thermostat Premium feature inventory (sources in the appendix).

> **Read this first:** the DT-1 is **pre-hardware**. Nothing in this document is
> field-verified. The most mature parts of the design exist as C++ libraries
> with native unit tests that have **never run on the target hardware or
> touched a live furnace bus**; the rest exists as reviewed design documents
> and draft manuals with explicit "pending verification" gates. Every status
> below uses the vocabulary in Section 2 — there is no "works" status anywhere
> in this document, by design.

---

## 1. Executive summary

The DT-1 is not an Ecobee clone and does not attempt feature parity; it is a purpose-built dual-fuel controller for one equipment family (Dettson Chinook modulating furnace + Gree-built heat pump) that delegates its smart-home surface to Home Assistant. Measured against the nine Ecobee functional areas, the DT-1's core HVAC control — dual setpoints, deadband, dual-fuel arbitration, compressor protection, sensor fusion with follow-me weighting — reaches or exceeds Ecobee's documented behavior on paper (one carve-out: no gas/aux-side minimum on/off time is documented anywhere — G14), and most of it is implemented as unit-tested (but hardware-unproven) firmware libraries. The headline advantages are structural: true 40–100 % modulating gas control via the furnace's own CT-485 bus instead of two relay stages (the DT-1 *will* modulate the furnace to the load — demand shaping is implemented and unit-tested, but the bus transmit path itself is design-only and gated on payload-offset verification, install §5.7), a physical outdoor sensor (plus bus OAT) instead of internet weather, decoded equipment fault telemetry instead of a relay-blind stat, every control parameter remotely tunable instead of device-only thresholds, and a fully local, open MQTT integration in an era when Ecobee has closed its developer API. The biggest gaps run the other way: the DT-1 has **no accessory control whatsoever** (humidifier, dehumidifier, ventilator/HRV — Ecobee has one port; we have zero), **no humidity control logic** of any kind (no overcool-to-dehumidify, no frost control, no feels-like compensation), **no Smart Recovery** (pre-heat/pre-cool to hit the schedule on time), **no built-in occupancy or air-quality sensing**, and **no on-device setup, calibration, or access lockout** — the wall settings page is informational. A second tier of gaps (schedules, vacation, maintenance reminders, presence-based home/away, energy reports) is deliberately delegated to Home Assistant, which genuinely provides local equivalents — but only if the owner builds them, so they are "Partial via HA," not parity out of the box. The DT-1 also forfeits Ecobee's certification, ENERGY STAR rating, utility-rebate/demand-response ecosystem, and polished retail UX; it is an experimental, uncertified control on a gas appliance, operated at the owner's risk. Finally, the caveat that frames everything: Ecobee's behaviors are shipping in millions of homes, while every DT-1 behavior in this document is either untested code or unverified design, with hardware-facing fundamentals (demand payload offsets, silence-drops-the-call, defrost ownership) still gated on commissioning. The honest verdict: superior architecture and control depth for this specific equipment, with real and acknowledged feature debt in comfort accessories, humidity, and out-of-box polish.

## 2. Method and status vocabulary

**Method.** Each feature row of the Ecobee inventory (`/tmp/ecobee-inventory.md`, sourced from ecobee's official support/threshold documentation plus marked community sources) is dispositioned against (a) the DT-1's documented product surface — the v0.1 draft **User Manual** (`manual/build/user-assembled.md`) and **Technical Installation Manual** (`manual/build/install-assembled.md`) — and (b) implementation reality in the repository (`lib/` modules with native tests, `docs/01-architecture.md`, `docs/05-firmware-plan.md`, `docs/06-home-assistant.md`, `lib/DettsonConfig/DettsonConfig.h`). This is a manuals-vs-Ecobee-documentation comparison; no hardware behavior on either side was tested for this analysis.

| Status | Meaning |
| --- | --- |
| **Implemented (untested)** | Code exists in `lib/` with native unit tests, but has never run on hardware |
| **Designed** | Specified in docs/manuals; no code |
| **Planned** | Explicitly deferred and tracked |
| **Partial** | Some of the Ecobee behavior covered (which part is stated) |
| **Partial via HA** | Home Assistant provides a genuine local equivalent, but it must be configured — not native to the thermostat |
| **Missing** | Absent from design and manuals |
| **Out of scope** | Deliberately excluded (rationale given) |
| **Exceeds** | Done better by design (justified) |

A row can carry a compound status (e.g. "Exceeds (gas) / Designed (HP)") where the comparison genuinely splits, and a status may carry a parenthetical **scope qualifier** naming which part of the row it applies to — e.g. "Exceeds (gas, by design)", "Missing (control) / Exceeds (potential)", "Partial (Designed)" — with the detail cell stating which part. Qualifiers scope a defined status; they never introduce a new one. **Nothing in this project is field-verified**; "Implemented (untested)" is the ceiling.

## 3. Feature matrix

### 3.1 Core HVAC control

| Ecobee feature | Ecobee behavior | DT-1 status | DT-1 detail / evidence |
| --- | --- | --- | --- |
| System modes | Heat/Cool/Auto/Off + Aux/Em heat | Implemented (untested) | OFF/HEAT/COOL/AUTO/EM HEAT; `lib/ModeStateMachine` + tests; User Manual *Choosing a mode* |
| Dual setpoints + deadband | Min delta 5 °F default, 2 °F floor; pushes other setpoint | Implemented (untested) | Identical: 2.8 °C default, 1.1 °C floor, clamp-and-push (`kMinSetpointDeltaC`, `DettsonConfig.h`; install §8.2) |
| Heat differential | 0.5 °F default; +1 °F auto-added when Away | Partial | Hysteresis 0.55 °C ≈ 1 °F implemented (`kCallHysteresisC`; deliberately wider — Ecobee's 0.5 °F short-cycles). No automatic Away widening; Away preset setpoints in HA achieve the same effect |
| Cool differential | Same logic | Partial | Same single hysteresis constant; same Away caveat |
| Staging: automatic vs manual | Toggle; firmware decides stage-up | Exceeds (gas, by design) / Designed (HP) | Gas 40–100 % modulation supersedes staging — but split the claim: demand **shaping** is Implemented (untested) (`lib/PidShaper`, `lib/DemandShaper`); bus **actuation** is Designed — the CT-485 TX path is not yet implemented and is gated on payload-offset verification (install §5.7, §12.4; `docs/05-firmware-plan.md` Phase 2 gate). HP staged/inverter shaping designed, path TBD Phase 2 (`docs/05-firmware-plan.md` §8.6 note) |
| Aux Savings Optimization | One-knob comfort/savings (0.5–2.5 °F) | Partial | No single knob; equivalent control via explicit escalation params (droop 1.0 °C / 30 min / ≥95 % HP demand → gas; install §8.3). More transparent, less approachable |
| Multi-stage compressor (manual) | Y2 by temp delta or stage-1 max runtime | Designed | Case B Y1/Y2 relays (install §6.1); HpRelayShaper duty-cycles within timers; explicit stage-2 delta/runtime params not specified |
| Reverse staging | Stage down near setpoint to finish | Out of scope (gas) / Missing (HP) | Gas: modulation makes it moot (PID ramps down naturally). HP staged case: no reverse-staging logic documented |
| Two-stage furnace deltas | Stage-2 delta, stage-1 max runtime | Out of scope | Superseded by true 40–100 % modulation — the DT-1's core advantage |
| Dual-fuel / aux logic | Aux on OAT < 35 °F (internet weather), or delta/runtime triggers; aux max-OAT lockout | Implemented (untested) + Exceeds | `lib/DualFuelArbiter` + tests: balance point −8 °C ± 2 °C, compressor lockout −20 °C, gas lockout > +10 °C, droop escalation, de-escalation, **plus** a validation rule rejecting configs that leave any OAT band with no heat source (install §8.3) — Ecobee has no such guard. OAT from physical sensors, not internet weather |
| Short-cycle protection | Compressor min on/off 300 s (240–900 s); **plus** Heat Min On Time 300 s and Aux Heat Min On Time 300 s (gas/aux side) | Implemented (untested) + Exceeds (compressor) / Missing (gas/aux min on-time) | Compressor: `lib/CompressorGuard` + tests — same defaults/ranges **plus** 3 starts/hour, NVS-persisted min-off across reboot, post-boot hold-off, reset-loop lockout (install §8.1) — Ecobee documents none of those. Gas/aux: **no minimum on/off time is documented anywhere** (`DettsonConfig.h` and install §8 contain only compressor timers); modulation and the wider 0.55 °C hysteresis mitigate short-cycling but are not a gas min-on/off parameter (see G14) |
| Fan control | Auto/On/per-comfort-setting; min fan runtime/hr (0–55 min); dissipation time | Partial (Designed) | `fan_mode` auto/on/**circulate** designed (`docs/06-home-assistant.md`; circulate = duty-cycled FAN_DEMAND). No minutes-per-hour parameter, no dissipation-time setting; blower demand mapping is pending verification (install §2.3) |
| Reversing valve O/B config | O or B energize convention, setup menu | Designed | Case B O/B relay with B=heat (Gree) convention, commissioning test E1; Case A handled on-bus (install §6.1, §10.5) |
| Sensor calibration | Temp ±10 °F, humidity ±10 % corrections | Missing / Partial via HA | No offset parameters documented anywhere. Workaround: per-sensor offsets on the HA side of the sensor bridge |
| Thermal Protect | Ignore remote sensors outside max-difference band | Implemented (untested) + Exceeds | `lib/SensorFusion` + tests: >4 °C-from-median outlier exclusion **plus** range gate, staleness, stuck-value detection, slew-limited rejoin (install §8.7) |
| Installer lockout | 4-digit code on installation settings | Missing | No access control on the wall UI or on HA-exposed config numbers |
| Thresholds device-only | Tunable only at the thermostat (a documented Ecobee weakness) | Exceeds | The weakness is inverted, not merely fixed: every parameter is an HA-editable, firmware-clamped MQTT number (install §8, §9.2). Honest flip side: DT-1 thresholds are **not** tunable on-device — the wall Settings page is informational (User Manual *The other pages*) |

### 3.2 Accessory control

| Ecobee feature | Ecobee behavior | DT-1 status | DT-1 detail / evidence |
| --- | --- | --- | --- |
| Accessory port | ONE accessory (ACC+/−) | Missing (control) / Exceeds (potential) | DT-1 has **zero accessory outputs** (install §6.1: Y1/Y2/O-B/G only, Case B). HA can control any number of *standalone* smart accessories — unlimited integration potential, but **no implemented thermostat-coordinated accessory control** |
| Humidifier (setpoint / Frost Control) | Fixed RH or OAT-computed frost-control setpoint; Window Efficiency (1–7) input shapes the frost-control curve | Missing / Partial via HA | No humidifier output or logic. A standalone smart humidifier + HA automation (using `sensor.dettson_outdoor_temp` for frost-control math, including a window-efficiency-style curve) is buildable but undocumented and uncoordinated with HVAC state |
| Dehumidifier | Fixed RH setpoint; dehumidify-with-fan | Missing / Partial via HA | Same situation as humidifier |
| AC Overcool Max | Overcool past setpoint by 0.5–5 °F to dehumidify | Missing | No RH input to control logic (an optional I2C humidity module is listed in install §12.1 but drives nothing); no overcool logic |
| Ventilator/HRV/ERV | Min runtime/hr occupied/unoccupied; Free Cooling | Missing / Partial via HA | No ventilator output. HA can run an HRV on its own schedule — and could even do CO2-driven ventilation, which Ecobee can't — but nothing is designed or documented here |

### 3.3 Sensors

| Ecobee feature | Ecobee behavior | DT-1 status | DT-1 detail / evidence |
| --- | --- | --- | --- |
| Built-in temp + humidity | On-board sensing drives control | Partial | Built-in DS18B20 is deliberately a **fallback/sanity floor only** (degraded mode 16–18 °C heat, cooling disabled; install §8.9). Humidity: optional I2C module listed (install §12.1), unused by control |
| Built-in radar occupancy | Proximity wake + occupancy for Smart Home/Away | Missing | No built-in occupancy sensor; occupancy comes only from remote sensors |
| Air quality (VOC + eCO2) | Poor-air alerts + tips | Missing / Partial via HA | Nothing on-board; any HA-known AQ sensor can alert in HA, uncoordinated with the thermostat |
| SmartSensor remotes (up to 32; temp+PIR; 5 yr battery) | RF remotes, proprietary | Partial + Exceeds | Open ecosystem instead: any Zigbee/ESPHome temp or mmWave occupancy sensor via the HA→MQTT bridge (User Manual *Supported sensors*; `docs/06-home-assistant.md`). mmWave beats PIR for still occupants; count unbounded. Ecobee's own sensors explicitly **incompatible** (proprietary 915 MHz). Cost: sensors are third-party purchases, bridge is HA config |
| Follow Me | Occupancy-dwell-weighted average, gradual | Implemented (untested) | `lib/SensorFusion` + tests: 2.0× occupied weight, 30 min window, 10–30 min weight ramp, slew-limited (install §8.7) |
| Per-comfort-setting sensor participation | Each comfort setting picks sensors | Designed | Sensor roster maps sensors to presets (`dettson/config/sensors`, install §9.4; User Manual *Per-preset rooms*) |
| Smart Home & Away | Occupancy auto-overrides schedule | Partial via HA | Presence automations ("switch to Away when both phones leave") explicitly suggested (User Manual *Schedules*); not native, must be built |
| Door/window sensors | Security + window-open HVAC alerts | Partial via HA / Out of scope (security) | Any HA contact sensor can drive a mode-off automation; no native support, and Smart-Security-style features are out of scope (HA is the security platform if wanted) |

### 3.4 Scheduling & comfort

| Ecobee feature | Ecobee behavior | DT-1 status | DT-1 detail / evidence |
| --- | --- | --- | --- |
| Comfort settings | Home/Away/Sleep + unlimited custom | Partial | Exactly three firmware presets (home/away/sleep pass-through; `docs/05-firmware-plan.md` Phase 4). Setpoint pairs live in HA. Custom presets beyond three: not in firmware enum; HA can write raw setpoints instead |
| Schedule editor | Per-day activity blocks on device/app/web | Partial via HA (by design) | The thermostat stores **no weekly schedule** — HA scheduler/automations own it; documented honestly with a starter schedule (User Manual *Schedules and presets*; install §9.5). More capable than Ecobee's editor (presence, calendars), but zero schedule without HA |
| Holds | Until next activity / 2 h / 4 h / indefinite, plus a "Decide at time of change" hold preference (inventory §4) | Partial | Manual change persists until the next scheduled change = Ecobee's default hold (User Manual *Choosing a preset*). Timed holds (2 h/4 h) and a decide-at-change prompt: only as DIY HA automations |
| Vacation mode | Date-range events with own setpoints | Missing / Partial via HA | No native vacation object; an HA calendar automation is the canonical local equivalent, but it is not documented as a recipe |
| Smart Recovery (pre-heat/pre-cool) | Learns ramp rates; hits setpoint at schedule time | Missing | No ramp-rate learning anywhere in design. Workaround is crude: schedule preset changes earlier. (Ecobee's overreach here is a community complaint — but the feature itself is genuinely expected) |
| eco+: Schedule Assistant | Recommends schedule edits from occupancy | Missing | Wontfix-class: cloud/ML feature; no local equivalent promised |
| eco+: Time of Use | Shifts run-time to off-peak rates | Partial via HA | HA energy/price integrations + automations can do this locally where tariff data is local; entirely DIY, not designed here |
| eco+: Community Energy Savings | Utility DR events, incentives | Out of scope | Requires the utility/cloud ecosystem the DT-1 deliberately rejects (local-only stance, `docs/06-home-assistant.md`) |
| eco+: Smart Home & Away | Occupancy override | Partial via HA | See §3.3 |
| eco+: Adjust for Humidity | Feels-like setpoint compensation | Missing | No humidity input to control |
| eco+ master slider | Single 1–5 savings-aggressiveness knob across eco+ features | Missing | No aggregate knob; each behavior above is dispositioned individually (explicit parameters instead of a master slider, consistent with the no-silent-learning stance) |

### 3.5 Alerts & reminders

| Ecobee feature | Ecobee behavior | DT-1 status | DT-1 detail / evidence |
| --- | --- | --- | --- |
| Maintenance reminders | Filter/service/UV intervals; screen+app+email | Missing / Partial via HA | No reminder system in firmware or manuals. Date/runtime-based HA automations are trivial and could key off real runtime telemetry; not provided |
| Low/High temp alerts | 35–68 °F low, 60–104 °F high | Partial + Partial via HA | Firmware: escalating "no heat — safe state" alarm, fallback floors (18 °C / 16–18 °C degraded) protect against freeze (User Manual alert table). No user-settable alert thresholds; HA can alert on any temperature trivially |
| Low/High humidity alerts | 5–95 % RH | Missing / Partial via HA | No native RH alerting; HA + any humidity sensor |
| Aux heat runtime alerts | "Aux running excessively" | Partial — Implemented (untested) analog | Analog, not the literal feature: gas: 4 h no-progress → drop + alarm (`kGasMaxRuntimeS`); HP: progress alarm, never auto-cycled (install §8.3–8.4). Different framing, same intent |
| Air quality alerts | VOC/eCO2 warnings (Premium) | Missing / Partial via HA | No AQ sensing; third-party sensor + HA |
| Smoke-alarm sound + freeze detection | Subscription-tied extras | Out of scope | No microphone; security features deliberately not this product's job. Freeze protection itself: covered by fallback floors above |
| Short-cycling / equipment-failure diagnostics | **Absent on Ecobee** (weakness #10) | Exceeds (by design) | Decoded furnace fault codes on screen + `sensor.dettson_fault`; Case B commanded-vs-sensed terminal mismatch detection; health binary_sensor; changeover-reason telemetry (install §6.4, §9.2). Fault-code decode wording pending on-site verification (User Manual flags it "⚠ To be confirmed during installation") |
| Alert suppression (screen vs app/email) | Configurable | Partial | Alerts surface on screen + HA notifications (User Manual *Alerts*); per-channel suppression not specified |

### 3.6 App & connectivity

| Ecobee feature | Ecobee behavior | DT-1 status | DT-1 detail / evidence |
| --- | --- | --- | --- |
| Mobile app | Full control, schedules, reports | Partial via HA (by design) | HA Companion app against `climate.dettson_hvac` — full card, history, notifications, fully local (User Manual *Mobile app*; `lib/HaMqtt` Implemented (untested)). EM HEAT not exposed in app mode list (wall-only; see G15) |
| Web portal | ecobee.com control | Partial via HA | HA web UI locally; plus a local diagnostic web page on the device as broker-down fallback (install §9.6) |
| Energy reports (Home IQ) | Runtime history, savings baseline, monthly email | Partial via HA | HA history/long-term statistics + energy dashboard; richer raw telemetry than Ecobee exposes (live modulation %, active equipment, changeover reasons) but no curated "report" product |
| Weather feed | Internet weather drives display + lockouts (no physical outdoor input — weakness #5) | Implemented (untested) + Exceeds | `lib/OutdoorTempSource` + tests: bus sensor → wired outdoor DS18B20 → HA weather rungs, staleness fallback, cross-rung disagreement alarm, fail-cold policy (install §8.8). Bus OAT rung pending Phase-2 confirmation — outdoor temp may not exist on the bus at all (`docs/05-firmware-plan.md` Phase 2 done-criterion (b)), leaving wired sensor → HA weather; the Exceeds claim stands on the wired DS18B20 alone. Works offline; Ecobee's lockouts don't |
| Wi-Fi | Dual-band 2.4/5 GHz + BT 5.0 | Partial | 2.4 GHz only (install §12.1). BT 5.0: DT-1 has provisioning-only BLE (install §9.1) — not a user-facing feature on either side. Low impact |
| Integrations | HomeKit (local path), Alexa, Google, SmartThings, IFTTT; no Matter | Partial via HA | HA bridges to HomeKit/Alexa/Google/Matter if the owner configures them; nothing direct on the device |
| Developer API | **Closed since Mar 2024** (weakness #1) | Exceeds | Fully documented open MQTT topic map (install §9.3); no account, no key, no cloud to revoke |

### 3.7 UI/UX

| Ecobee feature | Ecobee behavior | DT-1 status | DT-1 detail / evidence |
| --- | --- | --- | --- |
| Display | 540×540 colour touch, ~4", zinc body | Partial | 4.3" 480×272 IPS touch (User Manual *Specifications*); functional, lower-spec; UI model `lib/UiModel` Implemented (untested), LVGL screens Designed |
| Proximity wake / ambient dimming | Radar wake, light-sensor dimming | Missing | Not in design |
| Everything on-device | Full setup + thresholds on the thermostat | Partial | Inverted asymmetry: DT-1 wall UI covers setpoints/mode/presets/diagnostics; **tuning and schedules are HA-only** (User Manual *Settings*) — the opposite of Ecobee's device-only thresholds |
| Languages | EN/FR/ES on-device | Missing | English only; no localization documented |
| Accessibility / voice substitute | Large-font UI; Siri/Alexa on-device | Missing / Partial via HA | No formal accessibility mode; HA Assist/voice via other devices only |

### 3.8 Power / installation

| Ecobee feature | Ecobee behavior | DT-1 status | DT-1 detail / evidence |
| --- | --- | --- | --- |
| 24 VAC, C-wire or PEK | PEK rescues 4-wire installs | Partial | 24 VAC R+C required; no PEK equivalent, plus a mandatory isolated supply + fuse + MOV chain (install §4) — heavier install, justified by the half-wave-rectify hazard |
| Equipment capacity | 2H/2C conv; 4H/2C HP; generic | Out of scope | Purpose-built for one family: Dettson Chinook + Gree-built HP, Case A (bus) or Case B (relay) (install §2.2; User Manual *Compatible equipment*). Deliberate: depth over breadth |
| No comm-bus / OEM protocol support | Relay-only stat | Exceeds | CT-485 ClimateTalk is the entire point: modulating demand, status, faults over the OEM bus (`docs/02-protocol-climatetalk.md`; install §5) — pending payload-offset verification |
| Guided setup + installer code | Wire detection, installer code 3262 [community-sourced: hometrix] | Missing | No guided setup; commissioning is a rigorous manual procedure (install §10) — stronger for safety, far less consumer-friendly; no access code |

### 3.9 Other marketed features

| Ecobee feature | Ecobee behavior | DT-1 status | DT-1 detail / evidence |
| --- | --- | --- | --- |
| Built-in voice (Siri/Alexa, intercom) | Mic + speaker on device | Out of scope | No mic/speaker hardware; against the privacy posture; HA voice elsewhere if wanted |
| Smart Security platform | Sensors, smoke detection, subscription | Out of scope | HA is the home-automation/security hub in this architecture; the thermostat refuses hub duty |
| ENERGY STAR / utility rebates / DR ecosystem | Certified; rebates up to ~$125/yr | Missing | Structural: uncertified experimental product (install §12.3) — no listing, no rebates, no DR enrollment. A real cost-of-ownership disadvantage that engineering cannot close |

## 4. Gap register (severity-ranked)

Severity: **High** = an Ecobee owner expects it and it is absent; **Medium** = valued, but a workable HA path exists; **Low** = niche or cloud-bound. (G14–G15 were added in review and are appended rather than re-ranked.)

| # | Gap | Severity | Layer to close | Recommendation |
| --- | --- | --- | --- | --- |
| G1 | **Accessory + humidity control** (humidifier, dehumidifier, ventilator, AC Overcool, frost control) | **High** — climate owners in cold/humid regions actively use these | Hardware (no output exists) + firmware logic; partial mitigation via HA-coordinated standalone devices | **Backlog**: (a) ship a documented HA blueprint coordinating a standalone smart (de)humidifier/HRV with DT-1 telemetry — turns "Missing" into "Partial via HA"; (b) evaluate one spare relay/output for an accessory in the Case-B hardware rev. Native multi-accessory control: wontfix for v1 |
| G2 | **Smart Recovery / pre-heat & pre-cool** | **High** — "wake to a warm house" is table-stakes thermostat behavior | Firmware (ramp-rate estimate) or HA (predictive automation); HA workaround today = earlier schedule blocks | **Backlog** (firmware preferred — the modulation PID already implies a plant model). Document the earlier-schedule workaround now |
| G3 | **Out-of-box scheduling/vacation/reminders** (everything is "bring your own HA config") | Medium — HA covers schedules, vacations (calendar), maintenance reminders, temp/RH alerts locally and well, but nothing ships pre-built | HA config (blueprints/packages) | **Close now**: ship a curated HA package — starter schedule, vacation-calendar automation, filter-reminder, low/high temp + RH alerts, presence-based home/away. Cheap, converts five "Partial via HA" rows into product features |
| G4 | **Custom presets beyond home/away/sleep + timed holds** | Medium — Ecobee allows unlimited comfort settings and 2 h/4 h holds | Firmware (preset enum + hold timer) — small; HA can fake both | **Close now** (firmware): make preset list config-driven; add optional hold-until/hold-for semantics or document the HA pattern |
| G5 | **Fan min runtime per hour + dissipation time** | Medium — heavily used Ecobee circulation feature; we have only a `circulate` mode with no duty parameter | Firmware (duty-cycle parameter on FAN_DEMAND; gated on Phase-2 blower-mapping verification) | **Backlog** (blocked on pending verification of the blower demand path) |
| G6 | **Sensor calibration offsets** (temp/RH) | Medium — expected installer tool; HA-side offsets are a workaround | Firmware (per-sensor + DS18B20 offset numbers) | **Close now** — trivial addition to the existing clamped-number pattern |
| G7 | **Built-in occupancy sensor** | Medium — DT-1 needs third-party mmWave purchases for follow-me/away | Hardware | Backlog (hardware rev decision); meanwhile the manual already names ~$30 sensors |
| G8 | **Installer/settings lockout** | Medium — config is wide open on HA and physical access | Firmware/UI (PIN on wall settings; optional read-only MQTT mode) | **Backlog** |
| G9 | **On-device tuning** (wall Settings page is informational) | Medium — inverse of Ecobee's device-only weakness; a dead HA leaves parameters frozen (control itself continues) | Firmware/UI | Backlog; acceptable v1 posture since fallback behavior is autonomous |
| G10 | **Away auto-differential widening** | Low — Away preset setpoints achieve the same energy outcome | Firmware | **Wontfix** (redundant with preset design) |
| G11 | **Air quality (VOC/eCO2) + alerts** | Low — Premium-only nicety; any HA AQ sensor covers monitoring | Hardware | **Wontfix native**; mention HA path in docs |
| G12 | **Proximity wake / auto-dimming, display polish, languages, accessibility, per-channel alert suppression, 5 GHz** | Low | Hardware/UI/firmware | Wontfix for v1 (languages and a formal accessibility mode — large-font UI, voice substitute (§3.7 Missing): backlog if audience warrants; per-channel alert routing (§3.5 Partial) is cheap on the HA side and should ride along with G3's notification package) |
| G13 | **Voice, Smart Security, eco+ cloud suite (Schedule Assistant, Community Energy Savings), rebates/ENERGY STAR** | Low (cloud-bound or out-of-scope by philosophy) | — | **Wontfix with rationale**: cloud-dependent, subscription, or certification-ecosystem features structurally incompatible with an open, local, uncertified experimental controller. Do **not** market HA stand-ins for Schedule Assistant or utility DR — no genuine local equivalent exists |
| G14 | **Gas/aux minimum on/off time** (Ecobee: Heat/Aux Heat Min On Time 300 s; DT-1 documents only compressor timers) | Medium — modulation + 0.55 °C hysteresis mitigate, but the gas half of Ecobee's short-cycle protection is undispositioned without it | Firmware (config pair) | **Close now**: add `kGasMinOnS`/`kGasMinOffS` following the existing clamped-number pattern (install §8 / `DettsonConfig.h`) |
| G15 | **EM HEAT not selectable from the phone app** (wall-only, §3.6 mobile-app row) | Low/Medium — a real Ecobee-owner expectation when the HP fails while away | Firmware (`lib/HaMqtt` mode list) | **Close now** candidate: likely a small HaMqtt mode-list change; confirm the safety rationale (if any) for wall-only before exposing |

Where HA already provides the equivalent locally for free — schedules, history/energy dashboards, vacation via calendar, presence-based away, threshold alerts, maintenance reminders — those rows are **Partial via HA**, not Missing; G3 is the cheap, high-leverage move that makes that real for a non-expert owner.

## 5. Where the DT-1 exceeds Ecobee

Each claim maps to a documented Ecobee weakness (inventory "Known Weaknesses" list, numbered), except item 8, which covers DT-1 capabilities with no Ecobee equivalent anywhere in the inventory:

1. **Local-only, open integration vs the API shutdown (W1, W2, W11).** Ecobee closed new developer/API access in March 2024, stranding new HA and beestat users on the HomeKit side-door; its weather, alerts, and reports die with its cloud. The DT-1's entire integration surface is a documented local MQTT topic map with HA discovery — no account, no key, nothing revocable (install §9; `docs/06-home-assistant.md`). HA's local Matter bridging also covers Ecobee's no-Matter weakness (W2, §3.6 Integrations row).
2. **Physical outdoor temperature vs internet weather (W5).** Ecobee's compressor/aux lockouts and free cooling ride on an internet feed that misses microclimates and fails offline. The DT-1 uses a rung ladder — equipment bus OAT → wired shaded DS18B20 → HA weather — with staleness fallback, disagreement alarms, and a fail-cold policy (gas allowed, compressor locked out) when all rungs die (install §8.8). *Status: Implemented (untested).*
3. **Modulating 40–100 % gas vs two relay stages, and transparent dual-fuel vs opaque "auto" staging (W4, W6).** Ecobee's aux-overuse problem is infamous enough to have canonical community fix-it guides, and the thresholds are device-only. The DT-1 will modulate the furnace to the load over its own bus — demand shaping is Implemented (untested), but the bus transmit path is Designed only, gated on payload-offset verification (install §5.7) — and every dual-fuel parameter (balance point, lockouts, escalation droop/time/threshold) is named, documented, range-clamped, and remotely editable — plus a config validator that rejects any setting that leaves an outdoor band with no permitted heat source (install §8.3).
4. **Equipment fault telemetry vs a relay-blind stat (W10).** Ecobee cannot see why equipment didn't run. The DT-1 decodes furnace fault codes to the screen and HA, publishes modulation, blower state, changeover reasons, and (Case B) detects commanded-vs-actual terminal mismatches (install §6.4, §9.2).
5. **Deterministic behavior vs eco+ overreach (W8).** Ecobee users disable eco+ wholesale because equipment runs at unexpected times. Every DT-1 behavior is an explicit, inspectable parameter; nothing learns silently. (The flip side is honest: no learning features at all — see G2.)
6. **Unlimited sensor/accessory *integration* vs one ACC port and proprietary sensors (W3, plus the ventilator-coarseness complaint W7).** Any Zigbee/ESPHome temperature, mmWave occupancy, humidity, AQ, or contact sensor HA can read can inform the system, with no port or count limit — and HA could even do CO2-driven ventilation Ecobee can't. **Careful distinction:** this is monitoring/integration *potential*; implemented accessory **control** is zero (gap G1). We exceed on the input side, and currently lose on the output side.
7. **Engineered fail-safe chain.** Hardware watchdog forcing bus silence and relay-coil cut, boot-silent transceiver, persisted compressor timers, reset-loop lockout, fault-injection commissioning matrix (install §7, §10) — none of which Ecobee documents. Counterweight: Ecobee is a certified, mass-produced product; the DT-1 is uncertified and unproven (install §12.3). This row is an architecture claim, not a track-record claim.
8. **Heat-pump and cooling equipment protection Ecobee doesn't attempt (design-level claims, install-manual citations).** The Ecobee inventory has no defrost handling at all; the DT-1 designs defrost detection with coordinated furnace tempering and an explicit "Defrosting" state on the wall screen and in HA (`hvac_action: defrosting`; User Manual *About defrost*). Plus: a blower-proven interlock — cooling is refused without blower confirmation (install §7.3, commissioning test E3); a mandatory hardwired condensate float switch (install §6.5) and an 18 °C indoor cooling lockout; and an optional supply-air coil-freeze guard (install §3.5). All Designed, none field-verified — but none has any Ecobee equivalent in the inventory.

## 6. Recommended backlog additions

Issue-ready items for "close now" and "backlog" gaps:

1. **HA starter package: schedules, vacation, reminders, alerts, presence-away** *(G3, close now)*
   Ship a documented HA package/blueprints: starter weekly schedule, vacation via local calendar, furnace-filter reminder keyed to real runtime, low/high temp + humidity alerts, and phone-presence → Away. Converts five "Partial via HA" matrix rows into out-of-box features with zero firmware work.

2. **Firmware: config-driven preset list + timed holds** *(G4, close now)*
   Replace the fixed home/away/sleep enum with a config-defined preset roster (roster topic already exists for sensors), and add optional hold-for-duration semantics so a manual change can expire Ecobee-style. Small change in ModeStateMachine/HaMqtt.

3. **Firmware: per-sensor and fallback-sensor calibration offsets** *(G6, close now)*
   Add clamped offset numbers (±5 °C class) per remote sensor and for the DS18B20, following the existing HA-editable number pattern in install §8 / `DettsonConfig.h`.

4. **Firmware: smart-recovery (pre-heat/pre-cool) ramp estimation** *(G2, backlog)*
   Estimate degrees-per-hour ramp from observed runs and start calls early so the setpoint is met at the scheduled preset change; toggleable, bounded look-ahead. Coordinate with HA-owned schedules (HA publishes the next setpoint/time, firmware decides the early start).

5. **HA blueprint: coordinated standalone humidifier/dehumidifier/HRV** *(G1 step 1, backlog)*
   Automation template driving a standalone smart accessory from DT-1 telemetry: RH setpoint, frost-control curve from `sensor.dettson_outdoor_temp`, interlock with `active_equipment` (e.g. humidify only with blower proven). Documents the "unlimited accessories via HA" claim honestly.

6. **Hardware rev: one spare accessory output** *(G1 step 2, backlog)*
   Evaluate adding one watchdog-protected dry contact to the Case-B relay stage (and a bus-equivalent for Case A if one exists) for a single thermostat-coordinated accessory — matches Ecobee's one-ACC capability natively.

7. **Firmware: fan circulate duty parameter + dissipation time** *(G5, backlog — blocked)*
   Add minutes-per-hour circulation and post-call fan dissipation parameters on the FAN_DEMAND path. Blocked on Phase-2 verification of the blower demand mapping (install §2.3 pending-verification).

8. **UI/firmware: settings lockout (PIN)** *(G8, backlog)*
   PIN-gate the wall Settings page and add an optional read-only mode for the HA config numbers, so casual or unauthorized changes to protection parameters are prevented.

9. **Firmware: gas-side minimum on/off timers + EM HEAT in the app mode list** *(G14 + G15, close now)*
   Add `kGasMinOnS`/`kGasMinOffS` clamped numbers following the existing pattern in install §8 / `DettsonConfig.h` (closes the gas half of Ecobee's short-cycle protection), and expose EM HEAT in the `lib/HaMqtt` climate mode list (confirm any safety rationale for wall-only first).

## 7. Sources appendix

**Ecobee baseline** — `/tmp/ecobee-inventory.md`, drawing principally on:
- [Smart Thermostat Premium product page](https://www.ecobee.com/en-us/smart-thermostats/smart-thermostat-premium/)
- [Threshold settings guide (official article)](https://support.ecobee.com/s/articles/Threshold-settings-for-ecobee-thermostats) / [PDF mirror](https://tssassociatesinc.com/wp-content/uploads/2024/01/ecobee-Threshold-Settings-Guide.pdf)
- [Configuring Accessories](https://support.ecobee.com/s/articles/Configuring-Accessories-with-ecobee-thermostats), [one-accessory limitation](https://support.ecobee.com/s/articles/two-accessories-smart-thermostat-premium), [AC Overcool Max](https://support.ecobee.com/s/articles/How-to-use-AC-Overcool-Max-to-reduce-humidity), [Free Cooling](https://support.ecobee.com/s/articles/What-is-Free-Cooling-and-how-can-I-use-it)
- [Room Sensors FAQ](https://support.ecobee.com/s/articles/Room-Sensors-FAQs-Setup-Guide-and-Troubleshooting), [SmartSensor](https://www.ecobee.com/en-us/sensors/smart-temperature-occupancy-sensor/), [eco+](https://www.ecobee.com/en-us/eco-plus/), [Smart Recovery](https://support.ecobee.com/s/articles/What-is-Smart-Recovery-and-how-does-it-work), [Reminders & Alerts](https://support.ecobee.com/s/articles/How-to-set-Reminders-Alerts-on-your-ecobee-thermostat)
- [Developer API closure](https://www.ecobee.com/en-us/developers/), [HA core issue #131789](https://github.com/home-assistant/core/issues/131789)
- Community-sourced items as flagged in the inventory (hometrix thresholds, aphyr aux-heat tuning, HA ventilator thread, Tom's Guide Matter comparison).

**DT-1 product surface** —
- User Manual v0.1 (`manual/build/user-assembled.md`): *Welcome / What makes it different*, *Everyday use*, *Schedules and presets*, *Remote room sensors*, *Mobile app and remote control*, *Alerts and troubleshooting*, *Specifications*.
- Technical Installation Manual v0.1 (`manual/build/install-assembled.md`): §2 (cases A/B), §6 (relay/sense), §7 (safety chain), §8 (parameter reference), §9 (HA integration), §10 (commissioning), §12 (compliance, open items).

**Implementation reality** —
- `docs/01-architecture.md`, `docs/05-firmware-plan.md` (phases, canonical defaults), `docs/06-home-assistant.md` (entities, topics, sensor bridge).
- `lib/DettsonConfig/DettsonConfig.h` (canonical defaults in code).
- `lib/` modules with native tests under `test/`: CompressorGuard, Ct485Frame, Ct485Parser, DemandArbiter, DemandShaper, DualFuelArbiter, HaMqtt, ModeStateMachine, OutdoorTempSource, PidShaper, SensorFusion, UiModel — the basis for every "Implemented (untested)" status.
