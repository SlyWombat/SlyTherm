-- Shadow control telemetry (#139). Controller firmware >= 0.5.8 mirrors its
-- would-be DemandSet (TX stays disabled — telemetry only) to the telnet log:
--   [shadow] <millis> gas=<pct> hp=<pct> cool=<pct> fan=<pct> dfr=<pct>
--            T=<fusedC> setH=<C> setC=<C> mode=<n> action=<word>
-- emitted on change + 60 s heartbeat. Compared against the OEM stat's real
-- CT-485 demands (raw_frames/events) over matched windows.
-- Idempotent: applied by the entrypoint on fresh volumes AND via psql -f to
-- the live database.

CREATE TABLE IF NOT EXISTS shadow_demands (
    ts           timestamptz NOT NULL,   -- wall-clock receive time
    millis       bigint      NOT NULL DEFAULT 0,  -- device millis
    gas_pct      real,
    hp_pct       real,
    cool_pct     real,
    fan_pct      real,
    defrost_pct  real,
    fused_temp_c real,
    set_heat_c   real,
    set_cool_c   real,
    mode         smallint,
    action       text
);
SELECT create_hypertable('shadow_demands', 'ts', if_not_exists => TRUE);
CREATE UNIQUE INDEX IF NOT EXISTS shadow_demands_dedupe
    ON shadow_demands (ts, millis);
