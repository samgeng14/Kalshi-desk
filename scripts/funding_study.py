#!/usr/bin/env python3
"""Is funding worth collecting, once you price getting in and out?

Kalshi publishes a funding rate on the perpetual `ticker` channel, every market, roughly every
1.5 seconds:

    "funding_rate": {"rate": 0.000108955413907,
                     "next_funding_time_ms": 1789156800000,
                     "ts_ms": 1789138798666}

`rate` is per funding PERIOD, not annualised. Reading it as an annual number, or as a per-hour
number, changes the answer by three orders of magnitude in the direction that makes funding look
like free money, so this file converts once, at the edge, and labels every figure.

THE ARITHMETIC THAT DECIDES IT. Funding is received once per period by the side that is short
when the perp trades above its anchor (and paid when below). Collecting it requires holding the
position through the funding timestamp, which requires entering and exiting, and the execution
study measured what that costs: 12.3 bps to cross BTC one way, 21.3 for LINK. So the question is
not "is the funding rate positive" but

    periods_to_break_even = (entry cost + exit cost) / rate_per_period

and whether the price moves less than that over the days it takes.

WHAT THE CURRENT TAPE CAN AND CANNOT ANSWER. The recording covers 14:00 to 16:31 UTC and the next
funding timestamp in it is 20:00 UTC, so there is NO funding event on this tape. The announced
rate is observable; the event study -- pre-funding basis, post-funding return, convergence -- is
not, and this file says so rather than computing something adjacent and calling it that.

  python3 scripts/funding_study.py --all
"""
import argparse, csv, glob, gzip, json, os, statistics, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

MAKER_BPS, TAKER_BPS = 5.0, 12.0


def maker_round_trip(market, default=10.0):
    """What entering AND exiting passively actually costs, from the execution study.

    Not two maker fees. The `join` net at 60 seconds already contains the half-spread earned, the
    markout given back, and the fee paid; doubling it prices both legs on the same basis. On ZEC
    that is about 8.8 bps rather than the 10 bps a fee-schedule reading would give, and on AAVE
    it is 14, so the direction of the error is not consistent and cannot be waved away.
    """
    p = os.path.join(ROOT, 'reports', 'execution_simulator.csv')
    if not os.path.exists(p):
        return default
    with open(p) as f:
        for r in csv.DictReader(f):
            if r.get('market') == market and r.get('mode') == 'join' \
                    and r.get('queue_bound') == 'pess':
                try:
                    return abs(float(r['net_60s_bps'])) * 2
                except (KeyError, TypeError, ValueError):
                    return default
    return default


# Kalshi encodes "no order on this side" as INT64_MAX, which arrives as 922337203685477.6 after
# the 10^4 scaling. It has reached the research path twice already in this project; a mid built
# from one produced a volatility of 4.2e19 bps in the first version of this file.
SENTINEL = 1e9


def num(v):
    try:
        f = float(v)
    except (TypeError, ValueError):
        return None
    if f != f or abs(f) >= SENTINEL:
        return None
    return f


def collect(hours):
    """Funding rate, mark and reference price per market from the recorded ticker channel."""
    series = defaultdict(list)     # ticker -> [(ts_us, rate, next_ms, mark, ref, mid)]
    n = 0
    for h in hours:
        p = os.path.join(ROOT, 'data', 'raw', 'source=kalshi_margin', f'hour={h}',
                         'events.jsonl.gz')
        if not os.path.exists(p):
            continue
        try:
            with gzip.open(p, 'rt') as f:
                for line in f:
                    if '"ticker"' not in line:
                        continue
                    try:
                        d = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if d.get('ev') != 'ticker':
                        continue
                    m = d.get('msg') or {}
                    t = m.get('market_ticker')
                    fr = m.get('funding_rate') or {}
                    if not t:
                        continue
                    n += 1
                    mark = (m.get('settlement_mark_price') or {}).get('price')
                    ref = (m.get('reference_price') or {}).get('price')
                    bid, ask = num(m.get('bid')), num(m.get('ask'))
                    # Both sides positive and not crossed, or there is no mid to speak of.
                    mid = ((bid + ask) / 2
                           if (bid and ask and bid > 0 and ask > 0 and ask >= bid) else None)
                    series[t].append((d.get('src_ts_us'), num(fr.get('rate')),
                                      fr.get('next_funding_time_ms'), num(mark), num(ref), mid))
        except (EOFError, OSError) as e:
            print(f'  [truncated] hour={h}: {e}', file=sys.stderr)
    return series, n


def exit_cost_for(market, default=12.0):
    p = os.path.join(ROOT, 'reports', 'execution_simulator.csv')
    if not os.path.exists(p):
        return default
    with open(p) as f:
        for r in csv.DictReader(f):
            if r.get('market') == market and r.get('mode') == 'taker' \
                    and r.get('queue_bound') == 'pess':
                try:
                    return float(r['entry_cost_bps']) + float(r['fee_bps'])
                except (KeyError, TypeError, ValueError):
                    return default
    return default


def analyse(series):
    rows = []
    for t, pts in sorted(series.items()):
        rates = [p[1] for p in pts if p[1] is not None]
        if not rates:
            continue
        # rate is per PERIOD, as a fraction. 0.000108955 = 1.09 bps per period.
        bps = [r * 1e4 for r in rates]
        nxt = {p[2] for p in pts if p[2]}
        ts = [p[0] for p in pts if p[0]]
        marks = [p[3] for p in pts if p[3]]
        mids = [p[5] for p in pts if p[5]]
        basis = None
        if marks and mids and len(marks) == len(mids):
            b = [(a / c - 1) * 1e4 for a, c in zip(mids, marks) if c]
            basis = statistics.median(b) if b else None
        rt_taker = exit_cost_for(t) * 2
        # A maker round trip is NOT two maker fees. The execution study measured what a passive
        # fill actually keeps -- half-spread plus markout minus fee -- and that net is the real
        # cost of the leg. Pricing a maker entry at the fee alone ignores the adverse selection
        # that the same study showed is the larger term on every wide market.
        rt_maker = maker_round_trip(t)
        med = statistics.median(bps)
        # Realised volatility, sampled at FIVE-MINUTE spacing rather than per tick.
        #
        # Scaling a tick-by-tick standard deviation up to eight hours overstates volatility badly,
        # because most of the variance between consecutive quotes is bid-ask bounce rather than
        # price movement. The bounce does not compound, so the sqrt-of-time scaling attributes
        # microstructure noise to the underlying. Five-minute sampling is long enough that the
        # bounce is a small share of each return and short enough to get usable counts from a
        # 2.5-hour tape.
        vol_period = None
        paired = [(a, b) for a, b in zip(ts, mids) if a and b]
        if len(paired) > 40:
            step = 300_000_000
            t0 = paired[0][0]
            # `next_at`, not `nxt`: the enclosing scope already has a `nxt` holding the set of
            # funding timestamps, and shadowing it here turned it into an int.
            sampled, next_at = [], t0
            for tt, mm in paired:
                if tt >= next_at:
                    sampled.append(mm)
                    next_at = tt + step
            if len(sampled) >= 8:
                rets = [(b - a) / a * 1e4 for a, b in zip(sampled, sampled[1:]) if a > 0]
                if len(rets) >= 6:
                    per_5m = statistics.pstdev(rets)
                    vol_period = per_5m * ((8 * 60 / 5) ** 0.5)   # 96 five-minute steps in 8h
        rows.append({
            'market': t, 'n_obs': len(bps), 'vol_bps_per_period': vol_period,
            'funding_over_vol': (abs(med) / vol_period) if vol_period else None,
            'rate_bps_per_period_median': med,
            'rate_bps_min': min(bps), 'rate_bps_max': max(bps),
            'rate_changed': len(set(round(x, 9) for x in bps)) > 1,
            'next_funding_ms': sorted(nxt)[0] if nxt else None,
            'tape_end_us': max(ts) if ts else None,
            'median_mid_minus_mark_bps': basis,
            'round_trip_taker_bps': rt_taker,
            'round_trip_maker_bps': rt_maker,
            'periods_to_break_even_taker': (rt_taker / abs(med)) if med else None,
            'periods_to_break_even_maker': (rt_maker / abs(med)) if med else None,
        })
    return rows


def report(rows, hours, n_ticker, out_md, out_csv):
    L = []
    W = L.append
    W('# Funding study\n')
    W(f'{n_ticker:,} ticker frames across {len(hours)} recorded hours.\n')

    covered = [r for r in rows if r['next_funding_ms'] and r['tape_end_us']
               and r['next_funding_ms'] * 1000 <= r['tape_end_us']]
    W('## Can this tape answer the event question?\n')
    if covered:
        W(f'{len(covered)} markets have a funding timestamp inside the recording.\n')
    else:
        r0 = next((r for r in rows if r['next_funding_ms']), None)
        if r0:
            import datetime as dt
            nf = dt.datetime.fromtimestamp(r0['next_funding_ms'] / 1000, dt.timezone.utc)
            te = dt.datetime.fromtimestamp(r0['tape_end_us'] / 1e6, dt.timezone.utc)
            W(f'**No.** The next funding timestamp is `{nf:%Y-%m-%d %H:%M} UTC` and the tape ends '
              f'at `{te:%Y-%m-%d %H:%M} UTC`.\n')
        W('So the pre-funding basis, the post-funding return and the convergence afterwards are '
          'not measurable here. Only the ANNOUNCED rate is, and that is what is reported below. '
          'The event study needs recorder uptime spanning a funding timestamp; funding runs at '
          '00:00, 08:00 and 16:00 ET, so a single overnight recording covers two of them.\n')

    W('## Announced rates\n')
    W('`rate` is per funding PERIOD. A period is eight hours, so three per day.\n')
    W('| market | rate bps/period | changed | round trip taker | round trip maker | '
      'periods to break even, taker | maker | 8h volatility | funding / vol |')
    W('|---|---:|---|---:|---:|---:|---:|---:|---:|')
    for r in sorted(rows, key=lambda x: -abs(x['rate_bps_per_period_median'])):
        pb = r['periods_to_break_even_taker']
        basis = r['median_mid_minus_mark_bps']
        pbm = r['periods_to_break_even_maker']
        vol = r['vol_bps_per_period']
        fov = r['funding_over_vol']
        W('| {} | {:+.3f} | {} | {:.1f} | {:.1f} | {} | {} | {} | {} |'.format(
            r['market'], r['rate_bps_per_period_median'],
            'yes' if r['rate_changed'] else 'no',
            r['round_trip_taker_bps'], r['round_trip_maker_bps'],
            'never' if not pb else f'{pb:,.1f}',
            'never' if not pbm else f'{pbm:,.1f}',
            '' if vol is None else f'{vol:,.0f}',
            '' if fov is None else f'{fov:.3f}'))
    W('')

    live = [r for r in rows if abs(r['rate_bps_per_period_median']) > 0.001]
    zero = [r for r in rows if abs(r['rate_bps_per_period_median']) <= 0.001]
    W('## Verdict\n')
    W(f'{len(zero)} of {len(rows)} markets announced a funding rate of exactly zero over the '
      f'whole recording. A zero rate is not a small opportunity, it is no opportunity.\n')
    if live:
        best = max(live, key=lambda r: abs(r['rate_bps_per_period_median']))
        pb = best['periods_to_break_even_taker']
        pbm = best['periods_to_break_even_maker']
        W(f"The largest rate on the tape is `{best['market']}` at "
          f"**{best['rate_bps_per_period_median']:+.2f} bps per period**.\n")
        W(f"Against a {best['round_trip_taker_bps']:.1f} bps taker round trip that is "
          f"**{pb:,.0f} funding periods**, or about **{pb/3:,.0f} days** of holding the position "
          f"before the funding has paid for the trade that put it on.\n")
        W(f"Entering and exiting passively is cheaper. Priced at what the execution study "
          f"measured a passive leg to actually cost -- half-spread earned, markout given back, "
          f"fee paid -- the round trip is {best['round_trip_maker_bps']:.1f} bps, which the "
          f"funding covers in **{pbm:,.1f} periods**, about {pbm*8:,.0f} hours.\n")
        W('That is short enough to be worth taking seriously, so the objection has to be '
          'quantitative rather than a shrug at holding risk.\n')
        vol = best['vol_bps_per_period']
        fov = best['funding_over_vol']
        if vol and fov:
            W(f"Over one funding period `{best['market']}` moves with a standard deviation of "
              f"**{vol:,.0f} bps**. The funding collected over that period is "
              f"{abs(best['rate_bps_per_period_median']):.2f} bps, or **{fov:.2%} of one standard "
              f"deviation of the thing you are holding**.\n")
            n_periods = (1.0 / fov) ** 2
            W(f'To make the funding the dominant term rather than the noise, the position has to '
              f'be held for roughly **{n_periods:,.0f} periods** -- the ratio improves only as '
              f'the square root of holding time, because funding accumulates linearly and risk '
              f'accumulates as the square root. That is {n_periods/3:,.0f} days of unhedged '
              f'exposure to collect a coupon worth a fraction of one day of its own volatility.\n')
        W('So this is not a funding trade. It is a directional position with a small coupon. '
          'Hedging it turns it into a real strategy -- and that hedge has its own two-sided '
          'cost, its own basis risk, and is not measured anywhere in this repository.\n')
    W('## What would make this worth revisiting\n')
    W('- A funding rate an order of magnitude larger. The break-even is linear in the rate, so a '
      '10 bps per-period rate on BTC would pay for a taker round trip in about two periods.')
    W('- A cheaper round trip: see `reports/fee_ladder.md`.')
    W('- A tape that spans a funding timestamp, so the question stops being about the announced '
      'rate and becomes about what actually happens to the basis around the event.')
    W('- A hedge. Funding capture against a spot or external perp hedge is a different strategy '
      'with different arithmetic, and nothing here has measured it.\n')

    os.makedirs(os.path.dirname(out_md), exist_ok=True)
    with open(out_md, 'w') as f:
        f.write('\n'.join(L) + '\n')
    if rows:
        with open(out_csv, 'w') as f:
            w = csv.DictWriter(f, fieldnames=sorted(rows[0]), extrasaction='ignore')
            w.writeheader()
            for r in rows:
                w.writerow(r)


def discover_hours():
    base = os.path.join(ROOT, 'data', 'raw', 'source=kalshi_margin')
    return sorted(os.path.basename(p).split('=', 1)[1]
                  for p in glob.glob(os.path.join(base, 'hour=*')))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--hours', nargs='*')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--md', default=os.path.join(ROOT, 'reports', 'funding_study.md'))
    ap.add_argument('--csv', default=os.path.join(ROOT, 'reports', 'funding_study.csv'))
    args = ap.parse_args()
    hours = args.hours or (discover_hours() if args.all else [])
    if not hours:
        sys.exit('no hours selected: pass --all')
    series, n = collect(hours)
    rows = analyse(series)
    report(rows, hours, n, args.md, args.csv)
    print(f'{n:,} ticker frames, {len(rows)} markets with a funding rate')
    for r in sorted(rows, key=lambda x: -abs(x['rate_bps_per_period_median']))[:5]:
        pb = r['periods_to_break_even_taker']
        print(f"  {r['market']:14s} {r['rate_bps_per_period_median']:+.3f} bps/period  "
              f"break-even {'never' if not pb else f'{pb:,.0f} periods ({pb/3:,.0f} days)'}")
    print(f'wrote {os.path.relpath(args.md, ROOT)}')


if __name__ == '__main__':
    main()
