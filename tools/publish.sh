#!/usr/bin/env bash
# publish.sh — cut a SlyTherm release the way the workflow memory prescribes.
#
# What it does, in order:
#   1. preconditions  : on main, working tree clean (commit your feature work FIRST)
#   2. pick version   : arg, or auto-bump the patch of VERSION
#   3. pre-flight gate : `pio test -e native` + build EVERY release target
#                        (the same gates release.yml runs — fail HERE, before the
#                        tag, not mid-publish). Skippable with --skip-gate.
#   4. bump + commit   : write VERSION, commit "release vX.Y.Z"
#   5. pull --rebase   : land on the true main tip (CI pushes a catalog-refresh
#                        commit after each release; a tag off the real tip is refused)
#   6. tag + push      : push main + vX.Y.Z  -> CI builds, signs, publishes, and
#                        the fleet OTAs. This step prompts unless --yes.
#   7. watch CI        : gh run watch; report pass/fail.
#
# After a green run: the GitHub release + refreshed catalog are live; the fleet
# auto-updates; the LAN OTA mirror re-syncs on its own because the new version's
# asset filenames differ (so it never serves a stale binary — the 2026-07-15 trap).
#
# Usage:
#   tools/publish.sh                 # auto patch-bump (1.0.8 -> 1.0.9)
#   tools/publish.sh 1.1.0           # explicit version
#   tools/publish.sh --skip-gate     # trust CI's gate (publish is CI's LAST step,
#                                    #   so a build failure never ships a partial release)
#   tools/publish.sh --yes           # don't prompt before the push
#
# Env:
#   PIO   pio invocation for the gate (default: pio). On the WSL dev box the
#         local build is slow — set PIO to the fast build host's pio, e.g.
#         PIO="ssh kdocker2 -- cd ~/SlyTherm && ~/.pio-venv/bin/pio", or run this
#         script on the build host, or use --skip-gate.
set -euo pipefail

PIO="${PIO:-pio}"
# Keep in sync with the release targets in tools/release.py (TARGETS).
ENVS=(thermostat thermostat_s3_tx remote_p4 remote_p4_vpn)

VER="" ; GATE=1 ; ASSUME_YES=0
for a in "$@"; do
  case "$a" in
    --skip-gate) GATE=0 ;;
    --yes|-y)    ASSUME_YES=1 ;;
    -h|--help)   sed -n '2,40p' "$0"; exit 0 ;;
    -*)          echo "unknown flag: $a" >&2; exit 2 ;;
    *)           VER="$a" ;;
  esac
done

cd "$(git rev-parse --show-toplevel)"

say(){ printf '\n\033[1;36m== %s\033[0m\n' "$*"; }
die(){ printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

# 1. preconditions
[ "$(git rev-parse --abbrev-ref HEAD)" = main ] || die "not on main (checkout main and commit your feature work first)"
git diff --quiet && git diff --cached --quiet || die "working tree has uncommitted changes — commit your feature work before publishing"

# 2. version
cur="$(tr -d ' \t\n' < VERSION)"
if [ -z "$VER" ]; then
  IFS=. read -r MA MI PA <<< "$cur"
  [ -n "${PA:-}" ] || die "VERSION '$cur' is not X.Y.Z; pass an explicit version"
  VER="$MA.$MI.$((PA+1))"
fi
[[ "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "version '$VER' is not X.Y.Z"
git rev-parse "v$VER" >/dev/null 2>&1 && die "tag v$VER already exists"
say "release: $cur -> $VER"

# 3. pre-flight gate (the same checks CI will run)
if [ "$GATE" = 1 ]; then
  say "gate: native tests"
  $PIO test -e native
  for e in "${ENVS[@]}"; do
    say "gate: build $e"
    $PIO run -e "$e"
  done
else
  say "gate: SKIPPED (--skip-gate) — relying on CI; publish is CI's last step, so a build failure ships nothing"
fi

# 4. bump + commit
say "bump VERSION -> $VER and commit"
printf '%s\n' "$VER" > VERSION
git add VERSION
git commit -m "release v$VER"

# 5. land on the true main tip
say "pull --rebase origin main"
git pull --rebase origin main

# 6. tag + push (the irreversible, fleet-affecting step)
if [ "$ASSUME_YES" != 1 ]; then
  printf '\n\033[1;33mAbout to push main + v%s. CI will publish and the LIVE fleet will OTA-update. Continue? [y/N] \033[0m' "$VER"
  read -r ans; [ "$ans" = y ] || [ "$ans" = Y ] || die "aborted before push (VERSION bump commit is local; reset with: git reset --hard HEAD~1)"
fi
say "tag v$VER + push"
git tag "v$VER"
git push origin main "v$VER"

# 7. watch CI
if command -v gh >/dev/null 2>&1; then
  say "watching CI (Ctrl-C to stop watching — the release continues)"
  sleep 6
  rid="$(gh run list --limit 8 --json databaseId,headBranch,event,displayTitle \
        --jq '[.[] | select(.event=="push" and (.displayTitle|test("v'"$VER"'")))][0].databaseId' 2>/dev/null || true)"
  if [ -n "${rid:-}" ]; then
    gh run watch "$rid" --exit-status && \
      say "PUBLISHED v$VER — release + catalog live; fleet will OTA; the LAN mirror re-syncs (new filenames)." || \
      die "CI FAILED. Publish is CI's last step, so nothing shipped. Fix, then: git tag -d v$VER && git push origin :v$VER && re-run."
  else
    say "pushed v$VER — couldn't auto-find the run; check: gh run list"
  fi
else
  say "pushed v$VER — install gh to auto-watch, or check the Actions tab."
fi
