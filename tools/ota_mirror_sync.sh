#!/bin/bash
# ota_mirror_sync.sh — populate the LAN OTA mirror for the SlyTherm fleet.
#
# The mirror takes GitHub TLS out of the fleet's OTA path (#129) and, for the
# P4 remotes, is the ONLY safe download path (#182: nginx Range + pacing keep
# the esp-hosted SDIO RX pool alive). Integrity is carried by the per-target
# ECDSA signature + sha256 in the catalog, not by the transport.
#
# What one sync pass does (SlyTherm #206 — supervisable, self-reporting):
#   1. HOLD:    if <dir>/.hold exists, do nothing except refresh status.json.
#               (bench staging / diagnostic builds — see deploy/ota-mirror/README.md)
#   2. Catalog: fetch firmware/catalog.json from the repo's main branch into a
#               cache, merge any <dir>/overlay.d/*.json fragments over it (an
#               overlay target with the same id REPLACES the upstream one;
#               new ids are appended), and atomically publish <dir>/catalog.json.
#               With no overlays the upstream bytes are published verbatim.
#   3. Assets:  every image the published catalog names is verified on disk
#               (size + sha256) and (re)fetched from its appUrl on mismatch —
#               the 2026-07-15 "fresh catalog, stale binary" incident (#170).
#               Then the latest release's other assets (elf.gz, merged .bin,
#               manuals) are fetched by basename if absent; those are immutable
#               per tag so present == done.
#   4. Status:  <dir>/status.json is rewritten every pass (also while held or
#               failing). Alert on  .alert == true  — that is "no successful
#               sync for --stale-after seconds and not held".
#
# Under systemd (Type=notify) the loop sends READY/WATCHDOG/STATUS; outside
# systemd those calls are no-ops. It never exits on a fetch failure: it keeps
# serving what it has and says so in status.json. It exits non-zero only when
# the mirror directory is unusable.
#
# Usage:
#   ota_mirror_sync.sh [options]              run the sync loop
#   ota_mirror_sync.sh [options] --once       one pass, then exit
#   ota_mirror_sync.sh [options] hold [why]   create the hold file (and re-sync status)
#   ota_mirror_sync.sh [options] release      remove the hold file
#   ota_mirror_sync.sh [options] status       print status.json
# Options:
#   -d, --dir DIR           mirror directory   (default: $OTA_MIRROR_DIR or ~/SlyTherm/ota-mirror)
#   -i, --interval SECS     loop period        (default: 300)
#       --stale-after SECS  alert threshold    (default: 3600)
#   -r, --repo OWNER/NAME   GitHub repo        (default: SlyWombat/SlyTherm)
#   -b, --branch NAME       catalog branch     (default: main)
# Legacy positional form  ota_mirror_sync.sh [DIR] [INTERVAL]  still works.
set -u

SCRIPT_VERSION="2.0.0"

REPO="${OTA_MIRROR_REPO:-SlyWombat/SlyTherm}"
BRANCH="${OTA_MIRROR_BRANCH:-main}"
MIRROR_DIR="${OTA_MIRROR_DIR:-$HOME/SlyTherm/ota-mirror}"
INTERVAL="${OTA_MIRROR_INTERVAL:-300}"
STALE_AFTER="${OTA_MIRROR_STALE_AFTER:-3600}"
ONCE=0
CMD="loop"
CMD_ARG=""

while [ $# -gt 0 ]; do
  case "$1" in
    -d|--dir)          MIRROR_DIR="$2"; shift 2 ;;
    -i|--interval)     INTERVAL="$2"; shift 2 ;;
    --stale-after)     STALE_AFTER="$2"; shift 2 ;;
    -r|--repo)         REPO="$2"; shift 2 ;;
    -b|--branch)       BRANCH="$2"; shift 2 ;;
    -1|--once)         ONCE=1; shift ;;
    hold)              CMD="hold"; shift
                       if [ $# -gt 0 ] && [[ "$1" != -* ]]; then CMD_ARG="$1"; shift; fi ;;
    release|status)    CMD="$1"; shift ;;
    -h|--help)         sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*)                echo "unknown option: $1" >&2; exit 2 ;;
    *)                 # legacy positional: DIR [INTERVAL]
                       MIRROR_DIR="$1"; shift
                       if [ $# -gt 0 ] && [[ "$1" =~ ^[0-9]+$ ]]; then INTERVAL="$1"; shift; fi ;;
  esac
done

CATALOG_URL="https://raw.githubusercontent.com/$REPO/$BRANCH/firmware/catalog.json"
RELEASES_URL="https://api.github.com/repos/$REPO/releases/latest"
UPSTREAM_CACHE=".upstream-catalog.json"   # dot-files are 404 at the nginx edge
HOLD_FILE=".hold"
OVERLAY_DIR="overlay.d"
STATUS_FILE="status.json"
CURL_META=(--fail --silent --show-error --location --max-time 60 --retry 2 --retry-delay 5)
CURL_ASSET=(--fail --silent --show-error --location --max-time 900 --retry 2 --retry-delay 10)

log() { echo "$(date -Is) $*" >&2; }
notify() { [ -n "${NOTIFY_SOCKET:-}" ] && systemd-notify "$@" 2>/dev/null; return 0; }

SELF_PATH=$(readlink -f "$0" 2>/dev/null || echo "$0")
mkdir -p "$MIRROR_DIR" "$MIRROR_DIR/$OVERLAY_DIR" 2>/dev/null
cd "$MIRROR_DIR" || { echo "mirror dir unusable: $MIRROR_DIR" >&2; exit 1; }
[ -w . ] || { echo "mirror dir not writable: $MIRROR_DIR" >&2; exit 1; }

# ---- per-pass bookkeeping (persisted in status.json across restarts) ----
LAST_SUCCESS=0
LAST_ERROR=""
UPSTREAM_SHA=""
if [ -s "$STATUS_FILE" ]; then
  LAST_SUCCESS=$(python3 -c 'import json,sys;print(int(json.load(open(sys.argv[1])).get("lastSuccessEpoch") or 0))' "$STATUS_FILE" 2>/dev/null || echo 0)
fi
SELF_SHA=$(sha256sum "$SELF_PATH" 2>/dev/null | cut -c1-64)

# write_status <attempt-ok 0|1>
write_status() {
  local ok="$1" now held=false hold_reason="" hold_since=0 state alert
  now=$(date +%s)
  if [ -e "$HOLD_FILE" ]; then
    held=true; hold_reason=$(head -c 200 "$HOLD_FILE" 2>/dev/null | tr -d '\r\n"\\')
    hold_since=$(stat -c %Y "$HOLD_FILE" 2>/dev/null || echo 0)
  fi
  if [ "$held" = true ]; then state="held"
  elif [ "$ok" = 1 ]; then state="ok"
  elif [ $((now - LAST_SUCCESS)) -gt "$STALE_AFTER" ]; then state="stale"
  else state="failing"; fi
  alert=false; [ "$state" = stale ] && alert=true
  STATE_NOW="$state"
  OTA_STATUS_JSON="$(python3 - "$STATUS_FILE.tmp" "$state" "$alert" "$held" "$hold_reason" "$hold_since" \
      "$now" "$LAST_SUCCESS" "$LAST_ERROR" "$UPSTREAM_SHA" "$SCRIPT_VERSION" "$SELF_SHA" "$STALE_AFTER" "$INTERVAL" <<'PY'
import json, os, sys, socket, glob, hashlib
(out, state, alert, held, hold_reason, hold_since, now, last_ok, last_err,
 up_sha, ver, self_sha, stale_after, interval) = sys.argv[1:]
cat = {}
try:
    for t in json.load(open("catalog.json")).get("targets", []):
        cat[t.get("id", "?")] = t.get("version", "?")
except Exception:
    pass
served_sha = ""
try:
    served_sha = hashlib.sha256(open("catalog.json", "rb").read()).hexdigest()
except Exception:
    pass
overlays = sorted(os.path.basename(p) for p in glob.glob("overlay.d/*.json"))
doc = {
    "schema": 1,
    "state": state,                         # ok | failing | stale | held
    "alert": alert == "true",               # the one field a monitor needs
    "held": held == "true",
    "holdReason": hold_reason,
    "holdSinceEpoch": int(hold_since) if held == "true" else None,
    "updatedEpoch": int(now),
    "lastAttemptEpoch": int(now),
    "lastSuccessEpoch": int(last_ok) or None,
    "secondsSinceSuccess": (int(now) - int(last_ok)) if int(last_ok) else None,
    "staleAfterSeconds": int(stale_after),
    "intervalSeconds": int(interval),
    "lastError": last_err or None,
    "catalog": cat,
    "servedCatalogSha256": served_sha,
    "upstreamCatalogSha256": up_sha or None,
    "overlays": overlays,
    "script": {"version": ver, "sha256": self_sha},
    "host": socket.gethostname(),
    "pid": os.getppid(),
}
with open(out, "w") as f:
    json.dump(doc, f, indent=2)
    f.write("\n")
print(f"state={state} alert={alert} catalog={cat} overlays={overlays}")
PY
  )" || { log "status: writer failed"; return 1; }
  mv -f "$STATUS_FILE.tmp" "$STATUS_FILE"
  notify --status="$OTA_STATUS_JSON"
}

# merge_catalog: cache + overlay.d/*.json -> catalog.json.tmp ; prints
# "id<TAB>version<TAB>basename<TAB>size<TAB>sha256<TAB>url" per target.
merge_catalog() {
  python3 - "$UPSTREAM_CACHE" "$OVERLAY_DIR" "catalog.json.tmp" <<'PY'
import glob, json, os, sys
src, ovdir, out = sys.argv[1:]
REQ = ("id", "hwRev", "version", "appUrl", "appSize", "sha256", "sig")

def ascii_only(o):
    # The firmware's hand-rolled parser accepts only \" \\ \/ escapes; a \uXXXX
    # from any non-ASCII character rejects the WHOLE catalog. Refuse up front.
    if isinstance(o, str):
        if any(ord(c) > 126 or ord(c) < 32 for c in o):
            sys.exit("non-ASCII/control char in %r" % o)
    elif isinstance(o, dict):
        for k, v in o.items(): ascii_only(k); ascii_only(v)
    elif isinstance(o, list):
        for v in o: ascii_only(v)

raw = open(src, "rb").read()
cat = json.loads(raw)
if cat.get("schema") != 1 or not isinstance(cat.get("targets"), list):
    sys.exit("upstream catalog: unexpected shape")
targets = [dict(t) for t in cat["targets"]]
overlays = sorted(glob.glob(os.path.join(ovdir, "*.json")))
for path in overlays:
    try:
        frag = json.load(open(path))
    except Exception as e:
        sys.exit("overlay %s: invalid JSON: %s" % (path, e))
    items = frag["targets"] if isinstance(frag, dict) and "targets" in frag else [frag]
    for t in items:
        missing = [k for k in REQ if k not in t]
        if missing:
            sys.exit("overlay %s: target missing %s" % (path, ",".join(missing)))
        try:
            ascii_only(t)
        except SystemExit as e:
            sys.exit("overlay %s: %s" % (path, e))
        targets = [x for x in targets if not (x.get("id") == t["id"] and x.get("hwRev") == t["hwRev"])]
        targets.append(t)
with open(out, "wb") as f:
    if overlays:
        cat["targets"] = targets
        f.write(json.dumps(cat, indent=2, ensure_ascii=True).encode("ascii") + b"\n")
    else:
        f.write(raw)  # byte-identical to upstream
for t in targets:
    print("\t".join(str(x) for x in (t.get("id"), t.get("version"), t["appUrl"].rsplit("/", 1)[-1],
                                     t.get("appSize"), t.get("sha256"), t["appUrl"])))
PY
}

# verify <file> <size> <sha256> -> 0 if the file on disk matches
verify() {
  local f="$1" size="$2" sha="$3" have
  [ -s "$f" ] || return 1
  [ "$(stat -c %s "$f")" = "$size" ] || return 1
  have=$(sha256sum "$f" | cut -c1-64)
  [ "$have" = "$sha" ]
}

# fetch <url> <file> [size] [sha256] -> atomic, verified when a digest is known
fetch() {
  local url="$1" f="$2" size="${3:-}" sha="${4:-}"
  notify WATCHDOG=1
  log "fetching $f"
  if ! curl "${CURL_ASSET[@]}" -o "$f.tmp" "$url"; then
    rm -f "$f.tmp"; log "fetch failed: $f"; return 1
  fi
  if [ -n "$sha" ] && ! verify "$f.tmp" "$size" "$sha"; then
    rm -f "$f.tmp"; log "fetch REJECTED (size/sha256 mismatch vs catalog): $f"; return 1
  fi
  mv -f "$f.tmp" "$f"
}

sync_once() {
  local ok=1 entries
  LAST_ERROR=""
  notify WATCHDOG=1

  if [ -e "$HOLD_FILE" ]; then
    write_status 0
    return 0
  fi

  # 1. upstream catalog -> cache (never leaves a partial file behind)
  if curl "${CURL_META[@]}" -o "$UPSTREAM_CACHE.tmp" "$CATALOG_URL"; then
    mv -f "$UPSTREAM_CACHE.tmp" "$UPSTREAM_CACHE"
  else
    rm -f "$UPSTREAM_CACHE.tmp"
    LAST_ERROR="catalog fetch failed"
    log "$LAST_ERROR; serving previous"
    write_status 0
    return 0
  fi
  UPSTREAM_SHA=$(sha256sum "$UPSTREAM_CACHE" | cut -c1-64)

  # 2. merge overlays, publish atomically when changed
  if ! entries=$(merge_catalog 2>"$MIRROR_DIR/.merge.err"); then
    LAST_ERROR="catalog merge failed: $(head -c 300 .merge.err | tr -d '\n')"
    rm -f catalog.json.tmp .merge.err
    log "$LAST_ERROR; serving previous"
    write_status 0
    return 0
  fi
  rm -f .merge.err
  if ! cmp -s catalog.json.tmp catalog.json 2>/dev/null; then
    mv -f catalog.json.tmp catalog.json
    log "catalog published: $(echo "$entries" | awk -F'\t' '{printf "%s=%s ", $1, $2}')"
  else
    rm -f catalog.json.tmp
  fi

  # 3a. every image the published catalog names: verified on disk or (re)fetched
  while IFS=$'\t' read -r id ver base size sha url; do
    [ -n "$base" ] || continue
    if verify "$base" "$size" "$sha"; then continue; fi
    if [ -s "$base" ]; then
      log "MISMATCH on disk vs catalog ($id $ver): $base — quarantined as $base.bad"
      mv -f "$base" "$base.bad" 2>/dev/null
    fi
    case "$url" in
      https://github.com/*|https://objects.githubusercontent.com/*)
        fetch "$url" "$base" "$size" "$sha" || ok=0 ;;
      *)
        # An overlay pointing at a local/staged file we don't have: report, don't guess.
        log "missing/invalid staged image for $id $ver: $base (url $url)"
        LAST_ERROR="staged image missing or invalid: $base"; ok=0 ;;
    esac
  done <<< "$entries"

  # 3b. the rest of the latest release, by basename, immutable per tag
  local urls
  if urls=$(curl "${CURL_META[@]}" "$RELEASES_URL" |
            python3 -c 'import json,sys;[print(a["browser_download_url"]) for a in json.load(sys.stdin).get("assets",[])]' 2>/dev/null); then
    while read -r url; do
      [ -n "$url" ] || continue
      f="${url##*/}"
      [ -s "$f" ] && continue
      fetch "$url" "$f" || ok=0
    done <<< "$urls"
  else
    log "release asset listing failed; catalog images already verified above"
  fi

  if [ "$ok" = 1 ]; then
    LAST_SUCCESS=$(date +%s)
  else
    [ -n "$LAST_ERROR" ] || LAST_ERROR="one or more asset fetches failed"
  fi
  write_status "$ok"
}

case "$CMD" in
  hold)
    printf '%s\n' "${CMD_ARG:-held by $(id -un)@$(hostname) $(date -Is)}" > "$HOLD_FILE"
    log "HOLD set: $(cat "$HOLD_FILE")"
    write_status 0; exit 0 ;;
  release)
    rm -f "$HOLD_FILE"; log "hold released — next pass re-syncs from upstream"
    ONCE=1 ;;
  status)
    cat "$STATUS_FILE" 2>/dev/null || { echo "no status yet" >&2; exit 1; }; exit 0 ;;
esac

log "ota_mirror_sync $SCRIPT_VERSION: dir=$MIRROR_DIR repo=$REPO branch=$BRANCH interval=${INTERVAL}s stale-after=${STALE_AFTER}s"
first=1
while true; do
  sync_once
  if [ "$first" = 1 ]; then notify --ready; first=0; fi
  [ "$ONCE" = 1 ] && break
  notify WATCHDOG=1
  sleep "$INTERVAL" &
  wait $!
done
[ "${STATE_NOW:-ok}" = ok ] || [ "${STATE_NOW:-}" = held ] || [ "$ONCE" = 0 ] || exit 3
exit 0
