"""MQTT ingest (#132) — SUBSCRIBE ONLY. This module never publishes.

Observed payloads (2026-07-09 live sample):
  slytherm/sensors/<id>/state     {"temp":21.3,"occ":false}
  slytherm/sensors/<id>/presence  {"occupied":false, "last_seen":1783606334}
  slytherm/cmd/outdoor_temp       26.2                      (bare float, degC)
  slytherm/remote/state           {"heatC":16.5,"coolC":28,"mode":"cool",
                                   "action":"idle","equipment":"idle",
                                   "fusedTempC":21.8, ...}
  slytherm/boot                   {"reason":"wdt", ...}
  slytherm/controller/status      {"cid":"8d82f4","status":"online", ...}
  slytherm/state/ota              (json)
  slytherm/remote/<cid>/boot|status|state/ota
  slytherm/availability           online|offline            (LWT)
"""
from __future__ import annotations

import json
import logging
from datetime import datetime
from zoneinfo import ZoneInfo

import paho.mqtt.client as mqtt

from . import config
from .db import Db

log = logging.getLogger("mqtt")

SUBS = [
    "slytherm/sensors/+/state",
    "slytherm/sensors/+/presence",
    "slytherm/cmd/outdoor_temp",
    "slytherm/remote/state",
    "slytherm/boot",
    "slytherm/controller/status",
    "slytherm/state/ota",
    "slytherm/remote/+/boot",
    "slytherm/remote/+/status",
    "slytherm/remote/+/state/ota",
    "slytherm/availability",
]


def _json(payload: bytes):
    try:
        return json.loads(payload.decode("utf-8", "replace"))
    except (ValueError, UnicodeDecodeError):
        return None


def _bool(v) -> bool | None:
    if isinstance(v, bool):
        return v
    if isinstance(v, (int, float)):
        return bool(v)
    if isinstance(v, str):
        return v.strip().lower() in ("1", "true", "on", "yes", "occupied")
    return None


class MqttIngest:
    def __init__(self, db: Db):
        self.db = db
        self.tz = ZoneInfo(config.LOCAL_TZ)
        self._last_status: dict[str, str] = {}  # node -> last seen status
        self.client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id="slylog-collector", clean_session=True)
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.on_disconnect = self._on_disconnect

    def now(self) -> datetime:
        return datetime.now(self.tz)

    def run_forever(self) -> None:
        self.client.reconnect_delay_set(min_delay=1, max_delay=60)
        while True:
            try:
                self.client.connect(config.BROKER, config.BROKER_PORT, keepalive=60)
                break
            except OSError as e:
                log.warning("broker connect failed (%s), retrying", e)
                import time
                time.sleep(5)
        self.client.loop_forever(retry_first_connection=True)

    # ------------------------------------------------------------- callbacks
    def _on_connect(self, client, userdata, flags, reason_code, properties):
        log.info("connected to broker rc=%s; subscribing", reason_code)
        for topic in SUBS:
            client.subscribe(topic, qos=0)

    def _on_disconnect(self, client, userdata, flags, reason_code, properties):
        log.warning("broker disconnected rc=%s (auto-reconnect)", reason_code)

    def _on_message(self, client, userdata, msg):
        try:
            self._route(msg.topic, msg.payload)
        except Exception:
            log.exception("failed handling %s %r", msg.topic, msg.payload[:200])

    # --------------------------------------------------------------- routing
    def _route(self, topic: str, payload: bytes) -> None:
        ts = self.now()
        parts = topic.split("/")

        if len(parts) == 4 and parts[1] == "sensors":
            sensor, leaf = parts[2], parts[3]
            data = _json(payload) or {}
            if leaf == "state":
                self.db.insert_room_temp(ts, sensor, data.get("temp"),
                                         _bool(data.get("occ")))
            elif leaf == "presence":
                self.db.insert_room_temp(ts, sensor, None,
                                         _bool(data.get("occupied")))
            return

        if topic == "slytherm/cmd/outdoor_temp":
            try:
                self.db.insert_outdoor_temp(ts, float(payload.decode().strip()))
            except ValueError:
                log.warning("bad outdoor_temp payload %r", payload[:40])
            return

        if topic == "slytherm/remote/state":
            data = _json(payload)
            if isinstance(data, dict):
                self.db.insert_hvac_state(
                    ts, data.get("mode"), data.get("action"),
                    data.get("equipment"), data.get("heatC"), data.get("coolC"),
                    data.get("fusedTempC"), data)
            return

        # everything else -> events
        node = "controller"
        if len(parts) >= 3 and parts[1] == "remote" and parts[2] not in ("state",):
            node = f"remote/{parts[2]}"
        data = _json(payload)
        detail = {"topic": topic, "node": node}
        if isinstance(data, (dict, list)):
            detail["payload"] = data
        else:
            detail["payload"] = payload.decode("utf-8", "replace").strip()

        if topic.endswith("/boot") or topic == "slytherm/boot":
            self.db.insert_event(ts, "boot", detail)
        elif topic.endswith("/ota"):
            self.db.insert_event(ts, "ota", detail)
        else:  # status / availability — only record transitions (retained
               # topics replay on every reconnect)
            status = (data.get("status") if isinstance(data, dict)
                      else detail["payload"])
            key = topic
            if self._last_status.get(key) != status:
                self._last_status[key] = status
                self.db.insert_event(ts, "status", {**detail, "status": status})
