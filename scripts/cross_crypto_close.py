#!/usr/bin/env python3
"""Closing tests for the cross-crypto lane, under the corrected cost convention.

WHAT CHANGED SINCE THE FIRST VERSION. Two things, both found by an adversarial review of the
report rather than by the code:

  1. `evaluate()` charged a full Kalshi round trip on EVERY bar while the position only flips on
     about half of them (median turnover 0.54). The hurdle was roughly 1.9x too harsh. It now
     charges `turnover * round_trip + round_trip / n`, and because turnover-adjusted costing
     rewards holding, every specification is additionally scored against the better of
     always-long and always-short.
  2. The null rolled the whole panel and then the target, so the fixed test index range pointed
     at a different calendar window on every draw. That widened the null and made the observed
     maximum easier to place inside it -- a bias toward the conclusion the report was drawing.
     The null now rolls the target only.

WHAT THIS TESTS. A maximum over 210 correlated specifications is not an estimate of anything
until you know what the maximum looks like under a null. But a max-only test is weak against a
DIFFUSE effect, so the same null is applied to the aggregate statistics too, and to the two
gates the verdict actually rests on: the count with a bootstrap lower bound above cost, and the
count that also beats the better constant position.

THE NULL. Circularly shift each focal asset's future returns relative to the predictors by a
random offset, the same offset for every asset. Predictors, test window and bootstrap clusters
are identical in every draw; the only thing that changes is which future each predictor is
matched to. Per-asset autocorrelation and the full cross-sectional correlation structure survive.
"""
import os, sys, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cross_crypto as C

BARS = [60, 240, 720]
SUB = os.environ.get('CC_SUB', 'klines')
DROP = [a for a in os.environ.get('CC_DROP', '').split(',') if a]
FOLDS = int(os.environ.get('CC_FOLDS', '0'))
N_DRAWS = int(os.environ.get('CC_DRAWS', '300'))
STATS = ['max', 'mean', 'median', 'n_over_1', 'frac_positive', 'n_net_lo_pos', 'n_all_gates']


def test_slice(T):
    """The out-of-sample index range, matching whichever evaluation the sweep used.

    With walk-forward pooling the no-model rules have nothing to fit, so the union of the
    out-of-sample blocks is simply everything after the first two -- one contiguous slice.
    """
    if FOLDS:
        edges = [int(round(T * k / (FOLDS + 2))) for k in range(FOLDS + 3)]
        return edges[2], edges[FOLDS + 2]
    return int(T * (C.TRAIN_FRAC + C.VALID_FRAC)), T


def sweep(panels, shift=0):
    """Every specification's numbers, in a fixed order. shift=0 is the real data."""
    ratio, netlo, exlo, labels = [], [], [], []
    for bar, (rets, ts, names) in panels.items():
        for fi, focal in enumerate(names):
            oneway = C.KALSHI_ONEWAY_BPS.get(focal, C.DEFAULT_ONEWAY)
            X, y = C.design(rets, fi, n_lags=1)
            if shift:
                y = np.roll(y, shift)
            T = len(y)
            va, hi = test_slice(T)
            keys = C.day_keys(ts[-T:][va:hi])
            lag = X[:, :len(names)]
            cands = C.rule_set(lag, names, fi)
            for nm, p in cands.items():
                if p is None:
                    continue
                r = C.evaluate(p[va:hi], y[va:hi], keys, oneway, nm)
                if r is None or r['economic_ratio'] is None:
                    continue
                ratio.append(r['economic_ratio'])
                netlo.append(r['net_lo'] if r['net_lo'] is not None else -9e9)
                exlo.append(r['excess_lo'] if r['excess_lo'] is not None else -9e9)
                if not shift:
                    labels.append('{} {} {}m'.format(focal, nm, bar))
    ratio, netlo, exlo = np.array(ratio), np.array(netlo), np.array(exlo)
    return {
        'max': float(ratio.max()),
        'mean': float(ratio.mean()),
        'median': float(np.median(ratio)),
        'n_over_1': int((ratio >= 1.0).sum()),
        'frac_positive': float((ratio > 0).mean()),
        'n_net_lo_pos': int((netlo > 0).sum()),
        'n_all_gates': int(((ratio >= 1.0) & (netlo > 0) & (exlo > 0)).sum()),
    }, ratio, labels


def stability(panels, focal, model, bar, n_split=3):
    """Gross per-bar return in consecutive thirds of the held-out slice, with the noise band."""
    rets, ts, names = panels[bar]
    fi = names.index(focal)
    oneway = C.KALSHI_ONEWAY_BPS.get(focal, C.DEFAULT_ONEWAY)
    X, y = C.design(rets, fi, n_lags=1)
    T = len(y)
    va, hi = test_slice(T)
    lag = X[:, :len(names)]
    cands = C.rule_set(lag, names, fi)
    p, yy = cands[model][va:hi], y[va:hi]
    ok = np.isfinite(p) & np.isfinite(yy)
    p, yy = p[ok], yy[ok]
    sgn = np.sign(p)
    signed = sgn * yy
    n = len(signed)
    full = C.evaluate(p, yy, C.day_keys(ts[-T:][va:hi])[:n], oneway, model)
    cost = full['trade_cost_bps_per_bar']
    edges = [int(round(n * i / n_split)) for i in range(n_split + 1)]
    rows = []
    for i in range(n_split):
        sl = slice(edges[i], edges[i + 1])
        seg, sg = signed[sl], sgn[sl]
        nb = len(seg)
        turn = float(np.mean(np.abs(np.diff(sg)) / 2)) if nb > 1 else 0.0
        hit = float(np.mean(seg > 0))
        se = (0.25 / nb) ** 0.5
        rows.append((i + 1, nb, float(np.mean(seg)), float(np.mean(seg)) - cost,
                     turn, hit, (hit - 0.5) / se))
    return rows, n, cost, full


def main():
    data = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        'data', 'binance')
    kdir = os.path.join(data, SUB)
    floor = 380 if SUB == 'klines' else 24
    assets = sorted(a for a in os.listdir(kdir)
                    if os.path.isdir(os.path.join(kdir, a))
                    and len(os.listdir(os.path.join(kdir, a))) >= floor
                    and a not in DROP)
    panels = {}
    for bar in BARS:
        rets, ts, names, err = C.build_panel(data, assets, bar, SUB)
        if err:
            print('  {}m: {}'.format(bar, err)); continue
        panels[bar] = (rets, ts, names)
        print('  {}m panel: {:,} bars, {} assets'.format(bar, rets.shape[0], len(names)))

    obs, ratios, labels = sweep(panels)
    n_spec = len(ratios)
    print('\nobserved across {} specifications ({} independent -- `seesaw sign (minus basket)`'
          .format(n_spec, n_spec - n_spec // 5))
    print('is exactly -1 x `equal-weight basket`, so its {} rows carry no new information):'
          .format(n_spec // 5))
    for k in STATS:
        print('  {:<16} {}'.format(k, obs[k]))
    print('  best spec:       {}  ({:.4f}x)'.format(labels[int(np.argmax(ratios))], ratios.max()))

    shortest = min(r.shape[0] for r, _, _ in panels.values())
    shift_lo, shift_hi = max(20, shortest // 20), max(40, shortest // 2)
    print('\ncircular-shift offsets drawn from [{:,}, {:,}) bars (5%-50% of the shortest panel)'
          .format(shift_lo, shift_hi))
    rng = np.random.default_rng(11)
    n_draws = N_DRAWS
    null = {k: [] for k in STATS}
    t0 = time.time()
    for i in range(n_draws):
        # Scale the offset to the SHORTEST panel so the alignment is genuinely broken there
        # too: a shift that is 15% of a 720m panel is 1% of a 60m one.
        sh = int(rng.integers(shift_lo, shift_hi))
        v, _, _ = sweep(panels, shift=sh)
        for k in STATS:
            null[k].append(float(v[k]))
        if (i + 1) % 50 == 0:
            print('  {}/{} draws  ({:.0f}s)'.format(i + 1, n_draws, time.time() - t0),
                  file=sys.stderr)

    print('\nnull distributions over {} circular shifts of the target only:'.format(n_draws))
    print('  {:<16} {:>9} {:>9} {:>9} {:>9} {:>9}   {:>10} {:>8} {:>8}'
          .format('statistic', 'p50', 'p90', 'p95', 'p99', 'max',
                  'observed', 'p_right', 'p_left'))
    pv = {}
    for k in STATS:
        a = np.array(null[k], dtype=float)
        pr = float(np.mean(a >= obs[k]))
        pl = float(np.mean(a <= obs[k]))
        pv[k] = (pr, pl)
        print('  {:<16} {:>9.4f} {:>9.4f} {:>9.4f} {:>9.4f} {:>9.4f}   {:>10.4f} {:>8.3f} {:>8.3f}'
              .format(k, np.percentile(a, 50), np.percentile(a, 90), np.percentile(a, 95),
                      np.percentile(a, 99), a.max(), float(obs[k]), pr, pl))
    se = (0.167 * 0.833 / n_draws) ** 0.5
    print('\n  binomial SE on a p near 0.17 with {} draws: {:.3f}. Offsets are drawn WITH'
          .format(n_draws, se))
    print('  replacement from {:,} candidates, and nearby offsets give similar sweeps, so the'
          .format(shift_hi - shift_lo))
    print('  effective draw count is below {} and the p99/max columns rest on a handful of'
          .format(n_draws))
    print('  order statistics. Read those two columns as indicative.')
    fp = float(np.mean(np.array(null['n_all_gates']) > 0))
    print('\n  SPECIFICITY. Under the null, the all-gates count exceeds zero in {:.1%} of draws'
          .format(fp))
    print('  ({:,} specification-draws). That is a false-positive rate, NOT power: it says the'
          .format(n_draws * n_spec))
    print('  gate does not fire on noise, and says nothing about whether it would fire on a real')
    print('  effect. Power is a different measurement and lives in cross_crypto_power.py, which')
    print('  injects an edge of known size and reports the smallest one this sample can see. Do')
    print('  not read an observed zero as evidence of absence without that number.')

    best_label = labels[int(np.argmax(ratios))]
    focal, model, bar = best_label.split()[0], ' '.join(best_label.split()[1:-1]), \
        int(best_label.split()[-1].rstrip('m'))
    print('\nSIGN STABILITY -- {} {} {}m, held-out slice in consecutive thirds'
          .format(focal, model, bar))
    rows, n, cost, full = stability(panels, focal, model, bar, n_split=6 if FOLDS else 3)
    print('  ({:,} pooled out-of-sample bars, {:,} day-clusters)'
          .format(full['n'], full['clusters']))
    print('  trade cost = {:.2f} bp/bar (turnover {:.3f} x {:.2f} bp round trip + amortised '
          'open/close)'.format(cost, full['turnover'], full['round_trip_bps']))
    print('  {:<7} {:>6} {:>11} {:>10} {:>9} {:>9} {:>8}'
          .format('third', 'bars', 'gross bp', 'net bp', 'turnover', 'hit rate', 'z vs .5'))
    for i, nb, g, net, turn, hit, z in rows:
        print('  {:<7} {:>6} {:>11.2f} {:>10.2f} {:>9.3f} {:>9.3f} {:>8.2f}'
              .format(i, nb, g, net, turn, hit, z))
    tot = sum(nb * g for _, nb, g, _, _, _, _ in rows)
    print('  share of total gross by third: ' + '  '.join(
        '{}: {:.0%}'.format(i, nb * g / tot) for i, nb, g, _, _, _, _ in rows))
    print('  a |z| below 1.96 is not distinguishable from a coin flip in either direction')

    print('\nGATES ON THE OBSERVED SWEEP')
    print('  economic_ratio > 1                     {:>3} of {}'.format(obs['n_over_1'], n_spec))
    print('  and net_lo > 0                         {:>3}'.format(obs['n_net_lo_pos']))
    print('  and excess_lo > 0 (beats a constant)   {:>3}'.format(obs['n_all_gates']))
    print('\nVERDICT')
    if obs['n_all_gates'] == 0:
        print('  No specification passes all three gates, so there is nothing for the selection')
        print('  null to adjudicate. The lane closes on the gates, not on multiple testing.')
    elif pv['n_all_gates'][0] > 0.10:
        print('  {} specification(s) pass all three gates, but that count is inside its null'
              .format(obs['n_all_gates']))
        print('  (p = {:.3f}). Consistent with selection across correlated specifications.'
              .format(pv['n_all_gates'][0]))
    else:
        print('  {} specification(s) pass all three gates and the count is outside its null'
              .format(obs['n_all_gates']))
        print('  (p = {:.3f}). Not proof. It warrants the Kalshi mapping step and nothing more.'
              .format(pv['n_all_gates'][0]))


if __name__ == '__main__':
    main()
