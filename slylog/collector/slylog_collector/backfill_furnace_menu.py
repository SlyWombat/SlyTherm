"""Decode furnace menu pages (0xC1) already sitting in raw_frames (#221).

The collector stored these replies in raw_frames before it knew how to decode
them (the 2026-10-18 12:30 browsing session left 40). This replays them
through the same decoder the live path uses. Idempotent: both target tables
dedupe on (ts, page[, seq]).

    python -m slylog_collector.backfill_furnace_menu [--dry-run] [--since ISO]

Needs db/init/006_furnace_menu.sql applied first.
"""
from __future__ import annotations

import argparse
import logging

from .db import Db
from .furnace_menu import MSG_USER_MENU_REPLY, decode_menu_reply

log = logging.getLogger("backfill_furnace_menu")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--since", default="2000-01-01")
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(message)s")

    db = Db()
    db.connect()
    with db._lock, db._conn.cursor() as cur:
        cur.execute("SELECT ts, payload FROM raw_frames WHERE msg_type = %s"
                    " AND ts >= %s ORDER BY ts", (MSG_USER_MENU_REPLY, args.since))
        rows = cur.fetchall()
    pages = rows_n = 0
    for ts, payload in rows:
        page = decode_menu_reply(bytes(payload))
        if page is None:
            continue                     # e.g. a 17-byte session frame
        pages += 1
        rows_n += len(page.rows)
        if not args.dry_run:
            db.insert_furnace_menu(ts, page)
    log.info("%d 0xC1 frames, %d menu pages, %d rows%s", len(rows), pages,
             rows_n, " (dry run)" if args.dry_run else "")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
