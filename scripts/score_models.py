#!/usr/bin/env python3
"""Score every strategy's estimates against the price they were quoted against.

The gate for lane 1 is not "does the model look clever", it is "on markets that have settled, was
the model's number closer to the outcome than the market's own mid". This prints that comparison
and nothing else.

Two things it is careful about, because both were wrong before:

  One sample per MARKET. `model_fairs` holds a row per cycle, so a contract quoted all day
  contributes a hundred rows that share a single outcome. Averaging over rows inflates the sample
  count and over-weights whatever was quoted longest: 107 settled weather markets were being
  reported as 11,390 independent samples.

  An interval, not a point. "No worse than the market" on a point estimate is a null result
  wearing a hat. A model is promoted only when the whole 95% interval on the paired difference
  sits below zero.

  python3 scripts/score_models.py                 # last fair before settlement
  python3 scripts/score_models.py --pick median   # a mid-life fair, less favourable to the model
"""
import argparse, os, random, sqlite3, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def per_market(conn, strategy, pick):
    q = """select f.ticker, f.ts, f.p, f.yes_bid, f.yes_ask, m.result
           from model_fairs f join markets m on m.ticker = f.ticker
           where f.strategy = ? and m.result in ('yes','no')
             and f.yes_bid > 0 and f.yes_ask > 0 and f.yes_ask < 1"""
    by = {}
    for t, ts, p, b, a, res in conn.execute(q, (strategy,)):
        by.setdefault(t, []).append((ts, p, b, a, res))
    out = []
    for t, v in by.items():
        v.sort()
        ts, p, b, a, res = v[-1] if pick == 'last' else v[len(v) // 2]
        y = 1.0 if res == 'yes' else 0.0
        mid = .5 * (b + a)
        out.append((t, (p - y) ** 2, (mid - y) ** 2, len(v)))
    return out


def bootstrap(diffs, draws=4000, seed=7):
    if len(diffs) < 2:
        return 0.0, 0.0
    rnd = random.Random(seed)
    means = []
    for _ in range(draws):
        s = [diffs[rnd.randrange(len(diffs))] for _ in range(len(diffs))]
        means.append(sum(s) / len(s))
    means.sort()
    return means[int(.025 * draws)], means[int(.975 * draws)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--db', default=os.path.join(ROOT, 'data', 'prod.sqlite'))
    ap.add_argument('--pick', choices=['last', 'median'], default='last')
    ap.add_argument('--min-markets', type=int, default=100)
    a = ap.parse_args()

    conn = sqlite3.connect(f'file:{a.db}?mode=ro', uri=True)
    conn.execute('pragma busy_timeout=25000')
    strategies = [r[0] for r in conn.execute(
        "select distinct strategy from model_fairs where strategy is not null and strategy != ''")]
    if not strategies:
        sys.exit('no model fairs recorded yet')

    print(f'fair chosen per market: {a.pick}    promotion needs {a.min_markets}+ markets '
          f'and a 95% interval entirely below zero\n')
    print(f"{'strategy':20} {'mkts':>6} {'fairs':>7} {'model':>7} {'market':>7} "
          f"{'diff':>8} {'95% CI on the difference':>26}  verdict")
    promoted = []
    for s in sorted(strategies):
        pm = per_market(conn, s, a.pick)
        if not pm:
            print(f'{s:20} {"-":>6} {"-":>7}  no settled market with a two-sided book')
            continue
        n = len(pm)
        fairs = sum(x[3] for x in pm)
        bm = sum(x[1] for x in pm) / n
        bk = sum(x[2] for x in pm) / n
        diffs = [x[1] - x[2] for x in pm]        # model minus market; negative = model better
        lo, hi = bootstrap(diffs)
        if n < a.min_markets:
            verdict = f'only {n} markets'
        elif hi < 0:
            verdict = 'PROMOTE: better than the price'
            promoted.append(s)
        elif lo > 0:
            verdict = 'the price is better'
        else:
            verdict = 'indistinguishable from the price'
        ci = f'[{lo:+.4f}, {hi:+.4f}]'
        print(f'{s:20} {n:>6,} {fairs:>7,} {bm:>7.4f} {bk:>7.4f} {bm-bk:>+8.4f} {ci:>26}  {verdict}')

    print()
    if promoted:
        print('promote to lane 1: ' + ', '.join(promoted))
    else:
        print('nothing has beaten the price on settled markets. Lane 1 should size these flat, '
              'not by Kelly, and that is what an undemonstrated calibration now does.')


if __name__ == '__main__':
    main()
