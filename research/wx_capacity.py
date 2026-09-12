#!/usr/bin/env python3
"""WX-CAPACITY: how small an edge is real, and how many dollars fit behind it.

Sections 23, 24 and 25 of the volume-mode brief, in the order that matters. Section 25 is the one
that governs the rest, and the brief states it correctly: "a 0.2c economic edge is meaningless if
model calibration uncertainty is 5c." So this computes the minimum detectable edge FIRST and lets
it decide which floors are even discussable.

Minimum detectable edge is not a modelling choice. It is what the variance of a binary payoff and
the number of independent weather days allow. A contract that pays 1 or 0 has a per-contract
standard deviation near 50c; averaging brings that down as the square root of the number of
INDEPENDENT observations, and the independent unit here is a city-day, not a contract and
certainly not a quote. Every bracket in one ladder resolves on one temperature, so six brackets
are one observation wearing six hats.

Capacity uses recorded top-of-book size. The full ladder was never recorded, so section 7's book
walk cannot be replayed: these numbers are a floor on available size, not the whole book.
"""
import argparse, collections, importlib.util, math, os, random, sqlite3, sys, time

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
_spec = importlib.util.spec_from_file_location("wxh", os.path.join(HERE, "wx_horizons.py"))
WXH = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(WXH)
fee, iso_to_epoch = WXH.fee, WXH.iso_to_epoch

FLOORS = [0.002, 0.003, 0.005, 0.0075, 0.01, 0.02, 0.03, 0.05]


def mean(xs):
    xs = [x for x in xs if x == x]
    return sum(xs) / len(xs) if xs else float("nan")


def clustered_se(units, draws=3000, seed=23):
    """Bootstrap standard error of the mean, resampling whole clusters."""
    rng = random.Random(seed)
    keys = list(units)
    if len(keys) < 3:
        return float("nan")
    ms = []
    for _ in range(draws):
        flat = [x for _ in keys for x in units[keys[rng.randrange(len(keys))]]]
        if flat:
            ms.append(sum(flat) / len(flat))
    if len(ms) < 2:
        return float("nan")
    m = sum(ms) / len(ms)
    return math.sqrt(sum((x - m) ** 2 for x in ms) / (len(ms) - 1))


def load(db, weight, sample_s, uncertainty_k):
    con = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
    con.row_factory = sqlite3.Row
    mk = {r["ticker"]: dict(r) for r in con.execute(
        "SELECT ticker,event_ticker,series_ticker,result,close_time FROM markets"
        " WHERE series_ticker LIKE 'KXHIGH%' OR series_ticker LIKE 'KXLOW%'")}
    q = collections.defaultdict(list)
    for r in con.execute("SELECT ticker,ts,yes_bid,yes_ask,yes_bid_size,yes_ask_size FROM quotes"
                         " ORDER BY ts"):
        if r["ticker"] in mk:
            q[r["ticker"]].append((r["ts"], r["yes_bid"], r["yes_ask"],
                                   r["yes_bid_size"] or 0, r["yes_ask_size"] or 0))
    fairs = collections.defaultdict(list)
    for r in con.execute("SELECT ticker,ts,p,sigma FROM model_fairs WHERE strategy='weather_nws'"
                         " ORDER BY ts"):
        fairs[r["ticker"]].append((r["ts"], r["p"], r["sigma"]))

    out = []
    for tk, m in mk.items():
        if tk not in q or tk not in fairs or m["result"] not in ("yes", "no"):
            continue
        close = iso_to_epoch(m["close_time"])
        y = 1.0 if m["result"] == "yes" else 0.0
        bk = sorted(q[tk])
        ts_list = [x[0] for x in bk]
        import bisect
        last = -1e18
        for ts, raw, sig in fairs[tk]:
            if ts - last < sample_s or (close > 0 and ts >= close):
                continue
            i = bisect.bisect_right(ts_list, ts) - 1
            if i < 0 or ts - ts_list[i] > 120:
                continue
            _, bid, ask, bsz, asz = bk[i]
            if not (0 < bid < 1 and 0 < ask < 1 and ask > bid):
                continue
            last = ts
            cand = []
            mid = .5 * (bid + ask)
            desk = (1 - weight) * raw + weight * mid
            unc = uncertainty_k * (sig if sig == sig else 0.05)
            for side in ("yes", "no"):
                p_win = desk if side == "yes" else 1 - desk
                payout = y if side == "yes" else 1 - y
                taker_px = ask if side == "yes" else 1 - bid
                maker_px = bid if side == "yes" else 1 - ask   # join the resting bid
                depth = asz if side == "yes" else bsz          # size available to lift
                for style, px, f in (("taker", taker_px, fee(taker_px, True)),
                                     ("maker", maker_px, 0.0)):
                    if not (0.01 < px < 0.99):
                        continue
                    cand.append({
                        "event": m["event_ticker"], "series": m["series_ticker"], "ticker": tk,
                        "ts": ts, "side": side, "style": style, "px": px,
                        "net_edge": p_win - px - f - unc,
                        "raw_edge": p_win - px - f,
                        "realized": payout - px - f,       # held to settlement: entry fee only
                        "depth": depth, "p_win": p_win, "unc": unc})
            # ONE side per contract per timestamp per style: the side the desk would actually
            # take. Keeping both is not a conservative choice, it is a different instrument.
            # Buying YES at the bid and NO at the bid costs (1 - spread) and pays exactly 1, so a
            # sample containing both sides is a hedged book: its variance collapses, its return is
            # the spread, and neither number is about the weather model. The first run of this
            # file reported a 13% maker return that was entirely that.
            for style in ("maker", "taker"):
                v = [c for c in cand if c["style"] == style]
                if v:
                    out.append(max(v, key=lambda c: c["net_edge"]))
            cand = []
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=default_db(ROOT))
    ap.add_argument("--weight", type=float, default=0.5)
    ap.add_argument("--sample-seconds", type=int, default=1800)
    ap.add_argument("--uncertainty-k", type=float, default=0.5)
    ap.add_argument("--max-per-trade", type=float, default=500, help="cap on contracts per signal")
    ap.add_argument("--out", default=os.path.join(ROOT, "reports", "weather_capacity.md"))
    a = ap.parse_args()

    rows = load(a.db, a.weight, a.sample_seconds, a.uncertainty_k)
    L = []
    L.append("# Weather capacity, and the smallest edge that is real\n")
    L.append("Generated %s. Settlement entries only: held through resolution, so the entry fee is "
             "paid once and there is no exit transaction. Depth is recorded top of book; the full "
             "ladder was never stored, so these sizes are a floor on what was available.\n"
             % time.strftime("%Y-%m-%d %H:%M:%SZ", time.gmtime()))
    if not rows:
        L.append("**No settled candidate state.**\n")
        open(a.out, "w").write("\n".join(L)); print("\n".join(L)); return
    events = {r["event"] for r in rows}
    dates = {r["event"].rsplit("-", 1)[-1] for r in rows}
    L.append("%d candidate states, %d contracts, **%d settled city-days**, %d distinct dates.\n"
             % (len(rows), len({r["ticker"] for r in rows}), len(events), len(dates)))

    # ---- section 25, and it governs everything below ----------------------------------------
    L.append("\n## Section 25: the minimum detectable edge\n")
    # One observation per contract, taker side, so the sample is directional and each contract
    # counts once. A contract quoted every half hour for six hours is one bet, not twelve.
    per_contract = {}
    for r in rows:
        if r["style"] != "taker":
            continue
        k = r["ticker"]
        if k not in per_contract or r["net_edge"] > per_contract[k]["net_edge"]:
            per_contract[k] = r
    by_ev = collections.defaultdict(list)
    for r in per_contract.values():
        by_ev[r["event"]].append(r["realized"])
    se = clustered_se(by_ev)
    mde = 1.96 * se
    flat = [v for vs in by_ev.values() for v in vs]
    mu = mean(flat)
    sd = (sum((x - mu) ** 2 for x in flat) / max(1, len(flat) - 1)) ** 0.5
    L.append("\n| | value |")
    L.append("|---|---|")
    L.append("| contracts, one directional observation each | %d |" % len(flat))
    L.append("| per-contract standard deviation of settlement P&L | %.1fc |" % (100 * sd))
    L.append("| independent clusters (settled city-days) | **%d** |" % len(by_ev))
    L.append("| standard error of the mean, city-day clustered | %.2fc |" % (100 * se))
    L.append("| **minimum detectable edge (95%%, 50%% power)** | **%.1fc** |" % (100 * mde))
    L.append("\nThat is the smallest per-contract edge this desk can currently tell apart from "
             "zero. It is a property of the binary payoff and the number of independent weather "
             "days, not of the model.\n")
    L.append("\n### How many settled city-days each floor would need\n")
    L.append("Standard error falls as the square root of the number of independent clusters, so "
             "the requirement is `clusters_now x (MDE_now / target)^2`. At roughly 18 cities a "
             "day, every city-day is one cluster - but cities on the same date share a weather "
             "pattern, so this column is optimistic.\n")
    L.append("\n| target edge | city-days needed | days at 18 cities/day |")
    L.append("|---|---|---|")
    for t in (0.002, 0.005, 0.01, 0.02, 0.03, 0.05):
        need = len(by_ev) * (mde / t) ** 2
        L.append("| %.1fc | %s | **%s** |" % (
            100 * t, "{:,.0f}".format(need), "{:,.0f}".format(need / 18.0)))
    L.append("")

    # ---- section 24, the floor race ---------------------------------------------------------
    L.append("\n## Section 24: the edge-floor race\n")
    L.append("`size` is min(recorded top-of-book depth, %d). `expected $` sums net edge x size; "
             "`realized $` sums the settlement P&L those same contracts actually earned. A floor "
             "is only meaningful if it sits above the minimum detectable edge above.\n"
             % int(a.max_per_trade))
    for style in ("maker", "taker"):
        L.append("\n### %s entry\n" % style)
        if style == "maker":
            L.append("**Conditional on the order filling, which is not modelled here.** Section 11 "
                     "of the brief is explicit that quoted spread must not be credited "
                     "mechanically. A resting bid fills when someone crosses it, and the times "
                     "that happens are disproportionately the times the crosser knows something - "
                     "so the realised column below is an upper bound that a fill model will "
                     "lower, by an amount nobody has measured yet.\n")
        L.append("| floor | signals | contracts | capital | expected $ | **realized $** | realized per $ |")
        L.append("|---|---|---|---|---|---|---|")
        for f in FLOORS:
            v = [r for r in rows if r["style"] == style and r["net_edge"] >= f]
            if not v:
                L.append("| %.1fc | 0 | - | - | - | - | - |" % (100 * f)); continue
            n = cap = exp = real = 0.0
            for r in v:
                q = min(r["depth"], a.max_per_trade)
                if q <= 0:
                    continue
                n += q; cap += q * r["px"]; exp += q * r["net_edge"]; real += q * r["realized"]
            L.append("| %.1fc | %d | %s | $%s | $%s | **$%s** | %s |" % (
                100 * f, len(v), "{:,.0f}".format(n), "{:,.0f}".format(cap),
                "{:,.0f}".format(exp), "{:,.0f}".format(real),
                "%+.3f" % (real / cap) if cap > 0 else "-"))
    L.append("\n### What lowering the floor actually buys\n")
    L.append("The question volume mode asks is whether accepting smaller edges produces more total "
             "dollars. Marginal expected value of each step down, maker entry:\n")
    L.append("\n| floor | signals | expected $ | gained by lowering to here |")
    L.append("|---|---|---|---|")
    prev = None
    for f in sorted(FLOORS, reverse=True):
        v = [r for r in rows if r["style"] == "maker" and r["net_edge"] >= f]
        exp = sum(min(r["depth"], a.max_per_trade) * r["net_edge"] for r in v)
        gain = "-" if prev is None else "+$%s for +%d signals" % (
            "{:,.0f}".format(exp - prev[1]), len(v) - prev[0])
        L.append("| %.1fc | %d | $%s | %s |" % (100 * f, len(v), "{:,.0f}".format(exp), gain))
        prev = (len(v), exp)
    L.append("")

    L.append("\nThese are every candidate state, not a portfolio: the same contract appears at "
             "many timestamps and both sides appear separately, so `contracts` and `capital` are "
             "an upper bound on opportunity rather than a tradeable plan.\n")

    # ---- edge bands, section 19 --------------------------------------------------------------
    L.append("\n## Section 19: where the candidates actually sit\n")
    L.append("\n| net edge band | maker signals | taker signals | median depth |")
    L.append("|---|---|---|---|")
    bands = [(-9, 0), (0, .002), (.002, .005), (.005, .01), (.01, .02), (.02, .05), (.05, 9)]
    for lo, hi in bands:
        mk_ = [r for r in rows if r["style"] == "maker" and lo <= r["net_edge"] < hi]
        tk_ = [r for r in rows if r["style"] == "taker" and lo <= r["net_edge"] < hi]
        d = sorted(r["depth"] for r in (mk_ + tk_) if r["depth"] > 0)
        lab = "negative" if hi <= 0 else ("%.1f-%.1fc" % (100 * lo, 100 * hi) if hi < 9 else "5c+")
        L.append("| %s | %d | %d | %s |" % (lab, len(mk_), len(tk_),
                                            "{:,.0f}".format(d[len(d) // 2]) if d else "-"))
    L.append("")
    open(a.out, "w").write("\n".join(L))
    print("\n".join(L))
    print("\nwrote %s" % a.out)


if __name__ == "__main__":
    main()
