# 12. Specifications and Compliance

## 12.1 Electrical

| Item | Specification |
| --- | --- |
| Supply | 24 VAC nominal from the furnace control transformer (R/C); front end rated for ≥ 50 V peak (transformers run 28–30 VAC light-load) |
| Power consumption | ≤ ~3 VA from the transformer (display board 1.6–2.25 W plus conversion losses) |
| Power conversion | Isolated AC-input AC/DC module, 9–36 VAC input, 5 V ≥ 1 A output; DC ground isolated from furnace C |
| Input protection | Inline slow-blow fuse 250–500 mA on R; MOV (~40–47 VAC working) across R–C; 470–1000 µF / 63 V, 105 °C bulk capacitance |
| Bus interface | EIA/TIA-485 2-wire half-duplex (CT-485), 9600 bps 8N1; 3.3 V transceiver with explicit driver-enable; TVS-protected (SM712/SMBJ class) with ~10 Ω series elements; stub < 0.3 m |
| Relay outputs (Case B) | Y1, Y2, O/B, G — 24 VAC-rated dry contacts, normally open, de-energized = no demand; common coil feed interrupted by the hardware watchdog |
| Sense inputs (Case B / instrumentation) | Opto-isolated 24 VAC sense: D/W, Y, G, W |
| Local sensors | DS18B20 1-Wire (indoor fallback, outdoor, optional supply-air); 4.7 kΩ pull-up; optional I2C humidity/temperature module |
| Supervision | External hardware watchdog (TPL5010/MAX6369 class): on timeout forces driver-enable off and (Case B) cuts relay-coil power |
| Network | 2.4 GHz Wi-Fi; MQTT over the local network; no cloud dependency |

## 12.2 Environment

| Item | Specification |
| --- | --- |
| Controller mounting | Indoor living space, at the thermostat wall location |
| Furnace-side enclosures | Mounted on the cool side / outside the furnace cabinet; polycarbonate or ≥ 105 °C-rated material near heat (ABS is ~80 °C continuous) |
| Operating temperature range | **TBD** — to be characterized before release; display-board vendor ratings govern in the interim |
| Outdoor sensor | Outdoor-rated cable; north wall, shaded |

## 12.3 Compliance — honest statement

**The SlyTherm is not a certified or listed product.**

- It carries **no CSA, UL, or other safety listing**.
- The furnace and heat pump are certified **as systems with approved
  controls**; installing the SlyTherm in place of the OEM thermostat is a
  **modification of a certified gas appliance's control system** and places
  the installation **outside the certified configuration** — even though the
  design does not alter the combustion-safety chain (the certified IFC
  retains all combustion safeties, which are not reachable over the bus) or
  the refrigerant-circuit protections.
- This **can void the furnace and heat-pump warranties**, may **violate
  local gas or electrical code** (gas-appliance control work is often
  restricted to licensed technicians), and **could affect home insurance**
  after any fire, carbon-monoxide, or water incident traced to the
  controller.
- The product is **experimental and operated at the owner's risk** on a
  life-safety appliance. The supported alternative is a Dettson R02P034
  thermostat with integration at the Home Assistant level.

Conditions of installation (CO alarm, retained OEM thermostat, licensed
technician review, no safety-wiring modifications, documentation) are
binding — see Section 1.4.

## 12.4 Design provenance and open items

This draft reflects a reviewed engineering design whose equipment-facing
behaviors are **partially unverified**. The following remain open and are
marked throughout the manual:

| Open item | Where it matters |
| --- | --- |
| Installed architecture, Case A vs Case B (pre-install survey) | Sections 2, 6 |
| Demand-message payload offsets (confirmed only from captures) | Section 5.7; transmit path is gated on it |
| Bus token/turnaround timing budget; single-unit qualification | Section 2.2 |
| Mixed-mode IFC behavior (hardwired Y/G/W while communicating) | Section 6 (gates Case B) |
| Proportional vs staged heat-pump demand path | Section 8.6 |
| Silence propagation to the non-coordinator unit, per channel | Sections 7.2, 10.4 |
| Defrost tempering ownership | Sections 8.5, 10.5 |
| In-wall code treatment of the AC/DC module | Section 3.2 |
