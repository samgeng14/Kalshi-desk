#!/usr/bin/env python3
"""WX-SCORE: is the weather model's probability better than the price it is trying to beat?

Section 13 of the weather brief. Nothing here is a trading result; it asks the prior question,
which is whether the number is any good. A model that cannot beat the mid has no edge to size.

Three probabilities are scored separately, because conflating them is exactly what made the
Sep-11 review wrong (section 2):

  raw     what weather_nws itself believes, as stored in model_fairs.
  market  the Kalshi mid at that moment.
  desk    (1-w)*raw + w*market, the number the router actually decides on. At the shipped w=0.5
          half of the desk's opinion is the market's own opinion, so `desk` beating `market` is
          a weaker claim than it looks and `raw` beating `market` is the one that matters.

One observation per contract per horizon, horizons measured back from the contract's close.
Inference is clustered, because every city on one date is under one synoptic pattern and the
contracts within a ladder are six views of a single temperature: treating 185 contracts as 185
independent observations would shrink the interval by a factor of five for free.
"""
import argparse, collections, json, math, os, random, sqlite3, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HORIZONS = [("T-24h", 86400), ("T-12h", 43200), ("T-6h", 21600),
            ("T-3h", 10800), ("T-1h", 3600), ("T-30m", 1800)]


def iso_to_epoch(s):
    if not s:
        return 0
    s = s.replace("Z", "").split(".")[0]
    try:
        return int(time.mktime(time.strptime(s, "%Y-%m-%dT%H:%M:%S")) - time.timezone)
    except Exception:
        return 0


def brier(p, y):
    return (p - y) ** 2


def clustered_bootstrap(units, stat, draws=4000, seed=11):
    """Resample whole clusters. `units` is cluster -> list of per-observation tuples."""
    rng = random.Random(seed)
    keys = list(units)
    if len(keys) < 2:
        return (float("nan"), float("nan"))
    out = []
    for _ in range(draws):
        pick = [units[keys[rng.randrange(len(keys))]] for _ in keys]
        flat = [x for c in pick for x in c]
        v = stat(flat)
        if v == v:
            out.append(v)
    if not out:
        return (float("nan"), float("nan"))
    out.sort()
    return (out[int(0.025 * len(out))], out[int(0.975 * len(out))])


def load(db, weight):
    con = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
    con.row_factory = sqlite3.Row
    mk = {r["ticker"]: dict(r) for r in con.execute(
        "SELECT ticker,event_ticker,series_ticker,result,close_time FROM markets"
        " WHERE result IN ('yes','no') AND (series_ticker LIKE 'KXHIGH%' OR series_ticker LIKE 'KXLOW%')")}
    if not mk:
        return [], 0, 0
    qs = collections.defaultdict(list)
    for r in con.execute("SELECT ticker,ts,p,sigma,yes_bid,yes_ask FROM model_fairs"
                         " WHERE strategy='weather_nws' ORDER BY ts"):
        if r["ticker"] in mk:
            qs[r["ticker"]].append((r["ts"], r["p"], r["sigma"], r["yes_bid"], r["yes_ask"]))
    rows, skipped_one_sided, skipped_no_quote = [], 0, 0
    for tk, m in mk.items():
        close = iso_to_epoch(m["close_time"])
        if close <= 0 or tk not in qs:
            continue
        y = 1.0 if m["result"] == "yes" else 0.0
        date = m["event_ticker"].rsplit("-", 1)[-1]
        for label, back in HORIZONS:
            target = close - back
            # the latest quote at or before the horizon: what the desk could actually have known
            cand = [q for q in qs[tk] if q[0] <= target]
            if not cand:
                skipped_no_quote += 1
                continue
            ts, p, sig, bid, ask = cand[-1]
            if not (0 < bid < 1 and 0 < ask < 1 and ask > bid):
                skipped_one_sided += 1
                continue
            mid = 0.5 * (bid + ask)
            rows.append({"ticker": tk, "event": m["event_ticker"], "series": m["series_ticker"],
                         "date": date, "horizon": label, "y": y, "raw": p, "market": mid,
                         "desk": (1 - weight) * p + weight * mid, "sigma": sig, "ts": ts})
    return rows, skipped_one_sided, skipped_no_quote


def diff_stat(key):
    def f(flat):
        if not flat:
            return float("nan")
        return sum(brier(r[key], r["y"]) - brier(r["market"], r["y"]) for r in flat) / len(flat)
    return f


def table(rows, group, key, L, title):
    L.append("\n### %s\n" % title)
    L.append("| %s | n | Brier(raw) | Brier(market) | Brier(desk) | raw - market | desk - market |" % group)
    L.append("|---|---|---|---|---|---|---|")
    buckets = collections.defaultdict(list)
    for r in rows:
        buckets[r[key]].append(r)
    order = [h for h, _ in HORIZONS] if key == "horizon" else sorted(buckets)
    for g in order:
        v = buckets.get(g) or []
        if not v:
            continue
        br = sum(brier(r["raw"], r["y"]) for r in v) / len(v)
        bm = sum(brier(r["market"], r["y"]) for r in v) / len(v)
        bd = sum(brier(r["desk"], r["y"]) for r in v) / len(v)
        L.append("| %s | %d | %.4f | %.4f | %.4f | **%+.4f** | %+.4f |"
                 % (g, len(v), br, bm, bd, br - bm, bd - bm))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=os.path.join(ROOT, "data", "prod.sqlite"))
    ap.add_argument("--weight", type=float, default=0.5, help="market_weight the router uses")
    ap.add_argument("--out", default=os.path.join(ROOT, "reports", "wx_score.md"))
    a = ap.parse_args()

    rows, one_sided, no_quote = load(a.db, a.weight)
    L = []
    L.append("# Weather probability quality: model versus the price\n")
    L.append("Generated %s. Negative `raw - market` means the model's own number beat the mid.\n"
             % time.strftime("%Y-%m-%d %H:%M:%SZ", time.gmtime()))
    if not rows:
        L.append("**No scoreable observation.** No settled weather contract has a recorded quote "
                 "with a two-sided book at any horizon.\n")
        open(a.out, "w").write("\n".join(L))
        print("\n".join(L))
        return
    events = {r["event"] for r in rows}
    dates = {r["date"] for r in rows}
    L.append("%d observations, %d contracts, **%d settled event-days**, %d distinct dates, %d cities. "
             "Skipped: %d horizons with a one-sided book, %d with no quote yet.\n"
             % (len(rows), len({r["ticker"] for r in rows}), len(events), len(dates),
                len({r["series"] for r in rows}), one_sided, no_quote))

    br = sum(brier(r["raw"], r["y"]) for r in rows) / len(rows)
    bm = sum(brier(r["market"], r["y"]) for r in rows) / len(rows)
    bd = sum(brier(r["desk"], r["y"]) for r in rows) / len(rows)
    by_ev = collections.defaultdict(list)
    by_dt = collections.defaultdict(list)
    for r in rows:
        by_ev[r["event"]].append(r)
        by_dt[r["date"]].append(r)
    ci_ev = clustered_bootstrap(by_ev, diff_stat("raw"))
    ci_dt = clustered_bootstrap(by_dt, diff_stat("raw"))
    ci_ev_d = clustered_bootstrap(by_ev, diff_stat("desk"))

    L.append("\n## Headline\n")
    L.append("| | Brier | vs market |")
    L.append("|---|---|---|")
    L.append("| raw `weather_nws` | %.4f | **%+.4f** |" % (br, br - bm))
    L.append("| Kalshi mid | %.4f | - |" % bm)
    L.append("| desk fair (w=%.2f) | %.4f | %+.4f |" % (a.weight, bd, bd - bm))
    L.append("\n`raw - market` 95%% CI, clustered by event-day (%d clusters): **[%+.4f, %+.4f]**"
             % (len(by_ev), ci_ev[0], ci_ev[1]))
    L.append("\n`raw - market` 95%% CI, clustered by DATE (%d clusters): [%+.4f, %+.4f]"
             % (len(by_dt), ci_dt[0], ci_dt[1]))
    L.append("\n`desk - market` 95%% CI, clustered by event-day: [%+.4f, %+.4f]\n" % ci_ev_d)
    L.append("\nThe date-clustered interval is the honest one: every city on a given date sits "
             "under one synoptic pattern, so six dates is closer to the number of independent "
             "observations here than thirty-four event-days is, and far closer than %d rows. "
             "It is also too few clusters for a bootstrap interval to be trusted, which is the "
             "finding rather than a caveat.\n" % len(rows))

    table(rows, "horizon", "horizon", L, "By horizon")
    table(rows, "city", "series", L, "By city")

    L.append("\n## Calibration of the raw model\n")
    L.append("| predicted | n | mean predicted | observed frequency |")
    L.append("|---|---|---|---|")
    edges = [0, .05, .1, .2, .3, .5, .7, 1.01]
    for i in range(len(edges) - 1):
        v = [r for r in rows if edges[i] <= r["raw"] < edges[i + 1]]
        if not v:
            continue
        L.append("| %.0f-%.0f%% | %d | %.3f | %.3f |" % (
            edges[i] * 100, edges[i + 1] * 100, len(v),
            sum(r["raw"] for r in v) / len(v), sum(r["y"] for r in v) / len(v)))

    L.append("\n## Market-weight race (section 17)\n")
    L.append("Brier of `(1-w)*raw + w*market` at each weight. w=1.00 is the market alone: any "
             "weight that does not beat it is contributing no independent information.\n")
    L.append("\n| w | Brier | vs market |")
    L.append("|---|---|---|")
    for w in (0.0, 0.25, 0.5, 0.75, 1.0):
        b = sum(brier((1 - w) * r["raw"] + w * r["market"], r["y"]) for r in rows) / len(rows)
        L.append("| %.2f | %.4f | %+.4f |" % (w, b, b - bm))
    L.append("\nThis is an in-sample sweep over every settled day the desk has. It is not a "
             "train/validation split, because there are not enough days to cut one, and it must "
             "not be read as a tuned weight.\n")
    open(a.out, "w").write("\n".join(L))
    print("\n".join(L))
    print("\nwrote %s" % a.out)


if __name__ == "__main__":
    main()
