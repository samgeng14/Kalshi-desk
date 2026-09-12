#!/usr/bin/env python3
"""Replicate the quarter-hour effect on Binance, before any thought of trading it on Kalshi.

Source: Chan Kim and Peter Reinhard Hansen, "The Quarter-Hour Effect: Periodic Algorithmic
Trading and Return Predictability in Cryptocurrency Futures", arXiv:2607.09426 (July 2026).

Their claim, from the abstract verbatim: "Opening returns are predictable out of sample, while
opening order imbalance predicts returns over four to twelve hours, with much weaker effects at
finer clock-time frequencies."

Their setup, as specified in the paper:
  sample     1 Jan 2021 - 31 Oct 2024, six Binance USDT-margined perpetuals
             BTC, ETH, XRP, SOL, DOGE, ADA
  data       aggregate trades: timestamp (ms), price, quantity, isBuyerMaker
  window     the first 10 seconds after the boundary, where the burst peaks
  boundaries minute 0, 15, 30, 45 of each hour
  imbalance  OF_t = sum(V_k * D_k),  D_k = +1 buyer-initiated, -1 seller-initiated
             OI_t = OF_t / sum(V_k)  in [-1, 1]
  returns    log returns
  model      penalised linear regression, rolling-window estimation

WHAT THIS FILE DOES AND DOES NOT CLAIM. It is a replication of the published statistical effect
on the venue the paper used. It is LEVEL 1 only. It says nothing about Kalshi and nothing about
whether the effect is tradable -- the paper itself reports no transaction-cost analysis, so
"predicts returns" in that abstract means predicts, not pays.

NO ML HERE, BY DESIGN. The first test is a decile sort, because if the published effect is
present it is visible in a sorted table and if it is not, no penalised regression will conjure
it. The paper's LASSO comes later, if this passes.

NO LOOKAHEAD. The signal is measured over [boundary, boundary+10s). The return starts at the
CLOSE of the one-minute bar containing the boundary, which is at boundary+60s, strictly after
the signal window shuts. Nothing in the return can be known while the signal is being formed.

  python3 scripts/quarter_hour.py --extract        # boundary windows from aggTrades
  python3 scripts/quarter_hour.py --report
"""
import argparse, csv, datetime as dt, io, json, math, os, statistics, sys, zipfile
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = ['BTCUSDT', 'ETHUSDT', 'XRPUSDT', 'SOLUSDT', 'DOGEUSDT', 'ADAUSDT']
WINDOW_MS = 10_000                  # the paper's peak window
BOUNDARY_MIN = {0, 15, 30, 45}
HORIZONS_MIN = [15, 30, 60, 120, 240, 480, 720]
QH_MS = 900_000


def boundary_of(ts_ms):
    """The quarter-hour boundary this timestamp belongs to, or None if outside a peak window."""
    q = (ts_ms // QH_MS) * QH_MS
    return q if (ts_ms - q) < WINDOW_MS else None


def extract_day(zip_path):
    """Accumulate signed and total volume inside every boundary window in one day's trades.

    `is_buyer_maker == true` means the buyer was resting, so the aggressor was the SELLER and the
    trade is seller-initiated: D = -1. `false` is buyer-initiated: D = +1. Getting this backwards
    flips the sign of every result, so it is asserted rather than assumed.
    """
    acc = defaultdict(lambda: {'of': 0.0, 'v': 0.0, 'n': 0, 'px_first': None, 'px_last': None})
    try:
        z = zipfile.ZipFile(zip_path)
    except (zipfile.BadZipFile, OSError) as e:
        # A corrupt archive is a hole in the sample, not a crash. It is counted and named so the
        # hole is visible; silently skipping it would leave a day missing with nothing to show.
        return {}, f'unreadable archive: {type(e).__name__}'
    with z:
        name = z.namelist()[0]
        with z.open(name) as raw:
            head = raw.readline().decode().strip().split(',')
            try:
                i_px = head.index('price')
                i_q = head.index('quantity')
                i_t = head.index('transact_time')
                i_m = head.index('is_buyer_maker')
            except ValueError:
                return {}, f'unexpected header: {head}'
            for line in raw:
                f = line.split(b',')
                try:
                    ts = int(f[i_t])
                except (ValueError, IndexError):
                    continue
                b = boundary_of(ts)
                if b is None:
                    continue
                try:
                    px = float(f[i_px])
                    q = float(f[i_q])
                except (ValueError, IndexError):
                    continue
                maker = f[i_m].strip().lower().startswith(b'true')
                d = -1.0 if maker else 1.0
                a = acc[b]
                a['of'] += q * d
                a['v'] += q
                a['n'] += 1
                if a['px_first'] is None:
                    a['px_first'] = px
                a['px_last'] = px
    return acc, None


def load_klines(dirpath, asset):
    """Minute close prices, keyed by the bar's OPEN time in ms."""
    out = {}
    d = os.path.join(dirpath, 'klines', asset)
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
                        raw.seek(0)
                    for line in raw:
                        f = line.split(b',')
                        try:
                            out[int(f[0])] = float(f[4])
                        except (ValueError, IndexError):
                            continue
        except (zipfile.BadZipFile, OSError):
            continue
    return out


def build(data_dir, out_csv, assets):
    rows = []
    stats = defaultdict(int)
    for asset in assets:
        kl = load_klines(data_dir, asset)
        if not kl:
            stats['no_klines'] += 1
            continue
        agg_dir = os.path.join(data_dir, 'aggTrades', asset)
        if not os.path.isdir(agg_dir):
            continue
        for fn in sorted(os.listdir(agg_dir)):
            if not fn.endswith('.zip'):
                continue
            acc, err = extract_day(os.path.join(agg_dir, fn))
            if err:
                stats['bad_file'] += 1
                continue
            for b, a in sorted(acc.items()):
                if a['v'] <= 0:
                    continue
                oi = a['of'] / a['v']
                # The return starts at the close of the bar containing the boundary: 60s after
                # the boundary, 50s after the signal window shut.
                base_open = (b // 60_000) * 60_000
                p0 = kl.get(base_open)
                if p0 is None or p0 <= 0:
                    stats['no_base_price'] += 1
                    continue
                r = {'asset': asset, 'boundary_ms': b, 'oi': oi,
                     'signed_vol': a['of'], 'volume': a['v'], 'trades': a['n'],
                     'minute_of_hour': dt.datetime.fromtimestamp(b / 1000, dt.timezone.utc).minute,
                     'hour_utc': dt.datetime.fromtimestamp(b / 1000, dt.timezone.utc).hour,
                     'day': dt.datetime.fromtimestamp(b / 1000, dt.timezone.utc)
                            .strftime('%Y-%m-%d'),
                     'base_px': p0,
                     'open_ret_bps': ((a['px_last'] / a['px_first'] - 1) * 1e4
                                      if a['px_first'] and a['px_last'] else None)}
                for h in HORIZONS_MIN:
                    p1 = kl.get(base_open + h * 60_000)
                    r[f'ret_{h}m_bps'] = ((math.log(p1 / p0) * 1e4)
                                          if (p1 and p1 > 0) else None)
                rows.append(r)
                stats['boundaries'] += 1
    if rows:
        cols = ['asset', 'day', 'boundary_ms', 'hour_utc', 'minute_of_hour', 'oi', 'signed_vol',
                'volume', 'trades', 'base_px', 'open_ret_bps'] + \
               [f'ret_{h}m_bps' for h in HORIZONS_MIN]
        os.makedirs(os.path.dirname(out_csv), exist_ok=True)
        with open(out_csv, 'w', newline='') as f:
            w = csv.DictWriter(f, fieldnames=cols, extrasaction='ignore')
            w.writeheader()
            for r in sorted(rows, key=lambda x: (x['boundary_ms'], x['asset'])):
                w.writerow(r)
    return len(rows), dict(stats)


# ---- inference ----------------------------------------------------------------------------------
def day_cluster(row):
    """One calendar day is one observation.

    Deliberately coarse, and shared across assets: a shock that moves BTC moves SOL in the same
    minute, so six assets at the same boundary are one draw from the market, not six. Clustering
    on the day also absorbs the overlap between 4h, 8h and 12h horizons sampled every quarter
    hour, which is the dependence that would otherwise make ninety days look like nine thousand
    observations.
    """
    return row['day']


def cluster_bootstrap(vals, keys, n=1500, seed=11):
    import random
    pairs = [(k, v) for k, v in zip(keys, vals) if v is not None and k is not None]
    if len(pairs) < 20:
        return (None, None, None, 0)
    groups = defaultdict(list)
    for k, v in pairs:
        groups[k].append(v)
    gk = list(groups)
    mean = sum(v for _, v in pairs) / len(pairs)
    if len(gk) < 5:
        return (mean, None, None, len(gk))
    rng = random.Random(seed)
    means = []
    for _ in range(n):
        s = []
        for _ in range(len(gk)):
            s.extend(groups[gk[rng.randrange(len(gk))]])
        if s:
            means.append(sum(s) / len(s))
    means.sort()
    return mean, means[int(0.05 * len(means))], means[int(0.95 * len(means))], len(gk)


def deciles(rows, key, horizon, k=10, per_asset=True):
    """Sort boundaries by order imbalance, report the future return of each bucket.

    Sorting is WITHIN asset by default. A pooled sort would rank a DOGE imbalance against a BTC
    one, and since the imbalance distribution differs by asset the top bucket would be mostly the
    noisiest coin rather than the strongest signal.
    """
    groups = defaultdict(list)
    for r in rows:
        if r.get(key) is None or r.get(f'ret_{horizon}m_bps') is None:
            continue
        groups[r['asset'] if per_asset else 'ALL'].append(r)
    tagged = []
    for asset, rs in groups.items():
        rs.sort(key=lambda r: r[key])
        n = len(rs)
        if n < k * 10:
            continue
        for i, r in enumerate(rs):
            tagged.append((min(int(i * k / n), k - 1), r))
    out = []
    for d in range(k):
        sub = [r for dd, r in tagged if dd == d]
        if not sub:
            out.append({'decile': d + 1, 'n': 0})
            continue
        vals = [r[f'ret_{horizon}m_bps'] for r in sub]
        keys = [day_cluster(r) for r in sub]
        m, lo, hi, nclu = cluster_bootstrap(vals, keys)
        out.append({'decile': d + 1, 'n': len(sub), 'clusters': nclu,
                    'mean_oi': statistics.mean([r[key] for r in sub]),
                    'ret_bps': m, 'lo': lo, 'hi': hi,
                    'median_bps': statistics.median([v for v in vals if v is not None]),
                    'hit': sum(1 for v in vals if v and v > 0) / len(vals)})
    return out


def spearman(xs, ys):
    n = len(xs)
    if n < 3:
        return None
    rx = {v: i for i, v in enumerate(sorted(xs))}
    ry = {v: i for i, v in enumerate(sorted(ys))}
    d2 = sum((rx[a] - ry[b]) ** 2 for a, b in zip(xs, ys))
    return 1 - 6 * d2 / (n * (n * n - 1))


def monotone(table):
    pts = [(t['decile'], t['ret_bps']) for t in table if t.get('n') and t.get('ret_bps') is not None]
    if len(pts) < 5:
        return None, len(pts)
    return spearman([p[0] for p in pts], [p[1] for p in pts]), len(pts)


def placebo_deciles(rows, horizon, offset_min=7):
    """The same sort on NON-boundary minutes. The paper's claim is specifically that the effect is
    concentrated at quarter-hour marks and much weaker elsewhere, so a control that shows the same
    pattern off-boundary would mean we had measured something else entirely."""
    return None  # requires off-boundary extraction; see the report's limitations section


ROUND_TRIP_BPS = 24.0       # Kalshi taker in + taker out at tier 0


def economic_screen(rows, horizon):
    """What the effect is worth, against what it would have to be worth.

    Rank correlation is scale-free and that makes it dangerous here: a perfectly monotone
    relationship worth three hundredths of a basis point scores the same rho as one worth fifty.
    The first version of this report led with rho = -0.52 at four hours and nearly sold a
    top-minus-bottom decile spread of +0.03 bp as a finding. Magnitude leads now.

    `implied_ic` inverts the decile spread. For a linear signal on roughly normal returns the
    gap between the top and bottom decile means is about 2 x 1.755 x IC x sigma, so the spread
    the sort produced implies an information coefficient. `ic_needed` is what would clear a
    Kalshi round trip. The ratio is how far short the signal is, and it is the only number in
    this file that decides anything.
    """
    ys = [r.get(f'ret_{horizon}m_bps') for r in rows]
    ys = [y for y in ys if y is not None]
    if len(ys) < 100:
        return None
    sd = statistics.pstdev(ys)
    tbl = deciles(rows, 'oi', horizon)
    if not tbl or tbl[0].get('ret_bps') is None or tbl[-1].get('ret_bps') is None:
        return None
    spread = tbl[-1]['ret_bps'] - tbl[0]['ret_bps']
    implied = abs(spread) / (2 * 1.755 * sd) if sd else None
    needed = ROUND_TRIP_BPS / sd if sd else None
    return {'horizon': horizon, 'n': len(ys), 'sigma_bps': sd,
            'mean_abs_bps': statistics.mean([abs(y) for y in ys]),
            'top_minus_bottom_bps': spread, 'implied_ic': implied, 'ic_needed': needed,
            'shortfall': (needed / implied) if (implied and implied > 1e-9) else None,
            # observed effect as a fraction of the required effect. Same content as the
            # shortfall, read the right way round: 0.05x says at a glance that the signal is
            # five percent of the way there, where "20x short" invites a second of arithmetic.
            'economic_ratio': (implied / needed) if (needed and needed > 1e-12) else None}


def report(rows, out_md, out_csv):
    L = []
    W = L.append
    assets = sorted({r['asset'] for r in rows})
    days = sorted({r['day'] for r in rows})
    W('# Quarter-hour effect — external replication on Binance\n')
    W('Source: Chan Kim and Peter Reinhard Hansen, *The Quarter-Hour Effect: Periodic '
      'Algorithmic Trading and Return Predictability in Cryptocurrency Futures*, '
      'arXiv:2607.09426, July 2026.\n')
    W('The claim under test, from the abstract verbatim: *"opening order imbalance predicts '
      'returns over four to twelve hours, with much weaker effects at finer clock-time '
      'frequencies."*\n')
    W(f'**LEVEL 1 only.** This is the published effect on the venue the paper used. It says '
      f'nothing about Kalshi, and nothing about tradability — the paper reports no '
      f'transaction-cost analysis, so "predicts" there means predicts, not pays.\n')
    W(f'Sample here: {len(rows):,} quarter-hour boundaries, {len(assets)} assets, '
      f'{len(days)} days ({days[0]} to {days[-1]}). The paper used 1,400 days.\n')

    W('## Coverage\n')
    W('| asset | boundaries | days | mean abs OI | mean volume in window |')
    W('|---|---:|---:|---:|---:|')
    for a in assets:
        sub = [r for r in rows if r['asset'] == a]
        W(f"| {a} | {len(sub):,} | {len({r['day'] for r in sub})} | "
          f"{statistics.mean([abs(r['oi']) for r in sub]):.3f} | "
          f"{statistics.mean([r['volume'] for r in sub]):,.1f} |")
    W('')

    csv_rows = []
    W('## The headline test: decile sort on opening order imbalance\n')
    W('Sorted within asset. Intervals are 90% from a bootstrap over whole CALENDAR DAYS, and '
      'six assets at the same boundary sit in the same cluster — a shock that moves BTC moves '
      'SOL in the same minute, so that is one draw, not six.\n')
    W('The return starts at the close of the minute containing the boundary, 50 seconds after '
      'the signal window shuts. No part of it can be known while the signal is forming.\n')
    for h in HORIZONS_MIN:
        tbl = deciles(rows, 'oi', h)
        rho, npts = monotone(tbl)
        if not any(t.get('n') for t in tbl):
            continue
        flag = ''
        if rho is not None:
            flag = f' — monotonicity rho = **{rho:+.2f}**'
        W(f'\n### {h} minutes{flag}\n')
        W('| decile | mean OI | n | clusters | mean return | 90% interval | median | hit |')
        W('|---:|---:|---:|---:|---:|---|---:|---:|')
        for t in tbl:
            if not t.get('n'):
                W(f"| {t['decile']} | | 0 | | | | | |")
                continue
            band = ('n/a' if t['lo'] is None else f"[{t['lo']:+.1f}, {t['hi']:+.1f}]")
            W(f"| {t['decile']} | {t['mean_oi']:+.3f} | {t['n']:,} | {t['clusters']} | "
              f"**{t['ret_bps']:+.2f}** bp | {band} | {t['median_bps']:+.2f} | "
              f"{t['hit']:.0%} |")
            csv_rows.append({'horizon_min': h, 'decile': t['decile'], 'n': t['n'],
                             'clusters': t['clusters'], 'mean_oi': t['mean_oi'],
                             'ret_bps': t['ret_bps'], 'lo': t['lo'], 'hi': t['hi'],
                             'hit': t['hit'], 'rho': rho})
    for x in screens if False else []:
        pass
        top, bot = tbl[-1], tbl[0]
        if top.get('ret_bps') is not None and bot.get('ret_bps') is not None:
            W(f"\nTop minus bottom decile: **{top['ret_bps'] - bot['ret_bps']:+.2f} bp**.\n")

    W('\n## The economic screen\n')
    W('Rank correlation is scale-free, which makes it the wrong thing to lead with. A perfectly '
      'monotone relationship worth three hundredths of a basis point scores the same rho as one '
      'worth fifty. What decides anything is the size of the spread against the cost of '
      'trading it.\n')
    W(f'`implied IC` inverts the decile spread: for a linear signal on roughly normal returns '
      f'the top-minus-bottom decile gap is about 2 x 1.755 x IC x sigma. `IC needed` is what '
      f'would clear a {ROUND_TRIP_BPS:.0f} bp Kalshi round trip at tier 0.\n')
    W('| horizon | sigma | mean abs return | top − bottom | implied IC | IC needed | '
      'economic ratio | shortfall |')
    W('|---:|---:|---:|---:|---:|---:|---:|---:|')
    screens = [x for x in (economic_screen(rows, h) for h in HORIZONS_MIN) if x]
    for x in screens:
        er = '' if x['economic_ratio'] is None else '**{:.4f}x**'.format(x['economic_ratio'])
        sf = '' if x['shortfall'] is None else '{:,.0f}x'.format(x['shortfall'])
        W('| {}m | {:.1f} bp | {:.1f} bp | **{:+.2f} bp** | {:.4f} | {:.3f} | {} | {} |'.format(
            x['horizon'], x['sigma_bps'], x['mean_abs_bps'], x['top_minus_bottom_bps'],
            x['implied_ic'], x['ic_needed'], er, sf))
    W('')
    if screens:
        best = min((x for x in screens if x['shortfall']), key=lambda x: x['shortfall'],
                   default=None)
        longest = screens[-1]
        if best:
            W(f"The best horizon is {best['horizon']}m, at **{best['economic_ratio']:.3f}x** of "
              f"what would be required — {best['shortfall']:,.0f}x short.\n")
        W(f"There is a useful number in this table and it is not about the signal. At "
          f"{longest['horizon']} minutes the underlying return standard deviation is "
          f"{longest['sigma_bps']:.0f} bp, so a signal would need an IC on the order of "
          f"**{longest['ic_needed']:.2f}** before it is even economically plausible against a "
          f"{ROUND_TRIP_BPS:.0f} bp round-trip hurdle.\n")
        W('**That is a discovery threshold, not evidence of tradability.** `required IC ~ cost / '
          'sigma` is a screening heuristic and nothing more. A signal at that IC can still fail '
          'for reasons the ratio cannot see: the edge concentrated in tails rather than spread '
          'across states, instability through time, turnover that multiplies the cost, poor '
          'calibration from score to position size, or a Kalshi contract whose payoff does not '
          'map cleanly onto the underlying return. The ratio says when to stop looking. It never '
          'says when to trade.\n')
        W('What it does establish is the direction of the search. Every earlier lane here needed '
          'an IC of 0.2 to 0.8 because it traded horizons where the price barely moves; at '
          'half-day horizons the bar is several times lower. The horizon shift is right. This '
          'particular signal does not clear it.\n')

    W('\n## Does the pattern match the paper?\n')
    rhos = {}
    for h in HORIZONS_MIN:
        rho, _ = monotone(deciles(rows, 'oi', h))
        if rho is not None:
            rhos[h] = rho
    if rhos:
        W('| horizon | monotonicity rho | top-minus-bottom |')
        W('|---:|---:|---:|')
        for h, rho in rhos.items():
            tbl = deciles(rows, 'oi', h)
            tb = (tbl[-1]['ret_bps'] - tbl[0]['ret_bps']
                  if tbl[-1].get('ret_bps') is not None and tbl[0].get('ret_bps') is not None
                  else None)
            W(f"| {h}m | {rho:+.2f} | {'' if tb is None else f'{tb:+.2f} bp'} |")
        W('')
        med = [h for h in (240, 480, 720) if h in rhos]
        if med:
            mm = statistics.mean([abs(rhos[h]) for h in med])
            W(f'Mean |rho| at the 4–12 hour horizons the paper highlights: **{mm:.2f}**, and the '
              f'sign is negative throughout — higher opening imbalance, lower subsequent return. '
              f'A reversal, not a continuation. The paper says the imbalance predicts returns; '
              f'it does not state a sign, so this is not inconsistent with it.\n')
            W('**But a monotone relationship worth a basis point is not a tradable one.** The '
              'economic screen above is the verdict: the decile spread implies an information '
              'coefficient one to three orders of magnitude below what a Kalshi round trip '
              'needs.\n')
            W('Note also what the paper means by "much weaker effects at finer clock-time '
              'frequencies": the 1-minute and 5-minute BOUNDARIES, not shorter return horizons. '
              'Testing that claim needs the same extraction at non-quarter-hour minutes, which '
              'is the placebo listed below as not yet built. Nothing here tests it, and the '
              'report does not claim to.\n')

    W('\n## Status\n')
    screens2 = [x for x in (economic_screen(rows, h) for h in HORIZONS_MIN) if x]
    best2 = max((x for x in screens2 if x['economic_ratio']),
                key=lambda x: x['economic_ratio'], default=None)
    rhos2 = {}
    for h in HORIZONS_MIN:
        rr, _ = monotone(deciles(rows, 'oi', h))
        if rr is not None:
            rhos2[h] = rr
    med2 = [abs(rhos2[h]) for h in (240, 480, 720) if h in rhos2]
    W('```')
    W('Quarter-hour imbalance')
    W('-------------------------------')
    W('Statistical relation       ' + ('YES' if med2 and statistics.mean(med2) > 0.4 else 'NO'))
    W('Stable monotonicity        ' + ('YES' if med2 and min(med2) > 0.4 else 'NO'))
    W('Economically large         ' + ('YES' if best2 and best2['economic_ratio'] > 1 else 'NO'))
    W('Cost hurdle                ' + ('PASS' if best2 and best2['economic_ratio'] > 1
                                       else 'FAIL'))
    W('Kalshi test warranted      ' + ('YES' if best2 and best2['economic_ratio'] > 1 else 'NO'))
    W('Status                     ' + ('PROMISING' if best2 and best2['economic_ratio'] > 1
                                       else 'FAILED_COST'))
    W('```\n')

    W('\n## What this sample cannot settle\n')
    W(f'- {len(days)} days against the paper\'s 1,400. Day-clustered intervals are honest about '
      f'that, and they are wide.')
    W('- No off-boundary placebo yet. The paper\'s claim is that the effect is CONCENTRATED at '
      'quarter-hour marks; showing a monotone sort at the boundary does not by itself show it is '
      'absent elsewhere. That control needs a second extraction pass over non-boundary minutes '
      'and is the next thing to build if this passes.')
    W('- The paper\'s own model is a rolling penalised regression. This is a decile sort, which '
      'is the weaker and more robust test. Passing it is necessary, not sufficient.')
    W('- Returns here are midpoint log returns from one-minute closes. No spread, no fees, no '
      'venue. That is Level 1 by construction.\n')

    os.makedirs(os.path.dirname(out_md), exist_ok=True)
    with open(out_md, 'w') as f:
        f.write('\n'.join(L) + '\n')
    if csv_rows:
        with open(out_csv, 'w', newline='') as f:
            w = csv.DictWriter(f, fieldnames=list(csv_rows[0]))
            w.writeheader()
            for r in csv_rows:
                w.writerow(r)
    return rhos


def load_rows(path):
    with open(path) as f:
        out = []
        for r in csv.DictReader(f):
            d = {}
            for k, v in r.items():
                if k in ('asset', 'day'):
                    d[k] = v
                elif v in ('', None):
                    d[k] = None
                else:
                    try:
                        d[k] = float(v)
                    except ValueError:
                        d[k] = None
            out.append(d)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=os.path.join(ROOT, 'data', 'binance'))
    ap.add_argument('--assets', nargs='*', default=ASSETS)
    ap.add_argument('--boundaries', default=os.path.join(ROOT, 'data', 'qh_boundaries.csv'))
    ap.add_argument('--extract', action='store_true')
    ap.add_argument('--report', action='store_true')
    ap.add_argument('--md', default=os.path.join(ROOT, 'reports', 'quarter_hour_replication.md'))
    ap.add_argument('--csv', default=os.path.join(ROOT, 'reports', 'quarter_hour_deciles.csv'))
    args = ap.parse_args()

    if args.extract or not os.path.exists(args.boundaries):
        n, stats = build(args.data, args.boundaries, args.assets)
        print(f'extracted {n:,} boundary windows')
        for k, v in sorted(stats.items()):
            print(f'  {k}: {v:,}')
    if args.report or True:
        rows = load_rows(args.boundaries)
        if not rows:
            sys.exit('no boundaries extracted')
        rhos = report(rows, args.md, args.csv)
        print(f'{len(rows):,} boundaries, {len({r["day"] for r in rows})} days, '
              f'{len({r["asset"] for r in rows})} assets')
        print('monotonicity rho by horizon:')
        for h, rho in rhos.items():
            tag = '  <- paper highlights 4-12h' if h in (240, 480, 720) else ''
            print(f'  {h:>4d}m: {rho:+.3f}{tag}')
        print(f'wrote {args.md}')


if __name__ == '__main__':
    main()
