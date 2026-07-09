"""Thin psycopg wrapper: retrying connection + insert helpers.

All inserts that migration can replay use ON CONFLICT DO NOTHING against the
dedupe indexes (raw_frames_dedupe, events_dedupe) so re-runs are idempotent.
"""
from __future__ import annotations

import hashlib
import json
import logging
import threading
import time
from datetime import datetime

import psycopg

log = logging.getLogger("db")


class Db:
    """One connection, auto-reconnect, thread-safe via a lock (call rate is low)."""

    def __init__(self):
        self._conn: psycopg.Connection | None = None
        self._lock = threading.Lock()

    def connect(self, retries: int = 60) -> None:
        for attempt in range(retries):
            try:
                self._conn = psycopg.connect(autocommit=True)  # PG* env vars
                log.info("connected to postgres")
                return
            except psycopg.OperationalError as e:
                log.warning("postgres not ready (%s), retry %d/%d", e, attempt + 1, retries)
                time.sleep(5)
        raise RuntimeError("could not connect to postgres")

    def _exec(self, sql: str, params: tuple | list, many: bool = False) -> int:
        with self._lock:
            for attempt in (1, 2):
                try:
                    if self._conn is None or self._conn.closed:
                        self.connect()
                    with self._conn.cursor() as cur:
                        if many:
                            cur.executemany(sql, params)
                        else:
                            cur.execute(sql, params)
                        return cur.rowcount
                except psycopg.OperationalError as e:
                    log.warning("db error (%s), reconnecting", e)
                    try:
                        self._conn.close()
                    except Exception:
                        pass
                    self._conn = None
            return 0

    # -- raw_frames ---------------------------------------------------------
    def insert_raw_frames(self, rows: list[tuple]) -> int:
        """rows: (ts, millis, src, dst, msg_type, payload, payload_hash,
        valid, synthesized, truncated, source)"""
        if not rows:
            return 0
        return self._exec(
            "INSERT INTO raw_frames (ts, millis, src, dst, msg_type, payload,"
            " payload_hash, valid, synthesized, truncated, source)"
            " VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)"
            " ON CONFLICT (ts, millis, payload_hash) DO NOTHING",
            rows, many=True)

    # -- events --------------------------------------------------------------
    def insert_event(self, ts: datetime, kind: str, detail: dict) -> int:
        return self.insert_events([(ts, kind, detail)])

    def insert_events(self, rows: list[tuple[datetime, str, dict]]) -> int:
        if not rows:
            return 0
        return self._exec(
            "INSERT INTO events (ts, kind, detail) VALUES (%s,%s,%s)"
            " ON CONFLICT (ts, kind, detail_hash) DO NOTHING",
            [(ts, kind, json.dumps(detail, sort_keys=True)) for ts, kind, detail in rows],
            many=True)

    # -- MQTT-fed tables -------------------------------------------------------
    def insert_room_temp(self, ts, sensor_id, temp_c, occupied) -> None:
        self._exec("INSERT INTO room_temps (ts, sensor_id, temp_c, occupied)"
                   " VALUES (%s,%s,%s,%s)", (ts, sensor_id, temp_c, occupied))

    def insert_outdoor_temp(self, ts, temp_c, source="mqtt") -> None:
        self._exec("INSERT INTO outdoor_temps (ts, temp_c, source) VALUES (%s,%s,%s)",
                   (ts, temp_c, source))

    def insert_hvac_state(self, ts, mode, action, equipment, heat_sp, cool_sp,
                          fused_temp, extra: dict) -> None:
        self._exec(
            "INSERT INTO hvac_state (ts, mode, action, equipment, heat_sp,"
            " cool_sp, fused_temp, extra) VALUES (%s,%s,%s,%s,%s,%s,%s,%s)",
            (ts, mode, action, equipment, heat_sp, cool_sp, fused_temp,
             json.dumps(extra)))

    # -- forecasts -------------------------------------------------------------
    def insert_forecasts(self, rows: list[tuple]) -> int:
        """rows: (fetched_at, valid_at, temp_c, precip_mm, precip_prob_pct,
        wind_kmh, gust_kmh, wind_dir_deg, humidity_pct, code, condition, source)"""
        return self._exec(
            "INSERT INTO forecasts (fetched_at, valid_at, temp_c, precip_mm,"
            " precip_prob_pct, wind_kmh, gust_kmh, wind_dir_deg, humidity_pct,"
            " code, condition, source) VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)"
            " ON CONFLICT (fetched_at, valid_at, source) DO NOTHING",
            rows, many=True)


def frame_hash(raw: bytes) -> str:
    return hashlib.md5(raw).hexdigest()
