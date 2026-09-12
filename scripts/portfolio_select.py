#!/usr/bin/env python3
"""Choose the lookback and the model out of sample, then see what survives.

The baseline table has a sharp peak: residual momentum returns +5% a year at a 168-hour lookback,
+28% at 336, +30% at 504 and +1% at 1440. A peak that sharp over a grid of five, chosen on the
whole sample, is exactly the shape a selection artifact makes. So the lookback and the model are
not allowed to be chosen with hindsight here.

EXPANDING-WINDOW SELECTION. Every `reselect_weeks` weeks, rank every candidate on everything
observed so far by net Sharpe, take the best, and trade it for the next block. Pool the blocks.
Nothing in the pooled series was chosen using its own data.

Two controls run alongside:
  - ORACLE, which picks the best candidate with full hindsight. The gap between oracle and
    walk-forward is the price of not knowing.
  - FIXED, each candidate held for the whole period, so the reader can see whether the selector
    beat simply committing to one rule at the start.
"""
import os, sys, datetime as dt
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import portfolio_core as P
import cross_crypto as C

REBAL = 168
WARMUP = 720
RESELECT_W = 26          # half a year


def candidates(rets, resid, N):
    def trail(t, h):
        return rets[max(0, t - h):t].sum(axis=0)

    def rtrail(t, h):
        R = resid[max(0, t - h):t]
        return np.nansum(R, axis=0) if len(R) else np.zeros(N)

    c = {}
    for h in (168, 336, 504, 720, 1440):
        c['xs mom %dh' % h] = (lambda hh: (lambda t: trail(t, hh)))(h)
        c['resid mom %dh' % h] = (lambda hh: (lambda t: rtrail(t, hh)))(h)
    return c


def main():
    rets, ts, names = P.load_panel()
    N = len(names)
    print('panel %d assets, %s .. %s'
          % (N, dt.datetime.utcfromtimestamp(ts[0] / 1000).strftime('%Y-%m-%d'),
             dt.datetime.utcfromtimestamp(ts[-1] / 1000).strftime('%Y-%m-%d')))
    print('computing rolling PCA...', file=sys.stderr)
    betas, resid = P.rolling_pca(rets, window=720, n_pc=3, step=24)
    cands = candidates(rets, resid, N)

    # Run every candidate once over the whole history; selection then just slices these series.
    runs = {}
    for nm, fn in cands.items():
        runs[nm] = P.simulate(rets, ts, names, fn, rebalance_h=REBAL, k=3,
                              neutral=True, betas=betas, buffer=0, warmup=WARMUP)
    n = len(next(iter(runs.values()))['net'])
    rts = next(iter(runs.values()))['ts']
    block = RESELECT_W * REBAL

    # Walk-forward selection.
    wf = np.zeros(n)
    picks = []
    start = block                      # need at least one block of history to select on
    for b0 in range(start, n, block):
        b1 = min(b0 + block, n)
        best, best_s = None, -9e9
        for nm, r in runs.items():
            past = r['net'][:b0]
            sd = float(past.std())
            sh = float(past.mean()) / sd * np.sqrt(P.HOURS_PER_YEAR) if sd > 1e-12 else -9e9
            if sh > best_s:
                best, best_s = nm, sh
        wf[b0:b1] = runs[best]['net'][b0:b1]
        picks.append((dt.datetime.utcfromtimestamp(rts[b0] / 1000).strftime('%Y-%m'),
                      best, best_s))
    wf_slice = slice(start, n)

    def show(label, net, ts_):
        res = dict(net=net, gross=net, cost=np.zeros(len(net)),
                   traded=np.zeros(len(net)), W=None, ts=ts_, lev=np.ones(len(net)))
        m = P.metrics(res, label)
        g = P.gate(res)
        return m, g

    print('\nselection path (chosen on data before each block only)')
    print('%-10s %-18s %8s' % ('from', 'picked', 'train Sharpe'))
    for d, nm, sh in picks:
        print('%-10s %-18s %8.2f' % (d, nm, sh))

    # Oracle and fixed comparisons on the SAME out-of-sample span.
    rows = []
    m, g = show('WALK-FORWARD SELECT', wf[wf_slice], rts[wf_slice])
    m.update(g); rows.append(m)
    ora_nm = max(runs, key=lambda k: runs[k]['net'][wf_slice].mean() /
                 (runs[k]['net'][wf_slice].std() + 1e-12))
    m, g = show('ORACLE (%s)' % ora_nm, runs[ora_nm]['net'][wf_slice], rts[wf_slice])
    m.update(g); rows.append(m)
    for nm in sorted(runs):
        m, g = show('fixed: ' + nm, runs[nm]['net'][wf_slice], rts[wf_slice])
        m.update(g); rows.append(m)

    hdr = '%-26s %7s %6s %6s %7s %6s %9s' % ('', 'ann %', 'vol %', 'Shrp', 'maxDD', 'Calm', 'net_lo')
    print('\n' + hdr); print('-' * len(hdr))
    for r in rows:
        print('%-26s %7.1f %6.1f %6.2f %7.1f %6.2f %9.4f'
              % (r['label'][:26], r['ann_pct'], r['vol_pct'], r['sharpe'],
                 r['max_dd_pct'], r['calmar'],
                 r['net_lo'] if r['net_lo'] is not None else float('nan')))

    # Year by year for the walk-forward series -- "not dependent on one year" is a stated
    # success criterion, so it gets its own table rather than a sentence.
    yrs = np.array([dt.datetime.utcfromtimestamp(t / 1000).year for t in rts[wf_slice]])
    net = wf[wf_slice]
    print('\nwalk-forward selection, year by year')
    print('%-6s %8s %8s %8s %9s' % ('year', 'ann %', 'vol %', 'Sharpe', 'maxDD %'))
    for y in sorted(set(yrs)):
        msk = yrs == y
        if msk.sum() < 200:
            continue
        seg = net[msk]
        a = float(seg.mean()) * P.HOURS_PER_YEAR / 100
        v = float(seg.std()) * np.sqrt(P.HOURS_PER_YEAR) / 100
        eq = np.cumsum(seg) / 100
        dd = float(-(eq - np.maximum.accumulate(eq)).min())
        print('%-6d %8.1f %8.1f %8.2f %9.1f' % (y, a, v, a / v if v > 1e-9 else float('nan'), dd))


if __name__ == '__main__':
    main()
