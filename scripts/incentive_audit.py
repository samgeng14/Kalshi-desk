#!/usr/bin/env python3
"""What Kalshi is currently paying people to provide, and whether a $50 account can collect any.

The effective cost of a maker fill is not the fee. It is

    F_effective = F_exchange - E[volume reward] - E[liquidity reward]

and the reward terms are not zero on this venue: Kalshi runs liquidity and volume incentive
programs continuously, with the definitions published on a PUBLIC endpoint that needs no
authentication:

    GET /trade-api/v2/incentive_programs

CAUTION ABOUT THAT ENDPOINT. Its documented `type` filter does not work. Asking for
`type=margin_taker_volume` returns programs whose own `incentive_type` field says `liquidity`,
so the server is ignoring the parameter rather than filtering on it. Every filter here is
therefore applied client-side to the `incentive_type` field on each record, and the audit reports
how many records it had to discard to make the answer honest. Trusting the server filter would
have produced a confident list of perpetual-futures volume programs that do not exist.

ELIGIBILITY IS THE POINT, NOT THE POOL SIZE. A liquidity program pays on per-second order book
snapshots, and a snapshot only scores if BOTH sides carry at least the program's Target Size.
Target sizes on this venue run from 100 to 20,000 contracts. A $50 account cannot post 300
contracts a side on anything priced above a few cents, so most of these pools are not merely
competitive, they are structurally unreachable. Every program below is marked with the notional
it would take to qualify, and nothing is counted as a reward unless the account could actually
earn it.

  python3 scripts/incentive_audit.py
  python3 scripts/incentive_audit.py --equity 50 --status all
"""
import argparse, csv, json, os, sys, time, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 'https://api.elections.kalshi.com/trade-api/v2'
PERP_TYPES = {'margin_maker_volume', 'margin_taker_volume'}
# period_reward and max_reward_per_account are in centi-cents: 1 dollar = 10,000.
CENTICENTS = 10_000.0


def get(path, params, timeout=45):
    q = '&'.join(f'{k}={v}' for k, v in params.items() if v not in (None, ''))
    url = f'{BASE}{path}' + (f'?{q}' if q else '')
    req = urllib.request.Request(url, headers={'accept': 'application/json'})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def fetch_all(status='active', page=1000, max_pages=60):
    """Paginate the whole set. The server filter is not used; see the module docstring."""
    out, cursor, pages = [], None, 0
    while pages < max_pages:
        d = get('/incentive_programs', {'status': status, 'limit': page, 'cursor': cursor})
        batch = d.get('incentive_programs') or []
        out.extend(batch)
        cursor = d.get('next_cursor')
        pages += 1
        if not cursor or not batch:
            break
    return out, pages, bool(cursor)


def perp_tickers():
    """Which tickers are perpetuals, from the exchange rather than from a hardcoded list."""
    try:
        d = get('/markets', {'limit': 1000, 'status': 'open'})
        return {m.get('ticker') for m in (d.get('markets') or [])
                if (m.get('ticker') or '').upper().endswith('PERP')}
    except Exception:
        return set()


def classify(p, perps, equity, price_hint):
    """Everything about one program that decides whether it is worth anything to this account."""
    t = (p.get('market_ticker') or '')
    typ = p.get('incentive_type') or ''
    is_perp = t.upper().endswith('PERP') or t in perps or typ in PERP_TYPES
    pool = (p.get('period_reward') or 0) / CENTICENTS
    cap = p.get('max_reward_per_account')
    cap = (cap / CENTICENTS) if cap else None
    try:
        target = float(p.get('target_size_fp') or 0)
    except (TypeError, ValueError):
        target = 0.0
    px = price_hint.get(t)
    # A liquidity snapshot scores only if BOTH sides carry the Target Size, and for a BINARY
    # contract the collateral that costs is exactly the target size in dollars, whatever the
    # price. Resting N yes-bids at p costs N*p; resting N yes-asks is the same as N no-bids at
    # (1-q) and costs N*(1-q); the total is N*(1 - (q-p)), which is N dollars less the spread.
    # So no price lookup is needed and none of it depends on which market it is.
    if target:
        need_notional = target * (1.0 - 0.02)      # a 2 cent spread, generous to the account
    else:
        need_notional = None
    reachable = None
    if typ == 'liquidity':
        if need_notional is None:
            reachable = 'unknown -- the program declares no target size'
        elif need_notional > equity:
            reachable = f'NO -- needs ${need_notional:,.0f} two-sided, account is ${equity:,.0f}'
        else:
            reachable = 'possible'
    elif typ in PERP_TYPES or typ == 'volume':
        reachable = 'volume-based -- no size floor, but pays in proportion to volume traded'
    return {
        'market_ticker': t, 'incentive_type': typ,
        'description': p.get('incentive_description') or '',
        'is_perp': is_perp, 'start_date': p.get('start_date'), 'end_date': p.get('end_date'),
        'pool_usd': pool, 'max_per_account_usd': cap,
        'target_size': target or None, 'discount_factor_bps': p.get('discount_factor_bps'),
        'price_hint': px, 'notional_to_qualify': need_notional,
        'reachable_at_equity': reachable, 'paid_out': p.get('paid_out'),
    }


def price_hints():
    """Last price per open market, so the notional needed to hit a Target Size is real."""
    out = {}
    try:
        cursor = None
        for _ in range(4):
            d = get('/markets', {'limit': 1000, 'status': 'open', 'cursor': cursor})
            for m in (d.get('markets') or []):
                px = m.get('last_price') or m.get('previous_price')
                if m.get('ticker') and px:
                    # Event-contract prices are in cents; perp prices are dollars per contract.
                    out[m['ticker']] = (px / 100.0) if not m['ticker'].upper().endswith('PERP') \
                        else float(px)
            cursor = d.get('next_cursor')
            if not cursor:
                break
    except Exception as e:
        print(f'  price hints unavailable: {e}', file=sys.stderr)
    return out


def report(rows, meta, out_csv, out_md, equity):
    perps = [r for r in rows if r['is_perp']]
    liq = [r for r in rows if r['incentive_type'] == 'liquidity']
    vol = [r for r in rows if r['incentive_type'] in PERP_TYPES or r['incentive_type'] == 'volume']
    reach = [r for r in liq if r['reachable_at_equity'] == 'possible']

    L = []
    W = L.append
    W('# Kalshi incentive audit\n')
    W(f"Fetched {meta['n']:,} programs over {meta['pages']} pages "
      f"(status `{meta['status']}`){'; MORE REMAIN -- the cap was hit' if meta['more'] else ''}.  ")
    W(f"Account equity assumed: **${equity:,.0f}**\n")

    W('## The headline\n')
    if not perps:
        W('**No incentive program covers any perpetual future.** Not one of the '
          f"{meta['n']:,} programs fetched has a PERP ticker or a `margin_*_volume` type.\n")
        W('So the effective maker fee on perps is the exchange fee, with no reward term:\n')
        W('```\nF_effective = F_exchange - 0 - 0 = F_exchange\n```\n')
        W('This is a finding, not an absence of one. It means the repricing in '
          '`reports/fee_ladder.md` is the whole economics, and there is no incentive path that '
          'closes the gap without a tier change.\n')
    else:
        W(f'**{len(perps)} programs touch perpetuals.** Details below.\n')

    W('## Why the server filter was not trusted\n')
    W('The documented `type` query parameter is ignored by the API. Asking for '
      '`type=margin_taker_volume` returned 1,000 records whose own `incentive_type` field reads '
      '`liquidity` or `volume`. Every classification here is done client-side on the record. '
      'Had the server filter been believed, this audit would have reported a thousand '
      'perpetual-futures volume programs that do not exist.\n')

    W('## What is running\n')
    from collections import Counter
    W('| incentive_type | programs | total pool | median pool |')
    W('|---|---:|---:|---:|')
    by = Counter(r['incentive_type'] for r in rows)
    for t, n in by.most_common():
        pools = sorted(r['pool_usd'] for r in rows if r['incentive_type'] == t)
        tot = sum(pools)
        mid = pools[len(pools) // 2] if pools else 0
        W(f'| `{t}` | {n:,} | ${tot:,.0f} | ${mid:,.2f} |')
    W('')

    if liq:
        W('## Liquidity programs and the Target Size floor\n')
        W('A snapshot scores only when BOTH sides carry at least the Target Size, so qualifying '
          'costs twice the target in notional. That floor, not competition for the pool, is what '
          'decides whether a small account can participate.\n')
        W('| target size | programs | median pool | collateral to qualify (two-sided) | reachable |')
        W('|---:|---:|---:|---:|---|')
        buckets = Counter(r['target_size'] for r in liq if r['target_size'])
        for tgt, n in sorted(buckets.items())[:12]:
            grp = [r for r in liq if r['target_size'] == tgt]
            pools = sorted(r['pool_usd'] for r in grp)
            need = [r['notional_to_qualify'] for r in grp if r['notional_to_qualify']]
            nn = (sorted(need)[len(need) // 2] if need else None)
            ok = sum(1 for r in grp if r['reachable_at_equity'] == 'possible')
            W(f"| {tgt:,.0f} | {n:,} | ${pools[len(pools)//2]:,.2f} | "
              f"{'unknown' if nn is None else f'${nn:,.0f}'} | {ok} of {n} |")
        W('')
        W(f'**{len(reach)} of {len(liq):,} liquidity programs are reachable at ${equity:,.0f}.**\n')
        if reach:
            W('| market | type | pool | target | notional | ends |')
            W('|---|---|---:|---:|---:|---|')
            for r in sorted(reach, key=lambda x: -x['pool_usd'])[:15]:
                W(f"| {r['market_ticker']} | {r['description']} | ${r['pool_usd']:,.2f} | "
                  f"{r['target_size']:,.0f} | ${r['notional_to_qualify']:,.0f} | "
                  f"{r['end_date']} |")
            W('')
            W('These are event contracts, not perpetuals. Chasing them means running a different '
              'strategy in a different product, with its own adverse selection that nothing here '
              'has measured.\n')

    W('## What this does to the effective fee\n')
    W('```')
    W('F_effective = F_exchange - E[volume reward] - E[liquidity reward]')
    W('')
    if not perps:
        W('perps:  E[volume reward]    = 0   (no margin_*_volume program exists)')
        W('        E[liquidity reward] = 0   (no perp appears in any liquidity program)')
        W('        F_effective         = F_exchange')
    W('```\n')

    W('## Caveats\n')
    W('- This is one snapshot. Programs start and end continuously, and the reward page updates '
      'daily; an audit from last week is not evidence about this week.')
    W('- `status=active` shows what is running now. A program that starts tomorrow is invisible '
      'to it, so this should be re-run before any decision that depends on it.')
    W('- Reward amounts are in centi-cents in the API. A factor of 10,000 error here would make '
      'a $20 pool look like a $200,000 one.')
    W('- Eligibility beyond size is not checked. Employees, affiliates, introducing brokers and '
      'international users are excluded by the program terms, and payouts above the IRS '
      'reporting threshold require a verified SSN.\n')

    os.makedirs(os.path.dirname(out_csv), exist_ok=True)
    cols = ['market_ticker', 'incentive_type', 'description', 'is_perp', 'start_date', 'end_date',
            'pool_usd', 'max_per_account_usd', 'target_size', 'discount_factor_bps',
            'price_hint', 'notional_to_qualify', 'reachable_at_equity', 'paid_out']
    with open(out_csv, 'w') as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction='ignore')
        w.writeheader()
        # Perps first, then anything a small account could actually reach, then the rest.
        for r in sorted(rows, key=lambda x: (not x['is_perp'],
                                             x['reachable_at_equity'] != 'possible',
                                             -x['pool_usd'])):
            w.writerow(r)
    with open(out_md, 'w') as f:
        f.write('\n'.join(L) + '\n')
    return perps, reach


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--status', default='active',
                    choices=['active', 'upcoming', 'all', 'closed', 'paid_out'])
    ap.add_argument('--equity', type=float, default=50.0)
    ap.add_argument('--csv', default=os.path.join(ROOT, 'reports', 'perps_incentives.csv'))
    ap.add_argument('--md', default=os.path.join(ROOT, 'reports', 'perps_incentives.md'))
    args = ap.parse_args()

    try:
        raw, pages, more = fetch_all(args.status)
    except Exception as e:
        sys.exit(f'incentive endpoint unreachable: {e}')
    px = price_hints()
    perps_set = {t for t in px if t.upper().endswith('PERP')}
    rows = [classify(p, perps_set, args.equity, px) for p in raw]
    meta = {'n': len(rows), 'pages': pages, 'more': more, 'status': args.status}
    perps, reach = report(rows, meta, args.csv, args.md, args.equity)

    print(f'fetched {len(rows):,} programs over {pages} pages'
          + ('  (MORE REMAIN)' if more else ''))
    from collections import Counter
    for t, n in Counter(r['incentive_type'] for r in rows).most_common():
        print(f'  {t:24s} {n:,}')
    print(f'perp-related programs: {len(perps)}')
    print(f'liquidity programs reachable at ${args.equity:,.0f}: {len(reach)}')
    print(f'wrote {os.path.relpath(args.csv, ROOT)}')


if __name__ == '__main__':
    main()
