# SlyTherm Code Review — 2026-07-16

Full-codebase review of the LIVE SlyTherm controller (Dettson dual-fuel, CT-485)
at `v1.2.1`. Six parallel specialist passes: safety/control, CT-485 protocol,
network/OTA/security, sensors/HA, UI/RemoteLink, build/release. Every finding
below was verified by reading the cited code; line numbers are against the tree
at commit `0ed78b5`.

**Bottom line:** the core engineering is strong — the safety libraries, the
CT-485 frame layer, the OTA verify-before-activate flow, and the JSON parsers
are all careful and well-tested. The findings that matter are a small number of
*integration/glue* defects (the code between the well-tested libs and the wire),
plus two security exposures and one class of retained-topic config bug that has
already been filed once (#155) and recurs in several places.

Severity legend: **P0** = live-safety, fix before next release · **P1** = safety
or security, fix this season · **P2** = correctness/robustness/hardening · **P3**
= minor. Each item notes a filed issue number where one was opened.

---

## P0 — Fix before the next release

### P0-1 · Mode flip mid-cycle can latch the system into permanent no-heat AND no-cool  ·  issue #157
`lib/DemandArbiter/DemandArbiter.cpp:42-46`, `:30`; `src/main_thermostat.cpp:2789-2796`;
`lib/CompressorGuard/CompressorGuard.cpp:94`; `lib/DemandShaper/DemandShaper.cpp:293-305`.

A single ordinary user action reproduces it. While the AC is in the first ~13 min
of a run (inside `StagedCoolShaper`'s 780 s demand-level min-ON, so `shape(0)` keeps
emitting 30 %), the user flips **COOL→HEAT** and raises the heat setpoint (or an HA
automation / EM-HEAT toggle does). `evalSimple` starts a heat call immediately —
the AUTO-changeover dwell gate does **not** apply to explicit mode changes. In the
same cycle gas lights (or, above the balance point, `HpRelayShaper` starts because
`CompressorGuard::requestStart()` returns *kAlreadyInState* — the still-running cool
call holds the shared guard "running", so the heat shaper piggybacks). The request
now carries heat-family > 0 **and** `coolPct` = 30 → `conflictHeatCool` →
`invariantAlarm_` latches and forces every channel to zero.

`clearInvariantAlarm()` is declared (`DemandArbiter.h:70`) but **has zero callers in
`src/`** — verified. `hasAckAlarms` clears only the supervisor registry and CT-485
alarms, not this latch. The house then has no conditioning of any kind until a power
cycle. The glue comment at `main_thermostat.cpp:2790` ("Simultaneous nonzero requests
are unreachable…") is wrong for manual mode changes, in both directions.

**Fix (do all three):**
1. In the glue, gate the incoming family on the opposite shaper being fully off
   (zero `gasReq`/`hpReq` while `gCoolShaper.on()`; zero `coolReq` while
   `gGasShaper.lit() || gHpShaper.on()`), or treat a call-family change as a
   safety-class stop for the outgoing shaper.
2. Wire `gArbiter->clearInvariantAlarm()` into the deliberate-ack path so any latch
   is recoverable from the panel/HA — not only by a ladder and a screwdriver.
3. Consider evaluating the conflict on the *post-dwell-suppression* values so a
   min-ON hold vs. a new call is self-healing rather than latching.

---

## P1 — Fix this season (safety + security)

### P1-1 · Gas 4-hour max-runtime trip has no recovery path while the call persists  ·  issue #158
`lib/DemandShaper/DemandShaper.cpp:53-57, 71-76`; `kGasMaxRuntimeS = 14400` (`DettsonConfig.h:129`).

On a design-cold day a modulating furnace legitimately runs > 4 h continuously. The
trip extinguishes the burner; the room cools; PID output stays saturated ≥ 100 % (never
falls to the ≤ 35 % `offThresh` that is the *only* thing that clears `runtimeTripped_`);
the call never ends because the room can't reach setpoint without heat. Result: an
unattended house loses all heat with only an MQTT alarm. The existing test
(`test_gas_max_runtime_trips_and_recovers_only_after_call_ends`) enshrines the
non-recovery. **Fix:** auto-relight after `minOffS` with a bounded retry counter
(trip → cool-down → relight; latch only after N consecutive trips), matching the
"HP: alarm only" intent noted on the same config line.

### P1-2 · HP-heat is selectable but unmapped on the CT-485 build — no heat / 2 °C sawtooth across most of the heating season  ·  issue #159
`src/main_thermostat.cpp:583-586` (actuator drops `hpHeatPct`), `:2675-2678` (selection);
`lib/DualFuelArbiter/DualFuelArbiter.cpp:187-205` (escalation).

Above the balance point (OAT −8…+10 °C — most of the season) a heat call selects the
heat pump, but `Ct485Actuator` drops the HP-heat demand ("no confirmed bus mapping"),
so the furnace does nothing until the room droops ~2 °C and gas escalation fires. Worse,
escalation can barely fire: it wants `hpDemandPct ≥ 95` sustained 1800 s, but the fed
value is the 0/100 duty output whose max on-phase is 1200 s < 1800 s, so `droopTiming_`
resets every off-phase — escalation only triggers at full-scale 2.0 °C error, not the
intended 1.0 °C droop. Net: a persistent ~2 °C sawtooth and multi-hour no-heat windows,
plus phantom HP-runtime poisoning the #143 COP record. **Fix before heating season:**
force gas when the actuator can't express HP heat (config flag), *or* map the bus HP
command; and feed escalation the pre-duty-cycle requested percent.

### P1-3 · OTA signatures are not version-bound — forced downgrade to any older signed image  ·  issue #160
`tools/release.py:180-201` (signs image bytes only), `src/ota_client.cpp:162`, `lib/OtaCatalog.cpp:238-245`.

The signature covers only the raw image; nothing binds it to a version. Anti-downgrade
is enforced only against the *catalog-declared* version, which an attacker controls. A
tampered catalog offering `{version:"99.0.0", appUrl:<old release's real bin>,
sha256:<its real sha>, sig:<its real signature>}` passes both the sha and signature
checks and installs an old, validly-signed firmware — reintroducing any bug fixed since
(e.g. a pre-safety-fix build). Reachable because the mirror path is plain HTTP and the
mirror is selectable via **unauthenticated** MQTT (`slytherm/cmd/ota_mirror`). **Fix:**
sign `sha256 || version` (or a signed manifest), and on-device require the staged image's
embedded `esp_app_desc_t` version to equal the resolved catalog version before
`Update.end(true)`. Extends the #115 OTA-hardening work.

### P1-4 · Live WireGuard preshared key committed to the repo  ·  issue #161
`docs/opnsense-vpn.md:37` (committed in `34bee6f`) contains the PSK
`hKe5MCC92SB82pDQTfaW2hIXlwH/DuMmShatIkutkss=` — byte-for-byte the deployed
`SLYTHERM_WG_PSK` in `src/remote_secrets.h:13`. The repo is public. The device
*private* key was not committed (good). **Fix:** rotate the PSK, replace the doc value
with a placeholder, and — since the repo is public — scrub history.

### P1-5 · CT-485 ACK/NAK correlation reads the wrong payload offset — NAK safety handling is inert on the live bus  ·  issue #162
`lib/Ct485Thermostat/Ct485Thermostat.cpp:327, 348-350`; contradicted by the only real
captured ACK fixture `tools/test_ct485_decode.py:17` and the Python decoder
`tools/ct485_decode.py:637-639`.

Firmware treats `payload[0]` as the ACK/NAK code, but the real Net485 capture shows the
response *echoes the request* — `payload[0]` is the command code (0x64), and the ack/nak
byte is at `payload[2]`. So every real response falls into the `default:` branch
("unknown code: treat as delivered"). ACKs work by accident, but **NAK1 retransmit and
the NAK2 two-master stop (docs/02 §9) can never fire** — a furnace rejecting our ownership
is logged as delivered and the demand kept refreshing. The C++ tests synthesize the code
at `payload[0]`, pinning the wrong assumption. **Fix:** confirm the real Dettson response
layout from slylog captures, read the ack/nak from the confirmed offset, and make the
unknown-code default conservative (alert on unknown codes for demand frames).

### P1-6 · Shipped build has no local temperature source — a broker/Wi-Fi outage means no heat in winter  ·  issue #163
`src/main_thermostat.cpp:148-155, 3267-3273`; `kFusionCoastMaxS = 120` (`DettsonConfig.h:202`).

The default/UI builds register no slot-0 sensor (the SHT31-D lives only in
`main_hw_probe.cpp`, not the app). All fusion inputs arrive over MQTT. Broker down →
every remote goes stale → 120 s coast → `kAlarmAllBad` → invalid → control loop drops to
no-demand. The MQTT-stale fallback bounds only *setpoints*, never conjures a temperature,
and the degraded heat/cool floors are unreachable without a local rung. Router/HA box dies
while owners are away in January → the furnace never fires again until connectivity returns.
**Fix:** wire the hardwired SHT31 into SensorFusion slot 0 (issue #107), and/or raise a
loud, distinct "no temperature source" alarm on panel + HA. Related to #153 but distinct
(that's transient dropouts; this is the absence of any local floor).

### P1-7 · Roster replay silently clobbers HA-set calibration offsets  ·  issue #164
`src/main_thermostat.cpp:804` (`s.offsetC = e.offsetC` unconditionally), `:2040`.

Offsets set via the HA number entity live only in RAM — never persisted to NVS (unlike
participation, which got exactly this fix). On reconnect/HA-restart the retained roster
republishes with default 0.0 and the user's calibration silently vanishes, then the
heartbeat rewrites the HA entity back to 0.0. This is the **same disease as #155**, and it
recurs in four more places: display-name collision (`SensorRoster.h:36-44`), ghost retained
discovery on rename (`main:1230-1238`), slot leak/≥24-char-id no-fuse (`main:722-733, 919-921`),
and the HA presence-away blueprint's non-existent default preset. **Fix:** migrate offsets
(and the others) onto the durable wire-id-keyed `SensorParticipation` pattern; add the
regression test mirroring `test_roster_replay_keeps_off_sensor_off`.

### P1-8 · Remote node uses PubSubClient and shared roster arrays from two tasks with no lock  ·  issue #165
`src/remote_mqtt.cpp:665, 683, 690-692` (publish from UI task) vs `:616-617` (`loop()` on
Arduino task); `gRows[]` written at `:312, 334-374` and read/written at `:656-668, 148-167`.

PubSubClient is not thread-safe (shared packet buffer + WiFiClient writes). A fan/participation
tap landing while `loop()` is mid-packet interleaves TCP writes → corrupt frame → broker drops
the connection; a tap during a roster `memcpy` lands on a half-copied row and loses the pending
command — the exact "press on flaky link silently lost" case the pending machinery exists to
prevent. The **Controller codebase documents this exact rule** (`main_thermostat.cpp:334-336`)
and the Remote violates it. **Fix:** mirror the Controller — UI hooks set a pending struct/flag,
`remote_mqtt::loop()` does all publishes; guard all `gRows` access with `gModelMux`.

---

## P2 — Correctness, robustness, hardening

**Control / safety**
- **No low-ambient cooling lockout** (`main:2683-2685`): COOL on a cold sun-loaded morning
  runs the compressor at low condensing pressure. Add an OAT floor (~10-13 °C) beside the
  indoor floor — OAT is already in scope there. · issue #166
- **CT-485 deadman fires in 60 s, not the specified 30 s** — double-thresholded
  (`main:2767` pre-thresholds `busAlive`, then `SafetySupervisor.cpp:215-231` waits another
  full `kBusDeadmanS`). Feed the supervisor raw liveness. · issue #167
- **Every HA command arms the compressor min-OFF bypass** (`main:2097-2100`), including the
  5-preset daily *scheduler* writes — defeating #140 long-cycle hygiene at exactly the
  big-error moments. Arm only for panel/Remote-intent/manual paths. · issue #167
- **Defrost + gas-temper conflict latches the arbiter** (`DualFuelArbiter.cpp:140-150`) — dead
  in the CT-485 build, a loaded gun for Case B relay hardware. Suppress `temperRequest` when
  the source is gas. · covered by #157 (same latch mechanism)
- Case B blower-proof gating is a chicken-and-egg deadlock (`main:620-622`); PID integrates
  while demands are blocked (`main:2668`); external-WDT layer inert (`kWdtPetPin = -1`). Latent.

**Security / network hardening**  ·  issue #168
- Furnace is actuable over **unauthenticated plaintext MQTT** (mDNS saves empty user/pass);
  bench commands (`txturn_stress`, `sniff`, `sleep`, `ota_mirror`) subscribed on the live
  controller. Require broker auth + per-client ACLs; strip bench commands from release builds.
- **Unauthenticated coredump server on :8082** on every build — full RAM/stack disclosure (may
  contain Wi-Fi/MQTT creds) + unauthenticated `ERASE` anti-forensics. Gate behind a build flag/token.
- **Unauthenticated telnet log on :23** leaks SSIDs, broker host, VPN state. Gate/bind to mgmt VLAN.

**Build / release**  ·  issue #169
- **No CI on push/PR** — the only workflow is tag-triggered, so fixes accumulate on `main`
  untested until release day (pressuring `--skip-gate`). Add a push/PR `ci.yml` running native
  tests + the embedded compile set.
- **Unpinned `lib_deps`** (ArduinoJson/PubSubClient/OneWire/lvgl carets) → CI cache vs fresh
  local checkout silently build different code for the same tag. Pin exact versions.
- **The 2026-07-15 stale-mirror incident root cause is still live** in `ota_mirror_sync.sh:38`
  (`[ -s "$f" ] && continue` + no sha256 verify against the catalog). Verify every asset's sha256
  after catalog sync and re-fetch on mismatch. · issue #170
- **Nothing prevents re-publishing a tag's assets** (`release.yml:92-101`, no "release exists"
  guard) — a workflow re-run recreates the incident. Add a `gh release view … && exit 1` guard.
  · issue #170
- `main` is unprotected (force-push can delete CI's catalog commit); signing-key-missing only
  *warns* (`release.py:185-188`) → ships a fleet-wide un-appliable update; asset upload lacks
  `fail_on_unmatched_files`. Enable branch/tag protection; `die()` on missing key when `--require-tag`.

**Sensors / HA**  ·  issue #171
- `publishSnapshot` diff cache (192 B) is smaller than the fusion payload (224 B) → tail changes
  (occupied/dominant) suppressed up to 60 s (`main:1285-1292`).
- `participants[]` assembly can emit **invalid JSON** on truncation (`main:2417-2426`) → HA
  template errors, fusion sensor goes unknown.
- Invalid fusion publishes **`temp:0.00`** (`main:2451-2454`) poisoning history; `current_temp`/
  `outdoor_temp` freeze silently instead of going unavailable.
- The third OAT rung (HA weather) is **never fed** by any shipped automation (`docs/06` requires
  `slytherm/cmd/outdoor_temp`; grep of `ha/` finds none) → CT-485 OAT hiccup > 30 min locks out
  cooling. Add the weather-bridge automation.
- HA `repeat.for_each` bare `condition:` (`slytherm_sensors.yaml:83-98`) can halt the whole bridge
  run on one unavailable sensor — use `if/then`. Battery telemetry parsed then discarded.

**UI / RemoteLink**  ·  issue #172
- Auto-relock (120 s) fires during active use because ordinary touches never call
  `gUi.touchActivity()` (`UiModel.cpp:409-423`; `uiNoteTouch(){}` empty) → panel relocks
  mid-adjustment.
- Echo-suppression drops the controller's *corrected* echo wholesale (`remote_mqtt.cpp:255`)
  → Remote shows values the Controller rejected until an unrelated echo heals it.
- No data-age deadman on the Remote's controller echo (`remote_mqtt.cpp:194`) → stale temperature
  shown as live on a device whose whole job is showing the current temperature.
- Participation toggle silently no-ops for room names ≥16 chars (`ui_main.cpp:36` buffer vs
  `kSensorNameLen=24`) — same class as #155. Screenshot server blocks the UI task for seconds,
  unauthenticated, bypassing the lock (`ui_overlays.cpp:631-659`).

---

## P3 — Minor (see per-domain notes)
Torn diagnostic reads without the mutex (`main:2941-2975`; `remote_mqtt` gRows; `gGraphIn`);
`VacationState` NVS blob lacks magic/CRC; `changeReason` stale across cycles; PIN-backoff shown
as "Wrong PIN"; 8th sensor row never rendered; `rowEquals` ignores `dominant`/`lastOccAgeS`;
RemoteLink rejects (rather than truncates) over-long preset names; various `curl`/`grep`-parsed
JSON in shell tooling; repo-root binary clutter (one tracked `.jpg`, untracked strays not
gitignored). Full detail in the per-domain review transcripts.

---

## Verified clean (no action)
Frame encode/decode/accumulator (adversarial coverage excellent); CT-485 parser bounds; CompressorGuard;
ModeStateMachine; SafetySupervisor unit logic; PidShaper anti-windup; timekeeping (64-bit `esp_timer`,
no 49.7-day `millis()` wrap in any control timer); OTA verify-before-activate + pinned-CA TLS (no
`setInsecure` anywhere); HA inbound parsers (NaN/junk rejected); SensorParticipation NVS pattern;
LVGL threading discipline and the P4 port; `release.py` artifact sanity gates; secrets `.gitignore`
hygiene (only the PSK doc leaked); `test_cool_replay` real field-trace replay through the control chain.

## Cross-cutting themes
1. **Retained-topic identity/config clobber (#155's class).** Offsets, name collisions, ghost
   discovery, and slot lifecycle all need the durable wire-id-keyed store the participation fix
   already built. Migrate them onto it rather than patching individually. (P1-7)
2. **Glue is the weak layer.** The libraries are well-tested in isolation; the defects live in
   `main_thermostat.cpp`/`remote_mqtt.cpp` where libs meet the wire and where there are no host
   tests. The highest-leverage test investment is an integration harness over the two-shapers-one-
   guard wiring (would have caught P0-1) and extracting the remote_mqtt reconcile logic into a
   testable lib.
3. **Fail-safe vs. fail-operational.** Several designs correctly fail to no-demand on fault, but for
   a *heating* system in winter, no-demand *is* the hazard (P0-1, P1-1, P1-6). Recovery paths and a
   local temperature floor matter as much as the safe stop.
