# Competitive Feature Matrix — Notes, Sources & Roadmap Recommendations

Companion to `competitive-feature-matrix.csv`. Researched 2026-07-08 via multi-source
web research (adversarially verified where marked) plus a full code/docs inventory of
the SlyTherm tree at v0.5.1.

## Legend

| Mark | Meaning |
|---|---|
| **Yes** | Shipping / supported |
| **Yes\*** | SlyTherm only: implemented and host-tested in code, but the system ships **demands-disabled** (RS-485 listen-only) until live-furnace bring-up (Phase 3). Do not market as field-proven. |
| **Partial** | Limited or indirect support (see Notes column) |
| **No** | Not supported |
| **—** | Unverified / unknown (not confirmed against primary sources) |
| **N/A** | Not applicable to that product's architecture |
| Vnext **Roadmap** | Already documented in SlyTherm plans (docs/07, /10, /11, /12, /13, open issues) |
| Vnext **Proposed** | NEW recommendation from this competitive analysis — not yet in any plan doc |

## Honesty caveats (read before quoting externally)

- Nothing in SlyTherm is field-verified yet; the control pipeline runs `LoggingActuator`
  and the CT-485 TX path is gated off. "Yes\*" rows are real code with Unity tests, not
  live furnace control.
- Energy category is almost entirely Vnext for SlyTherm — docs/13 is explicitly a
  research doc ("nothing here is implemented yet").
- Humidity/HRV/accessory control is HA-blueprints-only in V1.
- Competitor cells marked — were not confirmed against primary sources; two claims were
  outright **refuted** during verification and excluded: (a) Honeywell T9 in-box
  adapter/sensor/pricing details from CR, (b) "Sensi Touch 2 is 2.4 GHz-only with
  Bluetooth QR setup."
- Wirecutter and CNET block automated fetching; their picks/criteria rest on secondary
  sources.

## Key verified sources

- Consumer Reports "Best Smart Thermostats of the Year" (updated Jan 2026) — picks:
  ecobee Enhanced, ecobee Premium, Nest Learning 4th gen, Honeywell T9. CR scores on:
  app features/voice, **automation & geofencing**, WiFi setup, manual usability,
  install ease.
- Tom's Guide (updated ~June 2026) — best overall: **Honeywell Home X8S** ($220, 5"
  screen, doubles as smart display w/ doorbell view); budget: ecobee Essential; Google
  homes: Nest Learning 4th gen.
- Wirecutter top pick (via secondary confirmation): ecobee Smart Thermostat Premium,
  held since 2017.
- Primary docs: Google Nest 4th-gen Pro Install Guide; ecobee inter-compatibility
  guide; Honeywell/Resideo T10 product data 33-00462 & X8S install/user guide; Copeland
  Sensi Touch 2 installation instructions; Carrier SYSTXCCITC-06PD & Côr TP-WEM-03PD
  product data.

## The positioning story the research confirms

**Carrier proves SlyTherm's premise at industrial scale.** Carrier splits its market
exactly like Dettson: a proprietary RS-485 4-wire bus control (Infinity, ~$914,
dealer-install-only, no HomeKit, no official local API) is the *only* way to unlock
modulation/zoning/IAQ integration, while the open 24VAC retail tier caps at 2-stage.
The community's answer (the Infinitude reverse-engineered proxy) is the direct analog
of SlyTherm's CT-485 work. SlyTherm's pitch: **flagship-communicating-system control
(modulation, defrost tempering, bus diagnostics) with the openness of a retail
thermostat — local, no account, no dealer.**

Features **nobody** on the market has, already designed in SlyTherm docs:
1. Economic (price+COP-aware) dual-fuel switchover — Nest's Heat Pump Balance is the
   closest thing and it is *disabled on dual-fuel systems*.
2. Cheapest-fuel smart recovery (pre-heat on the HP ramp, gas as fallback).
3. Self-learned COP curves from real runtime telemetry.
4. Solar/battery-aware preconditioning.
5. Sensor-health-gated multi-room fusion (stale/stuck/outlier rejection).
6. Signed A/B OTA with idle-gated apply + auto-rollback, crash-loop safe mode —
   engineered-for-a-furnace reliability story no consumer thermostat documents.
7. Fully local, no-vendor-account operation with native Home Assistant discovery.

## Proposed roadmap additions (beyond documented plans), ranked

| # | Proposal | Why (competitive evidence) | Effort guess |
|---|---|---|---|
| P1 | **Matter support** | One implementation unlocks Apple Home + Alexa + Google Home + SmartThings simultaneously — closes SlyTherm's four biggest ecosystem gaps at once. Nest 4th gen and X8S already lead here; X8S shows even a *minimal* Matter attribute set (temp/mode) earns the checkbox. | Medium-high (esp-matter on ESP32-S3/P4) |
| P2 | **Energy history + per-fuel cost reporting** | Every single competitor has usage reports; SlyTherm has only a 12h trend. But SlyTherm's data is *richer* (modulation %, fuel split, OAT) — a "what did heat cost this month, per fuel" report would beat every incumbent, and it feeds the documented economic-switchover work. | Medium |
| P3 | **Native alerts: filter reminders + extreme-temp/freeze alerts** | Table stakes — all competitors have them; ecobee and Sensi lead marketing with burst-pipe protection. SlyTherm has the alarm registry already; runtime-based filter reminders are cheap on top of existing blower-runtime data. | Low |
| P4 | **Learning / auto-schedule** | CR weights automation most heavily and Nest "aces" it; nobody else really has it. SlyTherm already has occupancy fusion + recovery-rate learning as building blocks. Even an ecobee-style "suggested schedule" earns the checkbox. | Medium-high |
| P5 | **First-class geofencing** | On every reviewer's criteria list; SlyTherm's HA-presence path works but isn't marketable as "geofencing." Could ship as a documented HA-companion-app recipe (low) or in-app (high). | Low-medium |
| P6 | **Turnkey remote access (no-cloud-optional)** | All competitors have app-anywhere access; SlyTherm's WireGuard/Tailscale pattern is power-user-only. A guided Tailscale setup keeps the "no vendor cloud" story intact. | Low-medium |
| P7 | **Utility demand response / ENERGY STAR path** | Gates utility rebates ($75-125/yr programs at every competitor). Long-lead; matters at commercialization, not v1. | High (certification/program work) |
| P8 | **Doorbell/camera view on the P4 remote** | X8S's headline differentiator (Tom's Guide best-overall driver). The P4 remote has the compute + MIPI-DSI display to match it via go2rtc/WebRTC from HA. Halo feature, not core. | Medium |

Suggested sequencing: **P3 → P2 → P5/P6 → P1 → P4**, with P7/P8 parked for
commercialization. P3+P2 close the embarrassing gaps cheaply; P1 flips four ecosystem
rows in one project; P4 attacks the one place Nest is still clearly ahead.

## Column caveats per product

- **ecobee Premium/Enhanced** — HA integration is cloud-polling and ecobee closed new
  developer-API registrations in late 2024 (existing keys only; HA 2026.3 reportedly
  removed the key requirement — verify before citing). Siri built-in needs a HomePod.
- **Nest Learning 4th gen** — All non-Google ecosystems via Matter only. Heat Pump
  Balance unavailable on dual fuel. "<1% need a C-wire" is Google's own number.
- **Nest Thermostat (2020)** — No remote-sensor support, no auto-schedule; got Matter
  via 2023 firmware update.
- **Honeywell T9** — Being superseded by the X-series (shown sold-out on
  honeywellhome.com). Not dual-fuel rated.
- **Honeywell T10/T10+** — Pro channel; stages beyond 3H/2C require the EIM. The 2-1
  verification split on staging means double-check before quoting exact stage counts.
- **Honeywell X8S** — Matter is minimal (temp/mode; no IAQ, no schedules); requires
  First Alert account. Official IUG says 3H/2C HP (a retail listing claiming 4H/2C
  conflicts — trust the IUG). Dual-fuel support unverified.
- **Sensi Touch 2** — C-wire required; up to 4 total heat stages on HP with aux;
  loss-of-heat/cool Smart Alerts are its diagnostics story.
- **Amazon Smart Thermostat** — Alexa-only ecosystem, no dual fuel, no accessories;
  stage support ambiguous in Amazon's own docs.
- **Carrier Infinity** — Same platform as Bryant Evolution Connex. Dealer-locked;
  Greenspeed modulation requires it.
- **Carrier Côr** — Legacy; superseded in Carrier's lineup by "ecobee for Carrier" and
  the Carrier Smart Thermostat.
