#!/usr/bin/env python3
"""Does the Kalshi perpetual basis revert, and does the reversion beat the round trip?

Three things this gets right that are easy to get wrong, each of which flipped the answer:

  P&L is on the PERP PRICE, not on the basis. The basis is (mid - index)/index and it closes
  either because the perp moved or because the index did. Only the first pays you. Scoring the
  basis closing made instruments look profitable that were not.

  Split longs from shorts. Three days of falling crypto makes "short the rich perp" look like a
  strategy when it is a bet on direction. The test is whether the side fighting the market still
  pays: ZEC's index fell 639 bps over the window and its LONGS still made +14.2 bps.

  Subtract the round trip. Maker in at 5 bps, taker out at 12, so 17 bps of the move is gone
  before anything is earned. Most of the gross edge lives below that line.

  python3 scripts/perp_basis.py                # the table
  python3 scripts/perp_basis.py --split        # longs against shorts, per instrument
"""
import argparse, os, random, sqlite3, statistics

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SANE = 'bid>0 and ask>0 and ask<1e9 and reference>1e-9'   # ask<1e9 drops Kalshi's "no offer" sentinel


def boot(xs, draws=3000, seed=5):
    if len(xs) < 20:
        return float('nan'), float('nan')
    r = random.Random(seed)
    m = sorted(sum(r.choice(xs) for _ in range(len(xs))) / len(xs) for _ in range(draws))
    return m[int(.025 * draws)], m[int(.975 * draws)]


def episodes(ser, thr, exit_bps, max_hold):
    """Non-overlapping excursions: enter when |basis| crosses thr, leave when it comes back inside
    exit_bps or the hold runs out. Consecutive quotes above the threshold are ONE trade, not one
    per quote; counting per quote turns a handful of dislocations into hundreds of fake samples."""
    i = 0
    while i < len(ser) - 1:
        ts0, b0, a0, r0, bas0 = ser[i]
        if abs(bas0) < thr:
            i += 1
            continue
        short = bas0 > 0
        entry = a0 if short else b0            # rest at the touch on the favourable side
        out = None
        for j in range(i + 1, min(i + 900, len(ser))):
            if ser[j][0] - ts0 > max_hold:
                break
            if abs(ser[j][4]) <= exit_bps:
                out = j
                break
        if out is None:
            out = min(i + 900, len(ser) - 1)
        _, b1, a1, _, _ = ser[out]
        exitp = b1 if short else a1            # cross to get out
        pnl = (entry - exitp) / entry * 1e4 if short else (exitp - entry) / entry * 1e4
        yield short, pnl
        i = out + 1


def load(conn, ticker):
    rows = list(conn.execute(
        f'select ts,bid,ask,reference from perp_quotes where ticker=? and {SANE} order by ts', (ticker,)))
    return [(ts, b, a, r, (.5 * (b + a) - r) / r * 1e4) for ts, b, a, r in rows], rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--db', default=os.path.join(ROOT, 'data', 'prod.sqlite'))
    ap.add_argument('--maker-bps', type=float, default=5.0)
    ap.add_argument('--taker-bps', type=float, default=12.0)
    ap.add_argument('--thresholds', type=float, nargs='*', default=[20, 30])
    ap.add_argument('--split', action='store_true')
    a = ap.parse_args()
    rt = a.maker_bps + a.taker_bps
    c = sqlite3.connect(f'file:{a.db}?mode=ro', uri=True)
    c.execute('pragma busy_timeout=90000')
    tickers = [r[0] for r in c.execute('select distinct ticker from perp_quotes')]

    print(f'net of a {rt:.0f} bps round trip (maker in {a.maker_bps:.0f}, taker out {a.taker_bps:.0f})\n')
    hdr = (f"  {'ticker':13} {'thr':>4} {'trades':>7} {'gross':>8} {'NET':>8} {'95% CI on net':>20} {'/day':>6}"
           if not a.split else
           f"  {'ticker':13} {'index bps':>10} {'n short':>8} {'short':>8} {'n long':>7} {'long':>8}")
    print(hdr)
    winners = []
    for t in sorted(tickers):
        ser, raw = load(c, t)
        if len(ser) < 2000:
            continue
        span = (raw[-1][0] - raw[0][0]) / 86400
        if a.split:
            S = [p for sh, p in episodes(ser, 10, 5, 3600) if sh]
            L = [p for sh, p in episodes(ser, 10, 5, 3600) if not sh]
            drift = (raw[-1][3] - raw[0][3]) / raw[0][3] * 1e4
            if len(S) + len(L) < 20:
                continue
            print(f'  {t:13} {drift:>+10.0f} {len(S):>8} {(statistics.mean(S) if S else 0):>8.1f} '
                  f'{len(L):>7} {(statistics.mean(L) if L else 0):>8.1f}')
            continue
        for thr in a.thresholds:
            P = [p - rt for _, p in episodes(ser, thr, 5, 3600)]
            if len(P) < 20:
                continue
            lo, hi = boot(P)
            mark = ' <-- clears fees' if lo > 0 else ''
            print(f'  {t:13} {thr:>4.0f} {len(P):>7} {statistics.mean(P)+rt:>8.1f} '
                  f'{statistics.mean(P):>8.1f} [{lo:>+7.1f},{hi:>+7.1f}] {len(P)/span:>6.0f}{mark}')
            if lo > 0:
                winners.append((t, thr, statistics.mean(P), len(P), len(P) / span))
    if a.split:
        print('\n  Both columns positive means the reversion is real. If only the side that agrees')
        print('  with the index move pays, it is a direction bet wearing a basis signal.')
        return
    print()
    if winners:
        for t, thr, m, n, pd in sorted(winners, key=lambda x: -x[2]):
            print(f'  {t} at {thr:.0f} bps: {m:+.1f} bps net on {n} trades, ~{pd:.0f} a day')
        print('\n  Sample sizes here are days, not months, and the entry assumes a maker fill at the')
        print('  touch that nobody promised. Treat this as where to look, not as a backtest.')
    else:
        print('  nothing clears the round trip with its whole interval above zero')


if __name__ == '__main__':
    main()
