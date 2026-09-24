"""Fill raw_frames' #204 header columns for rows written before they existed.

The collector parsed the full CT-485 RX header from the 2026-09-04 12:44 EDT
firmware cutover onward, but raw_frames had no columns for it, so the bytes
reached only the flat-file archive. This replays that archive through the live
ingest path (TelnetIngest.handle_line with a recording stub Db), so every row
comes out with exactly the (ts, millis, payload_hash) key the live collector
wrote. Then it UPDATEs the matching rows in bulk.

    python -m slylog_collector.backfill_header [--dry-run] \\
        [--since 2026-09-04T12:44:13] /captures-vol/ct485-live.log

Only rows whose packet_num IS NULL are touched, so a re-run is a no-op and
rows written by the header-aware collector are never overwritten. Frames
before --since carry no header and are skipped: those bytes were never
captured, and nothing can backfill them.

Needs db/init/005_raw_frames_header.sql applied first.
"""
from __future__ import annotations

import argparse
import logging
import os
import sys
import time
from datetime import datetime
from zoneinfo import ZoneInfo

from . import config
from .ct485_decode import CaptureStats, TelnetAssembler, _parse_iso_ms
from .db import Db
from .telnet_ingest import TelnetIngest

log = logging.getLogger("backfill_header")

CUTOVER = "2026-09-04T12:44:13"  # first full-header RX line (SlyTherm #204)
BATCH = 50_000


class _RecordingDb:
    """Stands in for Db inside TelnetIngest: keeps header-bearing frame rows,
    drops everything else (events, shadow, gaps are already in the DB)."""

    def __init__(self):
        self.rows: list[tuple] = []

    def insert_raw_frames(self, rows: list[tuple]) -> int:
        for r in rows:
            # frame_row layout: ts, millis, ..., payload_hash at [6], header at [11:16]
            if r[15] is not None:
                self.rows.append((r[0], r[1], r[6]) + tuple(r[11:16]))
        return len(rows)

    def insert_events(self, rows) -> int:
        return 0

    def insert_event(self, *a, **k) -> int:
        return 0

    def insert_shadow(self, *a, **k) -> int:
        return 0


class _NullDeriver:
    def feed_frame(self, frame, ts):
        return []

    def feed_stats(self, ts, counters):
        return []


def _apply(conn, rows: list[tuple]) -> tuple[int, int]:
    """COPY one batch into a temp table and UPDATE the matching raw_frames rows.
    -> (rows sent, rows updated)"""
    with conn.cursor() as cur:
        cur.execute(
            "CREATE TEMP TABLE IF NOT EXISTS hdr_fill ("
            " ts timestamptz, millis bigint, payload_hash text,"
            " subnet smallint, send_method smallint, send_param_hi smallint,"
            " src_node_type smallint, packet_num smallint) ON COMMIT DELETE ROWS")
        with cur.copy("COPY hdr_fill FROM STDIN") as cp:
            for r in rows:
                cp.write_row(r)
        cur.execute(
            "UPDATE raw_frames r SET subnet = h.subnet, send_method = h.send_method,"
            " send_param_hi = h.send_param_hi, src_node_type = h.src_node_type,"
            " packet_num = h.packet_num"
            " FROM hdr_fill h"
            " WHERE r.ts = h.ts AND r.millis = h.millis"
            " AND r.payload_hash = h.payload_hash AND r.packet_num IS NULL")
        updated = cur.rowcount
    conn.commit()
    return len(rows), updated


def main(argv: list[str] | None = None) -> int:
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(name)s %(levelname)s %(message)s")
    os.environ.setdefault("TZ", config.LOCAL_TZ)
    time.tzset()

    ap = argparse.ArgumentParser(description="Backfill raw_frames header columns (#204)")
    ap.add_argument("archive", help="the collector's flat-file archive (ct485-live.log)")
    ap.add_argument("--since", default=CUTOVER,
                    help=f"skip lines stamped before this local time (default {CUTOVER})")
    ap.add_argument("--dry-run", action="store_true",
                    help="parse and count only, no database writes")
    args = ap.parse_args(argv)

    tz = ZoneInfo(config.LOCAL_TZ)
    since_ms = datetime.fromisoformat(args.since).replace(tzinfo=tz).timestamp() * 1000.0

    rec = _RecordingDb()
    ing = TelnetIngest(rec, deriver=_NullDeriver(), archive_path=os.devnull)
    ing._archive = None  # never write: this is a replay
    ing._asm = TelnetAssembler("backfill", CaptureStats())

    conn = None
    if not args.dry_run:
        import psycopg
        conn = psycopg.connect()  # PG* env vars, same as the collector

    started = False
    lines = sent = updated = 0

    def flush_batch(final: bool = False) -> None:
        nonlocal sent, updated
        if final:
            ing.flush()
        if not rec.rows:
            return
        if conn is None:
            sent += len(rec.rows)
        else:
            s, u = _apply(conn, rec.rows)
            sent += s
            updated += u
        rec.rows = []
        log.info("%d lines read, %d header rows sent, %d rows updated", lines, sent, updated)

    with open(args.archive, errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\r\n")
            if line.startswith("# ct485cap "):
                # the live collector starts a fresh assembler per telnet session
                ing.flush()
                ing._asm = TelnetAssembler("backfill", CaptureStats())
                continue
            if not started:
                ms = _parse_iso_ms(line)
                if ms is None or ms < since_ms:
                    continue
                started = True
                log.info("starting at: %s", line[:80])
            lines += 1
            ing.handle_line(line)
            # The newest frame may still grow by [ct485+] continuations, and
            # TelnetIngest holds it as _pending until the next summary line, so
            # only rows already handed to the Db are final and safe to send.
            if len(rec.rows) >= BATCH:
                flush_batch()
    flush_batch(final=True)

    if conn is not None:
        conn.close()
    if args.dry_run:
        log.info("dry run: %d lines, %d header rows parsed; nothing written", lines, sent)
    else:
        log.info("done: %d header rows sent, %d raw_frames rows updated (%d unmatched)",
                 sent, updated, sent - updated)
    return 0


if __name__ == "__main__":
    sys.exit(main())
