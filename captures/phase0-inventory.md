# Phase 0 — Installed-equipment inventory

Record of the installed Dettson/Gree equipment, gathered for issue #24. Sources:
cabinet photos `PXL_20260703_162030901` and `PXL_20260703_162035598` (2026-07-03)
plus owner-read rating-plate serials.

## Equipment identified

| Item | Finding | Confidence | Source |
| --- | --- | --- | --- |
| Furnace model | **Dettson C105-MV** (Chinook modulating gas) | High | rating plate (`PXL_20260704_1531*`) |
| Furnace input / output | Max **105,000** / 99,750 BTU/h; Min **42,000** / 39,900 BTU/h | High | rating plate |
| Furnace electrical / blower | 120 V 60 Hz 15.8 A (20 A OCP); **1 HP ECM** blower | High | rating plate |
| Furnace gas / temp rise | NG, orifice #48 DMS, manifold 0.6"→3.0" WC; rise 40–70 °F, max outlet 170 °F, ESP 0.18–1.0" WC | High | rating plate |
| Furnace control board | Dettson/Vitréltech IFC, **P/N R99G014 Rev A** (assy 160-0230-V00) | High | board label |
| Indoor unit serial | `SD122553821` / `9AK235N000260` (BT 508124, X01496 L) | High (owner-read) | rating plate |
| Outdoor unit serial | `9V9506N000218` | High (owner-read) | rating plate |
| Outdoor unit **model** | **Not yet read** — on ODU nameplate outside; expected Gree FLEXX 4–5 ton | — | pending |
| Interface board (K03085) | **Not present** — no separate RJ-11 + COND-1/2/3 board in cabinet | Med-High | wide shot |
| Inducer motor | FASCO; standard layout | High | wide shot |
| ECM blower drive | Separate variable-speed inverter/VFD board, label `ODEL-VFD00H111A` (DC-bus cap, common-mode choke, IGBTs); green LED = powered. Commanded by the IFC, **not** on the CT-485 thermostat bus. | High | back-of-cabinet shot (`PXL_20260704_153152842`) |
| Air cleaner (airstream accessory) | **Honeywell F300E1019** whole-house **Electronic Air Cleaner**, 16" × 25", installed in the return. **Not a media/replaceable-filter unit** — washable electronic cells (clean every 6–12 mo, at least yearly), washable prefilter, replaceable postfilter (~6 mo). Passive to SlyTherm (no bus/24 V interface); tracked only via the HA runtime-service reminder. | High | owner-supplied (2026-07-15) |

> **Modulation floor confirmed:** min input 42,000 ÷ max 105,000 = **40 %** — exactly the firmware's
> 40–100 % demand range and min-fire clamp. Hardware matches the design target.

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

- [x] ~~Furnace model~~ — **C105-MV** confirmed (2026-07-04). ✓
- [ ] **Outdoor-unit model** — read from ODU nameplate outside next visit (serial already have).
- [ ] ST9 terminal strip with wires — map wire **colour → terminal** for the
      6-conductor wall cable.
- [ ] Identify daughtercard `CAO1194V0` (6-pin, lit red LED near ST6/ST8).

_Non-blocking / deferred:_
- **S4 DIP positions** — not needed to gather physically: the installed R02P032 works over
  CT-485, which already proves communicating mode (S4-2/S4-3 OFF). Confirmatory only.
- IFC banks S1 / S3 / S5 — record only if convenient.
