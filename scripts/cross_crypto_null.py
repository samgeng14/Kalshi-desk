#!/usr/bin/env python3
"""What does the best of 144 specifications look like when there is nothing there?

The cross-crypto sweep reported a best economic ratio of 1.54x -- the first thing in this project
to clear a cost hurdle. It is also the maximum over 8 assets x 6 models x 3 bar lengths, chosen
on a test slice of 160 bars. A maximum over 144 correlated tries is not an estimate of anything
until you know what the maximum looks like under a null.

THE NULL. Circularly shift the future returns relative to the predictors by a random offset, the
SAME offset for every asset. That preserves each asset's own autocorrelation and the entire
cross-sectional correlation structure -- the two things that make naive permutation tests too
easy -- while destroying the alignment between predictor and target. Then run the identical
sweep and record its maximum.

If the observed 1.54x sits inside this distribution, it is selection, not signal.
"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cross_crypto as C

BARS = [60, 240, 720]
MODELS = ['own momentum', 'BTC lagged return', 'equal-weight basket',
          'leader-only (BTC+ETH)', 'seesaw sign (minus basket)']


def sweep_max(panels, shift=0):
    """Best economic ratio across every asset, model and bar length. shift=0 is the real data."""
    best = -9e9
    best_label = None
    n_spec = 0
    for bar, (rets, ts, names) in panels.items():
        r = np.roll(rets, shift, axis=0) if shift else rets
        for fi, focal in enumerate(names):
            oneway = C.KALSHI_ONEWAY_BPS.get(focal, C.DEFAULT_ONEWAY)
            X, y = C.design(r, fi, n_lags=1)
            if shift:
                # Shift the TARGET only, so predictors keep their own history and the alignment
                # between X and y is what breaks.
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
                n_spec += 1
                res = C.evaluate(p[va:T], y[va:T], keys, oneway, nm)
                if res and res['economic_ratio'] is not None \
                        and res['economic_ratio'] > best:
                    best, best_label = res['economic_ratio'], f'{focal} {nm} {bar}m'
    return best, best_label, n_spec


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
            print(f'  {bar}m: {err}')
            continue
        panels[bar] = (rets, ts, names)
        print(f'  {bar}m panel: {rets.shape[0]:,} bars, {len(names)} assets')

    obs, label, n_spec = sweep_max(panels)
    print(f'\nobserved best of {n_spec} specifications: {obs:.4f}x  ({label})')

    rng = np.random.default_rng(11)
    nulls = []
    n_draws = 200
    for i in range(n_draws):
        # A shift of at least a tenth of the sample, so the alignment is genuinely broken.
        sh = int(rng.integers(80, 700))
        b, _, _ = sweep_max(panels, shift=sh)
        nulls.append(b)
        if (i + 1) % 50 == 0:
            print(f'  {i+1}/{n_draws} null draws', file=sys.stderr)
    nulls = np.array(sorted(nulls))
    p = float(np.mean(nulls >= obs))
    print(f'\nnull best-of-{n_spec} distribution over {n_draws} shifts:')
    for q in (50, 75, 90, 95, 99):
        print(f'  p{q}: {np.percentile(nulls, q):.4f}x')
    print(f'  max: {nulls[-1]:.4f}x')
    print(f'\nobserved {obs:.4f}x   ->   p = {p:.3f}')
    if p > 0.10:
        print('\nVERDICT: the observed maximum is inside the null. This is selection across '
              '{}'.format(n_spec))
        print('         correlated specifications, not a signal.')
    else:
        print('\nVERDICT: the observed maximum is outside the null at the 10% level. Not proof,')
        print('         but it survives the multiple-testing check and warrants the next step.')


if __name__ == '__main__':
    main()
