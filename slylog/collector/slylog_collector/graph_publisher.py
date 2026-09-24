"""graph_publisher — publish the System-tab trend graph series to MQTT (#156).

A SEPARATE process from the collector (which by design never publishes). Every
STEP_S it downsamples the last 12 h from TimescaleDB into N=144 five-minute
buckets and publishes a RETAINED `slytherm/graph/system` with three series, in
INT DECI-DEGREES (temp x 10) so the device drops them straight into its LVGL
ring buffers. -32768 (= LV_CHART_POINT_NONE / INT16_MIN) marks a bucket with no
data. Values are aligned to a fixed grid ending at the newest whole bucket:

  {"step_s":300,"n":144,"end":<unix of last bucket>,
   "oat":[...], "set":[...], "room":[...]}

  oat  = outside temperature        (outdoor_temps.temp_c)
  set  = active setpoint            (cool sp, or heat sp when mode=heat)
  room = averaged/fused room temp   (shadow_demands.fused_temp_c — the controller's
                                     basement-excluded, occupancy-weighted control input)
"""
import json
import logging
import time

import paho.mqtt.client as mqtt
import psycopg

from . import config

log = logging.getLogger("graph_publisher")

TOPIC = "slytherm/graph/system"
N = 144            # chart points (matches firmware kGraphPts)
STEP_S = 300       # 5-minute buckets -> 12 h window
NONE = -32768      # LV_CHART_POINT_NONE on the device

# (bucket_ts, value) -> {bucket_index (epoch//STEP_S): value}
_Q_OAT = ("select time_bucket('5 minutes', ts) b, avg(temp_c) "
          "from outdoor_temps where ts > now() - interval '12 hours' group by b")
_Q_ROOM = ("select time_bucket('5 minutes', ts) b, avg(fused_temp_c) "
           "from shadow_demands where ts > now() - interval '12 hours' group by b")
_Q_SET = ("select time_bucket('5 minutes', ts) b, "
          "avg(case when mode = 1 then set_heat_c else set_cool_c end) "
          "from shadow_demands where ts > now() - interval '12 hours' group by b")


def _bucketed(conn, sql):
    out = {}
    with conn.cursor() as cur:
        cur.execute(sql)
        for b, v in cur.fetchall():
            if v is not None:
                out[int(b.timestamp()) // STEP_S] = float(v)
    return out


def _series(m, base):
    return [int(round(m[base + i] * 10)) if (base + i) in m else NONE for i in range(N)]


def run():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")
    conn = psycopg.connect(autocommit=True)  # PG* env vars, same as the collector
    cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="slylog-graph-pub")
    cli.connect(config.BROKER, config.BROKER_PORT, keepalive=60)
    cli.loop_start()
    log.info("graph_publisher up: broker=%s topic=%s", config.BROKER, TOPIC)
    while True:
        try:
            base = (int(time.time()) // STEP_S) - (N - 1)
            oat = _bucketed(conn, _Q_OAT)
            room = _bucketed(conn, _Q_ROOM)
            setp = _bucketed(conn, _Q_SET)
            payload = json.dumps(
                {"step_s": STEP_S, "n": N, "end": (base + N - 1) * STEP_S,
                 "oat": _series(oat, base), "set": _series(setp, base), "room": _series(room, base)},
                separators=(",", ":"))
            cli.publish(TOPIC, payload, qos=0, retain=True)
            log.info("published: oat=%d room=%d set=%d buckets, %d bytes",
                     len(oat), len(room), len(setp), len(payload))
        except Exception as e:  # noqa: BLE001 — keep the loop alive across DB/broker blips
            log.warning("publish failed: %s", e)
            try:
                if conn.closed:
                    conn = psycopg.connect(autocommit=True)
            except Exception:
                pass
        time.sleep(STEP_S)


if __name__ == "__main__":
    run()
