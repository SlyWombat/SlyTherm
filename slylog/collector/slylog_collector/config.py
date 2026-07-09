"""Environment-driven configuration (one .env at the compose level)."""
import os

BROKER = os.environ.get("BROKER", "192.168.10.12")
BROKER_PORT = int(os.environ.get("BROKER_PORT", "1883"))
CONTROLLER_IP = os.environ.get("CONTROLLER_IP", "192.168.10.13")
CONTROLLER_PORT = int(os.environ.get("CONTROLLER_PORT", "23"))
LAT = os.environ.get("LAT", "CHANGE_ME")
LON = os.environ.get("LON", "CHANGE_ME")
ARCHIVE_PATH = os.environ.get("ARCHIVE_PATH", "/captures-vol/ct485-live.log")
LOCAL_TZ = os.environ.get("TZ", "America/Toronto")

# psycopg reads PGHOST/PGPORT/PGUSER/PGPASSWORD/PGDATABASE from the env itself.

TELNET_IDLE_LIMIT_S = 120     # [ct485-stats] beats every 30 s while capturing;
                              # 2 min of silence = dead link -> reconnect
TELNET_RECONNECT_DELAY_S = 10
FORECAST_INTERVAL_S = 3600
STATS_HEARTBEAT_S = 600       # store a ct485_stats event at least this often
NOVEL_CMD_INTERVAL_S = 3600   # rate-limit novel_command events per command code
