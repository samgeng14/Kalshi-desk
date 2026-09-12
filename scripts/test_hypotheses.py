#!/usr/bin/env python3
"""Run the hypotheses in docs/hypotheses.md against what the desk has actually recorded.

Every test here follows the same three rules, because breaking any one of them turns a null
result into a finding:

  Price at a HORIZON, not at the last tick. The final quote before a market stops trading is
  taken when the outcome is nearly determined, so "the price was wrong" there often just means
  "the book had not caught up with a settled fact". Each test is reported at several horizons and
  a claim that only survives at zero minutes has not survived.

  The EVENT is the unit. Brackets of one event are mutually exclusive: selling every cheap leg of
  one hour is a single bet, not ten independent ones. Averaging over legs understates the
  variance and inflates the sample.

  The EXECUTABLE price, with the fee. Selling YES means lifting the NO offer at 1 - yes_bid, not
  resting at 1 - yes_ask. Using the maker price made the longshot result look four times better
  than it is.

  python3 scripts/test_hypotheses.py                 # every implemented test
  python3 scripts/test_hypotheses.py --only LONGSHOT
"""
import argparse, collections, math, os, sqlite3, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TAKER_FEE = 0.07          # Kalshi quadratic: fee per contract = 0.07 * p * (1 - p)


def mean_ci(xs):
    n = len(xs)
    if n == 0:
        return 0.0, 0.0, 0.0
    m = sum(xs) / n
    if n < 2:
        return m, m, m
    var = sum((x - m) ** 2 for x in xs) / (n - 1)
    se = math.sqrt(var / n)
    return m, m - 1.96 * se, m + 1.96 * se


def priced_at(conn, minutes):
    """One quote per settled market: the earliest one at least `minutes` before it stopped."""
    q = f"""select m.ticker, m.series_ticker, m.event_ticker, m.result, q.yes_bid, q.yes_ask
            from markets m join quotes q on q.ticker = m.ticker
            where m.result in ('yes','no') and m.close_time is not null
              and q.yes_bid > 0 and q.yes_ask > 0 and q.yes_ask < 1
              and strftime('%s', m.close_time) - q.ts >= {int(minutes) * 60}
            group by m.ticker
            having min(strftime('%s', m.close_time) - q.ts)"""
    return list(conn.execute(q))


def longshot(conn, horizons=(0, 5, 15, 30), band=(.02, .10)):
    """Are cheap contracts overpriced by enough to pay for selling them?

    The trade priced here is: sell YES on every leg of an event whose mid sits in `band`, as a
    taker, held to settlement. That is the executable form of SPORT-3, FIN-2 and the tail half of
    CRYPTO-3.
    """
    print(f"\n[LONGSHOT] selling YES in the {band[0]:.0%}-{band[1]:.0%} band, taker, held to settlement")
    print(f"  {'horizon':>8} {'legs':>6} {'events':>7} {'priced':>8} {'settled':>8} "
          f"{'P&L/event':>10} {'95% CI':>22} {'on capital':>11}  verdict")
    out = []
    for mins in horizons:
        rows = priced_at(conn, mins)
        cheap = [r for r in rows if band[0] <= .5 * (r[4] + r[5]) < band[1]]
        if len(cheap) < 40:
            print(f"  {mins:>7}m {len(cheap):>6}  too few legs to say anything")
            continue
        events = collections.defaultdict(list)
        for r in cheap:
            events[r[2]].append(r)
        pnl, capital = [], 0.0
        for legs in events.values():
            got = 0.0
            for r in legs:
                cost = 1 - r[4]                     # lift the NO offer
                fee = TAKER_FEE * cost * (1 - cost)
                got += (0.0 if r[3] == 'yes' else 1.0) - cost - fee
                capital += cost
            pnl.append(got)
        m, lo, hi = mean_ci(pnl)
        priced = sum(.5 * (r[4] + r[5]) for r in cheap) / len(cheap)
        settled = sum(1 for r in cheap if r[3] == 'yes') / len(cheap)
        verdict = 'pays' if lo > 0 else ('loses' if hi < 0 else 'not significant')
        ci = f'[{lo:+.4f}, {hi:+.4f}]'
        print(f"  {mins:>7}m {len(cheap):>6} {len(pnl):>7} {priced:>8.4f} {settled:>8.4f} "
              f"{m:>+10.4f} {ci:>22} {sum(pnl)/capital:>+10.3%}  {verdict}")
        out.append((mins, m, lo, hi))
    if out:
        near = [x for x in out if x[0] <= 5 and x[2] > 0]
        far = [x for x in out if x[0] >= 30]
        print()
        if near and far and far[0][3] < 0:
            print("  Reading: the edge exists only in the last minutes and REVERSES further out. That is")
            print("  the shape of a book lagging a nearly-decided outcome, not of longshots being")
            print("  systematically overpriced. Tradeable only where you can lift an offer in the final")
            print("  minutes, which is the hardest place to get filled.")
        elif near:
            print("  Reading: positive near the close. Check fill rates before believing the return.")
        else:
            print("  Reading: no edge survives at an executable price.")
    return out


TESTS = {'LONGSHOT': longshot}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--db', default=os.path.join(ROOT, 'data', 'prod.sqlite'))
    ap.add_argument('--only', help='run one test by key')
    a = ap.parse_args()
    conn = sqlite3.connect(f'file:{a.db}?mode=ro', uri=True)
    conn.execute('pragma busy_timeout=90000')
    settled = conn.execute("select count(*) from markets where result in ('yes','no')").fetchone()[0]
    print(f'{settled:,} settled markets on disk')
    for key, fn in TESTS.items():
        if a.only and a.only.upper() != key:
            continue
        fn(conn)
    print('\nNot implemented here, and why: SPORT-2 and ECON-2 need an outside line (closing odds,')
    print('FedWatch); FIN-1 needs a real-time index feed; MICRO-1, SPORT-4 and WX-3 need')
    print('receipt-timestamped high-frequency data the desk does not record yet. Those are')
    print('collection projects, not analysis ones.')


if __name__ == '__main__':
    main()
