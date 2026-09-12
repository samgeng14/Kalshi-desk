#!/usr/bin/env python3
"""WX-HORIZONS: which weather holding period actually has edge, measured on executable prices.

Sections 7, 27, 28 and 32 of the V3 brief. The brief proposes five sleeves - scalp, intraday,
swing, settlement, day-ahead - and its own premise is that they may not all work: weather could
be excellent held to settlement and worthless over ten minutes, or the reverse. This measures
that before any of them is built, because five sleeves built on an unmeasured premise is five
things to unwind.

Everything here is executable, never midpoint (section 28):

  LONG YES  enter at the yes ask, exit at the yes bid that existed at t+h.
  LONG NO   enter at 1 - yes bid, exit at 1 - yes ask at t+h.

so the spread is paid on the way in and on the way out, as it would be in the book. Kalshi's fee
is quadratic and per leg, `0.07 * P * (1-P)` on these series (maker is free, `fee_type` is plain
`quadratic` on all ten), which means a round trip near 80c costs about 2.2c before the spread.
That number is the bar a ten-minute trade has to clear, and it is why this file exists.

Holding to settlement pays the entry fee only. That asymmetry is part of the answer.

Two things this deliberately does NOT do:
  - treat consecutive quotes as independent. One contract quoted every 15 seconds for six hours
    is not 1,440 observations; states are sampled apart and inference is clustered by event-day.
  - report a markout where no bid existed to sell into. A position you cannot exit has no
    markout, and dropping those rows silently would turn illiquidity into profit.
"""
import argparse, bisect, collections, math, os, random, sqlite3, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HORIZONS = [("5m", 300), ("10m", 600), ("15m", 900), ("30m", 1800),
            ("1h", 3600), ("2h", 7200), ("4h", 14400)]
TOL = 120          # a quote this close to the horizon stands in for it; beyond it, no observation


def fee(price, taker=True):
    """Kalshi quadratic, per contract, marginal. The exchange rounds each ORDER up to a centicent,
    so a one-lot pays more than this; using the unrounded rate is the right marginal number for
    deciding whether a horizon has edge at all, and it flatters short holds rather than the
    reverse."""
    if not taker or not (0 < price < 1):
        return 0.0
    return 0.07 * price * (1 - price)


def iso_to_epoch(s):
    if not s:
        return 0
    s = s.replace("Z", "").split(".")[0]
    try:
        return int(time.mktime(time.strptime(s, "%Y-%m-%dT%H:%M:%S")) - time.timezone)
    except Exception:
        return 0


def clustered_ci(units, stat, draws=3000, seed=17):
    rng = random.Random(seed)
    keys = list(units)
    if len(keys) < 3:
        return (float("nan"), float("nan"))
    out = []
    for _ in range(draws):
        flat = [x for _ in keys for x in units[keys[rng.randrange(len(keys))]]]
        v = stat(flat)
        if v == v:
            out.append(v)
    if not out:
        return (float("nan"), float("nan"))
    out.sort()
    return out[int(.025 * len(out))], out[int(.975 * len(out))]


def mean(xs):
    xs = [x for x in xs if x == x]
    return sum(xs) / len(xs) if xs else float("nan")


class Book:
    """Quote history for one ticker, with at-or-before lookup and a forward max over a window."""
    def __init__(self, rows):
        self.ts = [r[0] for r in rows]
        self.bid = [r[1] for r in rows]
        self.ask = [r[2] for r in rows]

    def at(self, t):
        i = bisect.bisect_right(self.ts, t) - 1
        if i < 0:
            return None
        return self.ts[i], self.bid[i], self.ask[i]

    def near(self, t, tol=TOL):
        """The quote standing at t, but only if the book was actually being recorded near then."""
        got = self.at(t)
        if not got or t - got[0] > tol:
            return None
        return got

    def max_bid_between(self, a, b):
        i = bisect.bisect_right(self.ts, a)
        j = bisect.bisect_right(self.ts, b)
        seg = [x for x in self.bid[i:j] if 0 < x < 1]
        return max(seg) if seg else None


def load(db, weight, sample_s):
    con = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
    con.row_factory = sqlite3.Row
    mk = {r["ticker"]: dict(r) for r in con.execute(
        "SELECT ticker,event_ticker,series_ticker,result,close_time FROM markets"
        " WHERE series_ticker LIKE 'KXHIGH%' OR series_ticker LIKE 'KXLOW%'")}
    books = collections.defaultdict(list)
    for r in con.execute("SELECT ticker,ts,yes_bid,yes_ask FROM quotes WHERE ticker IN"
                         " (SELECT ticker FROM markets WHERE series_ticker LIKE 'KXHIGH%'"
                         "  OR series_ticker LIKE 'KXLOW%') ORDER BY ts"):
        books[r["ticker"]].append((r["ts"], r["yes_bid"], r["yes_ask"]))
    books = {k: Book(v) for k, v in books.items() if len(v) > 10}

    fairs = collections.defaultdict(list)
    for r in con.execute("SELECT ticker,ts,p FROM model_fairs WHERE strategy='weather_nws' ORDER BY ts"):
        fairs[r["ticker"]].append((r["ts"], r["p"]))

    states = []
    for tk, m in mk.items():
        if tk not in books or tk not in fairs:
            continue
        close = iso_to_epoch(m["close_time"])
        settled = m["result"] in ("yes", "no")
        bk = books[tk]
        last_kept = -1e18
        for ts, raw in fairs[tk]:
            if ts - last_kept < sample_s:
                continue
            if close > 0 and ts >= close:
                continue
            q = bk.near(ts)
            if not q:
                continue
            _, bid, ask = q
            if not (0 < bid < 1 and 0 < ask < 1 and ask > bid):
                continue
            last_kept = ts
            mid = .5 * (bid + ask)
            states.append({
                "ticker": tk, "event": m["event_ticker"], "series": m["series_ticker"],
                "ts": ts, "close": close, "settled": settled,
                "y": 1.0 if m["result"] == "yes" else 0.0,
                "raw": raw, "mid": mid, "desk": (1 - weight) * raw + weight * mid,
                "bid": bid, "ask": ask, "book": bk})
    return states


def evaluate(st, side):
    """Returns (entry, terminal_edge, {horizon: executable pnl}, settlement pnl or None)."""
    bk = st["book"]
    if side == "yes":
        entry = st["ask"]
        p_win = st["desk"]
        settle_payout = st["y"]
    else:
        entry = 1 - st["bid"]
        p_win = 1 - st["desk"]
        settle_payout = 1 - st["y"]
    if not (0 < entry < 1):
        return None
    ef = fee(entry)
    edge = p_win - entry - ef
    outs = {}
    for label, h in HORIZONS:
        t = st["ts"] + h
        if st["close"] > 0 and t > st["close"]:
            outs[label] = None
            continue
        q = bk.near(t)
        if not q:
            outs[label] = None
            continue
        _, b2, a2 = q
        exitpx = b2 if side == "yes" else 1 - a2
        if not (0 < exitpx < 1):
            outs[label] = None          # nothing to sell into: not a markout of zero
            continue
        outs[label] = exitpx - entry - ef - fee(exitpx)
    stl = (settle_payout - entry - ef) if st["settled"] else None
    return entry, edge, outs, stl


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=os.path.join(ROOT, "data", "prod.sqlite"))
    ap.add_argument("--weight", type=float, default=0.5)
    ap.add_argument("--sample-seconds", type=int, default=900)
    ap.add_argument("--min-edge", type=float, default=0.03)
    ap.add_argument("--out", default=os.path.join(ROOT, "reports", "weather_horizons.md"))
    a = ap.parse_args()

    states = load(a.db, a.weight, a.sample_seconds)
    rows = []
    for st in states:
        for side in ("yes", "no"):
            got = evaluate(st, side)
            if not got:
                continue
            entry, edge, outs, stl = got
            rows.append({"event": st["event"], "series": st["series"], "ticker": st["ticker"],
                         "ts": st["ts"], "side": side, "entry": entry, "edge": edge,
                         "outs": outs, "settle": stl, "desk": st["desk"], "raw": st["raw"],
                         "close": st["close"], "book": st["book"], "y": st["y"],
                         "settled": st["settled"]})

    L = []
    L.append("# Which weather holding horizon actually has edge\n")
    L.append("Generated %s. Every number is executable: entry crosses the spread, exit sells into "
             "the bid that existed at the horizon, and Kalshi's quadratic fee is charged on each "
             "leg. Holding to settlement pays the entry fee only.\n"
             % time.strftime("%Y-%m-%d %H:%M:%SZ", time.gmtime()))
    if not rows:
        L.append("**No evaluable state.**\n")
        open(a.out, "w").write("\n".join(L)); print("\n".join(L)); return
    L.append("%d candidate states from %d contracts over %d event-days, sampled no more often "
             "than every %d minutes per contract.\n"
             % (len(rows), len({r["ticker"] for r in rows}), len({r["event"] for r in rows}),
                a.sample_seconds // 60))

    sig = [r for r in rows if r["edge"] >= a.min_edge]
    L.append("\n## The headline: mean executable P&L per contract, by horizon\n")
    L.append("Only states the desk would have called a signal (terminal edge >= %.0fc after fee).\n"
             % (a.min_edge * 100))
    L.append("\n| horizon | n | mean | median | win rate | 95%% CI (event-clustered) |")
    L.append("|---|---|---|---|---|---|")
    for label, _ in HORIZONS:
        v = [r for r in sig if r["outs"].get(label) is not None]
        if not v:
            L.append("| %s | 0 | - | - | - | - |" % label); continue
        xs = [r["outs"][label] for r in v]
        cl = collections.defaultdict(list)
        for r in v:
            cl[r["event"]].append(r["outs"][label])
        lo, hi = clustered_ci(cl, mean)
        xs_s = sorted(xs)
        L.append("| %s | %d | **%+.2fc** | %+.2fc | %.0f%% | [%+.2f, %+.2f] |" % (
            label, len(v), 100 * mean(xs), 100 * xs_s[len(xs_s) // 2],
            100.0 * sum(1 for x in xs if x > 0) / len(xs), 100 * lo, 100 * hi))
    v = [r for r in sig if r["settle"] is not None]
    if v:
        xs = [r["settle"] for r in v]
        cl = collections.defaultdict(list)
        for r in v:
            cl[r["event"]].append(r["settle"])
        lo, hi = clustered_ci(cl, mean)
        xs_s = sorted(xs)
        L.append("| **settlement** | %d | **%+.2fc** | %+.2fc | %.0f%% | [%+.2f, %+.2f] |" % (
            len(v), 100 * mean(xs), 100 * xs_s[len(xs_s) // 2],
            100.0 * sum(1 for x in xs if x > 0) / len(xs), 100 * lo, 100 * hi))

    L.append("\n## Does more predicted edge produce more realized profit? (section 16)\n")
    buckets = [(.03, .04), (.04, .05), (.05, .07), (.07, .10), (.10, 1.0)]
    L.append("\n| predicted edge | n | 15m | 1h | settlement |")
    L.append("|---|---|---|---|---|")
    for lo_e, hi_e in buckets:
        v = [r for r in rows if lo_e <= r["edge"] < hi_e]
        if not v:
            continue
        def m(key):
            xs = [r["outs"][key] for r in v if r["outs"].get(key) is not None]
            return "%+.2fc" % (100 * mean(xs)) if xs else "-"
        s = [r["settle"] for r in v if r["settle"] is not None]
        L.append("| %.0f-%.0fc | %d | %s | %s | %s |" % (
            lo_e * 100, hi_e * 100, len(v), m("15m"), m("1h"),
            "%+.2fc" % (100 * mean(s)) if s else "-"))

    L.append("\n## The 80c -> 90c question (section 32)\n")
    L.append("Entries taken between 78c and 82c where the desk fair said 90%% or better. "
             "`touch` means the executable bid actually reached that level, not the mid.\n")
    tgt = [r for r in rows if 0.78 <= r["entry"] <= 0.82 and
           ((r["side"] == "yes" and r["desk"] >= 0.90) or (r["side"] == "no" and 1 - r["desk"] >= 0.90))]
    if not tgt:
        L.append("\n**No such state occurred in the recorded window.** Across %d candidate states "
                 "over %d event-days, the desk fair never reached 90%% on a contract whose "
                 "executable entry was near 80c. The trade this study was built to validate did "
                 "not arise, so it cannot yet be made a rule.\n" % (len(rows), len({r["event"] for r in rows})))
        near = [r for r in rows if 0.70 <= r["entry"] <= 0.90]
        if near:
            best = max(near, key=lambda r: (r["desk"] if r["side"] == "yes" else 1 - r["desk"]))
            bp = best["desk"] if best["side"] == "yes" else 1 - best["desk"]
            L.append("\nThe closest any state came: `%s` %s at %.0fc with a desk fair of %.1f%%.\n"
                     % (best["ticker"], best["side"].upper(), 100 * best["entry"], 100 * bp))
    else:
        L.append("\n| window | n | touched 85c | touched 90c | mean exec P&L if exited at touch | hold to settlement |")
        L.append("|---|---|---|---|---|---|")
        for label, secs in (("15m", 900), ("1h", 3600), ("to close", None)):
            n85 = n90 = n = 0
            for r in tgt:
                end = r["close"] if secs is None else min(r["ts"] + secs, r["close"] or r["ts"] + secs)
                mb = r["book"].max_bid_between(r["ts"], end)
                if mb is None:
                    continue
                best = mb if r["side"] == "yes" else 1 - mb
                n += 1
                n85 += best >= 0.85
                n90 += best >= 0.90
            s = [r["settle"] for r in tgt if r["settle"] is not None]
            L.append("| %s | %d | %.0f%% | %.0f%% | - | %s |" % (
                label, n, 100.0 * n85 / n if n else 0, 100.0 * n90 / n if n else 0,
                "%+.2fc" % (100 * mean(s)) if s else "-"))

    L.append("\n## Entry-price bands (section 32)\n")
    L.append("\n| entry | n | 15m | 1h | settlement | settled win rate |")
    L.append("|---|---|---|---|---|---|")
    for lo_p, hi_p in ((.05, .30), (.30, .60), (.60, .70), (.70, .80), (.80, .90), (.90, .96)):
        v = [r for r in rows if lo_p <= r["entry"] < hi_p]
        if not v:
            continue
        def m(key):
            xs = [r["outs"][key] for r in v if r["outs"].get(key) is not None]
            return "%+.2fc" % (100 * mean(xs)) if xs else "-"
        s = [r for r in v if r["settle"] is not None]
        wr = (100.0 * sum(1 for r in s if r["settle"] > 0) / len(s)) if s else float("nan")
        L.append("| %.0f-%.0fc | %d | %s | %s | %s | %s |" % (
            lo_p * 100, hi_p * 100, len(v), m("15m"), m("1h"),
            "%+.2fc" % (100 * mean([r["settle"] for r in s])) if s else "-",
            "%.0f%%" % wr if wr == wr else "-"))

    L.append("\n## Coverage\n")
    miss = collections.Counter()
    for r in rows:
        for label, _ in HORIZONS:
            if r["outs"].get(label) is None:
                miss[label] += 1
    L.append("\n| horizon | states with no exitable bid at the horizon |")
    L.append("|---|---|")
    for label, _ in HORIZONS:
        L.append("| %s | %d of %d (%.0f%%) |" % (label, miss[label], len(rows),
                                                 100.0 * miss[label] / len(rows)))
    L.append("\nA missing horizon is a contract that either had no bid to sell into or ran past "
             "its close. Both are real constraints on a short-horizon sleeve, not gaps in the data.\n")
    open(a.out, "w").write("\n".join(L))
    print("\n".join(L))
    print("\nwrote %s" % a.out)


if __name__ == "__main__":
    main()
