"""Collector entrypoint: MQTT ingest + Open-Meteo (threads) + telnet ingest
(asyncio, main thread). One process, one DB connection per writer."""
from __future__ import annotations

import asyncio
import logging
import threading

from . import openmeteo
from .db import Db
from .mqtt_ingest import MqttIngest
from .telnet_ingest import TelnetIngest

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(name)s %(levelname)s %(message)s")
log = logging.getLogger("main")


def main() -> None:
    # Independent connections: psycopg connections are not thread-safe and the
    # three ingest paths run concurrently.
    mqtt_db, weather_db, telnet_db = Db(), Db(), Db()
    for d in (mqtt_db, weather_db, telnet_db):
        d.connect()

    threading.Thread(target=MqttIngest(mqtt_db).run_forever,
                     name="mqtt", daemon=True).start()
    threading.Thread(target=openmeteo.run_forever, args=(weather_db,),
                     name="openmeteo", daemon=True).start()

    log.info("starting telnet ingest (sole long-term client of the mirror)")
    asyncio.run(TelnetIngest(telnet_db).run())


if __name__ == "__main__":
    main()
