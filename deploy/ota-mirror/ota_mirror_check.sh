#!/bin/bash
# ota_mirror_check.sh — is the LAN OTA mirror serving CURRENT content?
#
# `docker ps` says Up and nginx says 200 even when the populating side is dead
# (SlyTherm #206). The only honest signal is status.json, which the sync loop
# rewrites every pass. This check fails (exit 1, message on stderr) when:
#   - status.json or catalog.json is not served (edge or bind mount broken)
#   - status.json is older than max(3 x interval, 15 min): the loop is dead/wedged
#   - status.json says alert=true: no successful upstream sync for stale-after
# A hold (state=held) is NOT a failure — it is a deliberate, recorded state.
#
# Usage: ota_mirror_check.sh [http://host:8090]   (exit 0 = fresh)
set -u
BASE="${1:-http://127.0.0.1:8090}"
MAX_AGE_MIN=900

status=$(curl -fsS --max-time 20 "$BASE/status.json") || { echo "mirror: status.json not served at $BASE" >&2; exit 1; }
curl -fsS --max-time 20 -o /dev/null "$BASE/catalog.json" || { echo "mirror: catalog.json not served at $BASE" >&2; exit 1; }

STATUS_JSON="$status" python3 - "$MAX_AGE_MIN" <<'PY'
import json, os, sys, time
max_age_min = int(sys.argv[1])
s = json.loads(os.environ["STATUS_JSON"])
now = int(time.time())
age = now - int(s.get("updatedEpoch") or 0)
interval = int(s.get("intervalSeconds") or 300)
max_age = max(3 * interval, max_age_min)
problems = []
if age > max_age:
    problems.append(f"status.json is {age}s old (limit {max_age}s): sync loop dead or wedged")
if s.get("alert"):
    problems.append(f"sync alert: state={s.get('state')} lastError={s.get('lastError')} "
                    f"secondsSinceSuccess={s.get('secondsSinceSuccess')}")
if problems:
    print("mirror NOT fresh: " + "; ".join(problems), file=sys.stderr)
    print("catalog=" + json.dumps(s.get("catalog")), file=sys.stderr)
    sys.exit(1)
print(f"mirror fresh: state={s.get('state')} age={age}s catalog={json.dumps(s.get('catalog'))} overlays={s.get('overlays')}")
PY
