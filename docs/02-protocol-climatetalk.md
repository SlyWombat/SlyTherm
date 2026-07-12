# 02 — The ClimateTalk / CT-485 Protocol

Reconstructed from three independent open-source implementations whose constants cross-validate each other — [`kpishere/Net485`](https://github.com/kpishere/Net485) (C++, authoritative for physical layer + checksum + token state machine), [`kdschlosser/ClimateTalk`](https://github.com/kdschlosser/ClimateTalk) (Python, most complete message/command model), and [`esphome-econet`](https://github.com/esphome-econet/esphome-econet) (Rheem EcoNet, a *variant* — useful plumbing reference, **different framing/CRC**) — plus the NEMA CT-485 standard descriptions.

**Confidence:** values agreed by two code bases are high-confidence. Two things **must be confirmed empirically by sniffing** (§8): the **exact message + byte that drives modulation on this specific furnace** (Chinook range is **40–100 %**, low fire = 40 % — not 0–100 %), and the **demand payload byte offsets**, which are single-sourced and internally inconsistent in the prior art (§5a).

> **EcoNet warning:** Rheem EcoNet runs **38400 baud + CRC-16/ARC + a 14-byte header**. The Dettson bus (furnace IFC + thermostat, and — if present — the Alizé K03085 interface board) is **native CT-485: 9600 8N1 + Fletcher-16 + 10-byte header.** Do not copy EcoNet's framing or checksum. Confirm baud by sniffing first.

---

## 1. Physical layer

| Parameter | Value | Source |
| --- | --- | --- |
| Standard | EIA/TIA-485, 2-wire half-duplex ("CT-485") | NEMA |
| Baud | **9600 bps** (`DATARATE_BPS_DEF 9600UL`) | Net485 |
| Format | **8N1** (8 data, 1 start, 1 stop, no parity) | Net485 |
| Byte frame time | ≈ **1041.6 µs/byte** at 9600 8N1 | Net485 `BYTE_FRAME_TIME` |
| Inter-packet idle before TX | **100 ms** (`INTER_PACKET_DELAY` = 100000 µs) | Net485 + kdschlosser |
| End-of-frame detection | inter-byte gap **> 3.5 ms** = frame boundary (`DELIMIT_MEASURE_PACKET 3500`) | both |
| Driver pre-enable hold | **300 µs** (200–500 µs allowable) before first byte | Net485 |
| Driver post-disable hold | **300 µs** (200–500 µs allowable) after last byte | Net485 |
| Termination / bias | standard RS-485 idle bias + 120 Ω at bus *ends*; numeric values not in code — **measure on the harness** | inferred |

There is **no preamble/sync byte**. Frame boundaries are defined purely by the ≥ 3.5 ms idle gap.

---

## 2. Frame structure

**Fixed 10-byte header + variable payload (0–240 B) + 2-byte Fletcher checksum.** Cross-confirmed by kdschlosser `packet.py` and Net485 `HeaderStructureE`/`Net485PacketS`.

| Offset | Size | Field | Notes |
| --- | --- | --- | --- |
| 0 | 1 | **Destination Address** | `0x00`=broadcast, `0xFF`=coordinator, `0xFE`=NAD (arbitration), `0xF1`=virtual-sub |
| 1 | 1 | **Source Address** | sender node ID |
| 2 | 1 | **Subnet** | `0x00`=broadcast, `0x01`=maintenance, `0x02`=V1, `0x03`=V2 |
| 3 | 1 | **Send Method** | `0x00` non-routed, `0x01` routed-by-priority/control-command, `0x02` by node-type, `0x03` by socket |
| 4 | 1 | **Send Parameter (hi)** | for control commands, the **command code** (e.g. `0x64`=HEAT). For node-type routing, the target node type. |
| 5 | 1 | **Send Parameter 1 (lo)** | coordinator→sub: "Nth node of that type"; sub→coordinator: `0x00` |
| 6 | 1 | **Source Node Type** | `0x01`=thermostat, `0x02`=gas furnace, `0x05`=heat pump … (§6) |
| 7 | 1 | **Message Type** | the message class/ID (§4). **Response = request \| 0x80.** |
| 8 | 1 | **Packet Number** | bit7=dataflow flag (1 on R2R/ACK), bit5=version (1=v1.0, 0=v2.0), bits4-0=chunk# (usually 0) |
| 9 | 1 | **Packet Length** | payload length, 0–240. **Excludes header and checksum.** |
| 10…10+len-1 | 0–240 | **Payload** | message-type specific |
| 10+len | 1 | **Checksum 1** | Fletcher byte 1 |
| 10+len+1 | 1 | **Checksum 2** | Fletcher byte 2 |

Constants: `MTU_HEADER=10`, `MTU_DATA=240`, `MTU_CHECKSUM=2`, `MTU_MAX=252`. **Total frame = `Packet Length` + 12.**

**Receiver algorithm:** read bytes until ≥ 3.5 ms gap → validate `len == buf[9]` and `total == buf[9] + 12` → verify Fletcher.

**MAC sub-structure** (8 bytes, inside discovery/addressing payloads): `[0]`=reserved (`0xFF` flags a random MAC), `[1..2]`=Manufacturer ID (big-endian), `[3..7]`=Device ID. Session ID is a separate 8-byte value per node.

---

## 3. Checksum — Fletcher-16 variant (implementable)

Both Net485 and kdschlosser use the **identical** algorithm: Fletcher-16 with non-standard **seed `s1=0xAA`, `s2=0x00`**, modulus **`0xFF`**, over the **entire header + payload** (offsets 0 .. 10+len-1). Net485 is the reference:

```cpp
// MTU_HEADER = 10; len = payload length
void ct485_checksum(uint8_t *buf, uint8_t len) {
    uint8_t s1 = 0xAA, s2 = 0x00;          // CT_ISUM1 / CT_ISUM2 ("New Fletcher Seed")
    uint16_t n = len + 10;
    for (uint16_t i = 0; i < n; i++) {
        s1 = (uint8_t)((s1 + buf[i]) % 0xFF);
        s2 = (uint8_t)((s2 + s1)     % 0xFF);
    }
    buf[n]     = 0xFF - ((s1 + s2)     % 0xFF);   // checksum byte 1
    buf[n + 1] = 0xFF - ((s1 + buf[n]) % 0xFF);   // checksum byte 2
}

// Valid frame: accumulate header+payload AND both checksum bytes → s1==0 && s2==0
bool ct485_checksum_ok(uint8_t *buf, uint8_t len) {
    uint8_t s1 = 0xAA, s2 = 0x00;
    uint16_t n = len + 10;
    for (uint16_t i = 0; i < n + 2; i++) {
        s1 = (uint8_t)((s1 + buf[i]) % 0xFF);
        s2 = (uint8_t)((s2 + s1)     % 0xFF);
    }
    return s1 == 0 && s2 == 0;
}
```

Notes: it's **mod 255, not mod 256**. kdschlosser's Python has minor bugs (`calc_checksum`/`is_valid`) — trust the C++ above. **EcoNet uses CRC-16/ARC instead — do not use it here.**

---

## 4. Message types (offset 7)

Cross-confirmed kdschlosser `message_types.py` + Net485 `Net485API.hpp`. **Response = request | 0x80.**

### Application (data-carrying) messages

| ID | Name | Carries |
| --- | --- | --- |
| `0x01`/`0x81` | Get Configuration / Resp | config DB (TLV records) |
| `0x02`/`0x82` | **Get Status / Resp** | operating status — fan/blower, demand, mode |
| `0x03`/`0x83` | **Set Control Command / Resp** | **the write channel — heat/cool/fan demand, motor speed/torque/airflow** (§5) |
| `0x04`/`0x84` | Set Display Message / Resp | thermostat display text |
| `0x05`/`0x85` | Set Diagnostics / Resp | push a fault |
| `0x06`/`0x86` | **Get Diagnostics / Resp** | **fault list (null-separated strings)** |
| `0x07`/`0x87` | Get Sensor Data / Resp | sensor DB records (temps, pressures) |
| `0x0D`/`0x0E` (+0x80) | Set/Get Identification | model, serial, MAC |
| `0x1D`/`0x9D` | **DMA Read / Resp** | **read raw MDI tables by start-byte + count — best for harvesting modulation/blower telemetry** |
| `0x1E` | DMA Read Resp (Motor) | motor payload at offset 13 |
| `0x1F`/`0x20` (+0x80) | Set/Get Mfg Generic Data | vendor blob w/ mfg ID |
| `0x41`/`0x42` (+0x80) | Get/Update User Menu | installer-menu nav & writes |
| `0x5A`/`0xDA` | Echo / Resp | bus test |

### Data-link / network-management messages

| ID | Name | Purpose |
| --- | --- | --- |
| `0x00` | **Request-to-Receive (R2R)** + ACK | token/polling hand-off; `R2R_CODE=0x00`, `R2R_ACK=0x06`; dataflow bit set |
| `0x75`/`0xF5` | Network State Req / Resp | coordinator node list |
| `0x77`/`0xF7` | **Token Offer / Resp** | token passing; carries address, subnet, MAC, session ID |
| `0x78` | **Version Announcement** (bcast) | CT-485 version/revision + FFD flag |
| `0x79`/`0xF9` | **Node Discovery / Resp** (bcast) | AutoNet join; carries node type, MAC, session ID |
| `0x7A`/`0xFA` | **Set Address / Resp** | coordinator assigns node ID + subnet to a MAC |
| `0x7B`/`0xFB` | **Get Node ID / Resp** | query node type/MAC/session |

**ACK/NAK (in response payloads):** `ACK1=0x06` (valid control cmd), `ACK2=0x0A` (undesired param), `ACK3=0x0D` (min params incomplete); `NAK1=0x15` (bad CRC), `NAK2=0x1B` (invalid for that application).

---

## 5. Where modulation %, blower, faults, setpoints live

### 5a. Commanding heat demand / modulation

Issued via **Set Control Command (`0x03`)**. Command code goes in **Send Parameter (offset 4)** and is echoed in the payload as a **16-bit `command_code` at [10..11] (little-endian)**, followed by a **single-byte refresh timer** and the **demand value**.

> ✅ **OFFSETS FIELD-CONFIRMED (2026-07-08/09, real-furnace capture — the #11 gate is CLEARED).**
> Live capture of the OEM thermostat (R02P032) driving the Chinook resolved kdschlosser's
> internal inconsistency in favour of the read-side layout: **timer at `[12]`, demand at
> `[13]`** (fan: `[12]=timer, [13]=mode, [14]=pct×2`). Evidence: dozens of `0x64`/`0x65`/`0x66`
> frames across the full value range — heat 0x00–0xC8, cool 0x00/0x3C, fan 0x32/0x64/0x96 —
> all with the timer-shaped `0x60` constant at `[12]`. The write-path variant (timer `[13]`,
> demand `[14]`) is disproven: real 0x64/0x65 payloads are only 4 bytes — there is no `[14]`.

| Code | Command | Payload encoding (after 2-byte cmd code) |
| --- | --- | --- |
| `0x64` | **HEAT_DEMAND** | `[12]=refreshTimer, [13]=demand`. **Demand byte = percent × 2** (0–200 = 0–100 %). **Primary modulation channel.** ✅ CONFIRMED 2026-07-09: true closed-loop modulation — OEM stat opened at 100 % (0xC8), walked the full 40–100 % range in ~5 % steps with corrections in BOTH directions, held the 40 % floor, ended with an explicit demand 0. |
| `0x65` | COOL_DEMAND | percent × 2. ✅ CONFIRMED: on this install cooling is **staged, not modulating** — always exactly `0x3C` (30 %) when on, `0x00` when off, regardless of setpoint error. **Re-confirmed at the EXTREME 2026-07-12** (annotated capture session): cool setpoint driven to **8 °C** still produced only `0x3C` (30 %) — cooling never modulates up, even at an absurd error. Heat vs cool asymmetry is fundamental: heat 40–100 % modulating, cool fixed 30 % (why #140 duty-cycles 30 % rather than raising demand). |
| `0x66` | FAN_DEMAND | `[12]=timer, [13]=mode, [14]=percent×2`. ✅ CONFIRMED: manual fan = mode 0, three discrete speeds **Low 25 % (0x32) / Med 50 % (0x64) / High 75 % (0x96)**; fan-on enters at Med. NOT used during heat/cool — the IFC maps airflow to fire rate internally; the stat never commands CFM in normal operation. |
| `0x61` | SUBSYSTEM_BUSY_STATUS | ✅ OBSERVED 2026-07-08/09, **characterization CORRECTED 2026-07-12**: sent **coordinator → thermostat** (payload `61 00 60 00`, value 0 = "not busy"), 1–6 s after a demand transition — an equipment-readiness handshake. The earlier "operator-initiated only / never during automatic cycling" note was WRONG (limited early window): overnight 2026-07-11→12 it fired 7× at 2 am with nobody at the stat, each 1–6 s after an **automatic COOL_DEMAND cycle transition** (start/stop). Arrives in a short burst with two `0x06` token/session frames (see below). **TX-relevance: when SlyTherm asserts its own COOL_DEMAND, expect and tolerate this `0x61` follow-up from the coordinator — it is not a command to us.** |
| `0x67`/`0x68`/`0x69` | BACKUP_HEAT / DEFROST / AUX_HEAT | percent × 2 |
| `0x60` | DAMPER_POSITION | position × 2 |
| `0xAC` | *unmapped status* (thermostat → bcast) | ✅ OBSERVED (not Set-Control, so it slips past the novel-command detector): 2-byte payload `AC 06`, sent **thermostat (node 1) → 255**, **75× over 2026-07-08→12**, correlating with cool-cycle transitions — part of the same handshake burst as `0x61` + the two `0x06` token frames. Value `0x06` constant so far. Meaning TBD; catalog it, don't let it alarm. |
| `0x6A` | SET_MOTOR_SPEED | RPM × 4, little-endian `[13][14]` |
| `0x6B` | SET_MOTOR_TORQUE | value × 2048, LE |
| `0x6C` | SET_AIRFLOW_DEMAND | CFM × 4, LE |
| `0x70` | SET_MOTOR_TORQUE_PERCENT | pct × 65535 / 100, LE — a true 0–100 % channel |
| `0x05` | SYSTEM_SWITCH_MODIFY | `0x00`off `0x01`cool `0x02`auto `0x03`heat `0x04`backup-heat |
| `0x01`/`0x02` | HEAT/COOL_SET_POINT_MODIFY | 1-byte temp at `[13]` |
| `0x07` | FAN_KEY_SELECTION | fan mode select (auto/on) |
| `0x47` | SET_POINT_TEMP_AND_TEMPORARY_HOLD | setpoint + hold-time combo |
| `0x5D`/`0x5E` | DEHUMIDIFICATION/HUMIDIFICATION_SET_POINT_MODIFY | setpoint % |
| `0x62`/`0x63` | DEHUMIDIFICATION/HUMIDIFICATION_DEMAND | demand %. ✅ CONFIRMED 2026-07-12 (annotated capture): both are **RH-vs-setpoint driven, NOT heat/cool-mode gated**. Structure `62/63 00 60 [val]`. **DEHUM `0x62`**: setpoint→95 % (max) drops the demand to `0x00` (off — house RH never reaches 95 %); nonzero values seen (0x1e/0x28/0x32) while active — value semantics TBD. **HUM `0x63`**: raising the humidification setpoint above the room's RH fired `HUM_DEMAND` = `0x96` (75 %) **during COOLING season** (no humidifier likely acting, but the demand is broadcast). **Install note:** the OEM stat's RH sensor read ~7 % LOW (43 % shown vs 50 % actual, corrected via the ±9 % Humidity-adjustment setting); dehum was left at 95 % = effectively disabled. Do NOT trust OEM RH — SlyTherm uses its own calibrated sensor (#106/#107) for the §8 humidity logic. |

**Refresh-timer byte:** high nibble = minutes (0–15), low nibble = seconds in 3.75 s units. The demand **must be re-sent before the timer expires** or the subordinate reverts to off — a deliberate safety watchdog the firmware must honour (re-issue well within it). The timer is **per demand channel** (HEAT, COOL, FAN, BACKUP, DEFROST, AUX each refresh independently).

**✅ Measured refresh behaviour (OEM stat, 2026-07-08/09):** timer is always `0x60` (6 min 00 s); steady-state refresh every **5 minutes** (~83 % of the timer). Cycle **starts** re-assert faster before settling: observed at +10 s, +40 s, +60 s after the opening demand. **Demand-off is EXPLICIT** — every cycle ends with a demand-value-0 frame, never by letting the timer lapse. **Zero demands are NOT refreshed**: between cycles the bus carries no demand traffic at all (revert-on-silence and "off" are equivalent, so the stat doesn't bother). A demand-value change is sent immediately, outside the refresh cadence.

**✅ Response contract (every accepted control command):** two replies from the recipient — (1) a conventional `0x83` echo of the 4/5-byte request payload, and (2) a **17-byte dataflow frame whose payload leads `0x06` (ACK1)** followed by a fixed station blob (`06 00 00 0C 01 62 8A 71 E9 …`). The 17-byte form also appears on the *request* msgType (`0x03`) during cycle-start bursts — decoders must not read its first two bytes as a command code (it census-pollutes as bogus "cmd 0x0006"). The same ACK form answers diagnostics pushes (`0x05`).

**Persistent-state commands:** `SYSTEM_SWITCH_MODIFY (0x05)` and the setpoint modifies (`0x01`/`0x02`/`0x47`) are **persistent state with no refresh timer** — they do not revert on silence. Whether the equipment persists switch state across its own power cycles is **undocumented** (open question; test at commissioning).

**No bus-level interlock:** nothing in the protocol prevents HEAT_DEMAND and COOL_DEMAND from being simultaneously nonzero. Heat/cool mutual exclusion is entirely **our controller's responsibility** — see `04-safety.md` (demand-conflict invariant).

> **✅ Dettson Chinook modulation — CONFIRMED by capture (2026-07-09, first instrumented heat cycle, 11:30–12:39 EDT):** modulation IS `HEAT_DEMAND (0x64)`, pct × 2. The OEM stat never commanded below the 40 % floor (0x50) and never used 0x50–0x00 intermediate values — the range on the wire is exactly the published 40–100 % plus explicit 0. Full reference trajectory: 100 % opening fire held ~15 min → ~5 %/step walk-down with two upward corrections while hunting the hold rate → 40 % floor → setpoint crossed → **min fire deliberately held 23 more minutes to +0.3 °C overshoot** → demand 0. Any future SlyTherm TX algorithm should be judged against this trajectory (raw record: SlyLog DB + captures archive, 2026-07-09).
>
> **✅ Fault channel decoded the same day:** the furnace pushes `Set Diagnostics (0x05)` with a 4-byte code header + **ASCII fault text** — observed `02 00 57 08 "HPC OPEN"` (high-pressure switch, flickers at high-fire derate transitions) and `02 00 45 08 "LPC OPEN"` (low-pressure switch, flickers at sustained minimum fire). Fault **clear** is a `0x05` push with a zeroed 4-byte payload (`02 00 00 00`). All observed faults self-cleared in ~90 s; the coordinator relays furnace faults to the thermostat, which ACKs each leg with the standard 17-byte form.
>
> **✅ MAC-layer reality (invisible until length-based framing, v0.5.7):** the bus is never idle — R2R token polls (`0x00`, 17-byte payloads carrying MAC + session id) run continuously: coordinator→thermostat ~every 5 s, coordinator→furnace ~every 20 s, Node Discovery broadcast ~every 20 s, and an address sweep of `0x03–0x0F` ~every 5 min. **Nodes answer the token only when they have something to say** — the furnace IFC goes completely silent at idle (357 consecutive unanswered polls observed) and that is normal, not a fault. A future SlyTherm node must mimic this etiquette. Frames on this bus arrive back-to-back with NO inter-frame gap — receivers must frame by the declared length (header[9]+12), not by bus idle time.

### 5b. Reading live modulation %, blower speed, faults

- **Get Status (`0x02`→`0x82`):** furnace returns a status block (payload at 11+). Contents firmware-specific — **map empirically**.
- **DMA Read (`0x1D`→`0x9D`):** request MDI table (`0x01`config/`0x02`status/`0x03`sensor/`0x0E`ident) + start-byte + count. Walk the furnace's tables to find live modulation %, gas rate, blower RPM. Motor variant (`0x9D`/`0x1E`) puts data at offset 13.
- **Get Diagnostics (`0x06`→`0x86`):** response is **null-separated fault strings**.
- **Get Sensor Data (`0x07`→`0x87`):** TLV `(db_id, db_len, data)`.
- **Outdoor temperature:** in the communicating (Alizé + interface-board) path, OAT is published as **sensor MDI id 0** on the HP/AC/crossover node — read via **Get Sensor Data (`0x07`→`0x87`)** or **DMA Read (`0x1D`) table `0x03`** (sensor table). Whether it appears on *this* install is an open question — add to the §8 capture list. Do **not** look for outdoor temp in the LWP `0x1E–0x27` range (see below).

**LWP node-type codes** (telemetry origin): blower motors `0x0A–0x13`, inducer `0x14–0x1D`, outdoor **fan motors** `0x1E–0x27` (motor codes — **not** outdoor temperature sensors), **gas-stepper `0x3C–0x45`**.

---

## 6. Addressing & node discovery

**Address map (Net485):** `0x00` broadcast · `0x01` primary (**thermostat default ID = 1**) · V1 range `0x01–0x0E` · V2 range `0x10–0x3E` · diagnostics `0x55–0x5A` · `0xC0` network-analysis (restricted) · `0xF1` virtual-sub · `0xFE` NAD/arbitration · `0xFF` coordinator. Subnets: `0x00`/`0x01`/`0x02`/`0x03`.

**Node-type codes (offset 6):** `0x01` thermostat · **`0x02` gas furnace (IFC)** · `0x03` air handler · `0x04` AC · **`0x05` heat pump** · `0x06` electric furnace · `0x09` crossover/OBBI · `0x0C` unitary control · `0x15` zone control · `0xA6` network coordinator.

**AutoNet join sequence:**

1. **Coordinator election (arbitration).** Power-up node with no coordinator runs `ANServerBecomingA → B → Waiting` with a randomized slot delay; highest version / earliest claim becomes coordinator (`0xFF`). Version from **Version Announcement (`0x78`)**.
2. **Node Discovery (`0x79` broadcast).** Each unaddressed node replies (`0xF9`) after a **random 6–30 s slot delay** (`ANET_SLOTLO 6000`, `ANET_SLOTHI 30000`) with node type + MAC + session ID.
3. **Address assignment.** Coordinator sends **Set Address (`0x7A`)** to the MAC with the assigned ID + subnet; node replies `0xFA`. Thermostat/zone-control always get **node ID 1**.
4. **Steady-state polling / token.** Coordinator cycles **R2R (`0x00`) / Token Offer (`0x77`)** to each node to grant bus access. `R2R_LOOPS_PERDATACYCLE=5`; node list re-polled ≈ 110 s; a node silent > 120 s is dropped. Per-exchange `RESPONSE_TIMEOUT=3000 ms`, `MSG_RESEND_ATTEMPTS=3`.

**Implication: the bus is coordinator-polled and token-mediated, not CSMA.** A subordinate transmits **only when granted the token / R2R**. This is the single biggest constraint on a virtual thermostat that wants to *write*.

> **Setpoints/modes are NEVER on the bus — only demands (annotated capture 2026-07-12).** Driving the OEM stat through hold 8/23/37/9 °C and mode heat/off/cool produced **zero** setpoint- or mode-modify frames (`0x01/0x02/0x05/0x47`); only the resulting `HEAT/COOL/FAN/DEHUM_DEMAND` broadcasts changed. The thermostat computes demands internally from setpoint + measured temp and puts only the demands on the wire. **TX design consequence: SlyTherm controls by writing DEMANDS (`0x64/0x65/0x66/...`), which we fully decode — there is no setpoint to echo. Our "setpoint" lives in OUR pipeline; the bus only ever carries the demand it produces.**

---

## 7. EcoNet vs native CT-485 (reference only)

| Aspect | Native CT-485 (this project) | EcoNet |
| --- | --- | --- |
| Baud | 9600 8N1 | 38400 |
| Header | 10 B, 1-byte addresses | 14 B, 5-byte addresses (`DST=0,SRC=5,LEN=10,CMD=13`) |
| Read/Write | Get/Set msg types | `READ=0x1E`, `WRITE=0x1F`, `ACK=0x06` |
| Checksum | Fletcher-16, seed 0xAA, mod 0xFF | CRC-16/ARC (poly 0xA001), LE |

EcoNet is useful only as an **ESPHome RS-485 plumbing pattern** (UART/DE handling, 30 s poll scheduler). Its framing/CRC are **not transferable**.

---

## 8. Known unknowns → sniffing methodology

**Must discover empirically:**

1. Confirm **baud (9600 vs 38400) and 8N1**.
2. Actual **node IDs** of the Dettson furnace and any HP-side node (see item 6 — the HP itself is likely *not* on the bus), and which device is **coordinator**.
3. Exact **Get-Status (`0x82`) payload layout** — offset of live modulation %, blower RPM, run mode.
4. Whether modulation is `HEAT_DEMAND (0x64)` pct×2, `SET_MOTOR_TORQUE_PERCENT (0x70)`, or a mfg-generic (`0x1F`/`0x20`) command.
5. Gas-stepper node (`0x3C–0x45`) telemetry — which DMA/status bytes report valve position.
6. **What represents the HP side on the bus, if anything.** A true Gree FLEXX is **not a CT-485 node** — it is commanded by conventional 24 V signals (Y1/Y2, B, G, plus the D defrost output), and its H1/H2 RS-485 link to the coil board is **Gree-proprietary**, not CT-485. What *may* appear on the bus is the Dettson **K03085 interface board** (Alizé communicating configuration). Open questions: does the interface board enumerate as node type `0x05` (heat pump) or `0x09` (crossover/OBBI)? Does it originate its own bus traffic we must arbitrate with (e.g. commanding furnace heat for defrost tempering)? Is its HP/cool demand modulating (`0x65` pct × 2) or staged? **If the install is conventional (R02P033 thermostat, no interface board), expect little or no HP-related traffic — possibly a near-silent bus** (Phase 0 inventory resolves which architecture exists before sniffing is even attempted).
7. **Manufacturer IDs** of Dettson and Gree (top of MAC).
8. Whether the furnace **accepts external Set-Control-Commands** or only its paired/owning thermostat (pairing commands exist; rejection returns `NAK2 0x1B`).

**Methodology (sniff-only first — RX wired, DE never asserted):**

1. **Hardware:** ESP32 + 3.3 V transceiver in permanent-receive. Tap A/B in **parallel** with the existing thermostat — do not break the bus. Add 120 Ω only if physically at a bus end.
2. **Auto-baud:** try 9600 then 38400; at the correct baud, frames terminate cleanly on ≥ 3.5 ms gaps and Fletcher passes.
3. **Framer:** accumulate bytes; close frame on > 3.5 ms gap; validate `len=buf[9]`, total `=len+12`, Fletcher. Log hex + decode header fields.
4. **Decode loop:** label by msgType (§4); for `0x03`, decode command code (offset 4) + demand byte; for `0x82`/`0x9D`, dump payload as a byte grid.
5. **Stimulus-response:** at the OEM thermostat, walk the heat call low→high fire (and run the HP). **Diff successive `0x03`/`0x82` payloads** to localize the modulation byte; cross-check demand÷2 against displayed % and measured fire rate.
6. **Map faults:** trigger a known fault per OEM service procedure; capture `0x86`/`0x05`.
7. **Build a field dictionary** (msgType, command, offset → meaning) *before writing a single byte*.
8. **Capture campaigns** (label per the `captures/README.md` naming convention; cooling/defrost captures are season-dependent):
   - **Node Discovery enumeration:** power-cycle / let AutoNet re-run; capture the full `0x78`/`0x79`/`0xF9`/`0x7A` sequence — every node type, address, and MAC on the bus. Answers whether an interface board exists and what it enumerates as.
   - **OEM cooling call:** does the thermostat send `COOL_DEMAND 0x65` / `FAN_DEMAND 0x66` *to the furnace* (node type `0x02`) for blower CFM, or is the blower driven by hardwired Y/G? Also: **confirm the demand payload offsets** from these real `0x03` frames (§5a hard gate).
   - **Mode change:** capture `SYSTEM_SWITCH_MODIFY (0x05)` traffic on OFF/HEAT/COOL/AUTO changes; power-cycle equipment afterward to probe switch-state persistence.
   - **Forced defrost:** force a defrost cycle from the OEM service menu ("FO 3 Force Cycle"); capture the defrost signature and who commands furnace tempering heat (thermostat vs interface board acting autonomously).
   - **Outdoor temp:** capture `Get Sensor Data 0x07/0x87` exchanges; confirm OAT at sensor MDI id 0 or rule it out.
   - **24 V terminal correlation:** simultaneously log the 24 V terminal states (Y, O/B, W, G, D) against bus frames (paired `*.terminals.csv`) — this is what determines which demands travel by bus vs by wire.
   - **Token-window timing:** measure the R2R/Token-Offer → response window (grant-to-TX deadline) from OEM-thermostat exchanges — this is the slot budget the Phase 3 single-unit jitter bench test (docs/05) is judged against.
   - **Coordinator / silence propagation:** identify the coordinator node; then (at commissioning, per `04-safety.md` matrix) pull-bus tests *per demand channel* — does our silence drop COOL/HP/aux calls at the non-coordinator unit, or only furnace heat?

Parser sanity-test frame (from the openHAB thread; `dst/src=0x30` is an ASCII-'0' adapter artifact):

```
30 30 7d 07 d4 d1 9f 9a c6 7c 10 a2 01 ff 02 00 00 00 a5 00 a0 11 …
```

---

## 9. Risk assessment of the WRITE path

| Risk | Severity | Mitigation |
| --- | --- | --- |
| **Bus contention / collisions** | High | Bus is coordinator-polled (R2R/token), not CSMA. Either **replace/impersonate** the OEM thermostat (remove it) or fully implement the coordinator/token state machine and TX only when granted the token. |
| **Two masters** | High | OEM thermostat + your demand = conflicting commands, undefined/oscillating behaviour. **Decide up front: replace, not coexist.** |
| **DE timing / driver fights** | Medium | Honour 300 µs pre/post-drive + 100 ms inter-packet gap; never assert DE during another node's transmission. |
| **Demand watchdog timeout** | Medium (safety-positive) | Crashed ESP32 → equipment reverts to off (good). But a sluggish loop can drop fire mid-cycle — re-issue demands well within the refresh timer. **Reversion-on-silence is per channel and only inferred from prior art for HEAT — verify it for COOL/FAN/BACKUP/AUX (and for the non-coordinator unit) with pull-bus tests per channel before relying on "silence = safe".** |
| **Unconfirmed demand offsets** | **Critical (TX gate)** | The `0x03` payload layout is single-sourced from kdschlosser and internally inconsistent ([12] vs [13]/[14] — §5a). A demand byte written at the wrong offset could land in the refresh-timer or another field. **No TX until offsets are confirmed from sniffed OEM frames.** |
| **Cooling / compressor write path** | **Critical** | `COOL_DEMAND 0x65` drives a compressor: nothing on the bus enforces heat/cool mutual exclusion or compressor minimum on/off timers — both are our firmware's responsibility (CompressorGuard + demand-conflict invariant, `04-safety.md`). Whether `0x65` is even honoured (and by which node — interface board vs furnace blower path) is unknown until Phase 2 captures. Oscillating or premature cool demands risk compressor short-cycling and latched ODU faults. |
| **Pairing / ownership rejection** | Medium | Furnace may honour only its paired controller; you may need to complete pairing or commands return `NAK2 0x1B`. |
| **Safety lockouts** | **Critical** | Combustion safeties (flame, pressure/limit, ignition lockout) are enforced by the IFC **independently of the bus** — the modulation command is a *request*. You cannot command past a lockout, but aggressive/oscillating demand can cause short-cycling, condensate/HX stress, nuisance lockouts. Respect `SET_DEMAND_RAMP_RATE`; never touch limit/pressure switch wiring. |

**Recommendation:** validate the **sniff-only decoder first** (zero bus risk). Implement TX only after the field dictionary is complete and the OEM thermostat is removed — then implement coordinator/token + Fletcher + demand-watchdog correctly, starting at low fire with a hard software clamp on the demand byte.

---

## 10. Reading list

**Source code (read these files):**

- `kdschlosser/ClimateTalk` — `packet.py` (offsets + Fletcher seed), `message_types.py` (all msg IDs), `commands.py` (all control codes incl. `HEAT_DEMAND 0x64`), `protocol.py` (ACK/NAK + LWP node types), `rs485.py` (3.5 ms / 100 ms thresholds), `mac_address.py`.
- `kpishere/Net485` — `Net485API.hpp` (offsets, MTU, addresses, node types, msg types, subnets, packet-number bits), `Net485DataLink.cpp` (**Fletcher reference impl**), `Net485Physical_HardwareSerial.hpp/.cpp` (9600 8N1, timings, DE state machine), `Net485Network.hpp/.cpp` (coordinator election, R2R/token, slot delays, timeouts, demand codes), `Doc/CoordinatorArbitration.pdf`, `diag/logs/*.log` (real captures for parser testing).
- `esphome-econet/esphome-econet` — `components/econet/econet.cpp`/`.h` (14-byte header, `READ=0x1E`/`WRITE=0x1F`, CRC-16/ARC), `econet_base.yaml` (38400 baud, DE pin, 30 s poll — plumbing pattern).

**Specifications (NEMA paywall):** CT-485 Data Link spec, CT-485 API Reference, Generic Application spec — nema.org. Freely viewable: CT-LWP 2.0 spec (docplayer) covers LWP node types / blower-motor demand mapping.

**Community/empirical:** [openHAB ClimateTalk thread](https://community.openhab.org/t/hvac-climatetalk-protocol/8367) (real captures, SessionID/MAC handling, multi-stage furnace+HP), [Net485 issue #1](https://github.com/kpishere/Net485/issues/1) (design discussion, ΔT-rise safety note), [climate-talk-web-api](https://github.com/rrmayer/climate-talk-web-api) (message examples), [HA EcoNet RS-485 thread](https://community.home-assistant.io/t/rheem-econet-rs485-integration-is-possible/465301) (wiring/pinout experience).

---

### TL;DR for the C++ implementation

Target **9600 8N1** ✅(confirmed on the wire); frame = **10-byte header + payload + 2-byte Fletcher (seed 0xAA, mod 0xFF)** ✅; **frame by declared length (header[9]+12), NOT by idle gaps** — real traffic runs back-to-back with no inter-frame silence (✅ field-proven; gap framing lost >98 % of frames); hold bus idle **100 ms** before TX with **300 µs** DE pre/post. Parse **msgType (offset 7)**; for writes use **Set Control Command (`0x03`)** with command code in **offset 4** — **`HEAT_DEMAND=0x64`, demand byte = percent × 2, timer at payload[12], demand at [13]** ✅(§5a — the TX gate evidence exists: 2026-07-08/09 capture). Measured OEM discipline to imitate: refresh nonzero demands every 5 min against a 6-min timer, re-assert fast (+10/40/60 s) at cycle start, end cycles with an explicit demand-0, never refresh zero demands, answer commands with echo + 17-byte ACK1 form, and stay SILENT on token polls unless carrying data. Read live data via **Get Status (`0x82`)** and **DMA Read (`0x9D`)**; faults arrive unsolicited as **`0x05` pushes with ASCII text** ✅ ("HPC OPEN"/"LPC OPEN" observed) and zeroed-payload clears. Node IDs on this install ✅: `0x01` = thermostat, `0x02` = furnace IFC, `0xFF` = coordinator. The bus is **coordinator/token polled** — sniff-only is safe; writing requires replacing the OEM thermostat and respecting the token, the per-channel demand watchdog, the heat/cool mutual-exclusion interlock (ours, not the bus's), compressor timers, and the equipment's independent safeties. Remaining unknowns before TX: token-acquisition/arbitration details and enumeration handshake (AutoNet join as a new node).
