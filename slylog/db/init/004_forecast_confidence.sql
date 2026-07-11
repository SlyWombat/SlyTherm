-- SlyLog LLM-graded weather-source confidence (#141). Fully idempotent: this
-- file is applied both by the image entrypoint on fresh volumes AND manually
-- against the live database (psql -f), so everything is IF NOT EXISTS.
--
-- One row per (review run, lead bucket): the predictor's daily confidence job
-- snapshots the numeric skill of the freshest fully-matured window (from the
-- forecast_scores view), asks the local LLM to grade the weather SOURCE
-- 0-100 for that lead, and records both the inputs and the verdict here.
-- model='fallback' rows are deterministic MAE-threshold scores inserted when
-- the LLM's answer was unusable twice — the table never silently gaps.
--
-- Retention: none (schema policy — only raw_frames has retention). A daily
-- run at 2 buckets is ~730 small rows/year; kept indefinitely so long-term
-- source-trust trends stay queryable.
CREATE TABLE IF NOT EXISTS forecast_confidence (
    id               bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    reviewed_at      timestamptz NOT NULL DEFAULT now(),
    source           text        NOT NULL DEFAULT 'open-meteo',
    lead_bucket      text        NOT NULL,  -- '24h' (lead 1-24 h) | '48h' (lead 25-48 h)
    window_start     timestamptz NOT NULL,  -- matured valid_at span reviewed
    window_end       timestamptz NOT NULL,
    n_samples        integer     NOT NULL,  -- graded forecast rows in the window
    -- numeric inputs snapshot (exactly what the LLM was shown)
    temp_mae_c       real,                  -- vs house OAT when available, else obs
    temp_bias_c      real,
    precip_hit_rate  real,                  -- NULL until any rain was observed
    precip_false_alarm_rate real,           -- NULL until any rain was predicted
    wind_mae_kmh     real,
    confidence_score smallint    NOT NULL CHECK (confidence_score BETWEEN 0 AND 100),
    rationale        text,
    model            text        NOT NULL   -- ollama model tag | 'fallback'
);
CREATE INDEX IF NOT EXISTS forecast_confidence_bucket_idx
    ON forecast_confidence (lead_bucket, reviewed_at DESC);
