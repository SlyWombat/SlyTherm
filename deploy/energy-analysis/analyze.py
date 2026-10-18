#!/usr/bin/env python3
"""
energy-analysis — monthly grid-energy report.

Reads the energy-weather-log SQLite DB (read-only), computes a monthly rollup
with year-over-year comparison plus a temperature-vs-energy view, asks a local
Ollama model to write a plain-English report, and emails it through the house
mail service (house-mail, #218). Since #220 it also reads the logger's
ecoflow_savings table (EcoFlow Ultra ULO arbitrage + solar, per tariff) so the
report can see solar, battery flows and dollars saved, and so deliberate ULO
grid-charging is never flagged as a high-usage day. Pure stdlib.

Modes:
  --once          run now for the most recent COMPLETE month, then exit
  --month YYYY-MM run for a specific month, then exit (testing/backfill)
  --check-mail    verify the house-mail key and grant, then exit
  --dry-run       build the report (with --month or for the last complete month),
                  print the stats JSON and the narrative, send nothing, exit
  (no flag)       daemon: verify house-mail, then sleep until the 1st of next
                  month at RUN_AT, run for the month that just ended, repeat.

Exit status is non-zero whenever the report could not be sent. The old mail
bridge failed silently for weeks; this job now fails where `docker ps` and the
container logs show it. The report text is still printed to the log on a mail
failure so it is never lost.
"""
import os, sys, json, sqlite3, time, urllib.request, urllib.error, calendar
from datetime import datetime, date
from zoneinfo import ZoneInfo

TZ           = os.environ.get("TZ", "America/Toronto")
DB_PATH      = os.environ.get("DB_PATH", "/db/daily.db")
OLLAMA_URL   = os.environ.get("OLLAMA_URL", "http://localhost:11434").rstrip("/")
MODEL        = os.environ.get("MODEL", "gemma4:31b")
KEEP_ALIVE   = os.environ.get("OLLAMA_KEEP_ALIVE", "0")   # unload after run -> free VRAM for Immich
# house-mail: base URL of the service and the path of the mounted 0600 key file.
# The key is read at call time (a rotation needs no restart) and is never logged.
HOUSE_MAIL_URL      = os.environ.get("HOUSE_MAIL_URL", "http://192.168.10.11:3510").rstrip("/")
HOUSE_MAIL_KEY_FILE = os.environ.get("HOUSE_MAIL_KEY_FILE", "/run/secrets/house-mail.key")
MAIL_TO      = os.environ.get("MAIL_TO", "dave@drscapital.com")
RUN_AT       = os.environ.get("RUN_AT", "06:00")          # local HH:MM on the 1st
RUN_ON_START = os.environ.get("RUN_ON_START", "false").lower() == "true"
ZONE = ZoneInfo(TZ)


def log(*a):
    print(datetime.now(ZONE).isoformat(timespec="seconds"), *a, flush=True)


def post_json(url, payload, timeout):
    data = json.dumps(payload).encode()
    req = urllib.request.Request(
        url, data=data, headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode()


# ---- mail (house-mail) -----------------------------------------------------

class MailError(Exception):
    """The report could not be sent. The message never contains the key."""


def house_mail_key():
    try:
        with open(HOUSE_MAIL_KEY_FILE) as f:
            key = f.read().strip()
    except OSError as e:
        raise MailError(f"house-mail key unreadable at {HOUSE_MAIL_KEY_FILE}: {e.strerror}") from None
    if not key:
        raise MailError(f"house-mail key file {HOUSE_MAIL_KEY_FILE} is empty")
    return key


def house_mail(method, path, payload=None, timeout=60):
    """One authenticated call to house-mail; returns the decoded JSON body."""
    headers = {"Authorization": f"Bearer {house_mail_key()}"}
    data = None
    if payload is not None:
        headers["Content-Type"] = "application/json"
        data = json.dumps(payload).encode()
    req = urllib.request.Request(f"{HOUSE_MAIL_URL}{path}", data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        detail = e.read().decode(errors="replace")[:300]
        raise MailError(f"house-mail {method} {path} -> HTTP {e.code}: {detail}") from None
    except (urllib.error.URLError, OSError, ValueError) as e:
        raise MailError(f"house-mail {method} {path} failed: {e}") from None


def send_mail(subject, body):
    # /send is house-mail's notify-style shim: {title, message, target?}. target is
    # explicit here even though the key carries default_to = MAIL_TO.
    house_mail("POST", "/send", {"title": subject, "message": body, "target": MAIL_TO})


def check_mail():
    """Startup self-test: the key file must be readable and the key must hold
    mail.send. A bad key or grant therefore shows up today as a container that
    will not stay up, instead of as a report that quietly never arrives."""
    who = house_mail("GET", "/v1/whoami", timeout=15)
    who = who.get("result", who)
    grants = who.get("grants") or []
    if not any("mail.send" in (g.get("caps") or []) for g in grants):
        raise MailError(f"house-mail key '{who.get('client', '?')}' has no mail.send grant")
    log(f"house-mail ok: key '{who.get('client')}' may send; reports go to {MAIL_TO}")


# ---- data ----------------------------------------------------------------

def connect():
    # The DB is WAL-mode and held open by energy-weather-log, so SQLite needs to
    # touch the -wal/-shm sidecars: open read-write, then hard-enforce read-only
    # at the connection level. We only ever SELECT.
    con = sqlite3.connect(DB_PATH, timeout=30)
    con.execute("PRAGMA query_only=ON")
    return con


def month_rows(con, y, m):
    s = date(y, m, 1).isoformat()
    e = (date(y + 1, 1, 1) if m == 12 else date(y, m + 1, 1)).isoformat()
    return con.execute(
        "SELECT date,temp_min_c,temp_max_c,temp_mean_c,grid_kwh,complete "
        "FROM daily WHERE date>=? AND date<? ORDER BY date", (s, e)).fetchall()


def summarize(rows):
    kwh   = [r[4] for r in rows if r[4] is not None]
    highs = [r[2] for r in rows if r[2] is not None]
    lows  = [r[1] for r in rows if r[1] is not None]
    means = [r[3] for r in rows if r[3] is not None]
    avg = lambda xs: round(sum(xs) / len(xs), 2) if xs else None
    return {
        "days": len(rows),
        "complete_days": sum(1 for r in rows if r[5] == 1),
        "total_kwh": round(sum(kwh), 1) if kwh else None,
        "avg_daily_kwh": avg(kwh),
        "avg_high_c": round(sum(highs) / len(highs), 1) if highs else None,
        "avg_low_c": round(sum(lows) / len(lows), 1) if lows else None,
        "avg_mean_c": round(sum(means) / len(means), 1) if means else None,
    }


def linreg(xs, ys):
    """Least-squares slope/intercept/pearson-r; None if not enough/degenerate."""
    n = len(xs)
    if n < 3:
        return None
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    if sxx == 0:
        return None
    slope = sxy / sxx
    r = sxy / ((sxx * syy) ** 0.5) if syy > 0 else 0.0
    return slope, my - slope * mx, r


TARIFFS = ["ulo", "weekend", "mid", "onpeak"]


def has_savings(con):
    return con.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='ecoflow_savings'").fetchone() is not None


def battery_month(con, y, m):
    """Month totals from energy-weather-log's ecoflow_savings table (EcoFlow Ultra
    ULO arbitrage + solar, one row per day). None if the table or the month's rows
    are absent."""
    if not has_savings(con):
        return None
    s = date(y, m, 1).isoformat()
    e = (date(y + 1, 1, 1) if m == 12 else date(y, m + 1, 1)).isoformat()
    cols = ([f"disch_{t}_kwh" for t in TARIFFS] + [f"chg_{t}_kwh" for t in TARIFFS]
            + ["solar_kwh", "discharge_kwh", "grid_charge_kwh", "value_out", "charge_cost", "saved"])
    row = con.execute(
        f"SELECT COUNT(*), {', '.join('SUM(' + c + ')' for c in cols)} "
        "FROM ecoflow_savings WHERE complete=1 AND date>=? AND date<?", (s, e)).fetchone()
    if not row or not row[0]:
        return None
    sums = dict(zip(cols, row[1:]))
    rates = con.execute(
        "SELECT rate_ulo, rate_weekend, rate_mid, rate_onpeak FROM ecoflow_savings "
        "WHERE date>=? AND date<? ORDER BY date DESC LIMIT 1", (s, e)).fetchone()
    r1 = lambda v: round(v, 1) if v is not None else None
    r2 = lambda v: round(v, 2) if v is not None else None
    return {
        "days_covered": row[0],
        "solar_kwh": r1(sums["solar_kwh"]),
        "battery_discharge_kwh": r1(sums["discharge_kwh"]),
        "battery_discharge_kwh_by_tariff": {t: r1(sums[f"disch_{t}_kwh"]) for t in TARIFFS},
        "battery_grid_charge_kwh": r1(sums["grid_charge_kwh"]),
        "battery_grid_charge_kwh_by_tariff": {t: r1(sums[f"chg_{t}_kwh"]) for t in TARIFFS},
        "rates_cad_per_kwh": dict(zip(TARIFFS, rates)) if rates else None,
        "value_of_displaced_grid_cad": r2(sums["value_out"]),
        "grid_charge_cost_cad": r2(sums["charge_cost"]),
        "net_saving_cad": r2(sums["saved"]),
    }


def build_stats(con, y, m):
    rows = month_rows(con, y, m)
    if not rows:
        return None
    cur = summarize(rows)
    month_name = calendar.month_name[m]

    # year-over-year: same calendar month, every prior year on record
    yoy = []
    for py in range(y - 1, 2010, -1):
        pr = month_rows(con, py, m)
        if not pr:
            continue
        ps = summarize(pr)
        delta_pct = None
        if ps["total_kwh"] and cur["total_kwh"]:
            delta_pct = round(100 * (cur["total_kwh"] - ps["total_kwh"]) / ps["total_kwh"], 1)
        yoy.append({"year": py, "total_kwh": ps["total_kwh"],
                    "avg_high_c": ps["avg_high_c"], "avg_mean_c": ps["avg_mean_c"],
                    "vs_this_year_pct": delta_pct})

    # monthly temp-vs-energy fit across all complete months that have temp,
    # to estimate "expected kWh for this month's temperature".
    mrows = con.execute(
        "SELECT strftime('%Y-%m',date) ym, AVG(temp_mean_c), SUM(grid_kwh), COUNT(*) "
        "FROM daily WHERE complete=1 AND temp_mean_c IS NOT NULL "
        "GROUP BY ym HAVING COUNT(*) >= 25").fetchall()
    temp_energy = None
    if cur["avg_mean_c"] is not None:
        xs = [r[1] for r in mrows]
        ys = [r[2] for r in mrows]
        fit = linreg(xs, ys)
        if fit:
            slope, intercept, r = fit
            expected = slope * cur["avg_mean_c"] + intercept
            dev = None
            if cur["total_kwh"] and expected:
                dev = round(100 * (cur["total_kwh"] - expected) / expected, 1)
            temp_energy = {
                "months_in_fit": len(mrows),
                "kwh_per_degC": round(slope, 1),
                "pearson_r": round(r, 2),
                "expected_kwh_for_this_temp": round(expected, 1),
                "actual_vs_expected_pct": dev,
            }

    # EcoFlow / solar / tariff view from ecoflow_savings (one row per day since
    # 2026-07-15). None when the month has no rows, so older months and the
    # year-over-year years simply have no such section. (#220)
    battery = battery_month(con, y, m)
    if battery and cur["total_kwh"] is not None and battery["battery_grid_charge_kwh"] is not None:
        net_kwh = round(cur["total_kwh"] - battery["battery_grid_charge_kwh"], 1)
        cur["grid_kwh_net_of_battery_charge"] = net_kwh
        # The temperature fit is a household-load model (most of its months
        # pre-date the battery), so the fair comparison is against net kWh.
        if temp_energy and temp_energy["expected_kwh_for_this_temp"]:
            exp = temp_energy["expected_kwh_for_this_temp"]
            temp_energy["net_of_battery_charge_vs_expected_pct"] = round(100 * (net_kwh - exp) / exp, 1)

    # daily anomalies within the month: high kWh relative to temperature,
    # judged against a trailing-365-day daily fit. Energy the battery pulled from
    # the grid that day (deliberate ULO charging) is subtracted first, so a cheap
    # overnight charge is never flagged as a heavy-appliance day. (#220)
    anomalies = []
    if has_savings(con):
        drows = con.execute(
            "SELECT d.date, d.temp_mean_c, d.grid_kwh, COALESCE(s.grid_charge_kwh, 0) "
            "FROM daily d LEFT JOIN ecoflow_savings s ON s.date = d.date "
            "WHERE d.complete=1 AND d.temp_mean_c IS NOT NULL AND d.grid_kwh IS NOT NULL "
            "AND d.date >= date(?, '-365 days')", (date(y, m, 1).isoformat(),)).fetchall()
    else:
        drows = [(d, t, k, 0.0) for d, t, k in con.execute(
            "SELECT date,temp_mean_c,grid_kwh FROM daily "
            "WHERE complete=1 AND temp_mean_c IS NOT NULL AND grid_kwh IS NOT NULL "
            "AND date >= date(?, '-365 days')", (date(y, m, 1).isoformat(),)).fetchall()]
    fit = linreg([r[1] for r in drows], [r[2] - r[3] for r in drows]) if drows else None
    if fit:
        slope, intercept, _ = fit
        resid = []
        for d, t, k, chg in drows:
            if d.startswith(f"{y:04d}-{m:02d}"):
                net = k - chg
                resid.append((round(net - (slope * t + intercept), 1), d, t, k, chg))
        resid.sort(reverse=True)
        for r_, d, t, k, chg in resid[:2]:
            if r_ > 0:
                a = {"date": d, "grid_kwh": k, "mean_temp_c": t, "kwh_above_expected": r_}
                if chg:
                    a["battery_grid_charge_kwh"] = round(chg, 1)
                    a["grid_kwh_net_of_battery_charge"] = round(k - chg, 1)
                anomalies.append(a)

    notes = ("grid_kwh = whole-home grid energy (Emporia Vue), INCLUDING any energy the "
             "EcoFlow battery drew from the grid; temps from Ecowitt outdoor sensor "
             "(only available from 2025-06 onward).")
    if battery:
        notes += (" battery_solar_tariff covers the EcoFlow Ultra and rooftop solar: the "
                  "battery is charged on purpose at the cheap ULO rate (and from solar) and "
                  "discharged to displace on-peak grid energy; battery_grid_charge_kwh is "
                  "cheap, intentional consumption, not waste. notable_high_days are judged "
                  "on grid kWh net of that charging. Earlier years have no battery/solar "
                  "data, so the year_over_year comparison is raw grid kWh only.")
    return {
        "report_for": f"{month_name} {y}",
        "this_month": cur,
        "year_over_year": yoy,
        "temperature_vs_energy": temp_energy,
        "notable_high_days": anomalies,
        "battery_solar_tariff": battery,
        "data_notes": notes,
    }


# ---- report --------------------------------------------------------------

SYSTEM = (
    "You are a home-energy analyst writing a short monthly report for a homeowner. "
    "Use ONLY the numbers in the JSON provided; never invent figures. Write 150-250 "
    "words of clear, friendly plain text (no markdown headings, no bullet symbols). "
    "Cover: the month's total grid energy and how it compares year-over-year; how "
    "energy tracked with outdoor temperature (use the temperature_vs_energy fit if "
    "present, noting whether usage was higher or lower than expected for the "
    "temperature; when net_of_battery_charge_vs_expected_pct is present, judge the "
    "household on THAT figure and say plainly how much of any excess was the battery "
    "charging); any notable high-usage days; and, when battery_solar_tariff is "
    "present, a short paragraph on the battery and solar: solar kWh, how much the "
    "battery discharged to displace on-peak grid energy, how much it charged from the "
    "grid at the cheap ULO rate, and the net dollar saving. Read data_notes: grid "
    "charging of the battery is deliberate cheap energy, never call it waste or a "
    "spike, and never suggest investigating it. Finish with one practical, "
    "non-preachy observation. If a value is null or a section is absent, simply omit "
    "it rather than guessing."
)


def text_figures(stats):
    """Readable plain-text figures — used only as a fallback if Ollama is down."""
    t = stats["this_month"]
    lines = [f"Figures for {stats['report_for']}:",
             f"  Total grid energy: {t['total_kwh']} kWh over {t['days']} days "
             f"(avg {t['avg_daily_kwh']}/day)"]
    if t["avg_high_c"] is not None:
        lines.append(f"  Outdoor temp: avg high {t['avg_high_c']}°C, "
                     f"avg low {t['avg_low_c']}°C, mean {t['avg_mean_c']}°C")
    for y in stats["year_over_year"]:
        # vs_this_year_pct is (this year - that year) / that year, so read it as
        # "this year is N% above/below <year>", not the other way round.
        d = f" (this year {y['vs_this_year_pct']:+}% vs {y['year']})" if y["vs_this_year_pct"] is not None else ""
        lines.append(f"  {y['year']}: {y['total_kwh']} kWh{d}")
    te = stats["temperature_vs_energy"]
    if te and te["actual_vs_expected_pct"] is not None:
        net = (f"; {te['net_of_battery_charge_vs_expected_pct']:+}% net of battery charging"
               if te.get("net_of_battery_charge_vs_expected_pct") is not None else "")
        lines.append(f"  Expected for this temperature: {te['expected_kwh_for_this_temp']} kWh "
                     f"({te['actual_vs_expected_pct']:+}% vs expected{net})")
    for a in stats["notable_high_days"]:
        net = (f", {a['grid_kwh_net_of_battery_charge']} kWh net of {a['battery_grid_charge_kwh']} kWh battery charging"
               if "battery_grid_charge_kwh" in a else "")
        lines.append(f"  High day {a['date']}: {a['grid_kwh']} kWh at {a['mean_temp_c']}°C "
                     f"({a['kwh_above_expected']:+} kWh vs expected{net})")
    b = stats.get("battery_solar_tariff")
    if b:
        lines.append(f"  Battery/solar ({b['days_covered']} days): solar {b['solar_kwh']} kWh; "
                     f"battery discharged {b['battery_discharge_kwh']} kWh "
                     f"(on-peak {b['battery_discharge_kwh_by_tariff']['onpeak']}), "
                     f"grid-charged {b['battery_grid_charge_kwh']} kWh "
                     f"(ULO {b['battery_grid_charge_kwh_by_tariff']['ulo']})")
        if b["net_saving_cad"] is not None:
            lines.append(f"  Arbitrage: displaced ${b['value_of_displaced_grid_cad']} of grid energy "
                         f"for ${b['grid_charge_cost_cad']} of charging, net saving ${b['net_saving_cad']}")
        if t.get("grid_kwh_net_of_battery_charge") is not None:
            lines.append(f"  Grid energy net of battery charging: {t['grid_kwh_net_of_battery_charge']} kWh")
    return "\n".join(lines)


def write_report(stats):
    payload = {
        "model": MODEL,
        "stream": False,
        "keep_alive": KEEP_ALIVE,
        # num_ctx: the prompt is a few KB of JSON. Ollama's default 32k context
        # on the 72B model asks for a 9 GB KV cache, which failed with
        # out-of-memory on 2026-10-01 and 2026-10-18 while other models were
        # resident on the GPU (HTTP 500, figures-only fallback). 8k is plenty.
        "options": {"temperature": 0.3, "num_ctx": 8192},
        "messages": [
            {"role": "system", "content": SYSTEM},
            {"role": "user", "content": json.dumps(stats)},
        ],
    }
    out = post_json(f"{OLLAMA_URL}/api/chat", payload, timeout=900)
    return json.loads(out)["message"]["content"].strip()


def run_for(y, m, dry_run=False):
    """Build and send the report for one month. Returns True when the mail went out
    (or, with dry_run, when the report was built and printed instead of sent)."""
    with connect() as con:
        stats = build_stats(con, y, m)
    if not stats or stats["this_month"]["total_kwh"] is None:
        log(f"no usable data for {y}-{m:02d}; skipping")
        return True
    try:
        body = write_report(stats)
        log(f"ollama report generated ({len(body)} chars)")
    except Exception as e:
        log(f"WARN ollama failed ({e}); sending figures only")
        body = f"(Automated report unavailable: {e})\n\n{text_figures(stats)}"

    subject = f"Energy report — {stats['report_for']}"
    if dry_run:
        print("--- stats ---\n" + json.dumps(stats, indent=1, ensure_ascii=False), flush=True)
        print(f"--- {subject} (NOT sent: --dry-run) ---\n{body}\n--- figures fallback would read ---\n"
              + text_figures(stats), flush=True)
        return True
    try:
        send_mail(subject, body)
        log(f"emailed '{subject}' to {MAIL_TO} via house-mail")
        return True
    except MailError as e:
        log(f"ERROR {e}")
        log("report body follows so it is not lost:")
        print(body, flush=True)
        return False


def prev_complete_month(now):
    y, m = now.year, now.month
    return (y - 1, 12) if m == 1 else (y, m - 1)


def next_run(now):
    hh, mm = map(int, RUN_AT.split(":"))
    # if it's already the 1st and we haven't passed RUN_AT, fire today (so a
    # restart on the 1st before RUN_AT doesn't skip the month's report).
    todays = now.replace(day=1, hour=hh, minute=mm, second=0, microsecond=0)
    if now.day == 1 and now < todays:
        return todays
    ny, nm = (now.year + 1, 1) if now.month == 12 else (now.year, now.month + 1)
    return datetime(ny, nm, 1, hh, mm, tzinfo=ZONE)


def main():
    if "--check-mail" in sys.argv:
        try:
            check_mail()
        except MailError as e:
            log(f"ERROR {e}")
            sys.exit(2)
        return
    dry = "--dry-run" in sys.argv
    if "--month" in sys.argv:
        ym = sys.argv[sys.argv.index("--month") + 1]
        y, m = map(int, ym.split("-"))
        sys.exit(0 if run_for(y, m, dry) else 1)
    if "--once" in sys.argv or dry:
        y, m = prev_complete_month(datetime.now(ZONE))
        sys.exit(0 if run_for(y, m, dry) else 1)

    log(f"daemon up; model={MODEL} db={DB_PATH} mail->{MAIL_TO} via {HOUSE_MAIL_URL}")
    try:
        check_mail()
    except MailError as e:
        # Refuse to idle for a month on a key that cannot send. With
        # restart: unless-stopped this is a visible restart loop, not silence.
        log(f"ERROR {e}")
        sys.exit(2)
    if RUN_ON_START:
        y, m = prev_complete_month(datetime.now(ZONE))
        if not run_for(y, m):
            sys.exit(1)
    while True:
        now = datetime.now(ZONE)
        nxt = next_run(now)
        secs = (nxt - now).total_seconds()
        log(f"next run {nxt.isoformat(timespec='minutes')} (in {secs/3600:.1f}h)")
        time.sleep(max(60, secs))
        y, m = prev_complete_month(datetime.now(ZONE))
        if not run_for(y, m):
            # Exit so the failure is visible (restart count, logs); the restart
            # re-runs check_mail() and then waits for the next month.
            sys.exit(1)


if __name__ == "__main__":
    main()
