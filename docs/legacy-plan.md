# LEGACY — Original Project Plan (superseded)

> ⚠️ **This is the original, pre-review plan, kept for historical reference only. Do not build from it.**
> It is heat-only, names discontinued hardware (R02P030, MAX485, 24 VAC buck), says "command the gas valve" (we send a *demand*), and predates the 2026-06 design review that re-scoped the project to a **dual-fuel heat pump + AC system with Ecobee-class features** (touchscreen wall unit, remote sensors, mobile app via Home Assistant).
> Current design: [`../README.md`](../README.md) and `docs/01`–`06`. Known corrections to this plan are catalogued in [`03-hardware-wiring.md`](03-hardware-wiring.md) §0 and the design-review changes throughout the docs.

# Project Plan: Custom ClimateTalk Thermostat Interface (Dettson Modulating Furnace)
## 1. Project Context & Objective
The goal of this project is to build a custom, local-only smart thermostat controller for a Dettson Chinook Modulating Gas Furnace (paired with a Gree FLEXX heat pump). 
The manufacturer requires a proprietary thermostat (R02P030) to achieve true 1% increment gas valve modulation. However, the furnace communicates using the **ClimateTalk** protocol over an **RS-485** serial bus. 
We will use an ESP32 microcontroller and an RS-485 transceiver to sniff the ClimateTalk bus, reverse-engineer the modulation command packets, and write custom firmware to act as the primary communicating thermostat. This will allow for advanced, open-source PID loop control and seamless integration with Home Assistant, completely bypassing the manufacturer's walled garden.
## 2. Hardware Architecture
### Components Required
*   **Microcontroller:** ESP32 Development Board (e.g., NodeMCU-32S). Chosen for built-in Wi-Fi and ample processing power for PID loops and MQTT/API handling.
*   **Transceiver:** MAX485 (or equivalent RS485-to-TTL module). *Note: An auto-direction (auto-TX/RX) RS-485 module is highly recommended to avoid manual DE/RE pin timing.*
*   **Power Supply:** 24VAC to 5VDC/3.3VDC Buck Converter. (To power the ESP32 directly from the furnace's `R` and `C` terminals).
### Wiring Schematic (Furnace to ESP32)
The Dettson control board features a 4-wire communicating terminal block: `R`, `C`, `1`, `2`.

| Dettson Board | Function | Hardware Interface | ESP32 Pin Mapping |
| :--- | :--- | :--- | :--- |
| **R** | 24VAC Power | Buck Converter IN (+) | N/A |
| **C** | 24VAC Common | Buck Converter IN (-) | N/A |
| **1** | RS-485 Data A (+) | MAX485 `A` Terminal | N/A |
| **2** | RS-485 Data B (-) | MAX485 `B` Terminal | N/A |
| N/A | 3.3V / 5V VCC | Buck Converter OUT (+) | ESP32 `VIN` / `3V3` & MAX485 `VCC` |
| N/A | DC Ground | Buck Converter OUT (-) | ESP32 `GND` & MAX485 `GND` |
| N/A | UART Receive | MAX485 `RO` (RX Out) | ESP32 `RX2` (GPIO 16) |
| N/A | UART Transmit | MAX485 `DI` (Data In) | ESP32 `TX2` (GPIO 17) |
| N/A | TX/RX Control | MAX485 `DE` / `RE` | ESP32 `GPIO 4` (If not using auto-direction) |

## 3. The Protocol: ClimateTalk
ClimateTalk is an OSI-model based protocol. We will be interacting at the physical layer (RS-485) and decoding the application layer (Layer 7).
*   **Baud Rate:** Typically 9600 or 38400 bps (to be verified during the sniffing phase).
*   **Packet Structure:** ClimateTalk packets generally consist of a preamble, source node ID, destination node ID, message class, message ID, payload length, payload, and a CRC/checksum.
*   **Prior Art/References:** The protocol has been partially reverse-engineered by the open-source community. 
    *   Reference Repo 1: `kdschlosser/ClimateTalk` (Python bindings and protocol structures).
    *   Reference Repo 2: `esphome-econet` (Rheem's EcoNet is a modified version of ClimateTalk; packet structure is highly similar).
    *   Reference Repo 3: `kpishere/Net485` (C++ implementation for HVAC RS485).
## 4. Development Phases
### Phase 1: Passive Bus Sniffing & Logging
**Objective:** Listen to the furnace without transmitting, identify the baud rate, and dump raw hex packets.
*   **LLM Task 1:** Write an ESP32 Arduino/C++ sketch that initializes `HardwareSerial`, listens to `RX2`, formats incoming bytes into HEX strings, and outputs them over Wi-Fi (via WebSockets or MQTT) to a PC for analysis.
*   *Constraint:* Ensure non-blocking serial reads to avoid dropping frames on the RS-485 bus.
### Phase 2: Packet Decoding & Node Identification
**Objective:** Parse the raw hex dumps into ClimateTalk packet structures.
*   **LLM Task 2:** Write a C++ parser class based on known ClimateTalk OSI Layer 7 structures. 
*   **Goals:** 
    1. Identify the Furnace Node ID (usually `0x20` or similar).
    2. Identify the Thermostat Node ID (if one is temporarily connected to sniff handshake traffic).
    3. Decode the "Status Broadcast" packets (which will contain current fan speed, fault codes, and gas valve modulation percentage).
### Phase 3: Active Transmission (The "Virtual Thermostat")
**Objective:** Program the ESP32 to mimic a ClimateTalk thermostat and take control of the bus.
*   **LLM Task 3:** Implement the RS-485 TX logic. Implement token arbitration (ClimateTalk requires devices to wait for a token before transmitting).
*   **Goals:**
    1. Send the "Network Join" handshake.
    2. Transmit a successful "Set Modulation" command (e.g., commanding the gas valve to 45%).
### Phase 4: Control Logic & Home Assistant Integration
**Objective:** Build the PID loop and expose the thermostat to Home Assistant.
*   **LLM Task 4:** Implement a PID controller in C++. 
    *   *Input:* Current room temperature (via external MQTT sensor or local DS18B20).
    *   *Setpoint:* Target temperature set by the user.
    *   *Output:* 0-100% modulation command sent over the RS-485 bus to the furnace.
*   **LLM Task 5:** Wrap the application in MQTT discovery protocols so Home Assistant automatically recognizes the ESP32 as an HVAC Climate entity.
## 5. Instructions for the Coding LLM
When acting upon this document, adhere to the following rules:
1.  **Framework:** Default to the Arduino framework for ESP32 (PlatformIO preferred).
2.  **Modularity:** Keep the RS-485 hardware serial interface, the ClimateTalk parser, and the PID logic in separate `.cpp`/`.h` files.
3.  **Safety First:** The Dettson furnace is a gas appliance. Include watchdog timers and fail-safes. If the ESP32 loses Wi-Fi or temperature sensor data, it MUST immediately transmit a 0% modulation (OFF) command to the furnace to prevent runaway heating.
4.  **First Action:** Acknowledge receipt of this plan, confirm your understanding of the RS-485 wiring, and immediately provide the code for **Phase 1: Passive Bus Sniffing & Logging**.