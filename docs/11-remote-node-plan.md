# 11 — Remote Node Plan (ESP32-P4 wall unit)

**Status: DESIGN — DECIDED 2026-07-07.** Code-organization plan for splitting SlyTherm
into two hardware roles. Companion to [`01-architecture.md`](01-architecture.md) and
[`05-firmware-plan.md`](05-firmware-plan.md). No code has been written yet; this is the
blueprint.

## Roles

- **Controller** — the furnace-wired board (Waveshare ESP32-S3-4.3B). *All existing
  code so far.* Talks to the furnace over CT-485, owns all HVAC authority, is the sole
  Home Assistant / MQTT-broker face, and fuses every Remote's sensors. **Keeps its wall
  UI.** Lives near the furnace.
- **Remote** — a new wall-mounted node (Guition **JC-ESP32P4-M3**: ESP32-P4 rev v1.3,
  16 MB flash, 32 MB PSRAM, WiFi via on-module **ESP32-C6**). Where a thermostat
  normally mounts. **Clean power only — no RS-485, no CT-485.** Carries the touchscreen
  wall UI, an optional local I²C temp sensor, and an **Rd-03D** presence radar. Talks to
  the Controller **only over the network (WiFi/MQTT) — there is no UART link to the
  Controller, ever.** One or more Remotes per system, and a Remote can be **powered up
  anywhere on the network** (need not sit at a thermostat-wire drop). At the primary wall
  location the thermostat-wire bundle carries **power for the Remote + the SHT31 I²C down
  to the Controller** — those wires are for the thermometer and power only.

## Topology — hub-and-spoke (DECIDED)

```
   HA / MQTT broker
          |
     [Controller]  <— sole HA identity (slytherm_esp32), sole authority, fuses all
      /    |    \
 [Remote][Remote][Remote]   <— spokes; each talks ONLY to the Controller
```

A Remote never publishes Home Assistant entities and never speaks the HA contract. It
speaks a **Controller-internal protocol**. This deliberately dissolves the per-node HA
identity problem: only the Controller has an HA device.

## The gating constraint: the P4 toolchain (do this FIRST)

The reuse story assumes the P4 runs this Arduino/LVGL/PubSubClient/WiFi stack. Today it
**cannot** on this project's platform:

- Installed `platform = espressif32 @ 7.0.1` ships **Arduino-ESP32 core 2.0.17** and has
  **no ESP32-P4 board definition**. P4 needs **core 3.x** → the **pioarduino** platform
  fork (`pioarduino/platform-espressif32`), a different `platform =` line.
- ESP32-P4 has **no radio**; WiFi runs through the **ESP32-C6 over esp-hosted** — a
  different bring-up than the S3's native path. `wifi_prov`'s "synchronous scan on S3"
  workaround may not hold.
- The P4 module's **panel + touch are different silicon** than the S3's RGB + GT911;
  the LovyanGFX/touch config must be redone.

### Phase 0 — prove the stack on COM6 before code-org matters
1. `env:remote_p4` on pioarduino (core 3.x) + a P4 board def (find or hand-roll). Build
   + flash a hello-world over COM6.
2. WiFi-through-C6 (esp-hosted) associates and gets an IP.
3. Panel + touch bring-up: LVGL flush + a touch event on the P4's real display.
4. PubSubClient connects to the broker.

If this stack doesn't stand up, the rest of the plan is moot.

## Code organization — THREE tiers

All 20 `lib/` modules are pure C++17 with time injected as `now_ms` — **zero
`Arduino.h`** — so the split is clean.

### Tier 1 — Pure logic (`lib/`), truly common, compiles verbatim, host-tested
- **UiModel** — the UI data contract (`DisplayState` + `UiIntent`).
- **HaMqtt** — MQTT topic/JSON builders + the sensor/presence wire contract.
- **DettsonConfig** — foundational constants both sides clamp to.
- **ModeStateMachine** — the Remote runs this as an *optimistic local replica* (below).
- **SleepState** — reusable.
- **SensorFusion** — stays on the **Controller only** (the fusion host); the Remote is a
  publisher, not a fusion host.

### Tier 2 — Core-dependent glue (`src/`), source-reusable, recompiles + must be re-validated on core 3.x
- `wifi_prov.cpp` / `mqtt_cfg.cpp` / `telnet_log.cpp` — zero furnace coupling; **re-test
  WiFi provisioning on C6.**
- `slytherm_ui.cpp` — the LVGL **screen-layout** code is one common file compiled into
  both Controller and Remote. Its board glue (panel + touch) is Tier 3.

### Tier 3 — Net-new P4 board bring-up (shares ~nothing with the S3; the bulk of the work)
- P4 panel driver + touch config; pinmap; backlight/reset.
- ESP32-C6 hosted-WiFi init.
- pioarduino platform + P4 board definition.
- **Rd-03D radar UART driver** (net-new).
- **Local I²C temp sensor** — Adafruit **BME688 QT** (@0x76/0x77, +humidity +VOC) *or*
  **SHT31-D** (@0x44/0x45, temp+humidity). Different addresses → **probe both at boot,
  use whichever answers** (no build flag). Tiny `TempSensor` interface, two impls.

## What is SUPPRESSED on the Remote (the "no RS-485" answer)

The Remote gets its **own entrypoint** `src/main_remote.cpp` + `env:remote_p4`, selected
by `build_src_filter` — the same convention that keeps `main_sniffer` and
`main_thermostat` from colliding. Suppression is **"the Remote main never constructs
it,"** not `#ifdef`-ing the 97 KB furnace main. Never linked into the Remote image:

- **All CT-485**: `Ct485Core/Frame/Parser/Thermostat`, `CaptureBuffer`, the `ct485Task`,
  UART2/DE glue, the RS-485 LISTEN sniff path.
- **Demand pipeline**: `DemandArbiter`, `DemandShaper`, `DualFuelArbiter`, `PidShaper`,
  `RelaySequencer`, `CompressorGuard`, `SafetySupervisor`.
- **Actuators**: `Ct485Actuator` / `RelayActuator` / `LoggingActuator`, all relay/sense
  GPIO, the external-WDT pet.
- **The control task** (`controlCycle`) itself — the Remote has no demand cycle.
- **`SensorFusion`, `OutdoorTempSource`, `RecoveryEstimator`** — the Controller's job.

UI-side suppression (keep ONE common `slytherm_ui.cpp`; inject persona via a capability
bit `hasBus` in `DisplayState`):
- Hide Diag's **"LISTEN on RS-485"** button + the CT-485 bus fields when `!hasBus`.
- Stub the five `uiSniff*` `extern "C"` hooks on the Remote.
- `uiToggleSensor` on the Remote forwards to the Controller (the roster lives there).

## The Remote has exactly ONE UART — the radar. NO Controller-link UART, ever.

- **Rd-03D radar UART** — Remote-local sensor. The only UART on the Remote. New driver.
- **There is NO UART between a Remote and the Controller.** The thermostat-wire bundle
  at the wall carries the **SHT31 I²C down to the Controller + power for the Remote** —
  nothing else. Remotes reach the Controller **only over the network (WiFi/MQTT)**, and
  can be powered up **anywhere on the network** (need not be at a thermostat-wire drop).

## Remote↔Controller link — a network-only SEAM whose payload IS the UI contract

Define **one interface**; the payload is the existing UI contract serialized:
**`DisplayState` downstream (Controller→Remote), `UiIntent` upstream (Remote→Controller).**

- **Single transport — MQTT private topics** (NOT the HA contract), over WiFi. No
  second transport. **RESOLVED 2026-07-07 (issue #104, Controller side landed):**
  the echo is **one shared retained topic**, not per-`<id>` — every Remote mirrors
  the same Controller-owned truth (hub-and-spoke), so there is nothing to
  distinguish per Remote:
  - `slytherm/remote/state` — retained `DisplayState` echo (downstream, shared).
  - `slytherm/remote/<id>/intent` — `UiIntent` (upstream, per Remote, NOT
    retained — live-only, see "NO intent queuing" below). `<id>` is the
    publishing Remote's own identifier; the Controller dedupes by a
    Remote-local monotonic intent id (starts at 1) to tolerate retry-on-no-ack
    redelivery. (Broker discovery + identity binding below.)
- **Trade-off accepted:** with no UART, there is **no local-control failover** — if the
  network/broker is down, a Remote cannot push changes until it returns (it degrades to
  view/queue). The continuity guarantee therefore rests entirely on the **Controller's
  autonomy**: it keeps running the furnace on last-known authoritative state using its
  **hardwired SHT31**, no Remote required.

The Remote also sends its sensor reading (temp / humidity / presence) to the Controller;
the Controller fuses it as a roster participant (the existing sensor contract, no
`SensorFusion` change).

## Discovery & fault tolerance (DECIDED) — bind to identity, never to IP

**Auto-discovery of the Controller:**
- **Broker-mediated rendezvous** — Controller and Remotes both find the MQTT broker via
  **mDNS** (the Controller already does this, `main_thermostat.cpp:1056-1074`; Remotes
  reuse it). They meet on the broker, so neither box's IP matters to the other.
- Controller announces a **retained identity heartbeat**. **RESOLVED 2026-07-07
  (issue #104, Controller side landed):** PubSubClient supports exactly one Will
  per connection, and it is already bound to `slytherm/availability` (HA's
  online/offline + LWT — unchanged). So the sketch above (a cid-suffixed topic
  with its *own* LWT) is not implementable without a second broker connection;
  the deviation actually shipped:
  - `slytherm/controller/status` (fixed, no `<cid>` in the path) — retained,
    republished on every connect: `{"cid":"<mac-hex>","status":"online",
    "version":"<fw>"}`. Identity/version ONLY — it has no offline/LWT half.
  - **Liveness is `slytherm/availability`** (existing online/offline + LWT), not
    `controller/status`. A Remote binds its Controller's **`cid`** (MAC-derived,
    NOT IP; last 3 MAC bytes, lowercase hex, no colons — e.g. `8d82f4`) from
    `controller/status` on first sight, then tracks liveness via `availability`.
  - This still satisfies "bind to identity, never IP" and the degraded-state
    machine below (broker reachable? + availability-offline ≥5 min? = Controller
    Offline) — it just moves the LWT-backed signal to the topic that already
    carries it, rather than duplicating a Will PubSubClient can't provide twice.
- Everything is broker-mediated by identity, so DHCP/IP moves on either box are
  transparent (`.13` reservation is convenience, not a dependency). No IP is ever
  hard-coded Remote↔Controller.
- **Boot behavior:** each Remote powers up (anywhere on the network) into its own
  **"Discovering Controller…" splash**, held until it joins WiFi, finds the broker, sees
  a live `slytherm/controller/+/status`, and binds a `cid`. On timeout it shows a
  retry/"no Controller found" state rather than a dead UI.

**Fault tolerance when the Controller is unavailable / the link is flaky:**
- **Retained authoritative state** — Controller retains setpoints/mode/hold/fused-temp/
  presets; a reconnecting or rebooting Remote **instantly restores last-known state**.
- **NO intent queuing.** Edits are only ever sent live and reconciled; an edit that
  can't be confirmed simply reverts on the next reconcile — the Remote never banks a
  queue to replay. (Removes stale-replay surprises entirely.)
- **Degraded-UX state machine (DECIDED), keyed on two signals — broker reachable? and
  `slytherm/controller/<cid>/status` live? — with a 5-minute sustained threshold so
  blips/Controller-reboots don't nag:**
  - **Brief blip (< 5 min):** keep last-known UI, retry quietly; unconfirmed edits revert.
  - **Network Unavailable (WiFi *or* broker unreachable ≥ 5 min):** full-screen blocking
    panel — **"Network Unavailable — Retrying (attempt N)"** + a **[Reboot]** button. No
    control surface.
  - **Controller Offline (broker reachable, Controller heartbeat/LWT offline ≥ 5 min):**
    same blocking panel **plus a "Check Controller Power" notice** (network is provably
    fine → the fault is the Controller). + **[Reboot]**.
  - **Retry policy:** **no max — retry forever**; the on-screen counter **increments per
    attempt** (unbounded N, resets on recovery). Cadence **backs off** — don't hammer;
    e.g. ~10 s ramping to a ~60 s ceiling, then steady — so a long outage isn't a tight
    5 s loop. The **[Reboot]** button is the user's escape hatch, not an automatic action.
  - **Recovery:** any layer returning → pull retained authoritative state → normal UI.
    Reconnect uses the same backoff → re-subscribe → pull retained → reconcile
    (**Controller wins**).
- **No failover transport (by design).** The link is network-only; there is no UART.
  If WiFi/broker is down the Remote simply degrades (view + queued intents) until it
  returns. If the Controller itself is down, nothing the Remote can do — and it has no
  CT-485, so it **cannot and will not attempt furnace-control takeover**. Fault
  tolerance = graceful degradation + clean resync, NOT control failover. Continuity is
  the **Controller's** job: it keeps running autonomously on its hardwired SHT31.

## Authority — the Controller owns setpoint/mode/hold truth (DECIDED)

"Local model on the Remote" is an **optimistic replica, not a second authority.**
Split-brain (two nodes each believing they own the target) is the failure mode to avoid.

- **Authoritative state = the Controller.** It creates, **persists (NVS)**, and echoes
  setpoints, mode, active preset, and any hold/override. (Already true today: on-device
  UI taps create a hold (#91); boot restores the persisted hold (#87).)
- **The Remote's local `ModeStateMachine`** exists only for (a) instant UI feedback and
  (b) offline survival.

### Override-with-hold flow (answers "where is the authoritative current setting?")
1. User taps +2° / "Hold" at the Remote → local model shows it **optimistically** + a
   hold pill, and sends a `UiIntent` to the Controller.
2. Controller applies via its `ModeStateMachine`, **creates + persists the hold**, and
   echoes authoritative `{heatC, coolC, holdType, holdRemainS, activePreset}` to all
   Remotes (and reflects to HA).
3. Remote **reconciles: the Controller echo wins.** Suppress echo-reconcile for ~1–2 s
   after a local tap so an in-flight echo can't yank a setpoint mid-adjust. Link down →
   **no queuing**; an unconfirmed edit reverts on reconcile, and a sustained (≥5 min)
   outage blocks the UI behind the Network-Unavailable / Controller-Offline screen — the
   Remote never invents an authoritative hold.

**So: the authoritative source for the current temperature setting — including an
override with a hold — is always the Controller. The Remote holds a reconciled replica.**

## Hardwired temperature backup (Controller-side) — REQUIRED

The Controller must keep a valid room temperature even if the Remote's MCU, firmware,
or WiFi is dead. So the wall temperature sensor is **also hardwired down to the
Controller** — an independent temperature path the Controller owns outright. This is
temperature-only; presence/humidity/VOC stay on the Remote's digital path.

**Fits existing code:** the Controller already has a gated `SLYTHERM_LOCAL_SENSOR`
path = `SensorFusion` **slot 0**, treated as a **degraded fallback** behind fresh
remote sensors (ladder: fused-remotes → single-remote → **local-degraded** → none),
currently OFF. The hardwired backup **re-activates slot 0** and points it at the
wall-wired sensor — no new fusion logic. Fusion prefers the Remote's fresh reading and
falls back to the wired sensor when the Remote goes stale. Authority unchanged
(Controller still owns the control temperature).

**Distinct from the UART failover:** the wired sensor keeps the Controller *sensing +
controlling* if the Remote is offline; it does NOT let the wall user change setpoints
during a WiFi outage (that's the separate optional UART, §"transport seam").

**Sensor — DECIDED: the SHT31-D is hardwired to the Controller.** No DS18B20 on hand,
so that idea is dropped — use parts we have. The SHT31-D mounts at the wall, its I²C
wires run down to the Controller, and the Controller is its I²C master, reading it as
slot 0. SHT31-D is also the right control-temp source (±0.3 °C, no self-heating vs the
BME688's temp-biasing gas heater). If the Remote wants its own local sensor for
display/humidity/VOC, use a **BME688 on the Remote's OWN separate I²C bus** (each bus
single-master → no two-master conflict).

Caveats:
- **I²C over the wall→Controller run.** Measure it: short RV run is OK at 100 kHz +
  strong pull-ups; longer/noisy → add an I²C bus buffer (P82B96 / LTC4311, ~$1) +
  twisted pair (SDA/GND, SCL/GND).
- **Small new code.** The Controller's existing slot-0 read is OneWire/DallasTemperature
  (DS18B20). Feeding slot 0 from the I²C SHT31 is a small new read replacing
  `gDallas->getTempCByIndex(0)` (`main_thermostat.cpp:1969`) — the *same* SHT31 driver
  the Remote uses. Fusion slot-0 + degraded-mode safety are reused as-is.

## Proposed layout

```
platformio.ini            + [env:remote_p4]  (pioarduino platform, core 3.x, P4 board)
                            build_src_filter = +<main_remote.cpp> +<slytherm_ui.cpp>
                              +<wifi_prov.cpp> +<mqtt_cfg.cpp> +<telnet_log.cpp>
                              +<remote_board_p4.cpp> +<sensor_i2c.cpp> +<radar_rd03d.cpp>
                              + fonts/img
src/main_remote.cpp        NEW — Remote entrypoint: WiFi+MQTT+UI tasks, sensor publisher,
                           Controller mirror + optimistic local model. NO control/
                           ct485/actuator tasks.
src/remote_board_p4.cpp    NEW — P4 panel/touch/pinmap/backlight + C6 WiFi init.
src/sensor_i2c.cpp         NEW — BME688/SHT31-D probe-and-select over I²C.
src/radar_rd03d.cpp        NEW — Rd-03D UART glue (parser could be lib/PresenceRadar).
lib/PresenceRadar/         NEW (optional) — pure Rd-03D frame parser, host-testable.
lib/UiModel/UiModel.h      + bool hasBus in DisplayState (persona capability bit).
src/slytherm_ui.cpp        shared; gate Diag LISTEN + bus fields on hasBus.
```

## Phased roadmap

- **P0 Toolchain** — pioarduino P4 env; C6 WiFi; panel + touch; MQTT hello-world on COM6.
- **P1 UI on P4** — port `slytherm_ui.cpp` onto the P4 panel; render a static UiModel.
- **P2 Mirror + optimistic model** — subscribe Controller state → fill UiModel; local
  edits optimistic → send `UiIntent`; reconcile to Controller echo.
- **P3 Sensors** — local I²C temp + Rd-03D radar → send to Controller; Controller fuses;
  verify the Remote's room appears on the Sensors tab. **Re-enable the Controller's
  local-sensor slot 0** fed by the hardwired wall sensor (backup temperature).
- **P4 Discovery & resilience** — "Discovering Controller…" splash; `cid` bind; retained-
  state restore; the 5-min degraded state machine (Network Unavailable "Retrying
  (attempt N)" + Reboot / Controller Offline + "Check Controller Power" + Reboot);
  unbounded backoff retry; no queuing.
- **P5 Multi-Remote** — confirm N Remotes (powered anywhere on the network) as N roster
  participants under one Controller.
