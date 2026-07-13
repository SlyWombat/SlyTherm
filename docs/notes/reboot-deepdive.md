# Reboot deep-dive (v1.0.5 controller, overnight 2026-07-12 → 07-13)

**Verdict: there was no second reboot. The "4 overnight reboots" are a telemetry
read artifact — retained `slytherm/boot` re-published on each MQTT reconnect,
plus a sticky stale `coredump:true` flag. The controller ran continuously for
14+ hours with `bootCount:0`.**

Branch: `fix/reboot-deepdive`. No firmware change made — none is warranted (see
"Recommendations"). Diagnosis is the deliverable.

---

## The reported symptom

Overnight the live controller (cid `8d82f4`, 192.168.10.13, v1.0.5) *appeared*
to reboot ~4 times between "00:34 and 06:04" (UTC), with a "rapid double at
06:03→06:04", then run stable for 5.5 h. Every event was labelled
`reason:"sw_reset"` (rawReason 3 = `ESP_RST_SW`), `coredump:true`,
`rtcReason0/1 = 12`. There is no automatic software-restart path in the code,
which made this look like a genuine mystery / a different watchdog (INT_WDT).

## What actually happened — the load-bearing proof

**The controller's live `millis()` uptime is ~51.24M ms = 14.2 hours** (read
from the telnet mirror on :23 at ~10:47 EDT 07-13; CT-485 log line timestamps
`[ct485] 51241006 ...`). `millis()` resets to 0 on *every* reset class
(SW / panic / INT_WDT / TASK_WDT / brownout / power-on). The controller booted
v1.0.5 at **20:34:05 EDT on 07-12** (the release deploy); 20:34 → 10:47 next day
is 14h13m ≈ 51.18M ms. The measured 51.24M ms matches to within a minute.

Had it rebooted at the task's "06:04" (= **02:04 EDT**, see timezone note),
current uptime would be ~8.7 h (~31M ms), not 14.2 h. **That single measurement
falsifies the reboot premise.** It is corroborated by every other signal:

- `bootCount:0` throughout (an abnormal boot increments it; `boot_guard.cpp`).
- `prevUptimeS:10877` constant across all rows (would change on a real reboot).
- Live `slytherm/state/ota` at 10:47 EDT: `state:up_to_date, running:1.0.5`.
- Only **one** controller boot row with a small uptimeS (`uptimeS:5`, at
  20:34:05) exists in SlyLog for the entire v1.0.5 window.

## Why it looked like 4 reboots

`slytherm/boot` (retained) is published **only inside the MQTT connect block**
(`main_thermostat.cpp:1505`), on **every (re)connect**, stamped with the current
`uptimeS`. The comment there says so explicitly:

> "#123/#145: retained boot/crash telemetry. uptimeS is stamped per publish so a
> reconnect echo is distinguishable from a fresh boot."

SlyLog's 5 controller v1.0.5 boot rows are therefore **1 post-boot connect +
4 reconnect echoes**, with a monotonically **climbing** uptimeS:

| ts (EDT) | uptimeS | meaning |
|---|---|---|
| 07-12 20:34:05 | 5 | real boot (1.0.5 deploy) |
| 07-13 00:12:34 | 13113 | reconnect echo (~3.6 h up) |
| 07-13 00:30:59 | 14218 | reconnect echo |
| 07-13 02:03:03 | 19742 | reconnect echo |
| 07-13 02:04:01 | 19800 | reconnect echo (~5.5 h up) |

Climbing uptimeS proves these are **controller-side republishes** on live
reconnects, not collector re-reads of a frozen retained value.

**Direct confirmation** — the availability LWT (`slytherm/availability`) shows
exactly 4 brief offline→online flips at the same instants:

```
00:12:31 offline -> 00:12:34 online
00:30:58 offline -> 00:30:59 online
02:00:57 offline -> 02:02:54 online
02:03:52 offline -> 02:03:58 online   <- the "rapid double"
```

Each flip is a short MQTT/TCP drop; the app reconnects seconds later and
republishes retained `slytherm/boot`. The furnace was controlled throughout
(`bus=1`, `alarms=0`, active `mode`).

### Timezone note
The task's "06:03→06:04" is **02:03→02:04 EDT** (EDT = UTC−4). The 58-second gap
between the last two events is the "rapid double." "Stable for 5.5 h+" = after
02:04 EDT the MQTT link stopped flapping (OTA status kept publishing every 60 s
straight through to 10:47, i.e. no further reconnects → no further boot echoes).

## The `sw_reset` + `coredump:true` + `rtcReason 12` labels are all benign

- **`sw_reset` / rtcReason 12**: primary-source confirmed against the pinned IDF
  (`soc/esp32s3/reset_reasons.h`): `0x0C = RESET_REASON_CPU0_SW`, "software
  resets CPU0 by RTC_CNTL_SW_PROCPU_RST" — exactly what `esp_restart()` emits.
  `esp_reset_reason()` returned `ESP_RST_SW` (not INT_WDT) **because the panic/WDT
  hint was clear** (`reset_reason.c` maps `CPU0_SW + no hint → ESP_RST_SW`).
  This describes the **one real boot**: the 20:34 OTA/deploy reboot into 1.0.5
  (`prevUptimeS 10877` ≈ the prior 1.0.4 run). Not INT_WDT, not brownout
  (would be rtc 15), not panic (would set the hint + write a fresh dump).

- **`coredump:true` is a sticky presence flag, and the dump is STALE.**
  `boot_guard.cpp:88` sets it from `esp_core_dump_image_check() == ESP_OK`, which
  is true whenever *any* valid image sits in the coredump partition — it is not
  per-reboot. Pulled the dump over :8082 (37412 B) and decoded it:
  `esp-coredump info_corefile` rejected it with
  **`coredump SHA256(2e65b8b1e) != app SHA256(dd7cf267e)`** — the dump was written
  by a *different* firmware. Its memory even contains
  `http://192.168.10.12:8090/wall-s3-1.0.0.app.bin`. It is a leftover from the
  1.0.0–1.0.4 dev/panic churn on the evening of 07-12 (SlyLog shows those
  `task_wdt`/`wdt`/`panic` boots). **A clean `esp_restart` does not write a
  coredump**, so the stale dump proves the overnight events were *not* panics.

## Root cause (of the misread) and the real (benign) phenomenon

1. **Misdiagnosis cause**: a consumer counted retained `slytherm/boot`
   republishes as reboots, and the sticky `coredump:true` made each look like a
   crash. The firmware already emits `uptimeS` and `bootCount` to disambiguate;
   the read did not use them.
2. **Real phenomenon**: the controller's MQTT link briefly dropped and
   reconnected 4× between 00:12 and 02:04 EDT, then stayed connected. Benign —
   control was never interrupted. The aggressive `gMqtt.setSocketTimeout(2)`
   (the v1.0.5 TWDT fix) is a plausible contributor to reconnect churn under
   transient latency, but has no effect on control and needs no change.

## Recommendations (no live-firmware change; nothing to deploy)

- **Consumer/alerting fix (SlyLog)**: treat a message as a *reboot* only when
  `uptimeS` is small (e.g. < 120 s) **or** `bootCount` increments. Dedup boot
  rows by `(bootCount, prevUptimeS, lastAliveEpoch)`; a climbing `uptimeS` with
  those constant is a reconnect echo, not a boot. This alone removes the false
  positive.
- **Next-release firmware nit (optional, do not hot-patch 1.0.5)**: make
  `coredump` freshness explicit — compare the stored dump's app SHA
  (`esp_core_dump_get_summary()`) to the running app and report e.g.
  `coredumpFresh:false` for a stale image, and/or erase the partition after a
  successful :8082 pull. This would have shown "present but stale" immediately.

## Evidence trail
- Live telnet :23 uptime ≈ 51.24M ms (14.2 h).
- Coredump pulled via :8082, decoded with `esp-coredump` + xtensa-esp32s3 gdb;
  app-SHA mismatch (stale). ELF: `wall-s3-1.0.5.elf` from OTA mirror
  `192.168.10.12:8090`.
- SlyLog (kdocker2 `/data/stacks/slylog`, Postgres `slylog/slylog`): `events`
  table — boot rows, ota state, availability LWT.
- IDF primary source (pinned `framework-espidf`): `reset_reasons.h`,
  `esp32s3/reset_reason.c`.
