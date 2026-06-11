# Next Steps

A plain-language, in-order guide from where the project stands today to a working
thermostat. Each step says **why** it matters, **what you need**, exactly **what to
do**, and **how you know you're done**. No prior knowledge is assumed.

**Where things stand today (2026-06-11):** All the software that can be written
without touching the furnace is written and tested (364 automated tests pass). The
house has been partially inspected: the wall thermostat is a Dettson **R02P032**
(the "communicating" kind — it talks to the furnace digitally over two data wires),
the outdoor unit is a Gree **FLEXX 4–5 ton heat pump**, the furnace is a Dettson
**105,000 BTU**, and the wall cable has **6 conductors** (4 in use + 2 spares).
What remains is physical work: one more inspection, ordering parts, listening to
the furnace's digital conversation, and only then — carefully, behind several
safety gates — talking on it.

> ⚠️ **The one rule that never changes:** the furnace is a gas appliance. Until
> step 10, nothing we build ever *transmits* to it — we only listen. Listening
> cannot affect the furnace in any way.

---

## Step 1 — Finish the equipment inspection (the "blower-door check")

**Why:** One question remains about how your system is wired: how does the
communicating thermostat control the heat pump? The answer is almost certainly a
small "interface board" inside the furnace cabinet, and seeing it tells us exactly
which messages to look for later. This step costs nothing and shapes everything.

**What you need:** A phone camera, a flashlight, and a screwdriver. Optionally a
multimeter (for the spare-wire check).

**Do this:**
1. Turn off the furnace at its power switch (looks like a light switch on or near
   the cabinet). Gas stays off-limits — you're only looking.
2. Remove the lower front panel of the furnace (the "blower door"). It usually
   lifts off or has two screws.
3. Look for a **small circuit board that is not the main furnace board**, with:
   a telephone-style **RJ-11 cable** running to the main board, and screw
   terminals labeled **COND-1, COND-2, COND-3** with wires leaving toward the
   outdoor unit. That's the interface board (Dettson kit K03085 or K03081).
4. **Photograph everything**: the interface board (close enough to read its
   label and the positions of any small DIP switches), the main board's terminal
   strip with all wires attached, and any wiring diagrams glued inside the panel.
5. Find DIP switches **S4-2 and S4-3** on the main furnace board and note their
   positions (expected: both OFF, meaning "communicating thermostat").
6. If you have a multimeter: find where the wall thermostat cable arrives and
   check the **two spare conductors** for continuity to the wall plate (twist them
   together at one end, measure resistance at the other). Note their colors.
7. ⚠️ **Never connect anything to the terminal labeled V or V/W2** — applying
   24 V to it damages the furnace board. You're only looking and photographing.
8. Put the door back, restore power.

**Done when:** every box in GitHub issue **#24** is checked and the photos are
saved (drop them in `captures/` or attach them to the issue).

---

## Step 2 — Order the bench parts

**Why:** The first hardware milestone is a "sniffer" — a $30 gadget that listens
to the furnace's digital chatter and shows it on a web page. Everything it needs
is in the shopping list document.

**What you need:** The minimum-viable cart in [`docs/ORDERING.md`](docs/ORDERING.md)
(≈ CAD $180–200 landed, and much of it is reusable for the final build):
- **ESP32-DevKitC** board (the small computer)
- **RS-485 transceiver module, 3.3 V** (the listening adapter — the Teyleten
  auto-flow module is fine for listening)
- The **AC/DC power module, fuse, MOV** (only needed later — for bench listening
  you can power everything from USB)
- DS18B20 temperature sensor, wires, terminals, enclosure
- The **$12 logic analyzer** (optional but recommended — it's the "second
  opinion" tool if anything looks odd)

You do **not** need the touchscreen display board yet (that's step 9), and you do
**not** need to buy any Dettson thermostat — your installed R02P032 is both our
reference and the fallback.

**Done when:** the parts are on your bench.

---

## Step 3 — Flash the sniffer firmware from your browser

**Why:** The sniffer program is already written. You install it onto the ESP32
with nothing but a USB cable and Chrome — no programming tools needed.

**What you need:** The ESP32 board, a **USB data cable** (some cables are
charge-only and won't work), and Chrome or Edge on any computer.

**Do this:**
1. In this repo, run `python3 tools/release.py` once (or use the files already in
   a GitHub Release), then open `web/installer/index.html` in Chrome.
2. Plug the ESP32 into USB.
3. On the page, choose **Sniffer**, click **Connect**, pick the serial port that
   appears (usually "CP210x" or "CH340"), and click Install. Wait ~2 minutes.

**Done when:** the page reports success. (Optional check: the installer's serial
console shows a heartbeat line every 10 seconds.)

---

## Step 4 — Bench-check the sniffer console

**Why:** Before going near the furnace, confirm the sniffer's web console works on
the bench — so at the furnace you're only debugging one thing (the wiring).

**Do this:**
1. With the ESP32 powered by USB, look for a Wi-Fi network named
   **`dettson-sniffer`** on your phone or laptop and join it.
2. Open `http://192.168.4.1` in a browser. You should see the console: counters,
   baud status, a pause button, an annotation box.
3. Counters will sit at zero — correct, nothing is connected yet.

**Done when:** the console page loads and updates its uptime/heap numbers.

---

## Step 5 — Connect the sniffer to the furnace (listen-only)

**Why:** This is the first contact with the real system. The connection is
designed so the gadget is physically incapable of transmitting — the firmware
never drives the transmit pin. The furnace cannot tell it's being listened to.

**What you need:** The flashed ESP32 + transceiver module, two short wires, and a
USB power bank (simplest power for the first session).

**Do this:**
1. Read [`docs/04-safety.md`](docs/04-safety.md) §1 and the tap procedure in
   [`docs/03-hardware-wiring.md`](docs/03-hardware-wiring.md) §2 once, fully.
2. Furnace power **off**. Photograph the terminal strip again before touching it.
3. Connect transceiver **A** to furnace terminal **1**, transceiver **B** to
   terminal **2** — *in parallel*: the existing thermostat wires stay exactly
   where they are; you're adding two more wires to the same screws. Keep your
   wires as a twisted pair, under ~30 cm.
4. Do **not** add any resistors yet, do **not** touch R, C, W, Y, or V terminals.
5. Wire transceiver→ESP32 per the pin table in `docs/03` §3 (RO→GPIO16, plus
   3.3 V and GND).
6. Power the ESP32 from the USB power bank. Furnace power back **on**.
7. Join `dettson-sniffer` Wi-Fi, open the console.

**Done when:** the frame counter climbs and the console settles on **9600 baud**
with the "Fletcher OK" (valid message) counter increasing. You are now watching
your thermostat and furnace talk to each other in real time.

---

## Step 6 — Run the capture campaigns

**Why:** We need recordings of specific conversations — especially the one where
the thermostat tells the furnace *how hard to fire* (the modulation command).
Those recordings are the key that unlocks safe transmitting later.

**What you need:** The running sniffer, the console open on your phone, and the
campaign list in [`captures/README.md`](captures/README.md).

**Do this (for each campaign):**
1. Type a note in the console annotation box describing what you're about to do
   (e.g. `note raising setpoint to force high fire`), so the recording is
   self-documenting.
2. Do the thing at the wall thermostat: raise the setpoint high and let the
   furnace climb through its firing range; change modes (Off → Heat → Cool →
   Auto); trigger a cooling call; force a defrost cycle if you can find it in the
   thermostat's service menu ("FO 3 Force Cycle"); power-cycle the furnace once
   to capture the network joining sequence.
3. After each session, click **Download capture** in the console and save the
   file into `captures/` using the naming convention in `captures/README.md`
   (e.g. `heat-call-walk-2026-06-12.txt`).

**Heads-up:** cooling and defrost campaigns need the matching season/weather —
capture what you can now, the rest when the weather cooperates.

**Done when:** you have at least: one full heat-call walk from low to high fire,
one mode-change session, one power-cycle (network join) session — and whatever
cooling/defrost the season allows.

---

## Step 7 — Analyze the captures

**Why:** This turns recordings into the "field dictionary" — the confirmed map of
which byte means what. It resolves the one deliberate blank in the firmware: two
candidate layouts (called variant A and B) exist for the demand message, and the
transmit code **refuses to run** until a real capture proves which is right.

**What you need:** The capture files and a computer with Python 3.

**Do this:**
1. `python3 tools/ct485_decode.py --summary captures/<file>` — shows who's
   talking (node addresses, message types).
2. `python3 tools/ct485_decode.py --diff captures/heat-call-walk...` — lines up
   the demand messages from your low-to-high-fire walk so the byte that changes
   with firing rate stands out.
3. Work through the "done when" checklist in
   [`docs/05-firmware-plan.md`](docs/05-firmware-plan.md) Phase 2: confirm the
   baud, the furnace and interface-board node addresses, who the coordinator is,
   the demand payload layout (A or B), and the modulation byte (the value should
   be exactly 2× the percentage shown on the thermostat).
4. Record findings in the field dictionary and in issue #11/#27.

**Done when:** every Phase 2 done-criterion in `docs/05` is met. **This is the
gate** — nothing transmits until this is complete.

---

## Step 8 — Bench-run the thermostat firmware (still transmit-disabled)

**Why:** The full thermostat application already exists and runs with its output
"disconnected" — it computes everything and logs what it *would* send. Running it
against your Home Assistant for a few days proves the brains before the mouth is
ever connected.

**Do this:**
1. Copy `src/thermostat_secrets.h.example` to `src/thermostat_secrets.h` and fill
   in your Wi-Fi and MQTT broker details (this file never leaves your machine).
2. Flash the **Thermostat (bench)** image from the web installer (or
   `pio run -e thermostat -t upload`).
3. In Home Assistant, watch the auto-discovered `climate.dettson_hvac` entity and
   friends appear. Install the starter package from [`ha/README.md`](ha/README.md)
   (schedules, vacation, filter reminder, alerts, presence-away).
4. Feed it a room sensor (any HA temperature sensor — see `docs/06`), set
   setpoints from the app, and watch the logs show the demands it would send.

**Done when:** it runs for several days doing sensible things in the log, and the
HA app controls it (modes, dual setpoints, presets, holds) to your satisfaction.

---

## Step 9 — Build the wall unit

**Why:** The production controller is a wall-mounted touchscreen replacing the
R02P032 at the same spot, using the same 4 wires (plus your 2 spares for future
use). The platform decision is made (custom firmware + LVGL — see `docs/08`);
the remaining work is the screen UI (issue #37) and the small safety carrier
board (issue #41).

**Do this:**
1. Order the display hardware from the HMI section of `docs/ORDERING.md`
   (Guition JC4827W543C ≈ CA$25, mounting parts; prefer the 16 MB flash variant).
2. Build/assemble the carrier: the manual-DE/RE transceiver and the hardware
   watchdog (the circuit that forces silence if the software ever hangs) per
   [`docs/03-hardware-wiring.md`](docs/03-hardware-wiring.md) §8.
3. The UI work is the next big software wave (issue #37) — kick it off any time;
   it doesn't depend on the furnace.
4. Run the Phase 3 timing bench test (issue #28): with the screen busy, confirm
   the board still answers the bus fast enough.

**Done when:** the wall unit runs the bench firmware with the touchscreen, and
the #28 timing test passes.

---

## Step 10 — First transmission and commissioning (the careful part)

**Why:** Everything before this was reversible and risk-free. This step replaces
the OEM thermostat with ours. It follows written gates, one at a time, and the
OEM thermostat goes in a drawer — not in the trash — as the instant fallback.

**Do this:**
1. Re-read [`docs/04-safety.md`](docs/04-safety.md) in full and the commissioning
   chapter of the Installation Manual (`docs/manuals/INSTALLATION_MANUAL.pdf`).
2. Confirm the gates: field dictionary complete (step 7), offset variant set,
   OEM thermostat physically disconnected (two thermostats on one bus is
   undefined behavior), CO alarm in the house, rollback procedure printed.
3. Enable transmission (deliberate build flags — see `platformio.ini` comments),
   starting with the lowest possible request: 40% fire, hard-clamped.
4. Verify the furnace acknowledges (ACK) and responds, then work through the
   **commissioning matrix** in the Installation Manual: every equipment type ×
   every failure (pull the bus mid-call, hang the controller, kill a sensor,
   power-cycle) must end with the equipment safely idle.
5. Have a licensed HVAC technician review the installation — required by our own
   safety doc, and worth every dollar.

**Done when:** the commissioning matrix is signed off and the house is heated and
cooled by firmware you can read.

---

## Quick reference

| If you want to… | Look at |
| --- | --- |
| See what's left, by phase | [GitHub issues](../../issues) + milestones |
| Understand the system design | `docs/01-architecture.md` |
| Understand the protocol | `docs/02-protocol-climatetalk.md` |
| Wire anything | `docs/03-hardware-wiring.md` + Installation Manual |
| Stay safe | `docs/04-safety.md` (read first, always) |
| Operate the finished thermostat | `docs/manuals/USER_MANUAL.pdf` |
| Compare with Ecobee | `docs/07-ecobee-gap-analysis.md` |
