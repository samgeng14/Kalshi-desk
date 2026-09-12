#!/usr/bin/env python3
"""Does the exit engine find a profitable round trip when one exists?

A study that reports "nothing works" is worthless unless it would have reported something else on
data where something does. Two synthetic tapes with hand-computed answers:

  FAVOURABLE  we are long from the bid and aggressive buyers repeatedly lift the offer. A resting
              passive exit at the offer fills, and the round trip is the full spread minus two
              maker fees.
  ADVERSE     nobody lifts, and the book walks away downhill. Every policy ends up crossing into
              a worse bid and the round trip is clearly negative.

If the engine reports the same thing on both, it is not measuring anything.
"""
import os, statistics, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'scripts'))
import exit_engine as X
from execution_sim import MAKER_BPS, TAKER_BPS

P = F = 0
def ok(c, l):
    global P, F
    if c: P += 1
    else:
        F += 1
        print(f'  FAIL {l}')

T0 = 1_789_000_000_000_000
TICK = 0.01


def tape(favourable, n_entries=40):
    """A stable 9.90/10.10 book, one entry every 20s, and either liquidity lifting the offer or
    a book that walks downhill."""
    evs, seq, ts = [], 1, T0
    bid, ask, qb, qa = 9.90, 10.10, 400.0, 400.0
    entries = []

    def push(side, price, d, reason='', typ='orderbook_delta', **extra):
        nonlocal seq
        seq += 1
        m = {'market_ticker': 'T'}
        if typ == 'orderbook_delta':
            m.update(side=side, price=round(price, 2), delta=d, last_update_reason=reason)
            sid = 1
        else:
            m.update(extra)
            sid = 3
        evs.append({'ev': typ, 'seq': seq, 'sid': sid, 'src_ts_us': ts, 'recv_ts_us': ts,
                    'msg': m})

    tseq = [1000]
    def trade(price, count, taker):
        tseq[0] += 1
        evs.append({'ev': 'trade', 'seq': tseq[0], 'sid': 3, 'src_ts_us': ts, 'recv_ts_us': ts,
                    'msg': {'market_ticker': 'T', 'price': round(price, 2), 'count': count,
                            'taker_side': taker}})

    evs.append({'ev': 'orderbook_snapshot', 'seq': seq, 'sid': 1, 'src_ts_us': ts,
                'recv_ts_us': ts,
                'msg': {'market_ticker': 'T', 'bid': [[bid, qb]], 'ask': [[ask, qa]]}})
    for p in [9.86, 9.87, 9.88, 9.89, 9.90, 9.91, 9.92, 9.93]:
        push('bid', p, 1.0)
        push('bid', p, -1.0)

    for i in range(n_entries):
        ts += 20_000_000
        push('bid', bid, 1.0)
        # We are long from the bid, filled right here.
        entries.append({'market': 'T', 'side': 'buy', 'fill_us': ts, 'price': bid,
                        'mid': (bid + ask) / 2, 'size': 2.0, 'rank': i, 'selector': 'model',
                        'n_test': n_entries * 10, 'score': 1.0})
        if favourable:
            # An aggressive buyer takes the offer, twice, which reaches a resting order there.
            for k in (1, 2):
                ts += 2_000_000
                trade(ask, 600.0, 'bid')
                push('ask', ask, -600.0)
                push('ask', ask, 600.0)
        else:
            # No lift, and the book walks downhill. It has to walk further than the HALF-SPREAD
            # to actually hurt: we are long from 9.90 inside a 200 bps book, so an offer four
            # ticks lower is still 160 bps above our entry. A first version stepped down four
            # ticks and reported +844 bps on a tape labelled adverse.
            for k in range(30):
                ts += 400_000
                nb, na = round(bid - TICK, 2), round(ask - TICK, 2)
                push('bid', nb, 400.0); push('bid', bid, -qb)
                push('ask', na, 400.0); push('ask', ask, -qa)
                bid, ask, qb, qa = nb, na, 400.0, 400.0
            ts += 40_000_000
            push('bid', bid, 1.0)
            # Put the book back where it started before the next entry. Without this the 30-tick
            # walk compounds across 40 entries, the price goes through zero, and every round trip
            # reports a 30,000 bps gain on a tape that is supposed to be losing money.
            ts += 1_000_000
            push('bid', 9.90, 400.0); push('bid', bid, -400.0)
            push('ask', 10.10, 400.0); push('ask', ask, -400.0)
            bid, ask, qb, qa = 9.90, 10.10, 400.0, 400.0
    ts += 120_000_000
    push('bid', bid, 1.0)
    return evs, entries


def run(favourable):
    evs, entries = tape(favourable)
    s = X.ExitSim(entries, None)
    for e in evs:
        src = 'kalshi_margin'
        s.feed(src, e)
    closed = s.finish()
    trades = [t for t in (X.round_trip(xo) for xo in closed) if t]
    by = {}
    for t in trades:
        by.setdefault(t['policy'], []).append(t)
    return s, {k: statistics.mean([x['net_bps'] for x in v]) for k, v in by.items()}, by


sF, netF, byF = run(True)
sA, netA, byA = run(False)

# The hand-computed favourable answer: buy 9.90, sell 10.10, two maker fees.
expect = (10.10 - 9.90) / 9.90 * 1e4 - 2 * MAKER_BPS
print(f'  hand-computed favourable round trip: {expect:+.1f} bps')
for k in sorted(netF):
    print(f'    {k:20s} favourable {netF[k]:+8.2f}   adverse {netA.get(k, float("nan")):+8.2f}')

ok('maker_touch_10s' in netF, 'power: the passive policy produced trades in the favourable tape')
if 'maker_touch_10s' in netF:
    ok(abs(netF['maker_touch_10s'] - expect) < 3.0,
       f"power: passive exit nets the full spread minus two maker fees "
       f"({netF['maker_touch_10s']:+.1f} vs {expect:+.1f})")
    pf = [t for t in byF['maker_touch_10s'] if t['passive_exit']]
    ok(len(pf) > len(byF['maker_touch_10s']) * 0.8,
       'power: and it really did exit passively rather than crossing')

ok('taker_now' in netF and netF['maker_touch_10s'] > netF['taker_now'] + 50,
   'power: waiting to be lifted beats crossing immediately, by a lot, when liquidity comes')

ok(all(v < 0 for v in netA.values()),
   f'power: every policy loses on the adverse tape, got {min(netA.values()):+.1f} to {max(netA.values()):+.1f}')
ok(netF['maker_touch_10s'] - netA['maker_touch_10s'] > 100,
   'power: the engine separates a favourable regime from an adverse one')

# The forced-exit accounting must not quietly drop the trades that could not be closed.
ok(sA.stats.get('exit_crossed', 0) + sA.stats.get('exit_forced', 0) > 0,
   'power: the adverse tape forced exits, and they were counted')
n_trades_A = sum(len(v) for v in byA.values())
ok(n_trades_A >= 40 * len(X.POLICIES) * 0.8,
   f'power: nearly every (entry, policy) pair produced a priced trade, got {n_trades_A}')

print(f'{P} passed, {F} failed')
sys.exit(1 if F else 0)
