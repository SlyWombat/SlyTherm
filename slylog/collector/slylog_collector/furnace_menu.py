"""CT-485 Get User Menu replies (msg 0xC1) -> key/value rows (#221).

The OEM stat reads the furnace's live status as text pages. Payload layout,
from the 2026-10-18 captures:

    <idx> <page> 00 00 00 F0 <body>

`page` echoes the requested page (00 = the menu list, 01 = Status 1, ...).
A page body is   B6 C5 D0 <title> B7   then one row per value:

    C2 E2 <label> E0 <value> C3

The menu list (page 0) is   A0 A6 { B6 [C5 D0] <title> B7 }*  with the C5 D0
marker only on the first two titles. Values are fixed-width ASCII, padded
with spaces ("  0%", "OPEN   ", "OFF       ").

The grammar is deliberately tolerant: only Status 1 has been seen so far, and
Status 2 / history / unit-info rows may carry formats we have not met. A reply
that does not parse still returns a MenuPage (rows empty, complete False) with
the raw payload attached, so the caller can store it for re-decoding.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field

MSG_GET_USER_MENU = 0x41
MSG_USER_MENU_REPLY = 0xC1

PAGE_HDR = b"\xb6\xc5\xd0"
PAGE_END = b"\xb7"
ROW_START = b"\xc2\xe2"
ROW_SEP = b"\xe0"
ROW_END = b"\xc3"
PREFIX_LEN = 6            # idx, page, 00 00 00, F0

_NUM_RE = re.compile(r"^-?\d+(?:\.\d+)?")


@dataclass
class MenuPage:
    page: int
    title: str | None = None
    rows: list[tuple[str, str, float | None]] = field(default_factory=list)
    menu_titles: list[str] = field(default_factory=list)   # page 0 only
    complete: bool = False
    payload: bytes = b""


def _text(b: bytes) -> str:
    return b.decode("latin-1").replace("\x00", "").strip()


def _number(value: str) -> float | None:
    """Leading number of a value: '100%' -> 100, '1682' -> 1682, 'OPEN' -> None."""
    m = _NUM_RE.match(value)
    return float(m.group(0)) if m else None


def decode_menu_reply(payload: bytes) -> MenuPage | None:
    """Decode the payload of a 0xC1 frame, or None if it is not a menu page."""
    if len(payload) < PREFIX_LEN + 2 or payload[5] != 0xF0:
        return None
    page = MenuPage(page=payload[1], payload=bytes(payload))
    body = payload[PREFIX_LEN:]

    if page.page == 0:
        # Menu list: each title sits between B6 (optionally B6 C5 D0) and B7.
        for chunk in body.split(PAGE_END):
            i = chunk.rfind(b"\xb6")
            if i < 0:
                continue
            raw = chunk[i + 1:]
            if raw[:2] == b"\xc5\xd0":
                raw = raw[2:]
            t = _text(raw)
            if t:
                page.menu_titles.append(t)
        page.complete = bool(page.menu_titles) and body.rstrip(b"\x00").endswith(PAGE_END)
        return page

    start = body.find(PAGE_HDR)
    if start < 0:
        return page
    end = body.find(PAGE_END, start + len(PAGE_HDR))
    if end < 0:
        return page
    page.title = _text(body[start + len(PAGE_HDR):end])

    pos = end + 1
    while True:
        r = body.find(ROW_START, pos)
        if r < 0:
            break
        sep = body.find(ROW_SEP, r + 2)
        stop = body.find(ROW_END, r + 2)
        if sep < 0 or stop < 0 or sep > stop:
            break                      # row cut off: leave the page incomplete
        label = _text(body[r + 2:sep])
        value = _text(body[sep + 1:stop])
        page.rows.append((label, value, _number(value)))
        pos = stop + 1
    page.complete = bool(page.rows) and body[pos:].rstrip(b"\x00") == b""
    return page
