# 6. Case B Wiring — 24 V Relay Outputs and Sense Inputs

This section applies **only to Case B** (hybrid installation: gas demand on
CT-485, heat pump on conventional 24 V signals). Skip it entirely for Case A
— except 6.5 (condensate float switch), which is required **whenever cooling
is enabled, in either case**, and 6.4 (sense inputs), which is recommended
instrumentation in both cases.

> **⚠ TBD (Phase 0):** do not order or install Case-B hardware until the
> pre-install survey confirms the conventional/hybrid architecture.

> **⚠ Pending verification (gates all of Case B):** whether the Chinook IFC
> honors hardwired Y/G/W while in communicating mode is unconfirmed (see
> Section 2.2). Prove mixed-mode operation on the installed equipment before
> committing this wiring.

## 6.1 Relay output stage

| Output | Function | Contact requirement |
| --- | --- | --- |
| **Y1** | Compressor stage 1 | 24 VAC-rated dry contact (relay or optotriac), normally open |
| **Y2** | Compressor stage 2 — only if the installed model supports two stages | as above |
| **O/B** | Reversing valve | as above |
| **G** | Indoor blower | as above |

Rules (all mandatory):

1. **Normally open, demand = energized.** De-energized = no demand, always.
2. **Relays must idle de-energized at boot and reset.** Drive GPIOs carry
   external pull-downs so a booting or crashed controller cannot close a
   contact — the same discipline as the bus driver-enable line. Verified by
   scope/sense input at commissioning.
3. **The common relay-coil feed is switched by the external hardware
   watchdog, not by a GPIO.** On watchdog timeout, power to *all* relay
   coils is cut (Section 7). Rationale: "bus silence = safe" does **not**
   transfer to relays — a hung controller with Y latched closed would run
   the compressor indefinitely, with no refresh timer on a closed contact.

> **WARNING — Reversing-valve polarity.** The Gree convention is
> **B = energized in HEATING** — the opposite of the common "O = cool"
> default. Getting this wrong inverts heating and cooling. The polarity is
> verified on the installed equipment at commissioning (Section 10), and the
> O/B output changes state **only with the compressor proven idle** — never
> under load.

**Firmware relay sequencing.** The firmware drives these four outputs
through a dedicated sequencer whose rules the commissioning interlock tests
(Section 10) verify at the terminals:

1. **O/B changes only with the compressor proven idle** — its own Y output
   off *and* the compressor minimum-off timer (Section 8.1) served. A
   demand that needs the opposite valve position drops Y first and waits;
   when idle, the valve is pre-positioned from the system mode so it flips
   unloaded ahead of the next call, not in front of one.
2. **G is forced on with any Y** — the blower runs whenever the compressor
   is called, regardless of the fan demand. (Blower-*proven* remains a
   sense-side interlock on the G feedback input, 6.4: no Y without proof.)
3. **Y2 is only ever energized together with Y1.**
4. **Successive output transitions are spaced at least 500 ms apart**
   (`kRelayMinTransitionMs`, Section 8) so contacts never chatter; a
   deferred change waits at most one spacing window.
5. **All outputs are off at boot and on any failsafe drop.** The failsafe
   all-off is immediate — never deferred by the transition spacing — and
   the watchdog coil-cut path (Section 7) backstops it.

The defrost D-wire sense input is observation only: the outdoor unit owns
its defrost cycle, and these outputs are not altered by it (defrost
*tempering* reaches the furnace over the bus, never through these relays).

## 6.2 Conductor allocation at the wall plate

A single-wall-unit Case B installation must carry, in the existing thermostat
cable: **R, C, 1, 2** (power + bus) **plus Y1, (Y2), O/B, G** and any sense
conductors. If the survey's conductor count is insufficient, install the
relay/sense stage at the furnace/coil end instead (split installation) — do
not run new cable through uninspected cavities without local-code review.

## 6.3 Where the 24 V signals land

Wire the relay outputs to the corresponding inputs at the **cased-coil
board / furnace terminal strip** exactly as a conventional thermostat would
(Y1in/Y2in, B, Gin per the survey's photographed wiring map). Do not open or
modify the Gree H1/H2 link or the COND wiring.

## 6.4 Opto-isolated 24 V sense inputs

Permanent instrumentation, recommended in both cases and required in Case B:

| Sense input | Source | Purpose |
| --- | --- | --- |
| **D/W (defrost)** | Outdoor unit's defrost output / furnace W node | Detects an active defrost so the controller can hold steady through it. **Note:** the thermostat's W2/AUX conductor ties to the same furnace-W node — the sense input must tolerate *either* source energizing the line |
| **Y** | Compressor call terminal | Output-vs-command mismatch detection (stuck relay) |
| **G** | Blower call terminal | Blower-proven interlock feedback |
| **W** | Gas/aux heat terminal | Instrumentation, mismatch detection |

A sensed mismatch between commanded and actual terminal state raises an
alarm and engages the watchdog coil-cut path (Section 7).

## 6.5 Condensate float switch — hardwired, both cases

> **Required whenever cooling is enabled, in either installation case.**

The float (wet-switch) in the condensate pan/drain must **break the cooling
call independently of software**:

- **Case B:** wire the float switch **in series with the Y circuit** — a
  tripped float physically opens the compressor call no matter what the
  controller does.
- **Case A:** there is no thermostat Y conductor; the float switch must break
  the cooling call in the equipment's own cooling-call circuit instead.
- An additional parallel connection to a controller sense input is for
  **alarming only** — it never replaces the hardwired series break.
- Verified at commissioning: the float switch must kill the cooling call
  with the firmware unaware (Section 10).

> **⚠ Pending verification (Case A break point):** the exact hardware break
> point for the float switch in a communicating installation (e.g. at the
> cased-coil board's cooling-call input) is finalized during installation
> engineering, against the photographed wiring map from the survey. Whatever
> point is chosen, it must pass commissioning test **E2** (Section 10): the
> float must kill the cooling call with the firmware unaware.

## 6.6 V/W2 modulating-signal path (fallback only)

The Chinook's V/W2 terminal accepts a 24 V **modulating signal** giving the
full 40–100 % gas range with no CT-485 connection (the R02P029/1F95M-class
thermostat path). It is documented here as a contingency only and is not the
standard installation.

> **WARNING:** **Never apply raw 24 VAC to V/W2.** This is a documented
> damage hazard. If this path is ever used, only the proper modulating
> signal source may drive the terminal.
