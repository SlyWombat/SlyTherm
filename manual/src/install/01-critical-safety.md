# 1. Critical Safety Information

> **WARNING — This product connects to the control system of a gas appliance
> and a heat-pump compressor. Read this section in full before energizing
> anything.**

## 1.1 What this controller does — and does not — control

The SlyTherm **does not command the gas valve.** It sends a ClimateTalk (CT-485)
**heat demand** — a capacity *request* — over the furnace's RS-485 bus. The
furnace's certified **Integrated Furnace Control (IFC)** is the only device
that opens or modulates the gas valve, and the IFC independently enforces
every combustion safety regardless of anything received on the bus:

- Flame sensing and flame-failure lockout (no proven flame → gas valve
  closes, lockout).
- Primary high-limit (overtemperature → shutdown).
- Rollout switch and pressure/vent (draft) switch proving.
- Ignition-trial limits, retries, and hard lockout.
- Internal modulation limits — the furnace never fires above its rated input
  regardless of the demand it receives.

These interlocks are hard-wired or firmware-locked inside the certified IFC
and are **not on the RS-485 bus**. They cannot be disabled over CT-485, and a
missing or malformed demand message does **not** open the gas valve.

The same boundary applies on the heat-pump side: the controller requests
capacity; the Gree-built outdoor unit's own controls execute it. Note,
however, that the outdoor unit's protections (pressure trips, restart delay,
fault latching) are **equipment protections, not a certified life-safety
chain** — the SlyTherm's own compressor timers and lockouts are the primary
protection layer (see Section 7).

## 1.2 Who may perform this work

- Work on gas-appliance controls is **restricted to licensed technicians in
  many jurisdictions**. Confirm local requirements before starting.
- Wiring at the furnace terminal strip, the cased-coil board, and the outdoor
  unit must be performed by, or under the supervision of, a **licensed HVAC
  technician**.
- The network/Home Assistant integration portion (Sections 8–9) may be
  performed by a competent systems integrator.
- A **licensed HVAC technician must review the completed installation** and
  witness the commissioning tests in Section 10 — in particular the
  per-channel loss-of-communications tests — before the system is left in
  service.

## 1.3 Certification, warranty, and insurance — stated honestly

- The furnace is certified (CSA, for Canada/US gas appliances) **as a system
  with an approved control**. The OEM communicating thermostat family
  (R02P032/R02P034; the original R02P030 is discontinued) is part of that
  certified configuration.
- Installing the SlyTherm in place of the OEM thermostat is a **modification of a
  certified gas appliance's control system**. Riding the bus *as a
  thermostat* — with the IFC keeping all combustion safeties — does not alter
  the certified combustion-safety path, but it **is outside the certified
  configuration**. The same logic applies on the heat-pump side: the SlyTherm
  stays on the thermostat side of the coil board / interface board and never
  touches the Gree-proprietary outdoor-unit link.
- **Honest implications:** this installation **can void the furnace and
  heat-pump warranties**, may **violate local gas or electrical code** (work
  on gas-appliance controls is often restricted to licensed technicians), and
  **could affect home insurance** after any fire, carbon-monoxide, or water
  incident traced to the controller.
- This is an **experimental control on a life-safety appliance, operated at
  the owner's risk**. If the owner is not willing to accept that, install a
  Dettson-supported thermostat (R02P034) and integrate at the Home Assistant
  level instead.

## 1.4 Mandatory conditions of installation

All of the following are required, not recommended:

1. **Do not remove or bypass any furnace or outdoor-unit safety wiring.** Tap
   the CT-485 bus / thermostat terminals only; leave the R/C/1/2 terminal
   semantics intact; never open the Gree H1/H2 or COND links.
2. **Install a carbon-monoxide (CO) alarm** in the dwelling and verify it
   works. This is a precondition of running the SlyTherm, not an option.
3. **Retain an OEM thermostat for rollback.** Keep a Dettson R02P034
   (communicating) — or an R02P033 for a conventional installation — on site
   so the system can be returned to its certified configuration in minutes
   (Section 11). Design the wiring so that swap takes minutes.
4. **Have a licensed HVAC technician review the final installation** and
   confirm, per demand channel, that loss of communications drops the
   equipment call (Section 10).
5. **Document** that the installation does not alter the combustion-safety
   chain or the refrigerant-circuit protections.

## 1.5 Specific wiring hazards

> **WARNING — V/W2 terminal.** The Chinook's V/W2 terminal accepts a 24 V
> *modulating signal* only. **Never apply raw 24 VAC to V/W2** — this is a
> documented damage hazard. Take care during any probing or wiring not to
> short R to V/W2.

> **CAUTION — 24 VAC is AC, and furnace boards commonly half-wave-rectify
> with the C terminal shared.** Powering the SlyTherm from anything other than
> the specified **isolated** AC-input supply, or bonding the controller's DC
> ground to furnace C, can short a half-cycle of the transformer and blow the
> furnace's control fuse (Section 4).

> **WARNING — Reversing-valve polarity (relay installations).** The Gree
> convention is **B = energized in HEATING** — the opposite of the common
> "O = cool" default. A wrong polarity setting inverts heating and cooling.
> Polarity must be verified at commissioning and is only ever changed with
> the compressor idle (Sections 6 and 10).

## 1.6 Loss of heat is also a hazard

The SlyTherm's design fails toward **no demand** on any fault. The flip side is
that a sustained fault in heating season means **no heat** — itself a hazard
(frozen pipes, habitability). The controller raises an escalating alarm
(on-screen and via Home Assistant) on sustained forced-no-demand; the
recovery path is rollback to the OEM thermostat (Section 11). Brief the
homeowner on both the alarm and the rollback procedure.

## 1.7 Stop conditions

Stop the installation and do not proceed if:

- Equipment bus captures ever show the thermostat sending a **raw valve
  duty** that the IFC obeys without independent flame/limit supervision. That
  would indicate an uncertified, unsafe control path and is out of scope for
  this product.
- Any commissioning test in Section 10 shows that a demand channel does
  **not** drop to zero on loss of communications and no hardware means of
  removing that call exists.
- The pre-install survey (Section 2.4) finds equipment outside the supported
  family.
