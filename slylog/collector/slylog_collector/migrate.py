"""One-shot historical backfill (#137).

Ingests existing ct485cap capture archives into raw_frames and re-derives the
whole event history (cycle_start/cycle_stop/demand_change/novel_command/
ct485_stats/capture_session) so day-1 history is queryable from the start.

Handles the day-1 file's mixed timestamping:
- lines are segmented into capture sessions by their '# ct485cap ...' headers
  (device millis resets at reboots; each session gets its own millis->wall map)
- unstamped lines are mapped to wall time via, in order of preference:
    1. an anchor from the .anchors sidecar whose millis falls in the session
       ("<ISO-with-offset> <millis>" per line)
    2. the first wall-stamped line in the same session
    3. the session header time aligned to the session's first line (warned)

Idempotent: raw_frames dedupes on (ts, millis, payload_hash), events on
(ts, kind, detail_hash) — safe to re-run, and the run_capture/collector
double-capture overlap at cutover collapses to single rows.

Usage (inside the collector container):
    python -m slylog_collector.migrate [--dry-run] FILE [FILE...]
"""
from __future__ import annotations

import argparse
import logging
import os
import sys
import time
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from zoneinfo import ZoneInfo

from . import config
from .ct485_decode import (CaptureStats, Frame, TelnetAssembler, _parse_iso_ms)
from .db import Db, frame_hash
from .events import EventDeriver
from .telnet_ingest import MILLIS_RE, frame_row, parse_stats_line

log = logging.getLogger("migrate")

HEADER_PREFIX = "# ct485cap "
TZ = ZoneInfo(config.LOCAL_TZ)


def load_anchors(log_path: Path) -> list[tuple[float, int]]:
    """[(epoch_ms, millis)] from the .anchors sidecar, if present."""
    sidecar = log_path.with_suffix(".anchors")
    anchors = []
    if sidecar.exists():
        for line in sidecar.read_text().splitlines():
            parts = line.split()
            if len(parts) != 2:
                continue
            try:
                dt = datetime.fromisoformat(parts[0])
                if dt.tzinfo is None:
                    dt = dt.replace(tzinfo=TZ)
                anchors.append((dt.timestamp() * 1000.0, int(parts[1])))
            except ValueError:
                log.warning("unparseable anchor line: %r", line)
    return anchors


def split_sessions(text: str) -> list[tuple[str | None, list[tuple[int, str]]]]:
    """[(header_line, [(line_no, line), ...]), ...] — one entry per capture
    session; a leading headerless chunk gets header None."""
    sessions: list[tuple[str | None, list[tuple[int, str]]]] = []
    current: list[tuple[int, str]] = []
    header: str | None = None
    started = False
    for no, line in enumerate(text.splitlines(), 1):
        if line.startswith(HEADER_PREFIX):
            if started:
                sessions.append((header, current))
            header, current, started = line, [], True
            continue
        if not started and not sessions:
            started = True  # headerless prefix
        current.append((no, line))
    sessions.append((header, current))
    return [(h, ls) for h, ls in sessions if ls or h]


def header_epoch_ms(header: str | None) -> float | None:
    if not header:
        return None
    ms = _parse_iso_ms(header.replace(" @ ", " "))
    return ms


def session_offset(lines: list[tuple[int, str]], anchors: list[tuple[float, int]],
                   header: str | None, source: str) -> float | None:
    """epoch_ms - device_millis for this session's unstamped lines."""
    millis_seen = []
    first_stamped: tuple[float, int] | None = None
    for _no, line in lines:
        m = MILLIS_RE.search(line)
        if not m:
            continue
        mil = int(m.group(1))
        millis_seen.append(mil)
        if first_stamped is None:
            iso = _parse_iso_ms(line)
            if iso is not None:
                first_stamped = (iso, mil)
    if not millis_seen:
        return None
    lo, hi = min(millis_seen), max(millis_seen)
    for epoch_ms, mil in anchors:
        if lo - 60000 <= mil <= hi + 60000:
            return epoch_ms - mil
    if first_stamped is not None:
        return first_stamped[0] - first_stamped[1]
    hdr_ms = header_epoch_ms(header)
    if hdr_ms is not None:
        log.warning("%s: session %r has no anchor/stamps — aligning first line "
                    "to the header time (approximate)", source, header)
        return hdr_ms - millis_seen[0]
    log.warning("%s: session with no timing reference — millis kept, wall time "
                "approximated as file order only", source)
    return None


def parse_file(path: Path) -> tuple[list[tuple[datetime, int, Frame, str]],
                                    list[tuple[datetime, dict]],
                                    list[tuple[datetime, dict]], CaptureStats]:
    """-> (frames [(ts, millis, frame, source)], stats_events, session_events, stats)"""
    source = path.name
    text = path.read_text(errors="replace")
    anchors = load_anchors(path)
    cap_stats = CaptureStats()
    frames_out: list[tuple[datetime, int, Frame, str]] = []
    stats_out: list[tuple[datetime, dict]] = []
    sessions_out: list[tuple[datetime, dict]] = []

    for header, lines in split_sessions(text):
        offset = session_offset(lines, anchors, header, source)

        def line_wall(line: str, millis: int | None) -> datetime | None:
            iso = _parse_iso_ms(line)
            if iso is not None:
                return datetime.fromtimestamp(iso / 1000.0, TZ)
            if offset is not None and millis is not None:
                return datetime.fromtimestamp((offset + millis) / 1000.0, TZ)
            return None

        hdr_ms = header_epoch_ms(header)
        if hdr_ms is not None:
            sessions_out.append((
                datetime.fromtimestamp(hdr_ms / 1000.0, TZ),
                {"header": header, "source": source}))

        asm = TelnetAssembler(source, cap_stats)
        millis_by_line: dict[int, int] = {}
        wall_by_line: dict[int, datetime] = {}
        session_frames: list[Frame] = []
        for no, line in lines:
            cap_stats.lines += 1
            st = parse_stats_line(line)
            mm = MILLIS_RE.search(line)
            millis = int(mm.group(1)) if mm else None
            wall = line_wall(line, millis)
            if millis is not None:
                millis_by_line[no] = millis
            if wall is not None:
                wall_by_line[no] = wall
            if st is not None and wall is not None:
                stats_out.append((wall, st[1]))
            handled, done = asm.feed(line, no)
            session_frames.extend(done)
        session_frames.extend(asm.finish())
        cap_stats.frames += len(session_frames)

        for f in session_frames:
            no = f.line_no or 0
            millis = millis_by_line.get(no, 0)
            wall = wall_by_line.get(no)
            if wall is None:
                # frame from a line we couldn't time — best effort: nearest
                # earlier timed line, else skip with a warning
                candidates = [w for n, w in wall_by_line.items() if n <= no]
                if candidates:
                    wall = max(candidates)
                else:
                    log.warning("%s line %s: frame has no derivable wall time — skipped",
                                source, no)
                    continue
            frames_out.append((wall, millis, f, source))
    return frames_out, stats_out, sessions_out, cap_stats


def main(argv: list[str] | None = None) -> int:
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(name)s %(levelname)s %(message)s")
    os.environ.setdefault("TZ", config.LOCAL_TZ)
    time.tzset()

    ap = argparse.ArgumentParser(description="SlyLog historical backfill (#137)")
    ap.add_argument("files", nargs="+", help="capture archive(s), oldest first")
    ap.add_argument("--dry-run", action="store_true",
                    help="parse + count only, no database writes")
    args = ap.parse_args(argv)

    all_frames: list[tuple[datetime, int, Frame, str]] = []
    all_stats: list[tuple[datetime, dict]] = []
    all_sessions: list[tuple[datetime, dict]] = []
    for path_s in args.files:
        path = Path(path_s)
        frames, stats_ev, sessions, cs = parse_file(path)
        log.info("%s: %d lines -> %d valid frames (%d garbage bytes), "
                 "%d stats snapshots, %d sessions",
                 path.name, cs.lines, len(frames), cs.garbage_bytes,
                 len(stats_ev), len(sessions))
        all_frames.extend(frames)
        all_stats.extend(stats_ev)
        all_sessions.extend(sessions)

    # chronological order across all inputs (event derivation needs it)
    all_frames.sort(key=lambda r: r[0])
    all_stats.sort(key=lambda r: r[0])

    # derive events over the ordered history with ONE tracker
    deriver = EventDeriver()
    events: list[tuple[datetime, str, dict]] = []
    for ts, _millis, frame, _src in all_frames:
        events.extend(deriver.feed_frame(frame, ts))
    for ts, counters in all_stats:
        events.extend(deriver.feed_stats(ts, counters))
    events.extend((ts, "capture_session", detail) for ts, detail in all_sessions)
    events.sort(key=lambda e: e[0])

    kind_counts = Counter(k for _, k, _ in events)
    log.info("derived events: %s", dict(kind_counts))
    day1 = [e for e in events if e[1] == "cycle_start"
            and e[2].get("cmd") == "COOL_DEMAND"]
    log.info("COOL_DEMAND cycle_start events: %d (first: %s, last: %s)",
             len(day1), day1[0][0] if day1 else "-", day1[-1][0] if day1 else "-")

    if args.dry_run:
        log.info("dry run: %d frames, %d events — nothing written", len(all_frames),
                 len(events))
        return 0

    db = Db()
    db.connect()
    inserted = 0
    rows = [frame_row(f, ts, millis, src) for ts, millis, f, src in all_frames]
    for i in range(0, len(rows), 1000):
        inserted += db.insert_raw_frames(rows[i:i + 1000])
    ev_inserted = 0
    for i in range(0, len(events), 1000):
        ev_inserted += db.insert_events(events[i:i + 1000])
    log.info("raw_frames: %d parsed, %d inserted (%d deduped)",
             len(rows), inserted, len(rows) - inserted)
    log.info("events: %d derived, %d inserted (%d deduped)",
             len(events), ev_inserted, len(events) - ev_inserted)
    return 0


if __name__ == "__main__":
    sys.exit(main())
