#!/usr/bin/env python3
"""Cross-crypto return predictability: replicate the published effect, economically, from line 1.

Two papers, both verified verbatim before any code was written (research/literature/
cross_crypto_replication.md):

  Jia, Wu, Yan & Liu, J Empirical Finance 74 (2023) — the seesaw effect. Intraday. "the large
  coins negatively predict the other coins but the small coins rarely predict the large coins",
  exploited via LASSO, "highly significant profits ... even in the presence of realistic
  transaction costs".

  Guo, Sang, Tu & Wang, JEDC 163 (2024) — "lagged returns of other cryptocurrencies serve as
  significant predictors of focal cryptocurrencies", adaptive LASSO and PCA, long-short portfolio
  with "sizable return out-of-sample after accounting for transaction costs".

WHAT THE LAST LANE TAUGHT, APPLIED HERE. The quarter-hour effect replicated as a rank
correlation of -0.6 and was worth 0.05x of what a Kalshi round trip needs. A monotone
relationship and a tradable one are different objects. So this file reports the economic
magnitude in the same breath as the statistic, every time, and the headline number is
`economic_ratio = observed effect / required effect` rather than an IC or a t.

THE HURDLE IS COMPUTED BEFORE THE MODEL IS TUNED, and the signal is frozen before it is compared
to the hurdle. Optimising against a 24 bp threshold is how a threshold becomes a target.

DIRECTION MATTERS. The seesaw result predicts SMALL coins from LARGE ones, not the reverse. On
Kalshi the small coins cost 18-21 bp one way to cross against BTC's 12.3, so the signal is
strongest exactly where execution is worst. Each focal asset is therefore screened against its
OWN Kalshi hurdle, not against BTC's.

  python3 scripts/cross_crypto.py --bar 60 --report
"""
import argparse, csv, json, math, os, statistics, sys, zipfile
from collections import defaultdict

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# One-way cost to cross on Kalshi, measured in reports/execution_simulator.csv. Assets with no
# Kalshi listing use the median of the ones that have one.
KALSHI_ONEWAY_BPS = {
    'BTCUSDT': 12.32, 'ETHUSDT': 12.50, 'SOLUSDT': 14.12, 'XRPUSDT': 14.54,
    'LTCUSDT': 15.58, 'DOGEUSDT': 16.79, 'ADAUSDT': 17.69, 'BNBUSDT': 16.93,
    'NEARUSDT': 19.74, 'AAVEUSDT': 19.28, 'SUIUSDT': 19.65, 'LINKUSDT': 21.30,
    'BCHUSDT': 20.15,
}
DEFAULT_ONEWAY = 17.0
TRAIN_FRAC, VALID_FRAC = 0.60, 0.20


def load_closes(data_dir, asset, sub='klines'):
    """Closes keyed by bar open time in ms, from whatever kline zips are in `sub`.

    Works for any interval -- daily 1m files or monthly 1h files -- because the only thing read
    is (open_time, close). The header line is parsed rather than skipped by seeking: a zip member
    stream is not seekable, so the previous version silently dropped the first bar of every file
    whose first line was NOT a header.
    """
    out = {}
    d = os.path.join(data_dir, sub, asset)
    if not os.path.isdir(d):
        return out
    for fn in sorted(os.listdir(d)):
        if not fn.endswith('.zip'):
            continue
        try:
            with zipfile.ZipFile(os.path.join(d, fn)) as z:
                with z.open(z.namelist()[0]) as raw:
                    for line in raw:
                        f = line.split(b',')
                        try:
                            out[int(f[0])] = float(f[4])
                        except (ValueError, IndexError):
                            continue      # the header line lands here and is skipped
        except (zipfile.BadZipFile, OSError):
            continue
    return out


def build_panel(data_dir, assets, bar_min, sub='klines'):
    """A dense panel of log returns over `bar_min` bars, on a common clock.

    Bars are aligned to the wall clock, and a bar is dropped for EVERY asset if it is missing
    for any of them. An unbalanced panel would let the cross-sectional predictors quietly change
    membership between periods, which is a different regression each time.
    """
    closes = {a: load_closes(data_dir, a, sub) for a in assets}
    min_obs = 10000 if sub == 'klines' else 2000
    closes = {a: c for a, c in closes.items() if len(c) > min_obs}
    if len(closes) < 3:
        return None, None, list(closes), 'not enough assets with data'
    step = bar_min * 60_000
    grids = []
    for a, c in closes.items():
        ts = sorted(c)
        grids.append({t for t in ts if t % step == 0})
    common = sorted(set.intersection(*grids))
    if len(common) < 200:
        return None, None, list(closes), f'only {len(common)} aligned bars'
    names = sorted(closes)
    px = np.array([[closes[a][t] for a in names] for t in common], dtype=float)
    with np.errstate(divide='ignore', invalid='ignore'):
        rets = np.diff(np.log(px), axis=0) * 1e4      # bps
    ts = common[1:]
    ok = np.isfinite(rets).all(axis=1)
    return rets[ok], [t for t, k in zip(ts, ok) if k], names, None


def design(rets, focal_idx, n_lags=1):
    """X = lagged returns of every asset (the paper's predictor set), y = focal next return."""
    T, N = rets.shape
    rows, ys = [], []
    for t in range(n_lags, T):
        x = []
        for L in range(1, n_lags + 1):
            x.extend(rets[t - L])
        rows.append(x)
        ys.append(rets[t, focal_idx])
    return np.array(rows), np.array(ys)


def block_ci(vals, keys, n=0, seed=7):
    """Cluster bootstrap on calendar-day blocks.

    Every perp moves together, so resampling individual bars would treat 160 correlated
    observations as 160 independent ones and produce an interval far too tight. Whole days are
    resampled instead, shared across the panel.

    Vectorised: a resample's mean is sum(day_sums[idx]) / sum(day_counts[idx]), which is exactly
    what concatenating the chosen days and averaging gives. The pure-Python version this replaces
    was the bottleneck in the null sweep (1200 resamples x 210 specifications x 300 draws). The
    RNG stream differs, so interval endpoints move in the last decimal against older outputs; the
    point estimates are untouched.
    """
    v = np.asarray(vals, dtype=float)
    k = np.asarray(keys)
    ok = np.isfinite(v)
    v, k = v[ok], k[ok]
    if len(v) < 20:
        return None, None, None, 0
    uniq, inv = np.unique(k, return_inverse=True)
    m = float(v.mean())
    g = len(uniq)
    if g < 5:
        return m, None, None, g
    if not n:
        # Monte Carlo error of a percentile depends on the resample count, not the cluster count,
        # so a short sample needs many resamples to keep a gate from being decided by bootstrap
        # noise, while a long one does not. At 80 clusters, 1,200 resamples once flipped a gate
        # that 8,000 resamples fails.
        n = 8000 if g < 400 else 2500
    sums = np.bincount(inv, weights=v, minlength=g)
    cnts = np.bincount(inv, minlength=g).astype(float)
    rng = np.random.default_rng(seed)
    idx = rng.integers(0, g, size=(n, g))
    means = sums[idx].sum(axis=1) / cnts[idx].sum(axis=1)
    means.sort()
    return m, float(means[int(0.05 * n)]), float(means[int(0.95 * n)]), g


def day_keys(ts_ms):
    """Calendar-day cluster labels as an integer array (UTC day number).

    Integers rather than date strings: these arrays run to tens of thousands of elements and are
    filtered and grouped on every call, and the string version turned that into Python-level list
    comprehensions that dominated the null sweep's runtime.
    """
    return np.asarray(ts_ms, dtype=np.int64) // 86_400_000


def evaluate(pred, y, keys, oneway_bps, label):
    """Statistical AND economic, always together. The economic number is the headline.

    COST CONVENTION. The position is sign(pred_t), held through bar t. You only pay the spread
    when the position CHANGES, so the cost charged per bar is

        turnover * round_trip  +  round_trip / n

    where turnover = mean(|diff(sign)|/2) is the fraction of bars on which the sign flips (a flip
    is two units traded, which is one round trip), and the second term amortises the unavoidable
    opening and closing trade over the sample. An earlier version of this function charged a full
    round trip on EVERY bar, which over-charged these rules by roughly 1.9x -- median turnover is
    0.54 -- and produced a hurdle nothing could clear. `economic_ratio_everybar` preserves that
    stricter number as a bound; `economic_ratio` is the honest one.

    THE DRIFT CONTROL IS NOT OPTIONAL HERE. Turnover-adjusted costing rewards holding, and the
    cheapest possible strategy is a constant position, which pays one round trip in total and
    collects the asset's drift. So every specification is also scored against the better of
    always-long and always-short on the same slice: `excess_vs_constant_bps`, with its own
    block-bootstrap lower bound. A signal that cannot beat a constant position has found the
    sample's drift, not a signal.
    """
    ok = np.isfinite(pred) & np.isfinite(y)
    pred, y, keys = pred[ok], y[ok], np.asarray(keys)[ok]
    if len(y) < 100:
        return None
    n = len(y)
    sd = float(np.std(y))
    ic = float(np.corrcoef(pred, y)[0, 1]) if np.std(pred) > 0 else 0.0
    rp, ry = np.argsort(np.argsort(pred)), np.argsort(np.argsort(y))
    ric = float(np.corrcoef(rp, ry)[0, 1])
    order = np.argsort(pred)
    k = len(y) // 10
    bot, top = y[order[:k]], y[order[-k:]]
    spread = float(np.mean(top) - np.mean(bot))
    round_trip = 2 * oneway_bps

    sgn = np.sign(pred)
    signed = sgn * y
    m, lo, hi, nclu = block_ci(signed, keys)
    turnover = float(np.mean(np.abs(np.diff(sgn)) / 2)) if n > 1 else 1.0
    trade_cost = turnover * round_trip + round_trip / n

    # The best constant position on the same slice, and the signal's excess over it.
    drift = float(np.mean(y))
    c = 1.0 if drift >= 0 else -1.0
    const_gross = c * drift
    const_cost = round_trip / n
    excess_m, excess_lo, excess_hi, _ = block_ci(signed - c * y, keys)

    return {
        'label': label, 'n': n, 'clusters': nclu,
        'sigma_bps': sd, 'ic': ic, 'rank_ic': ric,
        'top_minus_bottom_bps': spread,
        'gross_bps_per_bar': m, 'gross_lo': lo, 'gross_hi': hi,
        'turnover': turnover,
        'round_trip_bps': round_trip,
        'trade_cost_bps_per_bar': trade_cost,
        'net_bps_per_bar': (m - trade_cost) if m is not None else None,
        'net_lo': (lo - trade_cost) if lo is not None else None,
        'required_effect_bps': trade_cost,
        # SIGNED, not absolute. A negative gross means the signal pointed the wrong way out of
        # sample, and wrapping it in abs() would report that failure as a fraction of the way to
        # profitable. The sign is the finding.
        'economic_ratio': (m / trade_cost) if (m is not None and trade_cost > 1e-12) else None,
        # The stricter bound: a full round trip on every bar, i.e. a strategy that re-enters each
        # bar. Nobody would trade that, but it brackets the answer from above.
        'net_everybar_bps': (m - round_trip) if m is not None else None,
        'economic_ratio_everybar': (m / round_trip) if (m is not None and round_trip) else None,
        # Drift control.
        'const_side': ('long' if c > 0 else 'short'),
        'const_gross_bps_per_bar': const_gross,
        'const_net_bps_per_bar': const_gross - const_cost,
        'excess_vs_constant_bps': excess_m,
        'excess_lo': excess_lo, 'excess_hi': excess_hi,
    }


def baselines(rets, names, focal, ts, oneway):
    """Simple rules the published model has to beat, computed on the same held-out slice."""
    fi = names.index(focal)
    X, y = design(rets, fi, n_lags=1)
    T = len(y)
    tr, va = int(T * TRAIN_FRAC), int(T * (TRAIN_FRAC + VALID_FRAC))
    te = slice(va, T)
    keys = day_keys(ts[1:][va:T]) if len(ts) > T else day_keys(ts[-T:][va:T])
    out = []
    lag = X[:, :len(names)]
    bt = names.index('BTCUSDT') if 'BTCUSDT' in names else None
    cands = {
        'own momentum': lag[:, fi],
        'BTC lagged return': (lag[:, bt] if bt is not None else None),
        'equal-weight basket': lag.mean(axis=1),
        'leader-only (BTC+ETH)': (lag[:, [names.index(a) for a in ('BTCUSDT', 'ETHUSDT')
                                          if a in names]].mean(axis=1)),
        'seesaw sign (minus basket)': -lag.mean(axis=1),
    }
    for nm, p in cands.items():
        if p is None:
            continue
        r = evaluate(p[te], y[te], keys, oneway, nm)
        if r:
            out.append(r)
    return out, (X, y, tr, va, T, keys)


def fit_lasso(X, y, tr, va, T):
    """LASSO with the penalty chosen on the VALIDATION slice, never on the test slice."""
    from sklearn.linear_model import Lasso
    from sklearn.preprocessing import StandardScaler
    sc = StandardScaler().fit(X[:tr])
    Xtr, Xva, Xte = sc.transform(X[:tr]), sc.transform(X[tr:va]), sc.transform(X[va:T])
    best, best_a, best_m = -9e9, None, None
    for a in (0.01, 0.03, 0.1, 0.3, 1.0, 3.0, 10.0, 30.0):
        m = Lasso(alpha=a, max_iter=20000).fit(Xtr, y[:tr])
        p = m.predict(Xva)
        if np.std(p) < 1e-12:
            continue
        # Selected on ECONOMIC value on validation, not on R^2: the whole lesson of the last lane.
        score = float(np.mean(np.sign(p) * y[tr:va]))
        if score > best:
            best, best_a, best_m = score, a, m
    if best_m is None:
        return None, None, None
    return best_m.predict(Xte), best_a, int(np.sum(np.abs(best_m.coef_) > 1e-10))


def rule_set(lag, names, fi):
    """The no-model rules. No parameters, so nothing to fit -- only the choice of which to try,
    which is what the selection null is for."""
    bt = names.index('BTCUSDT') if 'BTCUSDT' in names else None
    leaders = [names.index(a) for a in ('BTCUSDT', 'ETHUSDT') if a in names]
    return {
        'own momentum': lag[:, fi],
        'BTC lagged return': (lag[:, bt] if bt is not None else None),
        'equal-weight basket': lag.mean(axis=1),
        'leader-only (BTC+ETH)': (lag[:, leaders].mean(axis=1) if leaders else None),
        'seesaw sign (minus basket)': -lag.mean(axis=1),
    }


def walk_forward(rets, names, focal, ts, oneway, folds=8):
    """Expanding-window walk-forward, pooling every out-of-sample block into one test series.

    WHY NOT ONE CHRONOLOGICAL SPLIT. A single 60/20/20 split puts the whole test slice in one
    regime. The first version of this study tested on 2026-06-23..09-10, in which all fourteen
    assets rose -- so every long-biased rule looked good and the drift control had nothing to bite
    on. Walk-forward spreads the out-of-sample evaluation across the entire history instead, so
    the pooled test slice contains drawdowns as well as rallies and `excess_vs_constant_bps` is
    asking a real question.

    Fold i trains on blocks [0, i-1], validates on block i, and tests on block i+1. The no-model
    rules have nothing to fit, but they are evaluated on exactly the same pooled blocks as the
    LASSO so the comparison is like for like.
    """
    fi = names.index(focal)
    X, y = design(rets, fi, n_lags=1)
    T = len(y)
    tstamps = ts[-T:]
    edges = [int(round(T * k / (folds + 2))) for k in range(folds + 3)]
    lag = X[:, :len(names)]
    pooled = defaultdict(lambda: ([], [], []))
    for i in range(folds):
        tr_end, va_end, te_end = edges[i + 1], edges[i + 2], edges[i + 3]
        te = slice(va_end, te_end)
        if te_end - va_end < 5:
            continue
        for nm, p in rule_set(lag, names, fi).items():
            if p is None:
                continue
            a, b, c = pooled[nm]
            a.extend(p[te]); b.extend(y[te]); c.extend(day_keys(tstamps[te]))
        pr, alpha, nz = fit_lasso(X, y, tr_end, va_end, te_end)
        if pr is not None:
            a, b, c = pooled['LASSO (walk-forward)']
            a.extend(pr); b.extend(y[te]); c.extend(day_keys(tstamps[te]))
    out = []
    for nm, (p, yy, kk) in pooled.items():
        r = evaluate(np.asarray(p), np.asarray(yy), kk, oneway, nm)
        if r:
            out.append(r)
    n_te = edges[folds + 2] - edges[2]
    return out, (X, y, edges, n_te)


def report(results, panel_info, out_md, out_csv, bar_min):
    names, n_bars, span_days = panel_info
    L = []
    W = L.append
    W(f'# Cross-crypto return predictability — replication at {bar_min}-minute bars\n')
    W('Sources, verified verbatim before any code: Jia, Wu, Yan & Liu, *J Empirical Finance* 74 '
      '(2023); Guo, Sang, Tu & Wang, *JEDC* 163 (2024). Specification in '
      '`research/literature/cross_crypto_replication.md`.\n')
    W(f'Panel: {len(names)} assets, {n_bars:,} aligned bars, {span_days} days. '
      f'Chronological split {TRAIN_FRAC:.0%} train / {VALID_FRAC:.0%} validate / '
      f'{1-TRAIN_FRAC-VALID_FRAC:.0%} test. The penalty is chosen on validation; the test slice '
      f'is touched once.\n')

    W('## The headline is the economic ratio\n')
    W('`economic_ratio = gross bps per bar / trade cost per bar`, signed. The trade cost is '
      '`turnover x round_trip + round_trip / n`: you pay the spread when the position changes '
      'sign, plus the unavoidable opening and closing trade amortised over the slice. A ratio of '
      '1.0 means the signal is exactly worth what it costs to trade; below 1 it loses. '
      '`economic_ratio_everybar` charges a full round trip on every bar instead -- a strategy '
      'nobody would trade, kept as an upper bound on the cost.\n')
    W('**Two gates beside it, both stricter.** `net_lo` is the block-bootstrap 90% lower bound on '
      'the gross return minus the trade cost: a specification whose `net_lo` is negative has a '
      'cost-adjusted return that is not distinguishable from zero. `excess_vs_constant_bps` is '
      'the return over the better of always-long and always-short on the same slice -- because '
      'turnover-adjusted costing rewards holding, and the cheapest strategy available is a '
      'constant position that pays one round trip in total and collects the drift.\n')
    W('This is a discovery screen. Clearing it makes a signal worth testing, not worth trading: '
      'the ratio cannot see tail concentration, instability through time, funding paid while the '
      'position is held, or whether a Kalshi contract tracks the underlying return at all.\n')

    rows = []
    for focal, res in results.items():
        if not res:
            continue
        W(f'\n### {focal} — Kalshi round trip '
          f'{2*KALSHI_ONEWAY_BPS.get(focal, DEFAULT_ONEWAY):.1f} bp\n')
        W('| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | '
          'net bps/bar | net_lo | excess vs constant | economic ratio |')
        W('|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|')
        for r in sorted(res, key=lambda x: -(x['economic_ratio'] or 0)):
            band = ('n/a' if r['gross_lo'] is None
                    else f"[{r['gross_lo']:+.1f}, {r['gross_hi']:+.1f}]")
            ex = r['excess_vs_constant_bps']
            W('| {} | {:,} | {:+.4f} | {:+.2f} | {} | {:.2f} | {:.2f} | {:+.2f} | {:+.2f} | '
              '{} | **{:.4f}x** |'.format(
                  r['label'], r['n'], r['ic'],
                  r['gross_bps_per_bar'] or 0.0, band, r['turnover'],
                  r['trade_cost_bps_per_bar'], r['net_bps_per_bar'] or 0.0,
                  r['net_lo'] if r['net_lo'] is not None else float('nan'),
                  ('n/a' if ex is None else '{:+.2f} [{:+.1f}, {:+.1f}]'.format(
                      ex, r['excess_lo'], r['excess_hi'])),
                  r['economic_ratio'] or 0.0))
            rows.append(dict(r, focal=focal, bar_min=bar_min))
        W('')

    allr = [r for res in results.values() for r in res if r.get('economic_ratio')]
    W('\n## Verdict\n')
    if not allr:
        W('Nothing evaluated.\n')
    else:
        best = max(allr, key=lambda r: r['economic_ratio'])
        # Three gates, all of which have to hold: the point estimate clears the cost, the
        # cost-adjusted return is distinguishable from zero, and the signal beats the better of
        # always-long and always-short. The third is what stops drift from being sold as alpha.
        clears = [r for r in allr if r['economic_ratio'] and r['economic_ratio'] > 1
                  and r.get('net_lo') is not None and r['net_lo'] > 0
                  and r.get('excess_lo') is not None and r['excess_lo'] > 0]
        W(f"Best across every focal asset and model: **{best['label']}** on "
          f"{best.get('focal','?')} at **{best['economic_ratio']:.3f}x** of the required effect "
          f"({best['gross_bps_per_bar']:+.2f} bp gross against a "
          f"{best['round_trip_bps']:.1f} bp round trip).\n")
        pt = [r for r in allr if r['economic_ratio'] and r['economic_ratio'] > 1]
        nl = [r for r in pt if r.get('net_lo') is not None and r['net_lo'] > 0]
        W('| gate | specifications passing |\n|---|---:|')
        W('| economic_ratio > 1 (point estimate) | {} of {} |'.format(len(pt), len(allr)))
        W('| and net_lo > 0 (cost-adjusted return distinguishable from zero) | {} |'.format(len(nl)))
        W('| and excess_lo > 0 (beats the better constant position) | {} |\n'.format(len(clears)))
        if clears:
            W(f'**{len(clears)} specification(s) pass all three gates.** That warrants the '
              f'selection null and then the Kalshi mapping step, and nothing more until those '
              f'are done.\n')
        elif nl:
            W(f'{len(nl)} specification(s) clear the cost with a bootstrap lower bound above '
              f'zero, but none of them beats the better of always-long and always-short. That '
              f'is drift, not signal.\n')
        elif pt:
            W(f'{len(pt)} point estimate(s) clear the hurdle; none has a bootstrap lower bound '
              f'above zero. That is a reason to record more data, not to trade.\n')
        else:
            W(f"**Nothing clears the hurdle.** The best specification is worth "
              f"{best['economic_ratio']:.1%} of what it costs to trade on Kalshi.\n")
            lasso = [r for r in allr if 'lasso' in r['label'].lower()]
            simple = [r for r in allr if 'lasso' not in r['label'].lower()]
            if lasso and simple:
                bl = max(lasso, key=lambda r: r['economic_ratio'])
                bs = max(simple, key=lambda r: r['economic_ratio'])
                if bl['economic_ratio'] <= bs['economic_ratio']:
                    W(f"The LASSO does not beat the simple baselines "
                      f"({bl['economic_ratio']:.3f}x against {bs['economic_ratio']:.3f}x for "
                      f"`{bs['label']}`), so the failure is not a modelling failure. There is "
                      f"not enough in the cross-section at this bar length to pay for a Kalshi "
                      f"round trip.\n")

    W('\n## What this does not settle\n')
    W('- The papers are intraday on Binance with many more coins and, in the seesaw case, a '
      'long-short portfolio across the cross-section. This is a directional per-asset test on '
      f'{len(names)} coins, which is the version that maps onto a Kalshi position.')
    W('- The seesaw direction is large-predicts-small. The small coins are exactly the ones that '
      'cost 18-21 bp one way on Kalshi, so a replication that works on Binance can still fail on '
      'transfer, and that would be `FAILED_TRANSFER` rather than a contradiction of the paper.')
    W('- Costs here are the Kalshi round trip. The papers clear their own venues\' costs, which '
      'are far lower. Both statements can be true at once.\n')

    os.makedirs(os.path.dirname(out_md), exist_ok=True)
    with open(out_md, 'w') as f:
        f.write('\n'.join(L) + '\n')
    if rows:
        cols = ['focal', 'bar_min', 'label', 'n', 'clusters', 'sigma_bps', 'ic', 'rank_ic',
                'top_minus_bottom_bps', 'gross_bps_per_bar', 'gross_lo', 'gross_hi',
                'turnover', 'round_trip_bps', 'trade_cost_bps_per_bar',
                'net_bps_per_bar', 'net_lo', 'required_effect_bps', 'economic_ratio',
                'net_everybar_bps', 'economic_ratio_everybar',
                'const_side', 'const_gross_bps_per_bar', 'const_net_bps_per_bar',
                'excess_vs_constant_bps', 'excess_lo', 'excess_hi']
        with open(out_csv, 'w', newline='') as f:
            w = csv.DictWriter(f, fieldnames=cols, extrasaction='ignore')
            w.writeheader()
            for r in rows:
                w.writerow(r)
    return allr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=os.path.join(ROOT, 'data', 'binance'))
    ap.add_argument('--bar', type=int, default=60, help='bar length in minutes')
    ap.add_argument('--focal', nargs='*', default=None)
    ap.add_argument('--sub', default='klines',
                    help="kline subdirectory: 'klines' (daily 1m) or 'klines_1h' (monthly 1h)")
    ap.add_argument('--folds', type=int, default=0,
                    help='0 = single chronological split; >0 = expanding-window walk-forward')
    ap.add_argument('--drop', nargs='*', default=[], help='assets to exclude')
    ap.add_argument('--tag', default='', help='suffix for the output filenames')
    ap.add_argument('--md', default='')
    ap.add_argument('--csv', default='')
    args = ap.parse_args()
    tag = args.tag
    md = args.md or os.path.join(ROOT, 'reports', f'cross_crypto_{args.bar}m{tag}.md')
    out_csv = args.csv or os.path.join(ROOT, 'reports', f'cross_crypto_{args.bar}m{tag}.csv')

    kdir = os.path.join(args.data, args.sub)
    floor = 380 if args.sub == 'klines' else 24
    assets = sorted(a for a in os.listdir(kdir)
                    if os.path.isdir(os.path.join(kdir, a))
                    and len(os.listdir(os.path.join(kdir, a))) >= floor
                    and a not in args.drop)
    rets, ts, names, err = build_panel(args.data, assets, args.bar, args.sub)
    if err:
        sys.exit(err)
    span_days = len({t // 86_400_000 for t in ts})
    print(f'panel: {len(names)} assets {names}')
    print(f'       {rets.shape[0]:,} bars of {args.bar}m over {span_days} days')

    focals = args.focal or names
    results = {}
    for focal in focals:
        if focal not in names:
            continue
        oneway = KALSHI_ONEWAY_BPS.get(focal, DEFAULT_ONEWAY)
        if args.folds:
            res, _ = walk_forward(rets, names, focal, ts, oneway, args.folds)
            results[focal] = res
            best = max((b for b in res if b.get('economic_ratio')),
                       key=lambda b: b['economic_ratio'], default=None)
            if best:
                print('  {:10s} best {:30s} gross {:+7.2f} bp  net {:+7.2f} bp  ratio {:.3f}x  '
                      'excess {:+7.2f} [{:+.1f}, {:+.1f}]'.format(
                          focal, best['label'][:28], best['gross_bps_per_bar'],
                          best['net_bps_per_bar'], best['economic_ratio'],
                          best['excess_vs_constant_bps'], best['excess_lo'], best['excess_hi']))
            continue
        base, (X, y, tr, va, T, keys) = baselines(rets, names, focal, ts, oneway)
        pred, alpha, nz = fit_lasso(X, y, tr, va, T)
        if pred is not None:
            r = evaluate(pred, y[va:T], keys, oneway, f'LASSO (alpha={alpha}, {nz} nonzero)')
            if r:
                base.append(r)
        results[focal] = base
        best = max((b for b in base if b.get('economic_ratio')),
                   key=lambda b: b['economic_ratio'], default=None)
        if best:
            print(f"  {focal:10s} best {best['label'][:28]:30s} "
                  f"gross {best['gross_bps_per_bar']:+7.2f} bp  "
                  f"net {best['net_bps_per_bar']:+7.2f} bp  "
                  f"ratio {best['economic_ratio']:.3f}x")

    allr = report(results, (names, rets.shape[0], span_days), md, out_csv, args.bar)
    if allr:
        b = max(allr, key=lambda r: r['economic_ratio'])
        print(f"\nbest overall: {b['economic_ratio']:.4f}x of the required effect")
    print(f'wrote {md}')


if __name__ == '__main__':
    main()
