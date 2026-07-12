"""Derive human-meaningful events from decoded CT-485 frames (#133).

Domain rules (docs/02 + observed captures):
- t03 SET_CONTROL_CMD payload = [cmdLo, cmdHi, timer, value]
- cmd 0x65 = COOL_DEMAND, 0x64 = HEAT_DEMAND, value = percent*2 (0x3C = 30%)
- demand > 0 after 0  -> cycle_start
- explicit demand = 0 -> cycle_stop (only on transition; the bus refreshes
  the same demand periodically)
- other nonzero -> demand_change
- unknown command codes (e.g. 0x0061, 0x0006) -> novel_command (rate-limited)
- t83 responses with ACK-led payloads are echoes: NEVER counted for cycles.
"""
from __future__ import annotations

from datetime import datetime

from .ct485_decode import ACK_NAK_NAMES, COMMAND_NAMES, MSG_TYPE_NAMES, Frame
from . import config

DEMAND_CMDS = {0x64: "HEAT_DEMAND", 0x65: "COOL_DEMAND"}


class EventDeriver:
    """Stateful frame -> events. One instance per continuous capture stream
    (live session, or the whole ordered backfill)."""

    def __init__(self):
        self._demand: dict[int, float] = {}          # cmd -> last percent
        self._novel_last: dict[int, float] = {}      # cmd -> last emit epoch
        self._stats_prev: dict[str, int] | None = None
        self._stats_last_emit: float = 0.0

    def feed_frame(self, frame: Frame, ts: datetime) -> list[tuple[datetime, str, dict]]:
        """Events derived from one validated frame at wall time `ts`."""
        # Only request-direction Set Control commands drive cycle events;
        # response frames (0x83) are echoes of what we already counted.
        if frame.base_msg_type != 0x03 or frame.is_response:
            return []
        p = frame.payload
        if len(p) < 1:
            return []
        # ClimateTalk command codes are a SINGLE byte. A prior 16-bit LE read
        # (p[0] | p[1]<<8) invented phantom opcodes like 0xA506 whenever the
        # second payload byte was nonzero MAC/session data (e.g. an 0x06
        # R2R-ACK frame), inflating novel_command. Demands are unaffected —
        # their p[1] is always 0x00.
        cmd = p[0]
        out: list[tuple[datetime, str, dict]] = []

        if cmd in DEMAND_CMDS:
            if len(p) < 4:
                return []
            pct = p[3] / 2.0
            prev = self._demand.get(cmd)
            base = {
                "cmd": DEMAND_CMDS[cmd], "code": cmd, "pct": pct,
                "prev_pct": prev, "millis": frame.ts_ms,
                "src": frame.src, "dst": frame.dst,
            }
            if prev is None:
                # First sighting this stream: only a nonzero demand is an event
                # (we may have joined mid-cycle).
                if pct > 0:
                    out.append((ts, "cycle_start", {**base, "resync": True}))
            elif prev == 0 and pct > 0:
                out.append((ts, "cycle_start", base))
            elif prev > 0 and pct == 0:
                out.append((ts, "cycle_stop", base))
            elif pct != prev:
                out.append((ts, "demand_change", base))
            self._demand[cmd] = pct
        elif cmd not in COMMAND_NAMES and cmd not in ACK_NAK_NAMES:
            # Genuinely unknown Set-Control command (known ACKs like 0x06 and
            # named statuses like 0x61 are no longer flagged).
            epoch = ts.timestamp()
            if epoch - self._novel_last.get(cmd, 0.0) >= config.NOVEL_CMD_INTERVAL_S:
                self._novel_last[cmd] = epoch
                out.append((ts, "novel_command", {
                    "code": cmd, "code_hex": f"0x{cmd:02X}",
                    "payload": p.hex(), "millis": frame.ts_ms,
                    "src": frame.src, "dst": frame.dst,
                    "msg_type": frame.msg_type,
                }))
        return out

    def feed_stats(self, ts: datetime, counters: dict[str, int]) -> list[tuple[datetime, str, dict]]:
        """[ct485-stats] snapshot: store when error counters change, and at
        least every STATS_HEARTBEAT_S as a liveness heartbeat."""
        errs = {k: counters.get(k, 0) for k in ("badLen", "badCk", "over")}
        prev_errs = ({k: self._stats_prev.get(k, 0) for k in errs}
                     if self._stats_prev is not None else None)
        epoch = ts.timestamp()
        emit = (prev_errs is None or errs != prev_errs
                or epoch - self._stats_last_emit >= config.STATS_HEARTBEAT_S)
        self._stats_prev = counters
        if not emit:
            return []
        self._stats_last_emit = epoch
        return [(ts, "ct485_stats", counters)]

    def demand_snapshot(self) -> dict[str, float]:
        return {DEMAND_CMDS[c]: v for c, v in self._demand.items()}


def msg_type_name(mt: int) -> str:
    name = MSG_TYPE_NAMES.get(mt & 0x7F, "?")
    return name + ("_RESP" if mt & 0x80 else "")
