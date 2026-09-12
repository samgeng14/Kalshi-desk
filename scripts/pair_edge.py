#!/usr/bin/env python3
"""Does the Kalshi/Polymarket pair actually lock, at prices you could have traded?

The pair story is that the same outcome trades on two venues, so buying the cheap side and
selling the rich one locks the difference whatever settles. That is true arithmetic and says
nothing about whether the difference is ever bigger than the cost of crossing to get it.

This prices both routes on every moment where both books were quoting within 20 seconds of each
other, once per pair per minute:

  route A   buy Kalshi YES, sell Polymarket YES
  route B   buy Polymarket YES, sell Kalshi YES

and both execution styles, because they are completely different trades:

  taker   cross both spreads. Kalshi charges 0.07*p*(1-p), Polymarket 0.06*p*(1-p).
  maker   rest both legs. Kalshi charges makers nothing on quadratic series and Polymarket PAYS
          its makers 0.0125*p*(1-p) - but a resting order is an offer, not a fill.

  python3 scripts/pair_edge.py
"""
import argparse, os, sqlite3, statistics

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
K_TAKER, P_TAKER, P_MAKER_REBATE = 0.07, 0.06, 0.0125


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--db', default=os.path.join(ROOT, 'data', 'prod.sqlite'))
    ap.add_argument('--window', type=int, default=20, help='seconds the two quotes may differ by')
    a = ap.parse_args()
    c = sqlite3.connect(f'file:{a.db}?mode=ro', uri=True)
    c.execute('pragma busy_timeout=90000')
    rows = list(c.execute(f"""
        select p.kalshi_ticker, p.slug, p.league, p.pm_yes_is_kalshi_yes,
               k.yes_bid, k.yes_ask, q.bid, q.ask
        from poly_pairs p
        join poly_quotes q on q.slug = p.slug
        join quotes k on k.ticker = p.kalshi_ticker and abs(k.ts - q.ts) <= {a.window}
        where k.yes_bid>0 and k.yes_ask>0 and k.yes_ask<1
          and q.bid>0 and q.ask>0 and q.ask<1
        group by p.kalshi_ticker, q.ts/60"""))
    if not rows:
        raise SystemExit('no moment where both venues were quoting the same outcome')

    taker, maker = [], []
    for _, _, _, same, kb, ka, pb, pa in rows:
        py_bid = pb if same else 1 - pa           # Polymarket, expressed as "Kalshi YES"
        py_ask = pa if same else 1 - pb
        kf = lambda p: K_TAKER * p * (1 - p)
        pf = lambda p: P_TAKER * p * (1 - p)
        rb = lambda p: -P_MAKER_REBATE * p * (1 - p)
        a_t = ka + (1 - py_bid) + kf(ka) + pf(1 - py_bid)
        b_t = py_ask + (1 - kb) + kf(1 - kb) + pf(py_ask)
        a_m = ka + (1 - py_bid) + rb(1 - py_bid)  # Kalshi makers pay nothing on quadratic series
        b_m = py_ask + (1 - kb) + rb(py_ask)
        taker.append(1 - min(a_t, b_t))
        maker.append(1 - min(a_m, b_m))

    n = len(rows)
    print(f'{n:,} paired observations across {len({r[0] for r in rows})} Kalshi markets\n')
    for name, xs in (('taker, cross both', taker), ('maker, rest both', maker)):
        locked = sum(1 for x in xs if x > 0)
        print(f'  {name:20} locked {locked:>5,} / {n:,} = {locked/n:6.2%}   '
              f'median {statistics.median(xs):+.4f}   best {max(xs):+.4f}')

    best_m = max(maker)
    print(f"""
What this means. Crossing both spreads locks a profit on {sum(1 for x in taker if x>0)/n:.2%} of observations, and the median
is {statistics.median(taker):+.4f} per dollar of pair: the taker arb does not exist.

Resting both legs looks better because Polymarket pays its makers, but a resting order is not a
fill. The best locked maker spread here is {best_m:+.4f} per dollar. If one leg fills and the other
does not, you are not arbitraged, you are outright on a game with about a dollar at risk. Unwinding
that costs the spread, one to two cents. So one miss undoes roughly {max(1,int(0.015/max(best_m,1e-9)))} completed pairs, and the
strategy needs both resting legs to fill on two venues that promise nothing about either.

That is the number to beat before any of this goes live: not the spread, the joint fill rate.""")


if __name__ == '__main__':
    main()
