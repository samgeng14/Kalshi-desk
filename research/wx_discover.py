#!/usr/bin/env python3
"""WX-DISCOVER: find every listed daily temperature city, every day, and verify its settlement.

Section 1 and 3 of the volume brief. The desk has been running an eight-series watchlist typed by
hand months ago. Kalshi lists 391 weather series and roughly a hundred daily high/low temperature
ones; whichever of those has an open market today is a city-day the desk could be pricing, and a
city-day is the unit that the minimum detectable edge shrinks with. Discovery is therefore the
cheapest statistical power available: it does not need the model to be right.

Settlement is verified from the contract's own words rather than from a table someone maintains.
Every US temperature contract names its NWS climate product inline - "the maximum temperature
recorded at Chicago (CLIMDW)" - so the product code is read out of `rules_primary` and then
CHECKED against api.weather.gov, which either serves that product or does not. A city whose
product cannot be read, or can be read but does not exist upstream, is RESEARCH. Nothing is
verified by being in a list.

International cities (London, Tokyo, Singapore, ...) list markets but settle on sources with no
NWS climate product, so they fall out here by construction rather than by an opinion about them.
"""
import argparse, collections, json, os, re, sqlite3, sys, time, urllib.error, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def default_db(root):
    """The snapshot, not the live file.

    A SQLite connection opened `mode=ro` still participates in WAL shared memory: it maps and
    updates `<db>-shm`. That was measured here, not assumed - with the desk stopped, a single
    read-only query moved the -shm mtime. Over the bridge mount this research runs on, that is a
    second process touching the writer's locking state from a different operating system, and
    prod.log carries three SQLite failures in one day ("database disk image is malformed", "file
    is not a database", "disk I/O error") on a file that passes quick_check. Read the snapshot.
    """
    snap = os.path.join(root, "data", "snapshots", "prod-latest.sqlite")
    return snap if os.path.exists(snap) else os.path.join(root, "data", "prod.sqlite")
UA = "kalshi-desk-research (samgeng14@gmail.com)"
KALSHI = "https://api.elections.kalshi.com/trade-api/v2"
NWS = "https://api.weather.gov"
SERIES_PAT = re.compile(r"^(KX)?(HIGH|LOW)T?[A-Z0-9]*$")
PRODUCT_PAT = re.compile(r"recorded at ([A-Za-z .'-]+?)\s*\(([A-Z]{3,8})\)")

SCHEMA = """
CREATE TABLE IF NOT EXISTS wx_city(
  series TEXT PRIMARY KEY, kind TEXT, city TEXT, cli_product TEXT, station TEXT,
  open_markets INTEGER, event_ticker TEXT, local_date TEXT, close_time TEXT,
  settlement_authority TEXT, product_ok INTEGER, station_ok INTEGER, verified INTEGER,
  note TEXT, seen_ts INTEGER);
"""


def get(url, timeout=30, accept="application/json"):
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": accept})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)


def discover(a):
    series = get("%s/series?category=Climate%%20and%%20Weather" % KALSHI)["series"]
    cand = [s for s in series
            if s.get("frequency") == "daily" and SERIES_PAT.match(s["ticker"])
            and not s["ticker"].endswith("US")]
    print("daily high/low temperature series listed: %d" % len(cand), file=sys.stderr)

    rows, product_cache, station_cache = [], {}, {}
    for s in sorted(cand, key=lambda x: x["ticker"]):
        tk = s["ticker"]
        try:
            ms = get("%s/markets?series_ticker=%s&status=open&limit=%d" % (KALSHI, tk, a.limit)).get("markets", [])
        except urllib.error.HTTPError:
            continue
        except Exception as e:
            print("  %s: %s" % (tk, e), file=sys.stderr)
            continue
        if not ms:
            continue
        m0 = ms[0]
        rules = m0.get("rules_primary", "") or ""
        low = "minimum temperature" in rules.lower() or "LOW" in tk
        hit = PRODUCT_PAT.search(rules)
        city, prod = (hit.group(1).strip(), hit.group(2)) if hit else ("", "")
        auth = ("TWC" if "weather company" in rules.lower() else
                "NWS" if "national weather service" in rules.lower() else "?")

        # Verify upstream, once per product. Existing at api.weather.gov is what makes a
        # settlement mapping real; a code parsed out of prose is only a claim until then.
        prod_ok = station_ok = 0
        station = ""
        if prod.startswith("CLI") and len(prod) >= 6:
            loc = prod[3:]
            station = "K" + loc
            if loc not in product_cache:
                try:
                    g = get("%s/products/types/CLI/locations/%s" % (NWS, loc), accept="application/geo+json")
                    product_cache[loc] = 1 if g.get("@graph") else 0
                except Exception:
                    product_cache[loc] = 0
            prod_ok = product_cache[loc]
            if station not in station_cache:
                try:
                    get("%s/stations/%s" % (NWS, station), accept="application/geo+json")
                    station_cache[station] = 1
                except Exception:
                    station_cache[station] = 0
            station_ok = station_cache[station]

        note = []
        if not hit:
            note.append("no settlement product named in the rules")
        elif not prod.startswith("CLI"):
            note.append("settlement source is not an NWS climate product")
        else:
            if not prod_ok:
                note.append("CLI product %s not served by api.weather.gov" % prod)
            if not station_ok:
                note.append("observing station %s not served by api.weather.gov" % station)
        verified = 1 if (prod_ok and station_ok) else 0
        rows.append({
            "series": tk, "kind": "low" if low else "high", "city": city, "cli_product": prod,
            "station": station if station_ok else "", "open_markets": len(ms),
            "event_ticker": m0.get("event_ticker", ""), "local_date": "",
            "close_time": m0.get("close_time", ""), "settlement_authority": auth,
            "product_ok": prod_ok, "station_ok": station_ok, "verified": verified,
            "note": "; ".join(note), "seen_ts": int(time.time())})
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-db", default=os.path.join(ROOT, "data", "wx_settlement.sqlite"))
    ap.add_argument("--out", default=os.path.join(ROOT, "reports", "weather_city_coverage.md"))
    ap.add_argument("--config-out", default=os.path.join(ROOT, "config", "weather_discovered.json"))
    ap.add_argument("--desk-db", default=default_db(ROOT))
    ap.add_argument("--limit", type=int, default=8)
    ap.add_argument("--max-watch", type=int, default=12,
                    help="cap on watched weather series. The engine cycle has to refresh every "
                         "watched market inside market_data_max_age_seconds, and on 2026-09-12 a "
                         "cycle already took 17-25s against a 25s bar. Discovery can find 25 "
                         "cities; watching all of them at once is how a desk starts skipping the "
                         "markets it is meant to be pricing.")
    a = ap.parse_args()

    rows = discover(a)
    old_cfg = {}
    try:
        old_cfg = json.load(open(a.config_out))
    except Exception:
        pass
    db = sqlite3.connect(a.out_db)
    db.executescript(SCHEMA)
    for r in rows:
        db.execute("INSERT OR REPLACE INTO wx_city VALUES(:series,:kind,:city,:cli_product,:station,"
                   ":open_markets,:event_ticker,:local_date,:close_time,:settlement_authority,"
                   ":product_ok,:station_ok,:verified,:note,:seen_ts)", r)
    db.commit()

    watched = set()
    try:
        import importlib.util
        sp = importlib.util.spec_from_file_location("c", os.path.join(HERE, "settlement_gate_census.py"))
        M = importlib.util.module_from_spec(sp); sp.loader.exec_module(M)
        watched = set(M.load_jsonc(os.path.join(ROOT, "config", "desk.json")).get("watch_series", []))
    except Exception:
        pass

    ver = [r for r in rows if r["verified"]]
    L = []
    L.append("# Weather city coverage\n")
    L.append("Generated %s. One row per listed daily temperature series with an open market. "
             "`verified` means the settlement product named in the contract's own rules exists at "
             "api.weather.gov, and so does the observing station - not that it appears in a "
             "hand-maintained table.\n" % time.strftime("%Y-%m-%d %H:%M:%SZ", time.gmtime()))
    L.append("\n## Section 31: coverage\n")
    L.append("| | count |")
    L.append("|---|---|")
    L.append("| daily temperature series with an open market | **%d** |" % len(rows))
    L.append("| settlement verified | **%d** |" % len(ver))
    L.append("| of those, currently in `watch_series` | %d |"
             % len([r for r in ver if r["series"] in watched]))
    L.append("| **verified but not watched** | **%d** |"
             % len([r for r in ver if r["series"] not in watched]))
    L.append("| in `watch_series` but not listed today | %d |"
             % len([w for w in watched if w.startswith(("KXHIGH", "KXLOW", "KXDEN"))
                    and w not in {r["series"] for r in rows}]))
    L.append("")

    L.append("\n## Every discovered city\n")
    L.append("| series | kind | city | product | station | auth | verified | in watchlist | note |")
    L.append("|---|---|---|---|---|---|---|---|---|")
    for r in sorted(rows, key=lambda x: (x["kind"], x["series"])):
        L.append("| `%s` | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            r["series"], r["kind"], r["city"] or "-", r["cli_product"] or "-", r["station"] or "-",
            r["settlement_authority"], "**yes**" if r["verified"] else "no",
            "yes" if r["series"] in watched else "**no**", r["note"] or ""))

    missing = [w for w in sorted(watched)
               if w.startswith(("KXHIGH", "KXLOW", "KXDEN")) and w not in {r["series"] for r in rows}]
    if missing:
        L.append("\n## Watched but not listed\n")
        L.append("These are in `config/desk.json` and have no open market under that ticker today. "
                 "A watchlist entry that names a series Kalshi does not list is a city the desk "
                 "believes it is covering and is not.\n")
        for w in missing:
            L.append("- `%s`" % w)
        L.append("")

    L.append("\n## What this does to the minimum detectable edge\n")
    L.append("MDE falls as the square root of independent city-days. The settled sample behind "
             "`reports/weather_capacity.md` accrues at the rate of the watchlist; discovery raises "
             "that rate to the verified count above. Nothing about the model changes - the same "
             "estimator simply becomes testable sooner, which is the whole argument for doing "
             "discovery before anything else.\n")
    out = "\n".join(L)
    open(a.out, "w").write(out)
    # A verified series is still unreachable by the pricing path without a grid point to fetch a
    # forecast for, a zone to measure its day in, and a reading of how often the station reports -
    # the last because the settlement shift is a property of reporting frequency, not of weather.
    cfg = {"stations": {}, "settlement_specs": {}}
    meta = {}
    for r in ver:
        st = r["station"]
        if st not in meta:
            lat = lon = None; tz = ""; per_day = 0
            try:
                g = get("%s/stations/%s" % (NWS, st), accept="application/geo+json")
                lon, lat = g["geometry"]["coordinates"][:2]
            except Exception:
                pass
            if lat is not None:
                try:
                    pt = get("%s/points/%.4f,%.4f" % (NWS, lat, lon), accept="application/geo+json")
                    tz = pt.get("properties", {}).get("timeZone", "")
                except Exception:
                    pass
                try:
                    import datetime as _dt
                    end = _dt.datetime.now(_dt.timezone.utc)
                    beg = end - _dt.timedelta(days=3)
                    obs = get("%s/stations/%s/observations?start=%s&end=%s&limit=500"
                              % (NWS, st, beg.strftime("%Y-%m-%dT%H:%M:%SZ"),
                                 end.strftime("%Y-%m-%dT%H:%M:%SZ")), accept="application/geo+json")
                    per_day = len(obs.get("features", [])) / 3.0
                except Exception:
                    per_day = 0
            meta[st] = (lat, lon, tz, per_day)
        lat, lon, tz, per_day = meta[st]
        if lat is None or not tz:
            r["note"] = (r["note"] + "; " if r["note"] else "") + "no grid point or zone: unreachable"
            r["verified"] = 0
            continue
        # Central Park files roughly 38 observations a day against an airport's 250. The hourly
        # series at a sparse site misses the day's peak far more often, which is the whole reason
        # the settlement shift is per station.
        sparse = per_day > 0 and per_day < 100
        cfg["stations"][r["series"]] = {"lat": round(lat, 4), "lon": round(lon, 4),
                                        "obs_station": st, "cli_product": r["cli_product"],
                                        "low": r["kind"] == "low"}
        cfg["settlement_specs"][r["series"]] = {
            "kind": "weather_cli", "product": r["cli_product"], "station": st, "tz": tz,
            "sparse": bool(sparse), "low": r["kind"] == "low",
            "observations_per_day": round(per_day, 1)}
    # Already-watched cities first - they have quote history and the cycle already carries them -
    # then the rest in a stable order, capped. Growing the universe is not free: every watched
    # market has to be re-quoted inside the staleness bar or it is skipped, and a skipped market
    # is worse than an unwatched one because the desk believes it is covering it.
    # Sticky across runs, in one direction only: mappings accumulate, they never disappear
    # because a city happened not to be listed at the minute this ran. Cities list and delist as
    # each local date rolls over, so a watch list rebuilt from only what is listed RIGHT NOW
    # churns hourly - the desk would keep changing which cities it covers for reasons that have
    # nothing to do with the cities. A watch entry with no listing today costs one wasted API
    # call, which is why KXHIGHAUS sitting unlisted in the config was harmless.
    for series, spec in old_cfg.get("settlement_specs", {}).items():
        if series not in cfg["settlement_specs"]:
            cfg["settlement_specs"][series] = spec
            st = old_cfg.get("stations", {}).get(series)
            if st:
                cfg["stations"][series] = st

    # Only series with BOTH a mapping and a grid point can be watched: one without a station entry
    # would be polled every cycle and refused at the gate every cycle.
    eligible = [x for x in cfg["settlement_specs"] if x in cfg["stations"]]
    # Cities the desk already watches come first - they have quote history and the cycle already
    # carries them - then the rest by name, so the list settles rather than reshuffling.
    order = sorted(x for x in eligible if x in watched) + sorted(x for x in eligible if x not in watched)
    cfg["watch"] = order[:a.max_watch]
    cfg["_discovered_not_watched"] = order[a.max_watch:]
    cfg["_watch_cap_note"] = (
        "capped at --max-watch=%d. Every watched market must be re-quoted inside "
        "market_data_max_age_seconds or the cycle skips it, and on 2026-09-12 a cycle already "
        "took 17-25s against a 25s bar. Raise this only after the skipped-markets count in "
        "prod.log falls." % a.max_watch)
    cfg["_generated"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    cfg["_source"] = ("research/wx_discover.py: settlement product read from each contract's own "
                      "rules_primary, then checked against api.weather.gov. Regenerated daily; "
                      "safe to delete, the desk falls back to config/desk.json alone.")
    cfg["_observations_per_day_note"] = ("counted over 3 days with the API's 500-row cap, so any "
                                         "value at 166.7 is censored rather than measured; only "
                                         "the sparse/dense split below 100 is meaningful")
    # Write through a temp file: the desk reads this at startup and a half-written JSON would be
    # a config parse failure rather than a missing file.
    tmp = a.config_out + ".tmp"
    json.dump(cfg, open(tmp, "w"), indent=2, sort_keys=True)
    os.replace(tmp, a.config_out)
    print(out)
    print("\nwrote %s and %s" % (a.out, a.config_out))


if __name__ == "__main__":
    main()
