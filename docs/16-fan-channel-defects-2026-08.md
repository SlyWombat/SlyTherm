# SlyTherm Fan-Channel Defects — 2026-08

Three defects on the CT-485 **fan** channel, found while investigating a
1-second "Furnace link interrupted (recovering)" alarm that fires 4–13 times a
day. The alarm turned out to be the visible symptom of a larger problem: **fan
circulate has never once run the blower.** Line numbers are against `v1.4.3`
(`000b667`); each was re-verified at that commit.

Severity legend matches [`14-code-review-2026-07.md`](14-code-review-2026-07.md):
**P1** = safety or security, fix this season · **P2** =
correctness/robustness · **P3** = minor.

**Bottom line:** the frame SlyTherm puts on the wire is well-formed and matches
the capture-confirmed spec. The defects are (1) a demand *level* below the
equipment's engage threshold, (2) a re-arm bug that floods the bus with
redundant zero-demand frames, and (3) a starvation watchdog whose join-grace
path is reachable in steady state, where it calls `goSilent()` on a healthy bus.

---

## Evidence

Gathered from the TimescaleDB history on kdocker2 and a live capture of the
controller's serial mirror (`telnet 192.168.10.13:23`) during a fan window.

**The blower has never run on a circulate call.** `hvac_state.equipment` has
never reported `fan` in the entire recorded history — every `action='fan'`
sample shows `equipment='idle'`:

```
 action  | equipment | count |          first_seen           |           last_seen
---------+-----------+-------+-------------------------------+------------------------------
 cooling | cool      | 76350 | 2026-07-09 12:35:04.067311-04 | 2026-08-03 06:59:31.319259-04
 idle    | idle      | 35963 | 2026-07-09 10:54:52.647905-04 | 2026-08-03 10:37:33.316625-04
 fan     | idle      | 10403 | 2026-07-16 13:25:47.741885-04 | 2026-08-03 09:58:32.948601-04
 off     | idle      |   323 | 2026-07-12 14:53:02.20948-04  | 2026-07-17 16:58:42.986044-04
```

Still true at v1.4.3: 2,128 `action='fan'` samples in the six days to 2026-08-08,
all `equipment='idle'`. Cooling drives `equipment='cool'` correctly, so this is
specific to the fan channel, not to demand TX in general.

**Room temperature does not respond during a fan window.** Across the captured
10:43:41–10:50:41 window, fused temp drifted *up* 20.90 → 20.96 °C. No air moving.

**The frame itself is correct.** Captured on the wire during that window:

```
01>FF t03 l5 sn02 sm01 sp66 nt01 pk20   66 00 60 00 32
                                        │  │  │  │  └─ demand 0x32 = 25 % x2
                                        │  │  │  └──── mode 0x00
                                        │  │  └─────── refresh timer 0x60 = 6 min
                                        │  └────────── payload[1]
                                        └───────────── cmd 0x66 FAN_DEMAND
```

This matches [`02-protocol-climatetalk.md`](02-protocol-climatetalk.md) §5a
(`[12]=timer, [13]=mode, [14]=percent×2`) exactly.

**The alarm is fan-only and lands on a 300 s boundary.** All 46 blips in the
7-day sample had `action='fan'`. 32 of them fired **301.3–301.8 s** after fan
demand onset. `badCk` added **zero** on every one of those days, and the bus ran
28.5–29.2 ok-frames/min throughout — this is not bus corruption.

---

## P2-1 · Fan circulate commands 25 %, below the speed at which the blower engages

`src/main_thermostat.cpp:616`; `lib/HaMqtt/HaMqtt.h:52`, `:81`.

[`02-protocol-climatetalk.md`](02-protocol-climatetalk.md) §5a records the
equipment's behaviour as field-confirmed from an annotated capture:

> manual fan = mode 0, three discrete speeds **Low 25 % (0x32) / Med 50 %
> (0x64) / High 75 % (0x96)**; **fan-on enters at Med**.

`fan_circulate_pct` is set to **25**, so every circulate call asks for Low — at
or below the threshold the doc says the blower actually starts at. That is
consistent with all three observations above: a correct frame on the wire, the
equipment reporting idle, and no temperature response.

**Fix.** Raise `fan_circulate_pct` to **50** (Med). It is a runtime setting —
publish to `slytherm/cmd/fan_circulate_pct` or use the HA number entity; no
firmware change and no OTA required.

**Verify.** During the next circulate window, confirm `hvac_state.equipment`
reports `fan` (it never has), and that fused temp responds. If 50 still does not
engage, try 75 (High) before concluding the demand path is wrong — and if High
also fails, the defect is in the demand mapping rather than the level, which
reopens the Phase-2 blower-mapping question already tracked in
[`07-ecobee-gap-analysis.md`](07-ecobee-gap-analysis.md) G5.

**Confidence.** The theory fits the spec and every observation, but it has not
been tested on the equipment. This is the one change that needs a physical
confirmation rather than a code review.

---

## P2-2 · The fan channel re-sends a zero demand on every grant, forever

`lib/Ct485Thermostat/Ct485Thermostat.cpp:194-201`, `:457`.

While the system is idle, `66 00 60 00 00` goes out every **4.5–11.4 s** in a
repeating 11.4 / 4.5 / 7.9 / 4.5 pattern — measured on the controller's own
`millis` clock across 45 consecutive frames, so this is not an artifact of
collector buffering. That is ~8.5 frames/min against a bus carrying ~29
ok-frames/min: roughly a quarter of the controller's TX is a redundant zero.

The intended behaviour is the opposite. `setDemandInternal` deliberately
suppresses redundant sends (`:171-176`):

> Only (re)assert sendNeeded on a real change to the wire demand. The control
> loop calls setDemand() every tick; without this a steady demand re-sent on
> every grant (~5 s) — ~60x the OEM refresh rate.

That guard covers the *active* path but not the zero path:

```cpp
} else {
  const bool onWire = cs.everSent;
  cs.active     = false;
  cs.pct        = 0.0f;
  cs.sendNeeded = false;
  // Explicit zero only if the equipment ever heard a nonzero demand;
  // otherwise there is nothing on the wire to cancel.
  cs.zeroPending = onWire;          // :201 — re-armed on EVERY tick
}
```

`everSent` is set true when the frame is queued (`:457`) and is never cleared on
the zero path. So each control tick with `pct == 0` re-reads `everSent == true`
and re-arms `zeroPending`; the next grant sends another zero, sets `everSent`
true again, and the cycle repeats indefinitely. The cancel is meant to fire
once; instead it fires forever.

**Fix.** Clear the "something is on the wire" flag once the cancelling zero has
actually been sent, so `onWire` reads false on the following tick. In the grant
path (`:450-457`), when the frame being sent is a zero for an inactive channel,
set `cs.everSent = false` alongside `cs.zeroPending = false`. Add a regression
test asserting that a single `setDemand(ch, 0)` followed by N grants produces
exactly one zero frame — `test/test_ct485_thermostat/test_main.cpp` already has
the harness for this shape of assertion.

**Verify.** Re-capture the serial mirror while idle and confirm the fan-zero
frame stops repeating. `events.kind='ct485_stats'` `ok` rate should drop
measurably.

---

## P2-3 · Starvation join-grace is reachable in steady state and calls `goSilent()` on a healthy bus

`lib/Ct485Thermostat/Ct485Thermostat.cpp:17`, `:578-590`.

```cpp
const uint32_t refMs   = cs.everSent ? cs.lastSentMs : cs.activatedMs;
// #178: tight refresh window once we've been granted at least once; a long
// join grace while still waiting for the first grant after a (re)join, so a
// normal reboot re-join doesn't false-positive.
const uint32_t limitMs = cs.everSent ? windowMs : kJoinStarvationGraceMs;
if (timeReached(nowMs, refMs + limitMs)) {
  starvationAlarm_ = true;
  starvedCh_ = static_cast<DemandChannel>(i);
  goSilent();
```

`kJoinStarvationGraceMs` is 300 000 ms (`:17`). The measured 301.3–301.8 s
offset from fan-demand onset is that constant plus detection and publish
latency, and it requires `everSent == false` for the fan channel at the moment
of the trip — i.e. the channel activated and 5 minutes passed with no demand
frame ever going out.

The `#178` comment shows the grace was designed for one situation: a channel
still waiting for its first grant after a reboot re-join. But the same branch is
taken by **any** channel that activates mid-session, because `setDemandInternal`
resets `everSent = false` on every 0 → nonzero transition (`:186-189`). The fan
channel does that every hour on the circulate cycle, so a reboot-only safeguard
is being applied to routine steady-state operation.

The consequence is not cosmetic: the trip calls `goSilent()`, taking the
controller off the bus, on a bus with zero checksum errors.

**Fix.** Distinguish "never joined" from "newly activated". Track first-grant
state per *session* rather than per *channel activation* — e.g. a
`joinedSinceBoot_` flag set on the first successful grant of any channel — and
apply `kJoinStarvationGraceMs` only while that is false. A channel activating in
an already-joined session should be judged by `windowMs`, and arguably should
not trip at all until it has had at least one grant opportunity.

**Verify.** With P2-2 fixed, re-run a week and confirm the blip count goes to
zero. `hvac_state.extra->>'alarm1'` transitions are the signal; there is no
alarm event kind.

**Not yet explained.** Why the fan demand send is delayed past 300 s on roughly
one hour in three. In the window captured for this write-up the frame went out
5 s after activation and no alarm fired, which is why the blips are intermittent
rather than hourly. The P2-2 zero-storm is a plausible contributor — it keeps
`out_` busy and competes for grants — but that link is **not proven**, and P2-3
should be fixed on its own merits regardless of what causes the delay.

**Ordering.** Fix P2-2 first, then re-measure. If the blips stop, P2-3 is
latent rather than active, but the reachable-`goSilent()` path should still be
closed.

---

## Related: HA room-sensor staleness threshold is too tight

Not a firmware defect, but it degrades control quality now and shares the
"fix the number, not the code" character of P2-1.

`/config/packages/slytherm_sensors.yaml` on kdocker2 (the **live** copy — it
diverges from `ha/packages/`). The staleness gate added 2026-08-03 correctly
dropped two dead sensors, but its 45-minute threshold is below what healthy
sensors reach. Measured over 24 h with the current sibling-entity logic:

| room | median | p90 | p99 | max | % gated at 45 min |
|---|---|---|---|---|---|
| Basement | 19.0 | 58.6 | 120.0 | 134.0 | 18.4 % |
| Dining Room | 18.3 | 88.2 | 133.6 | 143.2 | 24.0 % |
| Main Bedroom | 9.6 | 47.4 | 92.7 | 106.7 | 10.8 % |
| Living Room | 7.2 | 17.2 | 28.3 | 34.7 | 0.0 % |

Rooms drop out of the fused temperature 10–24 % of the time, and the stale count
reads 0 only 58 % of the time — so the offline alert will also cry wolf.

**Fix.** Set `input_number.slytherm_sensor_stale_min` to **180**. That is above
the observed healthy maximum (143 min) and still detects a genuinely dead device
in 3 h, versus the 6 days the 2026-07-27 failure went unnoticed. It is a UI
slider; `input_number` state persists, so editing `initial:` in YAML will not
change a value that already exists.

**Note for future work on this file.** On this HA build (2026.7),
`last_reported` advances only when a value *changes* — not on every state write,
contrary to the HA ≥ 2024.8 documentation. Verified against switchbot_cloud
coordinator refreshes, duplicate MQTT publishes and same-state REST writes. Any
liveness check written against `last_reported` on a single slow-moving entity
will false-fire; the working approach measures the freshest report across
sibling entities on the same device, plus a canary whose value cannot plausibly
stay byte-identical.

---

## Summary

| # | Defect | Severity | Change type | Needs OTA |
|---|---|---|---|---|
| P2-1 | Circulate commands 25 %, below blower engage threshold | P2 | Runtime setting | No |
| P2-2 | Fan channel re-sends zero demand every ~7 s | P2 | Firmware + test | Yes |
| P2-3 | Join-grace starvation reachable in steady state, calls `goSilent()` | P2 | Firmware + test | Yes |
| — | HA staleness threshold 45 → 180 min | — | Runtime setting | No |

The two runtime settings can be changed today and are independently reversible.
The two firmware fixes belong in one release with the regression tests described
above; verify with `pio test -e native` for the library changes, and note that
the `thermostat_ui_s3` environment does not compile `main_thermostat.cpp`, so it
green-lights nothing on the controller path.
