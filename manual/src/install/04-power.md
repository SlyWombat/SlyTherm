# 4. Power

## 4.1 Source and budget

The DT-1 is powered from the furnace's 24 VAC control transformer via the
**R** and **C** conductors at the wall plate.

| Item | Value |
| --- | --- |
| Nominal supply | 24 VAC RMS (≈ 34 V peak) |
| Real-world supply | HVAC transformers run hot: 28–30 VAC at light load → **40 V+ peak**. The converter front end must be rated ≥ 50 V input |
| Transformer capacity | Typically 40 VA with little headroom |
| DT-1 draw | Display-class board: 1.6–2.25 W verified; **≤ ~3 VA** from the transformer including conversion losses |
| Transients | Processor + Wi-Fi peaks ~500 mA @ 3.3 V, 600 mA+ transient; display backlight inrush at power-up |

## 4.2 Isolated supply — mandatory

> **CAUTION:** 24 VAC is **AC** — a DC-DC buck converter cannot take it.
> Worse, HVAC control boards frequently **half-wave-rectify with C shared**:
> full-wave-bridging the same transformer (or bonding your DC ground to C)
> can short a half-cycle and blow the furnace's control fuse.

Use an **isolated AC-input AC/DC converter module**: input rated 9–36 VAC
(≤ 50 VDC), output 5 V at ≥ 1 A (~5 W). Isolation eliminates the
shared-C short outright.

> **CAUTION:** many inexpensive AC-input modules are transformerless and
> **non-isolated**. The production wall installation requires a
> **confirmed-isolated** supply. If no suitable isolated AC/DC module is
> available, the fallback is: bridge rectifier (≥ 100 V) → bulk capacitor
> (≥ 470 µF / 63 V) → isolated DC-DC converter — and even then, **never bond
> the DC ground to furnace C anywhere**.

![Power chain — fuse, MOV, isolated converter, controller](diagrams/inst-power-chain.svg)

## 4.3 Input protection — mandatory

| Component | Specification | Purpose |
| --- | --- | --- |
| Inline fuse on **R** | Slow-blow **250–500 mA**, 5×20 mm holder | Well below the furnace's own control fuse, so a fault blows *this* fuse, never the furnace's. The display backlight inrush may nuisance-trip a 250 mA fuse — be prepared to fit 500 mA (still far below the furnace control fuse) |
| MOV across **R–C** | ~40–47 VAC working voltage | Clamps gas-valve / inducer / igniter switching transients |
| Bulk capacitance at the controller | 470–1000 µF (63 V, 105 °C) + 0.1 µF ceramic, at the 5 V input | Kills Wi-Fi-transient brownouts |

The controller's internal brownout detector remains **enabled**; on
brownout the firmware boots to a no-demand state (Section 7).

## 4.4 Power wiring table

| From | To | Notes |
| --- | --- | --- |
| Wall plate **R** (24 VAC hot) | Isolated AC/DC **IN+** | via inline fuse, 250–500 mA slow-blow |
| Wall plate **C** (24 VAC common) | Isolated AC/DC **IN−** | MOV connected across R–C |
| AC/DC **5 V OUT+** | Controller **5 V / VIN** | bulk capacitors at the controller |
| AC/DC **5 V OUT−** | Controller **GND** | **isolated DC ground — do NOT bond to C** |

The only deliberate reference between the controller's electronics and the
furnace's 24 V system is the RS-485 bus-side ground reference described in
Section 5.4 — nothing else may bridge the isolation barrier.

## 4.5 Power-up behavior

On power-up (or any reset) the controller:

- holds the RS-485 driver disabled (the bus stays silent through boot —
  verified by oscilloscope at commissioning, Section 10);
- holds all relay outputs de-energized (Case B);
- asserts **no demand on any channel** until mode, setpoints, and sensors
  are validated;
- enforces the compressor post-boot hold-off if compressor-timer state was
  lost (Section 8).
