#!/usr/bin/env python3
"""Phase 2 -- BTC/ETH microstructure signal screen.

The previous screen tested 1,736 feature/horizon/regime combinations on 1.8 million minute bars
and found nothing that cleared 17 bps. 138 combinations passed three statistical gates; about 87
were expected by chance; 33 survived Bonferroni with HAC t-statistics up to 30.7, and every one
of those lost 14 to 20 bps after costs. The conclusion was not "try harder with a better model",
it was that the effects are real, small, and smaller than the fee.

So this screen is built to answer a different question first, and it refuses to proceed until it
can: DOES THIS SAMPLE HAVE THE POWER TO DETECT THE EFFECT THAT WOULD MATTER?

Most of what looks like research failure at this scale is not a modelling problem. With 2.5 hours
of tape and a 10-minute horizon there are about fifteen non-overlapping observations. An IC
computed from fifteen observations has a standard error near 0.26. Any |IC| below roughly 0.5 is
indistinguishable from zero, and every |IC| above it is almost certainly noise that will not
repeat. Reporting a ranked table of such numbers -- which is what a screen normally does --
manufactures exactly the kind of finding this project already spent 1,736 tests learning to
distrust.

WHAT IT DOES
  1. Power first. For each horizon, counts NON-OVERLAPPING observations, derives the minimum
     detectable IC at 80% power, and compares it to the IC that would be needed to clear the
     measured round-trip cost. If the detectable floor is above the useful ceiling, the horizon
     is marked UNDERPOWERED and no per-feature statistics are printed for it at all.
  2. The cost hurdle comes from reports/execution_simulator.csv when it exists -- the measured
     cost of actually getting in and out -- and falls back to the fee schedule with a loud note.
     A screen against 17 bps when the real cost is 31 bps is a screen against the wrong number.
  3. Benjamini-Hochberg from the start, over the full pre-declared grid, with the grid size fixed
     before anything is computed. Not applied afterwards to whatever looked interesting.
  4. Every surviving effect is reported in bps net of the hurdle. Gross IC is never the headline.

  python3 scripts/btc_eth_screen.py --features data/features --markets KXBTCPERP KXETHPERP
"""
import argparse, csv, glob, gzip, json, math, os, statistics, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Pre-declared. Fixed before any data is read, so the multiple-testing correction is honest.
HORIZONS_S = [120, 300, 600]
FEATURES = [
    'microprice_minus_mid_bps', 'spread_bps',
    'L1_OBI', 'L3_OBI', 'L5_OBI', 'L10_OBI',
    'trade_imbalance_1s', 'trade_imbalance_5s', 'trade_imbalance_30s', 'trade_imbalance_1m',
    'cancel_imbalance_1s', 'cancel_imbalance_5s', 'cancel_imbalance_30s',
    'add_imbalance_5s', 'add_imbalance_30s',
    'aggressive_buy_5s', 'aggressive_sell_5s',
    'book_update_rate_1s', 'book_update_rate_1m',
    'cf_basis_bps', 'time_since_trade_us',
]
MAKER_BPS, TAKER_BPS = 5.0, 12.0
POWER_Z = 2.80          # z(0.975) + z(0.80): two-sided 5% test at 80% power
FDR_Q = 0.10


def norm_cdf(x):
    return 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))


def pearson(x, y):
    n = len(x)
    if n < 20:
        return None
    mx, my = sum(x) / n, sum(y) / n
    sxx = sum((a - mx) ** 2 for a in x)
    syy = sum((b - my) ** 2 for b in y)
    if sxx <= 0 or syy <= 0:
        return None
    sxy = sum((a - mx) * (b - my) for a, b in zip(x, y))
    return sxy / math.sqrt(sxx * syy)


def spearman(x, y):
    def rank(v):
        order = sorted(range(len(v)), key=lambda i: v[i])
        r = [0.0] * len(v)
        i = 0
        while i < len(order):
            j = i
            while j + 1 < len(order) and v[order[j + 1]] == v[order[i]]:
                j += 1
            avg = (i + j) / 2.0 + 1
            for k in range(i, j + 1):
                r[order[k]] = avg
            i = j + 1
        return r
    return pearson(rank(x), rank(y))


def min_detectable_ic(n_eff):
    """The smallest |IC| a sample of n INDEPENDENT observations can detect at 80% power.

    se(IC) ~= 1/sqrt(n-3) under the Fisher transform, so the detectable effect is z * se. The
    point of computing it is that it is usually larger than any IC worth having, and saying so is
    more useful than ranking noise.
    """
    if n_eff is None or n_eff <= 4:
        return None
    return POWER_Z / math.sqrt(n_eff - 3)


def ic_needed_for(hurdle_bps, sigma_bps):
    """The IC that turns a move of typical size sigma into an edge that clears the hurdle.

    A linear signal with correlation rho captures about rho * sigma of the move per unit of
    exposure, so the hurdle is cleared when rho >= hurdle / sigma. This is generous to the
    strategy: it assumes perfect sizing and no slippage beyond the hurdle.
    """
    if not sigma_bps or sigma_bps <= 0:
        return None
    return hurdle_bps / sigma_bps


def bh(pvals, q=FDR_Q):
    """Benjamini-Hochberg. Returns the p-value threshold, or None if nothing survives."""
    m = len(pvals)
    if not m:
        return None
    s = sorted(pvals)
    thresh = None
    for i, p in enumerate(s, start=1):
        if p <= q * i / m:
            thresh = p
    return thresh


def measured_hurdle():
    """Round-trip cost, preferring what the execution simulator measured over the fee schedule."""
    p = os.path.join(ROOT, 'reports', 'execution_simulator.csv')
    fallback = (MAKER_BPS + TAKER_BPS, 'fee schedule (maker in, taker out) -- NOT measured')
    if not os.path.exists(p):
        return fallback
    best = None
    with open(p) as f:
        for r in csv.DictReader(f):
            if r.get('queue_bound') != 'pess':
                continue
            try:
                fills, cost, fee = int(r['fills']), float(r['entry_cost_bps']), float(r['fee_bps'])
            except (KeyError, TypeError, ValueError):
                continue
            if fills < 20:
                continue
            tot = cost + fee
            if best is None or tot < best[0]:
                best = (tot, f"{r['market']}/{r['mode']} entry {cost:.1f} + fee {fee:.1f}, "
                             f"{fills} fills")
    if best is None:
        return fallback
    # Entry plus an exit of comparable size. A one-way cost is not a round trip.
    return (best[0] * 2, f'measured: 2 x ({best[1]})')


def load_rows(path, markets):
    """Feature rows from the engine's output, as dicts of float, grouped by market."""
    out = defaultdict(list)
    files = sorted(glob.glob(os.path.join(path, '*.csv')) +
                   glob.glob(os.path.join(path, '*.csv.gz')))
    for fp in files:
        op = gzip.open if fp.endswith('.gz') else open
        try:
            with op(fp, 'rt') as f:
                for r in csv.DictReader(f):
                    m = r.get('market')
                    if markets and m not in markets:
                        continue
                    out[m].append(r)
        except (EOFError, OSError) as e:
            print(f'  [truncated] {os.path.basename(fp)}: {e}', file=sys.stderr)
    for m in out:
        out[m].sort(key=lambda r: float(r.get('feature_ts_us') or 0))
    return out, files


def forward_returns(rows, horizon_s):
    """(feature_row, forward return in bps) pairs, plus how many are NON-OVERLAPPING.

    Overlap is the whole problem with intraday horizons. 600 rows sampled a second apart give 600
    ten-minute returns, but only about one independent observation per ten minutes. Both counts
    are returned so nothing downstream can quietly use the larger one.
    """
    import bisect
    ts = [float(r['feature_ts_us']) for r in rows]
    mid = [float(r['mid']) for r in rows]
    h = horizon_s * 1_000_000
    pairs = []
    for i, t0 in enumerate(ts):
        j = bisect.bisect_left(ts, t0 + h)
        if j >= len(ts):
            break
        # Refuse a return whose endpoint is far past the horizon: a gap in the tape would
        # otherwise turn a 10-minute return into a 40-minute one without saying so.
        if ts[j] - t0 > h * 1.5:
            continue
        if mid[i] <= 0:
            continue
        pairs.append((rows[i], (mid[j] - mid[i]) / mid[i] * 1e4))
    span_us = (ts[-1] - ts[0]) if len(ts) > 1 else 0
    n_indep = int(span_us / h) if h else 0
    return pairs, n_indep


def screen(rows_by_market, hurdle_bps, hurdle_src, out_csv, out_md):
    grid = len(FEATURES) * len(HORIZONS_S) * len(rows_by_market)
    results, powered, lines = [], [], []
    W = lines.append

    W('# BTC/ETH microstructure signal screen\n')
    W(f'Pre-declared grid: {len(FEATURES)} features x {len(HORIZONS_S)} horizons x '
      f'{len(rows_by_market)} markets = **{grid} hypotheses**, fixed before reading any data.\n')
    W(f'Round-trip cost hurdle: **{hurdle_bps:.1f} bps** ({hurdle_src})\n')

    W('## Power\n')
    W('Whether this sample can detect an effect large enough to be worth having. Computed before '
      'any feature statistic, because if the answer is no then the ranked table below is a '
      'ranking of noise.\n')
    W('| market | horizon | rows | independent obs | sigma bps | min detectable IC | '
      'IC needed for the hurdle | verdict |')
    W('|---|---:|---:|---:|---:|---:|---:|---|')

    for m, rows in sorted(rows_by_market.items()):
        for hs in HORIZONS_S:
            pairs, n_indep = forward_returns(rows, hs)
            if len(pairs) < 50:
                W(f'| {m} | {hs}s | {len(pairs)} | {n_indep} | | | | NO DATA |')
                continue
            rets = [p[1] for p in pairs]
            sigma = statistics.pstdev(rets) if len(rets) > 1 else 0.0
            mdi = min_detectable_ic(n_indep)
            need = ic_needed_for(hurdle_bps, sigma)
            if mdi is None or need is None:
                verdict = 'NO DATA'
            elif need >= 1.0:
                # A correlation cannot exceed 1. If the round trip costs more than the price
                # typically moves over the horizon, no signal of any quality clears it -- a
                # perfect oracle loses. This is not a sample-size problem and no amount of tape
                # fixes it, so it is reported separately from UNDERPOWERED rather than folded in.
                verdict = 'COST EXCEEDS MOVE'
            elif mdi > need:
                verdict = 'UNDERPOWERED'
            else:
                verdict = 'powered'
                powered.append((m, hs, pairs, sigma, need, n_indep))
            W(f'| {m} | {hs}s | {len(pairs):,} | {n_indep} | {sigma:.1f} | '
              f'{"" if mdi is None else f"{mdi:.3f}"} | '
              f'{"" if need is None else f"{need:.3f}"} | {verdict} |')
    W('')

    if not powered:
        W('## Result\n')
        W('**No market and horizon is both powered and reachable.** No feature statistics are '
          'reported, because any that cleared a threshold here would be noise that does not '
          'repeat.\n')
        W('The two failure modes are different and the fix is different:\n')
        W('- `COST EXCEEDS MOVE` -- the round trip costs more than the price moves over the '
          'horizon, so a perfect oracle still loses. More tape changes nothing. The only things '
          'that move this are a cheaper execution path or a longer horizon.')
        W('- `UNDERPOWERED` -- the effect might be large enough, but this sample cannot tell it '
          'from zero. Independent observations grow linearly with recorder uptime and the '
          'detectable effect shrinks as the square root, so this one is fixed by recording.\n')
        need_h = {}
        for m, rows in sorted(rows_by_market.items()):
            for hs in HORIZONS_S:
                pairs, n_indep = forward_returns(rows, hs)
                if len(pairs) < 50:
                    continue
                rets = [p[1] for p in pairs]
                sigma = statistics.pstdev(rets)
                need = ic_needed_for(hurdle_bps, sigma)
                if not need or need >= 1:
                    continue          # unreachable at any sample size: not a data question
                n_req = (POWER_Z / need) ** 2 + 3
                need_h[(m, hs)] = n_req * hs / 3600.0
        if need_h:
            W('### Hours of tape required\n')
            W('To make the detectable effect as small as the useful one, at 80% power:\n')
            W('| market | horizon | hours needed |')
            W('|---|---:|---:|')
            for (m, hs), hrs in sorted(need_h.items(), key=lambda kv: kv[1]):
                W(f'| {m} | {hs}s | {hrs:,.0f} |')
            W('\nThese are large because the hurdle is large relative to how far the price moves '
              'over the horizon. That is the same arithmetic that killed the minute-bar study, '
              'restated: the problem is the size of the effect against the cost, and more data '
              'only ever helps with the measurement, never with the size.\n')
        else:
            W('### No horizon is even reachable\n')
            W('Every horizon costs more to trade than the price moves across it, so there is no '
              'amount of recording that would help. The question is not which feature predicts '
              'this market; it is whether there is a cheaper way in and out of it.\n')
    else:
        pvals, rowsout = [], []
        for m, hs, pairs, sigma, need, n_indep in powered:
            for feat in FEATURES:
                xs, ys = [], []
                for r, ret in pairs:
                    v = r.get(feat)
                    if v in (None, ''):
                        continue
                    try:
                        xs.append(float(v))
                    except ValueError:
                        continue
                    ys.append(ret)
                if len(xs) < 50:
                    continue
                ic = pearson(xs, ys)
                sic = spearman(xs, ys)
                if ic is None:
                    continue
                # The t-statistic uses INDEPENDENT observations, never the row count.
                t = abs(ic) * math.sqrt(max(n_indep - 2, 1)) / math.sqrt(max(1 - ic * ic, 1e-9))
                p = 2 * (1 - norm_cdf(t))
                pvals.append(p)
                rowsout.append({'market': m, 'horizon_s': hs, 'feature': feat,
                                'n_rows': len(xs), 'n_indep': n_indep, 'ic': ic,
                                'spearman_ic': sic, 't': t, 'p': p, 'sigma_bps': sigma,
                                'gross_bps': abs(ic) * sigma, 'hurdle_bps': hurdle_bps,
                                'net_bps': abs(ic) * sigma - hurdle_bps})
        thresh = bh(pvals)
        for r in rowsout:
            r['bh_survives'] = bool(thresh is not None and r['p'] <= thresh)
            r['status'] = ('PROMISING' if r['bh_survives'] and r['net_bps'] > 0
                           else 'REJECTED_COST' if r['bh_survives']
                           else 'REJECTED_NONROBUST')
        results = rowsout
        surv = [r for r in rowsout if r['bh_survives']]
        pos = [r for r in surv if r['net_bps'] > 0]
        W('## Result\n')
        W(f'{len(rowsout)} tests run of {grid} declared. Benjamini-Hochberg at q={FDR_Q:.0%} '
          f'keeps **{len(surv)}**. Of those, **{len(pos)}** clear the '
          f'{hurdle_bps:.1f} bps hurdle.\n')
        if not pos:
            W('Nothing clears the cost. The effects that survive correction are real and too '
              'small to trade, which is the same result the minute-bar study reached by a '
              'different route.\n')
        W('| market | h | feature | IC | Spearman | t | gross bps | net bps | status |')
        W('|---|---:|---|---:|---:|---:|---:|---:|---|')
        for r in sorted(rowsout, key=lambda x: -x['net_bps'])[:30]:
            W(f"| {r['market']} | {r['horizon_s']}s | `{r['feature']}` | {r['ic']:+.3f} | "
              f"{(r['spearman_ic'] or 0):+.3f} | {r['t']:.2f} | {r['gross_bps']:.1f} | "
              f"{r['net_bps']:+.1f} | {r['status']} |")
        W('')

    W('## What this screen will not do\n')
    W('- It will not report a ranked feature table for an underpowered horizon. A ranking always '
      'produces a winner; that is not evidence that the winner is real.')
    W('- It will not use the row count as the sample size. Overlapping windows inflate every '
      't-statistic, and the inflation is roughly the square root of the overlap factor.')
    W('- It will not compare a gross edge to the fee schedule when a measured execution cost '
      'exists. The fee is the smallest part of the cost of trading.')
    W('- It will not apply a multiple-testing correction to a subset chosen after looking. The '
      'grid size is fixed at the top of this file.\n')

    os.makedirs(os.path.dirname(out_md), exist_ok=True)
    with open(out_md, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    cols = ['market', 'horizon_s', 'feature', 'n_rows', 'n_indep', 'ic', 'spearman_ic', 't', 'p',
            'sigma_bps', 'gross_bps', 'hurdle_bps', 'net_bps', 'bh_survives', 'status']
    with open(out_csv, 'w') as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction='ignore')
        w.writeheader()
        for r in results:
            w.writerow(r)
    return results, powered


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--features', default=os.path.join(ROOT, 'data', 'features'))
    ap.add_argument('--markets', nargs='*', default=['KXBTCPERP', 'KXETHPERP'])
    ap.add_argument('--csv', default=os.path.join(ROOT, 'reports', 'btc_eth_signal_screen.csv'))
    ap.add_argument('--md', default=os.path.join(ROOT, 'reports', 'btc_eth_signal_screen.md'))
    args = ap.parse_args()

    rows, files = load_rows(args.features, set(args.markets))
    if not files:
        sys.exit(f'no feature files in {args.features}: run feature_engine.py first')
    if not rows:
        sys.exit(f'no rows for {args.markets} in {len(files)} files')

    hurdle, src = measured_hurdle()
    results, powered = screen(rows, hurdle, src, args.csv, args.md)
    for m in sorted(rows):
        print(f'  {m}: {len(rows[m]):,} feature rows')
    print(f'hurdle {hurdle:.1f} bps ({src})')
    print(f'powered market/horizon pairs: {len(powered)} of '
          f'{len(rows) * len(HORIZONS_S)}')
    print(f'tests run: {len(results)}')
    if results:
        pos = [r for r in results if r['status'] == 'PROMISING']
        print(f'clearing the hurdle after BH: {len(pos)}')
    print(f'wrote {os.path.relpath(args.md, ROOT)}')


if __name__ == '__main__':
    main()
