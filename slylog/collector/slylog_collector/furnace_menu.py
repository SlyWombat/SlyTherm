"""CT-485 Get User Menu replies (msg 0xC1) -> key/value rows (#221).

The OEM stat reads the furnace's live status as text pages. Payload layout,
from the 2026-10-18 captures:

    <idx> <page> 00 00 00 F0 <body>

`page` echoes the requested page (00 = the menu list, 01 = Status 1, 02 =
Status 2, 03 = 2WK HIST, 04 = LIFE HIST, 05 = FAULT HIST, 06 = UNIT INFO,
07 = SETUP). A page body is   B6 [C5 D0] <title> B7   (the C5 D0 marker only on
Status 1/2), then one row per value. Row shapes seen on the real furnace:

    C2 E2 <label> E0 <value> C3                      read-only (status, history)
    C2 E1 <label> E0 <current> E0 <opt> E0 <opt> C3  editable (SETUP): the first
                                                     field is the current value,
                                                     the rest are its options
    C2 E1 <label> E8 <current> E0 <opt> C3           same, with an E8 separator

Fault history values read "DAYS 68" (days since the fault). The menu list
(page 0) is  A0 A6 { B6 [C5 D0] <title> B7 }*. Values are fixed-width ASCII,
padded with spaces.

The grammar is tolerant. A reply that does not parse still returns a MenuPage
(complete False) with the raw payload attached, so the caller can store it for
re-decoding.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field

MSG_GET_USER_MENU = 0x41
MSG_USER_MENU_REPLY = 0xC1

PAGE_END = b"\xb7"
ROW_START = b"\xc2"          # followed by E2 (read-only) or E1 (editable)
ROW_KINDS = (0xE2, 0xE1)
ROW_SEP = b"\xe0"
ROW_SEP2 = b"\xe8"           # label/current separator on some editable rows
ROW_END = b"\xc3"
PREFIX_LEN = 6               # idx, page, 00 00 00, F0

_NUM_RE = re.compile(r"^-?\d+(?:\.\d+)?")
_DAYS_RE = re.compile(r"^DAYS\s+(\d+)$")


@dataclass
class MenuRow:
    label: str
    value: str
    number: float | None = None
    options: str | None = None   # editable rows: the other choices, "|"-joined


@dataclass
class MenuPage:
    page: int
    title: str | None = None
    rows: list[MenuRow] = field(default_factory=list)
    menu_titles: list[str] = field(default_factory=list)   # page 0 only
    complete: bool = False
    payload: bytes = b""


def _text(b: bytes) -> str:
    return b.decode("latin-1").replace("\x00", "").strip()


def _number(value: str) -> float | None:
    """Reading of a value: '100%' -> 100, '1682' -> 1682, 'DAYS 68' -> 68."""
    m = _DAYS_RE.match(value)
    if m:
        return float(m.group(1))
    m = _NUM_RE.match(value)
    return float(m.group(0)) if m else None


def _title_at(body: bytes, start: int) -> tuple[str, int] | None:
    """Title after the B6 at `start` (optional C5 D0 marker): (text, next pos)."""
    t = start + 1
    if body[t:t + 2] == b"\xc5\xd0":
        t += 2
    end = body.find(PAGE_END, t)
    if end < 0:
        return None
    return _text(body[t:end]), end + 1


def decode_menu_reply(payload: bytes) -> MenuPage | None:
    """Decode the payload of a 0xC1 frame, or None if it is not a menu page."""
    if len(payload) < PREFIX_LEN + 2 or payload[5] != 0xF0:
        return None
    page = MenuPage(page=payload[1], payload=bytes(payload))
    body = payload[PREFIX_LEN:]

    if page.page == 0:
        pos = 0
        while True:
            i = body.find(b"\xb6", pos)
            if i < 0:
                break
            got = _title_at(body, i)
            if got is None:
                break
            if got[0]:
                page.menu_titles.append(got[0])
            pos = got[1]
        page.complete = bool(page.menu_titles) and body.rstrip(b"\x00").endswith(PAGE_END)
        return page

    start = body.find(b"\xb6")
    if start < 0:
        return page
    got = _title_at(body, start)
    if got is None:
        return page
    page.title, pos = got

    while True:
        r = body.find(ROW_START, pos)
        if r < 0 or r + 1 >= len(body) or body[r + 1] not in ROW_KINDS:
            break
        stop = body.find(ROW_END, r + 2)
        if stop < 0:
            break                      # row cut off: leave the page incomplete
        cells = body[r + 2:stop]
        seps = [i for i in (cells.find(ROW_SEP), cells.find(ROW_SEP2)) if i >= 0]
        if not seps:
            break
        sep = min(seps)
        label = _text(cells[:sep])
        fields = [_text(f) for f in
                  cells[sep + 1:].replace(ROW_SEP2, ROW_SEP).split(ROW_SEP)]
        value = fields[0]
        options = "|".join(fields[1:]) if len(fields) > 1 else None
        page.rows.append(MenuRow(label, value, _number(value), options))
        pos = stop + 1
    page.complete = bool(page.rows) and body[pos:].rstrip(b"\x00") == b""
    return page
