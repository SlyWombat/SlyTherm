# Overnight progress — SlyTherm

## 2026-07-06 session — #86 rework + #87 + #88

Items #86a/b/c, #87, #88. Each committed separately; single hardware build at the end.

- **#86a Night-only deep-blank** — DONE (`0bf79ae`). Backlight now blanks ONLY
  00:00-06:00 local after 15 min idle (`kNightBlankIdleMs`). Outside that window
  ambient stays fully lit and never blanks. `getLocalTime` failure fails SAFE
  (never blank; restore if blanked); clock rolling past 06:00 restores the light.
  Dropped the old all-day 30-min `kDeepIdleMs` blank.
- **#86b Smooth walk drift** — DONE (`0bf79ae`). `ambientShift` rewritten to a
  smooth one-step ping-pong "walk" (`kAmbSteps=6` each way) with a gentle diagonal
  Y drift — no teleporting between grid corners. Cadence `kAmbShiftMs` (15 min);
  full L→R→L cycle = 3 h.
- **#86c Bulletproof measured clamp** — DONE (`0bf79ae`). Walk clamp computed from
  ACTUAL rendered label geometry every drift (`lv_obj_update_layout` +
  `lv_obj_get_width/height`): right/bottom extents over the 4 labels bound the walk
  to `[Xmin,Xmax]/[Ymin,Ymax]`. Content wider/taller than the usable area pins to
  the top-left margin (never negative). Recomputed per drift → no clip for any
  room-name length / temp width.
- **#86 wake polish** — DONE (`762ce52`, coordinator follow-up). Wake no longer
  flashes a blank temperature: load Home → repaint with latest model → `lv_refr_now`
  → THEN backlight on. Same for night-window auto-restore (repaint ambient first)
  and non-blanked touch-wake.
- **#87 Hold pill after flash** — DONE (`266b458`). New-firmware latch-clear block
  now also `putUChar("hold",0)` + resets `gShadow.hold`, so a fresh flash boots with
  no hold pill; a normal same-firmware power-cycle still restores a legit hold.
- **#88 Sticky presence + HA last_seen 3-h away** — DONE (`c8e194e`). Presence no
  longer decays with the motion window. New per-sensor last_seen ledger in
  SensorFusion (`updatePresence` / `presence()` / `presenceWithin`, `kPresenceAwayS
  =3h`, `PresenceState`), decoupled from motion window + temp `maxAgeS`, factored
  for a future night "sleep" state. `parsePresenceJson` (full-precision integer
  last_seen). main_thermostat subscribes `slytherm/sensors/+/presence`, converts
  HA unix last_seen → monotonic via wall clock (occupied-with-no-clock = seen-now →
  Present at boot), feeds the ledger, fills `DisplayState.presence`. UI:
  `fillPresenceLine` (Home + ambient) shows Present / Nobody home, falling back to
  the legacy temp-source text only when no presence sensor reports. HA bridge
  publishes RETAINED presence per room (start / online / 1-min heartbeat) so
  boot/reconnect seeds it. 7 new SensorFusion tests + 1 parser test.

### Final build + tests
- `pio test -e native` (test_sensor_fusion + test_ha_mqtt): **62/62 PASSED**.
- `thermostat_s3`: **SUCCESS** (pre-polish build 10m38; polish rebuild below). NOT flashed.

### Needs owner's eyes on-glass
- **#86b walk feel/timing** — confirm the hero visibly walks (not teleports) and the
  3-h L→R→L cadence feels right; diagonal Y should use vertical space.
- **#86c can't-clip** — try a long room name and a 3-digit/negative temp; nothing
  should clip at any drift position.
- **#86a night blank** — verify blank only happens overnight (00:00-06:00) and a
  touch wakes it; wake shows the temperature immediately (no blank flash).
- **#88 present/away + HA seed** — with people home + HA presence on, Home shows
  "Reading <room> · Present" and does NOT decay to "Nobody home"; after 3 h with no
  presence it flips to "Nobody home"; a reboot with people home shows Present
  immediately (retained seed). Requires the updated `ha/packages/slytherm_sensors.yaml`
  loaded in HA (adds the retained presence automation).

---

Session started 2026-07-05 (overnight). Owner away until morning.
Build: `export PLATFORMIO_BUILD_DIR=/tmp/pio-ts3; /usr/bin/python3 -m platformio run -e thermostat_s3`.

## Pre-work
- DONE: Committed NOW-label fix (`2aaa0ad`) — "Home: lift NOW label clear of the temp digits".
- DONE: Reverted a stray, broken working-tree edit in `test/test_dual_fuel/test_main.cpp`
  (changed OAT -9→-7; at -7 with default balancePoint -8 the arbiter yields kHeatPump,
  not the asserted kGas — the edit would have failed the test). Tree now clean.

## Issues (in order)
1. #73 remove local DS18B20 sensor — DONE (f46f71b). thermostat_s3 SUCCESS (11m). Gated slot 0 behind DETTSON_LOCAL_SENSOR (default off); UI/discovery/snapshot exclude local; docs/04 tradeoff noted.
2. #72 alarms don't clear — DONE (739fabe). thermostat_s3 SUCCESS (9m); native test_safety_supervisor 28/28. Opt-in autoClear flag; recoverable alarms drop on resolution, latched ones persist-until-ack.
3. #81 hold-duration chooser — DONE (f45e577). thermostat_s3 SUCCESS (9m); native 104/104. On-device chooser sheet + Home hold pill + kSetHold/kClearHold intents + HA select. NOT flashed (chooser sheet layout unverified on-glass — worth an eyes-on check).
4. #75 presets tap doesn't select — DONE (a4d3c42). Co-built with #77 in one thermostat_s3 SUCCESS (13m); split into separate commits. Optimistic highlight + pressed style.
5. #77 Settings status colors — DONE (7798f3b). Co-built with #75. Green-when-working WiFi/Home rows + Clock on its own row.
6. #76 System 12h graph — DONE (93ae608). thermostat_s3 SUCCESS (11m). lv_chart, 144-pt/5-min ring, actual vs heat/cool. Flash 40.7% RAM 55.9%. NOT flashed (on-glass layout unverified).
7. #80 graceful UI-crash recovery — DONE (7029827). thermostat_s3 SUCCESS (9m). Reduced safe-UI on reset-loop latch (NVS "rui") + "Restore full screen" clear path + docs/04. Skipped per-feature auto-quarantine (beyond bar). NOT flashed.

## ALL 7 ISSUES DONE + COMMITTED, each thermostat_s3 SUCCESS. Native tests green.
Flashing NOT done this session — every issue is compile-verified. UI layouts
(hold chooser sheet, System chart placement, safe-mode screen) want an eyes-on
pass on-glass.

## Docs (#44 manual, #43 user manual) — DONE (markdown; fb7713d)
Refreshed manual/src markdown (rev 0.4) for all 7 changes: no-local-sensor
narrative (#73), hold pill/chooser (#81), System 12h graph in the page list
(#76), Settings green status + clock row (#77), auto-clearing alerts (#72),
reduced safe-UI + Restore button (#80). Added docs/screenshots/safe-mode.png.

CAVEATS for the owner:
- PDFs NOT rebuilt: pandoc/mmdc absent on this host. Run `manual/build.py`
  where the toolchain exists to regenerate docs/manuals/*.pdf.
- FLASHED the new build to the device this session (COM5) - verified + booted.
- The device is holding a REAL, pre-existing reset-loop latch (the original
  boot-loop bug), so it came up in the #80 SAFE MODE screen - a live validation
  of #80. Because of that, the normal-UI screens could NOT be recaptured
  remotely (no remote latch-clear by design). Fresh normal-UI screenshots of
  the new features need a physical tap on "Restore full screen" on the panel,
  then re-run tools/slyshot.py. Existing docs/screenshots/*.png are the prior
  (pre-tonight) UI and are left in place.

## Build/verify summary
Every issue compiled to `thermostat_s3 SUCCESS`. Native suites green
(safety 28/28 incl. new #72 tests; ui_model/ha_mqtt/mode_sm 104/104).
On-glass UI layout of the new hold chooser sheet, System chart, and safe
screen still wants an eyes-on pass (safe screen IS confirmed rendering).

---

## Session 2026-07-06 (overnight) — batch: UI fixes + #74 + #82 + #79

Build discipline note: `pkill -f platformio` self-matches the launching shell in
this harness (kills its own process); omit it. Foreground `sleep` is fine in a
plain command; the earlier instant-fails were the pkill, not sleep.

1. **UI batch (WiFi Done->Home, hold pill)** — DONE (`72967b9`). thermostat_s3
   SUCCESS (11m30, RAM 70.9% Flash 40.9%). WiFi "Done" closes the overlay and
   loads the Home tab; hold pill shows ONLY on an active hold (activeHoldType !=
   none), hidden at boot/default and Off; pill re-pinned under the "Reading..."
   line so it no longer overprints the mode bar. Includes the pre-existing
   larger Heat/Cool layoutCard + -DLV_SPRINTF_USE_FLOAT=1.
2. **#74 presets read the LIVE roster** — DONE (`7feabd3`). thermostat_s3
   SUCCESS (10m46). DisplayState gains a PresetView roster + setPresets();
   ModeStateMachine::presetAt(i); control task fills it each tick; buildPresets
   builds up to kMaxPresets cards (3-wide grid, hidden until populated);
   renderMain fills name + heat/cool from the live roster and highlights vs the
   live values; preset apply now carries a roster INDEX resolved to a name.
   PARTIAL: on-device EDIT of preset temps deferred (TODO in buildPresets) —
   displaying the live roster (the must-have) is done.
3. **#82 first-run Welcome onboarding** — DONE (`2c6cf6f`), in the final batched
   build. wifi_prov::hasSavedCredentials(); setup() computes gFirstRun before
   uiTask (race-free); standalone scrWelcome (big logo + "Let's Get Started" ->
   WiFi); service() auto-transitions to Home on connect and reloads Welcome if
   the user backs out of WiFi setup (never a bare screen). Configured units boot
   straight to Home.
4. **#79 rebrand Dettson -> SlyTherm** — DONE (`95c1a9c`), final batched build.
   Topic prefix via ONE constant (SLYTHERM_TOPIC_PREFIX / topic::kTopicPrefix);
   all firmware topic literals, HA discovery ids/names, -DDETTSON_* -> -DSLYTHERM_*
   flags (grep DETTSON_ == 0 repo-wide), ha/packages+blueprints renamed + content,
   docs/manual/web/README, and host tests. Migration note in docs/06. KEPT:
   dettson:: namespace, DettsonConfig lib, NVS namespace "dettson" (avoids
   orphaning persisted settings), the CT-485 sniffer dev tool, and Dettson
   furnace prose (Chinook/Alize/C105-MV/R02P034).

Per the owner's mid-session efficiency change, #82 + #79 were batched into ONE
final compile (items 1 and 2 were each already compiled before that change).

Build/verify: final combined thermostat_s3 build **SUCCESS** (12m49, RAM 71.0%,
Flash 40.9%) — validates #82 + #79 together (items 1 & 2 built individually).
NOT flashed — owner will flash. On-glass checks wanted: Welcome screen layout &
logo scale, Presets grid with a live roster, hold-pill placement.

## Session 2026-07-06 (overnight) — #84, #85, #86 + Welcome tofu fix
Owner rule: all edits first, ONE final build. Result below. NOT flashed.

- **#84 System graph position** — DONE (`f0a96a0`). Chart box top moved to y=48 so
  it aligns with wSysBody's first line ("Now running:"); "Last 12 h" caption above
  it (y=24); trend label follows under the chart (y=322). Right-aligned, clear of
  the top bar. (Same commit also fixes the #82 Welcome headline tofu — see below.)
- **Welcome headline tofu** (owner mid-session ask, folded into `f0a96a0`). The
  "Welcome to SlyTherm" headline used font_set48 (a digits/./deg/- SUBSET with no
  letters) → every letter was a missing-glyph box. Switched to lv_font_montserrat_28
  (full alphabet). Subtitle + button already used montserrat; no other letter-bearing
  Welcome label used a subset font.
- **#85 Friendly sensor display name** — DONE (`7c246f0`). Roster JSON gains optional
  "name" decoupled from "id" (wire/topic segment unchanged). HaMqtt: SensorRosterEntry.name
  + parse. main_thermostat: SensorEntry.disp, filled in handleSensorRoster, used for
  SensorRow.name (fallback to id when absent). UiModel kSensorNameLen 16→24 for headroom.
  ha/packages/slytherm_sensors.yaml: name: per room + published in roster JSON.
  test_ha_mqtt: asserts name parses / absent→empty. Backward compatible.
  NOTE: Sensors-screen row format is still `%-11s` on the name (line ~796 slytherm_ui);
  "Living Room" fits exactly (11) — longer names eat column spacing. Left as-is (row
  width is tight); flag for on-glass judgement.
- **#86 Screensaver dim + drift** — DONE (`52c70af`). Backlight is CH422G bit kBitBl,
  ON/OFF ONLY (no PWM pin in the 4.3B LGFX bring-up) → analog dimming impossible.
  Path taken: (1) darker ambient theme — big temp no longer bright-white, uses dimmed
  heat/cool greys or COL_TEXT3; (2) deep screensaver — after 30 min idle fully BLANK
  the backlight (latched, restored on touch via ambWake); (3) full-screen drift —
  ambientShift now steps the whole hero block through a fractional grid spanning the
  usable 800×480, clamped by a conservative block box so the widest presence line
  never clips (was ±few-px around the far-left spot).

### Final build
thermostat_s3 **SUCCESS** (10m07, RAM 71.1% / 233032 B, Flash 40.9% / 1367721 B).
NOT flashed — owner flashes.

### Needs owner's eyes on-glass
- #86 backlight: confirmed ON/OFF-only path (dim theme + 30-min deep-blank). Verify the
  dim ambient palette is still readable and the temp visibly travels across the screen
  over the 15-min cadence; confirm a touch wakes from the fully-blanked deep screensaver.
- #84: chart top lines up with "Now running" and caption sits above.
- #82 Welcome headline now renders as real letters.
- #85: with the HA bridge sending name:, the panel shows "Living Room" etc.

## 2026-07-06 — #89 Sensors columns, #78 Vacation hold, #83 docs pass

- **#89 Sensors page: aligned columns — DONE (38e7f1b).** Rebuilt each room row
  from one inline `%-11s` string into separate fixed-x `lv_label` columns:
  **Room** (montserrat_20, `LV_LABEL_LONG_DOT` ellipsize so long friendly names
  can't push other columns) | **Temp** (own column, right-aligned "22.2°") |
  **Status** (single word: stale > Following > In use > Away[ Nh], amber when
  stale) | **On/Off** toggle pinned right. Added a ROOM/TEMP/STATUS caption row.
  OWNER-EYES: verify columns line up on the flashed panel and long names clip.

- **#78 On-device Vacation hold — DONE (6c09dfe), date-picker PARTIAL.** Vacation
  sheet opens from the Presets page: steppers for Starts (Today / in N days),
  Length (nights), Eco heat, Eco cool + Start/Cancel (eco keeps cool >= heat+1).
  New kSetVacation/kClearVacation intents + requestVacation()/cancelVacation()
  (lock-gated) + setVacation() banner echo + DisplayState.vacationActive/Banner.
  Control: VacationState persisted as one NVS blob ("vac"), anchored to local
  midnight (calendar-aligned), re-anchors once NTP syncs if set cold. Each cycle
  before the mode SM, evaluateVacation() forces the eco setpoints in-window
  (overrides schedule + presence), captures prior setpoints on entry and restores
  them on auto-resume at the end date; Home shows a "Vacation until <date>" pill.
  Boot restore keeps absolute epochs so a reboot never extends the window.
  PARTIAL: date entry is duration-based (days-out + nights), not a full calendar
  picker (per the issue's fallback). DEFERRED: HA bidirectional retained-config
  sync (dettson/slytherm config/vacation) — shipped on-device + NVS only.
  OWNER-EYES: verify the vacation sheet + Home banner + auto-resume on the panel.

- **#83 Docs branding + screens + facts + PDFs — DONE (1303358).** build.py
  PRODUCT/MODEL/DRAFT -> SlyTherm / ST-1 (Dettson dual-fuel edition) / v0.4;
  both manuals now use the rich title-page cover with the SlyTherm logo
  (theme/slytherm-mark.png). Rebranded every DT-1/ElectricRV/"Dettson/Gree"
  ref (grep-clean). Rewrote the Home callout table to the CURRENT UI (was a
  stale "setpoint dial"); documented current Sensors columns, Ambient, Welcome,
  Safe mode, and a new on-device Vacation section. Validated facts: onboard
  isolated RS-485 A/B on GPIO43/44 (UART0), retained roster `name` field, new
  retained slytherm/sensors/<id>/presence topic; climate.slytherm_hvac + topics
  confirmed. Rebuilt USER_MANUAL.pdf + INSTALLATION_MANUAL.pdf.
  DEFERRED: screenshots NOT recaptured — the #89/#78 UI is not flashed yet;
  a post-flash recapture via tools/slyshot.py is still owed.

- **Firmware build:** `thermostat_s3 SUCCESS` (both #89 + #78 compiled together,
  one build at the end). Not flashed.
