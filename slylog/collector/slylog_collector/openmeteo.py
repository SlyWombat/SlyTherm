"""Hourly Open-Meteo forecast ingest (#134): NEXT 24 hours, keyed
(fetched_at, valid_at) so every fetch snapshots the full horizon.
Carries rain + wind + humidity for the garden's future watering suppression.
"""
from __future__ import annotations

import logging
import time
from datetime import datetime, timezone

import requests

from . import config
from .db import Db

log = logging.getLogger("openmeteo")

API = "https://api.open-meteo.com/v1/forecast"
HOURLY_VARS = ("temperature_2m,precipitation,precipitation_probability,"
               "wind_speed_10m,wind_gusts_10m,wind_direction_10m,"
               "relative_humidity_2m,weather_code")

# WMO weather interpretation codes -> human condition
WMO_CODES = {
    0: "clear", 1: "mainly clear", 2: "partly cloudy", 3: "overcast",
    45: "fog", 48: "rime fog",
    51: "light drizzle", 53: "drizzle", 55: "dense drizzle",
    56: "freezing drizzle", 57: "dense freezing drizzle",
    61: "light rain", 63: "rain", 65: "heavy rain",
    66: "freezing rain", 67: "heavy freezing rain",
    71: "light snow", 73: "snow", 75: "heavy snow", 77: "snow grains",
    80: "light showers", 81: "showers", 82: "violent showers",
    85: "snow showers", 86: "heavy snow showers",
    95: "thunderstorm", 96: "thunderstorm w/ hail", 99: "thunderstorm w/ heavy hail",
}


def coords_configured() -> bool:
    try:
        float(config.LAT), float(config.LON)
        return True
    except (TypeError, ValueError):
        return False


def fetch_once(db: Db) -> int:
    """One fetch of the next 24 h; returns rows inserted."""
    if not coords_configured():
        log.warning("LAT/LON not configured (LAT=%r LON=%r) — skipping forecast "
                    "fetch until real coordinates are set in .env",
                    config.LAT, config.LON)
        return 0
    resp = requests.get(API, params={
        "latitude": float(config.LAT), "longitude": float(config.LON),
        "hourly": HOURLY_VARS, "forecast_hours": 24, "timezone": "UTC",
    }, timeout=30)
    resp.raise_for_status()
    hourly = resp.json()["hourly"]
    fetched_at = datetime.now(timezone.utc)
    rows = []
    for i, iso in enumerate(hourly["time"]):
        valid_at = datetime.fromisoformat(iso).replace(tzinfo=timezone.utc)
        code = hourly["weather_code"][i]
        rows.append((
            fetched_at, valid_at,
            hourly["temperature_2m"][i],
            hourly["precipitation"][i],
            hourly["precipitation_probability"][i],
            hourly["wind_speed_10m"][i],
            hourly["wind_gusts_10m"][i],
            hourly["wind_direction_10m"][i],
            hourly["relative_humidity_2m"][i],
            code, WMO_CODES.get(code, f"wmo-{code}"), "open-meteo",
        ))
    n = db.insert_forecasts(rows)
    log.info("forecast: stored %d hourly rows (fetched_at=%s)", n, fetched_at.isoformat())
    return n


def run_forever(db: Db) -> None:
    while True:
        try:
            fetch_once(db)
        except Exception:
            log.exception("forecast fetch failed (will retry next hour)")
        time.sleep(config.FORECAST_INTERVAL_S)
