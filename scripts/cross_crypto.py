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


def load_closes(data_dir, asset):
    """Minute closes keyed by bar open time in ms."""
    out = {}
    d = os.path.join(data_dir, 'klines', asset)
    if not os.path.isdir(d):
        return out
    for fn in sorted(os.listdir(d)):
        if not fn.endswith('.zip'):
            continue
        try:
            with zipfile.ZipFile(os.path.join(d, fn)) as z:
                with z.open(z.namelist()[0]) as raw:
                    first = raw.readline()
                    if b'open_time' not in first:
                        try:
                            raw.seek(0)
                        except (OSError, AttributeError):
                            pass
                    for line in raw:
                        f = line.split(b',')
                        try:
                            out[int(f[0])] = float(f[4])
                        except (ValueError, IndexError):
                            continue
        except (zipfile.BadZipFile, OSError):
            continue
    return out


def build_panel(data_dir, assets, bar_min):
    """A dense panel of log returns over `bar_min` bars, on a common clock.

    Bars are aligned to the wall clock, and a bar is dropped for EVERY asset if it is missing
    for any of them. An unbalanced panel would let the cross-sectional predictors quietly change
    membership between periods, which is a different regression each time.
    """
    closes = {a: load_closes(data_dir, a) for a in assets}
    closes = {a: c for a, c in closes.items() if len(c) > 10000}
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


def block_ci(vals, keys, n=1200, seed=7):
    import random
    pairs = [(k, v) for k, v in zip(keys, vals) if v is not None and np.isfinite(v)]
    if len(pairs) < 20:
        return None, None, None, 0
    groups = defaultdict(list)
    for k, v in pairs:
        groups[k].append(v)
    gk = list(groups)
    m = float(np.mean([v for _, v in pairs]))
    if len(gk) < 5:
        return m, None, None, len(gk)
    rng = random.Random(seed)
    means = []
    for _ in range(n):
        s = []
        for _ in range(len(gk)):
            s.extend(groups[gk[rng.randrange(len(gk))]])
        means.append(float(np.mean(s)))
    means.sort()
    return m, means[int(0.05 * n)], means[int(0.95 * n)], len(gk)


def day_keys(ts_ms):
    import datetime as dt
    return [dt.datetime.fromtimestamp(t / 1000, dt.timezone.utc).strftime('%Y-%m-%d')
            for t in ts_ms]


def evaluate(pred, y, keys, oneway_bps, label):
    """Statistical AND economic, always together. The economic number is the headline."""
    ok = np.isfinite(pred) & np.isfinite(y)
    pred, y, keys = pred[ok], y[ok], [k for k, m in zip(keys, ok) if m]
    if len(y) < 100:
        return None
    sd = float(np.std(y))
    ic = float(np.corrcoef(pred, y)[0, 1]) if np.std(pred) > 0 else 0.0
    rp, ry = np.argsort(np.argsort(pred)), np.argsort(np.argsort(y))
    ric = float(np.corrcoef(rp, ry)[0, 1])
    # Decile spread: the economic version of the same thing.
    order = np.argsort(pred)
    k = len(y) // 10
    bot, top = y[order[:k]], y[order[-k:]]
    spread = float(np.mean(top) - np.mean(bot))
    # A long-short trade: long the top decile, short the bottom, one round trip each side.
    round_trip = 2 * oneway_bps
    # Signed per-trade return from taking the predicted direction on every bar.
    sgn = np.sign(pred)
    signed = sgn * y
    m, lo, hi, nclu = block_ci(signed, keys)
    turnover = float(np.mean(np.abs(np.diff(sgn)) / 2)) if len(sgn) > 1 else 1.0
    required = round_trip
    return {
        'label': label, 'n': len(y), 'clusters': nclu,
        'sigma_bps': sd, 'ic': ic, 'rank_ic': ric,
        'top_minus_bottom_bps': spread,
        'gross_bps_per_trade': m, 'gross_lo': lo, 'gross_hi': hi,
        'turnover': turnover,
        'round_trip_bps': round_trip,
        'net_bps_per_trade': (m - round_trip) if m is not None else None,
        'net_lo': (lo - round_trip) if lo is not None else None,
        'required_effect_bps': required,
        # SIGNED, not absolute. A negative gross means the signal pointed the wrong way out of
        # sample, and wrapping it in abs() would report that failure as 10% of the way to
        # profitable. The sign is the finding.
        'economic_ratio': (m / required) if (m is not None and required) else None,
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
    W('`economic_ratio = |observed effect| / required effect`, where the required effect is the '
      "focal asset's OWN Kalshi round trip. A ratio of 1.0 means the signal is exactly worth "
      'what it costs to trade; below 1 it loses. Statistical columns are reported beside it, '
      'never instead of it.\n')
    W('This is a discovery screen. Clearing it makes a signal worth testing, not worth trading: '
      'the ratio cannot see tail concentration, instability through time, turnover cost, or '
      'whether a Kalshi contract tracks the underlying return at all.\n')

    rows = []
    for focal, res in results.items():
        if not res:
            continue
        W(f'\n### {focal} — Kalshi round trip '
          f'{2*KALSHI_ONEWAY_BPS.get(focal, DEFAULT_ONEWAY):.1f} bp\n')
        W('| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | '
          'turnover | net bps/trade | economic ratio |')
        W('|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|')
        for r in sorted(res, key=lambda x: -(x['economic_ratio'] or 0)):
            band = ('n/a' if r['gross_lo'] is None
                    else f"[{r['gross_lo']:+.1f}, {r['gross_hi']:+.1f}]")
            W('| {} | {:,} | {} | {:+.4f} | {:+.4f} | {:+.2f} | {:+.2f} | {} | {:.2f} | '
              '{:+.2f} | **{:.4f}x** |'.format(
                  r['label'], r['n'], r['clusters'], r['ic'], r['rank_ic'],
                  r['top_minus_bottom_bps'], r['gross_bps_per_trade'] or 0.0, band,
                  r['turnover'], r['net_bps_per_trade'] or 0.0, r['economic_ratio'] or 0.0))
            rows.append(dict(r, focal=focal, bar_min=bar_min))
        W('')

    allr = [r for res in results.values() for r in res if r.get('economic_ratio')]
    W('\n## Verdict\n')
    if not allr:
        W('Nothing evaluated.\n')
    else:
        best = max(allr, key=lambda r: r['economic_ratio'])
        clears = [r for r in allr if r['economic_ratio'] and r['economic_ratio'] > 1
                  and r.get('net_lo') is not None and r['net_lo'] > 0]
        W(f"Best across every focal asset and model: **{best['label']}** on "
          f"{best.get('focal','?')} at **{best['economic_ratio']:.3f}x** of the required effect "
          f"({best['gross_bps_per_trade']:+.2f} bp gross against a "
          f"{best['round_trip_bps']:.1f} bp round trip).\n")
        if clears:
            W(f'**{len(clears)} specification(s) clear the hurdle with a bootstrap lower bound '
              f'above zero.** That is the first time anything in this project has. It warrants '
              f'the Kalshi mapping step, and nothing more until that step is done.\n')
        elif best['economic_ratio'] > 1:
            W('The point estimate clears the hurdle but the interval does not. That is a reason '
              'to record more data, not to trade.\n')
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
                'top_minus_bottom_bps', 'gross_bps_per_trade', 'gross_lo', 'gross_hi',
                'turnover', 'round_trip_bps', 'net_bps_per_trade', 'net_lo',
                'required_effect_bps', 'economic_ratio']
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
    ap.add_argument('--md', default='')
    ap.add_argument('--csv', default='')
    args = ap.parse_args()
    md = args.md or os.path.join(ROOT, 'reports', f'cross_crypto_{args.bar}m.md')
    out_csv = args.csv or os.path.join(ROOT, 'reports', f'cross_crypto_{args.bar}m.csv')

    kdir = os.path.join(args.data, 'klines')
    assets = sorted(a for a in os.listdir(kdir)
                    if os.path.isdir(os.path.join(kdir, a))
                    and len(os.listdir(os.path.join(kdir, a))) >= 380)
    rets, ts, names, err = build_panel(args.data, assets, args.bar)
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
                  f"gross {best['gross_bps_per_trade']:+7.2f} bp  "
                  f"net {best['net_bps_per_trade']:+7.2f} bp  "
                  f"ratio {best['economic_ratio']:.3f}x")

    allr = report(results, (names, rets.shape[0], span_days), md, out_csv, args.bar)
    if allr:
        b = max(allr, key=lambda r: r['economic_ratio'])
        print(f"\nbest overall: {b['economic_ratio']:.4f}x of the required effect")
    print(f'wrote {md}')


if __name__ == '__main__':
    main()
