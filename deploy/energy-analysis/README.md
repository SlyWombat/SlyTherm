# energy-analysis (kdocker2 Dockge stack)

Monthly grid-energy report. Reads the `energy-weather-log` SQLite DB (read-only),
computes a monthly rollup with **year-over-year** comparison and a
**temperature-vs-energy** fit, asks a local **Ollama** model to write a
plain-English report, and **emails** it through the house mail service
(`house-mail`, kdocker2 `:3510`). Pure Python stdlib, no pip dependencies.

## Ownership (#218)

| What | Owner | Where |
| :-- | :-- | :-- |
| `analyze.py`, `Dockerfile`, `compose.yaml`, this README | **SlyTherm** (this repo) | `deploy/energy-analysis/` — the canonical copy |
| Running it: the host, the stack as installed, the key file | **house IT** (`house-network-ops`) | kdocker2 `/data/stacks/energy-analysis` |
| The `energy-analysis` house-mail client and its grant | **house IT** (`house-mail`, private ops repo) | kdocker2 `/data/house-mail/clients.yaml` |

Deploy = copy these four files over the stack directory and `docker compose up -d --build`.
Edit here first, then copy; a live edit on kdocker2 is drift (the 2026-09-04
ota-mirror incident in `deploy/ota-mirror/README.md` is why this rule exists).

## What it sends

Once a month (1st at `RUN_AT`, summarizing the month that just ended): an email
"Energy report — <Month Year>" with a 150-250 word narrative. If Ollama is
unavailable the email still goes out with the plain figures and a note.

## Mail: house-mail, not the old bridge

Until 2026-10 the job posted to the kdocker1 mail-bridge (`:3501/send`). That
bridge is retired, and it failed silently for weeks before anyone noticed —
the September 2026 report (2026-10-01 06:01) went to the container log and
nowhere else. The job now:

- POSTs `{title, message, target: MAIL_TO}` to `HOUSE_MAIL_URL/send` with
  `Authorization: Bearer <key>`;
- reads the key from `HOUSE_MAIL_KEY_FILE` **at send time** (rotation needs no
  restart). The key is never an environment value, never on a command line,
  never logged;
- **fails loudly.** At daemon start it calls `/v1/whoami` and exits 2 unless the
  key holds `mail.send`, so a bad key is a restart loop today instead of a
  missing report on the 1st. A failed send exits 1 after printing the report
  body to the log. `--once` / `--month` exit non-zero on a failed send.

### Mail key

The key is a house-mail **service** client named `energy-analysis` with
`mail.send` on the owner mailbox and `default_to` = `MAIL_TO`. Issued on
kdocker2 by `house-mail-admin` (see `/data/stacks/house-mail/README.md`); the
admin writes the key to a file and never prints it:

```sh
cd /data/stacks/house-mail
docker compose exec house-mail house-mail-admin add-client energy-analysis \
  --kind service --grant owner:dave@drscapital.com:mail.send \
  --default-to dave@drscapital.com --key-out /data/out/energy-analysis.key
# the container's /data is the host's /data/house-mail
install -m 0600 -o dave -g dave /data/house-mail/out/energy-analysis.key \
  /data/stacks/energy-analysis/house-mail.key
shred -u /data/house-mail/out/energy-analysis.key
```

`compose.yaml` bind-mounts `./house-mail.key` read-only at
`/run/secrets/house-mail.key`. The file is git-ignored here and must stay 0600
on the host. Rotate with `house-mail-admin rotate-key energy-analysis --key-out ...`
and re-install the file; no container restart is needed.

### Verify

```sh
cd /data/stacks/energy-analysis
docker compose run --rm energy-analysis --check-mail      # key + grant, no mail
docker compose run --rm energy-analysis --month 2026-09   # sends the real report
docker compose logs --tail 5 energy-analysis
```

The second command sends a real email to `MAIL_TO`. First verified
2026-10-18 (September 2026 report delivered through house-mail).

## Config (compose env)

| var | default | meaning |
|---|---|---|
| `MODEL` | `qwen2.5:72b-instruct-q4_K_M` | any pulled Ollama model (`gemma4:31b` is lighter) |
| `OLLAMA_KEEP_ALIVE` | `0` | unload model after each run → frees VRAM for Immich ML |
| `HOUSE_MAIL_URL` | `http://192.168.10.11:3510` | house-mail base URL (LAN only) |
| `HOUSE_MAIL_KEY_FILE` | `/run/secrets/house-mail.key` | container path of the mounted key |
| `MAIL_TO` | `dave@drscapital.com` | recipient; also the key's `default_to` |
| `RUN_AT` | `06:00` | local time on the 1st of the month |
| `RUN_ON_START` | `false` | set `true` to fire immediately on container start |

## Run manually

```bash
docker compose run --rm energy-analysis --once            # most recent complete month
docker compose run --rm energy-analysis --month 2025-06   # a specific month
```

## Notes

- Temps are only present from **2025-06** (Ecowitt sensor added then); the
  temperature-vs-energy fit and daily anomaly flags use the months/days that
  have temperature. YoY energy comparison works back to **2024-11**.
- Depends on `energy-weather-log` for the DB at `../energy-weather-log/data/daily.db`,
  on Ollama (same host) and on house-mail being up.
- Ollama answered HTTP 500 on 2026-10-01; the figures-only fallback covered it.
  Check that `MODEL` is still pulled if that repeats.
