# Phase 0 — Installed-equipment inventory

Record of the installed Dettson/Gree equipment, gathered for issue #24. Sources:
cabinet photos `PXL_20260703_162030901` and `PXL_20260703_162035598` (2026-07-03)
plus owner-read rating-plate serials.

## Equipment identified

| Item | Finding | Confidence | Source |
| --- | --- | --- | --- |
| Furnace type | Modulating gas furnace (modulating gas valve w/ actuator, "adjust gas pressure" label) | High | valve close-up |
| Furnace control board | Dettson/Vitréltech IFC, **P/N R99G014 Rev A** (assy 160-0230-V00) | High | board label |
| Indoor unit serial | `SD122553821` / `9AK235N000260` | High (owner-read) | rating plate |
| Outdoor unit serial | `9V9506N000218` | High (owner-read) | rating plate |
| Interface board (K03085) | **Not present** — no separate RJ-11 + COND-1/2/3 board in cabinet | Med-High | wide shot |
| Blower/inducer | FASCO motor; standard layout | High | wide shot |

## CT-485 / comms — validates the sniffer plan

Full RS-485 front end on the main board:

- Transceiver U17 (MAX-series), **150 Ω termination** resistor
- **BIAS / BIAS / TERM** 3-way DIP selector
- Dedicated PIC18F comms MCU, RX + STATUS LEDs, debug header

**Implication:** the furnace ↔ R02P032 conversation is genuine RS-485/CT-485 on
terminals **1** and **2**. The sniff plan (NEXT-STEPS Steps 5–7) is valid.

## Low-voltage terminal block (ST9)

Conventional 24 V terminals present: **G, W1, Y1, Y2, B (reversing valve), Y/W2**
(plus R/C).

**Implication:** the heat pump is driven by **24 V from the main board**, not as a
CT-485 bus node.

> ⚠️ `Y/W2` is the terminal the safety doc says never to back-feed with 24 V.

## Working architecture hypothesis

```
R02P032 wall stat ──CT-485 (1,2)──► R99G014 main board ──24 V (Y1/Y2/B/G/W1)──► Gree FLEXX + gas
```

⇒ The replacement controller likely needs **CT-485 only, no 24 V relay board of its
own** — because the main board already translates CT-485 demands into the 24 V
heat-pump outputs. **This is PENDING a cool-call capture (Step 6)** proving the bus
actually carries the cooling/HP demand; treat it as the leading hypothesis, not
settled.

## Still open (to close issue #24)

- [ ] **Rating plate, straight-on** — capture furnace + ODU **model numbers**
      (BTU/tonnage). Only serials read so far.
- [ ] **S4 DIP bank, straight-on macro** — record positions (expected S4-2 & S4-3
      OFF = communicating thermostat). Unread — glare/angle.
- [ ] ST9 terminal strip with wires — map wire **colour → terminal** for the
      6-conductor wall cable.
- [ ] Identify daughtercard `CAO1194V0` (6-pin, lit red LED near ST6/ST8).
- [ ] IFC banks S1 / S3 / S5 positions (record for completeness).
