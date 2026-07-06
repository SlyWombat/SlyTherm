# Overnight progress — SlyTherm

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
