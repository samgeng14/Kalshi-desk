#!/usr/bin/env python3
"""WX-SETTLE-1: what does the desk have to add to its own observations to hit the settled value?

Kalshi settles these markets on The Weather Company's number, and the rules name an NWS daily
climate product as the location (CLIAUS, CLIMDW, ...). The desk does not read that product. It
reads api.weather.gov/stations/<id>/observations, which is the hourly METAR series, and takes a
running maximum of it. Those are two different numbers:

  - the CLI daily maximum comes from the ASOS 5-minute record over the local calendar day;
  - the hourly METAR maximum can only see the temperature at the times it happens to report,
    so it misses a peak that falls between reports and is biased low.

The old model carried a hard-coded +0/+1/+2F distribution (0.63/0.31/0.06) for this gap. That was
fitted from which brackets won, which is an interval-censored observation of the settled value and
not the value itself, so the estimate was never sound. This measures the gap directly, against
both numbers, for whatever window api.weather.gov still serves.
"""
import argparse, collections, datetime as dt, json, re, sys, urllib.request
from zoneinfo import ZoneInfo

UA = "kalshi-desk-research (samgeng14@gmail.com)"
STATIONS = {  # CLI location -> (METAR station, tz, Kalshi series)
    "AUS": ("KAUS", "America/Chicago", "KXHIGHAUS"),
    "MDW": ("KMDW", "America/Chicago", "KXHIGHCHI"),
    "NYC": ("KNYC", "America/New_York", "KXHIGHNY"),
    "MIA": ("KMIA", "America/New_York", "KXHIGHMIA"),
    "PHL": ("KPHL", "America/New_York", "KXHIGHPHIL"),
    "LAX": ("KLAX", "America/Los_Angeles", "KXHIGHLAX"),
}
MONTHS = ["JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY", "AUGUST",
          "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"]


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "application/geo+json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def cli_maxima(loc, limit):
    """date -> (max_F, issuance). Later issuances win, so a correction replaces its preliminary."""
    out = {}
    try:
        graph = get(f"https://api.weather.gov/products/types/CLI/locations/{loc}").get("@graph", [])
    except Exception as e:
        print(f"  {loc}: product list failed: {e}", file=sys.stderr)
        return out
    for item in graph[:limit]:
        try:
            text = get("https://api.weather.gov/products/" + item["id"]).get("productText", "")
        except Exception:
            continue
        m = re.search(r"SUMMARY FOR (\w+) (\d+) (\d{4})", text.upper())
        v = re.search(r"^\s*MAXIMUM\s+(-?\d+)", text, re.M)
        if not m or not v or m.group(1) not in MONTHS:
            continue
        d = dt.date(int(m.group(3)), MONTHS.index(m.group(1)) + 1, int(m.group(2)))
        stamp = item.get("issuanceTime", "")
        if d not in out or stamp > out[d][1]:
            out[d] = (int(v.group(1)), stamp)
    return out


def metar_maxima(station, tz, days):
    """Local calendar date -> max of the hourly series, in whole F, as the desk would see it."""
    end = dt.datetime.now(dt.timezone.utc)
    start = end - dt.timedelta(days=days + 1)
    url = (f"https://api.weather.gov/stations/{station}/observations"
           f"?start={start.strftime('%Y-%m-%dT%H:%M:%SZ')}&end={end.strftime('%Y-%m-%dT%H:%M:%SZ')}&limit=500")
    try:
        feats = get(url).get("features", [])
    except Exception as e:
        print(f"  {station}: observations failed: {e}", file=sys.stderr)
        return {}, {}
    hi, n = {}, collections.Counter()
    for f in feats:
        p = f.get("properties", {})
        t = (p.get("temperature") or {}).get("value")
        if t is None:
            continue
        ts = dt.datetime.fromisoformat(p["timestamp"].replace("Z", "+00:00")).astimezone(ZoneInfo(tz))
        d = ts.date()
        f_deg = t * 9 / 5 + 32
        hi[d] = max(hi.get(d, -999.0), f_deg)
        n[d] += 1
    return hi, n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--days", type=int, default=7)
    ap.add_argument("--products", type=int, default=40, help="CLI products to scan per location")
    a = ap.parse_args()

    rows = []
    for loc, (station, tz, series) in STATIONS.items():
        cli = cli_maxima(loc, a.products)
        obs, counts = metar_maxima(station, tz, a.days)
        for d, (cmax, _stamp) in sorted(cli.items()):
            if d not in obs or counts[d] < 18:   # need near-full coverage of the day
                continue
            rows.append((loc, series, d, cmax, obs[d], counts[d]))

    if not rows:
        print("no overlapping station-days; api.weather.gov keeps only a short product window")
        return
    print(f"{'CLI':5} {'series':12} {'date':11} {'CLI max':>8} {'hourly max':>11} {'obs':>4} {'gap':>6}")
    gaps = []
    for loc, series, d, cmax, omax, n in rows:
        gap = cmax - omax
        gaps.append(gap)
        print(f"{loc:5} {series:12} {d.isoformat():11} {cmax:>8} {omax:>11.1f} {n:>4} {gap:>+6.1f}")

    print()
    rounded = collections.Counter(int(round(g)) for g in gaps)
    tot = len(gaps)
    print(f"n = {tot} station-days.  CLI daily max minus the max of the hourly series:")
    for k in sorted(rounded):
        print(f"   {k:+d} F   {rounded[k]:>3}   {rounded[k]/tot:6.1%}")
    mean = sum(gaps) / tot
    var = sum((g - mean) ** 2 for g in gaps) / max(1, tot - 1)
    print(f"   mean {mean:+.2f} F   sd {var ** 0.5:.2f}")

    # The gap is a property of how often the station reports, not of the weather. A site that
    # files 24 observations a day can miss its own peak; one that files 288 cannot miss it by
    # much. A single global correction is therefore wrong in both directions at once.
    print()
    print(f"{'CLI':5} {'days':>5} {'obs/day':>8} {'mean gap':>9} {'max gap':>8}")
    per = collections.defaultdict(list)
    for loc, _series, _d, cmax, omax, n in rows:
        per[loc].append((cmax - omax, n))
    for loc, vs in sorted(per.items(), key=lambda kv: -sum(g for g, _ in kv[1]) / len(kv[1])):
        g = [x for x, _ in vs]
        obs = sum(n for _, n in vs) / len(vs)
        print(f"{loc:5} {len(g):>5} {obs:>8.0f} {sum(g)/len(g):>+9.2f} {max(g):>+8.1f}")
    print()
    print("The old hard-coded prior was +0 63%, +1 31%, +2 6% (mean +0.43).")
    print("It was fitted from bracket outcomes, which never observe the settled value directly.")


if __name__ == "__main__":
    main()
