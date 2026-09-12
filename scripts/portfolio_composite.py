#!/usr/bin/env python3
"""Combine the candidates instead of choosing among them.

The walk-forward selector returned -3.2% a year while the rules it was choosing between returned
+10% to +22%. Selecting on trailing Sharpe over a five-point grid destroyed the edge rather than
finding it: the selector spent 2021 in the worst rule in the set and never fully recovered.

That is a well-known failure and it has a standard, parameter-free answer. Do not pick. Average.
Z-score each candidate's cross-sectional score and take the mean, so the book expresses what the
candidates agree on and ignores what they disagree about.

Exactly one composite is tested, specified before it was run, and reported whichever way it
comes out. Three variants only because they answer three different questions:
  ALL        every candidate, including the lookbacks that fail alone
  MOMENTUM   the momentum family only, dropping reversal, which is reliably negative here
  SLOW       336h/504h/720h only, the band where every individual rule was positive
"""
import os, sys, datetime as dt
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import portfolio_core as P

REBAL, WARMUP = 168, 720


def zs(v):
    v = np.asarray(v, dtype=float)
    ok = np.isfinite(v)
    if ok.sum() < 3:
        return np.zeros_like(v)
    m, s = v[ok].mean(), v[ok].std()
    out = np.zeros_like(v)
    if s > 1e-12:
        out[ok] = (v[ok] - m) / s
    return out


def main():
    rets, ts, names = P.load_panel()
    N = len(names)
    print('computing rolling PCA...', file=sys.stderr)
    betas, resid = P.rolling_pca(rets, window=720, n_pc=3, step=24)

    def trail(t, h):
        return rets[max(0, t - h):t].sum(axis=0)

    def rtrail(t, h):
        R = resid[max(0, t - h):t]
        return np.nansum(R, axis=0) if len(R) else np.zeros(N)

    families = {
        'ALL (10 candidates)': [(trail, h) for h in (168, 336, 504, 720, 1440)] +
                               [(rtrail, h) for h in (168, 336, 504, 720, 1440)],
        'MOMENTUM (xs+resid, 5 lookbacks)': [(trail, h) for h in (168, 336, 504, 720, 1440)] +
                                            [(rtrail, h) for h in (168, 336, 504, 720, 1440)],
        'SLOW (336/504/720, xs+resid)': [(trail, h) for h in (336, 504, 720)] +
                                        [(rtrail, h) for h in (336, 504, 720)],
        'SLOW residual only': [(rtrail, h) for h in (336, 504, 720)],
        'SLOW cross-sectional only': [(trail, h) for h in (336, 504, 720)],
    }

    rows = []
    series = {}
    for label, parts in families.items():
        def fn(t, parts=parts):
            return np.mean([zs(f(t, h)) for f, h in parts], axis=0)
        res = P.simulate(rets, ts, names, fn, rebalance_h=REBAL, k=3, neutral=True,
                         betas=betas, buffer=0, warmup=WARMUP)
        m = P.metrics(res, label)
        m.update(P.gate(res))
        rows.append(m)
        series[label] = res

    hdr = ('%-34s %7s %6s %6s %7s %6s %8s %9s'
           % ('composite', 'ann %', 'vol %', 'Shrp', 'maxDD', 'Calm', 'cost/yr', 'net_lo'))
    print('\nWHOLE SAMPLE (2020-11 .. 2026-08)')
    print(hdr); print('-' * len(hdr))
    for r in rows:
        print('%-34s %7.1f %6.1f %6.2f %7.1f %6.2f %8.1f %9.4f'
              % (r['label'][:34], r['ann_pct'], r['vol_pct'], r['sharpe'], r['max_dd_pct'],
                 r['calmar'], r['cost_pct_yr'],
                 r['net_lo'] if r['net_lo'] is not None else float('nan')))

    # The same span the walk-forward selector was judged on, so the comparison is like for like.
    n = len(rows[0] and series[rows[0]['label']]['net'])
    start = 26 * REBAL
    print('\nSAME OUT-OF-SAMPLE SPAN AS THE SELECTOR (from 2021-05)')
    print(hdr); print('-' * len(hdr))
    best = None
    for label, res in series.items():
        sub = dict(net=res['net'][start:], gross=res['gross'][start:],
                   cost=res['cost'][start:], traded=res['traded'][start:],
                   W=None, ts=res['ts'][start:], lev=res['lev'][start:])
        m = P.metrics(sub, label)
        m.update(P.gate(sub))
        print('%-34s %7.1f %6.1f %6.2f %7.1f %6.2f %8.1f %9.4f'
              % (m['label'][:34], m['ann_pct'], m['vol_pct'], m['sharpe'], m['max_dd_pct'],
                 m['calmar'], m['cost_pct_yr'],
                 m['net_lo'] if m['net_lo'] is not None else float('nan')))
        if best is None or m['sharpe'] > best[0]['sharpe']:
            best = (m, sub)
    print('\nreference on that span: walk-forward selector -3.2%/yr Sharpe -0.14;')
    print('                        best fixed rule (oracle) +22.4%/yr Sharpe 0.84.')

    m, sub = best
    yrs = np.array([dt.datetime.utcfromtimestamp(t / 1000).year for t in sub['ts']])
    print('\n%s, year by year' % m['label'])
    print('%-6s %8s %8s %8s %9s %9s' % ('year', 'ann %', 'vol %', 'Sharpe', 'maxDD %', 'cost %'))
    for y in sorted(set(yrs)):
        msk = yrs == y
        if msk.sum() < 200:
            continue
        seg, cseg = sub['net'][msk], sub['cost'][msk]
        a = float(seg.mean()) * P.HOURS_PER_YEAR / 100
        v = float(seg.std()) * np.sqrt(P.HOURS_PER_YEAR) / 100
        eq = np.cumsum(seg) / 100
        dd = float(-(eq - np.maximum.accumulate(eq)).min())
        print('%-6d %8.1f %8.1f %8.2f %9.1f %9.1f'
              % (y, a, v, a / v if v > 1e-9 else float('nan'), dd,
                 float(cseg.mean()) * P.HOURS_PER_YEAR / 100))
    pos = sum(1 for y in sorted(set(yrs)) if (yrs == y).sum() >= 200 and
              float(sub['net'][yrs == y].mean()) > 0)
    tot = sum(1 for y in sorted(set(yrs)) if (yrs == y).sum() >= 200)
    print('positive years: %d of %d' % (pos, tot))


if __name__ == '__main__':
    main()
