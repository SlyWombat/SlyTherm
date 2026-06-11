# 7. The Safety Chain

This section explains, for the installer, how the system fails safe — what
the certified equipment enforces on its own, what the DT-1 enforces, and
what the external hardware enforces even when the DT-1's processor is dead.
Understanding this is a prerequisite for the commissioning tests in
Section 10.

![Safety chain — watchdog forcing no-demand via driver-enable and relay-coil cuts](diagrams/inst-safety-chain.svg)

## 7.1 Division of enforcement

| Layer | Enforces | Independent of the DT-1? |
| --- | --- | --- |
| **Furnace IFC (certified)** | Flame sensing/lockout, primary high-limit, rollout, pressure/vent proving, ignition-trial limits, modulation ceiling | **Yes** — not on the bus, cannot be disabled over CT-485 |
| **Heat-pump outdoor unit (equipment protections, not a certified chain)** | High/low refrigerant pressure trips, high discharge temperature, compressor overload, inverter-module protection, internal ~3-minute restart delay, minimum run time. Repeated trips **latch a manual-reset fault** | Yes — but treat as a **backstop only**. Note: smaller units have *no* low-pressure switch, and these are equipment protections, not life-safety. Verify the installed model's protections against its manual during the survey |
| **External hardware (watchdog, pull resistors, float switch)** | No-demand on processor hang; bus silence at boot; relays open at boot; cooling call broken on condensate overflow | Yes — works with the processor dead |
| **DT-1 firmware** | Everything in 7.3 | No — which is why the layers above exist |

Relying on the outdoor unit's 3-minute delay as the anti-short-cycle
mechanism is **not acceptable**: repeated trips latch manual-reset faults
that strand the system. The DT-1's compressor timers are the primary
protection.

## 7.2 Fail-to-no-demand, explained for installers

The DT-1's prime directive: **on any abnormal condition, fail to NO-DEMAND on
all channels** — heat, cool, fan, backup/aux, defrost — and, in Case B, all
relays de-energized — **without violating compressor minimum timers on the
way down or back up**.

Why "no demand" is the safe direction:

- On the bus, equipment treats **loss of the thermostat's periodic demand
  messages as "drop the call"** — a silent node ramps the equipment down via
  its own normal logic. The failsafe is "go quiet," not "send an off command
  and hope it arrives."
- On relays, the equivalent is an **open contact**. But relays have the
  opposite failure polarity — a *stuck-closed* contact keeps equipment
  running — which is exactly why the watchdog cuts relay-coil **power**
  rather than asking firmware to open the relay.

> **⚠ Pending verification (commissioning closes this):** "silence drops the
> call" is inferred from prior art for furnace heat and must be **proven per
> demand channel** on the installed system — including whether silence
> propagates to the non-coordinator unit (interface board / outdoor-unit
> side) in Case A. The pull-bus tests in Section 10 exist for this. Any
> channel where silence does **not** drop the call needs a hardware
> call-removal path before the system may be left in service — and that
> finding materially raises project risk (Section 1.7).

A flapping fault must not short-cycle the compressor through the failsafe
path itself: recovery from any failsafe trip passes through the compressor
minimum-off timer like any other start.

## 7.3 What the DT-1 firmware enforces

- **Compressor protection (primary):** minimum OFF time, minimum ON time,
  maximum starts/hour, post-boot hold-off — with timer state persisted so a
  reboot cannot erase a pending minimum-off. If persisted state is missing
  at boot, the full hold-off is enforced.
- **Mutual exclusion of gas heat and compressor heat** — prohibited by the
  manufacturer (the heat-pump coil sits downstream of the heat exchanger;
  gas-heated air entering the coil drives head pressure toward the
  high-pressure cutout). Nothing at the bus level enforces this; the DT-1
  does, at its single demand-emission point. Defrost tempering is the sole
  sanctioned overlap, with a fixed demand and a 15-minute hard cap.
- **Low-ambient compressor lockout and dual-fuel balance-point logic**, with
  a hard configuration rule: any lockout/balance-point combination that
  leaves some outdoor-temperature band with **no permitted heat source** is
  rejected as invalid.
- **Sensor fault handling:** range, staleness, stuck-value, and divergence
  checks on every temperature input; loss of all valid indoor sources →
  demand zero on all channels + alarm. Unknown outdoor temperature →
  fail cold: gas allowed, compressor locked out.
- **Blower-proven interlock:** no cooling call without blower confirmation
  (bus telemetry in Case A; sensed G/Y feedback in Case B); the cooling call
  is dropped if feedback disappears.
- **Boot validation:** every boot starts at no-demand until mode, setpoints,
  and sensors validate; the restored mode is cross-checked against outdoor
  lockouts before any demand.
- **Reset-loop lockout:** ≥ 3 watchdog/brownout resets within 30 minutes →
  latched NO-DEMAND requiring manual clearing — a reboot loop must not
  repeatedly restart the compressor.
- **Runtime policy:** gas call > 4 h without progress → drop + alarm. Heat
  pump: **progress alarm only, never auto-cycled** — continuous
  near-balance-point running is normal for an inverter heat pump.
- **Bus discipline:** transmit only when polled, checksum everything, go
  passive if clean communications cannot be maintained.

## 7.4 The external hardware watchdog

An independent supervisor (TPL5010 / MAX6369 class) that the controller's
safety task must pet periodically. The safety task pets it **only when its
own invariants hold** — a wedged UI or control task cannot keep it fed.

On timeout the watchdog — in hardware, with no firmware involvement —
**forces the no-demand state**:

1. **Holds the RS-485 driver-enable (DE) off** → the controller cannot drive
   the bus; its demands age out and equipment drops the calls.
2. **Cuts the common relay-coil feed (Case B)** → all output relays open
   regardless of GPIO states.

Both actions together are what "watchdog forces no-demand" means; the DE cut
alone is not sufficient once relays exist. The watchdog **remains armed at
all times**, including during firmware updates — there is no "disable for
update" mode.

The DT-1's processor-internal watchdog only reboots the chip; it is a
convenience layer, not the safety layer. The layered set is: internal task
watchdog (hung task) → external hardware watchdog (dead chip) → equipment
comms-loss timeout (catch-all).

## 7.5 UI failure is a comfort outage, not a safety event

The touchscreen UI runs isolated from the protocol/safety tasks. A UI crash
leaves the safety task, the demand-refresh discipline, and the watchdog
chain untouched; a whole-chip hang lands on the external watchdog as above.
The worst credible UI-load failure is a delayed demand refresh — which
degrades toward *no demand*, the safe direction.
