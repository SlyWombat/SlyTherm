#!/bin/bash
# install.sh — install or upgrade the SlyTherm LAN OTA mirror on a host
# (kdocker2). Idempotent: re-run after any change in deploy/ota-mirror/ or
# tools/ota_mirror_sync.sh. Run as root:
#
#   sudo deploy/ota-mirror/install.sh              # from a repo checkout
#   sudo SYNC_SCRIPT=/path/ota_mirror_sync.sh ./install.sh   # from a copied dir
#
# What it does (each step prints what changed):
#   1. /usr/local/sbin/ota_mirror_sync.sh + ota_mirror_check.sh (0755, root)
#   2. /data/stacks/ota-mirror/{compose.yaml,nginx.conf} — existing copies that
#      differ are kept as *.bak-YYYYMMDD (house convention) before replacing
#   3. /data/stacks/ota-mirror/mirror/ (+ overlay.d/) owned by the service user
#   4. systemd: ota-mirror-sync.service, ota-mirror-freshness.{service,timer},
#      OnFailure drop-ins only if backup-alert@.service exists on this host
#   5. daemon-reload, enable --now both, restart the sync service if its
#      script or unit changed, docker compose up -d for the nginx edge
#
# One-time data migration from the old location is NOT done here — see README.
set -euo pipefail

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
STACK="${STACK:-/data/stacks/ota-mirror}"
SVC_USER="${SVC_USER:-dave}"
SYNC_SCRIPT="${SYNC_SCRIPT:-$HERE/../../tools/ota_mirror_sync.sh}"
[ -f "$SYNC_SCRIPT" ] || SYNC_SCRIPT="$HERE/ota_mirror_sync.sh"
[ -f "$SYNC_SCRIPT" ] || { echo "cannot find ota_mirror_sync.sh (set SYNC_SCRIPT=)" >&2; exit 1; }
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
id "$SVC_USER" >/dev/null 2>&1 || { echo "service user $SVC_USER does not exist" >&2; exit 1; }

changed_sync=0
# put <src> <dst> <mode> [owner] — install only when content differs; report.
put() {
  local src="$1" dst="$2" mode="$3" owner="${4:-root:root}"
  if [ -f "$dst" ] && cmp -s "$src" "$dst"; then
    echo "  = $dst (unchanged)"; return 0
  fi
  if [ -f "$dst" ]; then
    case "$dst" in
      "$STACK"/*) cp -p "$dst" "$dst.bak-$(date +%Y%m%d)"; echo "  ~ $dst (backup $dst.bak-$(date +%Y%m%d))" ;;
      *) echo "  ~ $dst (updated)" ;;
    esac
  else
    echo "  + $dst"
  fi
  install -m "$mode" -o "${owner%%:*}" -g "${owner##*:}" "$src" "$dst"
  return 1
}

echo "[1/5] scripts"
put "$SYNC_SCRIPT" /usr/local/sbin/ota_mirror_sync.sh 0755 || changed_sync=1
put "$HERE/ota_mirror_check.sh" /usr/local/sbin/ota_mirror_check.sh 0755 || true

echo "[2/5] stack files in $STACK"
install -d -m 0755 -o "$SVC_USER" -g "$SVC_USER" "$STACK"
put "$HERE/compose.yaml" "$STACK/compose.yaml" 0664 "$SVC_USER:$SVC_USER" || true
put "$HERE/nginx.conf" "$STACK/nginx.conf" 0664 "$SVC_USER:$SVC_USER" || true

echo "[3/5] mirror directory"
install -d -m 0755 -o "$SVC_USER" -g "$SVC_USER" "$STACK/mirror" "$STACK/mirror/overlay.d"
echo "  = $STACK/mirror ($(find "$STACK/mirror" -maxdepth 1 -type f | wc -l) files)"

echo "[4/5] systemd units"
put "$HERE/ota-mirror-sync.service" /etc/systemd/system/ota-mirror-sync.service 0644 || changed_sync=1
put "$HERE/ota-mirror-freshness.service" /etc/systemd/system/ota-mirror-freshness.service 0644 || true
put "$HERE/ota-mirror-freshness.timer" /etc/systemd/system/ota-mirror-freshness.timer 0644 || true
if [ -f /etc/systemd/system/backup-alert@.service ]; then
  for u in ota-mirror-sync ota-mirror-freshness; do
    install -d -m 0755 "/etc/systemd/system/$u.service.d"
    put "$HERE/$u.service.d/onfailure.conf" "/etc/systemd/system/$u.service.d/onfailure.conf" 0644 || true
  done
else
  echo "  ! backup-alert@.service not on this host: OnFailure drop-ins NOT installed (no failure paging)"
fi
# The unit hardcodes the service user; patch if this host uses another.
if [ "$SVC_USER" != dave ]; then
  sed -i "s/^User=.*/User=$SVC_USER/; s/^Group=.*/Group=$SVC_USER/" /etc/systemd/system/ota-mirror-sync.service
fi

echo "[5/5] activate"
systemctl daemon-reload
systemctl enable --now ota-mirror-freshness.timer >/dev/null
if [ "$changed_sync" = 1 ] && systemctl is-active --quiet ota-mirror-sync.service; then
  systemctl restart ota-mirror-sync.service; echo "  restarted ota-mirror-sync.service"
else
  systemctl enable --now ota-mirror-sync.service >/dev/null; echo "  ota-mirror-sync.service enabled + started"
fi
if command -v docker >/dev/null; then
  (cd "$STACK" && docker compose up -d 2>&1 | sed 's/^/  /')
fi

echo
systemctl --no-pager --lines=0 status ota-mirror-sync.service | sed -n '1,6p'
echo
echo "verify:  curl -s http://127.0.0.1:8090/status.json | head -20"
echo "         /usr/local/sbin/ota_mirror_check.sh"
echo "         journalctl -u ota-mirror-sync -f"
