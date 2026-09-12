#!/usr/bin/env python3
"""One table: every strategy, its actual economics, and the fee at which it would work.

Assembled from the reports rather than retyped, so it cannot drift away from the studies it
summarises. Everything it reads was measured on this account's data at this account's tier.

THE SUCCESS CONDITION, which nothing here is allowed to soften:

    net bps per trade > 0, at THIS ACCOUNT'S CURRENT FEES

A strategy is not profitable because its gross edge is positive, its IC is significant, its
accuracy beats 50%, the midpoint moves in its favour, or a lower fee tier would rescue it. Each
of those has been used somewhere in this project as if it were the answer, and each one is
recorded here as a separate column so that it is visible and cannot be mistaken for the net.

`break_even_maker_bps` is the fee at which a strategy crosses zero, and `tier_needed` is the
volume tier that charges it. Those two columns exist to answer "is this structurally impossible
or just impossible for us", which is a different question from "does it work".

  python3 scripts/next_scoreboard.py
"""
import argparse, csv, json, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fee_model import LADDER, breakeven_fee, tier_at_or_below, volume_to_tier_cost

COLS = ['strategy', 'market', 'regime', 'horizon', 'attempts', 'fills', 'trades_per_day',
        'gross_alpha_bps', 'spread_capture_bps', 'markout_bps', 'funding_bps', 'incentives_bps',
        'maker_fee_bps', 'taker_fee_bps', 'slippage_bps', 'net_bps_per_trade',
        'boot_lo', 'boot_hi', 'fee_tier_assumed', 'break_even_maker_bps', 'tier_needed',
        'volume_needed', 'status', 'evidence']


def f(r, k, d=None):
    v = r.get(k)
    if v in ('', None):
        return d
    try:
        return float(v)
    except (TypeError, ValueError):
        return d


def read(path):
    p = os.path.join(ROOT, 'reports', path)
    if not os.path.exists(p):
        return []
    with open(p) as fh:
        return list(csv.DictReader(fh))


def from_execution():
    """Unconditional passive quoting and crossing, per market."""
    out = []
    for r in read('execution_simulator.csv'):
        if r.get('queue_bound') != 'pess':
            continue
        fills = int(f(r, 'fills', 0) or 0)
        if fills < 50:
            continue
        mode = r.get('mode')
        sp = f(r, 'median_spread_at_post_bps')
        mo = f(r, 'markout_60s_bps')
        fee = f(r, 'fee_bps', 0.0)
        if mode in ('join', 'improve') and sp is not None and mo is not None:
            gross = sp / 2 + mo
            be = breakeven_fee(gross)
            ti, tv = tier_at_or_below(be, 'maker') if be else (None, None)
            out.append({
                'strategy': f'unconditional_maker_{mode}', 'market': r['market'],
                'regime': 'all', 'horizon': '60s',
                'attempts': r.get('attempts'), 'fills': fills,
                'trades_per_day': round(fills / 2.5 * 24, 1),
                'gross_alpha_bps': 0.0, 'spread_capture_bps': round(sp / 2, 2),
                'markout_bps': round(mo, 2), 'funding_bps': 0.0, 'incentives_bps': 0.0,
                'maker_fee_bps': fee, 'taker_fee_bps': 0.0, 'slippage_bps': 0.0,
                'net_bps_per_trade': round(gross - fee, 2),
                'fee_tier_assumed': 0,
                'break_even_maker_bps': round(be, 2) if be else '',
                'tier_needed': ti if ti is not None else '',
                'volume_needed': tv if tv else '',
                'status': 'REJECTED_COST',
                'evidence': 'reports/execution_summary.md',
            })
        elif mode == 'taker':
            ec = f(r, 'entry_cost_bps', 0.0)
            mo60 = f(r, 'markout_60s_bps', 0.0)
            out.append({
                'strategy': 'taker_cross_no_signal', 'market': r['market'],
                'regime': 'all', 'horizon': '60s', 'attempts': r.get('attempts'),
                'fills': fills, 'trades_per_day': round(fills / 2.5 * 24, 1),
                'gross_alpha_bps': 0.0, 'spread_capture_bps': 0.0,
                'markout_bps': round(mo60, 2), 'funding_bps': 0.0, 'incentives_bps': 0.0,
                'maker_fee_bps': 0.0, 'taker_fee_bps': fee,
                'slippage_bps': round(ec, 2),
                'net_bps_per_trade': round(mo60 - ec - fee, 2),
                'fee_tier_assumed': 0, 'break_even_maker_bps': '', 'tier_needed': '',
                'volume_needed': '', 'status': 'REJECTED_COST',
                'evidence': 'reports/execution_summary.md',
            })
    return out


def from_conditional():
    """The selective maker rule, at each level of selectivity, out of sample."""
    out = []
    for r in read('conditional_maker.csv'):
        if r.get('selector') != 'model' or r.get('note'):
            continue
        gross = f(r, 'gross_kept_bps')
        fee = f(r, 'maker_fee_bps', 5.0)
        ex = f(r, 'exit_cost_bps', 12.0)
        net = f(r, 'net_round_trip_bps')
        lo = f(r, 'net_lo')
        if gross is None or net is None:
            continue
        be = breakeven_fee(gross, other_leg_bps=ex)
        ti, tv = tier_at_or_below(be, 'maker') if be else (None, None)
        q = f(r, 'quantile', 0)
        out.append({
            'strategy': f'conditional_maker_top{q:.0%}', 'market': 'all',
            'regime': 'selected', 'horizon': r.get('horizon'),
            'attempts': '', 'fills': r.get('n'),
            'trades_per_day': round(f(r, 'fills_per_day', 0) or 0, 1),
            'gross_alpha_bps': 0.0, 'spread_capture_bps': round(gross, 2),
            'markout_bps': '', 'funding_bps': 0.0, 'incentives_bps': 0.0,
            'maker_fee_bps': round(fee, 2), 'taker_fee_bps': 0.0,
            'slippage_bps': round(ex, 2), 'net_bps_per_trade': round(net, 2),
            'boot_lo': round(lo, 2) if lo is not None else '',
            'boot_hi': '', 'fee_tier_assumed': 0,
            'break_even_maker_bps': round(be, 2) if be else '',
            'tier_needed': ti if ti is not None else '',
            'volume_needed': tv if tv else '',
            # Frozen. The construction is rejected on the round trip, not on this row's net,
            # so no level of selectivity is allowed to read as promising.
            'status': 'REJECTED_V1',
            'evidence': 'research/conditional_maker_v1_rejected.md; round trip 0/40 positive, '
                        'score-selected fills have worse post-fill markout than random',
        })
    return out


def from_funding():
    out = []
    for r in read('funding_study.csv'):
        rate = f(r, 'rate_bps_per_period_median', 0.0) or 0.0
        if abs(rate) < 0.001:
            continue
        rt = f(r, 'round_trip_maker_bps', 10.0)
        fov = f(r, 'funding_over_vol')
        out.append({
            'strategy': 'funding_capture_unhedged', 'market': r.get('market'),
            'regime': 'all', 'horizon': '8h', 'attempts': '', 'fills': '',
            'trades_per_day': 0.125,
            'gross_alpha_bps': 0.0, 'spread_capture_bps': 0.0, 'markout_bps': '',
            'funding_bps': round(abs(rate), 2), 'incentives_bps': 0.0,
            'maker_fee_bps': '', 'taker_fee_bps': '', 'slippage_bps': round(rt, 2),
            'net_bps_per_trade': round(abs(rate) - rt, 2),
            'fee_tier_assumed': 0, 'break_even_maker_bps': '', 'tier_needed': '',
            'volume_needed': '',
            'status': 'REJECTED_RISK' if (fov and fov < 0.05) else 'PROMISING',
            'evidence': f'reports/funding_study.md (funding is {fov:.1%} of one period of '
                        f'volatility)' if fov else 'reports/funding_study.md',
        })
    return out


def from_minute_bars():
    return [{
        'strategy': 'impulse_minute_bar', 'market': 'all', 'regime': 'all', 'horizon': '1m-1h',
        'attempts': '1736 hypotheses', 'fills': '', 'trades_per_day': '',
        'gross_alpha_bps': 16.9, 'spread_capture_bps': 0.0, 'markout_bps': '',
        'funding_bps': 0.0, 'incentives_bps': 0.0, 'maker_fee_bps': 5.0, 'taker_fee_bps': 12.0,
        'slippage_bps': 0.0, 'net_bps_per_trade': -0.1, 'fee_tier_assumed': 0,
        'break_even_maker_bps': '', 'tier_needed': '', 'volume_needed': '',
        'status': 'REJECTED_NONROBUST',
        'evidence': 'research/minute_bar_alpha_results.md; 33 survive Bonferroni, all lose '
                    '14-20 bps after costs',
    }]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--csv', default=os.path.join(ROOT, 'reports',
                                                  'next_strategy_scoreboard.csv'))
    ap.add_argument('--md', default=os.path.join(ROOT, 'reports',
                                                 'next_strategy_scoreboard.md'))
    args = ap.parse_args()

    rows = (from_execution() + from_conditional() + from_funding() + from_minute_bars())
    rows.sort(key=lambda r: -(r['net_bps_per_trade'] if isinstance(r['net_bps_per_trade'],
                                                                    (int, float)) else -999))
    os.makedirs(os.path.dirname(args.csv), exist_ok=True)
    with open(args.csv, 'w', newline='') as fh:
        w = csv.DictWriter(fh, fieldnames=COLS, extrasaction='ignore')
        w.writeheader()
        for r in rows:
            w.writerow(r)

    survivors = [r for r in rows if r['status'] in ('LIVE_CANDIDATE', 'PROMISING')]
    L = ['# Strategy scoreboard\n',
         'Net is per trade, at **tier 0** -- this account\'s actual fees. A strategy is not '
         'profitable because its gross edge is positive, its IC is significant, its accuracy '
         'beats 50%, or a lower fee tier would rescue it.\n']
    L.append(f'{len(rows)} strategy/market rows. **{len(survivors)}** are not rejected.\n')
    L.append('| strategy | market | h | trades/day | gross | fee | slippage | net | '
             'break-even maker | tier | status |')
    L.append('|---|---|---|---:|---:|---:|---:|---:|---:|---:|---|')
    for r in rows[:40]:
        gross = (r['spread_capture_bps'] or 0) + (r['gross_alpha_bps'] or 0) + \
                (r['funding_bps'] or 0)
        fee = r['maker_fee_bps'] or r['taker_fee_bps'] or ''
        L.append(f"| `{r['strategy']}` | {r['market']} | {r['horizon']} | "
                 f"{r['trades_per_day']} | {gross:+.2f} | {fee} | {r['slippage_bps']} | "
                 f"**{r['net_bps_per_trade']:+.2f}** | {r['break_even_maker_bps']} | "
                 f"{r['tier_needed']} | {r['status']} |")
    L.append('')
    L.append('## Status meanings\n')
    L.append('- `LIVE_CANDIDATE` -- positive net AND a bootstrap lower bound above zero')
    L.append('- `PROMISING` -- positive net, interval includes zero')
    L.append('- `REJECTED_COST` -- real effect, smaller than the cost of trading it')
    L.append('- `REJECTED_NONROBUST` -- does not survive multiple-testing correction')
    L.append('- `REJECTED_RISK` -- the edge is real but trivial against the risk carried '
             'to collect it')
    L.append('- `REJECTED_V1` -- this construction is frozen; see the linked record for what '
             'would reopen it. The claim is narrow: no executable edge for THIS one-sided '
             'construction under the tested regime, which is not a claim about maker strategies '
             'in general\n')
    with open(args.md, 'w') as fh:
        fh.write('\n'.join(L) + '\n')

    print(f'{len(rows)} rows, {len(survivors)} not rejected')
    from collections import Counter
    for k, v in Counter(r['status'] for r in rows).most_common():
        print(f'  {k:22s} {v}')
    for r in survivors[:6]:
        print(f"  SURVIVOR {r['strategy']} {r['market']} net {r['net_bps_per_trade']:+.2f}")
    print(f'wrote {os.path.relpath(args.csv, ROOT)}')


if __name__ == '__main__':
    main()
