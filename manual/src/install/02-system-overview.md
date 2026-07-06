# 2. System Overview and Pre-Install Survey

## 2.1 What you are installing

The SlyTherm is a **single wall-mounted touchscreen controller** installed at the
location of the OEM thermostat. The existing thermostat wall plate already
carries the four conductors the controller needs in a communicating system:

| Wall terminal | Function |
| --- | --- |
| **R** | 24 VAC hot (from the furnace transformer) |
| **C** | 24 VAC common |
| **1** | CT-485 bus, Data A (+) |
| **2** | CT-485 bus, Data B (−) |

Behind the display, inside the wall enclosure, live the supporting
electronics — all required, none optional:

- **Isolated AC-input power supply** (24 VAC → 5 V) with inline fuse and MOV
  (Section 4).
- **3.3 V RS-485 transceiver** with explicit driver-enable (DE/RE) control
  and transient protection (Section 5).
- **External hardware watchdog** that forces the controller to a no-demand
  state if the processor hangs (Section 7).
- Local fallback temperature sensor (DS18B20, 1-Wire).

The controller speaks **ClimateTalk (CT-485)** to the furnace for modulating
gas demand (40–100 %), supervises the dual-fuel arbitration between gas and
heat pump, and integrates with **Home Assistant** over MQTT (local network
only — no cloud service).

## 2.2 The two installation cases

How the heat pump is commanded depends on which control architecture is
installed at the site. **The installation forks here**, and the pre-install
survey (Section 2.4) is how you determine which case applies. Do not assume
either case, and do not purchase Case-B hardware before the survey.

### Case A — Communicating (Dettson Alizé outdoor unit + K03085 interface board)

The outdoor unit is reached *via the bus*: a Dettson K03085 interface board
inside the furnace (RJ-11 cable to the IFC; COND-1/2/3 plus a
Gree-proprietary link to the outdoor unit) is a CT-485 node, and the SlyTherm
sends heat-pump and cooling demands to it over the same two bus wires already
at the wall. **No additional output hardware is required.** The 24 V sense
inputs described in Section 6 are still recommended as permanent
instrumentation.

![Case A — communicating installation, block diagram](diagrams/inst-case-a-block.svg)

### Case B — Hybrid (true Gree FLEXX, conventional 24 V outdoor-unit control)

A true Gree FLEXX outdoor unit is **not a CT-485 node** — it is commanded by
conventional 24 V signals at the cased-coil board (its H1/H2 RS-485 link is
Gree-proprietary and must never be touched). In this case, gas demand stays
on CT-485, and the SlyTherm adds a **24 V relay output stage** (Y1, Y2 if
supported, O/B, G) plus opto-isolated sense inputs and a hardwired condensate
float switch (Section 6).

![Case B — hybrid installation, block diagram](diagrams/inst-case-b-block.svg)

> **⚠ Pending verification (gates Case B):** it has not yet been confirmed
> whether the Chinook IFC honors hardwired Y/G/W signals **while operating in
> communicating mode**, or whether bus communications disable the 24 V
> terminals (the furnace's cooling-CFM DIP switches are documented as ignored
> in communicating mode). The hybrid path is viable only if this mixed mode
> works. This must be proven on the installed equipment before Case B wiring
> is committed.

> **⚠ Pending verification (Case B alternative gas path):** the Chinook's
> V/W2 terminal accepts a 24 V modulating signal providing the full 40–100 %
> range with no CT-485 connection at all. It exists as a documented fallback
> path only. **Never apply raw 24 VAC to V/W2** — documented damage hazard.

### Single unit vs. split installation

The standard installation is a single wall unit. Two findings force a
**split installation** instead (a headless controller at the furnace/coil,
with the wall display — or a Home Assistant dashboard — connected over the
network only):

1. **Case B with too few conductors at the wall plate.** A wall unit can only
   drive Y1/Y2/O-B/G if enough conductors exist in the existing thermostat
   cable. Count them during the survey.
2. **Bus-timing qualification failure.** Single-unit operation is gated on a
   bench measurement showing the touchscreen firmware meets the CT-485
   bus-timing budget. This is a recorded pass/fail from bench testing, not a
   field judgment.

> **⚠ TBD (Phase 0):** the installed architecture (Case A vs Case B) and the
> single-vs-split decision are outputs of the pre-install survey and bench
> qualification. This draft cannot tell you which case you have.

## 2.3 What travels where (summary)

| Signal | Case A | Case B |
| --- | --- | --- |
| Gas heat demand (40–100 % modulating) | CT-485 bus | CT-485 bus (V/W2 modulating signal exists as fallback — pending verification) |
| Heat-pump heat / cool demand | CT-485 bus (to interface-board node) | 24 V relays: Y1, Y2 (if supported), O/B, G |
| Blower/fan demand | CT-485 bus (mapping pending verification) | CT-485 bus and/or G relay (mapping pending verification) |
| Defrost indication | bus signature (pending verification) | D/W 24 V sense input |
| Furnace status, modulation %, faults | CT-485 bus | CT-485 bus |
| Condensate protection | hardwired float switch (required whenever cooling is enabled, either case) | hardwired float switch in series with Y |

## 2.4 Pre-install survey (required before any purchase or wiring)

The survey is a **zero-risk paper-and-camera exercise** — no electronics are
connected, nothing is energized beyond normal equipment operation. Its result
decides Case A vs Case B and single vs split. (Engineering documents call
this survey "Phase 0".)

> **WARNING:** during the survey, never apply 24 VAC to the V/W2 terminal,
> and do not disconnect any safety wiring. Photograph; do not rewire.

**Procedure:**

1. **Record the existing wall thermostat model.**
   - R02P032 or R02P034 → communicating installation (expect Case A).
   - R02P033 or any conventional thermostat → conventional installation
     (expect Case B).
2. **Open the Chinook blower door and look for an interface board.** A
   K03085 (or K03081) interface board is identifiable by its RJ-11 cable
   (A00443) to the IFC and COND-1/2/3 wires running toward the outdoor unit.
   Present → Case A. Absent → Case B.
3. **Read the outdoor-unit nameplate.** Record the model (MHD/Alizé =
   communicating family; GUD/FXD FLEXX = conventional family) and the rated
   minimum operating ambient (needed for configuration, Section 8).
4. **Record the IFC DIP switches S4-2 / S4-3.**
   - OFF/OFF = modulating/communicating mode.
   - ON/ON = 2-stage thermostat mode.
5. **Photograph every low-voltage conductor** at:
   - the furnace terminal strip (R, C, W1, Y1, Y/Y2, G, V/W2, B);
   - the cased-coil board (Y1in/Y2in/W1in/W2in/Gin, B, D, H1/H2, and the
     SA1 DIP switch);
   - the outdoor unit's low-voltage terminals.
6. **Count the conductors in the thermostat cable at the wall plate.** Note
   total conductors, spares, and condition. Case B as a single wall unit
   needs R, C, 1, 2 **plus** Y1/(Y2)/O-B/G and any sense lines — if the cable
   cannot carry them, plan the split installation.
7. **Document the result:** architecture (A or B), thermostat model, outdoor
   unit model and rated minimum ambient, IFC DIP states, the full low-voltage
   wiring map, and the wall-plate conductor count.

**Survey decision table:**

| Finding | Conclusion |
| --- | --- |
| Communicating thermostat + interface board + Alizé outdoor unit | **Case A** — no relay stage; all demands over the bus |
| Conventional thermostat, no interface board, FLEXX outdoor unit | **Case B** — relay stage required; mixed-mode verification required first |
| Mixed/ambiguous findings | Stop. Resolve with Dettson documentation before proceeding |
| Case B and insufficient wall conductors | **Split installation** (relay stage at the furnace/coil end) |
