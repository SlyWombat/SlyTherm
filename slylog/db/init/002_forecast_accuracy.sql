-- SlyLog forecast-vs-actual accuracy (#138). Fully idempotent: this file is
-- applied both by the image entrypoint on fresh volumes AND manually against
-- the live database (psql -f), so everything is IF NOT EXISTS / OR REPLACE.

-- ---------------------------------------------------------------------------
-- weather_obs: hourly Open-Meteo CURRENT conditions.
-- HONEST CAVEAT (recorded here per #138): source='open-meteo-current' is
-- model-analysis "actuals", NOT a house instrument. Temperature ground truth
-- from real hardware is outdoor_temps (house OAT sensor); rain/wind have no
-- local gauge/anemometer yet. If a rain gauge ever lands, ingest it with a
-- distinct source value and prefer it in the views.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS weather_obs (
    ts        timestamptz NOT NULL,
    temp_c    real,
    precip_mm real,
    wind_kmh  real,
    gust_kmh  real,
    source    text NOT NULL DEFAULT 'open-meteo-current'
);
SELECT create_hypertable('weather_obs', 'ts', if_not_exists => TRUE);
CREATE UNIQUE INDEX IF NOT EXISTS weather_obs_dedupe ON weather_obs (ts, source);

-- ---------------------------------------------------------------------------
-- forecast_scores: every graded forecast row (valid_at in the past), matched
-- to actuals on valid_at +/- 30 min. Two temperature truths on purpose:
-- house OAT (real sensor) and obs (model analysis) — comparing the two also
-- scores the OAT sensor against the model.
-- ---------------------------------------------------------------------------
CREATE OR REPLACE VIEW forecast_scores AS
SELECT
    f.fetched_at,
    f.valid_at,
    round(extract(epoch FROM f.valid_at - f.fetched_at) / 3600)::int AS lead_hours,
    f.temp_c          AS fc_temp_c,
    f.precip_mm       AS fc_precip_mm,
    f.precip_prob_pct AS fc_precip_prob_pct,
    f.wind_kmh        AS fc_wind_kmh,
    f.gust_kmh        AS fc_gust_kmh,
    oat.temp_c        AS house_temp_c,
    obs.temp_c        AS obs_temp_c,
    obs.precip_mm     AS obs_precip_mm,
    obs.wind_kmh      AS obs_wind_kmh,
    obs.gust_kmh      AS obs_gust_kmh,
    f.temp_c - oat.temp_c AS temp_err_house,
    f.temp_c - obs.temp_c AS temp_err_obs,
    CASE WHEN f.precip_prob_pct IS NULL THEN NULL
         ELSE f.precip_prob_pct >= 50 END                    AS precip_predicted,
    CASE WHEN obs.precip_mm IS NULL THEN NULL
         ELSE obs.precip_mm >= 0.5 END                       AS precip_observed
FROM forecasts f
LEFT JOIN LATERAL (
    SELECT avg(o.temp_c) AS temp_c
    FROM outdoor_temps o
    WHERE o.ts BETWEEN f.valid_at - interval '30 minutes'
                   AND f.valid_at + interval '30 minutes'
) oat ON true
LEFT JOIN LATERAL (
    SELECT avg(w.temp_c) AS temp_c, avg(w.precip_mm) AS precip_mm,
           avg(w.wind_kmh) AS wind_kmh, avg(w.gust_kmh) AS gust_kmh
    FROM weather_obs w
    WHERE w.ts BETWEEN f.valid_at - interval '30 minutes'
                   AND f.valid_at + interval '30 minutes'
) obs ON true
WHERE f.valid_at <= now();

-- ---------------------------------------------------------------------------
-- Per-lead-hour skill (1..48 — collector fetches a 48 h horizon since #141),
-- rolling 7d / 30d windows over valid_at.
-- ---------------------------------------------------------------------------
CREATE OR REPLACE VIEW forecast_skill_7d AS
SELECT
    lead_hours,
    count(temp_err_house)                        AS n_house,
    avg(temp_err_house)                          AS temp_bias_house,
    avg(abs(temp_err_house))                     AS temp_mae_house,
    sqrt(avg(temp_err_house * temp_err_house))   AS temp_rmse_house,
    count(temp_err_obs)                          AS n_obs,
    avg(temp_err_obs)                            AS temp_bias_obs,
    avg(abs(temp_err_obs))                       AS temp_mae_obs,
    sqrt(avg(temp_err_obs * temp_err_obs))       AS temp_rmse_obs,
    avg(abs(fc_wind_kmh - obs_wind_kmh))         AS wind_mae,
    avg(abs(fc_gust_kmh - obs_gust_kmh))         AS gust_mae,
    count(*) FILTER (WHERE precip_predicted AND precip_observed)          AS precip_hits,
    count(*) FILTER (WHERE precip_predicted AND NOT precip_observed)     AS precip_false_alarms,
    count(*) FILTER (WHERE NOT precip_predicted AND precip_observed)     AS precip_misses,
    count(*) FILTER (WHERE NOT precip_predicted AND NOT precip_observed) AS precip_correct_negatives
FROM forecast_scores
WHERE valid_at > now() - interval '7 days' AND lead_hours BETWEEN 1 AND 48
GROUP BY lead_hours;

CREATE OR REPLACE VIEW forecast_skill_30d AS
SELECT
    lead_hours,
    count(temp_err_house)                        AS n_house,
    avg(temp_err_house)                          AS temp_bias_house,
    avg(abs(temp_err_house))                     AS temp_mae_house,
    sqrt(avg(temp_err_house * temp_err_house))   AS temp_rmse_house,
    count(temp_err_obs)                          AS n_obs,
    avg(temp_err_obs)                            AS temp_bias_obs,
    avg(abs(temp_err_obs))                       AS temp_mae_obs,
    sqrt(avg(temp_err_obs * temp_err_obs))       AS temp_rmse_obs,
    avg(abs(fc_wind_kmh - obs_wind_kmh))         AS wind_mae,
    avg(abs(fc_gust_kmh - obs_gust_kmh))         AS gust_mae,
    count(*) FILTER (WHERE precip_predicted AND precip_observed)          AS precip_hits,
    count(*) FILTER (WHERE precip_predicted AND NOT precip_observed)     AS precip_false_alarms,
    count(*) FILTER (WHERE NOT precip_predicted AND precip_observed)     AS precip_misses,
    count(*) FILTER (WHERE NOT precip_predicted AND NOT precip_observed) AS precip_correct_negatives
FROM forecast_scores
WHERE valid_at > now() - interval '30 days' AND lead_hours BETWEEN 1 AND 48
GROUP BY lead_hours;

-- Daily rollup for the rolling-skill trend panel. DELIBERATELY lead 1..24
-- only: pooling in 48h leads (which mature 24 h later and score worse) would
-- shift the trend baseline as an artifact, not a real skill change. Per-lead
-- 48h skill lives in forecast_skill_7d/30d; LLM grading in forecast_confidence.
CREATE OR REPLACE VIEW forecast_skill_daily AS
SELECT
    time_bucket('1 day', valid_at)               AS day,
    avg(abs(temp_err_house))                     AS temp_mae_house,
    avg(abs(temp_err_obs))                       AS temp_mae_obs,
    avg(abs(fc_wind_kmh - obs_wind_kmh))         AS wind_mae,
    count(*) FILTER (WHERE precip_predicted AND precip_observed)      AS precip_hits,
    count(*) FILTER (WHERE precip_predicted AND NOT precip_observed) AS precip_false_alarms,
    count(*) FILTER (WHERE NOT precip_predicted AND precip_observed) AS precip_misses
FROM forecast_scores
WHERE lead_hours BETWEEN 1 AND 24
GROUP BY 1;

-- Headline single-row summary. DELIBERATELY lead 1..24 only: it grades the
-- next-24h horizon the predictor's load-forecast digest consumes.
CREATE OR REPLACE VIEW forecast_headline_7d AS
SELECT
    avg(abs(temp_err_house))                    AS temp_mae_house,
    avg(abs(temp_err_obs))                      AS temp_mae_obs,
    avg(temp_err_house)                         AS temp_bias_house,
    avg(abs(fc_wind_kmh - obs_wind_kmh))        AS wind_mae,
    count(*) FILTER (WHERE precip_predicted AND precip_observed)      AS precip_hits,
    count(*) FILTER (WHERE precip_predicted AND NOT precip_observed) AS precip_false_alarms,
    count(*) FILTER (WHERE NOT precip_predicted AND precip_observed) AS precip_misses,
    CASE WHEN count(*) FILTER (WHERE precip_observed) > 0
         THEN count(*) FILTER (WHERE precip_predicted AND precip_observed)::float
              / count(*) FILTER (WHERE precip_observed)
    END AS precip_hit_rate,
    CASE WHEN count(*) FILTER (WHERE precip_predicted) > 0
         THEN count(*) FILTER (WHERE precip_predicted AND NOT precip_observed)::float
              / count(*) FILTER (WHERE precip_predicted)
    END AS precip_false_alarm_rate
FROM forecast_scores
WHERE valid_at > now() - interval '7 days' AND lead_hours BETWEEN 1 AND 24;
