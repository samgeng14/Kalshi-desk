#!/usr/bin/env python3
"""The v1 book, with every gate, reported without hindsight.

THE ONE REMAINING PIECE OF HINDSIGHT, STATED FIRST. Compositing was chosen after watching the
walk-forward selector fail. "Average rather than pick" is parameter-free and was specified before
being run, but the family SUBSETS (slow-only, residual-only) were picked by looking at their
results. So the headline here is the ALL composite -- every candidate, no subsetting, no
discretion -- and the subsets are reported underneath as description, not as the result.

Gates, in order of severity:
  net_lo    > 0   the net return is distinguishable from zero
  excess_lo > 0   it beats always-long SCALED TO THE BOOK'S OWN VOLATILITY
  stability       positive in most years, and not carried by one
"""
import os, sys, datetime as dt
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import portfolio_core as P

REBAL, WARMUP, OOS_START = 168, 720, 26 * 168


def zs(v):
    v = np.asarray(v, dtype=float); ok = np.isfinite(v)
    if ok.sum() < 3:
        return np.zeros_like(v)
    m, s = v[ok].mean(), v[ok].std()
    out = np.zeros_like(v)
    if s > 1e-12:
        out[ok] = (v[ok] - m) / s
    return out


def sl(res, i):
    return dict(net=res['net'][i:], gross=res['gross'][i:], cost=res['cost'][i:],
                traded=res['traded'][i:], W=None, ts=res['ts'][i:], lev=res['lev'][i:])


def main():
    rets, ts, names = P.load_panel()
    N = len(names)
    print('computing rolling PCA...', file=sys.stderr)
    betas, resid = P.rolling_pca(rets, window=720, n_pc=3, step=24)

    def trail(t, h): return rets[max(0, t - h):t].sum(axis=0)
    def rtrail(t, h):
        R = resid[max(0, t - h):t]
        return np.nansum(R, axis=0) if len(R) else np.zeros(N)

    ALL = [(trail, h) for h in (168, 336, 504, 720, 1440)] + \
          [(rtrail, h) for h in (168, 336, 504, 720, 1440)]

    def composite(t):
        return np.mean([zs(f(t, h)) for f, h in ALL], axis=0)

    # Benchmark: equal-weight always-long, one round trip at inception.
    ow = P.oneway_bps(names)
    wl = np.full(N, 1.0 / N)
    bg = rets[WARMUP:] @ wl
    bc = np.zeros(len(bg)); bc[0] = float((np.abs(wl) * ow).sum())
    bench = dict(net=bg - bc, gross=bg, cost=bc,
                 traded=np.zeros(len(bg)), W=None, ts=ts[-rets.shape[0]:][WARMUP:],
                 lev=np.ones(len(bg)))

    print('\nHEADLINE: ALL composite, weekly rebalance, PC1-neutral, 3 long / 3 short')
    print('out-of-sample span starts 2021-05 (one selection block held back for comparability)\n')
    hdr = ('%-30s %7s %6s %6s %7s %6s %8s %9s %10s'
           % ('variant', 'ann %', 'vol %', 'Shrp', 'maxDD', 'Calm', 'cost/yr',
              'net_lo', 'excess_lo'))
    print(hdr); print('-' * len(hdr))

    out = {}
    for vt in (None, 15.0, 25.0, 35.0):
        res = P.simulate(rets, ts, names, composite, rebalance_h=REBAL, k=3, neutral=True,
                         betas=betas, buffer=0, vol_target_pct=vt, warmup=WARMUP)
        s = sl(res, OOS_START)
        m = P.metrics(s, 'ALL composite' + (' @%d%% vol' % vt if vt else ' (unscaled)'))
        m.update(P.gate(s, bench_net=bench['net'][OOS_START:]))
        out[vt] = (m, s)
        print('%-30s %7.1f %6.1f %6.2f %7.1f %6.2f %8.1f %9.4f %+10.4f'
              % (m['label'][:30], m['ann_pct'], m['vol_pct'], m['sharpe'], m['max_dd_pct'],
                 m['calmar'], m['cost_pct_yr'], m['net_lo'], m['excess_lo']))

    bm = P.metrics(sl(bench, OOS_START), 'always long (equal weight)')
    bm.update(P.gate(sl(bench, OOS_START)))
    print('%-30s %7.1f %6.1f %6.2f %7.1f %6.2f %8.1f %9.4f %10s'
          % (bm['label'][:30], bm['ann_pct'], bm['vol_pct'], bm['sharpe'], bm['max_dd_pct'],
             bm['calmar'], bm['cost_pct_yr'], bm['net_lo'], '-'))

    m, s = out[None]
    yrs = np.array([dt.datetime.utcfromtimestamp(t / 1000).year for t in s['ts']])
    byr = np.array([dt.datetime.utcfromtimestamp(t / 1000).year for t in bench['ts'][OOS_START:]])
    print('\nyear by year, ALL composite against always-long on the same bars')
    print('%-6s %9s %8s %8s %9s %13s' % ('year', 'ann %', 'vol %', 'Sharpe', 'maxDD %',
                                         'always-long %'))
    pos = tot = 0
    for y in sorted(set(yrs)):
        msk = yrs == y
        if msk.sum() < 200:
            continue
        seg = s['net'][msk]
        a = float(seg.mean()) * P.HOURS_PER_YEAR / 100
        v = float(seg.std()) * np.sqrt(P.HOURS_PER_YEAR) / 100
        eq = np.cumsum(seg) / 100
        dd = float(-(eq - np.maximum.accumulate(eq)).min())
        ba = float(bench['net'][OOS_START:][byr == y].mean()) * P.HOURS_PER_YEAR / 100
        print('%-6d %9.1f %8.1f %8.2f %9.1f %13.1f' % (y, a, v, a / v if v > 1e-9 else 0, dd, ba))
        tot += 1; pos += 1 if a > 0 else 0
    print('positive years: %d of %d' % (pos, tot))

    print('\nSUBSETS (chosen by looking at results -- description, not the headline)')
    for label, parts in (('slow only (336/504/720)',
                          [(trail, h) for h in (336, 504, 720)] +
                          [(rtrail, h) for h in (336, 504, 720)]),
                         ('slow residual only',
                          [(rtrail, h) for h in (336, 504, 720)]),
                         ('slow cross-sectional only',
                          [(trail, h) for h in (336, 504, 720)])):
        def fn(t, parts=parts):
            return np.mean([zs(f(t, h)) for f, h in parts], axis=0)
        res = P.simulate(rets, ts, names, fn, rebalance_h=REBAL, k=3, neutral=True,
                         betas=betas, buffer=0, warmup=WARMUP)
        ss = sl(res, OOS_START)
        mm = P.metrics(ss, label)
        mm.update(P.gate(ss, bench_net=bench['net'][OOS_START:]))
        print('%-30s %7.1f %6.1f %6.2f %7.1f %6.2f %8.1f %9.4f %+10.4f'
              % (mm['label'][:30], mm['ann_pct'], mm['vol_pct'], mm['sharpe'], mm['max_dd_pct'],
                 mm['calmar'], mm['cost_pct_yr'], mm['net_lo'], mm['excess_lo']))


if __name__ == '__main__':
    main()
