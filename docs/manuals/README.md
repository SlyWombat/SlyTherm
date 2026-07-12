# Manuals

Local copies of reference manuals so we never re-fetch. **Grep the key facts
below before opening a PDF** — they're the numbers we keep needing for
protocol (`docs/02`) and control (`docs/13`) work.

## Vendor — Dettson (the OEM equipment SlyTherm replaces)

### `dettson-chinook-modulating-furnace-install.pdf`
Chinook modulating gas furnace (this install = **C105-M-V**), install manual.
- **Cooling airflow DIP (S3-2/S3-1)** — Table 60 CFM (1st/2nd stage) for C105-M-V:
  OFF/OFF **1590/1750**, ON/OFF 1240/1695, OFF/ON 1055/1465, ON/ON 870/1245.
  One step down ≈ −22 to −34 % airflow. S3-3/4 = ±10 % trim (Table 12).
- **S5 (Table 14):** S5-1 = on-demand dehumidification via **HUM STAT** terminal
  (humidistat required); S5-2 = 1st-stage cooling airflow (70-80 % vs **50 %**
  of 2nd stage). Installer noise-derate lives here (#152).
- **S4 (Table 13):** heat-rise 55/60-65 °F; 1-stage vs 2-stage t-stat; continuous-fan
  normal vs higher CFM.
- **HUM STAT terminal:** furnace-side humidification AND/OR dehumidification input.
- **Fan-off delay is internal** (System menu: AC/HP ON delay 5-120 s, OFF delay
  5-240 s) — do NOT duplicate on the bus (#142 finding).
- Heating CFM Table 59; the IFC maps airflow to fire rate internally.

### `dettson-communicating-thermostat-R02P032-install.pdf`
Dettson communicating thermostat (R02P032 family; the OEM stat on the wall).
- **Temp setpoint range:** 41–95 °F.
- **De-Hum setpoint:** **40–95 %, default 60 %** (cooling season; DEHUM_DEMAND `0x62`).
- **Humidification setpoint:** **20–50 %** (heating season only; HUM_DEMAND `0x63`).
- **Humidity deadband:** 2–20 %, **default 5 %** (hysteresis on hum/dehum engage).
- **Humidity adjustment:** −9 to +9 %, default 0 % (offset/calibration).
- Mode via SYSTEM key; setpoint via up/down. Bus behavior decoded in `docs/02`
  (setpoints/modes are NOT transmitted — only the demands they produce).

## Ours (SlyTherm product docs, built by CI — see manual/)
- `INSTALLATION_MANUAL.pdf`, `USER_MANUAL.pdf`
