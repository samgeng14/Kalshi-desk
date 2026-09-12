#!/usr/bin/env python3
"""WX-SETTLE-WATCH: the standing check that Kalshi's settled value is the NWS climate maximum.

Three numbers exist for one station-day and the desk needs all three:

  metar_max   the running maximum of the hourly observation series - what weather_nws itself
              sees, and therefore what its estimate is anchored to.
  cli_max     the NWS daily climate product maximum (CLIAUS, CLIMDW, ...), taken from the ASOS
              record over the local calendar day. This is the location Kalshi's rules name.
  kalshi      the value Kalshi actually settled on, via The Weather Company since 2026-08-27.
              It is never observed directly: the ladder pins it to an interval of whole degrees,
              which is enough to test agreement even though it is not enough to fit against.

cli_max - metar_max is the settlement shift the model must add (docs/settlement.md, section 10 of
the weather brief). Whether cli_max falls inside the Kalshi interval is the TWC/NWS equivalence
test, and clearing it is gate A for weather ever trading unattended.

Why this has to run on a schedule rather than on demand: api.weather.gov keeps roughly a
fortnight of CLI products per location. A station-day not collected inside that window is gone.
The existing evidence is six agreements on a single date, which is an anecdote.

Writes to its own database, never to the desk's. Reads prod.sqlite read-only. Idempotent: run it
as often as you like; a later CLI issuance replaces an earlier one and the revision is counted.
"""
import argparse, collections, datetime as dt, json, os, re, sqlite3, sys, time, urllib.error, urllib.request
from zoneinfo import ZoneInfo

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
UA = "kalshi-desk-research (samgeng14@gmail.com)"

# series -> (CLI location, METAR station, IANA zone, low?). Mirrors settlement_specs() in
# src/universe.cpp; a series with no CLI product there has none here either.
STATIONS = {
    "KXHIGHAUS":  ("AUS", "KAUS", "America/Chicago",     False),
    "KXHIGHCHI":  ("MDW", "KMDW", "America/Chicago",     False),
    "KXHIGHNY":   ("NYC", "KNYC", "America/New_York",    False),
    "KXHIGHMIA":  ("MIA", "KMIA", "America/New_York",    False),
    "KXHIGHPHIL": ("PHL", "KPHL", "America/New_York",    False),
    "KXHIGHLAX":  ("LAX", "KLAX", "America/Los_Angeles", False),
    "KXLOWTCHI":  ("MDW", "KMDW", "America/Chicago",     True),
    "KXLOWTMIN":  ("MSP", "KMSP", "America/Chicago",     True),
}
MONTHS = ["JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY", "AUGUST",
          "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"]

SCHEMA = """
CREATE TABLE IF NOT EXISTS wx_cli_issuance(
  cli_loc TEXT, local_date TEXT, issuance_ts TEXT, local_hour INTEGER,
  next_day INTEGER,        -- issued after the local day ended: this is a final, not a running, value
  value_max INTEGER, value_min INTEGER, corrected INTEGER, recorded_ts INTEGER,
  PRIMARY KEY(cli_loc, local_date, issuance_ts));
CREATE TABLE IF NOT EXISTS wx_settlement(
  series TEXT, local_date TEXT, cli_loc TEXT, station TEXT, low INTEGER,
  metar_extreme REAL, metar_obs INTEGER,
  cli_extreme INTEGER, cli_issuance TEXT, cli_revisions INTEGER,
  cli_complete INTEGER,    -- the latest issuance landed after the local day ended
  kalshi_lo INTEGER, kalshi_hi INTEGER, kalshi_event TEXT, kalshi_ticker TEXT,
  agrees INTEGER,          -- CLI value inside the Kalshi interval; NULL when a side is missing
  shift REAL,              -- cli_extreme - metar_extreme, the correction the model owes
  recorded_ts INTEGER,
  PRIMARY KEY(series, local_date));
CREATE TABLE IF NOT EXISTS wx_settlement_log(
  ts INTEGER, series TEXT, local_date TEXT, what TEXT, detail TEXT);
"""


def get(url, timeout=30):
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "application/geo+json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)


def cli_issuances(loc, tz, limit):
    """Every CLI issuance for this location, as (local_date -> [(issuance, hour, next_day, max, min, corrected)]).

    The NWS does not issue one climate product per day. It issues several, each carrying the
    daily extreme SO FAR from the station's 5-minute record: a morning one that has only seen the
    night, one in the late afternoon, sometimes another an hour later, and a final one after local
    midnight. They are not corrections - none of them is flagged CORRECTED - they are a running
    tally, and only the issuance after the local day has ended is the complete daily value.

    This matters more than it looks. The afternoon issuance carries the number Kalshi settles on,
    from the same ASOS record, hours before the market closes at 01:00 local. It is a strictly
    better observation than the hourly METAR series the model reads today, because the 5-minute
    record cannot miss a peak between reports.
    """
    out = collections.defaultdict(list)
    try:
        graph = get("https://api.weather.gov/products/types/CLI/locations/%s" % loc).get("@graph", [])
    except Exception as e:
        print("  %s: CLI product list failed: %s" % (loc, e), file=sys.stderr)
        return out
    zone = ZoneInfo(tz)
    for item in graph[:limit]:
        try:
            text = get("https://api.weather.gov/products/" + item["id"]).get("productText", "")
        except Exception:
            continue
        m = re.search(r"SUMMARY FOR (\w+) (\d+) (\d{4})", text.upper())
        if not m or m.group(1) not in MONTHS:
            continue
        vmax = re.search(r"^\s*MAXIMUM\s+(-?\d+)", text, re.M)
        vmin = re.search(r"^\s*MINIMUM\s+(-?\d+)", text, re.M)
        d = dt.date(int(m.group(3)), MONTHS.index(m.group(1)) + 1, int(m.group(2)))
        stamp = item.get("issuanceTime", "")
        if not stamp:
            continue
        local = dt.datetime.fromisoformat(stamp).astimezone(zone)
        out[d.isoformat()].append((stamp, local.hour, 1 if local.date() > d else 0,
                                   int(vmax.group(1)) if vmax else None,
                                   int(vmin.group(1)) if vmin else None,
                                   1 if "CORRECTED" in text.upper()[:600] else 0))
    for d in out:
        out[d].sort()
    return out


def final_from(issuances, low):
    """(value, issuance, count, complete). Complete only when an issuance landed after the local
    day ended: a day still running has a running maximum, and storing that as settled would be a
    forecast recorded as an observation."""
    if not issuances:
        return None, "", 0, 0
    idx = 4 if low else 3
    done = [i for i in issuances if i[2] == 1 and i[idx] is not None]
    pick = done[-1] if done else [i for i in issuances if i[idx] is not None][-1:]
    if not done:
        if not pick:
            return None, "", len(issuances), 0
        p = pick[0]
        return p[idx], p[0], len(issuances), 0
    p = done[-1]
    return p[idx], p[0], len(issuances), 1


def metar_extremes(station, tz, days, low):
    """local date -> (extreme_F, observation count), as the desk's own reader would compute it."""
    end = dt.datetime.now(dt.timezone.utc)
    start = end - dt.timedelta(days=days + 1)
    url = ("https://api.weather.gov/stations/%s/observations?start=%s&end=%s&limit=500"
           % (station, start.strftime("%Y-%m-%dT%H:%M:%SZ"), end.strftime("%Y-%m-%dT%H:%M:%SZ")))
    # api.weather.gov rejects limit>500 outright with a 400, which is why the first run of this
    # collector silently recorded no observation at all for any station.
    try:
        feats = get(url, timeout=45).get("features", [])
    except Exception as e:
        print("  %s: observations failed: %s" % (station, e), file=sys.stderr)
        return {}
    ext, n = {}, collections.Counter()
    for f in feats:
        p = f.get("properties", {})
        t = (p.get("temperature") or {}).get("value")
        if t is None:
            continue
        stamp = dt.datetime.fromisoformat(p["timestamp"].replace("Z", "+00:00")).astimezone(ZoneInfo(tz))
        d = stamp.date().isoformat()
        deg = t * 9 / 5 + 32
        ext[d] = min(ext[d], deg) if (low and d in ext) else (max(ext.get(d, -999.0), deg) if not low else deg)
        n[d] += 1
    return {d: (v, n[d]) for d, v in ext.items()}


def kalshi_intervals(prod_db):
    """(series, local_date) -> (lo, hi, event, winning ticker) in whole degrees.

    The settled value is interval-censored: one bracket settles YES and pins it to that bracket's
    integers. An open end pins one side only, which is still a usable containment test. An event
    without exactly one YES is skipped rather than guessed at - a partially recorded ladder would
    otherwise silently widen the interval until every CLI value 'agrees'.
    """
    out = {}
    con = sqlite3.connect("file:%s?mode=ro" % prod_db, uri=True)
    con.row_factory = sqlite3.Row
    rows = con.execute(
        "SELECT ticker,event_ticker,series_ticker,result,strike_type,floor_strike,cap_strike"
        " FROM markets WHERE result IN ('yes','no') AND ("
        " series_ticker LIKE 'KXHIGH%' OR series_ticker LIKE 'KXLOW%' OR series_ticker='KXDENHIGH')")
    per = collections.defaultdict(list)
    for r in rows:
        per[r["event_ticker"]].append(dict(r))
    for ev, ms in per.items():
        yes = [m for m in ms if m["result"] == "yes"]
        if len(yes) != 1 or len(ms) < 3:
            continue
        w = yes[0]
        date = event_date(ev)
        if not date:
            continue
        lo = hi = None
        if w["strike_type"] == "between" and w["floor_strike"] is not None and w["cap_strike"] is not None:
            lo, hi = int(round(w["floor_strike"])), int(round(w["cap_strike"]))
        elif w["strike_type"] == "greater" and w["floor_strike"] is not None:
            lo, hi = int(round(w["floor_strike"])) + 1, None     # "greater than F"
        elif w["strike_type"] == "less" and w["cap_strike"] is not None:
            lo, hi = None, int(round(w["cap_strike"])) - 1       # "less than C"
        else:
            continue
        out[(w["series_ticker"], date)] = (lo, hi, ev, w["ticker"])
    return out


def event_date(ev):
    """KXHIGHLAX-26SEP11 -> 2026-09-11."""
    m = re.search(r"-(\d{2})([A-Z]{3})(\d{2})$", ev)
    if not m:
        return ""
    mon = ["JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"]
    if m.group(2) not in mon:
        return ""
    return "20%s-%02d-%s" % (m.group(1), mon.index(m.group(2)) + 1, m.group(3))


def inside(v, lo, hi):
    if v is None:
        return None
    if lo is not None and v < lo:
        return 0
    if hi is not None and v > hi:
        return 0
    return 1


def collect(a):
    db = sqlite3.connect(a.out_db)
    db.executescript(SCHEMA)
    intervals = kalshi_intervals(a.db)
    now = int(time.time())
    wrote = 0
    for series, (loc, station, tz, low) in STATIONS.items():
        iss = cli_issuances(loc, tz, a.products)
        obs = metar_extremes(station, tz, a.days, low)
        for d, lst in iss.items():
            for stamp, hour, nextday, vmax, vmin, corr in lst:
                db.execute("INSERT OR REPLACE INTO wx_cli_issuance VALUES(?,?,?,?,?,?,?,?,?)",
                           (loc, d, stamp, hour, nextday, vmax, vmin, corr, now))
        dates = set(iss) | set(obs) | {x for (sx, x) in intervals if sx == series}
        for d in sorted(dates):
            cv, ciss, cnt, complete = final_from(iss.get(d, []), low)
            mv, mn = obs.get(d, (None, 0))
            lo, hi, ev, tick = intervals.get((series, d), (None, None, "", ""))
            # A day the station barely reported is not a measurement of the gap: the hourly
            # maximum of four observations says almost nothing about the peak it missed.
            if mv is not None and mn < a.min_obs:
                mv = None
            # Only a completed CLI may be compared with anything. A running maximum recorded as
            # a settled value would make the model look right about a day that had not finished.
            ag = inside(cv, lo, hi) if (complete and (lo is not None or hi is not None)) else None
            shift = (cv - mv) if (complete and cv is not None and mv is not None) else None
            db.execute(
                "INSERT INTO wx_settlement VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
                " ON CONFLICT(series,local_date) DO UPDATE SET"
                " metar_extreme=COALESCE(excluded.metar_extreme,metar_extreme),"
                " metar_obs=MAX(excluded.metar_obs,metar_obs),"
                " cli_extreme=COALESCE(excluded.cli_extreme,cli_extreme),"
                " cli_issuance=CASE WHEN excluded.cli_issuance>cli_issuance THEN excluded.cli_issuance ELSE cli_issuance END,"
                " cli_revisions=MAX(excluded.cli_revisions,cli_revisions),"
                " cli_complete=MAX(excluded.cli_complete,cli_complete),"
                " kalshi_lo=COALESCE(excluded.kalshi_lo,kalshi_lo),"
                " kalshi_hi=COALESCE(excluded.kalshi_hi,kalshi_hi),"
                " kalshi_event=COALESCE(NULLIF(excluded.kalshi_event,''),kalshi_event),"
                " kalshi_ticker=COALESCE(NULLIF(excluded.kalshi_ticker,''),kalshi_ticker),"
                " agrees=COALESCE(excluded.agrees,agrees),"
                " shift=COALESCE(excluded.shift,shift),"
                " recorded_ts=excluded.recorded_ts",
                (series, d, loc, station, int(low), mv, mn, cv, ciss, cnt, complete,
                 lo, hi, ev, tick, ag, shift, now))
            if ag == 0:
                db.execute("INSERT INTO wx_settlement_log VALUES(?,?,?,?,?)",
                           (now, series, d, "disagree",
                            "CLI %s outside Kalshi [%s, %s] (%s)" % (cv, lo, hi, tick)))
            wrote += 1
    db.commit()
    print("collected %d station-days into %s" % (wrote, a.out_db))


def report(a):
    db = sqlite3.connect(a.out_db)
    db.executescript(SCHEMA)
    db.row_factory = sqlite3.Row
    rows = [dict(r) for r in db.execute("SELECT * FROM wx_settlement ORDER BY local_date, series")]
    L = []
    L.append("# Weather settlement watch\n")
    L.append("Generated %s. One row per station-day. `metar` is what `weather_nws` observes, "
             "`CLI` is the NWS climate maximum Kalshi's rules name, `Kalshi` is the interval the "
             "settled ladder pins the paid value to.\n" % time.strftime("%Y-%m-%d %H:%M:%SZ", time.gmtime()))
    triple = [r for r in rows if r["agrees"] is not None]
    agree = [r for r in triple if r["agrees"] == 1]
    L.append("\n## Gate A: does The Weather Company settle on the NWS climate value?\n")
    if not triple:
        L.append("**No station-day yet has both a CLI value and a settled Kalshi ladder.** "
                 "Nothing can be concluded, and weather stays RESEARCH.\n")
    else:
        L.append("**%d of %d** station-days agree (%.1f%%). Every disagreement is listed below; "
                 "one is enough to stop graduation.\n"
                 % (len(agree), len(triple), 100.0 * len(agree) / len(triple)))
        bad = [r for r in triple if r["agrees"] == 0]
        if bad:
            L.append("\n| series | date | CLI | Kalshi interval | ticker |")
            L.append("|---|---|---|---|---|")
            for r in bad:
                L.append("| `%s` | %s | %s | [%s, %s] | `%s` |" % (
                    r["series"], r["local_date"], r["cli_extreme"],
                    r["kalshi_lo"] if r["kalshi_lo"] is not None else "-inf",
                    r["kalshi_hi"] if r["kalshi_hi"] is not None else "+inf", r["kalshi_ticker"]))
            L.append("")
    L.append("\n## Section 10: the settlement shift, per station, with its sample size\n")
    per = collections.defaultdict(list)
    for r in rows:
        if r["shift"] is not None:
            per[r["series"]].append(r["shift"])
    if not per:
        L.append("No station-day yet has both a CLI value and enough observations. "
                 "**The shift distribution currently in the model has no support from this table.**\n")
    else:
        L.append("| series | station | n | mean shift | sd | observed distribution |")
        L.append("|---|---|---|---|---|---|")
        for s in sorted(per, key=lambda x: -len(per[x])):
            v = per[s]
            mean = sum(v) / len(v)
            sd = (sum((x - mean) ** 2 for x in v) / max(1, len(v) - 1)) ** 0.5
            c = collections.Counter(int(round(x)) for x in v)
            dist = " ".join("%+d:%d" % (k, c[k]) for k in sorted(c))
            st = next((r["station"] for r in rows if r["series"] == s), "")
            L.append("| `%s` | %s | **%d** | %+.2f | %.2f | %s |" % (s, st, len(v), mean, sd, dist))
        L.append("\nThe model currently ships `{+0: 0.60, +1: 0.40}` for dense stations and "
                 "`{+0: 0.29, +1: 0.57, +2: 0.14}` for Central Park. Any station above with "
                 "n in single digits cannot confirm or refute those, and the report says n so "
                 "that a point estimate is never mistaken for evidence.\n")
    L.append("\n## The intraday CLI: when the settling value becomes readable\n")
    L.append("The NWS issues a CLI several times a day, each carrying the daily maximum so far "
             "from the station's 5-minute record. They are not corrections. Only the issuance "
             "after local midnight is complete - but the afternoon one usually already carries "
             "the final value, and these markets trade until 01:00 local.\n")
    # The final value is taken from the issuance table itself, not from wx_settlement. Two series
    # can share one CLI location (KXHIGHCHI and KXLOWTCHI are both MDW), so a join on
    # (cli_loc, local_date) can return the daily MINIMUM row and compare a maximum against it.
    # That is what made an issuance after local midnight look 84% final instead of 100%.
    tl = [dict(r) for r in db.execute(
        "SELECT i.*, (SELECT j.value_max FROM wx_cli_issuance j WHERE j.cli_loc=i.cli_loc"
        "  AND j.local_date=i.local_date AND j.next_day=1 AND j.value_max IS NOT NULL"
        "  ORDER BY j.issuance_ts DESC LIMIT 1) AS final"
        " FROM wx_cli_issuance i ORDER BY i.cli_loc, i.local_date, i.issuance_ts")]
    tl = [r for r in tl if r["final"] is not None and r["value_max"] is not None]
    if not tl:
        L.append("No completed station-day with an issuance timeline yet.\n")
    else:
        buck = collections.defaultdict(lambda: [0, 0])
        for r in tl:
            h = r["local_hour"] + (24 if r["next_day"] else 0)
            buck[h][1] += 1
            if r["value_max"] == r["final"]:
                buck[h][0] += 1
        L.append("\n| issuance (local) | already equals the final value | n |")
        L.append("|---|---|---|")
        for h in sorted(buck):
            ok, n = buck[h]
            lab = "%02d:00 next day" % (h - 24) if h >= 24 else "%02d:00" % h
            L.append("| %s | **%.0f%%** | %d |" % (lab, 100.0 * ok / n, n))
        L.append("\nAn issuance that is not yet final can only be low, because a running maximum "
                 "cannot fall. So the afternoon CLI is a hard floor on the settled value, and the "
                 "only question left is how much further the day climbs.\n")
    L.append("\n## Days still running (recorded, not yet comparable)\n")
    prov = [r for r in rows if not r["cli_complete"] and r["cli_extreme"] is not None]
    L.append("%d station-days have a CLI value from an issuance before the local day ended. "
             "They are stored as provisional and excluded from every agreement and shift number "
             "above.\n" % len(prov))
    L.append("\n## Every row\n")
    L.append("| series | date | metar | obs | CLI | shift | Kalshi | agrees |")
    L.append("|---|---|---|---|---|---|---|---|")
    for r in rows:
        k = "-" if r["kalshi_lo"] is None and r["kalshi_hi"] is None else "[%s, %s]" % (
            r["kalshi_lo"] if r["kalshi_lo"] is not None else "-inf",
            r["kalshi_hi"] if r["kalshi_hi"] is not None else "+inf")
        L.append("| `%s` | %s | %s | %s | %s | %s | %s | %s |" % (
            r["series"], r["local_date"],
            "%.1f" % r["metar_extreme"] if r["metar_extreme"] is not None else "-",
            r["metar_obs"] or "-", r["cli_extreme"] if r["cli_extreme"] is not None else "-",
            "%+.1f" % r["shift"] if r["shift"] is not None else "-", k,
            {1: "yes", 0: "**NO**", None: "-"}[r["agrees"]]))
    L.append("")
    out = "\n".join(L)
    open(a.report_out, "w").write(out)
    print(out)
    print("\nwrote %s" % a.report_out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=os.path.join(ROOT, "data", "prod.sqlite"))
    ap.add_argument("--out-db", default=os.path.join(ROOT, "data", "wx_settlement.sqlite"))
    ap.add_argument("--report-out", default=os.path.join(ROOT, "reports", "wx_settlement_watch.md"))
    ap.add_argument("--days", type=int, default=7, help="observation window to pull")
    ap.add_argument("--products", type=int, default=40, help="CLI products to scan per location")
    ap.add_argument("--min-obs", type=int, default=18, help="observations a day needs to count")
    ap.add_argument("--report-only", action="store_true")
    a = ap.parse_args()
    if not a.report_only:
        collect(a)
    report(a)


if __name__ == "__main__":
    main()
