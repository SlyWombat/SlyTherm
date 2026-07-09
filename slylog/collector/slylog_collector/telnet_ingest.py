"""CT-485 telnet-mirror ingest (#133).

Single client of the controller's telnet log (:23, max 2 clients total).
Replaces captures/run_capture.sh: parses the [ct485]/[ct485+]/[ct485-rej]/
[ct485-stats] line families with the proven TelnetAssembler from
tools/ct485_decode.py, writes validated frames to raw_frames, derives events,
and keeps appending the flat-file archive in the exact ct485cap.py format
(so existing analysis tooling keeps working on it).

Semantics preserved from ct485cap.py: 120 s idle = dead link -> reconnect.
"""
from __future__ import annotations

import asyncio
import logging
import re
import time
from datetime import datetime
from zoneinfo import ZoneInfo

from . import config
from .ct485_decode import CaptureStats, Frame, TelnetAssembler, _parse_iso_ms
from .db import Db, frame_hash
from .events import EventDeriver

log = logging.getLogger("telnet")

MILLIS_RE = re.compile(r"\[ct485(?:\+|-rej)?\]\s+(\d+)")
STATS_LINE_RE = re.compile(
    r"\[ct485-stats\]\s+(\d+)\s+rx=(\d+)\s+lvl=(\d+)\s+ok=(\d+)"
    r"\s+badLen=(\d+)\s+badCk=(\d+)\s+over=(\d+)")
# Shadow control (#139, fw >= 0.5.8): the controller's would-be DemandSet,
# emitted on change + 60 s heartbeat. TX stays disabled — telemetry only.
SHADOW_LINE_RE = re.compile(
    r"\[shadow\]\s+(\d+)\s+gas=([\d.]+)\s+hp=([\d.]+)\s+cool=([\d.]+)"
    r"\s+fan=([\d.]+)\s+dfr=([\d.]+)\s+T=(-?[\d.]+|nan)\s+setH=(-?[\d.]+)"
    r"\s+setC=(-?[\d.]+)\s+mode=(\d+)\s+action=(\S+)")


def parse_shadow_line(line: str) -> dict | None:
    m = SHADOW_LINE_RE.search(line)
    if not m:
        return None
    g = m.groups()
    fused = None if g[6] == "nan" else float(g[6])
    return {"millis": int(g[0]), "gas_pct": float(g[1]), "hp_pct": float(g[2]),
            "cool_pct": float(g[3]), "fan_pct": float(g[4]),
            "defrost_pct": float(g[5]), "fused_temp_c": fused,
            "set_heat_c": float(g[7]), "set_cool_c": float(g[8]),
            "mode": int(g[9]), "action": g[10]}


def parse_stats_line(line: str) -> tuple[int, dict] | None:
    m = STATS_LINE_RE.search(line)
    if not m:
        return None
    millis, rx, lvl, ok, bad_len, bad_ck, over = (int(g) for g in m.groups())
    return millis, {"millis": millis, "rx": rx, "lvl": lvl, "ok": ok,
                    "badLen": bad_len, "badCk": bad_ck, "over": over}


def frame_row(frame: Frame, ts: datetime, millis: int, source: str) -> tuple:
    return (ts, millis, frame.src, frame.dst, frame.msg_type,
            bytes(frame.payload), frame_hash(bytes(frame.raw)),
            True, frame.synthesized, frame.truncated, source)


class TelnetIngest:
    def __init__(self, db: Db, deriver: EventDeriver | None = None,
                 archive_path: str | None = None, source: str = "live"):
        self.db = db
        self.deriver = deriver or EventDeriver()
        self.archive_path = archive_path or config.ARCHIVE_PATH
        self.source = source
        self.tz = ZoneInfo(config.LOCAL_TZ)
        self._archive = None
        # pending summary frame: may still be extended by [ct485+] lines
        self._pending: Frame | None = None
        self._pending_ts: datetime | None = None
        self._pending_millis: int = 0
        self._asm: TelnetAssembler | None = None
        self._stats = CaptureStats()

    # ------------------------------------------------------------------ utils
    def now(self) -> datetime:
        # whole-second resolution matches ct485cap stamps -> the (ts, millis,
        # payload) dedupe key lines up across the cutover overlap window.
        return datetime.now(self.tz).replace(microsecond=0)

    def line_ts(self, line: str) -> datetime:
        """Wall time for a line: embedded ISO stamp if present, else now."""
        ms = _parse_iso_ms(line)
        if ms is not None:
            return datetime.fromtimestamp(ms / 1000.0, self.tz)
        return self.now()

    def archive_write(self, line: str, ts: datetime) -> None:
        if self._archive is None:
            return
        if _parse_iso_ms(line) is None:
            line = ts.strftime("%Y-%m-%dT%H:%M:%S") + " " + line
        self._archive.write(line + "\n")

    # ------------------------------------------------------------- frame sink
    def _store_frame(self, frame: Frame, ts: datetime, millis: int) -> None:
        self.db.insert_raw_frames([frame_row(frame, ts, millis, self.source)])
        self.db.insert_events(self.deriver.feed_frame(frame, ts))

    def _flush_pending(self) -> None:
        if self._pending is not None:
            self._store_frame(self._pending, self._pending_ts, self._pending_millis)
            self._pending = None

    def handle_line(self, line: str) -> None:
        """One telnet line (any family; other lines are ignored)."""
        if "[shadow]" in line:
            sh = parse_shadow_line(line)
            if sh is not None:
                ts = self.line_ts(line)
                self.archive_write(line, ts)   # archived alongside [ct485*]
                self.db.insert_shadow(ts, sh)
            return
        if "[ct485" not in line:
            return
        ts = self.line_ts(line)
        self.archive_write(line, ts)

        stats = parse_stats_line(line)
        if stats is not None:
            self._flush_pending()
            self._asm.feed(line, 0)  # let the assembler flush any rej group
            self.db.insert_events(self.deriver.feed_stats(ts, stats[1]))
            return

        mm = MILLIS_RE.search(line)
        millis = int(mm.group(1)) if mm else 0

        before = self._asm.last_summary
        handled, done = self._asm.feed(line, 0)
        new_summary = self._asm.last_summary if self._asm.last_summary is not before else None

        for f in done:
            if f is new_summary:
                continue  # hold: may be extended by [ct485+] continuations
            # completed rej-salvage or a summary finalized by this line
            if f is self._pending:
                self._pending = None
            self._store_frame(f, ts if f.ts_ms is None or f.ts_ms < 1e11
                              else datetime.fromtimestamp(f.ts_ms / 1000.0, self.tz),
                              millis)
        if new_summary is not None:
            self._flush_pending()
            self._pending, self._pending_ts, self._pending_millis = new_summary, ts, millis

    def flush(self) -> None:
        self._flush_pending()
        if self._asm is not None:
            for f in self._asm.finish():
                self._store_frame(f, self.now(), 0)

    # ------------------------------------------------------------ live client
    async def run(self) -> None:
        """Connect-forever loop with ct485cap's idle semantics."""
        self._archive = open(self.archive_path, "a", buffering=1)
        first = True
        while True:
            start = time.time()
            try:
                await self._session(first)
            except (OSError, asyncio.IncompleteReadError) as e:
                log.warning("telnet session error: %s", e)
                self.db.insert_event(self.now(), "capture_gap", {
                    "reason": str(e), "session_s": round(time.time() - start, 1)})
            first = False
            self.flush()
            self._asm = None
            await asyncio.sleep(config.TELNET_RECONNECT_DELAY_S)

    async def _session(self, first: bool) -> None:
        log.info("connecting to %s:%d", config.CONTROLLER_IP, config.CONTROLLER_PORT)
        reader, writer = await asyncio.open_connection(
            config.CONTROLLER_IP, config.CONTROLLER_PORT)
        ts = self.now()
        # ct485cap-compatible session header keeps the archive re-parseable
        if self._archive is not None:
            self._archive.write(f"# ct485cap {config.CONTROLLER_IP} @ "
                                f"{ts.strftime('%Y-%m-%d %H:%M:%S')} (slylog-collector)\n")
        self.db.insert_event(ts, "capture_session", {
            "client": "slylog-collector", "controller": config.CONTROLLER_IP,
            "reconnect": not first})
        self._asm = TelnetAssembler("telnet-live", self._stats)
        buf = b""
        try:
            while True:
                try:
                    chunk = await asyncio.wait_for(
                        reader.read(4096), timeout=config.TELNET_IDLE_LIMIT_S)
                except asyncio.TimeoutError:
                    log.warning("no data for %ds — assuming dead link, reconnecting",
                                config.TELNET_IDLE_LIMIT_S)
                    self.db.insert_event(self.now(), "capture_gap", {
                        "reason": f"idle>{config.TELNET_IDLE_LIMIT_S}s"})
                    return
                if not chunk:
                    log.warning("connection closed by controller")
                    self.db.insert_event(self.now(), "capture_gap",
                                         {"reason": "connection closed"})
                    return
                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    self.handle_line(raw.decode("utf-8", "replace").rstrip("\r"))
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except OSError:
                pass
