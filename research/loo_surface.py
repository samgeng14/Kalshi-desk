#!/usr/bin/env python3
"""Do sibling Kalshi contracts price one withheld contract better than its own quote does?

This is the fast version Sam asked for first: partition arithmetic, no distribution fitting.

On an event whose brackets form a verified exhaustive partition, probability has to add to one, so
the fair value of any one bracket is fixed by the others:

    p_loo(i) = 1 - sum_{j != i} p(j)

The target is excluded by construction, not by discipline, which is the cleanest possible version
of leave-one-out. If this shows nothing, the smooth-distribution versions are unlikely to rescue
it, and the lane closes in an afternoon instead of a fortnight.

WHY THIS IS NOT THE ARB THAT ALREADY DIED
The constraint scan killed multi-leg baskets because Kalshi's fee is quadratic AND per leg, so a
5-leg basket pays five fees to capture one inconsistency. Here the siblings are only INFORMATION.
Exactly one contract is traded, so exactly one fee is paid. Same identity, a fifth of the cost.

WHAT IS VERIFIED RATHER THAN ASSUMED
- Exhaustiveness comes from the strikes, never from `event_meta.exhaustive`, which is 0 on all 813
  events in this database and therefore carries no information. A partition is accepted only when
  the brackets are contiguous and the two ends are open (a `less` at the bottom, a `greater` at
  the top). That works for integer temperatures and for continuous prices without a tick fudge.
- Every sibling must be quoted within `--skew` seconds of the decision, and EVERY bracket in the
  partition must be present. A missing bracket is not treated as zero: silence is not a price, and
  filling it with zero would bias every p_loo upward by exactly the mass we failed to observe.
- Entry is the executable ask, never the mid. Fees are Kalshi's real schedule.
- One trade per ticker, so repeated snapshots of one disagreement cannot become repeated bets.

  python3 research/loo_surface.py --self-test
  python3 research/loo_surface.py --category "Climate and Weather" --out reports/loo_surface.md
"""
import argparse, math, os, random, sqlite3, statistics, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def fee_per_contract(price, mult=1.0):
    if price <= 0 or price >= 1:
        return 0.0
    return math.ceil(mult * 0.07 * price * (1.0 - price) * 10000.0) / 10000.0


def verified_partition(rows):
    """rows: (ticker, strike_type, floor, cap). Returns brackets in order, or None.

    Kalshi brackets are NOT numerically contiguous, because the settled quantity is discretized.
    A temperature ladder runs [77,78] then [79,80]: no gap in integers, a gap of 1 in the numbers.
    A BTC ladder runs [70200, 70299.99] then [70300, ...]: a gap of one cent. So tiling is verified
    by a CONSTANT tick between consecutive brackets rather than by equality, and the tick must not
    exceed one bracket width - which is what rejects a ladder with a genuine hole in the middle,
    where the "tick" would be many brackets wide.

    The two ends must join exactly: Kalshi's `less` is strict (cap is the first EXCLUDED value, so
    less.cap == first bracket's floor) and `greater` is strict the other way (greater.floor ==
    last bracket's cap). Both checked, because an open end that does not meet the ladder means the
    partition has an uncovered tail and probability need not sum to one over what we can see.
    """
    lows = [r for r in rows if r[1] == "less" and r[3] is not None]
    highs = [r for r in rows if r[1] in ("greater", "greater_or_equal") and r[2] is not None]
    mids = [r for r in rows if r[1] == "between" and r[2] is not None and r[3] is not None]
    if len(lows) != 1 or len(highs) != 1 or len(mids) < 2:
        return None
    mids.sort(key=lambda r: r[2])
    widths = [m[3] - m[2] for m in mids]
    w = statistics.median(widths)
    if w <= 0 or any(abs(x - w) > max(w * 0.02, 1e-9) for x in widths):
        return None                                   # ragged bracket widths are not a ladder
    ticks = [b[2] - a[3] for a, b in zip(mids, mids[1:])]
    t = ticks[0]
    if t < 0 or any(abs(x - t) > max(w * 0.02, 1e-9) for x in ticks):
        return None                                   # the step between brackets must be constant
    if t > w + 1e-9:
        return None                                   # a "tick" wider than a bracket is a hole
    if abs(mids[0][2] - lows[0][3]) > 1e-6:
        return None                                   # the bottom tail must meet the ladder
    if abs(highs[0][2] - mids[-1][3]) > 1e-6:
        return None                                   # and so must the top tail
    return [lows[0][0]] + [m[0] for m in mids] + [highs[0][0]]


def load(conn, category, since_days, min_brackets):
    mk = {}
    q = ("SELECT m.ticker,m.event_ticker,m.series_ticker,m.strike_type,m.floor_strike,m.cap_strike,"
         "m.result,m.close_time,s.category,s.fee_multiplier FROM markets m "
         "LEFT JOIN series s ON s.ticker=m.series_ticker")
    for tk, ev, ser, st, fl, cp, res, ct, cat, fm in conn.execute(q):
        mk[tk] = dict(ev=ev, ser=ser, st=st, fl=fl, cp=cp, res=res, ct=ct,
                      cat=cat or "?", fee=fm if fm is not None else 1.0)
    by_ev = defaultdict(list)
    for tk, i in mk.items():
        if i["ev"]:
            by_ev[i["ev"]].append(tk)
    parts = {}
    for ev, tks in by_ev.items():
        if len(tks) < min_brackets:
            continue
        if category and mk[tks[0]]["cat"] != category:
            continue
        if not all(mk[t]["res"] in ("yes", "no") for t in tks):
            continue                      # settled events only: we need the outcome
        order = verified_partition([(t, mk[t]["st"], mk[t]["fl"], mk[t]["cp"]) for t in tks])
        if order and len(order) == len(tks):
            parts[ev] = order
    return mk, parts


def snapshots(conn, tickers, skew):
    """Chronological sweep returning only moments where every bracket is quoted within `skew`."""
    qm = ",".join("?" for _ in tickers)
    rows = conn.execute(
        f"SELECT ts,ticker,yes_bid,yes_ask FROM quotes WHERE ticker IN ({qm}) ORDER BY ts",
        tuple(tickers)).fetchall()
    live = {}
    need = set(tickers)
    for ts, tk, yb, ya in rows:
        live[tk] = (ts, yb or 0.0, ya or 0.0)
        if len(live) < len(need):
            continue
        fresh = {t: v for t, v in live.items() if ts - v[0] <= skew}
        if len(fresh) == len(need):
            yield ts, dict(fresh)


def study(conn, mk, parts, skew, horizons, edge_min, shuffle_ctrl, sib_delay):
    briers, trades, cov = [], [], {"events": 0, "with_snaps": 0, "snaps": 0}
    rng = random.Random(11)
    for ev, order in sorted(parts.items()):
        cov["events"] += 1
        close = mk[order[0]]["ct"]
        snaps = list(snapshots(conn, order, skew))
        if not snaps:
            continue
        cov["with_snaps"] += 1
        cov["snaps"] += len(snaps)
        traded = set()
        # one Brier sample per contract per horizon, never per snapshot
        # keyed by (ticker, horizon): Sam's spec is one sample per CONTRACT per
        # horizon. Keying on horizon alone silently took the first contract only.
        want = set()
        import datetime
        try:
            ct = datetime.datetime.strptime(close, "%Y-%m-%dT%H:%M:%SZ").replace(
                tzinfo=datetime.timezone.utc).timestamp()
        except Exception:
            ct = None
        for ts, fresh in snaps:
            mids = {t: (fresh[t][1] + fresh[t][2]) / 2.0 for t in order}
            if sib_delay:                       # control 3: stale siblings must hurt
                mids = {t: max(0.0, min(1.0, v + rng.gauss(0, sib_delay))) for t, v in mids.items()}
            total = sum(mids.values())
            for t in order:
                p_loo = max(0.0, min(1.0, 1.0 - (total - mids[t])))
                res = 1.0 if mk[t]["res"] == "yes" else 0.0
                if shuffle_ctrl:                # control 1: outcome must stop mattering
                    res = 1.0 if rng.random() < 0.5 else 0.0
                if ct:
                    for h in horizons:
                        lo, hi = h * 60 - 180, h * 60 + 180
                        if lo <= ct - ts <= hi and (t, h) not in want:
                            want.add((t, h))
                            briers.append((ev, t, h, p_loo, mids[t], res))
                # ---- single-leg trade, executable side only -----------------------------
                if t in traded:
                    continue
                f = mk[t]["fee"]
                ya, yb = fresh[t][2], fresh[t][1]
                # Evaluate both sides and take the better one. Checking YES first and breaking
                # silently preferred YES whenever both qualified.
                best = None
                for side in ("yes", "no"):
                    px = ya if side == "yes" else 1.0 - yb
                    if not (0 < px < 1):
                        continue
                    fair = p_loo if side == "yes" else 1.0 - p_loo
                    fee = fee_per_contract(px, f)
                    edge = fair - px - fee
                    if edge < edge_min:
                        continue
                    if best is None or edge > best[0]:
                        best = (edge, side, px, fee, fair)
                if best:
                    edge, side, px, fee, _ = best
                    won = (res == 1.0) if side == "yes" else (res == 0.0)
                    pnl = (1.0 if won else 0.0) - px - fee
                    trades.append(dict(ev=ev, tk=t, side=side, ts=ts, px=px, fee=fee,
                                       edge=edge, pnl=pnl, p_loo=p_loo, mid=mids[t]))
                    traded.add(t)
    return briers, trades, cov


def bucket(x):
    for lo, hi, lab in ((0, .01, "0-1c"), (.01, .02, "1-2c"), (.02, .03, "2-3c"),
                        (.03, .05, "3-5c"), (.05, .08, "5-8c")):
        if lo <= x < hi:
            return lab
    return "8c+"


def boot(vals, keys, n=2000, seed=7):
    if not vals:
        return (0, 0, 0)
    by = defaultdict(list)
    for v, k in zip(vals, keys):
        by[k].append(v)
    ks = list(by)
    rng = random.Random(seed)
    means = []
    for _ in range(n):
        s = [x for _ in ks for x in by[ks[rng.randrange(len(ks))]]]
        if s:
            means.append(sum(s) / len(s))
    means.sort()
    return (sum(vals) / len(vals), means[int(.025 * len(means))], means[int(.975 * len(means))])


def render(briers, trades, cov, args, path):
    L = []
    W = L.append
    W("# Leave-one-out partition arithmetic: do the siblings price the withheld contract?\n\n")
    W(f"`p_loo(i) = 1 - sum of the other brackets`, on events whose strikes form a **verified** "
      f"exhaustive partition. Every bracket must be quoted within {args.skew}s of the decision or "
      f"the snapshot is discarded whole. Entry is the executable ask with Kalshi's real fee, one "
      f"trade per ticker, held to settlement.\n\n")
    W(f"- settled partition events found: **{cov['events']}**\n")
    W(f"- with at least one fully synchronized snapshot: **{cov['with_snaps']}**\n")
    W(f"- synchronized snapshots: **{cov['snaps']:,}**\n\n")

    W("## Does it predict better than the contract's own price?\n\n")
    if not briers:
        W("No horizon samples: no snapshot landed in a scoring window.\n\n")
    else:
        bl = [(p - r) ** 2 for _, _, _, p, m, r in briers]
        bm = [(m - r) ** 2 for _, _, _, p, m, r in briers]
        d = [a - b for a, b in zip(bl, bm)]
        ev_keys = [x[0] for x in briers]
        mean, lo, hi = boot(d, ev_keys)
        W(f"{len(briers)} samples, one per contract per horizon, clustered by event.\n\n")
        W("| | Brier |\n|---|---:|\n")
        W(f"| market mid | {sum(bm)/len(bm):.4f} |\n")
        W(f"| leave-one-out | {sum(bl)/len(bl):.4f} |\n")
        W(f"| **difference** | **{mean:+.4f}** |\n")
        W(f"| 95% CI (event-clustered) | [{lo:+.4f}, {hi:+.4f}] |\n\n")
        W(f"Negative favours the model. {'It does not.' if mean >= 0 else 'It does.'}\n\n")

    W("## Executable single-leg trades\n\n")
    if not trades:
        W(f"No candidate ever cleared the {args.edge_min:.3f} entry threshold.\n\n")
    else:
        pnl = [t["pnl"] for t in trades]
        mean, lo, hi = boot(pnl, [t["ev"] for t in trades])
        W(f"- trades (one per ticker): **{len(trades)}**\n")
        W(f"- mean net P&L per trade: **{100*mean:+.2f}c**  95% CI [{100*lo:+.2f}, {100*hi:+.2f}]\n")
        W(f"- win rate: {100*sum(1 for p in pnl if p>0)/len(pnl):.1f}%\n")
        W(f"- total: ${sum(pnl):+.2f} on {len(trades)} contracts\n\n")
        W("### Edge buckets - the test that matters\n\n")
        W("If predicted edge is real it should order realized economics. If an 8c edge pays like "
          "a 1c edge, the number is not measuring edge.\n\n")
        W("| predicted edge | trades | mean predicted | mean realized | win rate |\n")
        W("|---|---:|---:|---:|---:|\n")
        by = defaultdict(list)
        for t in trades:
            by[bucket(t["edge"])].append(t)
        for lab in ("0-1c", "1-2c", "2-3c", "3-5c", "5-8c", "8c+"):
            g = by.get(lab)
            if not g:
                continue
            W(f"| {lab} | {len(g)} | {100*sum(x['edge'] for x in g)/len(g):+.2f}c | "
              f"{100*sum(x['pnl'] for x in g)/len(g):+.2f}c | "
              f"{100*sum(1 for x in g if x['pnl']>0)/len(g):.0f}% |\n")
        W("\n")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "w").write("".join(L))
    return "".join(L)


def self_test():
    # a clean 1-degree ladder is a partition; a hole in the middle is not
    ok = [("A", "less", None, 77.0), ("B", "between", 77.0, 78.0), ("C", "between", 79.0, 80.0),
          ("D", "between", 81.0, 82.0), ("E", "greater", 82.0, None)]
    assert verified_partition(ok) == ["A", "B", "C", "D", "E"]
    hole = [("A", "less", None, 77.0), ("B", "between", 77.0, 78.0), ("C", "between", 90.0, 91.0),
            ("D", "greater", 91.0, None)]
    assert verified_partition(hole) is None, "a gap must be refused"
    open_top = [("A", "less", None, 77.0), ("B", "between", 77.0, 78.0), ("C", "between", 79.0, 80.0)]
    assert verified_partition(open_top) is None, "no open top bracket must be refused"
    # continuous $100 buckets, the crypto shape
    crypto = ([("L", "less", None, 70200.0)] +
              [(f"B{i}", "between", 70200.0 + 100*i, 70300.0 + 100*i) for i in range(6)] +
              [("H", "greater", 70800.0, None)])
    assert verified_partition(crypto) is not None, "contiguous price buckets must be accepted"
    # the identity itself, and the leakage property: the target's own quote cannot move its fair
    mids = {"a": 0.10, "b": 0.25, "c": 0.40, "d": 0.25}
    tot = sum(mids.values())
    p_c = 1.0 - (tot - mids["c"])
    assert abs(p_c - 0.40) < 1e-12
    mids2 = dict(mids); mids2["c"] = 0.99          # move ONLY the target
    tot2 = sum(mids2.values())
    p_c2 = 1.0 - (tot2 - mids2["c"])
    assert abs(p_c2 - p_c) < 1e-12, "target quote leaked into its own fair value"
    assert fee_per_contract(0.5) > fee_per_contract(0.95)
    print("loo_surface self-test: PASS  partitions verified from strikes (gap and open-top")
    print("  refused, continuous buckets accepted), and the target's own quote provably cannot")
    print("  change its own leave-one-out fair value.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=os.path.join(ROOT, "data", "prod.sqlite"))
    ap.add_argument("--category", default="")
    ap.add_argument("--skew", type=float, default=5.0)
    ap.add_argument("--min-brackets", type=int, default=4)
    ap.add_argument("--edge-min", type=float, default=0.005)
    ap.add_argument("--horizons", default="360,180,60,30,10")
    ap.add_argument("--shuffle-control", action="store_true")
    ap.add_argument("--sibling-noise", type=float, default=0.0)
    ap.add_argument("--since-days", type=float, default=30.0)
    ap.add_argument("--out", default=os.path.join(ROOT, "reports", "loo_surface.md"))
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    conn = sqlite3.connect(f"file:{a.db}?mode=ro", uri=True)
    mk, parts = load(conn, a.category, a.since_days, a.min_brackets)
    print(f"verified settled partition events: {len(parts)}", file=sys.stderr)
    hz = [int(x) for x in a.horizons.split(",")]
    b, t, cov = study(conn, mk, parts, a.skew, hz, a.edge_min, a.shuffle_control, a.sibling_noise)
    print(render(b, t, cov, a, a.out))
    print(f"wrote {os.path.relpath(a.out, ROOT)}")


if __name__ == "__main__":
    main()
