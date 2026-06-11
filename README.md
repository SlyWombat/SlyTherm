# Dettson ClimateTalk Thermostat

A custom, **local-only**, Ecobee-class dual-fuel thermostat controlling **gas heat, heat-pump heat, and cooling** for a **Dettson Chinook modulating gas furnace** paired with a **Gree-built heat pump**, with balance-point changeover between fuels. The controller is a **wall-mounted ESP32-S3 touchscreen** unit at the OEM thermostat location, integrated with **Home Assistant** over MQTT.

The furnace speaks the **ClimateTalk / CT-485** protocol over an RS-485 bus (terminals `R`, `C`, `1`, `2`). This project sniffs that bus, decodes the packets, and — only after the protocol is fully mapped — acts as a *replacement communicating thermostat* that requests heat capacity (40–100 % modulation), enabling open-source supervisory control and HA integration alongside the OEM path (the current OEM stats are R02P032/R02P034 — the older R02P030 named in early notes is discontinued; an R02P034 is the recommended reference and rollback device). **The heat pump is not assumed to be on the CT-485 bus**: a true Gree FLEXX is commanded by conventional 24 V signals (Y1/Y2/B/G + D defrost sense), and CT-485 carries HP/cooling demands only in the Dettson Alizé + K03085 interface-board configuration — which architecture is installed is determined by the Phase 0 inventory (see Status).

**Target features (Ecobee-class):**

- Modes **OFF / HEAT / COOL / AUTO / EMERGENCY HEAT** with dual setpoints and auto-changeover deadband
- **Dual-fuel balance-point** logic (HP vs gas, with lockouts and escalation) and compressor minimum-timer protection
- **Wall touchscreen UI** (ESP32-S3 + LVGL) at the existing thermostat location
- **Remote room sensors** via Zigbee/ESPHome through Home Assistant (occupancy-weighted "follow me" fusion) — *not* Ecobee's proprietary 915 MHz sensors, which are unreceivable
- **Mobile control** via the Home Assistant Companion app — fully local on LAN; remote access via self-hosted WireGuard/Tailscale (no cloud)

> ⚠️ **Safety first — read [`docs/04-safety.md`](docs/04-safety.md) before touching the furnace.**
> This is a **gas appliance** plus a **compressor-bearing heat pump**. We do **not** command the gas valve directly — we send a *heat demand* over CT-485, and the furnace's certified Integrated Furnace Control (IFC) retains **all** combustion safety interlocks (flame sense, high-limit, pressure/rollout switches, ignition lockout). Likewise we only *request* compressor operation — the Gree equipment executes. The design's prime directive is **fail-to-no-demand** (all channels — no heat, no cool, no compressor; all relays de-energized), and **never violate compressor minimum timers on the way down or back up**. Modifying this equipment can affect **warranty, code compliance, and insurance** — see the safety doc.

---

## Status

🟡 **Design / pre-build.** Documentation and BOM complete; hardware not yet ordered; no firmware written. Work is tracked in [GitHub Issues](../../issues) and grouped by phase milestone.

**Current step: Phase 0 — installed-equipment inventory.** The design **forks** on which control architecture is physically installed:

- **Path A — communicating:** Dettson Alizé ODU + K03085 interface board → the heat pump is a CT-485 bus node and our controller commands it via bus demand messages.
- **Path B — conventional/hybrid:** Gree FLEXX ODU on 24 V signals → our controller drives CT-485 (or the 24 V "V" modulation signal) for the gas furnace plus **24 V relay outputs** (Y1/Y2/B/G) with D-wire defrost sensing for the heat pump.

Phase 0 inventories the wall-thermostat model, ODU nameplate, interface-board presence, IFC DIP switches, and every low-voltage conductor (including a conductor count at the wall plate, which gates the single-unit wall-mount option). Which path applies is an **open question until that inventory is done** — see `docs/05-firmware-plan.md`.

## How it works (one paragraph)

The production controller is a **wall-mounted ESP32-S3 touchscreen** (primary pick: Guition JC4827W543C, QSPI display, LVGL UI) at the OEM thermostat wall location, where `R`/`C`/`1`/`2` already arrive; an ESP32-DevKitC serves as the Phase 1/2 sniff rig and spare. It taps the bus through an external **3.3 V** RS-485 transceiver (explicit DE/RE, backed by an external hardware watchdog) and first listens (sniff-only, zero bus risk) to learn the CT-485 framing, node IDs, and — critically — *which message and bytes carry the 40–100 % modulation demand*. Once the protocol dictionary is built and verified, the firmware acts as a **hybrid** controller: a CT-485 virtual thermostat (or, alternatively, the 24 V "V" modulation signal) for 40–100 % gas demand, plus — architecture-dependent — bus demand messages to a heat-pump interface-board node (Path A) **or** 24 V relay outputs (Y1/Y2/B/G) with D-wire defrost sensing (Path B). A supervisory **mode state machine** (OFF/HEAT/COOL/AUTO/EMERGENCY HEAT) with **dual setpoints** feeds a dual-fuel arbiter and compressor guard; room temperature comes from **multi-sensor fusion** (HA/MQTT remote sensors with a local DS18B20 fallback). Power comes from the furnace's 24 VAC `R`/`C` via an **isolated AC/DC** supply. Layered watchdogs guarantee the system fails to **no demand on any channel** — without violating compressor minimum timers — on any fault. (Single-unit wall mount vs a split display/controller is gated by a Phase 3 bench test of TX-turnaround jitter with LVGL + Wi-Fi running.)

## Documentation

| Doc | What's in it |
| --- | --- |
| [`docs/01-architecture.md`](docs/01-architecture.md) | System overview, data flow, design decisions, scope |
| [`docs/02-protocol-climatetalk.md`](docs/02-protocol-climatetalk.md) | CT-485 deep dive: framing, Fletcher checksum, message/command IDs, token arbitration, sniffing methodology |
| [`docs/03-hardware-wiring.md`](docs/03-hardware-wiring.md) | Reviewed wiring, power, RS-485 interface, pin map, isolation |
| [`docs/04-safety.md`](docs/04-safety.md) | Functional-safety review, failure modes & failsafes, certification/legal honesty |
| [`docs/05-firmware-plan.md`](docs/05-firmware-plan.md) | Phased firmware plan, module layout, PlatformIO setup |
| [`docs/06-home-assistant.md`](docs/06-home-assistant.md) | MQTT discovery, HA climate entity, diagnostics |
| [`docs/07-ecobee-gap-analysis.md`](docs/07-ecobee-gap-analysis.md) | Feature gap analysis vs Ecobee Smart Thermostat Premium |
| [`docs/08-firmware-platform-decision.md`](docs/08-firmware-platform-decision.md) | **Decision record:** custom PlatformIO + LVGL for the wall unit (issue #38 ESPHome spike) |
| [`docs/ORDERING.md`](docs/ORDERING.md) | **Bill of materials with live purchasing links** and a minimum-viable cart |
| [`docs/legacy-plan.md`](docs/legacy-plan.md) | *Historical* — the original heat-only project plan, superseded by the docs above |

## Quick start (current phase)

> 🗺️ **New here or coming back after a break? Read [`NEXT-STEPS.md`](NEXT-STEPS.md)** — the full numbered sequence from today's state to a working thermostat, written for someone starting cold.

1. Read [`docs/04-safety.md`](docs/04-safety.md) and [`docs/03-hardware-wiring.md`](docs/03-hardware-wiring.md).
2. **Phase 0 — inventory the installed equipment** (wall-thermostat model, ODU nameplate, interface-board presence, IFC DIP switches, full low-voltage wiring map + wall-plate conductor count) — see [`docs/05-firmware-plan.md`](docs/05-firmware-plan.md). ⚠️ Never apply 24 VAC to `V/W2` while probing. This determines Path A vs Path B **before any ordering beyond the sniff rig**.
3. Order parts from [`docs/ORDERING.md`](docs/ORDERING.md) — the sniff-rig minimum-viable cart (≈ CAD $180–200) is unchanged; the full dual-fuel build adds Phase-0-gated items (relay/sense hardware if Path B, sensors, optional R02P034), so the final cart price depends on the Phase 0 outcome.
4. Build the **sniff-only** rig and capture the bus (Phase 1 — see [`docs/05-firmware-plan.md`](docs/05-firmware-plan.md)). Note: a conventional (Path B) install yields little or no ClimateTalk traffic to sniff — an R02P034 reference thermostat is the fallback bus master.
5. Track progress in [Issues](../../issues).

## Flashing

Two paths onto the ESP32-DevKitC bench rig:

- **Web installer (no toolchain):** open [`web/installer/`](web/installer/) in Chrome/Edge (Web Serial; serve over `https://` or `localhost`) and flash a pre-built **Sniffer** or **Thermostat (bench)** image over USB. Merged images + ESP Web Tools manifests are built by `python3 tools/release.py` (version from the root `VERSION` file) and attached to tagged releases by CI. The thermostat image boots **demands-disabled** — flashing convenience never bypasses the Phase 2/3 safety gates.
- **Developer path:** `pio run -e sniffer -t upload` (or `-e thermostat`), then `pio device monitor`.

## Reference prior art

- [`kdschlosser/ClimateTalk`](https://github.com/kdschlosser/ClimateTalk) — Python protocol model (most complete message/command tables)
- [`kpishere/Net485`](https://github.com/kpishere/Net485) — C++ HVAC RS-485 (authoritative physical layer + Fletcher checksum + token state machine)
- [`esphome-econet`](https://github.com/esphome-econet/esphome-econet) — Rheem EcoNet (a ClimateTalk variant; useful ESPHome RS-485 plumbing reference)
- **Dettson / Gree manual set** (primary references): R02P034 thermostat (X00507), K03085 interface board (X00510), Chinook installation (X40225Y), FLEXX cased coil (X40273A), Gree FLEXX quick-start

## Tech stack

ESP32-S3 wall touchscreen (custom PlatformIO + LVGL — the ESPHome-vs-PlatformIO decision is settled in [`docs/08-firmware-platform-decision.md`](docs/08-firmware-platform-decision.md)) · LVGL · RS-485 / CT-485 · 24 V relay/sense I/O (Path B) · MQTT · Home Assistant (+ Companion app) · supervisory mode/dual-fuel control with PID gas-demand shaping

---

*Part of the ElectricRV project family. `.env` (cPanel/MQTT/Wi-Fi secrets) is gitignored — see `.env.example`.*
