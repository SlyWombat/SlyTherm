# Dettson ClimateTalk Thermostat

A custom, **local-only** communicating thermostat for a **Dettson Chinook modulating gas furnace** (paired with a **Gree FLEXX** heat pump), built on an **ESP32 + RS-485** and integrated with **Home Assistant** over MQTT.

The furnace speaks the **ClimateTalk / CT-485** protocol over an RS-485 bus (terminals `R`, `C`, `1`, `2`). This project sniffs that bus, decodes the packets, and — only after the protocol is fully mapped — acts as a *replacement communicating thermostat* that requests heat capacity, enabling open-source PID control and HA integration without the OEM walled garden.

> ⚠️ **Safety first — read [`docs/04-safety.md`](docs/04-safety.md) before touching the furnace.**
> This is a **gas appliance**. We do **not** command the gas valve directly — we send a *heat demand* over CT-485, and the furnace's certified Integrated Furnace Control (IFC) retains **all** combustion safety interlocks (flame sense, high-limit, pressure/rollout switches, ignition lockout). The design's prime directive is **fail-to-no-heat**. Modifying a gas appliance can affect **warranty, code compliance, and insurance** — see the safety doc.

---

## Status

🟡 **Design / pre-build.** Documentation and BOM complete; hardware not yet ordered; no firmware written. Work is tracked in [GitHub Issues](../../issues) and grouped by phase milestone.

## How it works (one paragraph)

An ESP32 taps the furnace's RS-485 bus through a **3.3 V** RS-485 transceiver and listens (sniff-only, zero bus risk) to learn the CT-485 framing, node IDs, and — critically — *which message and byte carry the 0–100 % modulation demand*. Once the protocol dictionary is built and verified, the firmware impersonates the communicating thermostat: it runs a PID loop on a room-temperature input (HA/MQTT sensor with a local DS18B20 fallback) and issues `HEAT_DEMAND` messages at the right capacity, refreshing them within the protocol's demand watchdog. Power comes from the furnace's 24 VAC `R`/`C` via an **isolated AC/DC** supply. Layered watchdogs guarantee the system fails to **no heat demand** on any fault.

## Documentation

| Doc | What's in it |
| --- | --- |
| [`docs/01-architecture.md`](docs/01-architecture.md) | System overview, data flow, design decisions, scope |
| [`docs/02-protocol-climatetalk.md`](docs/02-protocol-climatetalk.md) | CT-485 deep dive: framing, Fletcher checksum, message/command IDs, token arbitration, sniffing methodology |
| [`docs/03-hardware-wiring.md`](docs/03-hardware-wiring.md) | Reviewed wiring, power, RS-485 interface, pin map, isolation |
| [`docs/04-safety.md`](docs/04-safety.md) | Functional-safety review, failure modes & failsafes, certification/legal honesty |
| [`docs/05-firmware-plan.md`](docs/05-firmware-plan.md) | Phased firmware plan, module layout, PlatformIO setup |
| [`docs/06-home-assistant.md`](docs/06-home-assistant.md) | MQTT discovery, HA climate entity, diagnostics |
| [`docs/ORDERING.md`](docs/ORDERING.md) | **Bill of materials with live purchasing links** and a minimum-viable cart |

## Quick start (current phase)

1. Read [`docs/04-safety.md`](docs/04-safety.md) and [`docs/03-hardware-wiring.md`](docs/03-hardware-wiring.md).
2. Order parts from [`docs/ORDERING.md`](docs/ORDERING.md) (minimum-viable cart ≈ CAD $180–200).
3. Build the **sniff-only** rig and capture the bus (Phase 1 — see [`docs/05-firmware-plan.md`](docs/05-firmware-plan.md)).
4. Track progress in [Issues](../../issues).

## Reference prior art

- [`kdschlosser/ClimateTalk`](https://github.com/kdschlosser/ClimateTalk) — Python protocol model (most complete message/command tables)
- [`kpishere/Net485`](https://github.com/kpishere/Net485) — C++ HVAC RS-485 (authoritative physical layer + Fletcher checksum + token state machine)
- [`esphome-econet`](https://github.com/esphome-econet/esphome-econet) — Rheem EcoNet (a ClimateTalk variant; useful ESPHome RS-485 plumbing reference)

## Tech stack

ESP32 (Arduino framework, PlatformIO) · RS-485 / CT-485 · MQTT · Home Assistant · PID control

---

*Part of the ElectricRV project family. `.env` (cPanel/MQTT/Wi-Fi secrets) is gitignored — see `.env.example`.*
