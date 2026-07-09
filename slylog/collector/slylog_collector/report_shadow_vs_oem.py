"""Shadow-vs-OEM comparison report (#139).

Given a time window, compares the OEM thermostat's real CT-485 demands
(raw_frames: t03 SET_CONTROL_CMD, value = percent*2) against our controller's
shadow DemandSet (shadow_demands, fw >= 0.5.8, TX disabled) and writes a
markdown report: per-channel cycle tables, divergence metrics (time-weighted
|OEM - shadow|, one-side-idle minutes, cycle-start timing offsets) and an
energy proxy (integral of demand% * dt).

Usage (inside the collector container):
    python -m slylog_collector.report_shadow_vs_oem \
        [--start 2026-07-09T17:45:00Z] [--hours 24] [--out /reports/...]

Defaults: the last 24 h, output /reports/shadow-vs-oem-<date>.md.
"""
from __future__ import annotations

import argparse
import logging
import statistics
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path
from zoneinfo import ZoneInfo

import psycopg

from . import config

log = logging.getLogger("report")

# OEM CT-485 demand command byte -> channel; percent byte offset in payload
# [cmdLo, cmdHi, timer, value] — FAN_DEMAND carries an extra mode byte:
# [cmdLo, cmdHi, timer, mode, value] (docs/02 §5a, provisional offsets).
OEM_CMDS = {0x64: ("heat", 3), 0x65: ("cool", 3), 0x66: ("fan", 4)}
CHANNELS = ("heat", "cool", "fan")
PAIR_WINDOW_MIN = 60  # max offset when pairing shadow/OEM cycle starts

Step = list[tuple[datetime, float]]  # ordered (time, value-from-here-on)


# --------------------------------------------------------------------- fetch
def fetch(conn, sql: str, params=()) -> list[tuple]:
    with conn.cursor() as cur:
        cur.execute(sql, params)
        return cur.fetchall()


def oem_steps(conn, start: datetime, end: datetime) -> dict[str, Step]:
    """Per-channel OEM demand step series (2 h pre-roll fixes initial state)."""
    rows = fetch(conn, """
        SELECT ts, get_byte(payload, 0) AS cmd, payload
        FROM raw_frames
        WHERE msg_type = 3 AND length(payload) >= 4
          AND get_byte(payload, 1) = 0
          AND get_byte(payload, 0) IN (100, 101, 102)
          AND ts BETWEEN %s AND %s
        ORDER BY ts""", (start - timedelta(hours=2), end))
    steps: dict[str, Step] = {c: [] for c in CHANNELS}
    for ts, cmd, payload in rows:
        chan, off = OEM_CMDS[cmd]
        if len(payload) <= off:
            continue
        steps[chan].append((ts, payload[off] / 2.0))
    return steps


def shadow_steps(conn, start: datetime, end: datetime
                 ) -> tuple[dict[str, Step], int, datetime | None]:
    rows = fetch(conn, """
        SELECT ts, gas_pct, hp_pct, cool_pct, fan_pct
        FROM shadow_demands
        WHERE ts BETWEEN %s AND %s
        ORDER BY ts""", (start - timedelta(hours=2), end))
    steps: dict[str, Step] = {c: [] for c in CHANNELS}
    first_in_window = None
    n = 0
    for ts, gas, hp, cool, fan in rows:
        # dual fuel: shadow heat channel = whichever heat source it would run
        steps["heat"].append((ts, max(gas or 0.0, hp or 0.0)))
        steps["cool"].append((ts, cool or 0.0))
        steps["fan"].append((ts, fan or 0.0))
        if ts >= start:
            n += 1
            if first_in_window is None:
                first_in_window = ts
    return steps, n, first_in_window


# ------------------------------------------------------------ step-series math
def clip(step: Step, start: datetime, end: datetime) -> Step:
    """Step series restricted to [start, end] with an explicit start point."""
    val = 0.0
    out: Step = []
    for t, v in step:
        if t <= start:
            val = v
        elif t <= end:
            if not out:
                out.append((start, val))
            out.append((t, v))
            val = v
    if not out:
        out.append((start, val))
    return out


def integrate(step: Step, end: datetime, fn) -> float:
    """sum fn(value) * dt(hours) over the clipped series."""
    total = 0.0
    for i, (t, v) in enumerate(step):
        t2 = step[i + 1][0] if i + 1 < len(step) else end
        total += fn(v) * (t2 - t).total_seconds() / 3600.0
    return total


def merged(a: Step, b: Step, end: datetime):
    """Yield (dt_hours, va, vb) over the union of both series' breakpoints."""
    times = sorted({t for t, _ in a} | {t for t, _ in b})
    ia = ib = 0
    va = vb = 0.0
    for i, t in enumerate(times):
        while ia < len(a) and a[ia][0] <= t:
            va = a[ia][1]; ia += 1
        while ib < len(b) and b[ib][0] <= t:
            vb = b[ib][1]; ib += 1
        t2 = times[i + 1] if i + 1 < len(times) else end
        dt_h = (t2 - t).total_seconds() / 3600.0
        if dt_h > 0:
            yield dt_h, va, vb


def cycles(step: Step, end: datetime) -> list[dict]:
    """ON intervals (value > 0) with duration and mean demand."""
    out: list[dict] = []
    cur: dict | None = None
    for i, (t, v) in enumerate(step):
        t2 = step[i + 1][0] if i + 1 < len(step) else end
        if v > 0:
            if cur is None:
                cur = {"start": t, "pct_h": 0.0}
            cur["pct_h"] += v * (t2 - t).total_seconds() / 3600.0
            cur["end"] = t2
        elif cur is not None:
            cur["end"] = t
            out.append(cur)
            cur = None
    if cur is not None:
        cur["open"] = True
        out.append(cur)
    for c in out:
        c["minutes"] = (c["end"] - c["start"]).total_seconds() / 60.0
        c["avg_pct"] = c["pct_h"] / ((c["end"] - c["start"]).total_seconds() / 3600.0 or 1)
    return out


def channel_metrics(oem: Step, sh: Step, end: datetime) -> dict:
    window_h = 0.0
    mean_abs = both_on = both_off = oem_only = sh_only = 0.0
    for dt_h, vo, vs in merged(oem, sh, end):
        window_h += dt_h
        mean_abs += abs(vo - vs) * dt_h
        if vo > 0 and vs > 0:
            both_on += dt_h
        elif vo == 0 and vs == 0:
            both_off += dt_h
        elif vo > 0:
            oem_only += dt_h
        else:
            sh_only += dt_h
    return {
        "mean_abs_pct": mean_abs / window_h if window_h else 0.0,
        "both_on_min": both_on * 60, "both_off_min": both_off * 60,
        "oem_only_min": oem_only * 60, "sh_only_min": sh_only * 60,
        "agree_pct": 100.0 * (both_on + both_off) / window_h if window_h else 0.0,
        "oem_pct_h": integrate(oem, end, lambda v: v),
        "sh_pct_h": integrate(sh, end, lambda v: v),
    }


def pair_starts(oem_c: list[dict], sh_c: list[dict]) -> list[dict]:
    """Pair each shadow cycle start with the nearest OEM start (±60 min)."""
    pairs = []
    for s in sh_c:
        best = None
        for o in oem_c:
            off = (o["start"] - s["start"]).total_seconds() / 60.0
            if abs(off) <= PAIR_WINDOW_MIN and (
                    best is None or abs(off) < abs(best[1])):
                best = (o, off)
        pairs.append({"shadow_start": s["start"],
                      "oem_start": best[0]["start"] if best else None,
                      "offset_min": best[1] if best else None})
    return pairs


# ------------------------------------------------------------------- markdown
def fmt_t(dt: datetime, tz) -> str:
    return dt.astimezone(tz).strftime("%m-%d %H:%M")


def cycle_table(cs: list[dict], tz) -> list[str]:
    if not cs:
        return ["*(no cycles)*"]
    out = ["| # | start | end | minutes | avg demand % |",
           "|---|---|---|---|---|"]
    for i, c in enumerate(cs, 1):
        out.append(f"| {i} | {fmt_t(c['start'], tz)} | "
                   f"{fmt_t(c['end'], tz)}{' (open)' if c.get('open') else ''} | "
                   f"{c['minutes']:.1f} | {c['avg_pct']:.1f} |")
    return out


def build_report(conn, start: datetime, end: datetime) -> str:
    tz = ZoneInfo(config.LOCAL_TZ)
    oem = {c: clip(s, start, end) for c, s in oem_steps(conn, start, end).items()}
    sh_raw, n_shadow, first_shadow = shadow_steps(conn, start, end)
    sh = {c: clip(s, start, end) for c, s in sh_raw.items()}
    (n_frames,) = fetch(conn, "SELECT count(*) FROM raw_frames WHERE ts BETWEEN %s AND %s",
                        (start, end))[0]

    lines = [
        "# Shadow vs OEM demand comparison (#139)",
        "",
        f"- window: **{start.astimezone(tz).isoformat()}** -> "
        f"**{end.astimezone(tz).isoformat()}** ({(end-start).total_seconds()/3600:.1f} h)",
        f"- generated: {datetime.now(tz).isoformat()}",
        f"- data coverage: {n_frames} OEM CT-485 frames, {n_shadow} shadow lines"
        + (f" (first shadow row {fmt_t(first_shadow, tz)})" if first_shadow else
           " — **NO SHADOW DATA IN WINDOW**"),
        "- OEM truth: t03 SET_CONTROL_CMD values (percent*2) from raw_frames; "
        "shadow: [shadow] DemandSet telemetry (TX disabled). Shadow heat "
        "channel = max(gas, hp). Fan offsets are provisional (docs/02 §5a).",
        "",
    ]

    for chan in CHANNELS:
        m = channel_metrics(oem[chan], sh[chan], end)
        oc, sc = cycles(oem[chan], end), cycles(sh[chan], end)
        pairs = pair_starts(oc, sc)
        offs = [p["offset_min"] for p in pairs if p["offset_min"] is not None]
        lines += [
            f"## {chan.upper()} channel",
            "",
            "| metric | value |", "|---|---|",
            f"| time-weighted mean \\|OEM - shadow\\| | {m['mean_abs_pct']:.2f} % |",
            f"| agreement (both on or both off) | {m['agree_pct']:.1f} % of window |",
            f"| OEM ran, shadow idle | {m['oem_only_min']:.0f} min |",
            f"| shadow would have run, OEM idle | {m['sh_only_min']:.0f} min |",
            f"| both demanding | {m['both_on_min']:.0f} min |",
            f"| energy proxy OEM (∫demand%·dt) | {m['oem_pct_h']:.1f} %·h "
            f"(= {m['oem_pct_h']/100:.2f} full-demand hours) |",
            f"| energy proxy shadow | {m['sh_pct_h']:.1f} %·h "
            f"(= {m['sh_pct_h']/100:.2f} full-demand hours) |",
            f"| cycles OEM / shadow | {len(oc)} / {len(sc)} |",
        ]
        if offs:
            lines += [
                f"| cycle-start offset (OEM minus shadow) mean / median | "
                f"{statistics.mean(offs):+.1f} / {statistics.median(offs):+.1f} min |",
                f"| paired starts (±{PAIR_WINDOW_MIN} min) | {len(offs)}/{len(sc)} |",
            ]
        lines += ["", f"### OEM {chan} cycles", ""]
        lines += cycle_table(oc, tz)
        lines += ["", f"### Shadow {chan} cycles", ""]
        lines += cycle_table(sc, tz)
        if offs or sc:
            lines += ["", f"### {chan} cycle-start pairing", "",
                      "| shadow start | nearest OEM start | offset (min, +=OEM later) |",
                      "|---|---|---|"]
            for p in pairs:
                lines.append(
                    f"| {fmt_t(p['shadow_start'], tz)} | "
                    + (fmt_t(p["oem_start"], tz) if p["oem_start"] else "—")
                    + " | "
                    + (f"{p['offset_min']:+.1f}" if p["offset_min"] is not None else "unpaired")
                    + " |")
        lines.append("")

    lines += [
        "---",
        "*Setpoint context (window mean of shadow telemetry):*",
    ]
    row = fetch(conn, """
        SELECT round(avg(fused_temp_c)::numeric, 2), round(avg(set_heat_c)::numeric, 1),
               round(avg(set_cool_c)::numeric, 1),
               count(DISTINCT action), string_agg(DISTINCT action, ', ')
        FROM shadow_demands WHERE ts BETWEEN %s AND %s""", (start, end))
    if row and row[0][0] is not None:
        f_avg, sh_h, sh_c2, _n, actions = row[0]
        lines.append(f"fused T avg {f_avg} C, setH {sh_h} C, setC {sh_c2} C, "
                     f"actions seen: {actions}")
    else:
        lines.append("no shadow telemetry in window")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    logging.basicConfig(level=logging.INFO)
    ap = argparse.ArgumentParser(description="Shadow-vs-OEM report (#139)")
    ap.add_argument("--start", help="window start (ISO8601; default now-24h)")
    ap.add_argument("--hours", type=float, default=24.0)
    ap.add_argument("--out", help="output path (default /reports/shadow-vs-oem-<date>.md)")
    args = ap.parse_args(argv)

    if args.start:
        start = datetime.fromisoformat(args.start.replace("Z", "+00:00"))
        if start.tzinfo is None:
            start = start.replace(tzinfo=timezone.utc)
    else:
        start = datetime.now(timezone.utc) - timedelta(hours=args.hours)
    end = min(start + timedelta(hours=args.hours), datetime.now(timezone.utc))

    out = Path(args.out or
               f"/reports/shadow-vs-oem-{start.astimezone(ZoneInfo(config.LOCAL_TZ)):%Y%m%d}.md")
    conn = psycopg.connect(autocommit=True)  # PG* env
    report = build_report(conn, start, end)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(report)
    log.info("report written to %s (%d bytes)", out, len(report))
    print(report[:2000])
    return 0


if __name__ == "__main__":
    sys.exit(main())
