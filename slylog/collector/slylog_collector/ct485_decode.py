#!/usr/bin/env python3
"""ct485_decode.py — PC-side CT-485 capture decoder / analysis instrument (Phase 2).

Decodes raw hex captures of the ClimateTalk / CT-485 bus (docs/02-protocol-climatetalk.md)
into validated frames, with analysis modes for the Phase 2 field-dictionary hunt
(docs/02 §8, docs/05 Phase 2).

Python 3.12, stdlib only. Protocol constants mirror lib/Ct485Core/Ct485Core.h —
that header is the single source of truth; keep the tables below in sync with it.

Usage examples:
  ct485_decode.py capture.log                       # full frame-by-frame decode
  ct485_decode.py --summary capture.log             # frame census by src/dst/msgType
  ct485_decode.py --diff capture.log                # 0x03/0x82 byte-grid diffs (modulation hunt)
  ct485_decode.py --terminals cap.terminals.csv cap.log   # annotate with 24V terminal states
  ct485_decode.py --field-dict dict.md capture.log  # emit/update field-dictionary markdown
  ct485_decode.py --no-gap-split blob.txt           # continuous hex blob, resync across lines
  ct485_decode.py ct485-capture.log                 # wall-unit telnet mirror lines
                                                    # ("[ct485] ..." via ct485cap.py) are
                                                    # auto-detected alongside hex captures
"""
from __future__ import annotations

import argparse
import bisect
import csv
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

# ---------------------------------------------------------------------------
# Protocol constants (mirror lib/Ct485Core/Ct485Core.h — do not invent values)
# ---------------------------------------------------------------------------

HEADER_LEN = 10
MAX_PAYLOAD = 240
CHECKSUM_LEN = 2
MIN_FRAME = HEADER_LEN + CHECKSUM_LEN  # 12
MAX_FRAME = HEADER_LEN + MAX_PAYLOAD + CHECKSUM_LEN  # 252

RESPONSE_FLAG = 0x80

# Header offsets (Ct485Core.h HeaderOffset)
OFF_DST, OFF_SRC, OFF_SUBNET, OFF_SEND_METHOD = 0, 1, 2, 3
OFF_PARAM_HI, OFF_PARAM_LO, OFF_SRC_NODE_TYPE = 4, 5, 6
OFF_MSG_TYPE, OFF_PACKET_NUM, OFF_PAYLOAD_LEN = 7, 8, 9

MSG_TYPE_NAMES = {
    0x00: "R2R",
    0x01: "GET_CONFIG",
    0x02: "GET_STATUS",
    0x03: "SET_CONTROL_CMD",
    0x04: "SET_DISPLAY_MSG",
    0x05: "SET_DIAGNOSTICS",
    0x06: "GET_DIAGNOSTICS",
    0x07: "GET_SENSOR_DATA",
    0x0D: "SET_IDENTIFICATION",
    0x0E: "GET_IDENTIFICATION",
    0x1D: "DMA_READ",
    0x1E: "DMA_READ_MOTOR",
    0x1F: "SET_MFG_GENERIC",
    0x20: "GET_MFG_GENERIC",
    0x41: "GET_USER_MENU",
    0x42: "UPDATE_USER_MENU",
    0x5A: "ECHO",
    0x75: "NETWORK_STATE_REQ",
    0x77: "TOKEN_OFFER",
    0x78: "VERSION_ANNOUNCE",
    0x79: "NODE_DISCOVERY",
    0x7A: "SET_ADDRESS",
    0x7B: "GET_NODE_ID",
}

NODE_TYPE_NAMES = {
    0x01: "THERMOSTAT",
    0x02: "GAS_FURNACE",
    0x03: "AIR_HANDLER",
    0x04: "AIR_CONDITIONER",
    0x05: "HEAT_PUMP",
    0x06: "ELECTRIC_FURNACE",
    0x09: "CROSSOVER_OBBI",
    0x0C: "UNITARY_CONTROL",
    0x15: "ZONE_CONTROL",
    0xA6: "COORDINATOR",
}

ADDR_NAMES = {
    0x00: "BROADCAST",
    0x01: "THERMOSTAT",
    0xF1: "VIRTUAL_SUB",
    0xFE: "ARBITRATION",
    0xFF: "COORDINATOR",
}

SUBNET_NAMES = {0x00: "BCAST", 0x01: "MAINT", 0x02: "V1", 0x03: "V2"}

SEND_METHOD_NAMES = {
    0x00: "NON_ROUTED",
    0x01: "PRIORITY/CTRL_CMD",
    0x02: "BY_NODE_TYPE",
    0x03: "BY_SOCKET",
}

COMMAND_NAMES = {
    0x01: "HEAT_SET_POINT_MODIFY",
    0x02: "COOL_SET_POINT_MODIFY",
    0x05: "SYSTEM_SWITCH_MODIFY",
    0x07: "FAN_KEY_SELECTION",
    0x47: "SET_POINT_TEMP_AND_HOLD",
    0x5D: "DEHUM_SET_POINT",
    0x5E: "HUM_SET_POINT",
    0x60: "DAMPER_POSITION",
    0x62: "DEHUM_DEMAND",
    0x63: "HUM_DEMAND",
    0x64: "HEAT_DEMAND",
    0x65: "COOL_DEMAND",
    0x66: "FAN_DEMAND",
    0x67: "BACKUP_HEAT_DEMAND",
    0x68: "DEFROST_DEMAND",
    0x69: "AUX_HEAT_DEMAND",
    0x6A: "SET_MOTOR_SPEED",
    0x6B: "SET_MOTOR_TORQUE",
    0x6C: "SET_AIRFLOW_DEMAND",
    0x70: "SET_MOTOR_TORQUE_PCT",
}

SYSTEM_SWITCH_NAMES = {
    0x00: "OFF",
    0x01: "COOL",
    0x02: "AUTO",
    0x03: "HEAT",
    0x04: "BACKUP_HEAT",
}

ACK_NAK_NAMES = {
    0x06: "ACK1",
    0x0A: "ACK2_UNDESIRED_PARAM",
    0x0D: "ACK3_MIN_PARAMS_INCOMPLETE",
    0x15: "NAK1_BAD_CRC",
    0x1B: "NAK2_INVALID",
}

# Commands whose value byte is percent*2 (docs/02 §5a table)
PCT_DEMAND_CMDS = {0x60, 0x62, 0x63, 0x64, 0x65, 0x67, 0x68, 0x69}


# ---------------------------------------------------------------------------
# Fletcher-16 (seed 0xAA, mod 0xFF) — ported from docs/02 §3 reference C++
# ---------------------------------------------------------------------------

def fletcher_pair(data: bytes) -> tuple[int, int]:
    """Checksum bytes for header+payload `data` (offsets 0..10+len-1)."""
    s1, s2 = 0xAA, 0x00
    for b in data:
        s1 = (s1 + b) % 0xFF
        s2 = (s2 + s1) % 0xFF
    c1 = 0xFF - ((s1 + s2) % 0xFF)
    c2 = 0xFF - ((s1 + c1) % 0xFF)
    return c1, c2


def fletcher_ok(frame: bytes) -> bool:
    """Valid iff accumulating header+payload AND both checksum bytes -> s1==s2==0."""
    s1, s2 = 0xAA, 0x00
    for b in frame:
        s1 = (s1 + b) % 0xFF
        s2 = (s2 + s1) % 0xFF
    return s1 == 0 and s2 == 0


def build_frame(header: bytes, payload: bytes = b"") -> bytes:
    """Assemble a valid frame (test/synthesis helper). header must be 10 bytes
    with header[9] == len(payload)."""
    assert len(header) == HEADER_LEN and header[OFF_PAYLOAD_LEN] == len(payload)
    body = header + payload
    c1, c2 = fletcher_pair(body)
    return body + bytes((c1, c2))


# ---------------------------------------------------------------------------
# Frame model
# ---------------------------------------------------------------------------

@dataclass
class Frame:
    raw: bytes
    ts_ms: float | None = None      # capture timestamp (epoch ms or sniffer millis)
    gap_us: int | None = None       # inter-frame gap reported by the sniffer
    line_no: int | None = None
    source: str = ""
    index: int = 0                  # ordinal across the whole run
    terminals: str | None = None    # 24V annotation, filled by --terminals
    synthesized: bool = False       # built from a telnet [ct485] summary line —
                                    # subnet/sendMethod/param/nodeType/pktNum are
                                    # zero-filled, checksum recomputed
    truncated: bool = False         # telnet mirror clips payloads at 16 bytes

    @property
    def dst(self) -> int: return self.raw[OFF_DST]
    @property
    def src(self) -> int: return self.raw[OFF_SRC]
    @property
    def subnet(self) -> int: return self.raw[OFF_SUBNET]
    @property
    def send_method(self) -> int: return self.raw[OFF_SEND_METHOD]
    @property
    def param_hi(self) -> int: return self.raw[OFF_PARAM_HI]
    @property
    def param_lo(self) -> int: return self.raw[OFF_PARAM_LO]
    @property
    def src_node_type(self) -> int: return self.raw[OFF_SRC_NODE_TYPE]
    @property
    def msg_type(self) -> int: return self.raw[OFF_MSG_TYPE]
    @property
    def packet_num(self) -> int: return self.raw[OFF_PACKET_NUM]
    @property
    def payload(self) -> bytes: return self.raw[HEADER_LEN:HEADER_LEN + self.raw[OFF_PAYLOAD_LEN]]

    @property
    def is_response(self) -> bool: return bool(self.msg_type & RESPONSE_FLAG)
    @property
    def base_msg_type(self) -> int: return self.msg_type & ~RESPONSE_FLAG

    def command_code(self) -> int | None:
        """Set-Control command code from the 16-bit LE payload echo at [10..11].
        Falls back to Send Parameter hi (header offset 4) if payload is short."""
        if self.base_msg_type != 0x03:
            return None
        p = self.payload
        if len(p) >= 2:
            return p[0] | (p[1] << 8)
        return self.param_hi


# ---------------------------------------------------------------------------
# Frame extraction with resync (slide until a frame validates)
# ---------------------------------------------------------------------------

def try_parse_at(buf: bytes, i: int) -> int | None:
    """Return total frame length if a valid frame starts at buf[i], else None."""
    if i + MIN_FRAME > len(buf):
        return None
    plen = buf[i + OFF_PAYLOAD_LEN]
    if plen > MAX_PAYLOAD:
        return None
    total = plen + MIN_FRAME
    if i + total > len(buf):
        return None
    if not fletcher_ok(buf[i:i + total]):
        return None
    return total


def extract_frames(buf: bytes) -> tuple[list[tuple[int, bytes]], int]:
    """Walk `buf` yielding (start_offset, frame_bytes); skipped bytes counted as
    garbage. Handles mid-stream joins: slides one byte at a time until a frame
    validates (length + Fletcher), so a join mid-frame costs only the partial
    frame's bytes."""
    frames: list[tuple[int, bytes]] = []
    garbage = 0
    i = 0
    n = len(buf)
    while i + MIN_FRAME <= n:
        total = try_parse_at(buf, i)
        if total is not None:
            frames.append((i, bytes(buf[i:i + total])))
            i += total
        else:
            garbage += 1
            i += 1
    garbage += n - i
    return frames, garbage


# ---------------------------------------------------------------------------
# Input parsing — permissive line formats
# ---------------------------------------------------------------------------

ISO_TS_RE = re.compile(r"(\d{4}-\d{2}-\d{2})[ T](\d{2}:\d{2}:\d{2}(?:\.\d+)?)")
HEX_PAIR_RE = re.compile(r"^[0-9A-Fa-f]{2}$")
LONG_HEX_RE = re.compile(r"^[0-9A-Fa-f]+$")

# Wall-unit telnet mirror (ct485cap.py): "[ct485] <millis> <SS>><DD> t<TT> l<N> <hex...>",
# optionally prefixed with an ISO wall-clock stamp. Pre-parsed summary, not a raw
# frame: header metadata beyond src/dst/msgType is absent and the preview line
# clips payloads at 16 bytes (main_thermostat.cpp sniffFrame). Firmware >= 0.5.2
# follows the preview with "[ct485+]" payload-continuation chunks, mirrors
# gap-framing rejects (torn/merged bursts) as raw "[ct485-rej]" chunks, and
# emits periodic "[ct485-stats]" counter lines.
SUMMARY_RE = re.compile(
    r"\[ct485\]\s+(\d+)\s+([0-9A-Fa-f]{2})>([0-9A-Fa-f]{2})"
    r"\s+t([0-9A-Fa-f]{2})\s+l(\d+)((?:\s+[0-9A-Fa-f]{2})*)\s*$")
CONT_RE = re.compile(r"\[ct485\+\]\s+(\d+)\s+(\d+)((?:\s+[0-9A-Fa-f]{2})+)\s*$")
REJ_RE = re.compile(r"\[ct485-rej\]\s+(\d+)\s+(\d+)((?:\s+[0-9A-Fa-f]{2})+)\s*$")
STATS_RE = re.compile(r"\[ct485-stats\]\s")


def _parse_iso_ms(line: str) -> float | None:
    m = ISO_TS_RE.search(line)
    if not m:
        return None
    try:
        dt = datetime.fromisoformat(m.group(1) + " " + m.group(2))
        return dt.timestamp() * 1000.0
    except ValueError:
        return None


def parse_summary_line(line: str) -> Frame | None:
    """Build a Frame from one telnet-mirror summary line, or None if not one.

    The summary already carries src/dst/msgType/payload, so a raw frame is
    synthesized around them (zero-filled header metadata, recomputed Fletcher)
    to ride the normal decode/summary/diff/field-dict paths. ts_ms is the ISO
    wall stamp when present (epoch ms), else the device millis."""
    m = SUMMARY_RE.search(line)
    if not m:
        return None
    declared = int(m.group(5))
    payload = bytes(int(t, 16) for t in m.group(6).split())
    header = bytes((
        int(m.group(3), 16),          # dst
        int(m.group(2), 16),          # src
        0, 0, 0, 0, 0,                # subnet/sendMethod/param/nodeType: not mirrored
        int(m.group(4), 16),          # msgType
        0,                            # packetNum: not mirrored
        len(payload)))
    ts_ms = _parse_iso_ms(line)
    if ts_ms is None:
        ts_ms = float(m.group(1))
    return Frame(raw=build_frame(header, payload), ts_ms=ts_ms,
                 synthesized=True, truncated=declared > len(payload))


class TelnetAssembler:
    """Stateful reassembly of the telnet-mirror line family.

    feed() returns frames completed BY that line (rej-group flushes); the
    summary frame itself is appended immediately by the caller and then
    extended in place by later [ct485+] continuations (same device millis).
    [ct485-rej] chunks accumulate until their group ends (chunk# restarts or a
    different line type arrives) and are then re-framed with the resync
    slider — recovering the valid frames a merged burst contains. finish()
    flushes a trailing rej group at end of input."""

    def __init__(self, source: str, stats: CaptureStats):
        self.source = source
        self.stats = stats
        self.last_summary: Frame | None = None
        self.last_summary_ms: str | None = None
        self.declared: int = 0
        self.rej_ms: str | None = None
        self.rej_ts: float | None = None
        self.rej_line: int | None = None
        self.rej_blob = bytearray()

    def _flush_rej(self) -> list[Frame]:
        if not self.rej_blob:
            return []
        found, garbage = extract_frames(bytes(self.rej_blob))
        self.stats.garbage_bytes += garbage
        out = [Frame(raw=raw, ts_ms=self.rej_ts, line_no=self.rej_line,
                     source=self.source) for _, raw in found]
        self.rej_blob = bytearray()
        self.rej_ms = None
        return out

    def feed(self, line: str, line_no: int) -> tuple[bool, list[Frame]]:
        """(handled, completed_frames) for one input line."""
        m = REJ_RE.search(line)
        if m:
            # Pool ALL consecutive rej lines into one blob before resync: the
            # firmware's poll-boundary gap detection slices frames into
            # multiple rejections (fw <= 0.5.2), so fragments of one frame
            # arrive as separate rej events. The bytes are contiguous bus
            # order; the Fletcher-validated slider finds the real frames.
            if not self.rej_blob:
                self.rej_ts = _parse_iso_ms(line) or float(m.group(1))
                self.rej_line = line_no
            self.rej_blob.extend(int(t, 16) for t in m.group(3).split())
            return True, []

        out = self._flush_rej()                  # any other line ends a group

        m = CONT_RE.search(line)
        if m:
            f = self.last_summary
            if f is not None and self.last_summary_ms == m.group(1):
                payload = f.payload + bytes(int(t, 16) for t in m.group(3).split())
                header = f.raw[:OFF_PAYLOAD_LEN] + bytes((len(payload),))
                f.raw = build_frame(header, payload)
                f.truncated = self.declared > len(payload)
            return True, out                     # orphan continuation: dropped

        sf = parse_summary_line(line)
        if sf is not None:
            sf.line_no, sf.source = line_no, self.source
            sm = SUMMARY_RE.search(line)
            self.last_summary = sf
            self.last_summary_ms = sm.group(1)
            self.declared = int(sm.group(5))
            return True, out + [sf]

        if STATS_RE.search(line):
            return True, out
        return False, out

    def finish(self) -> list[Frame]:
        return self._flush_rej()


def parse_line(line: str) -> tuple[float | None, int | None, bytes]:
    """Extract (ts_ms, gap_us, bytes) from one capture line.

    Permissive: takes the longest run of whitespace-separated 2-hex-digit
    tokens on the line (so dates, '(00,00)' markers and trailing text
    annotations are ignored). Sniffer format 'millis gap_us HEX...' is
    recognised by the decimal tokens immediately preceding the hex run.
    A single long even-length hex token (continuous blob line) also works.
    """
    if line.lstrip().startswith("#"):  # comment line (annotated fixtures)
        return None, None, b""
    ts_ms = _parse_iso_ms(line)
    gap_us: int | None = None
    tokens = line.split()

    runs: list[tuple[int, int]] = []
    start: int | None = None
    for idx, tok in enumerate(tokens):
        if HEX_PAIR_RE.match(tok):
            if start is None:
                start = idx
        elif start is not None:
            runs.append((start, idx))
            start = None
    if start is not None:
        runs.append((start, len(tokens)))

    data = b""
    run_start = None
    if runs:
        run_start, run_end = max(runs, key=lambda r: r[1] - r[0])
        data = bytes(int(tokens[k], 16) for k in range(run_start, run_end))

    if len(data) < MIN_FRAME:
        # Continuous-blob fallback: one long pure-hex token. Length/letter gates
        # keep decimal sniffer metadata (millis/gap) from being eaten as hex.
        for tok in tokens:
            if (LONG_HEX_RE.match(tok) and len(tok) % 2 == 0 and len(tok) > 2
                    and (len(tok) >= 12 or re.search(r"[A-Fa-f]", tok))):
                data = bytes.fromhex(tok)
                run_start = None
                break

    if ts_ms is None and run_start is not None:
        pre = [t for t in tokens[:run_start] if t.isdigit()][-2:]
        if pre:
            ts_ms = float(pre[0])
            if len(pre) == 2:
                gap_us = int(pre[1])

    return ts_ms, gap_us, data


@dataclass
class CaptureStats:
    lines: int = 0
    frames: int = 0
    garbage_bytes: int = 0


def read_capture(path: str, gap_split: bool = True,
                 stats: CaptureStats | None = None) -> list[Frame]:
    """Read one capture file into validated Frames.

    gap_split=True (default): each line is an independent frame boundary (the
    sniffer closes frames on the >=3.5 ms bus gap and emits one per line).
    gap_split=False: bytes from all lines are concatenated into one stream and
    the resync slider runs across line boundaries (continuous hex blob).
    """
    stats = stats if stats is not None else CaptureStats()
    text = Path(path).read_text(errors="replace")
    frames: list[Frame] = []

    asm = TelnetAssembler(path, stats)
    if gap_split:
        for line_no, line in enumerate(text.splitlines(), 1):
            stats.lines += 1
            handled, done = asm.feed(line, line_no)
            frames.extend(done)
            if handled:
                continue
            ts_ms, gap_us, data = parse_line(line)
            if not data:
                continue
            found, garbage = extract_frames(data)
            stats.garbage_bytes += garbage
            for _, raw in found:
                frames.append(Frame(raw=raw, ts_ms=ts_ms, gap_us=gap_us,
                                    line_no=line_no, source=path))
    else:
        buf = bytearray()
        meta: list[tuple[float | None, int | None]] = []  # per byte: (ts, line)
        for line_no, line in enumerate(text.splitlines(), 1):
            stats.lines += 1
            handled, done = asm.feed(line, line_no)  # mirror lines never join the blob
            frames.extend(done)
            if handled:
                continue
            ts_ms, _gap, data = parse_line(line)
            buf.extend(data)
            meta.extend([(ts_ms, line_no)] * len(data))
        found, garbage = extract_frames(bytes(buf))
        stats.garbage_bytes += garbage
        for off, raw in found:
            ts_ms, line_no = meta[off]
            frames.append(Frame(raw=raw, ts_ms=ts_ms, line_no=line_no, source=path))

    frames.extend(asm.finish())
    stats.frames += len(frames)
    return frames


# ---------------------------------------------------------------------------
# Decoders
# ---------------------------------------------------------------------------

def _name(table: dict[int, str], v: int) -> str:
    return table.get(v, "?")


def _addr(v: int) -> str:
    return f"0x{v:02X}({_name(ADDR_NAMES, v)})" if v in ADDR_NAMES else f"0x{v:02X}"


def fmt_refresh_timer(b: int) -> str:
    """High nibble = minutes (0-15), low nibble = seconds in 3.75 s units."""
    return f"0x{b:02X} ({b >> 4}m{(b & 0x0F) * 3.75:04.1f}s)"


def fmt_demand_pct(b: int) -> str:
    return f"0x{b:02X} ({b / 2:.1f}%)"


def hex_str(data: bytes) -> str:
    return " ".join(f"{b:02x}" for b in data)


def byte_grid(payload: bytes, mark: set[int] | None = None,
              indent: str = "    ") -> list[str]:
    """Render payload as an offset-labelled byte grid; offsets in `mark` get a
    caret row. Offsets are PAYLOAD offsets (add 10 for whole-buffer offsets)."""
    mark = mark or set()
    out: list[str] = []
    for base in range(0, len(payload), 16):
        chunk = payload[base:base + 16]
        offs = " ".join(f"{base + j:3d}" for j in range(len(chunk)))
        vals = " ".join(f" {b:02x}" for b in chunk)
        out.append(f"{indent}off {offs}")
        out.append(f"{indent}hex {vals}")
        carets = " ".join("^^^" if (base + j) in mark else "   "
                          for j in range(len(chunk)))
        if any((base + j) in mark for j in range(len(chunk))):
            out.append(f"{indent}    {carets}")
    if not payload:
        out.append(f"{indent}(empty payload)")
    return out


def decode_demand_variants(p: bytes) -> list[str]:
    """Decode the demand bytes under BOTH provisional offset variants
    (docs/02 §5a OFFSET WARNING — single-sourced and internally inconsistent;
    real captures are the arbiter). Payload offsets: variant A = buffer
    [12]/[13] = payload [2]/[3]; variant B = buffer [13]/[14] = payload [3]/[4].
    """
    out = ["  demand decode is AMBIGUOUS until offsets are sniff-confirmed (docs/02 §5a):"]

    def cell(i: int, fmt) -> str:
        if i < len(p):
            return fmt(p[i])
        return f"ABSENT (payload too short — evidence AGAINST this variant)"

    out.append(f"    variant A [12]/[13]: timer={cell(2, fmt_refresh_timer)} "
               f"demand={cell(3, fmt_demand_pct)}")
    out.append(f"    variant B [13]/[14]: timer={cell(3, fmt_refresh_timer)} "
               f"demand={cell(4, fmt_demand_pct)}")
    return out


def decode_set_control(frame: Frame) -> list[str]:
    p = frame.payload
    out: list[str] = []
    if len(p) < 2:
        out.append(f"  payload too short for command echo; header SendParam(hi)="
                   f"0x{frame.param_hi:02X} {_name(COMMAND_NAMES, frame.param_hi)}")
        return out
    cmd = p[0] | (p[1] << 8)
    out.append(f"  command=0x{cmd:04X} {_name(COMMAND_NAMES, cmd if cmd <= 0xFF else -1)}"
               f"{' (response/echo)' if frame.is_response else ''}")
    cmd8 = cmd if cmd <= 0xFF else -1

    if cmd8 in PCT_DEMAND_CMDS:
        out += decode_demand_variants(p)
    elif cmd8 == 0x66:  # FAN_DEMAND: timer, mode, percent*2 (one extra byte)
        out.append("  FAN_DEMAND decode (both provisional variants, docs/02 §5a):")

        def cell(i: int, fmt) -> str:
            return fmt(p[i]) if i < len(p) else \
                "ABSENT (payload too short — evidence AGAINST this variant)"

        hexb = lambda b: f"0x{b:02X}"
        out.append(f"    variant A [12]/[13]/[14]: timer={cell(2, fmt_refresh_timer)} "
                   f"mode={cell(3, hexb)} pct={cell(4, fmt_demand_pct)}")
        out.append(f"    variant B [13]/[14]/[15]: timer={cell(3, fmt_refresh_timer)} "
                   f"mode={cell(4, hexb)} pct={cell(5, fmt_demand_pct)}")
    elif cmd8 == 0x05:
        if len(p) > 3:
            v = p[3]
            out.append(f"  system switch -> 0x{v:02X} {_name(SYSTEM_SWITCH_NAMES, v)} "
                       f"(persistent, no refresh timer)")
        else:
            out.append("  system switch value byte absent")
    elif cmd8 in (0x01, 0x02, 0x47):
        if len(p) > 3:
            out.append(f"  setpoint byte [13]=0x{p[3]:02X} ({p[3]} raw; persistent)")
    elif cmd8 in (0x6A, 0x6B, 0x6C, 0x70) and len(p) >= 5:
        le = p[3] | (p[4] << 8)
        scale = {0x6A: ("RPM", 4), 0x6B: ("torque", 2048),
                 0x6C: ("CFM", 4), 0x70: ("%", 655.35)}[cmd8]
        out.append(f"  LE16 [13..14]=0x{le:04X} -> {le / scale[1]:.1f} {scale[0]} "
                   f"(provisional offsets)")

    if frame.is_response and len(p) >= 3 and p[2] in ACK_NAK_NAMES:
        out.append(f"  possible ack/nak byte [12]=0x{p[2]:02X} {ACK_NAK_NAMES[p[2]]} "
                   f"(or refresh-timer echo — ambiguous)")
    return out


def decode_diagnostics(payload: bytes) -> list[str]:
    """Get Diagnostics response: null-separated fault strings (docs/02 §5b)."""
    if not payload:
        return ["  (no faults / empty fault list)"]
    faults = []
    for chunk in payload.split(b"\x00"):
        if not chunk:
            continue
        text = "".join(chr(b) if 32 <= b < 127 else f"\\x{b:02x}" for b in chunk)
        faults.append(text)
    if not faults:
        return ["  (no faults / empty fault list)"]
    return [f"  fault[{i}]: {s}" for i, s in enumerate(faults)]


def walk_sensor_tlv(payload: bytes) -> tuple[list[tuple[int, bytes]], bool]:
    """Get Sensor Data response TLV walk: (db_id, db_len, data)*. Never reads
    past the buffer; returns (records, truncated). OAT expected at MDI id 0
    (docs/02 §5b — confirm on this install)."""
    recs: list[tuple[int, bytes]] = []
    i = 0
    truncated = False
    while i < len(payload):
        if i + 2 > len(payload):
            truncated = True
            break
        db_id, db_len = payload[i], payload[i + 1]
        if i + 2 + db_len > len(payload):
            truncated = True
            break
        recs.append((db_id, payload[i + 2:i + 2 + db_len]))
        i += 2 + db_len
    return recs, truncated


def decode_sensor_data(payload: bytes) -> list[str]:
    recs, truncated = walk_sensor_tlv(payload)
    out = [f"  sensor TLV: {len(recs)} record(s)"]
    for db_id, data in recs:
        note = "  <- MDI id 0: expected OAT slot (confirm)" if db_id == 0 else ""
        out.append(f"    id={db_id} len={len(data)} data=[{hex_str(data)}]{note}")
    if truncated:
        out.append("    WARNING: TLV truncated mid-record (bad length or short frame)")
    return out


def decode_frame(frame: Frame) -> list[str]:
    """Full human-readable decode of one validated frame."""
    f = frame
    mt = f.base_msg_type
    mt_name = _name(MSG_TYPE_NAMES, mt) + ("_RESP" if f.is_response else "")
    ts = format_ts(f.ts_ms)
    head = (f"#{f.index:05d} {ts} line {f.line_no or '?'} len={len(f.raw)} "
            f"src={_addr(f.src)} dst={_addr(f.dst)} msg=0x{f.msg_type:02X} {mt_name}")
    out = [head]
    pn = f.packet_num
    out.append(
        f"  subnet=0x{f.subnet:02X}({_name(SUBNET_NAMES, f.subnet)}) "
        f"sendMethod=0x{f.send_method:02X}({_name(SEND_METHOD_NAMES, f.send_method)}) "
        f"param=0x{f.param_hi:02X}/0x{f.param_lo:02X}"
        + (f" (target nodeType {_name(NODE_TYPE_NAMES, f.param_hi)})"
           if f.send_method == 0x02 else "")
        + (f" (cmd {_name(COMMAND_NAMES, f.param_hi)})" if f.send_method == 0x01
           and f.param_hi in COMMAND_NAMES else "")
        + f" srcNodeType=0x{f.src_node_type:02X}({_name(NODE_TYPE_NAMES, f.src_node_type)})"
        f" pktNum=0x{pn:02X}(dataflow={1 if pn & 0x80 else 0},"
        f"ver={'1.0' if pn & 0x20 else '2.0'},chunk={pn & 0x1F})"
        f" payloadLen={f.raw[OFF_PAYLOAD_LEN]}")
    if f.synthesized:
        out.append("  (from telnet summary: subnet/sendMethod/param/nodeType/pktNum"
                   " not mirrored, zero-filled"
                   + (f"; payload clipped at {len(f.payload)}B by the mirror"
                      if f.truncated else "") + ")")
    if f.gap_us is not None:
        out.append(f"  bus gap before frame: {f.gap_us} us")
    if f.terminals:
        out.append(f"  24V terminals: {f.terminals}")

    if mt == 0x03:
        out += decode_set_control(f)
    elif mt == 0x06 and f.is_response:
        out += decode_diagnostics(f.payload)
    elif mt == 0x07 and f.is_response:
        out += decode_sensor_data(f.payload)
    elif f.msg_type in (0x82, 0x9D):
        out.append("  payload byte grid (payload offsets; +10 for buffer offsets):")
        out += byte_grid(f.payload)
    elif f.payload:
        out.append(f"  payload: [{hex_str(f.payload)}]")
    out.append(f"  raw: {hex_str(f.raw)}")
    return out


def format_ts(ts_ms: float | None) -> str:
    if ts_ms is None:
        return "t=?"
    if ts_ms > 1e11:  # epoch milliseconds
        return "t=" + datetime.fromtimestamp(ts_ms / 1000.0).isoformat(sep=" ")
    return f"t={ts_ms:.0f}ms"


# ---------------------------------------------------------------------------
# Analysis: --summary
# ---------------------------------------------------------------------------

def summarize(frames: list[Frame], stats: CaptureStats) -> list[str]:
    census = Counter((f.src, f.dst, f.msg_type) for f in frames)
    out = ["== frame census (src -> dst, msgType) =="]
    out.append(f"{'src':>4} {'dst':>4} {'msg':>5}  {'name':<24} {'count':>6}")
    for (src, dst, mt), n in sorted(census.items(), key=lambda kv: -kv[1]):
        name = _name(MSG_TYPE_NAMES, mt & ~RESPONSE_FLAG)
        if mt & RESPONSE_FLAG:
            name += "_RESP"
        out.append(f"0x{src:02X} 0x{dst:02X}  0x{mt:02X}  {name:<24} {n:>6}")
    node_types = Counter((f.src, f.src_node_type) for f in frames)
    out.append("== nodes seen (src address -> advertised node type) ==")
    for (src, nt), n in sorted(node_types.items()):
        out.append(f"  src 0x{src:02X}: nodeType 0x{nt:02X} "
                   f"{_name(NODE_TYPE_NAMES, nt)} ({n} frames)")
    out.append(f"== totals: {stats.frames} valid frames, "
               f"{stats.garbage_bytes} garbage bytes skipped, "
               f"{stats.lines} input lines ==")
    return out


# ---------------------------------------------------------------------------
# Analysis: --diff (stimulus-response modulation-byte hunt, docs/02 §8 step 5)
# ---------------------------------------------------------------------------

def diff_key(frame: Frame) -> tuple | None:
    """Frames eligible for --diff: Set-Control (0x03) grouped by command code
    and (for node-type routing) target node type, and Get-Status responses
    (0x82)."""
    if frame.msg_type == 0x03:
        target = frame.param_hi if frame.send_method == 0x02 else None
        return (frame.src, frame.dst, frame.msg_type, frame.command_code(), target)
    if frame.msg_type == 0x82:
        return (frame.src, frame.dst, frame.msg_type, None, None)
    return None


def diff_groups(frames: list[Frame]) -> dict[tuple, list[Frame]]:
    groups: dict[tuple, list[Frame]] = defaultdict(list)
    for f in frames:
        key = diff_key(f)
        if key is not None:
            groups[key].append(f)
    return dict(groups)


def diff_payloads(a: bytes, b: bytes) -> list[tuple[int, int | None, int | None]]:
    """(payload_offset, old, new) for every differing byte; None = absent."""
    diffs = []
    for i in range(max(len(a), len(b))):
        va = a[i] if i < len(a) else None
        vb = b[i] if i < len(b) else None
        if va != vb:
            diffs.append((i, va, vb))
    return diffs


def changed_offsets(group: list[Frame]) -> set[int]:
    offs: set[int] = set()
    for prev, curr in zip(group, group[1:]):
        offs |= {i for i, _, _ in diff_payloads(prev.payload, curr.payload)}
    return offs


def render_diff(groups: dict[tuple, list[Frame]]) -> list[str]:
    out: list[str] = []
    for key in sorted(groups, key=lambda k: (k[2], k[0], k[1], k[3] or -1, k[4] or -1)):
        src, dst, mt, cmd, target = key
        group = groups[key]
        label = f"src={_addr(src)} dst={_addr(dst)} msg=0x{mt:02X} " \
                f"{_name(MSG_TYPE_NAMES, mt & ~RESPONSE_FLAG)}" \
                + ("_RESP" if mt & RESPONSE_FLAG else "")
        if cmd is not None:
            label += f" cmd=0x{cmd:04X} {_name(COMMAND_NAMES, cmd if cmd <= 0xFF else -1)}"
        if target is not None:
            label += f" target=0x{target:02X}({_name(NODE_TYPE_NAMES, target)})"
        out.append(f"== diff group: {label} ({len(group)} frames) ==")
        if not group:
            continue
        offs = changed_offsets(group)
        out.append(f"  baseline {format_ts(group[0].ts_ms)} payload:")
        out += byte_grid(group[0].payload, mark=offs)
        unchanged = 0
        for prev, curr in zip(group, group[1:]):
            diffs = diff_payloads(prev.payload, curr.payload)
            if not diffs:
                unchanged += 1
                continue
            if unchanged:
                out.append(f"  ... {unchanged} identical frame(s) ...")
                unchanged = 0
            cells = "  ".join(
                f"[{i}] " +
                (f"{old:02x}" if old is not None else "--") + "->" +
                (f"{new:02x}" if new is not None else "--")
                for i, old, new in diffs)
            term = f"  24V[{curr.terminals}]" if curr.terminals else ""
            out.append(f"  #{curr.index:05d} {format_ts(curr.ts_ms)}: {cells}{term}")
        if unchanged:
            out.append(f"  ... {unchanged} identical frame(s) ...")
        if offs:
            buf_offs = ", ".join(f"{o}(buf {o + 10})" for o in sorted(offs))
            out.append(f"  variable payload offsets: {buf_offs}")
        else:
            out.append("  no payload variation in this group")
        out.append("")
    if not groups:
        out.append("no 0x03 / 0x82 frames to diff")
    return out


# ---------------------------------------------------------------------------
# Analysis: --terminals (24V terminal-state CSV merge, captures/README.md)
# ---------------------------------------------------------------------------

def _parse_csv_ts(cell: str, unit: str) -> float | None:
    cell = cell.strip()
    iso = _parse_iso_ms(cell)
    if iso is not None:
        return iso
    try:
        v = float(cell)
    except ValueError:
        return None
    return v * 1000.0 if unit == "s" else v


def load_terminals(path: str, unit: str = "ms") -> list[tuple[float, str]]:
    """CSV: first column timestamp (ISO, epoch s, or sniffer ms — must share the
    capture's clock per captures/README.md), remaining columns terminal states
    (e.g. Y,O-B,W,G,D with 0/1 values). Header row optional."""
    rows: list[tuple[float, str]] = []
    header: list[str] | None = None
    with open(path, newline="") as fh:
        for rec in csv.reader(fh):
            rec = [c.strip() for c in rec]
            if not rec or not any(rec):
                continue
            ts = _parse_csv_ts(rec[0], unit)
            if ts is None:
                if header is None:
                    header = rec
                continue
            names = (header[1:] if header else
                     [f"T{i}" for i in range(1, len(rec))])
            state = " ".join(f"{n}={v}" for n, v in zip(names, rec[1:]))
            rows.append((ts, state))
    rows.sort(key=lambda r: r[0])
    return rows


def annotate_terminals(frames: list[Frame], rows: list[tuple[float, str]]) -> int:
    """Attach the most recent terminal state at-or-before each frame timestamp."""
    if not rows:
        return 0
    keys = [r[0] for r in rows]
    n = 0
    for f in frames:
        if f.ts_ms is None:
            continue
        i = bisect.bisect_right(keys, f.ts_ms) - 1
        if i >= 0:
            f.terminals = rows[i][1]
            n += 1
    return n


# ---------------------------------------------------------------------------
# Analysis: --field-dict (emit/update the field-dictionary markdown table)
# ---------------------------------------------------------------------------

FIELD_DICT_HEADER = [
    "# CT-485 field dictionary",
    "",
    "Auto-maintained by tools/ct485_decode.py --field-dict. The `meaning` column",
    "is human-edited and preserved across updates; everything else is regenerated",
    "from captures. Offsets are PAYLOAD offsets (buffer offset = payload + 10).",
    "",
    "| msgType | command | payload offset | observed | meaning |",
    "|---|---|---|---|---|",
]

ROW_RE = re.compile(
    r"^\|\s*(0x[0-9A-Fa-f]{2})\S*[^|]*\|\s*([^|]*?)\s*\|\s*([^|]*?)\s*\|"
    r"\s*([^|]*?)\s*\|\s*([^|]*?)\s*\|\s*$")


def parse_field_dict(text: str) -> dict[tuple[int, int | None, int | None], dict]:
    """Parse existing dictionary rows -> {(msgType, command, offset): {observed, meaning}}."""
    entries: dict[tuple[int, int | None, int | None], dict] = {}
    for line in text.splitlines():
        m = ROW_RE.match(line)
        if not m:
            continue
        mt = int(m.group(1), 16)
        cmd_cell, off_cell = m.group(2), m.group(3)
        cmd = None
        cm = re.match(r"0x([0-9A-Fa-f]+)", cmd_cell)
        if cm:
            cmd = int(cm.group(1), 16)
        off = int(off_cell) if off_cell.isdigit() else None
        entries[(mt, cmd, off)] = {"observed": m.group(4), "meaning": m.group(5)}
    return entries


def collect_field_observations(frames: list[Frame]) -> dict[tuple[int, int | None, int | None], str]:
    """Observations: one presence row per (msgType[,command]) group, plus one row
    per payload offset that VARIES within a diff-eligible group (the candidate
    fields worth naming — the modulation byte hunt)."""
    obs: dict[tuple[int, int | None, int | None], str] = {}
    presence: dict[tuple[int, int | None], int] = Counter()
    for f in frames:
        cmd = f.command_code() if f.base_msg_type == 0x03 else None
        presence[(f.msg_type, cmd)] += 1
    for (mt, cmd), n in presence.items():
        obs[(mt, cmd, None)] = f"{n} frame(s)"
    for key, group in diff_groups(frames).items():
        _src, _dst, mt, cmd, _target = key
        for off in sorted(changed_offsets(group)):
            values = sorted({f.payload[off] for f in group if off < len(f.payload)})
            shown = ", ".join(f"0x{v:02x}" for v in values[:10])
            if len(values) > 10:
                shown += f", ... ({len(values)} distinct)"
            prev = obs.get((mt, cmd, off))
            obs[(mt, cmd, off)] = shown if prev is None else prev
    return obs


def update_field_dict(path: str, frames: list[Frame]) -> str:
    existing: dict = {}
    p = Path(path)
    if p.exists():
        existing = parse_field_dict(p.read_text())
    obs = collect_field_observations(frames)

    merged: dict[tuple[int, int | None, int | None], dict] = {}
    for key, entry in existing.items():
        merged[key] = dict(entry)
    for key, observed in obs.items():
        meaning = merged.get(key, {}).get("meaning", "")
        merged[key] = {"observed": observed, "meaning": meaning}

    lines = list(FIELD_DICT_HEADER)
    def sort_key(k):
        mt, cmd, off = k
        return (mt, -1 if cmd is None else cmd, -1 if off is None else off)
    for mt, cmd, off in sorted(merged, key=sort_key):
        entry = merged[(mt, cmd, off)]
        mt_cell = f"0x{mt:02X} {_name(MSG_TYPE_NAMES, mt & ~RESPONSE_FLAG)}" \
                  + ("_RESP" if mt & RESPONSE_FLAG else "")
        cmd_cell = ("—" if cmd is None else
                    f"0x{cmd:02X} {_name(COMMAND_NAMES, cmd if cmd <= 0xFF else -1)}")
        off_cell = "—" if off is None else str(off)
        lines.append(f"| {mt_cell} | {cmd_cell} | {off_cell} "
                     f"| {entry['observed']} | {entry['meaning']} |")
    text = "\n".join(lines) + "\n"
    p.write_text(text)
    return text


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="CT-485 capture decoder / Phase 2 analysis instrument "
                    "(docs/02-protocol-climatetalk.md)")
    ap.add_argument("captures", nargs="+", help="capture file(s): hex lines or blob")
    ap.add_argument("--gap-split", action=argparse.BooleanOptionalAction, default=True,
                    help="treat each input line as a frame boundary (default); "
                         "--no-gap-split concatenates all bytes and resyncs "
                         "across line boundaries (continuous hex blob)")
    ap.add_argument("--summary", action="store_true",
                    help="frame census by src/dst/msgType instead of full decode")
    ap.add_argument("--diff", action="store_true",
                    help="byte-grid diffs of successive 0x03/0x82 frames "
                         "(stimulus-response modulation-byte hunt)")
    ap.add_argument("--terminals", metavar="FILE.terminals.csv",
                    help="merge a 24V terminal-state CSV by timestamp and "
                         "annotate frames (captures/README.md convention)")
    ap.add_argument("--terminals-unit", choices=["ms", "s"], default="ms",
                    help="unit of numeric timestamps in the terminals CSV "
                         "(must share the capture's clock)")
    ap.add_argument("--field-dict", metavar="OUT.md",
                    help="emit/update a field-dictionary markdown table")
    args = ap.parse_args(argv)

    stats = CaptureStats()
    frames: list[Frame] = []
    for path in args.captures:
        frames.extend(read_capture(path, gap_split=args.gap_split, stats=stats))
    for i, f in enumerate(frames):
        f.index = i

    if args.terminals:
        rows = load_terminals(args.terminals, unit=args.terminals_unit)
        n = annotate_terminals(frames, rows)
        print(f"terminals: {len(rows)} state rows loaded from {args.terminals}, "
              f"{n}/{len(frames)} frames annotated")

    ran_mode = False
    if args.summary:
        print("\n".join(summarize(frames, stats)))
        ran_mode = True
    if args.diff:
        print("\n".join(render_diff(diff_groups(frames))))
        ran_mode = True
    if args.field_dict:
        update_field_dict(args.field_dict, frames)
        print(f"field dictionary written to {args.field_dict}")
        ran_mode = True
    if not ran_mode:
        for f in frames:
            print("\n".join(decode_frame(f)))
            print()
        print(f"{stats.frames} valid frames, {stats.garbage_bytes} garbage bytes "
              f"skipped, {stats.lines} lines")
    return 0


if __name__ == "__main__":
    sys.exit(main())
