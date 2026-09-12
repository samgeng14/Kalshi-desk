#!/usr/bin/env python3
"""Audit what the backfill actually retrieved, per contract, and grade each one for research use.

`--days 90` is a request, not a fact. Kalshi launched perps in late May 2026 and listed the first
crypto contracts in early June, so those can have roughly three months by September; gold and
silver listed 2026-09-10 and have days. Fitting a model across contracts with wildly different
history, without knowing which is which, is how a result ends up being a statement about BTC's
three months and nothing else.

Writes reports/perps_data_audit.md and prints the grade table.

  python3 scripts/perps_audit.py
  python3 scripts/perps_audit.py --db data/perps_research.sqlite
"""
import argparse, os, sqlite3, statistics, sys
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# Kalshi encodes "no order on this side" as INT64_MAX, which arrives as 922337203685477.6 after the
# 10^4 fixed-point scaling. It is not a price. Left in, one such bar produces two astronomical
# returns -- into it and out of it -- which is enough to move a decile mean to 1e14 bps and make a
# feature look like a 300-trillion-basis-point edge. This exact sentinel was already fixed once in
# src/perps.cpp; it was not carried into the research path.
SENTINEL = 1e9


def px(v):
    """A price, or 0 if the field is the no-order sentinel, negative, or not finite."""
    try:
        f = float(v)
    except (TypeError, ValueError):
        return 0.0
    return f if (f == f and 0.0 < f < SENTINEL) else 0.0


def pct(x, q):
    if not x: return float('nan')
    s = sorted(x)
    i = min(len(s) - 1, max(0, int(round(q * (len(s) - 1)))))
    return s[i]


def grade(days, coverage, bars, gap_med, trades_per_min):
    """A: launch-era and dense. B: usable. C: newly listed. D: do not fit on this."""
    if bars < 500 or days < 2: return 'D'
    if days >= 60 and coverage >= 0.80: return 'A'
    if days >= 21 and coverage >= 0.60: return 'B'
    return 'C'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--db', default=os.path.join(ROOT, 'data', 'perps_research.sqlite'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'reports', 'perps_data_audit.md'))
    ap.add_argument('--period', type=int, default=1)
    a = ap.parse_args()
    if not os.path.exists(a.db):
        sys.exit(f"{a.db} not found -- run scripts/backfill_perps_history.py first")
    db = sqlite3.connect("file:%s?mode=ro" % a.db, uri=True)

    tickers = [r[0] for r in db.execute(
        "select distinct ticker from perp_bars where period=? order by ticker", (a.period,))]
    if not tickers:
        sys.exit("no bars in the database for that period")

    funding = dict(db.execute("select ticker, count(*) from perp_funding group by ticker"))
    trades = dict(db.execute("select ticker, count(*) from perp_hist_trades group by ticker"))
    # An empty trades table means the backfill was run without --trades, which is not the same
    # claim as "these markets never traded". Reporting 0.00 trades/min for a market whose bars
    # show volume in half its minutes would be a plainly false number.
    trades_fetched = bool(trades)
    rows = []
    for t in tickers:
        bars = db.execute(
            "select end_ts, bid_close, ask_close, volume, volume_notional, open_interest "
            "from perp_bars where ticker=? and period=? order by end_ts", (t, a.period)).fetchall()
        if not bars: continue
        first, last = bars[0][0], bars[-1][0]
        span_min = max(1, (last - first) // 60)
        expected = span_min + 1
        coverage = min(1.0, len(bars) / expected) if expected else 0.0
        days = (last - first) / 86400

        gaps, notionals, vols, ois = [], [], [], []
        zero_vol = 0
        for _, b, k, v, n, oi in bars:
            b, k = px(b), px(k)
            # ask.close - bid.close is the CLOSE QUOTE GAP, not a spread. Nothing in the API
            # guarantees the last observed bid and the last observed ask inside a minute share a
            # timestamp, so this is a coarse liquidity proxy and nothing finer. The live L2
            # recorder is the only source of a contemporaneous spread.
            if b > 0 and k > 0 and k >= b:
                mid = .5 * (b + k)
                if mid > 0: gaps.append((k - b) / mid * 1e4)
            if n: notionals.append(n)
            if oi: ois.append(oi)
            vols.append(v or 0)
            if not v: zero_vol += 1

        tr = trades.get(t, 0)
        rows.append({
            'ticker': t, 'first': first, 'last': last, 'bars': len(bars), 'expected': expected,
            'coverage': coverage, 'days': days,
            'trades': tr, 'trades_per_min_avg': tr / span_min if span_min else 0,
            'trades_per_min_med': statistics.median(vols) if vols else 0,
            'zero_vol_minutes': zero_vol,
            'zero_vol_frac': zero_vol / len(bars),
            'notional_med': statistics.median(notionals) if notionals else 0,
            'gap_med': statistics.median(gaps) if gaps else float('nan'),
            'gap_p25': pct(gaps, .25), 'gap_p75': pct(gaps, .75),
            'gap_p95': pct(gaps, .95), 'gap_n': len(gaps),
            'oi_avg': statistics.mean(ois) if ois else 0,
            'funding': funding.get(t, 0),
        })
        rows[-1]['grade'] = grade(rows[-1]['days'], coverage, len(bars),
                                  rows[-1]['gap_med'], rows[-1]['trades_per_min_avg'])

    rows.sort(key=lambda r: (r['grade'], -r['days']))
    d = lambda ts: datetime.fromtimestamp(ts, timezone.utc).strftime('%Y-%m-%d %H:%M')

    if not trades_fetched:
        print("NOTE: no rows in perp_hist_trades -- the backfill was run without --trades, so the\n"
              "      trade columns below are blank rather than zero. Per-minute VOLUME from the\n"
              "      bars is still real and is what the 0-vol column uses.\n")
    hdr = (f"{'ticker':14s} {'first':>16s} {'last':>16s} {'days':>6s} {'bars':>8s} {'cov':>6s} "
           f"{'trades':>8s} {'t/min':>7s} {'0-vol':>6s} {'$/min':>10s} {'gap':>6s} {'p95':>6s} "
           f"{'OI':>10s} {'fund':>5s}  grade")
    print(hdr); print('-' * len(hdr))
    for r in rows:
        tcol = f"{r['trades']:8,d}" if trades_fetched else f"{'-':>8s}"
        tmin = f"{r['trades_per_min_avg']:7.2f}" if trades_fetched else f"{'-':>7s}"
        print(f"{r['ticker']:14s} {d(r['first']):>16s} {d(r['last']):>16s} {r['days']:6.1f} "
              f"{r['bars']:8,d} {r['coverage']*100:5.0f}% {tcol} "
              f"{tmin} {r['zero_vol_frac']*100:5.0f}% "
              f"{r['notional_med']:10,.0f} {r['gap_med']:6.1f} {r['gap_p95']:6.1f} "
              f"{r['oi_avg']:10,.0f} {r['funding']:5d}  {r['grade']}")

    groups = {'A': 'launch / long history', 'B': 'intermediate history',
              'C': 'newly listed', 'D': 'insufficient data -- do not fit'}
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, 'w') as f:
        w = f.write
        w("# Perps data audit\n\n")
        w(f"Generated {datetime.now(timezone.utc):%Y-%m-%d %H:%M} UTC from `{a.db}`, "
          f"{a.period}-minute bars.\n\n")
        w("`--days 90` is a request, not a fact. Kalshi launched perps in late May 2026 with the\n"
          "first crypto contracts listed in early June; gold and silver listed 2026-09-10. History\n"
          "is per-contract and the grades below are the gate on which contracts a model may be\n"
          "fitted across.\n\n")
        w("**This report contains no spreads.** The column is `close_quote_gap_bps` =\n"
          "`ask_close - bid_close` over the mid. Nothing in the API guarantees that the last\n"
          "observed bid and the last observed ask inside a minute share a timestamp, so this is a\n"
          "**coarse liquidity proxy**, not an instantaneous spread, and it must not be used as an\n"
          "execution cost. Differencing `bid_high` against `ask_low` would be worse still: two\n"
          "different instants, producing a gap that never existed. The live L2 recorder is the\n"
          "only source of a contemporaneous spread.\n\n")
        for g, label in groups.items():
            sel = [r for r in rows if r['grade'] == g]
            w(f"## Group {g}: {label} ({len(sel)})\n\n")
            if not sel:
                w("_none_\n\n"); continue
            w("| ticker | first | last | days | bars | coverage | trades | trades/min | "
              "0-vol min | median $/min | close quote gap bps (p25/med/p75/p95) | avg OI | funding obs |\n")
            w("|" + "---|" * 13 + "\n")
            for r in sel:
                w(f"| `{r['ticker']}` | {d(r['first'])} | {d(r['last'])} | {r['days']:.1f} | "
                  f"{r['bars']:,} / {r['expected']:,} | {r['coverage']*100:.0f}% | "
                  f"{r['trades']:,} | " if trades_fetched else
                  f"| `{r['ticker']}` | {d(r['first'])} | {d(r['last'])} | {r['days']:.1f} | "
                  f"{r['bars']:,} / {r['expected']:,} | {r['coverage']*100:.0f}% | not fetched | ")
                w(f"{r['trades_per_min_avg']:.2f} | {r['zero_vol_frac']*100:.0f}% | "
                  if trades_fetched else f"- | {r['zero_vol_frac']*100:.0f}% | "
                  f"{r['notional_med']:,.0f} | {r['gap_p25']:.1f} / {r['gap_med']:.1f} / "
                  f"{r['gap_p75']:.1f} / {r['gap_p95']:.1f} | {r['oi_avg']:,.0f} | "
                  f"{r['funding']} |\n")
            w("\n")
        a_b = [r for r in rows if r['grade'] in ('A', 'B')]
        w("## What this means for the research\n\n")
        w(f"- **{len(a_b)} contracts are fittable** (grade A or B). Everything else is a "
          "description of a few days.\n")
        if a_b:
            w(f"- Longest history: `{a_b[0]['ticker']}` at {a_b[0]['days']:.0f} days.\n")
            thin = [r for r in a_b if r['zero_vol_frac'] > .5]
            if thin:
                w(f"- **{len(thin)} of them trade in under half of their minutes** "
                  f"({', '.join('`'+r['ticker']+'`' for r in thin[:8])}). A quote change with no "
                  "trade is not meaningless -- orders arriving and cancelling move the implied "
                  "clearing price, and that can carry information. What it is not is the *same* "
                  "data-generating process as a minute with flow, so results are reported by "
                  "trade-count regime (0, 1-5, >5) rather than pooled across them.\n")
        w("- Any pooled fit must either restrict to one grade or carry a contract fixed effect. "
          "Pooling a 90-day contract with a 2-day one weights the result entirely toward the "
          "former while appearing to be a statement about the book.\n")
        w("\n## Coverage gaps\n\nA coverage figure below 100% means minutes the exchange returned "
          "no bar for. That is usually a market with no quote rather than an outage, but a run of "
          "them is worth checking before treating the series as continuous.\n")
    print(f"\nwrote {a.out}")
    for g, label in groups.items():
        n = sum(1 for r in rows if r['grade'] == g)
        print(f"  {g}: {n:3d}  {label}")


main()
