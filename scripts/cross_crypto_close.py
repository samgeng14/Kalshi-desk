#!/usr/bin/env python3
"""Closing tests for the cross-crypto lane.

The first null test asked one question: is the MAXIMUM of 120 economic ratios unusual? It is
not (p = 0.135). But a max-only test is weak against a DIFFUSE effect -- a real mechanism that
lifts every specification a little would not necessarily produce a standout maximum. Closing
the lane on the max alone would be the same class of error as closing it on a single spec.

So this script adds two things.

  1. The diffuse test. Under the same circular-shift null, compare the MEAN and the MEDIAN
     economic ratio across all 120 specifications, and the COUNT that clear 1.0x. If a real
     mechanism is present but spread thin, those aggregate statistics shift even when the max
     does not.

  2. Sign stability. For the one specification that cleared the hurdle (SOL own momentum at
     720m), split the held-out slice into consecutive thirds and report the gross per-trade
     return in each. The reviewer's gate requires a stable sign across periods; a signal that
     lives in one third of a 160-bar test slice is a period, not an edge.
"""
import os, sys, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cross_crypto as C

BARS = [60, 240, 720]


def spec_ratios(panels, shift=0):
    """Every specification's economic ratio, in a fixed order. shift=0 is the real data."""
    out, labels = [], []
    for bar, (rets, ts, names) in panels.items():
        r = np.roll(rets, shift, axis=0) if shift else rets
        for fi, focal in enumerate(names):
            oneway = C.KALSHI_ONEWAY_BPS.get(focal, C.DEFAULT_ONEWAY)
            X, y = C.design(r, fi, n_lags=1)
            if shift:
                y = np.roll(y, shift)
            T = len(y)
            va = int(T * (C.TRAIN_FRAC + C.VALID_FRAC))
            keys = C.day_keys(ts[-T:][va:T])
            lag = X[:, :len(names)]
            bt = names.index('BTCUSDT') if 'BTCUSDT' in names else None
            leaders = [names.index(a) for a in ('BTCUSDT', 'ETHUSDT') if a in names]
            cands = {
                'own momentum': lag[:, fi],
                'BTC lagged return': (lag[:, bt] if bt is not None else None),
                'equal-weight basket': lag.mean(axis=1),
                'leader-only (BTC+ETH)': lag[:, leaders].mean(axis=1),
                'seesaw sign (minus basket)': -lag.mean(axis=1),
            }
            for nm, p in cands.items():
                if p is None:
                    continue
                res = C.evaluate(p[va:T], y[va:T], keys, oneway, nm)
                if res is None or res['economic_ratio'] is None:
                    continue
                out.append(res['economic_ratio'])
                if not shift:
                    labels.append('{} {} {}m'.format(focal, nm, bar))
    return np.array(out), labels


def stability(panels, focal, model, bar, n_split=3):
    """Gross per-trade return in consecutive thirds of the held-out slice."""
    rets, ts, names = panels[bar]
    fi = names.index(focal)
    oneway = C.KALSHI_ONEWAY_BPS.get(focal, C.DEFAULT_ONEWAY)
    X, y = C.design(rets, fi, n_lags=1)
    T = len(y)
    va = int(T * (C.TRAIN_FRAC + C.VALID_FRAC))
    lag = X[:, :len(names)]
    leaders = [names.index(a) for a in ('BTCUSDT', 'ETHUSDT') if a in names]
    bt = names.index('BTCUSDT') if 'BTCUSDT' in names else None
    cands = {
        'own momentum': lag[:, fi],
        'BTC lagged return': (lag[:, bt] if bt is not None else None),
        'equal-weight basket': lag.mean(axis=1),
        'leader-only (BTC+ETH)': lag[:, leaders].mean(axis=1),
        'seesaw sign (minus basket)': -lag.mean(axis=1),
    }
    p, yy = cands[model][va:T], y[va:T]
    ok = np.isfinite(p) & np.isfinite(yy)
    p, yy = p[ok], yy[ok]
    signed = np.sign(p) * yy
    n = len(signed)
    edges = [int(round(n * i / n_split)) for i in range(n_split + 1)]
    rows = []
    for i in range(n_split):
        seg = signed[edges[i]:edges[i + 1]]
        rows.append((i + 1, len(seg), float(np.mean(seg)),
                     float(np.mean(seg)) - 2 * oneway,
                     float(np.mean(seg > 0))))
    return rows, n, 2 * oneway


def main():
    data = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        'data', 'binance')
    kdir = os.path.join(data, 'klines')
    assets = sorted(a for a in os.listdir(kdir)
                    if os.path.isdir(os.path.join(kdir, a))
                    and len(os.listdir(os.path.join(kdir, a))) >= 380)
    panels = {}
    for bar in BARS:
        rets, ts, names, err = C.build_panel(data, assets, bar)
        if err:
            print('  {}m: {}'.format(bar, err)); continue
        panels[bar] = (rets, ts, names)
        print('  {}m panel: {:,} bars, {} assets'.format(bar, rets.shape[0], len(names)))

    obs, labels = spec_ratios(panels)
    n_spec = len(obs)
    stats_obs = {
        'max': float(obs.max()),
        'mean': float(obs.mean()),
        'median': float(np.median(obs)),
        'n_over_1': int((obs >= 1.0).sum()),
        'frac_positive': float((obs > 0).mean()),
    }
    print('\nobserved across {} specifications:'.format(n_spec))
    for k in ('max', 'mean', 'median', 'n_over_1', 'frac_positive'):
        print('  {:<14} {}'.format(k, stats_obs[k]))
    print('  best spec:     {}  ({:.4f}x)'.format(labels[int(np.argmax(obs))], obs.max()))

    rng = np.random.default_rng(11)
    n_draws = 300
    keys = ['max', 'mean', 'median', 'n_over_1', 'frac_positive']
    null = {k: [] for k in keys}
    t0 = time.time()
    for i in range(n_draws):
        sh = int(rng.integers(80, 700))
        v, _ = spec_ratios(panels, shift=sh)
        null['max'].append(float(v.max()))
        null['mean'].append(float(v.mean()))
        null['median'].append(float(np.median(v)))
        null['n_over_1'].append(int((v >= 1.0).sum()))
        null['frac_positive'].append(float((v > 0).mean()))
        if (i + 1) % 50 == 0:
            print('  {}/{} draws  ({:.0f}s)'.format(i + 1, n_draws, time.time() - t0),
                  file=sys.stderr)

    print('\nnull distributions over {} circular shifts (same shift for every asset):'
          .format(n_draws))
    print('  {:<14} {:>9} {:>9} {:>9} {:>9} {:>9}   {:>9}'
          .format('statistic', 'p50', 'p90', 'p95', 'p99', 'max', 'observed'))
    pvals = {}
    for k in keys:
        a = np.array(null[k], dtype=float)
        p = float(np.mean(a >= stats_obs[k]))
        pvals[k] = p
        print('  {:<14} {:>9.4f} {:>9.4f} {:>9.4f} {:>9.4f} {:>9.4f}   {:>9.4f}   p={:.3f}'
              .format(k, np.percentile(a, 50), np.percentile(a, 90), np.percentile(a, 95),
                      np.percentile(a, 99), a.max(), float(stats_obs[k]), p))

    print('\nSIGN STABILITY -- {} {} {}m, held-out slice in consecutive thirds'
          .format('SOLUSDT', 'own momentum', 720))
    rows, n, rt = stability(panels, 'SOLUSDT', 'own momentum', 720)
    print('  round trip = {:.1f} bp'.format(rt))
    print('  {:<8} {:>6} {:>12} {:>12} {:>10}'
          .format('third', 'bars', 'gross bp', 'net bp', 'hit rate'))
    for i, nb, g, net, hit in rows:
        print('  {:<8} {:>6} {:>12.2f} {:>12.2f} {:>10.3f}'.format(i, nb, g, net, hit))
    signs = [1 if g > 0 else -1 for _, _, g, _, _ in rows]
    print('  sign pattern: {}   ->  {}'.format(
        ' '.join('+' if s > 0 else '-' for s in signs),
        'stable' if len(set(signs)) == 1 else 'NOT STABLE'))

    print('\nVERDICT')
    if pvals['max'] > 0.10 and pvals['mean'] > 0.10 and pvals['n_over_1'] > 0.10:
        print('  Neither the maximum nor the diffuse aggregate statistics are unusual under a')
        print('  null that preserves autocorrelation and cross-sectional structure. The lane')
        print('  closes on selection, not on a measured negative effect.')
    else:
        print('  At least one statistic sits outside its null. Report which, and do not close')
        print('  the lane on the max test alone.')


if __name__ == '__main__':
    main()
