# 03 — Hardware & Wiring (reviewed design)

This is the **corrected** hardware design after engineering review. The original plan in [`legacy-plan.md`](legacy-plan.md) had three things that would bite hard — they are fixed here. Read [`04-safety.md`](04-safety.md) alongside this. Two additions follow the dual-fuel re-scope: §7 (HP command-path contingency — the Case A/B design fork) and §8 (production wall-mounted touchscreen controller).

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
- **Auto-direction modules:** cheap CD4069-type auto-dir hold TX for a fixed ~9.5 bit-times (baud-dependent) and can clip framing on a noisy bus. For a bus you must *coexist with*, prefer **explicit DE/RE in firmware** (precise turnaround; guaranteed silent when idle). If you go auto-direction, use a **silicon auto-dir part (THVD14xx)**, not the CD4069 hack. *(The ordering doc lists a 3.3 V auto-flow module for the RX-only sniff rig; the production wall unit requires manual DE/RE so the hardware watchdog can cut DE — see §8.)*
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

*(This table is the **DevKitC sniff-rig / spare** map. The production controller is an ESP32-S3 display board with a much tighter free-GPIO budget — see §8.)*

**Case B additions (only if Phase 0 finds the conventional/hybrid architecture — see §7):**

| Signal | Pin | Notes |
| --- | --- | --- |
| Relay drives **Y1, Y2, O/B, G** | 4× non-strapping GPIOs (e.g. GPIO18/19/21/22 on classic ESP32) | **external pull-downs; relays must idle de-energized at boot/reset** — same discipline as DE/RE |
| Relay-coil common feed | switched by the **external watchdog**, not a GPIO | watchdog timeout cuts power to *all* relay coils (§7) |
| Opto-isolated 24 V sense: **D/W defrost, Y, G, W** | input-capable GPIOs (GPIO34/35/36/39 are input-only — ideal) | permanent instrumentation |
| Condensate float switch | **series-wired in the cool-call circuit** (hardware break) | optional parallel GPIO sense for alarming only |

On the S3 display board (§8) this full Case B set may not fit alongside UART1 + DE + 1-Wire + I2C — verify the pin budget against the factory pinout, or use an I/O expander on the carrier board, or fall back to a split build.

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
| External watchdog out | DE-disable / power-cut to transceiver **+ relay-coil power cut (Case B, §7)** | **forces NO-DEMAND on ESP32 hang** — bus silence alone is not enough once relays exist |
| **DS18B20** data | ESP32 **GPIO13/14** | **4.7 kΩ pull-up to 3V3** |
| Status LED | non-strapping GPIO | not GPIO2 |

## 6. Component list (specs — links in [`ORDERING.md`](ORDERING.md))

- **Isolated AC/DC**, AC-input 9–36 VAC / ≤ 50 VDC, 5 V ≥ 1 A (~5 W).
- **Inline fuse + holder** 250–500 mA slow-blow; **MOV** ~40–47 VAC across R–C.
- **Bulk cap** 470–1000 µF **63 V** + 0.1 µF; **105 °C** electrolytics.
- **ESP32 dev board** — confirm PSRAM (affects GPIO16/17); brownout detector left enabled.
- **RS-485 transceiver** — 3.3 V protected: **THVD1410/THVD1450** or **MAX3485** (*not MAX485*). For full bus isolation: ADM2587E/ADM2682E or ISO3086T-class.
- **RS-485 line protection** — TVS array (SM712/SMBJ) A–B / A/B–GND; ~10 Ω series.
- **External hardware watchdog** — TPL5010 / MAX6369 class (or ATtiny) → forces NO-DEMAND on timeout (DE-cut + Case B relay-coil cut).
- **Temperature sensors** — DS18B20 (local fallback, 4.7 kΩ pull-up) + MQTT room sensor (primary); optional 2nd DS18B20 for voting. **Designate one of the existing waterproof DS18B20s as the outdoor sensor** (north wall, shaded, outdoor-rated cable run). 1-Wire tolerates long runs — an option for relocating the indoor fallback sensor into living space.
- **Display board (production HMI, §8)** — Guition JC4827W543C class ESP32-S3 touchscreen + wall mount + small transceiver/watchdog carrier board.
- **Case B hardware (gated on Phase 0, §7):**
  - Relay/optotriac output modules — 4–6 ch, **24 VAC-rated dry contacts**, normally-open (Y1, Y2, O/B, G + spare).
  - Watchdog-controlled relay-coil power cut (MOSFET/relay on the common coil feed).
  - 24 V opto-isolated AC sense modules — 4–6 ch (D/W defrost, Y/G/W monitoring).
  - **Condensate float switch** (wet-switch) — required whenever cooling is enabled, either path.
- **Enclosure** — UL94 V-0 / polycarbonate, screw terminals, glands, 105 °C-tolerant, mounted on the **cool side** of the furnace *(sniff rig / split-fallback controller; the production single-unit lives at the wall per §8, with the furnace-side box becoming a junction point or omitted)*.
- **CO alarm** in the dwelling; **OEM thermostat retained** for rollback.

## 7. HP command-path contingency — the design fork (Case A vs Case B)

How the heat pump is commanded is **not yet known**: it depends on which control architecture Phase 0 (installed-equipment inventory, see [`05-firmware-plan.md`](05-firmware-plan.md)) finds in the house. Everything in §§1–5 above is common to both cases; the items below are **gated on the Phase 0 answer** — do not order them before it.

**Case A — communicating (Dettson Alizé ODU + K03085 interface board in the furnace):** HP and cooling demands travel over CT-485 to the interface-board node. **The existing wiring design above survives intact** — no new output hardware. (The 24 V sense inputs below are still recommended as permanent instrumentation.)

**Case B — conventional/hybrid (true Gree FLEXX, 24 V ODU control):** gas modulation stays on CT-485 (or the V-signal, below), but the HP needs a 24 V output stage. ⚠ **Open question (Phase 2 test, gates this whole case):** whether the Chinook IFC honors hardwired Y/G/W *while in communicating mode*, or bus comms disable the 24 V terminals (the S3 cooling-CFM DIPs are documented as ignored in communicating mode) — the hybrid path is viable only if mixed mode works.

- **24 V relay output stage** — **Y1, Y2 (if the installed model supports two stages), O/B, G.** Dry contacts or optotriac, **normally-open, demand = energized**. ⚠ Gree reversing-valve convention is **B = energized in HEATING** — opposite the common O=cool default; getting this wrong inverts heat and cool.
- **Watchdog relay-coil cut** — the common relay-coil feed is **interrupted by the external hardware watchdog** (extends the §5 watchdog row beyond DE-disable). Rationale: "silence = safe" does **not** transfer to relays — a hung ESP32 with Y latched closed runs the compressor indefinitely.
- **24 V opto-isolated sense inputs** — the ODU's **D/W1 defrost output** (note: the thermostat W2/AUX conductor ties to the same furnace-W node, so the sense input must tolerate *either* source energizing that line), plus **Y/G/W terminal monitoring** as permanent instrumentation.
- **Condensate float switch** — wired to **break the cool call independently of software** (series in the Y circuit, not merely a GPIO input).
- **Optional third gas path** — the Chinook's **V/W2 terminal accepts a 24 V modulating signal** giving the full **40–100%** range with no CT-485 stack at all (R02P029/1F95M-class thermostat path). It exists as a fallback. ⚠ **Never apply raw 24 VAC to V/W2** — documented damage hazard.
- **Conductor count at the wall (Phase 0 task):** the production controller sits at the wall plate (§8). A wall unit can drive Y/G/O-B/W **only if enough conductors exist in the existing thermostat cable**. Phase 0 must **count conductors at the wall plate**; if there aren't enough, fall back to a split build (headless controller at the furnace/coil, wall display or HA dashboard over MQTT).

## 8. Production HMI — wall-mounted ESP32-S3 touchscreen

The production controller is a **single wall-mounted ESP32-S3 touchscreen board that is also the CT-485 controller**, installed at the OEM thermostat location (the wall plate already carries R/C/1/2 — 24 VAC power plus the bus). The DevKitC map in §3 remains the **Phase 1/2 sniff rig and spare**. Single-unit vs split is **gated by a Phase 3 bench test** of TX-turnaround jitter with LVGL + Wi-Fi + MQTT running (open question — see the HMI research notes).

- **Board — Guition JC4827W543C (primary pick).** 4.3" IPS 480×272, capacitive GT911, 8 MB PSRAM. Chosen specifically because its **NV3041A display is QSPI with internal GRAM**: the framebuffer is *not* continuously streamed from PSRAM, which sidesteps the documented RGB-parallel hazard (PSRAM contention → tearing/stalls during Wi-Fi/OTA/NVS activity). Flash caveat: the common N4R8 variant is **4 MB** — tight for dual OTA app partitions; prefer a 16 MB variant or plan the partition table carefully (verify when ordering).
- **Power budget.** Display boards of this class draw a verified **1.6–2.25 W**; with AC/DC conversion losses that is **≤ ~3 VA** from the 40 VA furnace transformer (an Ecobee3 measures ~1.4 VA peak — we're ~2×, still a small fraction of the transformer). The §1 fuse spec stands, but **backlight inrush may nuisance-trip a 250 mA fuse — be prepared to fit 500 mA** (still far below the furnace's control fuse).
- **External transceiver + watchdog are RETAINED — do not use onboard auto-direction RS-485.** The §2 transceiver (3.3 V, **explicit DE/RE**, pulled to receive/idle) and the §5 external hardware watchdog DE-cut move onto a **small carrier/daughterboard behind the display**, together with the isolated AC/DC, fuse/MOV, and TVS protection. A display board's built-in auto-direction RS-485 gives firmware no DE for the watchdog to cut and no guaranteed-silent idle — it fails the §2 requirements.
- **⚠ Waveshare ESP32-S3-Touch-LCD-4.3B red flag:** its onboard RS-485 sits on **GPIO43/44 = S3 UART0**, the pins that carry the boot-ROM log at every reset. Through an auto-direction transceiver, **every boot babbles 115200-baud noise onto the live furnace bus**, violating the silent-at-boot rule (§3). Mitigable via the UART print-control efuse, but a foot-gun — confirm RS-485 TXD = U0TXD before ever considering this board (open question).
- **S3 free-GPIO budget (verify before ordering):** the Guition exposes roughly ~10 free IO. **UART1 RX/TX + DE + 1-Wire (+ I2C for an optional SHT30/AHT20 temp/humidity sensor) must all fit** — confirm against the factory pinout. Avoid S3 strapping pins (GPIO0, 3, 45, 46) and the UART0 pins per the red flag above. Case B relay/sense pins on top of this may not fit — see §3/§7 (I/O expander or split build).
- **In-wall AC/DC (open code question):** Class-2 24 VAC in the wall cavity is generally permissible; **confirm local code treatment of the AC/DC converter module mounted in-wall** before finalizing the enclosure.
- **Re-run the §2 termination/bias measurement at the WALL plate.** The §2 measure-first rule was framed at the furnace tap; the OEM thermostat location **may have been a terminated bus end**. If our node physically becomes the bus end at the wall, the 120 Ω/bias answer differs — measure idle |V_AB| and A–B DC resistance *at the wall plate* before deciding.
