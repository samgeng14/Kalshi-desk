#!/usr/bin/env python3
"""Does resting a quote pay, on this desk's own fills?

Kalshi pays makers nothing on quadratic series (BTC/ETH among them), so a maker's entire revenue
is the half-spread he captured. His entire cost is what the mid then does against him. Those two
numbers decide whether market-making is a strategy here, and neither is a model forecast: both
are measurable from fills that already happened.

The split between maker and taker fills matters and is easy to get wrong. A taker crossed because
he wanted to, so the mid moving against him afterwards is partly his own impact; mixing the two
made the adverse selection look far worse than a maker actually faces.

  python3 scripts/maker_edge.py
"""
import argparse, bisect, os, random, sqlite3, statistics

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--db', default=os.path.join(ROOT, 'data', 'prod.sqlite'))
    ap.add_argument('--horizons', type=int, nargs='*', default=[30, 60, 300, 900])
    a = ap.parse_args()
    random.seed(11)
    c = sqlite3.connect(f'file:{a.db}?mode=ro', uri=True)
    c.execute('pragma busy_timeout=90000')

    fills = list(c.execute('select ticker, outcome_side, ts, is_taker from fills order by ts'))
    if not fills:
        raise SystemExit('no fills recorded yet')
    tickers = tuple({f[0] for f in fills})
    quotes = {}
    for t, ts, b, k in c.execute(
            f"select ticker, ts, yes_bid, yes_ask from quotes where ticker in "
            f"({','.join('?' * len(tickers))}) and yes_bid>0 and yes_ask>0 and yes_ask<1", tickers):
        quotes.setdefault(t, []).append((ts, .5 * (b + k), k - b))
    for v in quotes.values():
        v.sort()

    def book_at(t, ts):
        v = quotes.get(t) or []
        if not v:
            return None
        keys = [x[0] for x in v]
        i = bisect.bisect_left(keys, ts)
        best = None
        for j in (i - 1, i):
            if 0 <= j < len(v) and abs(v[j][0] - ts) <= 120:
                if best is None or abs(v[j][0] - ts) < abs(best[0] - ts):
                    best = v[j]
        return best

    def boot(xs, draws=5000):
        m = sorted(sum(random.choice(xs) for _ in range(len(xs))) / len(xs) for _ in range(draws))
        return m[int(.025 * draws)], m[int(.975 * draws)]

    counts = {('taker' if r[0] else 'maker'): r[1] for r in c.execute(
        'select is_taker, count(*) from fills group by 1')}
    print(f'fills: {counts}\n')
    print('Maker fills: the half-spread captured, minus what the mid then did to you.')
    print(f"  {'horizon':>8} {'n':>4} {'half-spread':>12} {'drift':>10} {'net':>10} {'95% CI':>24}  verdict")
    for h in a.horizons:
        net, halves, drifts = [], [], []
        for t, side, ts, taker in fills:
            if taker:
                continue
            b0, b1 = book_at(t, ts), book_at(t, ts + h)
            if not b0 or not b1:
                continue
            drift = (b1[1] - b0[1]) if side == 'yes' else (b0[1] - b1[1])
            halves.append(b0[2] / 2)
            drifts.append(drift)
            net.append(b0[2] / 2 + drift)
        if len(net) < 10:
            print(f'  {h:>7}s {len(net):>4}  too few maker fills at this horizon')
            continue
        lo, hi = boot(net)
        v = 'pays' if lo > 0 else ('loses' if hi < 0 else 'not distinguishable from zero')
        print(f'  {h:>7}s {len(net):>4} {statistics.mean(halves):>12.5f} '
              f'{statistics.mean(drifts):>+10.5f} {statistics.mean(net):>+10.5f} '
              f'[{lo:+.5f}, {hi:+.5f}]  {v}')

    # The selection effect, which is the actionable part.
    filled_half = [book_at(t, ts)[2] / 2 for t, side, ts, taker in fills
                   if not taker and book_at(t, ts)]
    allq = [x[2] / 2 for v in quotes.values() for x in v]
    if filled_half and allq:
        print(f'\n  half-spread when you were filled : {statistics.mean(filled_half):.5f}')
        print(f'  half-spread on the book generally: {statistics.mean(allq):.5f}')
        ratio = statistics.mean(filled_half) / max(statistics.mean(allq), 1e-9)
        if ratio < 0.85:
            print('  You are filled at a materially narrower spread than the book usually shows.')
            print('  That is selection, and the lever is to rest only when the spread is wide.')
        else:
            print('  These are close, so there is no evidence you are being picked off at narrow')
            print('  spreads specifically. Whatever is eating the half-spread is the drift after')
            print('  the fill, not the width at which you get filled.')


if __name__ == '__main__':
    main()
