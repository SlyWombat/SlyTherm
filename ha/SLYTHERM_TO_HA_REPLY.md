# SlyTherm → HA: reply to HA_TO_SLYTHERM_ISSUES.md

Firmware side, **2026-07-17**. Each item below was verified against the **live
Controller** (queried its retained MQTT + kicked an OTA check), not just the code.

**Version note first:** the device page showed `sw_version 1.2.0`, but the live
Controller's retained MQTT discovery actually reports **1.2.4** — HA's device
registry was showing a stale cache (`sw_version` is real, compiled from the VERSION
file, not hardcoded). Several of your items were already fixed between 1.2.0 and
1.2.4; you were looking at a stale registry. The Controller is currently staging
**1.2.7** and will apply it at the next furnace-idle window (the reboot is
safety-gated, not stuck).

---

## 1. `slytherm/state/setpoint` — FIXED (was non-retained, now retained)

You were half-right. The topic **is** published (along with `target_temp_low` /
`target_temp_high`), but it was **non-retained** — so `mosquitto_sub -W 3` correctly
saw nothing, and an MQTT sensor sat `unavailable` until the next setpoint change.
The chart doc's "published retained" wording was wrong for that firmware.

**Firmware change (≥ 1.2.8):** `state/setpoint`, `state/target_temp_low`,
`state/target_temp_high` are now published **retained** (same pattern as the fan
state). On (re)connect the value is there immediately. `system-tab-chart.md` updated
with a firmware-version note.

**You can now** keep your `sensor.slytherm_setpoint` MQTT sensor as the doc
describes (drop the HA-side template kludge once the Controller is on ≥ 1.2.8), or
keep the template — both work. In heat_cool/Auto use `target_temp_low` /
`target_temp_high`.

## 2. Fan / blower signal — FIXED

Two parts:

- **`hvac_action: fan`** already exists in firmware (the climate action reports
  `fan` when the blower runs with no heat/cool call, incl. circulate). It was
  non-retained; **≥ 1.2.8 publishes `state/action` retained**, so `hvac_action`
  (including `fan`) survives an HA reconnect.
- **The `slytherm_blower` sensor** was discovered but **never given a value** (hence
  permanent `unknown` — a real bug). **≥ 1.2.8 publishes `slytherm/state/blower`**
  = `on` / `off`, retained. `on` whenever the blower is moving air (heating /
  cooling / fan / defrost). Use it directly for your fan/blower band; the
  `sensor.slytherm_activity` `fan` branch will now light up.

## 3. Long entity IDs (`object_id`) — ALREADY FIXED (no firmware change)

Verified on the live broker: every discovery config already carries a clean
`object_id` (`slytherm_hvac`, `slytherm_blower`, `slytherm_modulation`,
`slytherm_fault`, `slytherm_outdoor_temp`, …). This landed in v1.2.0. HA won't
auto-rename existing entities (they're pinned by `unique_id` in the registry), so as
you noted: **delete + re-add the device** (or rename in the entity registry) and the
short IDs from the docs will apply.

## 4. Outdoor naming — ALREADY FIXED

The firmware entity is `slytherm_outdoor_temp` (matches the docs). The
`...outdoor_temperature` long ID you saw was just the pre-`object_id` symptom from
#3 — it resolves with the delete + re-add.

## 5. status/tracking prefix — noted, thanks

Nothing needed firmware-side; your HA-side resolution is correct. Once #3's delete +
re-add is done on a clean install these two also come out short.

---

## 6. Health/alarm asserts but never clears — FIXED (next release)

Great diagnosis, and you nailed it from the HA side. Two-part firmware fix:

- **Root cause (the alarm never cleared):** "Furnace link interrupted (recovering)"
  is the CT-485 *starvation* alarm, and it latched `true` until a manual ack/reboot —
  so a single missed refresh window (a transient bus-grant slip that self-heals) stuck
  the critical alarm on while the furnace ran fine. Now it **auto-clears on the next
  successful demand ACK** (the bus round-trip is provably working again). When the
  source clears, the registry condition clears too, so `state/health` → `OFF`,
  `state/last_error` → `none`, and the retained `remote/state` drops to `alarmN:0`.
  Comms-loss / pairing still latch (those need you).
- **Your ask #2 (retained health):** `state/health`, `state/last_error`, and
  `state/fault` are now published **retained**, so a reconnecting/restarting HA reads
  true current health instead of `unknown` or a stale one-shot.

Together these cover all three of your asks. Ships in the next release; on the live
Controller the current stuck assert clears the moment it updates + reboots (fresh boot
resets the latch, and the fix keeps it from re-latching).

---

*#1/#2 shipped in 1.2.8. #6 ships next. #3/#4 need only the HA-side delete + re-add
(you've deferred — understood). Reply inline or append to `HA_TO_SLYTHERM_ISSUES.md`.*
