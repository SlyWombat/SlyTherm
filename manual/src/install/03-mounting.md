# 3. Mounting

## 3.1 Location

Mount the SlyTherm wall unit at the **existing OEM thermostat location**. This is
deliberate: the wall plate there already terminates R/C/1/2 (24 VAC power and
the CT-485 bus), and the location was chosen by the original installer for
representative room air. Do not relocate the controller to a wall cavity with
poor air circulation, direct sunlight, or proximity to supply registers —
the on-board/local temperature sensing is a safety fallback and must read
plausible room air.

Standard thermostat-placement practice applies: interior wall, approximately
1.5 m above the floor, away from drafts, sun, lamps, and appliances.

## 3.2 Wall bracket and enclosure

1. Remove the OEM thermostat from its base. **Keep the thermostat and its
   base intact** — it is your certified rollback device (Section 11).
2. Label every conductor at the wall plate before disconnecting anything
   (R, C, 1, 2, and any extras found in the survey).
3. Install a **low-voltage old-work bracket** in the existing opening. The
   bracket carries the SlyTherm wall enclosure; no electrical box is required for
   Class-2 wiring, subject to local code.
4. The SlyTherm wall enclosure must house, behind the display:
   - the isolated AC/DC converter module (Section 4);
   - the inline fuse and MOV;
   - the RS-485 transceiver carrier board with TVS protection;
   - the external hardware watchdog;
   - terminal blocks for the field wiring.
5. Use **ferrules on all stranded conductors** entering screw terminals, and
   strain-relieve all wiring. Thermal cycling loosens bare strands over time.
6. **Fully enclose the AC/DC converter module** — no exposed mains-style
   terminals or bare boards in the wall cavity.

> **⚠ Pending verification (local code):** Class-2 24 VAC wiring in a wall
> cavity is generally permissible, but the code treatment of an **AC/DC
> converter module mounted in-wall** must be confirmed for your jurisdiction
> before the enclosure design is finalized.

## 3.3 Enclosure material and temperature

| Location | Requirement |
| --- | --- |
| Wall enclosure (living space) | Flame-retardant enclosure (UL94 V-0 rated plastic or polycarbonate preferred); screw terminals; strain relief |
| Furnace-side enclosure (split installation or Case-B relay stage at the coil) | **Polycarbonate or equivalent rated ≥ 105 °C near heat.** ABS is only ~80 °C continuous and furnace cabinets exceed that near the burner — mount on the **cool side of the furnace, outside the cabinet** |
| Electrolytic capacitors (any enclosure) | 105 °C-rated parts |

## 3.4 Furnace-side junction point

In a single-unit Case A installation there is no controller at the furnace;
an enclosure at the furnace is optional and serves only as a junction or
instrumentation point. In a **split installation** or a **Case B
installation with the relay stage at the coil**, the furnace-side enclosure
houses the headless controller and/or the relay and sense modules — apply the
temperature rules above.

## 3.5 Sensor placement

- **Local fallback sensor (DS18B20):** in a single-unit wall installation
  the sensor is at the wall — in living space, which is ideal. In a split
  installation, do **not** place it adjacent to the furnace plenum if
  avoidable; the 1-Wire bus tolerates long runs, so relocate it into living
  space. A plenum-adjacent fallback sensor is a floor-keeper, not a comfort
  input, and forces a degraded operating mode if it ever becomes the only
  source.
- **Outdoor temperature sensor (wired DS18B20):** mount on a **north wall,
  shaded**, with an outdoor-rated cable run. This sensor feeds the dual-fuel
  balance point and compressor lockouts (Section 8); a sensor in sun or over
  a dryer vent will corrupt changeover decisions.
- **Optional supply-air sensor (DS18B20):** in the supply trunk if the
  coil-freeze guard option is used (cooling dropped if supply air falls below
  approximately 5 °C).
