#!/usr/bin/env python3
"""The impulse sign table, and the no-model baselines. This runs before any model.

The question, stated so that a flat answer ends the lane cheaply:

    When the external venues move hard and Kalshi lags, does Kalshi follow -- and by enough to
    pay for crossing the spread twice?

The shape that would justify modelling is monotone: bigger external impulse, bigger subsequent
Kalshi move. A flat or sign-flipping column means no amount of gradient boosting will help, and
the honest response is to stop rather than to add features.

TWO NUMBERS PER CELL, never one.

  `mid` is the midpoint move. It answers "did the price go there". It is research only and no
  profitability claim may rest on it.
  `net` is a taker round trip at recorded quotes: enter at the ask, leave at the bid, pay two
  taker fees. It answers "could we have had it".

SEPARATION IS ENFORCED HERE, not at emission. Events are emitted on a two-minute cooldown so the
short horizon keeps its sample; a 10-minute study filters to events at least 10 minutes apart
before it computes anything, and reports how many survived.

INFERENCE IS ON CLUSTERS. Every interval is a bootstrap over whole time clusters, and the number
of independent clusters is printed next to the event count. Two thousand events inside fifty
market minutes is fifty observations.

  python3 scripts/impulse_sign_table.py --events reports/impulse_events.csv
"""
import argparse, csv, math, os, statistics, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import conditional_markout as CM
from execution_sim import TAKER_BPS

HORIZONS = [120, 300, 600]
BUCKETS = [(0, 5), (5, 10), (10, 15), (15, 20), (20, 30), (30, 50), (50, 10 ** 9)]
BUCKET_LABEL = ['0-5', '5-10', '10-15', '15-20', '20-30', '30-50', '50+']


def num(v):
    if v in ('', None):
        return None
    try:
        f = float(v)
    except (TypeError, ValueError):
        return None
    return None if f != f else f


def load(path):
    with open(path) as f:
        rows = [{k: num(v) if k not in ('market', 'trigger_window') else v
                 for k, v in r.items()} for r in csv.DictReader(f)]
    rows.sort(key=lambda r: r['entry_us'] or 0)
    return rows


def separate(rows, horizon_s):
    """Keep only events at least one horizon apart, per market. Overlapping windows are not
    independent trades and must not be counted as such."""
    out, last = [], {}
    need = horizon_s * 1_000_000
    for r in rows:
        m, t = r['market'], r['entry_us']
        if t is None:
            continue
        if m in last and t - last[m] < need:
            continue
        last[m] = t
        out.append(r)
    return out


def bucket_of(mag):
    a = abs(mag)
    for i, (lo, hi) in enumerate(BUCKETS):
        if lo <= a < hi:
            return i
    return len(BUCKETS) - 1


def cell(rows, key):
    vals = [r.get(key) for r in rows]
    keys = [CM.time_cluster(r['entry_us']) for r in rows]
    m, lo, hi, n_clu = CM.cluster_bootstrap(vals, keys, want_n=True)
    got = [v for v in vals if v is not None]
    return {'n': len(got), 'clusters': n_clu, 'mean': m,
            'median': statistics.median(got) if got else None,
            'lo': lo, 'hi': hi,
            'hit': (sum(1 for v in got if v > 0) / len(got)) if got else None}


def sign_table(rows, market, horizon_s):
    sub = [r for r in rows if r['market'] == market]
    out = []
    for i, lab in enumerate(BUCKET_LABEL):
        b = [r for r in sub if r.get('impulse_bps') is not None
             and bucket_of(r['impulse_bps']) == i]
        if not b:
            out.append({'bucket': lab, 'n': 0})
            continue
        lag_key = 'lag_5s_bps'
        lags = [r.get(lag_key) for r in b if r.get(lag_key) is not None]
        row = {'bucket': lab,
               'mean_impulse': statistics.mean([abs(r['impulse_bps']) for r in b]),
               'mean_lag': statistics.mean(lags) if lags else None}
        row['mid'] = cell(b, f'signed_mid_ret_{horizon_s}s')
        row['net'] = cell(b, f'signed_taker_net_{horizon_s}s')
        row['gross'] = cell(b, f'signed_taker_gross_{horizon_s}s')
        row['n'] = row['mid']['n']
        out.append(row)
    return out


def monotonic(table, field='mid'):
    """Spearman of bucket index against the mean, over buckets that have data.

    The desired shape is monotone. A single rank correlation is the whole diagnostic and it is
    reported with the number of buckets behind it, because a rank correlation over three points
    is not evidence of anything.
    """
    pts = [(i, r[field]['mean']) for i, r in enumerate(table)
           if r.get('n') and r[field]['mean'] is not None]
    if len(pts) < 3:
        return None, len(pts)
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    n = len(pts)
    rx = {v: i for i, v in enumerate(sorted(xs))}
    ry = {v: i for i, v in enumerate(sorted(ys))}
    d2 = sum((rx[a] - ry[b]) ** 2 for a, b in zip(xs, ys))
    return 1 - 6 * d2 / (n * (n * n - 1)), n


# ---- no-model baselines -------------------------------------------------------------------------
def baselines(rows, market, horizon_s):
    """Simple rules, no fitting. A model that cannot beat these has not earned its complexity."""
    sub = [r for r in rows if r['market'] == market]
    h = horizon_s
    out = []

    def add(name, picked, note=''):
        if len(picked) < 8:
            out.append({'name': name, 'n': len(picked), 'note': note or 'too few events'})
            return
        c = cell(picked, f'signed_taker_net_{h}s')
        g = cell(picked, f'signed_taker_gross_{h}s')
        m = cell(picked, f'signed_mid_ret_{h}s')
        out.append({'name': name, 'n': c['n'], 'clusters': c['clusters'],
                    'net_mean': c['mean'], 'net_lo': c['lo'], 'net_hi': c['hi'],
                    'gross_mean': g['mean'], 'mid_mean': m['mean'],
                    'hit': c['hit'], 'note': note})

    out.append({'name': 'B0 no trade', 'n': len(sub), 'clusters': None, 'net_mean': 0.0,
                'net_lo': 0.0, 'net_hi': 0.0, 'gross_mean': 0.0, 'mid_mean': 0.0,
                'hit': None, 'note': 'the benchmark every other row must beat'})

    add('B1 kalshi momentum',
        [r for r in sub if (r.get('kalshi_ret_5s_bps') or 0) * (r.get('impulse_sign') or 0) > 0],
        'trade the impulse direction only when Kalshi has already started moving that way')
    add('B2 external composite momentum', sub,
        'trade the impulse direction on every event')
    add('B3 external-Kalshi residual',
        [r for r in sub if (r.get('lag_5s_bps') or 0) * (r.get('impulse_sign') or 0) > 0],
        'trade only when Kalshi has NOT yet taken the external move')
    add('B4 CF-Kalshi residual',
        [r for r in sub if r.get('cf_basis_bps') is not None
         and r['cf_basis_bps'] * (r.get('impulse_sign') or 0) < 0],
        'trade toward the CF anchor')
    if market == 'KXETHPERP':
        add('B5 BTC shock -> ETH', [r for r in sub if abs(r.get('impulse_bps') or 0) >= 10],
            'ETH follows a large move; BTC cross-feature needs the paired BTC series')
    add('B6 shock + lag threshold',
        [r for r in sub if abs(r.get('impulse_bps') or 0) >= 10
         and (r.get('lag_5s_bps') or 0) * (r.get('impulse_sign') or 0) > 0],
        'both conditions: a large external move AND Kalshi behind it')
    return out


def fmt(x, n=2):
    return '' if x is None else f'{x:+.{n}f}'


def incorporation(rows, window):
    """Regress the Kalshi return on the external return over the same window.

    This is the premise of the whole lane, tested directly and before anything else. IMPULSE-1
    says the external venues incorporate information first and Kalshi follows. If that is true
    the slope is below 1 and the residual is large: Kalshi has taken only part of the move and
    the rest is still to come. A slope of 1 with a high R-squared says the opposite -- Kalshi is
    already there, and there is nothing left to follow.

    No model can recover a lag that is not in this regression.
    """
    pts = [(r.get(f'ext_ret_{window}_bps'), r.get(f'kalshi_ret_{window}_bps'),
            r.get(f'lag_{window}_bps'), r.get('entry_us')) for r in rows]
    pts = [p for p in pts if p[0] is not None and p[1] is not None]
    if len(pts) < 20:
        return None
    n = len(pts)
    mx = sum(p[0] for p in pts) / n
    my = sum(p[1] for p in pts) / n
    sxx = sum((p[0] - mx) ** 2 for p in pts)
    syy = sum((p[1] - my) ** 2 for p in pts)
    if sxx <= 0 or syy <= 0:
        return None
    sxy = sum((p[0] - mx) * (p[1] - my) for p in pts)
    beta = sxy / sxx
    r2 = (sxy * sxy) / (sxx * syy)
    lags = [p[2] for p in pts if p[2] is not None]
    keys = [CM.time_cluster(p[3]) for p in pts if p[2] is not None]
    lm, llo, lhi, lclu = CM.cluster_bootstrap(lags, keys, want_n=True)
    return {'window': window, 'n': n, 'beta': beta, 'r2': r2,
            'lag_mean': lm, 'lag_lo': llo, 'lag_hi': lhi, 'clusters': lclu,
            'lag_sd': statistics.pstdev(lags) if len(lags) > 1 else None,
            'mean_abs_ext': statistics.mean([abs(p[0]) for p in pts]),
            'mean_abs_lag': statistics.mean([abs(p[2]) for p in pts if p[2] is not None])
                            if lags else None}


def report(rows, out_md, out_csv, hours_span, days):
    L = []
    W = L.append
    W('# BTC/ETH impulse v1 — sign table and no-model baselines\n')
    W(f'{len(rows):,} events over {hours_span:.2f} hours of tape '
      f'({days} calendar day(s)). No model has been fitted.\n')

    W('## Power, before any table is read\n')
    W('| horizon | events after separation | independent clusters | markets |')
    W('|---:|---:|---:|---:|')
    sep_cache = {}
    for h in HORIZONS:
        sep = separate(rows, h)
        sep_cache[h] = sep
        clu = len({CM.time_cluster(r['entry_us']) for r in sep if r['entry_us']})
        mk = len({r['market'] for r in sep})
        W(f'| {h}s | {len(sep)} | {clu} | {mk} |')
    W('')
    worst = min(len(sep_cache[h]) for h in HORIZONS)
    if worst < 200:
        W(f'**This is far too little data to conclude anything.** The tables below are printed '
          f'so the pipeline is exercised end to end and the shape can be eyeballed, not because '
          f'{worst} events support an inference. Seven magnitude buckets over '
          f'{worst} events is roughly {worst // 7} events per cell.\n')
        W('What would be enough: the event rate is set by the cooldown, so at a 2-minute '
          'cooldown each market yields at most 30 events an hour and a 10-minute separation '
          'leaves 6. Reaching 50 events per bucket at the 10m horizon needs on the order of '
          f'{max(1, int(50 * 7 / max(6, 1) / 24))} days of continuous recording per market.\n')

    W('## The premise, tested first\n')
    W('IMPULSE-1 says the external venues take the information before Kalshi does, leaving a '
      'move still to come. That is a regression, not a model: `kalshi_return = beta x '
      'external_return` over the same window. A beta below 1 with real residual means Kalshi is '
      'behind. A beta of 1 with a high R-squared means it is already there.\n')
    W('| window | n | beta | R^2 | mean abs external | mean abs residual | residual / external |')
    W('|---|---:|---:|---:|---:|---:|---:|')
    inc = [x for x in (incorporation(rows, w) for w in ('1s', '5s', '30s', '1m')) if x]
    for x in inc:
        frac = ((x['mean_abs_lag'] / x['mean_abs_ext'])
                if x['mean_abs_ext'] and x['mean_abs_lag'] is not None else None)
        lagtxt = '' if x['mean_abs_lag'] is None else '{:.2f} bp'.format(x['mean_abs_lag'])
        fractxt = '' if frac is None else '{:.0%}'.format(frac)
        W('| {} | {} | **{:+.3f}** | {:.3f} | {:.2f} bp | {} | {} |'.format(
            x['window'], x['n'], x['beta'], x['r2'], x['mean_abs_ext'], lagtxt, fractxt))
    W('')
    long_w = [x for x in inc if x['window'] in ('30s', '1m')]
    if long_w and all(abs(x['beta'] - 1.0) < 0.15 and x['r2'] > 0.9 for x in long_w):
        b = long_w[-1]
        W(f"**Kalshi has already incorporated the move.** At {b['window']} the slope is "
          f"{b['beta']:+.3f} with an R-squared of {b['r2']:.3f}: Kalshi moves one-for-one with "
          f"the external composite and almost nothing is left unexplained. The residual averages "
          f"{b['mean_abs_lag']:.2f} bp against an external move averaging "
          f"{b['mean_abs_ext']:.2f} bp.\n")
        W('That is the hypothesis failing at its premise rather than at its model. There is no '
          'lag for a 2m, 5m or 10m trade to close, and no feature set recovers one that the '
          'regression says is not there.\n')
        short = [x for x in inc if x['window'] == '1s']
        if short:
            sx = short[0]
            W(f"The one place a residual survives is at 1 second: beta {sx['beta']:+.3f}, "
              f"R-squared {sx['r2']:.3f}, mean absolute residual {sx['mean_abs_lag']:.2f} bp. "
              f"Against a taker round trip of {2*TAKER_BPS:.0f} bps, a perfect predictor of that "
              f"residual still loses {2*TAKER_BPS - sx['mean_abs_lag']:.0f} bps. It is also the "
              f"window where a clock offset of tens of milliseconds is a material fraction of "
              f"the measurement, so part of it is not a real lead at all.\n")

    rows_csv = []
    for market in sorted({r['market'] for r in rows}):
        W(f'\n## {market}\n')
        for h in HORIZONS:
            sep = sep_cache[h]
            tbl = sign_table(sep, market, h)
            rho, nb = monotonic(tbl, 'mid')
            rho_n, _ = monotonic(tbl, 'net')
            W(f'\n### {h // 60}m horizon'
              + (f' — monotonicity (mid) rho={rho:+.2f} over {nb} buckets'
                 if rho is not None else ' — too few populated buckets to rank')
              + '\n')
            W('| external impulse | mean impulse | mean Kalshi lag | events | clusters | '
              'mid move | net taker | 90% interval | hit rate |')
            W('|---|---:|---:|---:|---:|---:|---:|---|---:|')
            for r in tbl:
                if not r.get('n'):
                    W(f"| {r['bucket']} bp | | | 0 | | | | | |")
                    continue
                mid, net = r['mid'], r['net']
                band = ('n/a' if net['lo'] is None
                        else f"[{net['lo']:+.1f}, {net['hi']:+.1f}]")
                W(f"| {r['bucket']} bp | {r['mean_impulse']:.1f} | {fmt(r['mean_lag'],1)} | "
                  f"{r['n']} | {net['clusters']} | {fmt(mid['mean'],1)} | "
                  f"**{fmt(net['mean'],1)}** | {band} | "
                  f"{'' if net['hit'] is None else f'{net[chr(104)+chr(105)+chr(116)]:.0%}'} |")
                rows_csv.append({'market': market, 'horizon_s': h, 'bucket': r['bucket'],
                                 'events': r['n'], 'clusters': net['clusters'],
                                 'mean_impulse_bps': r['mean_impulse'],
                                 'mean_lag_bps': r['mean_lag'],
                                 'mid_move_bps': mid['mean'], 'gross_bps': r['gross']['mean'],
                                 'net_bps': net['mean'], 'net_lo': net['lo'],
                                 'net_hi': net['hi'], 'hit_rate': net['hit']})
        W(f'\n### {market} — baselines at the 10m horizon\n')
        W('| baseline | events | clusters | mid move | gross taker | net taker | 90% interval | '
          'hit | note |')
        W('|---|---:|---:|---:|---:|---:|---|---:|---|')
        for b in baselines(sep_cache[600], market, 600):
            if 'net_mean' not in b:
                W(f"| {b['name']} | {b['n']} | | | | | | | {b['note']} |")
                continue
            band = ('n/a' if b['net_lo'] is None
                    else f"[{b['net_lo']:+.1f}, {b['net_hi']:+.1f}]")
            W(f"| {b['name']} | {b['n']} | {b.get('clusters','')} | {fmt(b['mid_mean'],1)} | "
              f"{fmt(b['gross_mean'],1)} | **{fmt(b['net_mean'],1)}** | {band} | "
              f"{'' if b['hit'] is None else f'{b[chr(104)+chr(105)+chr(116)]:.0%}'} | "
              f"{b['note']} |")
        W('')

    W('\n## How to read this\n')
    W('- `mid move` is research only. `net taker` is the economics: enter at the ask, exit at '
      f'the bid, two taker fees at {TAKER_BPS:.0f} bps each.')
    W('- `clusters` is the sample size for the interval. The event count is not.')
    W('- Monotonicity is the thing to look for. A column that rises with the impulse bucket is '
      'worth modelling; one that wanders is not, and no model recovers a relationship that is '
      'not in the sign table.\n')

    os.makedirs(os.path.dirname(out_md), exist_ok=True)
    with open(out_md, 'w') as f:
        f.write('\n'.join(L) + '\n')
    if rows_csv:
        with open(out_csv, 'w', newline='') as f:
            w = csv.DictWriter(f, fieldnames=list(rows_csv[0]))
            w.writeheader()
            for r in rows_csv:
                w.writerow(r)
    return sep_cache


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--events', default=os.path.join(ROOT, 'reports', 'impulse_events.csv'))
    ap.add_argument('--md', default=os.path.join(ROOT, 'reports', 'impulse_sign_table.md'))
    ap.add_argument('--csv', default=os.path.join(ROOT, 'reports', 'impulse_scoreboard.csv'))
    args = ap.parse_args()
    if not os.path.exists(args.events):
        sys.exit(f'no events at {args.events}: run impulse_events.py first')
    rows = load(args.events)
    if not rows:
        sys.exit('no events')
    ts = [r['entry_us'] for r in rows if r['entry_us']]
    span = (max(ts) - min(ts)) / 3.6e9 if len(ts) > 1 else 0.0
    import datetime as dt
    days = len({dt.datetime.fromtimestamp(t / 1e6, dt.timezone.utc).date() for t in ts})
    sep = report(rows, args.md, args.csv, span, days)

    print(f'{len(rows):,} events over {span:.2f} h, {days} calendar day(s)')
    for h in HORIZONS:
        clu = len({CM.time_cluster(r['entry_us']) for r in sep[h] if r['entry_us']})
        print(f'  {h}s horizon: {len(sep[h])} separated events, {clu} clusters')
    print()
    print('PREMISE: kalshi_return = beta x external_return')
    for w in ('1s', '5s', '30s', '1m'):
        x = incorporation(rows, w)
        if x:
            print(f"  {w:>3s}: beta {x['beta']:+.3f}  R2 {x['r2']:.3f}  "
                  f"mean|residual| {x['mean_abs_lag']:.2f} bp of {x['mean_abs_ext']:.2f} bp "
                  f"external  (n={x['n']})")
    print()
    for market in sorted({r['market'] for r in rows}):
        tbl = sign_table(sep[600], market, 600)
        rho, nb = monotonic(tbl, 'mid')
        print(f'  {market}: 10m monotonicity rho='
              f'{"n/a" if rho is None else f"{rho:+.2f}"} over {nb} populated buckets')
    print(f'wrote {os.path.relpath(args.md, ROOT)}')


if __name__ == '__main__':
    main()
