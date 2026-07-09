"""SlyLog collector — READ-ONLY ingest of SlyTherm MQTT + CT-485 telnet mirror
+ Open-Meteo forecasts into TimescaleDB (#130, #132, #133, #134).

Never publishes to MQTT, never writes to the controller. The only sockets it
opens toward SlyTherm are a subscribing MQTT client and a telnet reader.
"""
