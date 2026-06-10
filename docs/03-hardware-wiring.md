# 03 — Hardware & Wiring (reviewed design)

This is the **corrected** hardware design after engineering review. The original plan in `dettson.md` had three things that would bite hard — they are fixed here. Read [`04-safety.md`](04-safety.md) alongside this.

## 0. Three corrections from the original plan

1. **MAX485 is a 5 V part.** Its `RO` output drives toward 5 V and **over-volts the ESP32's 3.3 V GPIO** (abs-max ~3.6 V). → **Use a 3.3 V transceiver** (THVD14xx or MAX3485), or an auto-direction module that is explicitly 3.3 V-native.
2. **"24VAC → 5V buck" is wrong.** 24 VAC is **AC**; a DC-DC buck cannot take it, and HVAC boards frequently **half-wave-rectify with `C` shared**, so full-wave-bridging the same transformer can **short a half-cycle and pop the furnace's control fuse**. → **Use an isolated AC-input AC/DC converter.**
3. **The plan said "command the gas valve."** We don't — we send a CT-485 **demand**; the furnace's IFC keeps all combustion safety. (See [`01-architecture.md`](01-architecture.md) §2 and [`04-safety.md`](04-safety.md).)

## 1. Power supply

- **24 VAC RMS ≈ 34 V peak**, and HVAC transformers run hot (28–30 VAC light-load → **~40 V+ peak**). Front end must be rated **≥ 50 V input**.
- The furnace transformer is typically **40 VA** with little headroom; the ESP32 + Wi-Fi peaks at **~500 mA @ 3.3 V (~1.7 W), 600 mA+ transient** — fine on VA (~0.1 A AC), but inrush can nuisance-trip the furnace.

**Recommended topology:**

1. **Isolated AC/DC module, AC-input rated 9–36 VAC / ≤ 50 VDC → 5 V ≥ 1 A (~5 W).** Isolation solves the half-wave/shared-`C` short outright.
   - Fallback if no tiny isolated AC/DC: bridge (≥ 100 V, e.g. MB10S) → bulk cap (≥ 470 µF/63 V) → isolated DC-DC, and then **do not bond DC ground to furnace `C` anywhere**.
2. Feed 5 V to the ESP32 `5V`/`VIN`; add **470–1000 µF + 0.1 µF** right at the ESP32 to kill Wi-Fi-transient brownouts. **Leave the ESP32 brownout detector enabled.**
3. **Input protection (mandatory):**
   - **Inline fuse on `R`:** slow-blow **250–500 mA** — well below the furnace's own control fuse, so a fault blows *your* fuse, never the furnace's.
   - **MOV across `R`–`C`** (~40–47 VAC working) to clamp gas-valve/inducer/HSI switching transients.

## 2. RS-485 interface

- **Transceiver:** **3.3 V, protected.** Preferred **TI THVD1410/THVD1450** (±16 kV IEC ESD, explicit DE/RE) or **MAX3485** (3.0–3.6 V, ESP32-native logic). **Not MAX485.** Power it from the ESP32's clean **3.3 V**.
- **Auto-direction modules:** cheap CD4069-type auto-dir hold TX for a fixed ~9.5 bit-times (baud-dependent) and can clip framing on a noisy bus. For a bus you must *coexist with*, prefer **explicit DE/RE in firmware** (precise turnaround; guaranteed silent when idle). If you go auto-direction, use a **silicon auto-dir part (THVD14xx)**, not the CD4069 hack. *(The ordering doc lists a 3.3 V auto-flow module as the convenient starter pick — fall back to a manual MAX3485 if you see framing errors.)*
- **Termination — do NOT add 120 Ω blindly.** You are adding a node to an **already-terminated** 2–3-node bus. An extra 120 Ω in the middle overloads the drivers and can break comms. **Measure** idle differential and A–B DC resistance first; add 120 Ω only if you've physically become a bus *end*.
- **Bias — likewise.** The furnace almost certainly biases the idle bus. Add pull-up/pull-down bias only if measurement shows idle |V_AB| < ~200–300 mV.
- **Stub length:** keep the tap **< ~0.3 m**.
- **A/B protection:** **bus-rated TVS array (SM712 or SMBJ pair)** A–B and A/B–GND, plus **~10 Ω series** resistors into the transceiver. A THVD14xx with built-in ESD reduces what you need externally.
- **Ground reference:** RS-485 needs a common-mode reference (−7…+12 V window). With isolated power, give the bus side a defined reference — tie transceiver GND to furnace `C` (directly on the bus side of an isolated barrier, or via ~100 Ω if non-isolated). **Do not** hard-bond your DC ground to `C` (recreates the half-wave short).

## 3. GPIO / pin map

- **RO → GPIO** is safe with a 3.3 V transceiver (no divider). If a 5 V MAX485 is ever used, a divider on RO is mandatory — but don't.
- **Strapping-pin cautions:** avoid driving GPIO0, 2, 5, 12, 15 at boot. **GPIO12 pulled HIGH at boot selects 1.8 V flash and bricks the boot** — keep it clear.
- **PSRAM caution:** on ESP32-WROVER and some dev boards, **GPIO16/17 are used by PSRAM**. ESP32 UART is fully remappable — if your board has PSRAM, move UART2 to GPIO25/26. **Check your specific board.**
- **Pull DE/RE to the receive/idle state** with a resistor so a booting/resetting/crashed ESP32 **releases the bus and stays silent**.

**Recommended pin map (verify against your board's PSRAM/flash usage):**

| Signal | Pin | Notes |
| --- | --- | --- |
| RS-485 `RO` → ESP32 RX | **GPIO16** (or GPIO25 if PSRAM) | input only |
| RS-485 `DI` ← ESP32 TX | **GPIO17** (or GPIO26) | |
| RS-485 `DE`+`RE` | **GPIO4** | **external pull-down → receive/idle**; bus released at boot |
| DS18B20 1-Wire | **GPIO13** or **GPIO14** | **4.7 kΩ pull-up to 3V3** |
| Status LED | a non-strapping GPIO (not GPIO2) | external LED |
| Avoid | GPIO0, 2, 5, 12, 15 | strapping — leave at defaults |

## 4. Galvanic isolation — recommended

For a **permanently installed gas appliance**, isolate power and ideally the bus:

- Eliminates the half-wave/shared-`C` short.
- Breaks ground loops (node-to-node ground offsets routinely exceed the RS-485 common-mode window and destroy transceivers).
- Contains faults — a fault on the ESP32 side can't inject onto the furnace's 24 V / safety circuits.

Two practical options:

- **A (simplest, recommended):** isolated AC/DC for power + a 3.3 V transceiver whose **bus-side ground references `C`**; the barrier lives in the power module.
- **B (fully isolated bus):** isolated RS-485 transceiver with integrated isolated DC-DC (**ADI ADM2587E/ADM2682E**, or TI **ISO3086T** + iso supply) — bus side galvanically isolated from logic *and* power, referencing `C` on the bus side only. Costs more; best robustness.

## 5. Revised wiring table

| From | To | Notes |
| --- | --- | --- |
| Furnace **R** (24 VAC) | Isolated AC/DC **IN+** | via **inline fuse 250–500 mA slow-blow** |
| Furnace **C** (24 VAC com) | Isolated AC/DC **IN−** | **MOV across R–C** |
| AC/DC **5V OUT+** | ESP32 **5V/VIN** | **470–1000 µF + 0.1 µF** at ESP32 |
| AC/DC **5V OUT−** | ESP32 **GND** | **isolated DC ground — do NOT bond to C** |
| ESP32 **3V3** | Transceiver **VCC** | 3.3 V transceiver only |
| Furnace **1** (Data A+) | Transceiver **A** | **TVS A–B & A/B–GND; ~10 Ω series**; short stub; no added 120 Ω/bias until measured |
| Furnace **2** (Data B−) | Transceiver **B** | as above |
| Furnace **C** (bus ref) | Transceiver **GND** (bus side) | RS-485 common-mode reference (via barrier or ~100 Ω if non-isolated) |
| Transceiver **RO** | ESP32 **GPIO16** (GPIO25 if PSRAM) | native 3.3 V — no divider |
| Transceiver **DI** | ESP32 **GPIO17** (GPIO26 if PSRAM) | |
| Transceiver **DE+RE** | ESP32 **GPIO4** | **pull-down to receive/idle** |
| External watchdog out | DE-disable / power-cut to transceiver | **forces NO-DEMAND on ESP32 hang** |
| **DS18B20** data | ESP32 **GPIO13/14** | **4.7 kΩ pull-up to 3V3** |
| Status LED | non-strapping GPIO | not GPIO2 |

## 6. Component list (specs — links in [`ORDERING.md`](ORDERING.md))

- **Isolated AC/DC**, AC-input 9–36 VAC / ≤ 50 VDC, 5 V ≥ 1 A (~5 W).
- **Inline fuse + holder** 250–500 mA slow-blow; **MOV** ~40–47 VAC across R–C.
- **Bulk cap** 470–1000 µF **63 V** + 0.1 µF; **105 °C** electrolytics.
- **ESP32 dev board** — confirm PSRAM (affects GPIO16/17); brownout detector left enabled.
- **RS-485 transceiver** — 3.3 V protected: **THVD1410/THVD1450** or **MAX3485** (*not MAX485*). For full bus isolation: ADM2587E/ADM2682E or ISO3086T-class.
- **RS-485 line protection** — TVS array (SM712/SMBJ) A–B / A/B–GND; ~10 Ω series.
- **External hardware watchdog** — TPL5010 / MAX6369 class (or ATtiny) → forces NO-DEMAND on timeout.
- **Temperature sensors** — DS18B20 (local fallback, 4.7 kΩ pull-up) + MQTT room sensor (primary); optional 2nd DS18B20 for voting.
- **Enclosure** — UL94 V-0 / polycarbonate, screw terminals, glands, 105 °C-tolerant, mounted on the **cool side** of the furnace.
- **CO alarm** in the dwelling; **OEM thermostat retained** for rollback.
