# LAN OTA mirror (kdocker2 `:8090`)

The fleet updates from this mirror, not from GitHub. It exists for two hard
reasons, not convenience:

- **#182** — the P4 remotes crash (esp-hosted SDIO RX pool exhaustion) on an
  unpaced download. The firmware's 16 KB `Range` chunking targets the mirror,
  and the nginx `limit_rate` here is the only thing that keeps pre-1.4.0
  clients alive. GitHub cannot be told to pace.
- **#129** — GitHub TLS is out of the OTA path; integrity is the per-target
  ECDSA-P256 signature + sha256 in the catalog, so plain HTTP on the LAN is
  fine and a WAN outage does not block a rollout that is already mirrored.

It is also the only way to stage an **unreleased image** (bench validation,
a diagnostic build on one device — #193's `remote-p4-vpn-heapdbg` is live as
an overlay right now).

## Ownership (settled in #206, 2026-09-08)

| What | Owner | Where |
| :-- | :-- | :-- |
| Sync script, its contract, this README, unit files | **SlyTherm** (this repo) | `tools/ota_mirror_sync.sh`, `deploy/ota-mirror/` |
| Running it: the host, the units as installed, failure paging, schedule-of-record | **house IT** (`house-network-ops`) | kdocker2 `/data/stacks/ota-mirror`, `/etc/systemd/system` |
| Holds, overlays, what version the fleet is offered | **SlyTherm** | `mirror/.hold`, `mirror/overlay.d/` |

Rule that came out of #206: **nobody restarts or "fixes" the sync without
reading `status.json` first.** A stopped or held mirror can be deliberate (a
device on a diagnostic build depends on an entry GitHub does not have). Since
2.0.0 that intent is recorded in the hold file and served, so there is no
longer any need to guess.

## The two halves

```
 GitHub (raw catalog.json + release assets)
        │  every 300 s, verified by size+sha256 against the catalog
        ▼
 ota-mirror-sync.service  (systemd, Type=notify, watchdog, Restart=always)
        │  writes /data/stacks/ota-mirror/mirror/{catalog.json, *.bin, status.json}
        ▼
 ota-mirror container     (nginx, Dockge stack, restart: unless-stopped)
        │  http://192.168.10.12:8090/  — Range + limit_rate, dot-files 404
        ▼
 fleet  (GET /catalog.json, then /<basename>.app.bin in 16 KB ranges)

 ota-mirror-freshness.timer (every 15 min) → ota_mirror_check.sh → OnFailure pages
```

The container being `Up` says nothing about freshness. **`status.json` is the
health signal**; the freshness timer reads it and pages through the house
handler (`backup-alert@`) when the loop is dead, wedged, or has not synced
for an hour. The alert throttles per unit (house #135), so a long outage is
one page plus a count, not a flood.

## Contract (`tools/ota_mirror_sync.sh` 2.0.0)

One pass, every `--interval` (300 s):

1. **Hold** — if `mirror/.hold` exists, do nothing except rewrite
   `status.json` (`state: held`, reason = file contents). Never alerts.
2. **Catalog** — fetch `firmware/catalog.json` from `main` into
   `.upstream-catalog.json`; merge `overlay.d/*.json` over it (same `id`+`hwRev`
   replaces, new ids append); publish `catalog.json` atomically, only if it
   changed. No overlays → upstream bytes verbatim. Any overlay error → the
   previously published catalog stays, `lastError` says why.
3. **Assets** — every image the *published* catalog names is verified on disk
   (size + sha256). Mismatch → quarantined as `*.bad` and re-fetched from its
   `appUrl` if that is GitHub; if it is a locally staged image, reported as
   `lastError` (the mirror never invents a binary). Then the latest release's
   remaining assets (`.elf.gz`, merged `.bin`, manuals) are fetched by basename
   if absent — immutable per tag, so present == done.
4. **Status** — `status.json` rewritten every pass, held or not:

| field | meaning |
| :-- | :-- |
| `state` | `ok` · `failing` (last pass failed, within grace) · `stale` (no success for `--stale-after`, 3600 s) · `held` |
| `alert` | `true` only when `stale`. **This is the one field a monitor needs.** |
| `held`, `holdReason`, `holdSinceEpoch` | the deliberate-pause record |
| `updatedEpoch` | when this file was written — older than 15 min = loop dead |
| `lastSuccessEpoch`, `secondsSinceSuccess`, `lastError` | what went wrong, since when |
| `catalog` | `{ targetId: version }` actually being served |
| `servedCatalogSha256`, `upstreamCatalogSha256` | equal ⇔ no overlay in effect |
| `overlays` | overlay files currently merged |
| `script.version`, `script.sha256` | what is running vs. what is in git |

Under systemd the loop sends `READY` after its first pass and `WATCHDOG=1`
before every download; it **never exits on a fetch failure** (it keeps serving
and says so). Outside systemd those calls are no-ops, so it runs unchanged on
a laptop: `tools/ota_mirror_sync.sh -d /tmp/m --once`.

Subcommands (all take `-d DIR`, default `/data/stacks/ota-mirror/mirror` in the
unit; `$OTA_MIRROR_DIR` or `~/SlyTherm/ota-mirror` otherwise):

```
ota_mirror_sync.sh hold "why"     # freeze: writes .hold, status → held
ota_mirror_sync.sh release        # remove .hold, re-sync now
ota_mirror_sync.sh status         # cat status.json
ota_mirror_sync.sh --once         # one pass; exit 3 if state is failing/stale
```

## Overlays: staging an image GitHub does not have

Drop a JSON fragment in `mirror/overlay.d/` and the image file next to
`catalog.json`. The fragment is one target object, or `{"targets":[...]}`, with
every field the firmware requires (`id hwRev version appUrl appSize sha256 sig`).
`appUrl` may be the mirror's own URL — the fleet fetches by basename anyway.
**ASCII only**: the firmware's parser rejects `\uXXXX`, so the merge refuses a
non-ASCII fragment rather than break the whole catalog.

Sign exactly as `tools/release.py` does:

```sh
sha256sum X.app.bin ; stat -c %s X.app.bin
openssl dgst -sha256 -sign ~/.slytherm/ota_signing_key.pem X.app.bin | base64 -w0   # → "sig"
```

Overlays survive every sync, so a device on a diagnostic build keeps a
reachable OTA target without stopping the mirror (this is what the 2026-08-24
hand-edit + stopped daemon was doing by hand, and what the 2026-09-04 restart
silently undid). Remove the fragment when the device is back on fleet firmware.

## Runbooks

**Release day** — nothing to do. `publish.sh` tags, CI commits the catalog,
the next pass (≤ 5 min) publishes it and fetches the images; the fleet's
daily check or a `cmd/ota_check` kick picks it up. Confirm with
`curl -s http://192.168.10.12:8090/status.json | jq .catalog`.

**Bench validation of an unreleased build** — either an overlay (preferred:
single new `id`, nothing else can match it) or, for the old "single-target test
catalog" recipe, `ota_mirror_sync.sh hold "bench: #NNN"` then edit
`catalog.json` by hand and `release` when done. The hold is visible in
`status.json` and in the journal, and the freshness check treats it as healthy.

**A device stuck on a diagnostic target id** (dc25b0 today) — keep its overlay
in place. To bring it back: build the real env with `VERSION` above the
device's running version, sign, add/replace the overlay entry with that image
under the *diagnostic* id, kick `ota_check`; once it reports the fleet target
id, delete the overlay.

**Sync unit in `failed`** (ten deaths in an hour — a real bug, or the host ran
out of something): `journalctl -u ota-mirror-sync -n 100`, fix, then
`systemctl reset-failed ota-mirror-sync && systemctl start ota-mirror-sync`.
The nginx edge keeps serving the last good content throughout.

**Freshness alert fired** — `curl -s 127.0.0.1:8090/status.json`. `updatedEpoch`
old → the loop is dead: `systemctl status ota-mirror-sync`. `alert: true` with
a recent `updatedEpoch` → GitHub unreachable or the merge is failing; read
`lastError`. A `state: held` never alerts by design; if a hold is older than the
work it was for, `release` it.

**After a host rebuild** — `sudo deploy/ota-mirror/install.sh` from a checkout.
The mirror directory is data (1.4 GB, every release since 0.5.7 including
`.elf.gz` for coredump decoding); restore it from backup or let the first pass
refill the current release. Old versions are only needed to decode a coredump
from a device still running them.

## Install / upgrade

```sh
git clone https://github.com/SlyWombat/SlyTherm && cd SlyTherm   # or any checkout
sudo deploy/ota-mirror/install.sh
```

Idempotent. Installs the scripts to `/usr/local/sbin`, the stack files to
`/data/stacks/ota-mirror` (existing copies that differ are kept as
`*.bak-YYYYMMDD`), the units + house `OnFailure` drop-ins, enables both, and
`docker compose up -d`s the edge. Re-run after editing anything in this
directory or the script. The service runs as `dave` (the stack owner) with
`ProtectSystem=strict` and write access to the mirror directory only.

### One-time migration (done 2026-09-08 on kdocker2)

The mirror lived in `/home/dave/SlyTherm/ota-mirror` (a build checkout's
working tree, not a production location — house #123 pattern). It was
rsync'd to `/data/stacks/ota-mirror/mirror`, the hand-started loop killed, the
compose bind mount repointed, the units installed, and the #193 overlay
re-created from the diagnostic image that was still on disk. The old directory
was left in place for house IT to delete once the new one has run for a while.

## Monitoring hooks for house IT

- **Paging** — both units carry `OnFailure=backup-alert@%n.service` drop-ins.
- **Kuma / blackbox (optional, extra)** — HTTP JSON-query monitor on
  `http://192.168.10.12:8090/status.json`, expression `$.alert == false`, plus
  a keyword monitor that `catalog.json` contains `"schema"`. Do not alert on
  `state == "held"`.
- **Schedule-of-record (#82)** — the units are defined in this directory; the
  installed copies are byte-identical (install.sh reports otherwise).

## History

- 2026-07-15 — stale mirror incident: fresh catalog + old binary served as 200;
  the script skipped any file already present. Fixed in 2.0.0 by sha256
  verification against the catalog (#170 root cause).
- 2026-08-07 — nginx replaces `python3 -m http.server` (#182: Range + pacing).
- 2026-08-24 — daemon stopped on purpose and catalog hand-edited for #193.
- 2026-09-04 — house IT restarted the loop for v1.4.6, unknowingly wiping the
  hand edit (house-network-ops#105).
- 2026-09-08 — #206: 2.0.0 script (hold, overlays, verification, status.json),
  systemd supervision + watchdog, freshness timer, moved to `/data/stacks`.
