# Apply: room-sensor staleness gate fix (SlyTherm#192)

**Status:** prepared 2026-08-22, **not applied** — HA changes are the owner's
to make (`ha-work-is-owners`). Everything below is remote-only; no physical
access, no button-presses, nothing that has to wait for the ~08-25 return.

Files in this repo, ready to copy:

| repo file | live path on kdocker2 |
|---|---|
| `ha/packages/slytherm_sensors.yaml` | `/data/stacks/homeassistant/config/packages/slytherm_sensors.yaml` |
| `ha/packages/zigbee_sensors.yaml` | `/data/stacks/homeassistant/config/packages/zigbee_sensors.yaml` |
| `ha/patches/192-sensor-stale-gate.patch` | the fix-only delta (`patch -p1` from the config dir) |

Both repo copies were re-synced from live on 2026-08-22 first (the previous
`slytherm_sensors.yaml` in this repo was two months stale), so the *fix* is the
patch, not the whole-file diff.

---

## What was wrong

1. **`initial: 45` on `input_number.slytherm_sensor_stale_min`.** A defined
   `initial:` overrides state restoration, so every HA restart forced the
   helper back to 45 and silently reverted kazoo-home-assistant#4's
   `set_value` to 180. Recorder proof: every write in a 10-day window was
   `45.0`, every one at an HA start event.
2. **The gate measured the wrong thing for the ZHA rooms.** It scores the
   freshest `last_reported` across a room's sibling entities, and on HA 2026.7
   `last_reported` only advances on a *value change*. Dining Room went 801.7
   min between value changes while its radio link was healthy — longer than
   ZHA's own 6 h battery timeout, which never fired. Result: the gate was hot
   **65.6% of minutes** and **48 of 51 phone pushes in 48 h** were this one
   alert flapping offline→OK every ~55 min.
3. `max: 240` on the helper was below Dining's real tail, so no value
   reachable from the UI slider could have fixed it either.

## What changed

**`slytherm_sensors.yaml`**

- Helper: `initial: 45` **removed**, `max: 240` → `1440`. All three consumers
  already carry `| float(45)` (the bridge publish condition and the two
  template blocks — grepped 2026-08-22: nothing outside `slytherm_sensors.yaml`
  reads this helper), so a never-set helper degrades to exactly today's
  behaviour — nothing changes until you set a value.
- New per-room `liveness:` field in the roster, defaulting to `report`
  (today's last_reported logic). Main Bedroom and Dining Room are now
  `liveness: availability`: alive unless the integration marks the entity
  unavailable. ZHA end devices have no cloud cache to freeze behind, so
  availability is the honest signal for them; the cloud-backed rooms keep
  report-age gating, because a wedged cloud serving frozen data from a dead
  device is exactly the 2026-07-27 failure that guard exists to catch.
- Applied in three places (bridge publish condition, stale-count sensor,
  `stale` attribute). The `ages` attribute still reports **raw** report ages
  for every room — availability-mode rooms are tagged `[zha-avail]` so the
  diagnostic can't be mistaken for the gate.

Nothing new reaches the wire: the retained roster payload
(`slytherm/config/sensors`) is built from `id` / `name` / `max_age_s` /
`offset` only, so `liveness:` never leaves HA and the thermostat sees an
identical roster.

**Trade-off you accepted (2026-08-22):** a genuinely dead SNZB-02D now keeps
feeding fusion until ZHA marks it unavailable (`consider_unavailable_battery`,
6 h default) instead of at `stale_min`. Slower than 45 min, faster than the
~900 min threshold that was the only alternative that silenced the false
alarms — so it wins on both axes. **Do not** lower
`consider_unavailable_battery` to compensate without first measuring the
devices' real check-in cadence (device `last_seen`, which advances on any
received frame — not the value-change age the recorder shows). Set below the
real cadence and it flaps again.

**`zigbee_sensors.yaml`** — header corrected (#192 §5d): it claimed there were
no Zigbee routers and recommended buying one. There are two (both Third
Reality night lights, `logical_type: 1`), and both SNZB-02Ds are parented
through them. Added a note that this package's availability alert is now
load-bearing for SlyTherm's fusion gate.

---

## Apply

```bash
# 1. back up live
ssh kdocker2 'cd /data/stacks/homeassistant/config/packages && \
  ts=$(date -u +%Y%m%dT%H%M%SZ) && \
  cp slytherm_sensors.yaml slytherm_sensors.yaml.bak-$ts && \
  cp zigbee_sensors.yaml  zigbee_sensors.yaml.bak-$ts && ls -l *.bak-$ts'

# 2. copy the fixed files up  (run from the repo root)
scp ha/packages/slytherm_sensors.yaml ha/packages/zigbee_sensors.yaml \
    kdocker2:/data/stacks/homeassistant/config/packages/
#    ...or, to patch in place instead:
#    scp ha/patches/192-sensor-stale-gate.patch kdocker2:/tmp/ && \
#    ssh kdocker2 'cd /data/stacks/homeassistant/config && patch -p1 --dry-run < /tmp/192-sensor-stale-gate.patch'
```

3. **Developer Tools → YAML → Check configuration** — must come back green.
4. **Restart Home Assistant.** A helper *definition* change needs a restart to
   be sure; a YAML reload is not enough here.
5. **Set the threshold once** (this is the step that actually changes
   behaviour — editing the YAML alone leaves the restored value at 45):
   Developer Tools → Actions → `input_number.set_value`
   - entity: `input_number.slytherm_sensor_stale_min`
   - value: `180`

   Sized for the cloud rooms only: Living Room's 10-day max is 34.7 min;
   Basement's is 383.9 min but it is `participating: off`. If Basement is ever
   turned back on for fusion, raise this to ~420 or give it a per-room
   override.
6. **Confirm it sticks**: restart HA once more and check the helper still
   reads 180. That is the whole point of change (1).

## Verify (first hour, then next day)

- `sensor.slytherm_stale_room_sensors` sits at **0**; its `ages` attribute
  shows the two ZHA rooms tagged `[zha-avail]` with whatever raw age they
  happen to have.
- No further `🌡️ SlyTherm room sensor offline` / `OK` pushes. Before the fix
  they arrived roughly every 50 min.
- The two ZHA rooms keep publishing on `slytherm/sensors/{dining,bedroom}/state`
  every 30 s regardless of value-change age.
- Existing `📡 … Zigbee sensor offline` alerts still work — they are the
  detection path now.

## Rollback

Restore the `.bak-<ts>` files from step 1 and restart HA. The helper's value
persists independently, so also set it back to 45 if you want the old
behaviour exactly.

## What is and isn't proven

- **Proven offline:** both files parse (`yaml.safe_load`); the patch applies
  cleanly to the live files (`patch --dry-run`, both hunks); the YAML anchor
  `&slytherm_rooms` propagates `liveness:` to every alias automatically; and
  the changed Jinja was render-tested against a stubbed `states`/`now` for five
  scenarios — ZHA silent 800 min but available (→ not stale), ZHA unavailable
  (→ both rooms stale), cloud rooms silent 400 min (→ stale), Basement
  `participating: off` still exempt (ka#5 intact), and helper `unknown` falling
  back to 45.
- **Not proven:** anything requiring HA itself — real template rendering,
  `check_config`, and the fusion-side effect. The wall Controller is in
  `bus_mode listen` until the ~08-25 switch-back, so fusion changes are inert
  right now; the visible change today is the alert channel going quiet.

## Follow-ups (not in this change)

- **kazoo-home-assistant#4** should be reopened/closed out with the real
  mechanism — its advice ("editing `initial:` won't change an existing value")
  was right at runtime and backwards across restarts.
- **ka#6 ZHA reporting pass** stays parked to ~08-25 (needs button-presses).
  It is no longer urgent for alert noise, but still wanted: it would make
  `last_reported` honest and let these rooms go back to `report` mode with a
  short threshold.
- ⚠️ **This change defeats resume-checklist step 2.** The switch-back plan
  validates ka#6 by watching `slytherm/state/sensor/+/age` stay under ~12 min
  for 24 h. After this fix HA publishes dining and bedroom every 30 s
  unconditionally (availability-gated, not age-gated), so their `age` reads
  ~0-30 s whether or not the reporting pass ever ran — the gate goes green
  vacuously for exactly the two rooms it was written to check. Validate ka#6
  instead from the device-side `last_seen` (ZHA device page / zha_toolkit) or
  from the `[zha-avail]`-tagged **raw** ages in the `ages` attribute, which
  still show true value-change cadence.
- **#192 §5c** — `sensor.sonoff_snzb_02d_rssi` / `_lqi` are
  `disabled_by=integration` and need no physical access. Enable → watch 24 h →
  only then consider wiring them into `live:`. Unverified: LQI is an 8-bit
  value and may repeat across packets, hitting the same `last_reported` trap.
- **#192 §5e** — Basement is `participating: off`, so a genuinely dead Basement
  sensor currently looks identical to a healthy idle one. Correct per ka#5, but
  worth a separate guard someday.
