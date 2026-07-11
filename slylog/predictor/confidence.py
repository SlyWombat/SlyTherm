"""SlyLog weather-source confidence review (#141) — local-LLM graded, record-only.

Runs inside the predictor container as a sibling loop to the 4h load forecast
(started as a daemon thread from predictor.main; `python confidence.py` runs
one review immediately for manual/one-shot use).

Every CONFIDENCE_INTERVAL_H hours (default 24, first run staggered
CONFIDENCE_STAGGER_S after start so it never lands on top of a load-forecast
run), for each lead bucket ('24h' = lead 1-24 h, '48h' = lead 25-48 h):
  1. Pull the numeric skill of the freshest fully-matured window (last
     CONFIDENCE_WINDOW_DAYS days of graded rows in forecast_scores at that
     lead): temp MAE + bias, precip hit/false-alarm rates, wind MAE.
  2. Ask the local Ollama model for STRICT JSON
       {"confidence": <0-100>, "rationale": "<= 2 sentences"}
     (retried once on a malformed answer).
  3. Insert into forecast_confidence. If the LLM fails twice, insert the same
     numeric row with model='fallback' and a deterministic MAE-threshold score
     instead — the table NEVER silently gaps on LLM flakiness.
  Insufficient matured data (the 48h bucket for ~2 days after the horizon
  extension) -> log + skip that bucket, nothing inserted.

Like the predictor: no MQTT client, no write path to SlyTherm. Reads the DB,
talks to Ollama, writes forecast_confidence rows. Nothing else, ever.
"""
from __future__ import annotations

import json
import logging
import os
import time

import psycopg
import requests

from predictor import MODEL, OLLAMA_TIMEOUT_S, OLLAMA_URL, connect_db, jdumps, q

log = logging.getLogger("confidence")

INTERVAL_S = float(os.environ.get("CONFIDENCE_INTERVAL_H", "24")) * 3600
STAGGER_S = float(os.environ.get("CONFIDENCE_STAGGER_S", "1800"))
WINDOW_DAYS = int(os.environ.get("CONFIDENCE_WINDOW_DAYS", "7"))
MIN_SAMPLES = int(os.environ.get("CONFIDENCE_MIN_SAMPLES", "24"))

# lead_bucket -> inclusive lead_hours range in forecast_scores
BUCKETS: dict[str, tuple[int, int]] = {"24h": (1, 24), "48h": (25, 48)}

PROMPT_TEMPLATE = """You are auditing a weather forecast source (Open-Meteo \
hourly) used by a home HVAC controller in Mississauga, Ontario. Below are \
verification statistics for its {bucket}-lead forecasts, computed against \
actuals over the matured review window:

{numbers}

Definitions: temp_mae_c = mean absolute temperature error in deg C \
(truth: {temp_truth}); temp_bias_c = mean signed error (positive = forecast \
too warm); precip_hit_rate = fraction of observed rain hours that were \
predicted (prob >= 50%); precip_false_alarm_rate = fraction of rain \
predictions with no rain observed; wind_mae_kmh = mean absolute wind speed \
error; null = not enough data for that metric yet.

Rate the overall confidence this source deserves at this lead time as an \
integer 0-100: 90+ nearly perfect (temp MAE well under 1 C, rain reliable), \
70-89 good enough to act on, 40-69 useful but hedge, below 40 poor. Weigh \
temperature most (it drives HVAC), then precipitation, then wind.

Respond with ONLY a JSON object, no markdown, no commentary, exactly:
{{"confidence": <integer 0-100>, "rationale": "<at most 2 sentences>"}}"""


# ------------------------------------------------------------------- numerics
def bucket_scores(conn, lead_lo: int, lead_hi: int) -> dict | None:
    """Numeric skill snapshot for one lead bucket over the freshest matured
    window, or None when there is not enough graded data yet."""
    row = q(conn, """
        SELECT count(*),
               min(valid_at), max(valid_at),
               count(temp_err_house), avg(abs(temp_err_house)), avg(temp_err_house),
               count(temp_err_obs),   avg(abs(temp_err_obs)),   avg(temp_err_obs),
               avg(abs(fc_wind_kmh - obs_wind_kmh)),
               count(*) FILTER (WHERE precip_predicted AND precip_observed),
               count(*) FILTER (WHERE precip_predicted AND NOT precip_observed),
               count(*) FILTER (WHERE NOT precip_predicted AND precip_observed)
        FROM forecast_scores
        WHERE lead_hours BETWEEN %s AND %s
          AND valid_at > now() - make_interval(days => %s)""",
            (lead_lo, lead_hi, WINDOW_DAYS))[0]
    (n, w_start, w_end, n_house, mae_house, bias_house,
     n_obs, mae_obs, bias_obs, wind_mae, hits, fas, misses) = row

    if n < MIN_SAMPLES:
        return None
    # temp truth: house OAT sensor when graded, else Open-Meteo current analysis
    if n_house and mae_house is not None:
        truth, mae, bias = "house OAT sensor", mae_house, bias_house
    elif n_obs and mae_obs is not None:
        truth, mae, bias = "Open-Meteo current analysis", mae_obs, bias_obs
    else:
        return None

    def f(v, nd=2):
        return round(float(v), nd) if v is not None else None

    return {
        "n_samples": int(n),
        "window_start": w_start, "window_end": w_end,
        "temp_truth": truth,
        "temp_mae_c": f(mae), "temp_bias_c": f(bias),
        "wind_mae_kmh": f(wind_mae, 1),
        "precip_hit_rate": f(hits / (hits + misses)) if hits + misses > 0 else None,
        "precip_false_alarm_rate": f(fas / (hits + fas)) if hits + fas > 0 else None,
        "precip_hits": int(hits), "precip_false_alarms": int(fas),
        "precip_misses": int(misses),
    }


def fallback_score(m: dict) -> int:
    """Deterministic MAE-threshold score used when the LLM answer is unusable
    twice. Same weighting philosophy as the prompt: temp first, then precip,
    then wind. Perfect skill -> 100; each metric burns a capped penalty."""
    score = 100.0
    score -= min(45.0, 15.0 * m["temp_mae_c"])          # 1 C MAE costs 15
    score -= min(15.0, 10.0 * abs(m["temp_bias_c"]))    # systematic bias extra
    if m["wind_mae_kmh"] is not None:
        score -= min(15.0, 1.5 * m["wind_mae_kmh"])
    if m["precip_hit_rate"] is not None:
        score -= 12.5 * (1.0 - m["precip_hit_rate"])
    if m["precip_false_alarm_rate"] is not None:
        score -= 12.5 * m["precip_false_alarm_rate"]
    return max(0, min(100, round(score)))


# --------------------------------------------------------------------- ollama
def ask_llm(bucket: str, m: dict) -> tuple[int, str]:
    numbers = {k: v for k, v in m.items() if k not in ("window_start", "window_end")}
    prompt = PROMPT_TEMPLATE.format(bucket=bucket, temp_truth=m["temp_truth"],
                                    numbers=jdumps(numbers, indent=1))
    resp = requests.post(f"{OLLAMA_URL}/api/generate", json={
        "model": MODEL, "prompt": prompt, "stream": False, "format": "json",
        "options": {"temperature": 0.2, "num_ctx": 4096},
    }, timeout=OLLAMA_TIMEOUT_S)
    resp.raise_for_status()
    answer = json.loads(resp.json()["response"])
    conf = round(float(answer["confidence"]))
    if not 0 <= conf <= 100:
        raise ValueError(f"confidence out of range: {answer['confidence']!r}")
    rationale = str(answer["rationale"]).strip()
    if not rationale:
        raise ValueError("empty rationale")
    return conf, rationale[:500]


# ----------------------------------------------------------------------- main
def store(conn, bucket: str, m: dict, score: int, rationale: str, model: str) -> None:
    with conn.cursor() as cur:
        cur.execute("""
            INSERT INTO forecast_confidence
                (source, lead_bucket, window_start, window_end, n_samples,
                 temp_mae_c, temp_bias_c, precip_hit_rate,
                 precip_false_alarm_rate, wind_mae_kmh,
                 confidence_score, rationale, model)
            VALUES ('open-meteo', %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)""",
            (bucket, m["window_start"], m["window_end"], m["n_samples"],
             m["temp_mae_c"], m["temp_bias_c"], m["precip_hit_rate"],
             m["precip_false_alarm_rate"], m["wind_mae_kmh"],
             score, rationale, model))


def review_bucket(conn, bucket: str) -> None:
    lead_lo, lead_hi = BUCKETS[bucket]
    m = bucket_scores(conn, lead_lo, lead_hi)
    if m is None:
        log.info("%s: <%d graded samples in last %dd — skipping (matures later)",
                 bucket, MIN_SAMPLES, WINDOW_DAYS)
        return
    for attempt in (1, 2):
        try:
            score, rationale = ask_llm(bucket, m)
            store(conn, bucket, m, score, rationale, MODEL)
            log.info("%s: confidence=%d (%s) | %s", bucket, score, MODEL, rationale)
            return
        except psycopg.Error:
            raise  # db trouble is not the LLM's fault — let the loop reconnect
        except Exception as e:
            log.warning("%s: LLM attempt %d unusable (%s)", bucket, attempt, e)
    score = fallback_score(m)
    rationale = (f"Deterministic fallback (LLM unavailable/malformed twice): "
                 f"temp MAE {m['temp_mae_c']} C, bias {m['temp_bias_c']} C, "
                 f"wind MAE {m['wind_mae_kmh']} km/h, "
                 f"precip hit rate {m['precip_hit_rate']}, "
                 f"false-alarm rate {m['precip_false_alarm_rate']} "
                 f"over {m['n_samples']} samples.")
    store(conn, bucket, m, score, rationale, "fallback")
    log.warning("%s: fallback confidence=%d", bucket, score)


def run_once(conn) -> None:
    for bucket in BUCKETS:
        review_bucket(conn, bucket)


def run_forever() -> None:
    """Daemon-thread entry point (own DB connection — never shared with the
    load-forecast loop)."""
    log.info("confidence loop: every %.1fh, first run in %ds, window %dd",
             INTERVAL_S / 3600, int(STAGGER_S), WINDOW_DAYS)
    time.sleep(STAGGER_S)
    conn = connect_db()
    while True:
        started = time.time()
        try:
            run_once(conn)
        except psycopg.OperationalError:
            log.exception("db connection lost, reconnecting")
            conn = connect_db()
        except Exception:
            log.exception("confidence review failed")
        time.sleep(max(60.0, INTERVAL_S - (time.time() - started)))


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s confidence %(levelname)s %(message)s")
    run_once(connect_db())
