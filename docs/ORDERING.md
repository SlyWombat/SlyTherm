# ORDERING — Bill of Materials & Purchasing Links

What to order, with live buy links. User is in **Canada** — Amazon.ca / AliExpress preferred; DigiKey / Mouser for precision parts; Adafruit / SparkFun where relevant. Prices are approximate (mostly USD on generic listings) and drift weekly — **confirm at checkout.**

> **Read first:** [`03-hardware-wiring.md`](03-hardware-wiring.md) and [`04-safety.md`](04-safety.md). The two non-negotiable compatibility points: the RS-485 transceiver **must be 3.3 V-native** (a 5 V MAX485 over-volts the ESP32), and the power converter **must accept AC input** (24 VAC is AC, not DC).

---

## 1. ESP32 dev board

| Item | Recommended part | ~Price | Buy link |
| --- | --- | --- | --- |
| Primary | Espressif **ESP32-DevKitC-32E** (genuine WROOM-32E, micro-USB) | $15–18 | https://www.amazon.com/Espressif-ESP32-DevKitC-32E-Development-Board/dp/B09MQJWQN2 |
| Alt (USB-C, 2-pack) | AITRIP ESP-WROOM-32 DevKitC, USB-C, CH340C | ~$13/2pk | https://www.amazon.com/AITRIP-ESP-WROOM-32-Development-Bluetooth-ESP32-DevKitC-32/dp/B0B82BBKCY |
| Alt (DOIT V1) | DIYmall DOIT ESP32 DevKit V1, CP2102 | ~$10 | https://www.amazon.com/ESP32-WROOM-32-Development-ESP-32S-Bluetooth-forArduino/dp/B08PCPJ12M |

**Pick:** genuine **ESP32-DevKitC-32E** for a gas-appliance build. Note connector: genuine DevKitC-32E is **micro-USB**; AITRIP is **USB-C** — match your cable (§9). Confirm whether your board has **PSRAM** (frees/blocks GPIO16/17 — see wiring doc).

## 2. RS-485 transceiver (3.3 V)

| Item | Recommended part | ~Price | Buy link |
| --- | --- | --- | --- |
| Primary (auto-flow, 3.3 V) | **Teyleten TTL↔RS485 auto flow-control, 3.3–5.5 V** (5-pack) | ~$9/5pk | https://www.amazon.com/Teyleten-Robot-Conversion-Automatic-3-3V-5-5V/dp/B0CPFDJZXL |
| Alt (auto-dir + 120 Ω onboard) | Fafeicy TTL↔RS-485 auto-direction, 3.3/5 V | ~$7 | https://www.amazon.com/RS-485-Converter-Automatic-Direction-Control/dp/B0C6FHR5W7 |
| Alt (MAX3485, manual DE/RE) | KNACRO **MAX3485** 3.3 V module (2-pack) | ~$8/2pk | https://www.amazon.com/KNACRO-Conversion-Overvoltage-protectionWith-indicator/dp/B07V3JQRZB |
| Alt (premium SP3485) | SparkFun SP3485 breakout (3.3 V native) | ~$8 | https://www.amazon.com/SparkFun-Transceiver-Breakout-Half-Duplex-Interoperable/dp/B082MM58RF |
| Quality breakout | Waveshare RS485 Board (3.3 V), SP3485 | ~$6 | https://www.waveshare.com/rs485-board-3.3v.htm |
| Bare IC (precision) | MaxLinear **SP3485** (DigiKey) | ~$1.50 | https://www.digikey.com/en/products/base-product/maxlinear-inc/1016/SP3485/335340 |

**Pick:** start with the **Teyleten auto-flow 3.3 V** module (no DE/RE GPIO). Keep a **MAX3485 manual** module as fallback — if you see framing errors when sniffing, switch and drive DE/RE from GPIO4. **Never** substitute a 5 V-only MAX485 as primary. *(For full bus isolation instead: ADI ADM2587E/ADM2682E or TI ISO3086T-class — DigiKey/Mouser.)*

## 3. Power (24 VAC → 5 V + protection)

| Item | Recommended part | ~Price | Buy link |
| --- | --- | --- | --- |
| 24 VAC→5 V (AC-input, potted) | **Fafeicy AC 12/24 V → DC 5 V, 2 A, IP67** | ~$10 | https://www.amazon.com/Fafeicy-Converter-AC-DC-Supply-Module/dp/B08CC74SDQ |
| 24 VAC→5 V (alt, housed) | AC 12/24 V → 5 VDC 2 A, flame-retardant housing | ~$9 | https://www.amazon.com/Converter-Retardant-Plastic-Housing-Reliable/dp/B0CTXX687S |
| 24 VAC→5 V (AliExpress) | Isolated 24 VAC/DC → 5 V modules (pick potted/2 A) | ~$3–6 | https://www.aliexpress.com/w/wholesale-isolated-dc-dc-converter-24v-5v.html |
| 3.3 V rail | ESP32 onboard AMS1117 supplies 3.3 V — **no separate LDO needed** | $0 | (onboard) |
| Inline fuse | 5×20 mm holder + **0.25–0.5 A slow-blow** on the 24 VAC input | ~$7 | https://www.amazon.com/24vac-power-supply/s?k=24vac+power+supply |
| MOV (AC side) | 24 VAC-rated MOV (~39–47 V working, Littelfuse V-series) across R↔C | ~$1 | https://www.digikey.com/en/products/filter/varistors/142 |

**Pick:** **Fafeicy AC→5 V potted** module. ⚠️ Cheap AC-input modules are often **non-isolated/transformerless** — for a gas appliance, **fully enclose** it (§8), **fuse the hot side**, and add the **MOV across R/C**. For true galvanic isolation, put a small 24 VAC→9 VAC class-2 transformer ahead of a standard AC-DC, or use a DIN-rail isolated 24 VAC→5 V supply. See [`03-hardware-wiring.md`](03-hardware-wiring.md) §1/§4.

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

**Pick:** an **SM712** (designed for RS-485 ±7/±12 V) or an SMBJ pair on A/B to GND, plus ~10 Ω series resistors into the transceiver. Cheap insurance on a gas appliance.

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

## Nice-to-have

- **Comidox 24 MHz logic analyzer** (~$12) — effectively mandatory in practice for debugging CT-485 framing.
- **FTDI USB↔RS485 adapter** (~$13–20) — passive PC-side monitoring / reverse-engineering.
- **Spare MAX3485 manual module** (~$8) — fallback if auto-direction clips bits.
- **Polycarbonate enclosure** upgrade if mounting close to furnace heat.
- **Isolated supply path** (24 VAC→9 VAC transformer, or ADM2587E isolated transceiver) for belt-and-suspenders safety.
- **CO alarm** for the dwelling — non-negotiable for running this (see [`04-safety.md`](04-safety.md)).

---

### Buying cautions

- **Transceiver logic level is the #1 risk** — confirmed 3.3 V auto-flow as primary; do **not** substitute a 5 V MAX485.
- **AC-DC isolation:** cheap AC-input modules are usually non-isolated — enclose fully, fuse the hot side, MOV across R/C; prefer a truly isolated supply for a gas appliance.
- **Enclosure temperature:** ABS ~80 °C — mount away from burner heat or use polycarbonate.
- Prices are approximate and mostly USD on generic listings; Amazon.ca equivalents exist for most and land cheaper after shipping/duty.
