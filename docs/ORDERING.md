# ORDERING — Bill of Materials & Purchasing Links

What to order, with live buy links. User is in **Canada** — Amazon.ca / AliExpress preferred; DigiKey / Mouser for precision parts; Adafruit / SparkFun where relevant. Prices are approximate (mostly USD on generic listings) and drift weekly — **confirm at checkout.**

> **Read first:** [`03-hardware-wiring.md`](03-hardware-wiring.md) and [`04-safety.md`](04-safety.md). The two non-negotiable compatibility points: the RS-485 transceiver **must be 3.3 V-native** (a 5 V MAX485 over-volts the ESP32), and the power converter **must accept AC input** (24 VAC is AC, not DC).

> ⚠️ **Phase 0 gate:** order the **sniff-rig cart (§1–§10) now**; everything else — the wall touchscreen (§11) and the dual-fuel/Case B items (§12) — is **gated on the Phase 0 installed-equipment inventory** (see [`05-firmware-plan.md`](05-firmware-plan.md)). Which architecture is actually installed (communicating Alizé+interface-board vs conventional 24 V FLEXX) decides which of those parts you need at all.

---

## 1. ESP32 dev board (Phase 1/2 sniff rig + spare)

| Item | Recommended part | ~Price | Buy link |
| --- | --- | --- | --- |
| Primary | Espressif **ESP32-DevKitC-32E** (genuine WROOM-32E, micro-USB) | $15–18 | https://www.amazon.com/Espressif-ESP32-DevKitC-32E-Development-Board/dp/B09MQJWQN2 |
| Alt (USB-C, 2-pack) | AITRIP ESP-WROOM-32 DevKitC, USB-C, CH340C | ~$13/2pk | https://www.amazon.com/AITRIP-ESP-WROOM-32-Development-Bluetooth-ESP32-DevKitC-32/dp/B0B82BBKCY |
| Alt (DOIT V1) | DIYmall DOIT ESP32 DevKit V1, CP2102 | ~$10 | https://www.amazon.com/ESP32-WROOM-32-Development-ESP-32S-Bluetooth-forArduino/dp/B08PCPJ12M |

**Pick:** genuine **ESP32-DevKitC-32E**. Note connector: genuine DevKitC-32E is **micro-USB**; AITRIP is **USB-C** — match your cable (§9). Confirm whether your board has **PSRAM** (frees/blocks GPIO16/17 — see wiring doc).

**Role change:** the DevKitC is **no longer the production controller** — it is the Phase 1/2 **sniff rig and long-term spare** (keep it). The production controller is the wall-mounted ESP32-S3 touchscreen in **§11**. Single-unit vs split is gated by a Phase 3 bench test of TX-turnaround jitter with LVGL + Wi-Fi running; if split is chosen later, the DevKitC BOM is unchanged.

## 2. RS-485 transceiver (3.3 V)

| Item | Recommended part | ~Price | Buy link |
| --- | --- | --- | --- |
| Primary (auto-flow, 3.3 V) | **Teyleten TTL↔RS485 auto flow-control, 3.3–5.5 V** (5-pack) | ~$9/5pk | https://www.amazon.com/Teyleten-Robot-Conversion-Automatic-3-3V-5-5V/dp/B0CPFDJZXL |
| Alt (auto-dir + 120 Ω onboard) | Fafeicy TTL↔RS-485 auto-direction, 3.3/5 V | ~$7 | https://www.amazon.com/RS-485-Converter-Automatic-Direction-Control/dp/B0C6FHR5W7 |
| Alt (MAX3485, manual DE/RE) | KNACRO **MAX3485** 3.3 V module (2-pack) | ~$8/2pk | https://www.amazon.com/KNACRO-Conversion-Overvoltage-protectionWith-indicator/dp/B07V3JQRZB |
| Alt (premium SP3485) | SparkFun SP3485 breakout (3.3 V native) | ~$8 | https://www.amazon.com/SparkFun-Transceiver-Breakout-Half-Duplex-Interoperable/dp/B082MM58RF |
| Quality breakout | Waveshare RS485 Board (3.3 V), SP3485 | ~$6 | https://www.waveshare.com/rs485-board-3.3v.htm |
| Bare IC (precision) | MaxLinear **SP3485** (DigiKey) | ~$1.50 | https://www.digikey.com/en/products/base-product/maxlinear-inc/1016/SP3485/335340 |

**Pick:** the **Teyleten auto-flow 3.3 V** module is for the **RX-only sniff rig** (Phases 1–2) only. The **production wall unit requires a manual-DE/RE part** (MAX3485/SP3485, or THVD14xx driven manually) — the external hardware watchdog must be able to force DE off, which an auto-direction module gives no pin for (see [`03-hardware-wiring.md`](03-hardware-wiring.md) §2/§8). **Never** substitute a 5 V-only MAX485 anywhere. *(For full bus isolation instead: ADI ADM2587E/ADM2682E or TI ISO3086T-class — DigiKey/Mouser.)*

## 3. Power (24 VAC → 5 V + protection)

| Item | Recommended part | ~Price | Buy link |
| --- | --- | --- | --- |
| 24 VAC→5 V (AC-input, potted) | **Fafeicy AC 12/24 V → DC 5 V, 2 A, IP67** | ~$10 | https://www.amazon.com/Fafeicy-Converter-AC-DC-Supply-Module/dp/B08CC74SDQ |
| 24 VAC→5 V (alt, housed) | AC 12/24 V → 5 VDC 2 A, flame-retardant housing | ~$9 | https://www.amazon.com/Converter-Retardant-Plastic-Housing-Reliable/dp/B0CTXX687S |
| 24 VAC→5 V (AliExpress) | Isolated 24 VAC/DC → 5 V modules (pick potted/2 A) | ~$3–6 | https://www.aliexpress.com/w/wholesale-isolated-dc-dc-converter-24v-5v.html |
| 3.3 V rail | ESP32 onboard AMS1117 supplies 3.3 V — **no separate LDO needed** | $0 | (onboard) |
| Inline fuse | 5×20 mm holder + **0.25–0.5 A slow-blow** on the 24 VAC input | ~$7 | https://www.amazon.com/24vac-power-supply/s?k=24vac+power+supply |
| MOV (AC side) | 24 VAC-rated MOV (~39–47 V working, Littelfuse V-series) across R↔C | ~$1 | https://www.digikey.com/en/products/filter/varistors/142 |

**Pick:** **Fafeicy AC→5 V potted** module **for the bench/sniff rig**. ⚠️ Cheap AC-input modules are often **non-isolated/transformerless** — the **production wall build requires a confirmed-isolated supply** (docs/03 §1 mandates isolation, and the module now lives in the wall cavity behind the display — see [`03-hardware-wiring.md`](03-hardware-wiring.md) §8, including the in-wall code question). Whatever you use: **fully enclose** it, **fuse the hot side**, and add the **MOV across R/C**. For true galvanic isolation, put a small 24 VAC→9 VAC class-2 transformer ahead of a standard AC-DC, or use a DIN-rail isolated 24 VAC→5 V supply. See [`03-hardware-wiring.md`](03-hardware-wiring.md) §1/§4.

## 4. Temperature sensor

| Item | Recommended part | ~Price | Buy link |
| --- | --- | --- | --- |
| DS18B20 + 4.7 k resistors | Keyestudio 3× waterproof DS18B20 **+ 4.7 k**, 100 cm | ~$11 | https://www.amazon.com/KEYESTUDIO-Ds18b20-Temperature-Waterproof-Resistor/dp/B07DVJ1JHP |
| Alt (5-pack + resistors) | WWZMDiB 5× DS18B20 + 5× 4.7 k | ~$13 | https://www.amazon.com/WWZMDiB-Temperature-High-Accuracy-Waterproof-Experiments/dp/B0C8J77NJR |
| Premium single | Adafruit waterproof DS18B20 | ~$10 | https://www.adafruit.com/product/381 |
| Amazon.ca | DS18B20 search | varies | https://www.amazon.ca/ds18b20-temperature-sensor/s?k=ds18b20+temperature+sensor |

**Pick:** **Keyestudio kit** (includes the 4.7 kΩ pull-up). Better: use an **existing HA room sensor** as the primary PID input and the DS18B20 as the local fallback (a wall-location sensor reflects comfort better than one near the furnace).

## 5. Termination / biasing

| Item | Recommended part | ~Price | Buy link |
| --- | --- | --- | --- |
| Resistor kit (incl. 120 Ω) | 1/4 W metal-film assortment | ~$10/kit | https://www.amazon.com/dp/B07L851T3V |
| Precision 120 Ω 1% | DigiKey through-hole | ~$0.10 | https://www.digikey.com/en/products/filter/through-hole-resistors/53 |

**Note:** terminate **only at the two physical bus ends**, and add bias **only if measurement shows the bus floats**. You are tapping an already-terminated bus — see [`03-hardware-wiring.md`](03-hardware-wiring.md) §2. **Measure before adding anything.**

## 6. Protection (RS-485 ESD / overcurrent)

| Item | Recommended part | ~Price | Buy link |
| --- | --- | --- | --- |
| RS-485 TVS | Littelfuse **SMBJ15A** (600 W) across A/B–GND | ~$0.40 | https://www.amazon.com/LITTELFUSE-SMBJ15A-Protection-tvs-diodes-Uni-Directional/dp/B073466PYW |
| TVS (purpose-built) | **SM712** RS-485 protector (DigiKey) | ~$0.50 | https://www.digikey.com/en/products/filter/tvs-diodes/144 |
| Resettable fuse (PTC) | SparkFun PTC (or DigiKey 0.5–1 A PPTC) | ~$0.95 | https://www.sparkfun.com/resettable-fuse-ptc.html |
| External HW watchdog | **TPL5010 / MAX6369-class** (or ATtiny) — on timeout forces NO-DEMAND: DE-disable + relay-coil power cut (Case B); see [`03-hardware-wiring.md`](03-hardware-wiring.md) §5 | ~$3–7 | https://www.digikey.com/en/products/filter/pmic-supervisors/591 |

**Pick:** an **SM712** (designed for RS-485 ±7/±12 V) or an SMBJ pair on A/B to GND, plus ~10 Ω series resistors into the transceiver. Cheap insurance on a gas appliance. The **external watchdog is mandatory for any TX-capable build** (Phase 3+), not for the RX-only sniff rig.

## 7. Wiring / connectors

| Item | Recommended part | ~Price | Buy link |
| --- | --- | --- | --- |
| Pluggable screw terminals | DBParts 5.08 mm PCB terminal set | ~$10 | https://www.amazon.com/DBParts-5-08mm-Straight-Terminal-Connector/dp/B07SRM36LK |
| Dupont jumpers | M-M / M-F / F-F assortment | ~$7 | https://www.amazon.com/dupont-connectors/s?k=dupont+connectors |
| 22 AWG wire | Stranded hookup wire (multi-color) | ~$15 CAD | https://www.amazon.ca/s?k=22awg+stranded+hookup+wire |
| Ferrules + crimper | Ferrule kit + crimp tool | ~$25 CAD | https://www.amazon.ca/s?k=wire+ferrule+crimper+kit |
| Heat shrink | Assorted kit | ~$12 CAD | https://www.amazon.ca/s?k=heat+shrink+tubing+kit |

**Note:** use **ferrules on all stranded conductors** into screw terminals (thermal cycling near a furnace loosens bare strands). Keep RS-485 A/B as a twisted pair.

## 8. Enclosure

| Item | Recommended part | ~Price | Buy link |
| --- | --- | --- | --- |
| DIN/junction box | Joisyoya IP67 enclosure + DIN rail, 5.9×3.9×2.8" | ~$18 | https://www.amazon.com/Joisyoya-Electrical-Waterproof-Weatherproof-Electronics/dp/B0D9CR3MDH |
| ABS project box (alt) | ABS box assortment | ~$12 | https://www.amazon.com/abs-project-box/s?k=abs+project+box |
| Polycarbonate (hot mount) | Polycase DIN-rail (~115–125 °C, flame-retardant) | varies | https://www.polycase.com/din-rail-enclosures |

**Note:** ABS is ~80 °C continuous; furnace cabinets exceed that near the burner. **Mount on the cool side / outside the cabinet**, or use polycarbonate if close to heat. Enclose the AC-DC module fully and strain-relieve all wires.

**Role change:** with the production controller now a **wall-mounted touchscreen (§11)**, there is no controller at the furnace — the Joisyoya box is **optional**: keep it as a junction/instrumentation point at the furnace (or for Case B relay/sense hardware, §12), or skip it. The wall unit gets its own enclosure (§11). Still fine for the sniff rig.

## 9. Prototyping

| Item | Recommended part | ~Price | Buy link |
| --- | --- | --- | --- |
| Breadboard | 830-point solderless | ~$12 CAD | https://www.amazon.ca/s?k=830+point+breadboard |
| USB cable (micro) | Micro-USB **data** cable (genuine DevKitC-32E) | ~$8 CAD | https://www.amazon.ca/s?k=micro+usb+data+cable |
| USB cable (Type-C) | USB-C **data** cable (if Type-C board) | ~$8 CAD | https://www.amazon.ca/s?k=usb+c+data+cable |
| Logic analyzer (budget) | Comidox 24 MHz 8-ch USB (sigrok/PulseView) | ~$12 | https://www.amazon.com/Comidox-Analyzer-Device-Channel-Arduino/dp/B07KW445DJ |
| Logic analyzer (premium) | Saleae Logic 8 | ~$650 CAD | https://www.amazon.ca/Logic-Black-Compatible-Ultra-Portable-Frustration/dp/B0749G85W2 |

**Note:** the **$12 Comidox 24 MHz 8-ch** is plenty to sniff CT-485 framing in PulseView (UART decoder on the transceiver's TTL side). Confirm your USB cable is a **data** cable, not charge-only.

## 10. Optional — PC-side RS-485 sniffer (cross-check)

| Item | Recommended part | ~Price | Buy link |
| --- | --- | --- | --- |
| USB↔RS-485 (FTDI) | FTDI USB↔RS485, 3.3/5 V, screw terminals | ~$13 | https://www.amazon.com/Converter-Adapter-Terminals-Windows-Support/dp/B07SD65BVF |
| USB↔RS-485 (industrial) | FT232RL + SP485EEN, fused + ESD | ~$20 | https://www.amazon.com/Industrial-USB-RS485-Converter-Communication/dp/B081MB6PN2 |
| USB↔RS-485 (genuine FTDI cable) | FTDI USB-RS485-WE-1800-BT | ~$35 | https://www.amazon.com/Brand-New-USB-RS485-WE-1800-BT-RS485-Cable/dp/B01HI9CMT2 |

A **genuine-FTDI USB↔RS485** lets you passively monitor the bus from a laptop as an independent cross-check before the ESP32 ever transmits onto a live gas-furnace bus.

## 11. Production controller — wall-mounted ESP32-S3 touchscreen

The production controller is a **single wall-mounted ESP32-S3 touchscreen** at the OEM thermostat location (R/C/1/2 are already there). The external 3.3 V transceiver (manual DE/RE), isolated AC-DC, fuse + MOV, TVS, DS18B20 and the **external hardware watchdog** from §2–§6 are all **still required at the wall** — the display board's onboard peripherals do not replace them.

| Item | Recommended part | ~Price | Buy link |
| --- | --- | --- | --- |
| Primary | **Guition JC4827W543C** — 4.3" IPS 480×272, capacitive GT911, **QSPI NV3041A** (internal GRAM, no PSRAM-streaming hazard), 8 MB PSRAM | ~US$13–18 / ~CA$20–28 | https://www.aliexpress.com/w/wholesale-JC4827W543C.html |
| Alt | Waveshare **ESP32-S3-Touch-LCD-4.3B** — 800×480 RGB, onboard RS-485 + isolated DI/DO, case available | ~US$33–39 / Amazon.ca ~CA$50–60 | https://www.waveshare.com/esp32-s3-touch-lcd-4.3b.htm |
| Wall mount | 3D-printed enclosure (models on Printables/MakerWorld) + **low-voltage old-work bracket**; must house the AC-DC + transceiver behind the display | ~CA$5–15 | https://www.amazon.ca/s?k=low+voltage+old+work+bracket |
| Optional | **SHT30 or AHT20** I2C temp+humidity module on the display I2C header (Ecobee-style humidity display; DS18B20 stays the safety fallback) | ~CA$5–10 | https://www.amazon.ca/s?k=sht30+i2c+module |

**Cautions:**
- **Guition flash variant:** common JC4827W543 stock is **N4R8 = 4 MB flash** — tight for LVGL + protocol stack + OTA dual app partitions. **Prefer a 16 MB variant or plan a careful partition table — verify when ordering.** Also confirm the free-GPIO map fits UART1 TX/RX + DE + 1-Wire (+ I2C) — factory pinout: https://github.com/lsdlsd88/JC4827W543
- **Waveshare 4.3B boot-babble warning:** its onboard RS-485 sits on **GPIO43/44 = UART0**, which carries the boot-ROM log at reset — with auto-direction RS-485 every boot **babbles 115200-baud noise onto the live furnace bus**, violating the silent-at-boot rule. Mitigable (print-control efuse) but a foot-gun; if chosen, use the **external** DE/RE transceiver anyway so the hardware watchdog can cut DE.
- Buy **capacitive** touch ("C" suffix), not resistive.

## 12. Phase 0-gated additions (dual-fuel / cooling build)

**Do not order these until the Phase 0 installed-equipment inventory** determines the architecture (Case A communicating vs Case B conventional/hybrid — see [`03-hardware-wiring.md`](03-hardware-wiring.md)).

| Item | When | ~Price | Buy link |
| --- | --- | --- | --- |
| **Dettson R02P034 communicating thermostat** | strongly recommended either path — reference bus master for Phases 1–2 + certified rollback device | ~CA$200–300 | Dettson distributor (e.g. Mid-State Supply) — https://www.dettson.com |
| Relay/optotriac output board (4–6 ch, **24 VAC-rated dry contacts**: Y1, Y2, O/B, G + spare) | **Case B** | ~$10–20 | https://www.amazon.ca/s?k=opto+isolated+relay+module+board |
| Watchdog-controlled relay-coil power cut (MOSFET/relay on the common coil feed) | **Case B** | ~$5 | https://www.amazon.ca/s?k=mosfet+switch+module |
| 24 V opto-isolated AC **sense inputs** (D/W defrost, Y/G/W monitoring; 4–6 ch) | **Case B** (recommended as permanent instrumentation either path) | ~$10 | https://www.amazon.ca/s?k=24v+ac+optocoupler+isolation+module |
| Condensate **float switch** (wet-switch) — hardwired to break the cool call independent of software | cooling enabled, either path | ~$15–25 | https://www.amazon.ca/s?k=condensate+overflow+float+switch |
| Outdoor temp sensor | **no new purchase** — designate one waterproof DS18B20 from the §4 3-pack (north wall, shaded) + outdoor-rated cable run | ~$5 cable | https://www.amazon.ca/s?k=outdoor+rated+22awg+cable |
| Optional supply-air DS18B20 (coil-freeze guard) | cooling | from same 3-pack | (§4) |
| Remote room sensors: 2–4× Zigbee temp (**SONOFF SNZB-02D** ~$15 ea) + occupancy (**Aqara FP2** ~$80 or **Apollo MSR-2** ~$35/room) | Ecobee-class features, via HA→MQTT | ~$50–200 | https://www.amazon.ca/s?k=sonoff+snzb-02d |
| K03085 interface kit | contingent — only if converting an Alizé system to communicating; **confirm compatibility with Dettson first** (capacity DIP table stops at 36k BTU) | TBD | Dettson distributor |

**Note:** R02P030 named in earlier docs is **discontinued**; its current successors are **R02P032/R02P034** — the R02P034 is the behavior spec the firmware clones and the recommended rollback device. Do **not** buy Ecobee room sensors for this project — their 915 MHz protocol is proprietary and unreceivable by an ESP32.

---

## Minimum-viable shopping cart (essentials)

| # | Item | Part | ~Price |
| --- | --- | --- | --- |
| 1 | ESP32 | ESP32-DevKitC-32E (genuine) | $16 |
| 2 | RS-485 transceiver | Teyleten auto-flow 3.3 V (5-pk) | $9 |
| 3 | Power | Fafeicy AC 12/24 V→5 V 2 A potted | $10 |
| 4 | Temp sensor | Keyestudio DS18B20 + 4.7 k (3-pk) | $11 |
| 5 | Fuse + MOV/TVS | 5×20 holder + SMBJ/SM712 | $9 |
| 6 | Termination/bias | resistor assortment (120 Ω etc.) | $10 |
| 7 | Connectors/wire | terminals + Dupont + 22 AWG + ferrules | $35 |
| 8 | Enclosure | Joisyoya IP67 DIN box | $18 |
| 9 | Breadboard + USB cable | 830-pt + data cable | $20 |

**Rough MVP total: ~$135–145 (mixed USD/CAD) ≈ ~$180–200 CAD landed.**

This MVP cart is the **sniff rig** and is **unchanged** by the re-scope — order it now. The full dual-fuel build adds, after Phase 0:

| Delta | Items | ~Cost |
| --- | --- | --- |
| Wall display path (§11) | Guition JC4827W543C + wall mount + optional SHT30 | **≈ +CA$30–55** (Waveshare alt: +CA$60–85) |
| Case B hardware (§12) | relay board + watchdog coil-cut + sense inputs + float switch | **≈ +CA$50–100** |
| Remote sensors (§12) | Zigbee temp + occupancy | ~CA$50–200 |
| Optional reference/rollback (§12) | Dettson R02P034 | ~CA$200–300 |

## Nice-to-have

- **Comidox 24 MHz logic analyzer** (~$12) — effectively mandatory in practice for debugging CT-485 framing.
- **FTDI USB↔RS485 adapter** (~$13–20) — passive PC-side monitoring / reverse-engineering.
- **Spare MAX3485 manual module** (~$8) — fallback if auto-direction clips bits.
- **Polycarbonate enclosure** upgrade if mounting close to furnace heat.
- **Isolated supply path** (24 VAC→9 VAC transformer, or ADM2587E isolated transceiver) for belt-and-suspenders safety.
- **CO alarm** for the dwelling — non-negotiable for running this (see [`04-safety.md`](04-safety.md)).

---

### Buying cautions

- **Phase 0 first:** do not order §11/§12 items before the installed-equipment inventory — Case A vs Case B changes what you need (and whether a K03085 path even exists).
- **Transceiver logic level is the #1 risk** — confirmed 3.3 V auto-flow as primary; do **not** substitute a 5 V MAX485.
- **Display-board variants:** confirm the Guition flash size (4 MB N4R8 vs 16 MB) and capacitive touch before checkout; if buying the Waveshare 4.3B, check the UART0 boot-babble caveat (§11).
- **AC-DC isolation:** cheap AC-input modules are usually non-isolated — enclose fully, fuse the hot side, MOV across R/C; prefer a truly isolated supply for a gas appliance.
- **Enclosure temperature:** ABS ~80 °C — mount away from burner heat or use polycarbonate.
- Prices are approximate and mostly USD on generic listings; Amazon.ca equivalents exist for most and land cheaper after shipping/duty.
