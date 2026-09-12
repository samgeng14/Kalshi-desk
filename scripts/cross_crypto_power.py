#!/usr/bin/env python3
"""How large an edge would this test actually catch?

The selection null answers "could noise produce this?" It does NOT answer "could this test have
found a real effect?" Those are different questions, and conflating them is how a negative result
gets oversold. In the walk-forward null, no draw out of 300 produced a single specification with
`net_lo > 0` -- 0 false positives in 58,500 specification-draws. That is SPECIFICITY. It says
nothing about power.

So this measures power directly. Take the real predictors and the real returns, add a synthetic
edge of known size -- a multiple `k` of the predictor's own standardised sign, scaled so the
injected per-bar gross return is exactly `k x cost` -- and ask at what `k` each gate starts
firing. The answer is the minimum detectable effect, in the same units as the headline.

A gate that fires only at k = 10 would be useless and the observed zero would mean nothing. A
gate that fires at k = 1.6 means the test would have caught anything meaningfully profitable, and
the observed 1.04 is a real measurement of a signal that does not clear.
"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cross_crypto as C

FOLDS = 8


def slice_for(T):
    e = [int(round(T * k / (FOLDS + 2))) for k in range(FOLDS + 3)]
    return e[2], e[FOLDS + 2]


def main():
    data = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        'data', 'binance')
    assets = sorted(a for a in os.listdir(os.path.join(data, 'klines_1h')) if a != 'SUIUSDT')
    rets, ts, names, err = C.build_panel(data, assets, 720, 'klines_1h')
    if err:
        sys.exit(err)
    focal = 'XRPUSDT'
    fi = names.index(focal)
    oneway = C.KALSHI_ONEWAY_BPS[focal]
    X, y = C.design(rets, fi, n_lags=1)
    T = len(y)
    va, hi = slice_for(T)
    keys = C.day_keys(np.asarray(ts[-T:])[va:hi])
    pred = X[:, fi][va:hi]
    y0 = y[va:hi]

    base = C.evaluate(pred, y0, keys, oneway, 'observed')
    cost = base['trade_cost_bps_per_bar']
    print('%s own momentum 720m, %d pooled bars, %d day-clusters' %
          (focal, base['n'], base['clusters']))
    print('observed  gross %+.2f  cost %.2f  ratio %.3f  net_lo %+.2f  excess_lo %+.2f'
          % (base['gross_bps_per_bar'], cost, base['economic_ratio'],
             base['net_lo'], base['excess_lo']))

    # The injected edge: add k*cost bp to every bar in the direction the predictor points. That
    # is exactly the shape of a real edge of that size, laid on top of the real return series so
    # the noise, the autocorrelation and the fat tails are all still there.
    sgn = np.sign(pred)
    print('\ninjected edge, on top of the real returns')
    print('%-6s %10s %10s %9s %10s %11s %8s %8s'
          % ('k', 'gross', 'ratio', 'net', 'net_lo', 'excess_lo', 'gate2', 'gate3'))
    first2 = first3 = None
    for k in (0.0, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0):
        yk = y0 + sgn * (k * cost)
        r = C.evaluate(pred, yk, keys, oneway, 'k=%.2f' % k)
        g2 = r['net_lo'] is not None and r['net_lo'] > 0
        g3 = g2 and r['excess_lo'] is not None and r['excess_lo'] > 0
        if g2 and first2 is None:
            first2 = k
        if g3 and first3 is None:
            first3 = k
        print('%-6.2f %10.2f %10.3f %9.2f %10.2f %11.2f %8s %8s'
              % (k, r['gross_bps_per_bar'], r['economic_ratio'], r['net_bps_per_bar'],
                 r['net_lo'], r['excess_lo'], 'PASS' if g2 else '-', 'PASS' if g3 else '-'))

    print('\nminimum detectable effect')
    print('  net_lo > 0 first fires at an injected edge of k = %s x cost' % first2)
    print('  all three gates fire at k = %s x cost' % first3)
    # Closed form for the second gate, as a cross-check on the search above.
    hw = (base['gross_hi'] - base['gross_lo']) / 2
    need = cost + hw
    print('  closed form: net_lo > 0 needs gross > cost + half-width = %.2f + %.2f = %.2f bp/bar,'
          % (cost, hw, need))
    print('               i.e. an economic ratio of %.2f. Observed is %.2f.'
          % (need / cost, base['economic_ratio']))
    print('\nInterpretation: the test detects an edge worth %.0f%% of its trading cost. The'
          % ((need / cost - 1) * 100))
    print('observed signal is worth %.0f%% of its trading cost. The zero is a measurement, not a'
          % ((base['economic_ratio'] - 1) * 100))
    print('shrug -- but it also means an edge smaller than %.2f bp/bar would not have been seen.'
          % (need - cost))


if __name__ == '__main__':
    main()
