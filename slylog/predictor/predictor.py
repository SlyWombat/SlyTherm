"""SlyLog predictor (#136) — local-LLM HVAC load forecast, RECORD-ONLY.

Every PREDICT_INTERVAL_H hours:
  1. Build a structured digest from the DB: last-24h cooling/heating cycles,
     room + outdoor temperature trajectories, occupancy, latest 24h forecast.
  2. Ask the local Ollama model for strict JSON:
       {expected_cooling_hours, expected_heating_hours, peak_window,
        confidence, recommendation}
     Full inputs + outputs stored in `predictions` for reproducibility.
     Timeouts/failures produce a failure row so silent skips are visible.
  3. Compute + store a degree-day linear baseline row alongside every run
     so the LLM stays scoreable against an honest model.

This process has NO MQTT client and NO write path to SlyTherm. It reads the
database, talks to Ollama, and writes prediction rows. Nothing else, ever.
"""
from __future__ import annotations

import json
import logging
import os
import time
from datetime import datetime, timezone
from decimal import Decimal

import psycopg
import requests


def jdumps(obj, **kw) -> str:
    """json.dumps that survives SQL numerics (Decimal) and timestamps."""
    def default(o):
        if isinstance(o, Decimal):
            return float(o)
        if isinstance(o, datetime):
            return o.isoformat()
        return str(o)
    return json.dumps(obj, default=default, **kw)

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s predictor %(levelname)s %(message)s")
log = logging.getLogger("predictor")

OLLAMA_URL = os.environ.get("OLLAMA_URL", "http://ollama:11434")
MODEL = os.environ.get("OLLAMA_MODEL", "qwen2.5:7b-instruct")
INTERVAL_S = float(os.environ.get("PREDICT_INTERVAL_H", "4")) * 3600
OLLAMA_TIMEOUT_S = int(os.environ.get("OLLAMA_TIMEOUT_S", "900"))
BASE_C = 18.0  # degree-day balance point

PROMPT_TEMPLATE = """You are an HVAC load forecaster for a single-family house \
in Mississauga, Ontario (dual-fuel: gas furnace + AC, staged cooling at 30%).
Below is a machine-generated digest of the last 24 hours of operation and the \
weather forecast for the next 24 hours.

{digest}

Predict the NEXT 24 hours. Respond with ONLY a JSON object, no markdown, \
no commentary, exactly these keys:
{{
  "expected_cooling_hours": <float, total compressor-on hours in next 24h>,
  "expected_heating_hours": <float, total heating-on hours in next 24h>,
  "peak_window": "<HH:MM-HH:MM local time span of highest expected load>",
  "confidence": <float 0.0-1.0>,
  "recommendation": "<one short paragraph of plain-language advice for the homeowner>"
}}"""


# --------------------------------------------------------------------------- db
def connect_db() -> psycopg.Connection:
    while True:
        try:
            return psycopg.connect(autocommit=True)  # PG* env
        except psycopg.OperationalError as e:
            log.warning("db not ready (%s), retrying", e)
            time.sleep(5)


def q(conn, sql: str, params=()) -> list[tuple]:
    with conn.cursor() as cur:
        cur.execute(sql, params)
        return cur.fetchall()


def cycles_last_24h(conn, cmd: str) -> list[dict]:
    """Pair cycle_start/cycle_stop events for one demand command."""
    rows = q(conn, """
        SELECT ts, kind, (detail->>'pct')::float
        FROM events
        WHERE kind IN ('cycle_start','cycle_stop')
          AND detail->>'cmd' = %s
          AND ts > now() - interval '25 hours'
        ORDER BY ts""", (cmd,))
    cycles, start, pct = [], None, None
    for ts, kind, p in rows:
        if kind == "cycle_start":
            start, pct = ts, p
        elif kind == "cycle_stop" and start is not None:
            cycles.append({"start": start.isoformat(), "stop": ts.isoformat(),
                           "minutes": round((ts - start).total_seconds() / 60, 1),
                           "demand_pct": pct})
            start = None
    if start is not None:
        cycles.append({"start": start.isoformat(), "stop": None,
                       "minutes": round((datetime.now(timezone.utc) - start)
                                        .total_seconds() / 60, 1),
                       "demand_pct": pct, "still_running": True})
    return cycles


def build_digest(conn) -> dict:
    digest: dict = {"generated_at": datetime.now().astimezone().isoformat()}

    cool = cycles_last_24h(conn, "COOL_DEMAND")
    heat = cycles_last_24h(conn, "HEAT_DEMAND")
    digest["cooling_cycles_last_24h"] = cool
    digest["heating_cycles_last_24h"] = heat
    digest["cooling_on_hours_last_24h"] = round(sum(c["minutes"] for c in cool) / 60, 2)
    digest["heating_on_hours_last_24h"] = round(sum(c["minutes"] for c in heat) / 60, 2)

    digest["room_temps_last_24h"] = [
        {"sensor": s, "avg_c": round(a, 2), "min_c": round(lo, 2),
         "max_c": round(hi, 2), "latest_c": round(last, 2),
         # avg(CASE...1.0...) is SQL numeric -> Decimal; cast before rounding
         "occupied_fraction": round(float(occ or 0), 2)}
        for s, a, lo, hi, last, occ in q(conn, """
            SELECT sensor_id, avg(temp_c), min(temp_c), max(temp_c),
                   (array_agg(temp_c ORDER BY ts DESC))[1],
                   avg(CASE WHEN occupied THEN 1.0 ELSE 0.0 END)
            FROM room_temps
            WHERE ts > now() - interval '24 hours' AND temp_c IS NOT NULL
            GROUP BY sensor_id ORDER BY sensor_id""")]

    digest["outdoor_hourly_last_24h"] = [
        {"hour": h.isoformat(), "avg_c": round(t, 1)}
        for h, t in q(conn, """
            SELECT time_bucket('1 hour', ts) h, avg(temp_c)
            FROM outdoor_temps WHERE ts > now() - interval '24 hours'
            GROUP BY 1 ORDER BY 1""")]

    digest["hvac_now"] = next(iter([
        {"mode": m, "action": a, "heat_sp": hs, "cool_sp": cs, "fused_temp": ft}
        for m, a, hs, cs, ft in q(conn, """
            SELECT mode, action, heat_sp, cool_sp, fused_temp
            FROM hvac_state ORDER BY ts DESC LIMIT 1""")]), None)

    digest["forecast_next_24h"] = [
        {"hour": v.isoformat(), "temp_c": t, "precip_mm": p, "precip_prob_pct": pp,
         "wind_kmh": w, "humidity_pct": h, "condition": c}
        for v, t, p, pp, w, h, c in q(conn, """
            SELECT valid_at, temp_c, precip_mm, precip_prob_pct, wind_kmh,
                   humidity_pct, condition
            FROM forecasts
            WHERE fetched_at = (SELECT max(fetched_at) FROM forecasts)
            ORDER BY valid_at""")]

    # Forecast trustworthiness (#138): rolling 7d skill vs actuals, so the
    # LLM knows how much weight the forecast above deserves. Null-safe: all
    # fields are null until enough graded history accrues.
    def _f(v, nd=2):
        return round(float(v), nd) if v is not None else None

    try:
        row = q(conn, """
            SELECT temp_mae_house, temp_bias_house, temp_mae_obs, wind_mae,
                   precip_hit_rate, precip_false_alarm_rate,
                   precip_hits, precip_false_alarms, precip_misses
            FROM forecast_headline_7d""")
        r = row[0] if row else [None] * 9
        digest["forecast_skill_last_7d"] = {
            "note": ("accuracy of past forecasts vs actuals; temp truth = house "
                     "OAT sensor, rain/wind truth = open-meteo current analysis; "
                     "nulls mean not enough graded history yet"),
            "temp_mae_vs_house_c": _f(r[0]),
            "temp_bias_vs_house_c": _f(r[1]),
            "temp_mae_vs_obs_c": _f(r[2]),
            "wind_mae_kmh": _f(r[3]),
            "precip_hit_rate": _f(r[4]),
            "precip_false_alarm_rate": _f(r[5]),
            "precip_hits": int(r[6]) if r[6] is not None else None,
            "precip_false_alarms": int(r[7]) if r[7] is not None else None,
            "precip_misses": int(r[8]) if r[8] is not None else None,
        }
    except psycopg.Error:
        log.exception("forecast_headline_7d unavailable — digest goes without it")
        digest["forecast_skill_last_7d"] = None
    return digest


# ---------------------------------------------------------------------- ollama
def ensure_model() -> None:
    """Pull the model if missing (first run on a fresh volume takes a while)."""
    while True:
        try:
            tags = requests.get(f"{OLLAMA_URL}/api/tags", timeout=10).json()
            break
        except requests.RequestException as e:
            log.warning("ollama not ready (%s), retrying", e)
            time.sleep(5)
    have = {m["name"] for m in tags.get("models", [])}
    if MODEL in have or f"{MODEL}:latest" in have:
        log.info("model %s already present", MODEL)
        return
    log.info("pulling model %s (may take many minutes)...", MODEL)
    with requests.post(f"{OLLAMA_URL}/api/pull", json={"model": MODEL},
                       stream=True, timeout=7200) as r:
        r.raise_for_status()
        for line in r.iter_lines():
            if line:
                status = json.loads(line).get("status", "")
                if "pulling" not in status:
                    log.info("pull: %s", status)
    log.info("model %s ready", MODEL)


def ask_llm(digest: dict) -> dict:
    prompt = PROMPT_TEMPLATE.format(digest=jdumps(digest, indent=1))
    resp = requests.post(f"{OLLAMA_URL}/api/generate", json={
        "model": MODEL, "prompt": prompt, "stream": False, "format": "json",
        "options": {"temperature": 0.2, "num_ctx": 8192},
    }, timeout=OLLAMA_TIMEOUT_S)
    resp.raise_for_status()
    answer = json.loads(resp.json()["response"])
    # strict shape check
    for key in ("expected_cooling_hours", "expected_heating_hours",
                "peak_window", "confidence", "recommendation"):
        if key not in answer:
            raise ValueError(f"LLM answer missing key {key!r}: {answer}")
    return answer


# -------------------------------------------------------------------- baseline
def degree_day_baseline(conn, digest: dict) -> dict:
    """Honest linear baseline: on-hours ~= slope * degree-hours (through origin),
    slope fit over up to 14 daily buckets of history, falling back to the
    last-24h ratio while history is short."""
    daily = q(conn, """
        WITH dd AS (
            SELECT time_bucket('1 day', ts) d,
                   sum(greatest(temp_c - %s, 0)) / count(*) * 24 AS cdh,
                   sum(greatest(%s - temp_c, 0)) / count(*) * 24 AS hdh
            FROM outdoor_temps WHERE ts > now() - interval '14 days'
            GROUP BY 1),
        cyc AS (
            SELECT time_bucket('1 day', ts) d, detail->>'cmd' cmd, kind, ts,
                   lead(ts) OVER (PARTITION BY detail->>'cmd' ORDER BY ts) next_ts,
                   lead(kind) OVER (PARTITION BY detail->>'cmd' ORDER BY ts) next_kind
            FROM events
            WHERE kind IN ('cycle_start','cycle_stop')
              AND ts > now() - interval '14 days'),
        onh AS (
            SELECT d, cmd,
                   sum(extract(epoch FROM next_ts - ts)) / 3600 AS hours
            FROM cyc WHERE kind = 'cycle_start' AND next_kind = 'cycle_stop'
            GROUP BY 1, 2)
        SELECT dd.d, dd.cdh, dd.hdh,
               coalesce(c.hours, 0) AS cool_h, coalesce(h.hours, 0) AS heat_h
        FROM dd
        LEFT JOIN onh c ON c.d = dd.d AND c.cmd = 'COOL_DEMAND'
        LEFT JOIN onh h ON h.d = dd.d AND h.cmd = 'HEAT_DEMAND'
        ORDER BY dd.d""", (BASE_C, BASE_C))

    def slope(pairs: list[tuple[float, float]]) -> float:
        # SQL numeric arrives as Decimal — normalize to float before mixing
        pairs = [(float(x), float(y)) for x, y in pairs]
        num = sum(x * y for x, y in pairs)
        den = sum(x * x for x, y in pairs)
        return num / den if den > 0 else 0.0

    slope_cool = slope([(r[1] or 0, r[3] or 0) for r in daily])
    slope_heat = slope([(r[2] or 0, r[4] or 0) for r in daily])

    fc = digest.get("forecast_next_24h", [])
    cdh24 = sum(max((h["temp_c"] or 0) - BASE_C, 0) for h in fc)
    hdh24 = sum(max(BASE_C - (h["temp_c"] or 0), 0) for h in fc)

    return {
        "expected_cooling_hours": round(slope_cool * cdh24, 2),
        "expected_heating_hours": round(slope_heat * hdh24, 2),
        "method": "degree-day linear (through-origin least squares)",
        "base_c": BASE_C,
        "cdh_next_24h": round(cdh24, 1), "hdh_next_24h": round(hdh24, 1),
        "slope_cool_h_per_cdh": round(slope_cool, 4),
        "slope_heat_h_per_hdh": round(slope_heat, 4),
        "history_days_used": len(daily),
    }


# ------------------------------------------------------------------------ main
def store(conn, model: str, kind: str, status: str, inputs: dict,
          prediction: dict, recommendation: str | None, error: str | None) -> None:
    with conn.cursor() as cur:
        cur.execute(
            "INSERT INTO predictions (model, kind, status, inputs, prediction,"
            " recommendation, error) VALUES (%s,%s,%s,%s,%s,%s,%s)",
            (model, kind, status, jdumps(inputs), jdumps(prediction),
             recommendation, error))


def run_once(conn) -> None:
    digest = build_digest(conn)

    # baseline first — it must exist even when the LLM fails
    try:
        baseline = degree_day_baseline(conn, digest)
        store(conn, "degree-day-linear", "baseline", "ok",
              {"forecast_next_24h": digest.get("forecast_next_24h", [])},
              baseline, None, None)
        log.info("baseline: cool=%.2fh heat=%.2fh",
                 baseline["expected_cooling_hours"],
                 baseline["expected_heating_hours"])
    except Exception as e:
        log.exception("baseline failed")
        store(conn, "degree-day-linear", "baseline", "error", {}, {}, None, str(e))

    try:
        answer = ask_llm(digest)
        rec = str(answer.pop("recommendation"))
        store(conn, MODEL, "llm", "ok", digest, answer, rec, None)
        log.info("llm: %s | %s", json.dumps(answer), rec[:120])
    except requests.Timeout:
        log.error("ollama timed out after %ds", OLLAMA_TIMEOUT_S)
        store(conn, MODEL, "llm", "timeout", digest, {}, None,
              f"ollama timeout after {OLLAMA_TIMEOUT_S}s")
    except Exception as e:
        log.exception("llm prediction failed")
        store(conn, MODEL, "llm", "error", digest, {}, None, str(e))


def main() -> None:
    conn = connect_db()
    ensure_model()
    while True:
        started = time.time()
        try:
            run_once(conn)
        except psycopg.OperationalError:
            log.exception("db connection lost, reconnecting")
            conn = connect_db()
        except Exception:
            log.exception("run failed")
        time.sleep(max(60.0, INTERVAL_S - (time.time() - started)))


if __name__ == "__main__":
    main()
