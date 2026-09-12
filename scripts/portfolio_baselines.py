#!/usr/bin/env python3
"""Baselines first. No model until these are on the table.

The lesson this project has paid for twice: the first result is never a fitted model. It is the
table of things a model would have to beat. Every row here is a rule with no free parameters,
run through the same simulator, the same Kalshi cost model and the same gates.

The two that matter most are the constant positions. Equal-weight long returned +191% in 2021
and -151% in 2022; any book that quietly rides that beta will look excellent on the wrong half
of the sample. `excess_lo` against the better constant is what catches it.
"""
import os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import portfolio_core as P
import cross_crypto as C

REBAL = 168          # weekly: 13%/yr in costs against 187% at 12h. See portfolio_feasibility.
WARMUP = 720         # one PCA window


def build_scores(rets, resid, names):
    """Score functions, each mapping a bar index to a cross-sectional score vector."""
    T, N = rets.shape

    def trail(t, h):
        return rets[max(0, t - h):t].sum(axis=0)

    def resid_trail(t, h):
        R = resid[max(0, t - h):t]
        return np.nansum(R, axis=0) if len(R) else np.zeros(N)

    return {
        'xs momentum 168h': lambda t: trail(t, 168),
        'xs momentum 336h': lambda t: trail(t, 336),
        'xs momentum 504h': lambda t: trail(t, 504),
        'xs momentum 720h': lambda t: trail(t, 720),
        'xs momentum 1440h': lambda t: trail(t, 1440),
        'xs reversal 168h': lambda t: -trail(t, 168),
        'xs reversal 24h': lambda t: -trail(t, 24),
        'residual momentum 336h': lambda t: resid_trail(t, 336),
        'residual momentum 168h': lambda t: resid_trail(t, 168),
        'residual reversion 168h': lambda t: -resid_trail(t, 168),
        'residual reversion 24h': lambda t: -resid_trail(t, 24),
        'random (control)': None,     # filled in below with a fixed seed
    }


def constant_book(rets, ts, names, side=1.0):
    """Always-long or always-short equal weight, paying one round trip at the start."""
    T, N = rets.shape
    ow = P.oneway_bps(names)
    w = np.full(N, side / N)
    gross = rets[WARMUP:] @ w
    cost = np.zeros(len(gross))
    cost[0] = float((np.abs(w) * ow).sum())
    return dict(net=gross - cost, gross=gross, cost=cost,
                traded=np.concatenate([[float(np.abs(w).sum())], np.zeros(len(gross) - 1)]),
                W=None, ts=ts[-T:][WARMUP:], lev=np.ones(len(gross)))


def main():
    rets, ts, names = P.load_panel()
    print('panel: %d assets, %d hourly bars, %s .. %s'
          % (len(names), rets.shape[0],
             __import__('datetime').datetime.utcfromtimestamp(ts[0] / 1000).strftime('%Y-%m-%d'),
             __import__('datetime').datetime.utcfromtimestamp(ts[-1] / 1000).strftime('%Y-%m-%d')))
    print('rebalance every %dh; Kalshi one-way %.2f-%.2f bps\n'
          % (REBAL, P.oneway_bps(names).min(), P.oneway_bps(names).max()))

    print('computing rolling PCA (720h trailing window, refit daily)...', file=sys.stderr)
    betas, resid = P.rolling_pca(rets, window=720, n_pc=3, step=24)

    # The two constants, and the better of them, which every strategy has to beat.
    cl = constant_book(rets, ts, names, +1.0)
    cs = constant_book(rets, ts, names, -1.0)
    best_const = cl if cl['net'].mean() >= cs['net'].mean() else cs
    const_side = 'long' if best_const is cl else 'short'

    scores = build_scores(rets, resid, names)
    rng = np.random.default_rng(4)
    fixed_noise = rng.normal(size=(rets.shape[0], len(names)))
    scores['random (control)'] = lambda t: fixed_noise[t]

    rows = []
    for neutral, buf, vt in ((False, 0, None), (True, 0, None), (True, 2, None),
                             (True, 2, 200.0)):
        for nm, fn in scores.items():
            res = P.simulate(rets, ts, names, fn, rebalance_h=REBAL, k=3,
                             neutral=neutral, betas=betas, buffer=buf,
                             vol_target_bps=vt, warmup=WARMUP)
            m = P.metrics(res, nm)
            g = P.gate(res, bench_net=best_const['net'])
            m.update(g, neutral=neutral, band=buf, voltgt=vt)
            rows.append(m)

    for c in (cl, cs):
        m = P.metrics(c, 'ALWAYS ' + ('LONG' if c is cl else 'SHORT'))
        m.update(P.gate(c), neutral=False, band=0, voltgt=None)
        rows.insert(0, m)

    hdr = ('%-24s %-4s %-4s %-5s %7s %6s %6s %7s %6s %8s %8s %9s'
           % ('strategy', 'neu', 'hys', 'vtgt', 'ann %', 'vol %', 'Shrp', 'maxDD',
              'Calm', 'cost/yr', 'net_lo', 'excess_lo'))
    print(hdr)
    print('-' * len(hdr))
    for r in rows:
        print('%-24s %-4s %-4s %-5s %7.1f %6.1f %6.2f %7.1f %6.2f %8.1f %8.4f %9s'
              % (r['label'][:24],
                 'Y' if r['neutral'] else 'N',
                 (str(r['band']) if r['band'] else '-'),
                 ('%.0f' % r['voltgt']) if r.get('voltgt') else '-',
                 r['ann_pct'], r['vol_pct'], r['sharpe'], r['max_dd_pct'], r['calmar'],
                 r['cost_pct_yr'],
                 r['net_lo'] if r['net_lo'] is not None else float('nan'),
                 ('%+.4f' % r['excess_lo']) if r.get('excess_lo') is not None else '-'))

    bm = P.metrics(best_const)
    print('\nbenchmark: ALWAYS %s, %.1f%%/yr at %.1f%% vol, maxDD %.1f%%, Sharpe %.2f.'
          % (const_side.upper(), bm['ann_pct'], bm['vol_pct'], bm['max_dd_pct'], bm['sharpe']))
    print('excess_lo compares each row against that benchmark SCALED TO THE ROW\'S OWN VOLATILITY,')
    print('so the question is "at equal risk, does this beat holding the market" rather than')
    print('"does a 30%%-vol book out-return an 81%%-vol one".')
    passed = [r for r in rows if r.get('net_lo') is not None and r['net_lo'] > 0
              and r.get('excess_lo') is not None and r['excess_lo'] > 0]
    print('\nGATES: %d of %d rows have net_lo > 0 AND excess_lo > 0.' % (len(passed), len(rows)))
    for r in passed:
        print('   %s (neutral=%s hys=%s)  ann %.1f%%  Sharpe %.2f  maxDD %.1f%%'
              % (r['label'], r['neutral'], r['band'], r['ann_pct'], r['sharpe'], r['max_dd_pct']))
    if not passed:
        print('   none. No model is warranted until a no-parameter rule clears, or until the')
        print('   reason none does is understood.')


if __name__ == '__main__':
    main()
