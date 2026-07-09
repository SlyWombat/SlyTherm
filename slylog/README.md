# SlyLog — SlyTherm observability + prediction stack

Optional dockerized stack (#130) that records everything SlyTherm already
emits and turns it into history, graphs, and non-binding predictions.
**Strictly a read-only consumer**: it subscribes to MQTT, reads the
controller's telnet log mirror, and pulls Open-Meteo. It publishes nothing,
controls nothing — SlyTherm behaves identically whether the stack is up or down.

## Containers

| service   | image                          | role |
|-----------|--------------------------------|------|
| db        | timescale/timescaledb:latest-pg16 | hypertables for time series, relational tables for events/forecasts/predictions |
| collector | ./collector (python:3.12-slim) | MQTT ingest (#132), CT-485 telnet ingest + live decode (#133), hourly Open-Meteo pull (#134) |
| grafana   | grafana/grafana                | provisioned dashboards + event annotations (#135), host port **3300** |
| ollama    | ollama/ollama                  | local CPU LLM (no host port — 11434 is taken on kdocker2) |
| predictor | ./predictor (python:3.12-slim) | 4-hourly LLM load forecast + degree-day baseline, record-only (#136) |

## Run

```sh
cd slylog
cp .env.example .env        # set POSTGRES_PASSWORD / GRAFANA_ADMIN_PASSWORD
docker compose up -d db     # schema in db/init/ auto-applies on first boot
docker compose up -d --build
```

Grafana: http://kdocker2:3300 (admin / $GRAFANA_ADMIN_PASSWORD), folder
"SlyLog", four dashboards: temperatures+forecast, cycle timeline+duty stats,
demand/action strip, predictions vs actuals. Events draw on the graphs as
annotations.

Pull the LLM early (first prediction otherwise waits on a ~4.7 GB download —
the predictor also pulls automatically on first run):

```sh
docker compose exec ollama ollama pull qwen2.5:7b-instruct
```

## Schema / retention

`db/init/001_schema.sql`: raw_frames, events, room_temps, outdoor_temps,
forecasts, hvac_state, predictions. Hypertables where high-rate; retention is
**90 days on raw_frames only**, everything else indefinite (~10 MB/day total).

Dedupe keys make ingest idempotent: raw_frames `(ts, millis, payload_hash)`,
events `(ts, kind, detail_hash)`.

## Historical migration (#137)

One-shot backfill of the pre-SlyLog capture archives (mounted read-only at
`/legacy-captures` from `$LEGACY_CAPTURES`):

```sh
docker compose exec collector python -m slylog_collector.migrate \
    /legacy-captures/ct485-capture-20260708.log \
    /legacy-captures/ct485-live.log
```

- Day-1 unstamped prefix is wall-mapped via the `.anchors` sidecar; sessions
  are segmented on `# ct485cap` headers (device millis resets per reboot).
- Events (cycle_start/cycle_stop/demand_change/novel_command/ct485_stats)
  are re-derived over the whole ordered history.
- Idempotent — safe to re-run any time (`--dry-run` to parse and count only).

## Capture cutover (telnet mirror allows exactly 2 clients)

1. `docker compose up -d collector` — collector connects as client #2
   alongside the old `run_capture.sh` loop.
2. Verify live rows: `SELECT max(ts) FROM raw_frames;` advances.
3. Kill the old loop on the host (note the `[r]` — avoids self-match):
   `pkill -f '[r]un_capture.sh'; pkill -f '[c]t485cap.py'`
4. Collector remains sole client and keeps writing the flat archive in
   ct485cap format inside the `captures` volume
   (`docker compose exec collector tail /captures-vol/ct485-live.log`).
5. Re-run the migration once after cutover — dedupe collapses the
   double-captured overlap minutes.

### Rollback

`captures/run_capture.sh` stays in the main repo. To restore the old logger:

```sh
docker compose stop collector     # frees a telnet slot
ssh kdocker2 'cd ~/SlyTherm && nohup captures/run_capture.sh >/dev/null 2>&1 &'
```

## Operating notes

- **Read-only guarantee**: the collector's MQTT client subscribes only; the
  predictor has no MQTT client at all. Nothing in this stack writes to the
  controller, the remotes, or the broker.
- Forecast: Open-Meteo, keyless, LAT/LON from `.env` (Mississauga ON:
  43.59, -79.64). If LAT/LON are unset/placeholder, the module logs a warning
  and skips fetching rather than inventing coordinates.
- Timezone: `TZ=America/Toronto` flows into containers, timestamp parsing of
  the (local-stamped) capture archives, and the Grafana dashboards.
- Predictor failure rows: ollama timeouts/errors insert `status='timeout'|
  'error'` rows in `predictions`, annotated red on the predictions dashboard —
  silent skips are visible.
- Backup path (documented alternative to a pg_dump sidecar):
  `docker compose exec db pg_dump -U slylog slylog | gzip > slylog-$(date +%F).sql.gz`
  — cron it on the host if desired. Volumes: `db_data`, `grafana_data`,
  `ollama_data`, `captures`.
- Admin DB access from the host: `psql -h 127.0.0.1 -p 5433 -U slylog slylog`.

## Data sources ingested

| source | destination |
|---|---|
| `slytherm/sensors/+/state` `{"temp":21.6,"occ":false}` | room_temps |
| `slytherm/sensors/+/presence` `{"occupied":false,...}` | room_temps (temp NULL) |
| `slytherm/cmd/outdoor_temp` bare float | outdoor_temps |
| `slytherm/remote/state` | hvac_state |
| `slytherm/boot`, `slytherm/remote/+/boot` | events kind=boot |
| `slytherm/controller/status`, `slytherm/remote/+/status`, `slytherm/availability` | events kind=status (transitions only) |
| `slytherm/state/ota`, `slytherm/remote/+/state/ota` | events kind=ota |
| controller telnet :23 `[ct485]`/`[ct485+]`/`[ct485-rej]`/`[ct485-stats]` | raw_frames + events + flat archive |
| Open-Meteo hourly (next 24 h: temp, rain mm+prob, wind+gusts+dir, humidity, WMO code) | forecasts |
