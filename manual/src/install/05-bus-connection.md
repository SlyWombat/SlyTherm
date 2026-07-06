# 5. CT-485 Bus Connection

## 5.1 Bus basics

| Parameter | Value |
| --- | --- |
| Physical layer | EIA/TIA-485, 2-wire half-duplex ("CT-485") |
| Wall terminals | **1 = Data A (+)**, **2 = Data B (−)** |
| Signaling | 9600 bps, 8N1 (confirm by capture at commissioning) |
| Frame delimiting | inter-byte idle gap > 3.5 ms (no preamble byte) |
| Topology | coordinator-polled (token); the thermostat transmits only in its slot |

Keep the bus pair **twisted** all the way to the transceiver, and keep the
tap stub **shorter than ~0.3 m**.

![Wall-plate wiring — R/C power and CT-485 bus to the SlyTherm wall unit](diagrams/inst-wall-plate-wiring.svg)

## 5.2 Transceiver requirements

The SlyTherm board's transceiver (on the carrier board behind the display) must meet
all of these — substitutions that miss any one of them break the safety
chain:

- **3.3 V-native** part (THVD1410/THVD1450 or MAX3485 class). **Never a
  MAX485** — it is a 5 V part whose receive output over-volts the
  controller's 3.3 V inputs.
- **Explicit DE/RE (driver-enable) control** — no auto-direction modules.
  The external hardware watchdog must be able to force the driver off
  (Section 7), and an auto-direction module gives it no pin to do that, nor
  a guaranteed-silent idle.
- **DE/RE pulled to the receive/idle state by a resistor**, so a booting,
  resetting, or crashed controller **releases the bus and stays silent**.
- Powered from the controller's clean 3.3 V rail.

> **Onboard transceiver.** The SlyTherm carrier board satisfies these
> requirements on-board: it provides a **galvanically-isolated RS-485
> transceiver** whose A/B data lines are driven from the ESP32-S3 UART0 on
> **GPIO43 (TX)** and **GPIO44 (RX)**, with the DE/RE direction control on a
> controller GPIO that the external hardware watchdog can force off
> (Section 7). Because the transceiver is isolated, the isolation requirement
> of Sections 4 and 5.4 is met at the board — no external isolator is needed.

## 5.3 Line protection

| Component | Placement |
| --- | --- |
| Bus-rated TVS array (SM712, or SMBJ pair) | A–B, and A/B to bus-side ground |
| ~10 Ω series resistors | in the A and B lines into the transceiver |

## 5.4 Bus ground reference

RS-485 requires a common-mode reference (−7 to +12 V window). With the
isolated power supply of Section 4, give the bus side a defined reference:
tie the transceiver's **bus-side ground to furnace C** — directly on the bus
side of the isolation barrier, or through ~100 Ω if the design is
non-isolated at that point.

> **CAUTION:** do **not** hard-bond the controller's DC ground to C — that
> recreates the half-wave short described in Section 4.2.

## 5.5 Termination and bias — measure first, at the wall plate

You are adding a node to an **already-terminated, already-biased** 2–3-node
bus. Blindly adding a 120 Ω terminator in the middle of the bus overloads
the drivers and can break communications for every node. Likewise, the
furnace almost certainly biases the idle bus — adding more bias can skew it.

The OEM thermostat location **may have been a terminated bus end** — if your
node physically becomes the bus end at the wall, the answer differs from a
mid-bus tap. That is why this measurement is performed **at the wall plate**,
not assumed from the furnace end.

**Procedure (at the wall plate, before connecting the SlyTherm's A/B lines):**

1. With the system powered **off**, measure the DC resistance between
   terminals 1 and 2 (A–B).
   - ≈ 120 Ω suggests one terminator is reachable from here (you may be at
     an unterminated end of a singly-terminated bus);
   - ≈ 60 Ω suggests both terminators are present elsewhere — the bus is
     fully terminated and you are a mid-bus tap. **Add nothing.**
2. With the system powered **on** and the bus idle, measure the differential
   idle voltage |V(A−B)|.
   - ≥ ~200–300 mV: the bus is adequately biased. **Add no bias.**
   - < ~200–300 mV: note it; bias may be required at this node.
3. **Add a 120 Ω terminator only if** the measurements show your node has
   physically become a bus *end* that lacks termination.
4. **Add pull-up/pull-down bias only if** step 2 showed an inadequately
   biased idle bus.
5. Record all measurements and what (if anything) was fitted — they are part
   of the commissioning record (Section 10).

## 5.6 Connection table

| From | To | Notes |
| --- | --- | --- |
| Wall plate **1** (Data A+) | Transceiver **A** | TVS + ~10 Ω series; twisted pair; stub < 0.3 m; no added 120 Ω/bias until measured (5.5) |
| Wall plate **2** (Data B−) | Transceiver **B** | as above |
| Furnace **C** (via wall plate C) | Transceiver bus-side **GND** | common-mode reference (5.4) |
| Transceiver **RO** | Controller UART RX | 3.3 V native — no divider |
| Transceiver **DI** | Controller UART TX | |
| Transceiver **DE+RE** | Controller GPIO **and** watchdog cut path | resistor pull to receive/idle; watchdog can force off (Section 7) |

## 5.7 Boot silence and bus discipline

The CT-485 bus carries the furnace's own control traffic; corrupting it is a
failure mode in its own right. The SlyTherm's bus discipline (all enforced in
firmware, verified at commissioning):

- **Silent at boot/reset** — DE is resistor-pulled off; the oscilloscope
  check in Section 10 proves no byte is emitted during a reset.
- **Transmit only when granted the bus** by the coordinator's polling
  (token) mechanism; release the driver at idle.
- Driver pre/post-enable hold of ~300 µs around each transmission; 100 ms
  inter-packet idle respected.
- Every frame checksummed; persistent inability to obtain clean
  communications → the controller goes passive (no demand).

> **⚠ Pending verification:** the exact demand-message payload offsets and
> the coordinator token timing budget have **not yet been confirmed from
> captures of the installed equipment**. Active transmission is gated on that
> confirmation; an installation from this draft must not enable the
> transmit path.

> **WARNING — one thermostat only.** The OEM communicating thermostat must be
> **removed from the bus** before the SlyTherm ever transmits. Two masters on a
> coordinator-polled bus is undefined behavior. (Keep the OEM thermostat on
> site for rollback — Section 11.)
