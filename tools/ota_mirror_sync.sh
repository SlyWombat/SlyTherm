#!/bin/bash
# ota_mirror_sync.sh — maintain a LAN OTA mirror of the latest SlyTherm
# release (#129: takes GitHub TLS out of the fleet's OTA path; integrity is
# carried by the catalog signature + sha256, not the transport).
#
# Syncs catalog.json (raw main) + the latest release's assets into MIRROR_DIR,
# then loops. Serve the directory with any static HTTP server, e.g.:
#   cd "$MIRROR_DIR" && python3 -m http.server 8090 --bind 0.0.0.0
# Point the fleet at it:  slytherm/cmd/ota_mirror = http://<host>:8090
#
# Usage: ota_mirror_sync.sh [mirror_dir] [interval_s]
set -u

REPO="SlyWombat/SlyTherm"
MIRROR_DIR="${1:-$HOME/SlyTherm/ota-mirror}"
INTERVAL="${2:-300}"

mkdir -p "$MIRROR_DIR"
cd "$MIRROR_DIR" || exit 1

while true; do
  # Catalog first: it names the versions the fleet will ask for.
  if curl -fsS -o catalog.json.tmp \
       "https://raw.githubusercontent.com/$REPO/main/firmware/catalog.json"; then
    mv catalog.json.tmp catalog.json
  else
    rm -f catalog.json.tmp
    echo "$(date -Is) catalog fetch failed; keeping previous" >&2
  fi

  # Latest release assets, flat by basename (the firmware's mirror URL scheme).
  # Only download what we don't already have (assets are immutable per tag).
  curl -fsS "https://api.github.com/repos/$REPO/releases/latest" |
    grep -o '"browser_download_url": *"[^"]*"' |
    sed 's/.*"\(https[^"]*\)"/\1/' |
    while read -r url; do
      f="${url##*/}"
      [ -s "$f" ] && continue
      echo "$(date -Is) fetching $f" >&2
      curl -fsSL -o "$f.tmp" "$url" && mv "$f.tmp" "$f" || rm -f "$f.tmp"
    done

  sleep "$INTERVAL"
done
