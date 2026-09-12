#!/usr/bin/env python3
"""After realistic entry latency, is there any external price move left for us to capture?

That is the whole question, and it is narrower than "can we predict crypto". External venues move;
Kalshi's perp follows; the only thing that matters is whether any of the move is still available
by the time an order of ours could have been filled.

The study is built so that a negative answer is cheap to reach and hard to argue with:

  * Entry is TAKER and comes from the reconstructed book, not from a midpoint. Buying pays the ask
    that existed when the order would have arrived. This is deliberately the expensive assumption.
  * Every realized number goes through research/impulse_target_audit.py, which tracks provenance
    through arithmetic and refuses anything drawing on information from before the entry.
  * Prediction quality and executable P&L are reported in separate tables and never added up.
    The midpoint markout says whether the move continued; it is not money.
  * One external move is one event. A cooldown collapses the hundred feed updates it produces into
    a single observation, and the confidence intervals resample events rather than updates.
  * The splits are chronological, by event, never shuffled.

A prior result bounds what can be concluded here. `research/impulse_viability.md` shows that at a
17 bps round trip (maker 5 + taker 12) a PERFECT oracle on the top decile of one-minute BTC moves
still loses 3.2 bps, and that sub-minute horizons are smaller and therefore worse. So the
executable table at these horizons is expected to be negative whatever the signal is, and a
negative there is NOT evidence that the lag is absent. The markout table is what answers the
question; the executable table says whether this particular execution can harvest it.

Usage:
  python3 research/impulse_study.py --db data/impulse.sqlite
  python3 research/impulse_study.py --db data/impulse.sqlite --coin BTC --stage events
  python3 research/impulse_study.py --self-test
"""
import argparse
import bisect
import collections
import math
import os
import random
import sqlite3
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from impulse_target_audit import Stamped, Clock, Series, TargetAudit, CausalityError, markout_bps

HERE = os.path.dirname(os.path.abspath(__file__))
DB = os.path.join(HERE, '..', 'data', 'impulse.sqlite')

GRID_US = 50_000                       # 50 ms: finer than any latency we can plausibly achieve
IMPULSE_WINDOWS_US = [100_000, 250_000, 500_000, 1_000_000, 2_000_000, 5_000_000]
COOLDOWNS_US = [2_000_000, 5_000_000, 10_000_000, 30_000_000]
MARKOUT_HORIZONS_US = [250_000, 500_000, 1_000_000, 2_000_000, 5_000_000, 10_000_000,
                       30_000_000, 60_000_000]
RESPONSE_POINTS_US = [50_000, 100_000, 250_000, 500_000, 1_000_000, 2_000_000, 5_000_000]
VENUES = ['coinbase', 'binance', 'kraken', 'hyperliquid']

# docs/perps-basis.md: entry rests at the touch, exit crosses, so maker 5 + taker 12 = 17 bps.
# This study enters as a TAKER, so the round trip it charges itself is 12 + 12.
TAKER_FEE_BPS = 12.0
MAKER_FEE_BPS = 5.0

# An order does not arrive the instant the external venue prints. This is the budget from the
# external observation to our order resting at the exchange, and it is the number the whole
# question turns on, so it is a parameter that gets raced rather than a constant.
LATENCY_BUDGETS_US = [100_000, 250_000, 500_000, 1_000_000]
DEFAULT_LATENCY_US = 250_000


# ---- loading -------------------------------------------------------------------------------------

def load_grid(con, coin, grid_us=GRID_US):
    """Forward-filled venue mids and the index on a fixed grid, plus the raw Kalshi series.

    A grid rather than message time because the detector should not fire more often on a venue
    that happens to send more messages. Forward filling is exactly right here: between quotes the
    best price IS the last one quoted.
    """
    quotes = collections.defaultdict(list)
    for ts, venue, bid, ask in con.execute(
            'SELECT ts_us, venue, bid, ask FROM ext_quote WHERE coin=? AND bid>0 AND ask>0 ORDER BY ts_us',
            (coin,)):
        quotes[venue].append((ts, (bid + ask) / 2))
    index = [(ts, v) for ts, v in con.execute(
        'SELECT ts_us, value FROM index_value WHERE coin=? AND value>0 ORDER BY ts_us', (coin,))]
    if not quotes and not index:
        return None
    starts = [v[0][0] for v in quotes.values() if v] + ([index[0][0]] if index else [])
    ends = [v[-1][0] for v in quotes.values() if v] + ([index[-1][0]] if index else [])
    t0, t1 = max(starts), min(ends)
    if t1 <= t0:
        return None
    n = int((t1 - t0) // grid_us) + 1
    grid = [t0 + i * grid_us for i in range(n)]
    out = {'ts': grid, 'venues': {}, 'index': None, 't0': t0, 't1': t1}
    for venue, pts in quotes.items():
        out['venues'][venue] = forward_fill(pts, grid)
    if index:
        out['index'] = forward_fill(index, grid)
    return out


def forward_fill(points, grid):
    """Last observed value at or before each grid point; None before the first observation."""
    out, i, last = [], 0, None
    for t in grid:
        while i < len(points) and points[i][0] <= t:
            last = points[i][1]
            i += 1
        out.append(last)
    return out


def load_quote_series(con, coin):
    """Kalshi's own quoted bid and ask, from the `ticker` channel.

    THIS IS THE ONLY EXECUTABLE PRICE IN THE RECORDING, and it arrives about every two seconds.
    That is a property of the channel, not of this study: Kalshi throttles `ticker`, and the
    measured rate on this recording is 0.48 messages per second per contract.

    The `orderbook_delta` stream is three hundred times denser and would give millisecond timing,
    but no snapshot was ever captured on this recording, so the absolute book cannot be rebuilt
    from it. A "best bid" derived from unanchored deltas would be invented, so none is derived.
    What the deltas can honestly say is WHEN the book was touched, which `load_activity` keeps.
    """
    pts = [(ts, bid, ask, bs, asz) for ts, bid, ask, bs, asz in con.execute(
        'SELECT ts_us, bid, ask, bid_size, ask_size FROM kalshi_ticker '
        'WHERE coin=? AND bid>0 AND ask>0 AND ask>bid ORDER BY ts_us', (coin,))]
    gaps = [b[0] - a[0] for a, b in zip(pts, pts[1:])] if len(pts) > 1 else []
    gaps.sort()
    return {
        'points': pts,
        'bid': Series('kalshi_bid', [(p[0], p[1]) for p in pts]),
        'ask': Series('kalshi_ask', [(p[0], p[2]) for p in pts]),
        'mid': Series('kalshi_mid', [(p[0], (p[1] + p[2]) / 2) for p in pts]),
        'bid_size': Series('kalshi_bid_size', [(p[0], p[3] or 0) for p in pts]),
        'ask_size': Series('kalshi_ask_size', [(p[0], p[4] or 0) for p in pts]),
        'median_gap_us': gaps[len(gaps) // 2] if gaps else None,
        'p90_gap_us': gaps[int(0.9 * len(gaps))] if gaps else None,
    }


def load_activity(con, coin):
    """When Kalshi's book was touched, at 100 ms, from the delta stream.

    This answers "how long after an external move does the book start churning" without claiming
    to know what the book was. It is a timing measurement and nothing else: no price is taken from
    it, because none can be.
    """
    rows = [(b, side, ev) for b, side, ev in con.execute(
        'SELECT bucket_us, side, events FROM book_activity WHERE coin=? ORDER BY bucket_us', (coin,))]
    total = collections.defaultdict(int)
    for b, _side, ev in rows:
        total[b] += ev
    buckets = sorted(total)
    # No sequence-gap count is reported per coin. Kalshi's `seq` numbers the SUBSCRIPTION, not
    # the contract, so every message about another ticker looks like a gap from one coin's point
    # of view. Counting them that way produced nine million "gaps" on a stream with none, which is
    # the kind of number that gets quoted once and believed forever.
    return {'buckets': buckets, 'events': total, 'seq_gaps': None}


# ---- Part F: the impulse event -------------------------------------------------------------------

def composite(grid, i):
    """Median mid across the venues that have quoted by grid point i. Median because one venue
    printing a bad tick should not become an impulse."""
    vals = [v[i] for v in grid['venues'].values() if v[i] is not None]
    if not vals:
        return None
    return statistics.median(vals)


def detect_events(grid, window_us, threshold_bps, cooldown_us):
    """Every grid point where the cross-venue composite has moved by at least the threshold over
    the window, collapsed so that one external move is one event."""
    step = max(1, window_us // GRID_US)
    ts = grid['ts']
    comp = [composite(grid, i) for i in range(len(ts))]
    events, last_fire = [], -10 ** 18
    for i in range(step, len(ts)):
        a, b = comp[i - step], comp[i]
        if a is None or b is None or a <= 0 or b <= 0:
            continue
        r = math.log(b / a) * 10000.0
        if abs(r) < threshold_bps:
            continue
        if ts[i] - last_fire < cooldown_us:
            continue
        last_fire = ts[i]
        events.append({'i': i, 'ts_us': ts[i], 'return_bps': r, 'side': 1 if r > 0 else -1,
                       'window_us': window_us, 'composite': b})
    return events, comp


def features_at(grid, comp, i, window_us):
    """Everything a model could see at signal time. Nothing here may look past grid point i."""
    step = max(1, window_us // GRID_US)
    j = max(0, i - step)
    per_venue = {}
    for venue, series in grid['venues'].items():
        a, b = series[j], series[i]
        per_venue[venue] = math.log(b / a) * 10000.0 if a and b and a > 0 and b > 0 else None
    have = [v for v in per_venue.values() if v is not None]
    idx = None
    if grid['index'] and grid['index'][j] and grid['index'][i] and grid['index'][j] > 0:
        idx = math.log(grid['index'][i] / grid['index'][j]) * 10000.0
    return {
        'per_venue': per_venue,
        'median_return_bps': statistics.median(have) if have else None,
        'max_return_bps': max(have, key=abs) if have else None,
        'dispersion_bps': statistics.pstdev(have) if len(have) > 1 else 0.0,
        'venues_agreeing': sum(1 for v in have if v > 0) if have else 0,
        'venues_seen': len(have),
        'leader': max(per_venue, key=lambda k: abs(per_venue[k] or 0)) if have else None,
        'index_return_bps': idx,
    }


# ---- Parts E, H, I: entry, markout, executable ----------------------------------------------------

def build_samples(events, grid, comp, quotes, latency_us, audit):
    """One sample per event: a TAKER entry after the latency budget, then markouts.

    A buy pays the ask that existed when the order would have arrived, and leaves by hitting the
    bid. Everything realized is drawn at or after the entry, and the audit enforces that rather
    than the author's care.

    Note what the two-second quote cadence does here: the entry price is the first quote at or
    after the budget, which on average is a second late. That is not a modelling convenience, it
    is the truth about this channel, and it is reported rather than smoothed over.
    """
    samples, refused, no_quote, late = [], 0, 0, 0
    for n, ev in enumerate(events):
        signal_us = ev['ts_us']
        entry_target = signal_us + latency_us
        side = ev['side']
        entry = (quotes['ask'] if side > 0 else quotes['bid']).first_at_or_after(entry_target)
        if entry is None or entry.v <= 0:
            no_quote += 1
            continue
        actual_entry_us = entry.ts
        # A quote from long after the decision is a different trade. The cadence is what it is, so
        # the allowance is stated in terms of it rather than pretending to millisecond fills.
        if actual_entry_us - entry_target > 3 * (quotes['median_gap_us'] or 2_000_000):
            late += 1
            continue
        clock = Clock(signal_us, signal_us, actual_entry_us, event_id=n)
        s = {'event_id': n, 'signal_us': signal_us, 'target_entry_us': entry_target,
             'actual_entry_us': actual_entry_us, 'latency_ms': (actual_entry_us - signal_us) / 1000.0,
             'side': side, 'entry_price': entry, 'impulse_bps': ev['return_bps'],
             'window_us': ev['window_us'],
             'size_at_entry': float((quotes['ask_size'] if side > 0 else quotes['bid_size'])
                                    .first_at_or_after(entry_target) or 0),
             'features': features_at(grid, comp, ev['i'], ev['window_us']),
             'markout': {}, 'executable': {}}
        try:
            audit.check(clock, entry_price=entry)
            for h in MARKOUT_HORIZONS_US:
                m = quotes['mid'].first_at_or_after(actual_entry_us + h)
                if m is not None:
                    audit.check(clock, exit_mid=m)
                    s['markout'][h] = float(markout_bps(side, entry, m))
                x = (quotes['bid'] if side > 0 else quotes['ask']).first_at_or_after(actual_entry_us + h)
                if x is not None:
                    audit.check(clock, exit_price=x)
                    s['executable'][h] = float(markout_bps(side, entry, x)) - 2 * TAKER_FEE_BPS
        except CausalityError:
            refused += 1
            continue
        samples.append(s)
    return samples, {'refused': refused, 'no_quote': no_quote, 'late': late}


# ---- Part J: baselines ---------------------------------------------------------------------------
#
# Every one of these is something a person could do without a model. A richer model has to beat
# them out of sample or it has not earned its complexity, and "beat" means on the same events with
# the same entry and the same fees.

def baselines(sample, rng):
    f = sample['features']
    pv = f['per_venue']
    target = f.get('target_return_bps')
    lag = None
    if target is not None and f['median_return_bps'] is not None:
        lag = f['median_return_bps'] - target        # how much of the move the perp has not taken
    return {
        'B0 random': rng.choice((1, -1)),
        'B1 target venue': sign(target),
        'B2 coinbase': sign(pv.get('coinbase')),
        'B3 cross-venue median': sign(f['median_return_bps']),
        'B4 threshold only': sample['side'],
        'B5 composite minus target (lag)': sign(lag),
    }


def sign(x):
    if x is None:
        return 0
    return 1 if x > 0 else (-1 if x < 0 else 0)


# ---- Part L: event-level inference ---------------------------------------------------------------

def event_bootstrap(values, event_ids, n=2000, seed=5):
    """Resample EVENTS, not rows. A hundred feed updates from one external move is one
    observation, and treating them as a hundred is how three hours becomes a huge sample."""
    if not values:
        return float('nan'), float('nan'), float('nan')
    by_event = collections.defaultdict(list)
    for v, e in zip(values, event_ids):
        by_event[e].append(v)
    groups = list(by_event.values())
    rng = random.Random(seed)
    mean = sum(values) / len(values)
    means = []
    for _ in range(n):
        total, count = 0.0, 0
        for _ in range(len(groups)):
            g = groups[rng.randrange(len(groups))]
            total += sum(g)
            count += len(g)
        means.append(total / count if count else 0.0)
    means.sort()
    return mean, means[int(0.05 * n)], means[int(0.95 * n) - 1]


# ---- Part K: chronological splits -----------------------------------------------------------------

def split(samples, fractions=(0.50, 0.25, 0.25)):
    """Train / validation / test in time order, split by EVENT. Never shuffled: a shuffled split
    puts the same external move on both sides and scores a model on what it was fitted to."""
    ordered = sorted(samples, key=lambda s: s['signal_us'])
    n = len(ordered)
    a = int(n * fractions[0])
    b = a + int(n * fractions[1])
    return ordered[:a], ordered[a:b], ordered[b:]


# ---- Part M: the market response curve -------------------------------------------------------------

def response_curve(events, grid, comp, quotes, activity):
    """How much of an external move has been taken up, at each delay after it, on three clocks.

    Three, because the recording supports three different claims at three different resolutions,
    and running them together would let the coarsest one borrow the finest one's precision:

      external    the cross-venue composite itself, at 50 ms. Dense and reliable.
      book touch  how many Kalshi book messages arrived in each 100 ms after the move, against the
                  same window's baseline rate. This is TIMING ONLY. It says the book started
                  moving; it does not say where to, because no snapshot was captured and an
                  unanchored delta stream cannot give a price.
      quoted      Kalshi's own bid/ask, which arrives about every two seconds. Anything below that
                  is not measured here and is reported as unmeasurable rather than interpolated.
    """
    out = {'buckets': collections.defaultdict(list), 'quote_cadence_us': quotes['median_gap_us']}
    acts, ev_counts = activity['buckets'], activity['events']
    for ev in events:
        base_mid = quotes['mid'].at_or_before(ev['ts_us'])
        size = abs(ev['return_bps'])
        label = ('2-5 bps' if size < 5 else '5-10 bps' if size < 10 else
                 '10-20 bps' if size < 20 else '20+ bps')
        row = {'impulse_bps': ev['return_bps'], 'side': ev['side'],
               'external': {}, 'quoted': {}, 'activity': {}}
        # External: the composite's own continuation, on the 50 ms grid.
        i0 = ev['i']
        base_comp = comp[i0]
        for d in RESPONSE_POINTS_US + [10_000_000]:
            j = i0 + d // GRID_US
            if j < len(comp) and comp[j] and base_comp and base_comp > 0:
                row['external'][d] = ev['side'] * math.log(comp[j] / base_comp) * 10000.0
        # Kalshi quoted, where the cadence allows it to mean anything.
        if base_mid is not None and base_mid.v > 0:
            for d in RESPONSE_POINTS_US + [10_000_000]:
                m = quotes['mid'].first_at_or_after(ev['ts_us'] + d)
                if m is not None and m.v > 0 and m.ts - (ev['ts_us'] + d) < 2_000_000:
                    row['quoted'][d] = ev['side'] * math.log(m.v / base_mid.v) * 10000.0
        # Book activity: messages per 100 ms, against the rate in the ten seconds before.
        #
        # And the same measurement at NEGATIVE delays, which is the control that decides whether
        # this means anything. If the book is already elevated before the external move, the
        # detector is firing on periods that were busy anyway and the post-event number is not a
        # reaction. A control that is flat is what makes the reaction real.
        b0 = (ev['ts_us'] // 100_000) * 100_000
        before = sum(ev_counts.get(b0 - k * 100_000, 0) for k in range(11, 111)) / 100.0
        for d in [-r for r in reversed(RESPONSE_POINTS_US)] + RESPONSE_POINTS_US:
            got = ev_counts.get(b0 + (d // 100_000) * 100_000, 0)
            row['activity'][d] = (got / before) if before > 0 else None
        out['buckets'][label].append(row)
    return out


# ---- reporting (Part O) ----------------------------------------------------------------------------
#
# Per trade, per event and per recorded hour, with the number of recorded hours stated. Never per
# day from a partial day, and never annualized.

def hours_of(samples):
    if not samples:
        return 0.0
    return (max(s['signal_us'] for s in samples) - min(s['signal_us'] for s in samples)) / 3.6e9


def report(coin, quotes, activity, events, samples, stats, latency_us, threshold_bps,
           window_us, cooldown_us, span_hours):
    hrs = hours_of(samples)
    print(f'\n{"="*104}')
    print(f'{coin}: external move >= {threshold_bps:.0f} bps over {window_us/1000:.0f} ms, '
          f'cooldown {cooldown_us/1e6:.0f}s, taker entry {latency_us/1000:.0f} ms after the signal')
    print(f'{"="*104}')
    cad = quotes['median_gap_us']
    print(f'  RESOLUTION. Kalshi quotes this contract every {cad/1000:.0f} ms (median, p90 '
          f'{quotes["p90_gap_us"]/1000:.0f} ms). Nothing below that is measured.')
    print(f'  The book delta stream is dense enough for millisecond timing but no snapshot was')
    print(f'  captured, so it gives WHEN the book moved and never to what price.')
    print(f'  {len(events)} events, {len(samples)} with a quote to enter on '
          f'({stats["no_quote"]} had none, {stats["late"]} only had one too late to call an entry, '
          f'{stats["refused"]} refused by the causal audit)')
    print(f'  {span_hours:.2f} hours recorded, {hrs:.2f} hours spanned by these events, '
          f'{sum(activity["events"].values()):,} book messages behind the activity column')
    if not samples:
        print('  Nothing to report.')
        return
    lat = sorted(s['latency_ms'] for s in samples)
    print(f'  achieved entry latency (signal to the quote we could have taken): '
          f'median {lat[len(lat)//2]:.0f} ms, p90 {lat[int(0.9*len(lat))]:.0f} ms, max {lat[-1]:.0f} ms')

    print('\n  PREDICTION QUALITY - midpoint markout after entry, in the direction of the impulse.')
    print('  This says whether the move continued. It is not money and is never added to the table below.')
    print(f'  {"horizon":>9} {"n":>6} {"mean bps":>10} {"median":>9} {"hit rate":>9} {"90% CI (by event)":>26}')
    for h in MARKOUT_HORIZONS_US:
        vals = [s['markout'][h] for s in samples if h in s['markout']]
        ids = [s['event_id'] for s in samples if h in s['markout']]
        if len(vals) < 10:
            continue
        m, lo, hi = event_bootstrap(vals, ids)
        hit = sum(1 for v in vals if v > 0) / len(vals)
        print(f'  {h/1000:>8.0f}ms {len(vals):>6} {m:>+10.2f} {statistics.median(vals):>+9.2f} '
              f'{hit:>8.1%} {f"[{lo:+.2f}, {hi:+.2f}]":>26}')

    print(f'\n  EXECUTABLE - taker in at the ask, taker out at the bid, {2*TAKER_FEE_BPS:.0f} bps of fees.')
    print('  research/impulse_viability.md already shows a perfect oracle on the top decile of')
    print('  one-minute BTC moves loses 3.2 bps at a 17 bps round trip, so a negative here at these')
    print('  horizons is arithmetic, not evidence about the signal.')
    print(f'  {"horizon":>9} {"n":>6} {"mean bps":>10} {"hit rate":>9} {"90% CI (by event)":>26}')
    for h in MARKOUT_HORIZONS_US:
        vals = [s['executable'][h] for s in samples if h in s['executable']]
        ids = [s['event_id'] for s in samples if h in s['executable']]
        if len(vals) < 10:
            continue
        m, lo, hi = event_bootstrap(vals, ids)
        hit = sum(1 for v in vals if v > 0) / len(vals)
        print(f'  {h/1000:>8.0f}ms {len(vals):>6} {m:>+10.2f} {hit:>8.1%} {f"[{lo:+.2f}, {hi:+.2f}]":>26}')

    h = 10_000_000
    vals = [s['executable'][h] for s in samples if h in s['executable']]
    if vals and hrs > 0:
        print(f'\n  {len(vals)} trades over {hrs:.2f} recorded hours at the {h/1e6:.0f}s horizon: '
              f'{sum(vals)/len(vals):+.2f} bps per trade, {len(vals)/hrs:.1f} trades per recorded hour.')
        print('  Not extrapolated to a day and not annualized: this is a fraction of one day.')


def report_baselines(samples, horizon_us=10_000_000):
    """Part J. Each baseline picks a direction; the entry, the exit and the fees are the study's
    own, so the only thing being compared is the direction call."""
    tr, va, te = split(samples)
    if len(te) < 20:
        print(f'\n  BASELINES: {len(te)} events in the held-out slice, too few to score.')
        return
    rng = random.Random(3)
    print(f'\n  BASELINES on the chronologically last {len(te)} events, {horizon_us/1e6:.0f}s horizon.')
    print('  Nothing richer is worth building until it beats these, out of sample, on these events.')
    print(f'  {"baseline":>34} {"n":>6} {"markout bps":>13} {"executable":>12} {"agrees w/ impulse":>19}')
    for name in baselines(samples[0], rng):
        vals, exe, ids, agree = [], [], [], 0
        for s in te:
            if horizon_us not in s['markout']:
                continue
            d = baselines(s, rng)[name]
            if d == 0:
                continue
            flip = 1 if d == s['side'] else -1
            agree += 1 if flip > 0 else 0
            vals.append(flip * s['markout'][horizon_us])
            gross = s['executable'].get(horizon_us)
            if gross is not None:
                exe.append(flip * (gross + 2 * TAKER_FEE_BPS) - 2 * TAKER_FEE_BPS)
            ids.append(s['event_id'])
        if len(vals) < 10:
            continue
        m, lo, hi = event_bootstrap(vals, ids)
        me, _, _ = event_bootstrap(exe, ids[:len(exe)]) if exe else (float('nan'),) * 3
        print(f'  {name:>34} {len(vals):>6} {m:>+13.2f} {me:>+12.2f} {agree/len(vals):>18.0%}')


def report_response(curve):
    print('\n  MARKET RESPONSE CURVE')
    print('  Three clocks, because the recording supports three claims at three resolutions and')
    print('  running them together would let the coarsest borrow the finest one\'s precision.')
    cad = curve['quote_cadence_us']
    for label in ('2-5 bps', '5-10 bps', '10-20 bps', '20+ bps'):
        rows = curve['buckets'].get(label)
        if not rows or len(rows) < 10:
            continue
        impulse = sum(abs(r['impulse_bps']) for r in rows) / len(rows)
        print(f'\n    {label}  (n={len(rows)}, mean external move {impulse:.1f} bps)')
        print(f'    {"delay":>8} {"external bps":>14} {"% of move":>11} {"book activity":>15} '
              f'{"Kalshi quoted":>15} {"% of move":>11}')
        for d in [-1_000_000, -500_000, -250_000] + RESPONSE_POINTS_US + [10_000_000]:
            ex = [r['external'][d] for r in rows if d in r['external']]
            qt = [r['quoted'][d] for r in rows if d in r['quoted']]
            ac = [r['activity'][d] for r in rows if r['activity'].get(d) is not None]
            mex = sum(ex) / len(ex) if ex else float('nan')
            if d < 0:
                mac0 = sum(ac) / len(ac) if ac else None
                print(f'    {d/1000:>7.0f}ms {"(control)":>14} {"":>10} '
                      f'{(f"{mac0:.2f}x" if mac0 is not None else "-"):>15} {"":>15} {"":>11}')
                continue
            mq = sum(qt) / len(qt) if qt else None
            mac = sum(ac) / len(ac) if ac else None
            below = cad and d < cad
            qcol = 'not measured' if (mq is None or below) else f'{mq:+.2f}'
            qpct = '' if (mq is None or below) else f'{mq/impulse:.0%}'
            accol = f'{mac:.2f}x' if mac is not None else '-'
            print(f'    {d/1000:>7.0f}ms {mex:>+14.2f} {(mex/impulse if impulse else 0):>10.0%} '
                  f'{accol:>15} {qcol:>15} {qpct:>11}')
    if cad:
        print(f'\n    "not measured" is where the delay is shorter than Kalshi\'s own {cad/1000:.0f} ms')
        print('    quote cadence. Book activity is a multiple of the message rate in the ten seconds')
        print('    before the event: it says the book was moving, not where it moved to.')
    # The control is not decoration. If the book is already elevated before the detector fires,
    # the post-event elevation is not a reaction to anything, and saying so is the report's job
    # rather than the reader's.
    for label, rows in curve['buckets'].items():
        if len(rows) < 10:
            continue
        pre = [r['activity'][-500_000] for r in rows if r['activity'].get(-500_000) is not None]
        post = [r['activity'][50_000] for r in rows if r['activity'].get(50_000) is not None]
        if not pre or not post:
            continue
        mpre, mpost = sum(pre) / len(pre), sum(post) / len(post)
        if mpre >= mpost * 0.9:
            print(f'\n    READ THE CONTROL on {label}: the book was already at {mpre:.2f}x half a second')
            print(f'    BEFORE the detector fired, against {mpost:.2f}x just after. The elevation is not a')
            print('    reaction to this signal. An event stamped at the END of its detection window is')
            print('    already up to that window old, so activity "before" the stamp may be activity')
            print('    during the move. Nothing here supports a claim that Kalshi reacts in 50 ms; what')
            print('    it does support is that the book is busy around these moves, which is not news.')
            break


# ---- driver ------------------------------------------------------------------------------------

def run(db, coins, threshold_bps, window_us, cooldown_us, latency_us, stages):
    con = sqlite3.connect(f'file:{db}?mode=ro', uri=True)
    for coin in coins:
        grid = load_grid(con, coin)
        if not grid:
            print(f'{coin}: no external quotes recorded')
            continue
        quotes = load_quote_series(con, coin)
        activity = load_activity(con, coin)
        if not quotes['points']:
            print(f'{coin}: no Kalshi quotes recorded')
            continue
        span_hours = (grid['t1'] - grid['t0']) / 3.6e9
        kalshi_mid = forward_fill([(p[0], (p[1] + p[2]) / 2) for p in quotes['points']], grid['ts'])
        events, comp = detect_events(grid, window_us, threshold_bps, cooldown_us)
        audit = TargetAudit()
        samples, stats = build_samples(events, grid, comp, quotes, latency_us, audit)
        step = max(1, window_us // GRID_US)
        by_ts = {e['ts_us']: e['i'] for e in events}
        for s in samples:
            i = by_ts.get(s['signal_us'])
            if i is None:
                continue
            a, b = kalshi_mid[max(0, i - step)], kalshi_mid[i]
            s['features']['target_return_bps'] = (math.log(b / a) * 10000.0
                                                  if a and b and a > 0 and b > 0 else None)
        report(coin, quotes, activity, events, samples, stats, latency_us, threshold_bps,
               window_us, cooldown_us, span_hours)
        if samples and 'baselines' in stages:
            report_baselines(samples)
        if 'response' in stages and events:
            report_response(response_curve(events, grid, comp, quotes, activity))
        a = audit.summary()
        print(f'\n  causal audit: {a["checked"]:,} realized components checked, {a["rejected"]} refused')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--db', default=DB)
    ap.add_argument('--coins', nargs='*', default=['BTC', 'ETH'])
    ap.add_argument('--threshold-bps', type=float, default=5.0)
    ap.add_argument('--window-ms', type=int, default=1000)
    ap.add_argument('--cooldown-s', type=int, default=5)
    ap.add_argument('--latency-ms', type=int, default=250)
    ap.add_argument('--stages', nargs='*', default=['baselines', 'response'])
    args = ap.parse_args()
    if not os.path.exists(args.db):
        raise SystemExit(f'no database at {args.db}; run research/impulse_prepare.py first')
    run(args.db, args.coins, args.threshold_bps, args.window_ms * 1000, args.cooldown_s * 1_000_000,
        args.latency_ms * 1000, args.stages)


if __name__ == '__main__':
    main()
