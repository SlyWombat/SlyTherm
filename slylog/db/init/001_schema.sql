-- SlyLog schema (#130/#131). TimescaleDB hypertables for time series,
-- plain relational tables for the small stuff. Applied automatically by the
-- timescaledb image on first boot of an empty volume (docker-entrypoint-initdb.d).

CREATE EXTENSION IF NOT EXISTS timescaledb;

-- ---------------------------------------------------------------------------
-- raw_frames: every validated CT-485 frame from the controller telnet mirror.
-- 90-day rolling retention (the only table with retention — everything else
-- is kept indefinitely).
-- ---------------------------------------------------------------------------
CREATE TABLE raw_frames (
    ts            timestamptz NOT NULL,           -- wall-clock capture time
    millis        bigint      NOT NULL DEFAULT 0, -- device millis (resets on reboot)
    src           smallint    NOT NULL,
    dst           smallint    NOT NULL,
    msg_type      smallint    NOT NULL,           -- full byte incl. 0x80 response flag
    payload       bytea       NOT NULL DEFAULT ''::bytea,
    payload_hash  text        NOT NULL,           -- md5 of raw frame bytes (dedupe key)
    valid         boolean     NOT NULL DEFAULT true,
    synthesized   boolean     NOT NULL DEFAULT true,  -- rebuilt from telnet summary line
    truncated     boolean     NOT NULL DEFAULT false, -- mirror clipped payload (>16B, no cont.)
    source        text        NOT NULL DEFAULT 'live' -- 'live' | archive filename
);
SELECT create_hypertable('raw_frames', 'ts');
-- Idempotent ingest/migration: (ts, millis, payload) identifies a capture line.
CREATE UNIQUE INDEX raw_frames_dedupe ON raw_frames (ts, millis, payload_hash);
CREATE INDEX raw_frames_msg_type_idx ON raw_frames (msg_type, ts DESC);
SELECT add_retention_policy('raw_frames', INTERVAL '90 days');

-- ---------------------------------------------------------------------------
-- events: decoded/derived happenings (cycle_start/cycle_stop/demand_change/
-- novel_command/ct485_stats/capture_gap/capture_session/boot/status/ota).
-- Feeds Grafana annotations. detail_hash makes re-derivation idempotent.
-- ---------------------------------------------------------------------------
CREATE TABLE events (
    ts          timestamptz NOT NULL,
    kind        text        NOT NULL,
    detail      jsonb       NOT NULL DEFAULT '{}'::jsonb,
    detail_hash text GENERATED ALWAYS AS (md5(detail::text)) STORED
);
SELECT create_hypertable('events', 'ts');
CREATE UNIQUE INDEX events_dedupe ON events (ts, kind, detail_hash);
CREATE INDEX events_kind_idx ON events (kind, ts DESC);

-- ---------------------------------------------------------------------------
-- room_temps: slytherm/sensors/+/state and /presence.
-- temp_c is NULL for presence-only updates.
-- ---------------------------------------------------------------------------
CREATE TABLE room_temps (
    ts        timestamptz NOT NULL,
    sensor_id text        NOT NULL,
    temp_c    real,
    occupied  boolean
);
SELECT create_hypertable('room_temps', 'ts');
CREATE INDEX room_temps_sensor_idx ON room_temps (sensor_id, ts DESC);

-- ---------------------------------------------------------------------------
-- outdoor_temps: slytherm/cmd/outdoor_temp (bare float, deg C).
-- ---------------------------------------------------------------------------
CREATE TABLE outdoor_temps (
    ts     timestamptz NOT NULL,
    temp_c real        NOT NULL,
    source text        NOT NULL DEFAULT 'mqtt'
);
SELECT create_hypertable('outdoor_temps', 'ts');

-- ---------------------------------------------------------------------------
-- forecasts: hourly Open-Meteo snapshots of the NEXT 24 h (#134). Keyed on
-- (fetched_at, valid_at, source) so every fetch snapshots the full horizon
-- and accuracy is scoreable later. Carries rain + wind for the garden.
-- ---------------------------------------------------------------------------
CREATE TABLE forecasts (
    fetched_at      timestamptz NOT NULL,
    valid_at        timestamptz NOT NULL,
    temp_c          real,
    precip_mm       real,
    precip_prob_pct real,
    wind_kmh        real,
    gust_kmh        real,
    wind_dir_deg    real,
    humidity_pct    real,
    code            smallint,       -- WMO weather code
    condition       text,           -- human name for code
    source          text NOT NULL DEFAULT 'open-meteo',
    PRIMARY KEY (fetched_at, valid_at, source)
);
CREATE INDEX forecasts_valid_idx ON forecasts (valid_at);

-- ---------------------------------------------------------------------------
-- hvac_state: slytherm/remote/state (controller-authoritative replica state).
-- ---------------------------------------------------------------------------
CREATE TABLE hvac_state (
    ts         timestamptz NOT NULL,
    mode       text,
    action     text,
    equipment  text,
    heat_sp    real,
    cool_sp    real,
    fused_temp real,
    extra      jsonb NOT NULL DEFAULT '{}'::jsonb  -- full original payload
);
SELECT create_hypertable('hvac_state', 'ts');

-- ---------------------------------------------------------------------------
-- predictions: LLM forecasts + degree-day baseline, record-only (#136).
-- Full inputs stored for reproducibility; failure rows keep silent skips visible.
-- ---------------------------------------------------------------------------
CREATE TABLE predictions (
    ts             timestamptz NOT NULL DEFAULT now(),
    model          text        NOT NULL,  -- ollama model tag or 'degree-day-linear'
    kind           text        NOT NULL,  -- 'llm' | 'baseline'
    status         text        NOT NULL DEFAULT 'ok',  -- 'ok'|'timeout'|'error'
    inputs         jsonb       NOT NULL DEFAULT '{}'::jsonb,
    prediction     jsonb       NOT NULL DEFAULT '{}'::jsonb,
    recommendation text,
    error          text
);
CREATE INDEX predictions_ts_idx ON predictions (ts DESC);
