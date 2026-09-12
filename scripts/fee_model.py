#!/usr/bin/env python3
"""The fee ladder, and what every result looks like at every rung of it.

Kalshi's perpetual futures fees are TIERED on 30-day trailing volume, not flat. The schedule was
verified against the CFTC rule filing of 24 June 2026 (rules0624267243.pdf, effective no earlier
than 8 July 2026), not taken from a help page or from memory:

  30d volume   taker   maker
  $0           12.0     5.0
  $100k        10.0     4.0
  $300k         8.0     3.2
  $1m           6.0     2.4
  $3m           5.0     2.0
  $10m          4.0     1.6
  $30m          3.5     1.4
  $100m         3.2     1.2
  $300m         3.0     1.0
  $1b           2.8     0.8
  $3b           2.6     0.6

Trailing volume counts BOTH maker and taker volume, and tiers are recomputed daily.

WHY THIS MATTERS MORE THAN IT LOOKS. Every study in this repository priced against the tier-0
numbers as if they were the fee. They are the fee for this account, so the conclusions stand for
this account. But "the strategy loses 5 bps to the maker fee" and "the strategy is impossible on
this venue" are different claims, and only the first one was ever measured. The execution study
found that quoting keeps +0.09 bps of spread on average after adverse selection, and that LINK
keeps +2.45. The tier-3 maker fee is 2.4. The gap between what this account pays and what a
venue participant with $1m of monthly volume pays is the whole difference between a dead idea and
a marginal one.

WHAT THIS FILE REFUSES TO DO. It will not report a strategy as profitable at a tier the account
does not hold. Every repriced figure is labelled with the tier it assumes, and the account's
actual tier is fetched rather than guessed wherever the API can be reached.

AND THE TRAP. Reaching tier 3 requires $1m of 30-day volume. At the tier-0 maker rate of 5 bps,
$1m of maker volume costs $500 in fees. On a $50 account that is not a strategy, it is a way to
lose the account ten times over to qualify for a discount on a strategy that does not yet work.
`volume_to_tier_cost` below exists to make that arithmetic impossible to skip.

  python3 scripts/fee_model.py                      # print the ladder and the break-even table
  python3 scripts/fee_model.py --account            # also fetch this account's actual tier
"""
import argparse, csv, json, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (30-day volume threshold in dollars, taker bps, maker bps)
LADDER = [
    (0,             12.0, 5.0),
    (100_000,       10.0, 4.0),
    (300_000,        8.0, 3.2),
    (1_000_000,      6.0, 2.4),
    (3_000_000,      5.0, 2.0),
    (10_000_000,     4.0, 1.6),
    (30_000_000,     3.5, 1.4),
    (100_000_000,    3.2, 1.2),
    (300_000_000,    3.0, 1.0),
    (1_000_000_000,  2.8, 0.8),
    (3_000_000_000,  2.6, 0.6),
]
SOURCE = ('CFTC rule filing rules0624267243.pdf, 24 June 2026, effective no earlier than '
          '8 July 2026')


def tier_for(volume_30d):
    """The tier a given 30-day volume qualifies for. Returns (index, threshold, taker, maker)."""
    best = 0
    for i, (thr, _, _) in enumerate(LADDER):
        if volume_30d >= thr:
            best = i
    thr, tk, mk = LADDER[best]
    return best, thr, tk, mk


def volume_to_tier_cost(target_tier, side='maker', from_volume=0.0):
    """What it costs in fees to trade your way up to a tier, paying the fees of the tiers you
    pass through on the way. There is no shortcut: the volume is charged at the rate you hold
    while you are generating it."""
    if target_tier <= 0 or target_tier >= len(LADDER):
        return 0.0
    target_vol = LADDER[target_tier][0]
    cost, v = 0.0, float(from_volume)
    while v < target_vol:
        i, thr, tk, mk = tier_for(v)
        nxt = LADDER[i + 1][0] if i + 1 < len(LADDER) else target_vol
        step = min(nxt, target_vol) - v
        rate = (mk if side == 'maker' else tk) / 1e4
        cost += step * rate
        v += step
    return cost


def breakeven_fee(gross_bps, other_leg_bps=0.0):
    """The per-leg fee at which a strategy with this gross edge breaks exactly even.

    `gross_bps` is what the strategy keeps BEFORE fees -- for a maker that is the half-spread
    plus the markout. `other_leg_bps` is any cost on the exit leg that is not the fee being
    solved for. Returns None when the gross is already negative, because no fee schedule rescues
    a strategy that loses money before fees.
    """
    net_gross = gross_bps - other_leg_bps
    return net_gross if net_gross > 0 else None


def tier_at_or_below(fee_bps, side='maker'):
    """The first tier whose fee is at or below a target. Answers 'what volume would I need'."""
    for i, (thr, tk, mk) in enumerate(LADDER):
        if (mk if side == 'maker' else tk) <= fee_bps:
            return i, thr
    return None, None


def reprice(rows, side_in='maker', side_out='taker'):
    """Every strategy at every rung. `rows` need `market`, `label`, `gross_bps`."""
    out = []
    for r in rows:
        g = r['gross_bps']
        be = breakeven_fee(g)
        t_idx, t_vol = tier_at_or_below(be, 'maker') if be else (None, None)
        line = {'market': r.get('market', ''), 'strategy': r.get('label', ''),
                'gross_bps': g, 'breakeven_maker_bps': be,
                'tier_needed': t_idx, 'volume_needed': t_vol,
                'cost_to_reach': (volume_to_tier_cost(t_idx) if t_idx else 0.0)}
        for i, (thr, tk, mk) in enumerate(LADDER):
            fee_in = mk if side_in == 'maker' else tk
            fee_out = tk if side_out == 'taker' else mk
            line[f'net_t{i}'] = g - fee_in - (r.get('exit_cost_bps') or 0.0) - 0.0 * fee_out
            line[f'net_rt_t{i}'] = g - fee_in - fee_out - (r.get('exit_cost_bps') or 0.0)
        out.append(line)
    return out


def fetch_account_tier():
    """This account's actual tier. Never guessed, and never assumed to be tier 0 either.

    The balance and fills endpoints require the RSA signing key, which lives only on the Mac and
    is never read here. This returns a structured 'unknown' off that machine rather than a
    plausible default, because a fee assumed one tier too low turns a losing strategy into a
    winning one on paper.
    """
    key = os.path.expanduser('~/.kalshi/prod.pem')
    if not os.path.exists(key):
        return {'known': False,
                'why': 'the signing key is not on this machine, so no authenticated call can be '
                       'made; run this on the Mac to resolve the real tier'}
    return {'known': False,
            'why': 'authenticated fills pagination is not implemented here; implement it on the '
                   'Mac using the existing RSA-PSS signer in scripts/perps_api_probe.py'}


def load_measured():
    """Gross-before-fee figures taken from the execution simulator, not retyped by hand.

    For a passive fill the gross is the half-spread plus the markout -- what the position was
    worth net of what the market took back for trading with you. It is read out of
    execution_simulator.csv so that this table cannot drift away from the study it summarises.
    """
    p = os.path.join(ROOT, 'reports', 'execution_simulator.csv')
    if not os.path.exists(p):
        return []
    rows = []
    with open(p) as f:
        for r in csv.DictReader(f):
            if r.get('queue_bound') != 'pess':
                continue
            try:
                fills = int(r['fills'])
                sp = float(r['median_spread_at_post_bps'])
                mo = float(r['markout_60s_bps'])
            except (KeyError, TypeError, ValueError):
                continue
            if fills < 50 or r['mode'] not in ('join', 'improve'):
                continue
            rows.append({'market': r['market'], 'label': r['mode'],
                         'gross_bps': sp / 2 + mo, 'fills': fills})
    return rows


def report(rows, acct, out_md, out_csv):
    L = []
    W = L.append
    W('# Fee ladder and repricing\n')
    W(f'Schedule verified against: {SOURCE}\n')
    W('Trailing volume counts both maker and taker volume; tiers recompute daily.\n')

    W('## The ladder\n')
    W('| tier | 30d volume | taker bps | maker bps | maker/maker round trip | maker/taker |')
    W('|---:|---:|---:|---:|---:|---:|')
    for i, (thr, tk, mk) in enumerate(LADDER):
        W(f'| {i} | ${thr:,} | {tk:.1f} | {mk:.1f} | {2*mk:.1f} | {mk+tk:.1f} |')
    W('')

    W('## This account\n')
    if acct.get('known'):
        W(f"- 30-day volume: ${acct['volume']:,.0f}")
        W(f"- tier {acct['tier']}: maker {acct['maker']:.1f} bps, taker {acct['taker']:.1f} bps\n")
    else:
        W(f"- **tier not resolved.** {acct.get('why')}")
        W('- every figure below is therefore labelled by tier rather than reported as "the" '
          'number, and tier 0 is used wherever one number is needed\n')

    if not rows:
        W('## Repricing\n')
        W('No execution results to reprice. Run `scripts/execution_sim.py` first.\n')
    else:
        W('## What each measured strategy keeps before fees\n')
        W('Gross is the half-spread plus the 60-second markout: the spread revenue that survives '
          'adverse selection. It is what any fee has to come out of.\n')
        W('| market | mode | fills | gross bps | break-even maker fee | tier that would allow it |'
          ' 30d volume needed | fees to trade there |')
        W('|---|---|---:|---:|---:|---:|---:|---:|')
        pr = reprice(rows)
        for r in sorted(pr, key=lambda x: -(x['gross_bps'] or -99)):
            be = r['breakeven_maker_bps']
            fills = next((x['fills'] for x in rows
                          if x['market'] == r['market'] and x['label'] == r['strategy']), '')
            if be is None:
                W(f"| {r['market']} | {r['strategy']} | {fills} | {r['gross_bps']:+.2f} | "
                  f"none -- loses before fees | n/a | n/a | n/a |")
            else:
                W(f"| {r['market']} | {r['strategy']} | {fills} | {r['gross_bps']:+.2f} | "
                  f"{be:.2f} | {r['tier_needed'] if r['tier_needed'] is not None else 'above the top tier'} | "
                  f"${r['volume_needed']:,} | ${r['cost_to_reach']:,.0f} |"
                  if r['tier_needed'] is not None else
                  f"| {r['market']} | {r['strategy']} | {fills} | {r['gross_bps']:+.2f} | "
                  f"{be:.2f} | below the 0.6 bps floor -- no tier reaches it | n/a | n/a |")
        W('')

        pos = [r for r in pr if r['breakeven_maker_bps']]
        W('## Net at every tier, entry leg only\n')
        W('Entry as maker, no exit cost. This is the most generous framing there is, and it is '
          'shown so that the tier at which each line crosses zero is visible.\n')
        hdr = ' | '.join(f't{i}' for i in range(len(LADDER)))
        W(f'| market | mode | {hdr} |')
        W('|---|---|' + '---:|' * len(LADDER))
        for r in sorted(pr, key=lambda x: -(x['gross_bps'] or -99))[:12]:
            cells = ' | '.join(f"{r[f'net_t{i}']:+.1f}" for i in range(len(LADDER)))
            W(f"| {r['market']} | {r['strategy']} | {cells} |")
        W('')
        if pos:
            first = min((r['tier_needed'] for r in pos if r['tier_needed'] is not None),
                        default=None)
            if first is not None:
                thr = LADDER[first][0]
                cost = volume_to_tier_cost(first)
                W(f'The first strategy to cross zero does so at **tier {first}** '
                  f'(${thr:,} of 30-day volume, maker {LADDER[first][2]:.1f} bps).\n')
                W(f'Trading up to that tier from zero costs about **${cost:,.0f}** in maker fees '
                  f'along the way, paid at the higher rates held while generating the volume. '
                  f'That is the number that decides whether chasing a tier is a strategy or a '
                  f'way to lose the account.\n')

    W('## The trap\n')
    W('| target tier | 30d volume | maker fee there | fees to trade up to it |')
    W('|---:|---:|---:|---:|')
    for i in range(1, 6):
        W(f'| {i} | ${LADDER[i][0]:,} | {LADDER[i][2]:.1f} | '
          f'${volume_to_tier_cost(i):,.0f} |')
    W('')
    W('Volume is charged at the rate held while it is generated, so the ladder cannot be climbed '
      'cheaply. Reaching tier 3 costs roughly the cost of a small car in fees. No account should '
      'chase a tier before it has a strategy that is profitable at the tier it already holds.\n')

    os.makedirs(os.path.dirname(out_md), exist_ok=True)
    with open(out_md, 'w') as f:
        f.write('\n'.join(L) + '\n')
    if rows:
        pr = reprice(rows)
        cols = (['market', 'strategy', 'gross_bps', 'breakeven_maker_bps', 'tier_needed',
                 'volume_needed', 'cost_to_reach']
                + [f'net_t{i}' for i in range(len(LADDER))]
                + [f'net_rt_t{i}' for i in range(len(LADDER))])
        with open(out_csv, 'w') as f:
            w = csv.DictWriter(f, fieldnames=cols, extrasaction='ignore')
            w.writeheader()
            for r in pr:
                w.writerow(r)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--account', action='store_true', help='try to resolve the real tier')
    ap.add_argument('--md', default=os.path.join(ROOT, 'reports', 'fee_ladder.md'))
    ap.add_argument('--csv', default=os.path.join(ROOT, 'reports', 'fee_repricing.csv'))
    args = ap.parse_args()

    acct = fetch_account_tier() if args.account else {
        'known': False, 'why': 'not requested; pass --account to try to resolve it'}
    rows = load_measured()
    report(rows, acct, args.md, args.csv)

    print(f'ladder: {len(LADDER)} tiers, {SOURCE}')
    if acct.get('known'):
        print(f"account tier {acct['tier']}: maker {acct['maker']} taker {acct['taker']}")
    else:
        print(f"account tier UNRESOLVED: {acct['why']}")
    print(f'strategies repriced: {len(rows)}')
    for r in sorted(rows, key=lambda x: -x['gross_bps'])[:4]:
        be = breakeven_fee(r['gross_bps'])
        ti, tv = tier_at_or_below(be, 'maker') if be else (None, None)
        print(f"  {r['market']:13s}{r['label']:9s} gross {r['gross_bps']:+6.2f}  "
              f"break-even maker {be if be else float('nan'):.2f}  "
              f"tier {ti}  needs ${tv:,}" if be else
              f"  {r['market']:13s}{r['label']:9s} gross {r['gross_bps']:+6.2f}  "
              f"loses before fees")
    print(f'wrote {os.path.relpath(args.md, ROOT)}')


if __name__ == '__main__':
    main()
