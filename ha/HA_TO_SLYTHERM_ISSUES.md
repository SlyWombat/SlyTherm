# HA-integration issues for the SlyTherm firmware/controller process

Raised from the **Home Assistant side** (Kazoo HA on kdocker2) on **2026-07-17**,
while wiring up the wall-panel "System" chart (`system-tab-chart.md`) and finishing
the status/tracking-line migration. Each item is something the **firmware / controller
/ docs** process should resolve; where I worked around it on the HA side I say so.

Controller firmware observed this session: **sw_version 1.2.0** (MQTT device
"SlyTherm ClimateTalk Thermostat", `identifiers: ["slytherm_esp32"]`).

---

## 1. `slytherm/state/setpoint` is not actually published (chart doc is wrong)

`system-tab-chart.md` → *"Add the Setpoint sensor"* tells the HA admin to add an MQTT
sensor on `slytherm/state/setpoint`, stating the value is *"published retained on
`slytherm/state/setpoint`."* It is **not**:

```
# on the broker (chirpstack-mosquitto-1), nothing retained:
mosquitto_sub -t 'slytherm/state/setpoint' -W 3          # → (no message)
mosquitto_sub -t 'slytherm/state/target_temp_low' -W 3   # → (no message)
mosquitto_sub -t 'slytherm/state/target_temp_high' -W 3  # → (no message)
```

So the doc's MQTT sensor would sit permanently `unavailable`.

**Workaround applied (HA side):** derived a `sensor.slytherm_setpoint` template from the
climate entity's `temperature` attribute (falls back to `target_temp_low/high` by
`hvac_action` in heat_cool). Works, but it's a HA-local kludge.

**Ask (pick one):**
- (a) Publish retained `slytherm/state/setpoint` (single active setpoint) — and
  `target_temp_low` / `target_temp_high` for heat_cool/Auto — ideally auto-discovered
  as their own sensors; **or**
- (b) Correct `system-tab-chart.md` to say "derive Setpoint from the climate entity's
  `temperature` attribute; there is no setpoint topic," and drop the MQTT-sensor step.

---

## 2. No fan-only / blower-running signal

The chart was asked to shade a light band when **fan-only** is active. There is no
reliable signal for "blower spinning without heat/cool":

- `sensor.slytherm_climatetalk_thermostat_slytherm_blower` reads **`unknown`** (never
  populates).
- No retained `slytherm/state/fan` or `slytherm/state/blower` topic.
- The climate entity's `hvac_action` only ever reports `heating` / `cooling` / `idle` /
  `off` — **never `fan`** — even though `fan_mode` exposes `auto` / `on` / `circulate`.

**Workaround applied (HA side):** `sensor.slytherm_activity` outputs
heating/cooling/fan/idle/off; the `fan` branch keys off `hvac_action == 'fan'`, so the
fan band will simply stay dark until the firmware provides the signal.

**Ask:** publish a blower/fan-running state (populate the `blower` sensor, or add a
retained `slytherm/state/blower` boolean), **and/or** report climate `hvac_action: fan`
when the blower runs with no active heat/cool stage.

---

## 3. Discovery entities get very long ids because `object_id` isn't set

Every auto-discovered entity lands with the full device-name prefix, e.g.:

```
climate.slytherm_climatetalk_thermostat_slytherm_hvac
sensor.slytherm_climatetalk_thermostat_slytherm_fusion
sensor.slytherm_climatetalk_thermostat_slytherm_active_equipment
sensor.slytherm_climatetalk_thermostat_slytherm_outdoor_temperature
```

because the discovery configs set the device `name` ("SlyTherm ClimateTalk Thermostat")
but **don't set an explicit `object_id`**, so HA builds `entity_id =
{device_name}_{entity_name}`. Meanwhile **every doc/README example uses the short ids**
(`climate.slytherm_hvac`, `sensor.slytherm_fusion`, `sensor.slytherm_outdoor_temp`,
`sensor.slytherm_setpoint`, …), so each example has to be hand-rewired for this install.

Proof the fix is `object_id`: the v1.2.0 `status_line` / `tracking_line` discovery
configs **do** set `object_id: slytherm_status_line` — those would have produced clean
short ids (see #5 for why they didn't, this time).

**Ask:** set an explicit short `object_id` on **all** discovery configs
(`slytherm_hvac`, `slytherm_fusion`, `slytherm_active_equipment`,
`slytherm_outdoor_temperature`, `slytherm_setpoint`, …) so entity_ids match the docs.
**Caveat:** on an already-provisioned unit this is a breaking change — HA keeps the old
entity_ids (they're pinned by `unique_id` in the registry and won't auto-rename), so ship
it with a migration note (delete + re-add the device, or rename in the entity registry).

---

## 4. Doc naming mismatch: outdoor temperature

`system-tab-chart.md` and the README reference `sensor.slytherm_outdoor_temp`; the actual
firmware entity is `...slytherm_outdoor_temperature` (plus the long prefix from #3). Pick
one spelling and make the docs match the firmware.

---

## 5. FYI — status/tracking-line prefix hybrid (already resolved on HA side)

When v1.2.0 began publishing `status_line` / `tracking_line` (good — richer text than our
stopgap templates, e.g. "Heating soon • 2 min"), they **collided** with the two temporary
template sensors we'd been running on the short slugs `sensor.slytherm_status_line` /
`_tracking_line`. Result: the firmware/MQTT entities fell back to the long-prefixed ids
`sensor.slytherm_climatetalk_thermostat_slytherm_{status,tracking}_line`.

**Resolved on the HA side:** deleted the template sensors, removed the two orphaned
template registry entries, and repointed the dashboards at the long firmware ids. No
firmware action needed — just noting that if #3 is done (consistent `object_id` + a clean
install) these two would also be short.

---

*Contact point: this file is the HA→SlyTherm channel for integration issues; append
replies inline or in a sibling `SLYTHERM_TO_HA_REPLY.md`.*

---

## HA-side acknowledgement (2026-07-17, later)

Thanks — verified **1.2.8 live** on the Controller (retained MQTT discovery reports
1.2.8; our HA registry now agrees, the stale 1.2.0 caught up). Confirmed the fixes on
the broker:

- `slytherm/state/setpoint` = 21.0, `target_temp_low` = 18.2 — now **retained** ✓
- `slytherm/state/blower` = on, `slytherm/state/action` = cooling — now **retained** ✓
- `sensor.slytherm_blower` now reads `on` (was permanently `unknown`) ✓

**#1 setpoint:** keeping our HA-side template `sensor.slytherm_setpoint` for now (it
works); may switch to the retained-MQTT sensor later — low priority since both work.

**#2 fan/blower:** our System-trend chart's fan band keys off `hvac_action == 'fan'`
via a `sensor.slytherm_activity` helper; now that `state/action` is retained it lights
up reliably. No further firmware ask.

**#3/#4 short entity IDs:** understood — available via delete + re-add of the MQTT
device. Deferred: it would rename every `slytherm_climatetalk_thermostat_slytherm_*`
entity to the short form and break all our dashboard/package/automation references, so
we'll do it as its own migration if/when we take it on, not right now.

Net: nothing outstanding on the firmware side. Appreciate the fast turnaround.

---

## 6. Health/alarm asserts but never clears (2026-07-17, live 1.2.8)

Symptom: HA's `binary_sensor…health` (device_class problem) stuck **on** with
`sensor…last_error` = **"Furnace link interrupted (recovering)"** since 21:25:32 —
the 1.2.8 reboot window — while the wall unit shows **no** error and the system is
healthy (`slytherm/remote/state`: `fusedTempValid:true`, `action:cooling`).

Root cause (verified on the broker):
- `slytherm/state/health` (feeds the binary_sensor) and `slytherm/state/last_error`
  are **non-retained** and were published **once** at assertion; a 20 s live watch
  saw **no** further publishes. So HA holds the last value forever.
- The retained `slytherm/remote/state` still carries `alarmN:1` /
  `alarm1:"…(recovering)"` — i.e. the alarm flag was **never cleared** there either,
  even though the condition recovered.

So HA is faithful; it's simply never told the alarm cleared.

**Ask:**
1. **Publish the clear** — when an alarm recovers, publish `slytherm/state/health` =
   `OFF`, clear `slytherm/state/last_error`, and set `alarmN:0` / drop `alarm1` in
   `slytherm/remote/state`.
2. **Make `slytherm/state/health` retained** (like `state/action` now is), so a
   reconnecting/restarting HA gets the true current health instead of `unknown` or a
   stale value.
3. Minor: a "recovering" alarm that has recovered should transition to cleared rather
   than staying asserted.
