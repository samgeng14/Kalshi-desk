#!/usr/bin/env python3
"""Tests for the impulse event builder and the sign table.

The failures that matter here are silent ones: a target that reads a price before the entry, an
event series that counts one price move as fifty trades, or a long that exits at the ask instead
of the bid and so reports a spread it never crossed.
"""
import csv, os, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'scripts'))
import impulse_events as E
import impulse_sign_table as S
from execution_sim import TAKER_BPS

P = F = 0
def ok(c, l):
    global P, F
    if c: P += 1
    else:
        F += 1
        print(f'  FAIL {l}')

T0 = 1_789_000_000_000_000


# ---- quote series --------------------------------------------------------------------------------
q = E.Quotes()
for i, (b, a) in enumerate([(9.90, 10.10), (9.90, 10.10), (9.95, 10.15), (10.00, 10.20)]):
    q.add(T0 + i * 1_000_000, b, a)
ok(len(q.ts) == 3, f'quotes: unchanged quotes are not stored twice, got {len(q.ts)}')
ok(q.at(T0 + 500_000) == (9.90, 10.10), 'quotes: the quote in force is the last one at or before')
ok(q.at(T0 - 1) is None, 'quotes: nothing before the series starts')
ok(q.at(T0 + 500_000_000) is None,
   'quotes: a quote 500s stale is not carried forward as if it were current')
ok(len(q.span(T0, T0 + 3_000_000)) == 3, 'quotes: span returns the path over an interval')

# A later add at the same timestamp overwrites rather than appending a second entry.
q.add(T0 + 3_000_000, 10.01, 10.21)
ok(q.bid[-1] == 10.01 and len(q.ts) == 3, 'quotes: same-timestamp update overwrites in place')


# ---- executable targets ---------------------------------------------------------------------------
def one_event(sign=1, bid=9.90, ask=10.10):
    return {'market': 'T', 'signal_us': T0, 'entry_us': T0, 'bid': bid, 'ask': ask,
            'impulse_sign': sign, 'impulse_bps': 20.0}

qq = E.Quotes()
qq.add(T0, 9.90, 10.10)
for hs in E.HORIZONS_S:
    qq.add(T0 + hs * 1_000_000, 10.30, 10.50)          # the market rose

ev = [one_event(sign=1)]
E.attach_targets(ev, {'T': qq})
e = ev[0]
# A long pays 10.10 and leaves at the later BID, 10.30.
exp_gross = (10.30 / 10.10 - 1) * 1e4
ok(abs(e['taker_long_gross_120s'] - exp_gross) < 1e-6,
   f"target: a long exits at the BID, not the mid or the ask ({e['taker_long_gross_120s']:.1f} "
   f"vs {exp_gross:.1f})")
ok(abs(e['taker_long_net_120s'] - (exp_gross - 2 * TAKER_BPS)) < 1e-6,
   'target: net pays two taker fees')
ok(e['signed_taker_net_120s'] == e['taker_long_net_120s'],
   'target: a positive impulse sign selects the long')
ok(e['taker_long_gross_120s'] < e['mid_ret_120s'],
   'target: the executable return is worse than the midpoint return, always')

# A short must be the mirror: sell the bid now, buy the ask later.
ev2 = [one_event(sign=-1)]
E.attach_targets(ev2, {'T': qq})
e2 = ev2[0]
exp_short = (9.90 / 10.50 - 1) * 1e4
ok(abs(e2['taker_short_gross_120s'] - exp_short) < 1e-6,
   'target: a short enters at the bid and covers at the ask')
ok(e2['signed_taker_net_120s'] == e2['taker_short_net_120s'],
   'target: a negative impulse sign selects the short')
ok(e2['signed_taker_net_120s'] < 0, 'target: shorting a rising market loses')

# Excursions must be on executable prices and bracket the outcome.
ok(e['mfe_120s'] >= e['taker_long_gross_120s'] - 1e-6,
   'target: the favourable excursion is at least the realised return')
ok(e['mae_120s'] <= e['taker_long_gross_120s'] + 1e-6,
   'target: and the adverse excursion is at most it')

# No future quote means no target, never a zero.
ev3 = [one_event()]
E.attach_targets(ev3, {'T': E.Quotes()})
ok(ev3[0]['taker_long_net_600s'] is None,
   'target: a missing future quote yields None, not a fabricated zero')


# ---- cooldown -------------------------------------------------------------------------------------
b = E.Builder(['T'], threshold_bps=5.0, cooldown_us=120_000_000)
b.last_event['T'] = T0
b.suppressed.clear()
class FakeBook:
    best_bid, best_ask, mid, spread_bps = 9.9, 10.1, 10.0, 20.0
    microprice, bids, asks, empty_snapshot = 10.0, {9.9: 1}, {10.1: 1}, False
    def obi(self, n): return 0.0
    def depth(self, n, s): return 1.0
b.eng.connect_us = None
before = len(b.events)
b._maybe_event('T', FakeBook(), T0 + 60_000_000)     # inside the cooldown
ok(len(b.events) == before, 'cooldown: an event inside the window is not emitted')

# Separation is enforced downstream, per horizon.
rows = [{'market': 'T', 'entry_us': T0 + i * 60_000_000} for i in range(20)]
ok(len(S.separate(rows, 120)) == 10, f'separation: 2m apart from 1m events keeps half, got '
   f'{len(S.separate(rows, 120))}')
ok(len(S.separate(rows, 600)) == 2, f'separation: 10m keeps 2 of 20, got '
   f'{len(S.separate(rows, 600))}')
two = [{'market': 'A', 'entry_us': T0}, {'market': 'B', 'entry_us': T0}]
ok(len(S.separate(two, 600)) == 2, 'separation: is per market, not global')


# ---- buckets and monotonicity ----------------------------------------------------------------------
ok(S.bucket_of(3) == 0 and S.bucket_of(7) == 1 and S.bucket_of(60) == len(S.BUCKETS) - 1,
   'buckets: magnitudes land in the right bin')
ok(S.bucket_of(-25) == S.bucket_of(25), 'buckets: the bin is on magnitude, not direction')

rising = [{'n': 10, 'mid': {'mean': v}} for v in (1, 3, 6, 12, 20)]
rho, nb = S.monotonic(rising, 'mid')
ok(rho is not None and rho > 0.99, f'monotonicity: a rising column scores +1, got {rho}')
noisy = [{'n': 10, 'mid': {'mean': v}} for v in (1, -2, 3, -1, 2)]
rho2, _ = S.monotonic(noisy, 'mid')
ok(rho2 is not None and abs(rho2) < 0.8, f'monotonicity: a wandering column does not, got {rho2}')
ok(S.monotonic([{'n': 1, 'mid': {'mean': 1.0}}], 'mid')[0] is None,
   'monotonicity: fewer than three populated buckets returns nothing rather than a rank')

print(f'{P} passed, {F} failed')
sys.exit(1 if F else 0)
