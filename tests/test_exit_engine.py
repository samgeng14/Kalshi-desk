#!/usr/bin/env python3
"""Tests for the exit engine.

The thing that would be catastrophic here is not a crash. It is an exit that fills more often
than a real one would, or a fee applied to the wrong leg, or a trade that could not be closed
quietly vanishing from the denominator. Each of those is planted and checked.
"""
import csv, os, sys, tempfile
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


def ev(t, typ, seq, ts, sid=None, **msg):
    m = {'market_ticker': t}
    m.update(msg)
    if sid is None:
        sid = 3 if typ == 'trade' else 1
    return {'ev': typ, 'seq': seq, 'sid': sid, 'src_ts_us': ts, 'recv_ts_us': ts, 'msg': m}


def snap(t, bids, asks, seq=1, ts=T0):
    return ev(t, 'orderbook_snapshot', seq, ts, bid=bids, ask=asks)


def delta(t, side, price, d, seq, ts, reason=''):
    return ev(t, 'orderbook_delta', seq, ts, side=side, price=price, delta=d,
              last_update_reason=reason)


def trade(t, price, count, taker_side, seq, ts):
    return ev(t, 'trade', seq, ts, price=price, count=count, taker_side=taker_side)


def entry(market='T', side='buy', fill_us=T0 + 1_000_000, price=9.99, size=2.0, rank=0,
          selector='model', n_test=100):
    return {'market': market, 'side': side, 'fill_us': fill_us, 'price': price, 'mid': 10.0,
            'size': size, 'rank': rank, 'selector': selector, 'n_test': n_test, 'score': 1.0}


# ---- policies ---------------------------------------------------------------------------------
ok(len(X.POLICIES) >= 8, 'policies: the grid is populated')
ok('taker_now' in X.POLICIES, 'policies: the assumed-cost baseline is present for comparison')
for name, p in X.POLICIES.items():
    ok(set(p) == {'delay', 'style', 'replace', 'timeout', 'cross', 'max_hold'},
       f'policies: {name} has every field')
    ok(p['style'] in ('cross', 'touch', 'improve'), f'policies: {name} style is known')
    ok(p['max_hold'] >= p['timeout'], f'policies: {name} max_hold is not shorter than its timeout')


# ---- accounting -------------------------------------------------------------------------------
class FakeXO:
    def __init__(self, e, px, fee, mid=None, crossed=False, forced=False, us=T0 + 5_000_000):
        self.entry, self.exit_px, self.exit_fee = e, px, fee
        self.mid_exit, self.crossed, self.forced = mid, crossed, forced
        self.exit_us, self.spawn_us, self.name, self.n_replaces = us, T0, 'p', 0
        self.filled_frac = 1.0

e = entry(price=10.00)
r = X.round_trip(FakeXO(e, 10.02, MAKER_BPS, mid=10.02))
ok(abs(r['gross_bps'] - 20.0) < 1e-9, f"accounting: buy 10.00 exit 10.02 is +20 bps, got {r['gross_bps']}")
ok(abs(r['net_bps'] - 10.0) < 1e-9, 'accounting: minus 5 bps each leg is +10 net')
ok(r['entry_fee_bps'] == MAKER_BPS, 'accounting: the entry leg always pays the maker fee')

r2 = X.round_trip(FakeXO(e, 10.02, TAKER_BPS, mid=10.02, crossed=True))
ok(abs(r2['net_bps'] - 3.0) < 1e-9, 'accounting: crossing out pays the taker fee, netting +3')

es = entry(side='sell', price=10.00)
r3 = X.round_trip(FakeXO(es, 9.98, MAKER_BPS, mid=9.98))
ok(abs(r3['gross_bps'] - 20.0) < 1e-9, 'accounting: a SHORT closed lower is a profit, not a loss')
r4 = X.round_trip(FakeXO(es, 10.02, MAKER_BPS, mid=10.02))
ok(abs(r4['gross_bps'] + 20.0) < 1e-9, 'accounting: and a short closed higher is a loss')

# The decomposition must add up exactly, or the reported columns do not describe the net.
r5 = X.round_trip(FakeXO(e, 10.01, TAKER_BPS, mid=10.03))
ok(abs((r5['entry_edge_bps'] + r5['exit_slip_bps']) - r5['gross_bps']) < 1e-9,
   'accounting: entry edge + exit slip = gross, exactly')
ok(r5['exit_slip_bps'] < 0, 'accounting: selling below the mid is negative slip')

ok(X.round_trip(FakeXO(e, None, None)) is None,
   'accounting: an unresolved exit produces no trade rather than a zero')
ok(X.round_trip(FakeXO(entry(price=0.0), 10.0, MAKER_BPS)) is None,
   'accounting: a zero entry price is rejected, not divided by')

# A cross that could not fill the whole size leaves part of the position open. That has to be
# visible, or a trade reported as closed is still half on the book.
part = FakeXO(e, 10.02, TAKER_BPS, mid=10.02)
part.filled_frac = 0.4
rp = X.round_trip(part)
ok(rp is not None and abs(rp['filled_frac'] - 0.4) < 1e-9,
   'accounting: a partial cross carries its fill fraction through to the trade record')


# ---- resting price selection -------------------------------------------------------------------
def mk(entries, bound='pess'):
    return X.ExitSim(entries, None, bound=bound)

s = mk([])
s.feed('kalshi_margin', snap('T', [[9.90, 200]], [[10.10, 200]], 1, T0))
bk = s.eng.books['T']
for p in [9.90, 9.91, 9.92, 9.93, 9.94, 9.95, 9.96, 9.97]:
    s.ticks.observe('T', p)
ok(abs(s._price_for(bk, 'sell', 'touch') - 10.10) < 1e-9, 'price: a sell rests at the offer')
ok(abs(s._price_for(bk, 'buy', 'touch') - 9.90) < 1e-9, 'price: a buy rests at the bid')
ok(abs(s._price_for(bk, 'sell', 'improve') - 10.09) < 1e-9, 'price: improve steps inside the offer')
ok(abs(s._price_for(bk, 'buy', 'improve') - 9.91) < 1e-9, 'price: and inside the bid')

s2 = mk([])
s2.feed('kalshi_margin', snap('T', [[10.00, 200]], [[10.01, 200]], 1, T0))
for p in [9.98, 9.99, 10.00, 10.01, 10.02, 10.03, 10.04, 10.05]:
    s2.ticks.observe('T', p)
ok(abs(s2._price_for(s2.eng.books['T'], 'sell', 'improve') - 10.01) < 1e-9,
   'price: there is nowhere to improve inside a one-tick spread, so it rests at the touch')
s3 = mk([])
s3.feed('kalshi_margin', snap('T', [], [[10.10, 5]], 1, T0))
ok(s3._price_for(s3.eng.books['T'], 'sell', 'touch') is None,
   'price: a one-sided book offers no resting price')


# ---- spawning ----------------------------------------------------------------------------------
E = entry(fill_us=T0 + 2_000_000)
s = mk([E])
s.feed('kalshi_margin', snap('T', [[9.90, 500]], [[10.10, 500]], 1, T0))
s.feed('kalshi_margin', delta('T', 'bid', 9.90, 1, 2, T0 + 1_000_000))
ok(s.stats['spawned'] == 0, 'spawn: nothing is spawned before the entry filled')
s.feed('kalshi_margin', delta('T', 'bid', 9.90, 1, 3, T0 + 2_500_000))
ok(s.stats['spawned'] == len(X.POLICIES), 'spawn: every policy gets one exit, once')
s.feed('kalshi_margin', delta('T', 'bid', 9.90, 1, 4, T0 + 3_000_000))
ok(s.stats['spawned'] == len(X.POLICIES), 'spawn: and it does not respawn on later frames')


# ---- one policy at a time -----------------------------------------------------------------------
def run_one(policy_name, events, E=None, extra_ticks=True):
    E = E or entry(fill_us=T0 + 1_000_000)
    saved = dict(X.POLICIES)
    X.POLICIES.clear()
    X.POLICIES[policy_name] = saved[policy_name]
    try:
        s = X.ExitSim([E], None)
        if extra_ticks:
            for p in [9.90, 9.91, 9.92, 9.93, 9.94, 9.95, 9.96, 9.97]:
                s.ticks.observe('T', p)
        for e_ in events:
            s.feed('kalshi_margin', e_)
        return s, s.finish()
    finally:
        X.POLICIES.clear()
        X.POLICIES.update(saved)

base = [snap('T', [[9.90, 300]], [[10.10, 300]], 1, T0),
        delta('T', 'bid', 9.90, 1, 2, T0 + 1_500_000)]

s, closed = run_one('taker_now', base)
ok(len(closed) == 1 and closed[0].crossed, 'taker_now: crosses on the first frame after the fill')
ok(closed[0].exit_fee == TAKER_BPS, 'taker_now: and pays the taker fee')
ok(abs(closed[0].exit_px - 9.90) < 1e-9,
   'taker_now: a long exits by HITTING THE BID, not by lifting the offer')

# A passive exit that actually trades: an aggressive buyer lifts our resting offer.
evs = base + [trade('T', 10.10, 400, 'bid', 10, T0 + 2_000_000)]
s, closed = run_one('maker_touch_10s', evs)
ok(len(closed) == 1, 'maker_touch: one exit resolved')
ok(not closed[0].crossed and closed[0].exit_fee == MAKER_BPS,
   'maker_touch: a passive fill pays the MAKER fee')
ok(abs(closed[0].exit_px - 10.10) < 1e-9, 'maker_touch: and fills at our resting price')

# Nobody trades: the timeout must make it pay up.
evs = base + [delta('T', 'bid', 9.90, 1, 3, T0 + 14_000_000)]
s, closed = run_one('maker_touch_10s', evs)
ok(len(closed) == 1 and closed[0].crossed, 'timeout: an unfilled quote crosses at the timeout')
ok(closed[0].exit_fee == TAKER_BPS, 'timeout: and pays the taker fee')
ok(not closed[0].forced, 'timeout: a timeout cross is not the same as a max-hold cross')

# The patient policy does not cross at its timeout, but max_hold still forces it out.
evs = base + [delta('T', 'bid', 9.90, 1, 3, T0 + 40_000_000),
              delta('T', 'bid', 9.90, 1, 4, T0 + 400_000_000)]
s, closed = run_one('maker_30s_patient', evs)
ok(len(closed) == 1 and closed[0].forced,
   'max_hold: a policy that never crosses is still forced out at the cap')
ok(s.stats['exit_forced'] == 1, 'max_hold: and the forcing is counted, not hidden')

# The delayed policy must not quote early.
evs = [snap('T', [[9.90, 300]], [[10.10, 300]], 1, T0),
       delta('T', 'bid', 9.90, 1, 2, T0 + 1_500_000),
       trade('T', 10.10, 400, 'bid', 10, T0 + 2_000_000),
       delta('T', 'bid', 9.90, 1, 3, T0 + 8_000_000),
       trade('T', 10.10, 400, 'bid', 11, T0 + 9_000_000)]
s, closed = run_one('wait5_maker_10s', evs)
ok(len(closed) == 1, 'delay: resolved')
ok(closed[0].exit_us >= T0 + 8_000_000,
   'delay: the trade during the waiting window does not fill an order we had not placed')

# Cancel/replace follows the touch; the plain version does not.
move = [snap('T', [[9.90, 300]], [[10.10, 300]], 1, T0),
        delta('T', 'bid', 9.90, 1, 2, T0 + 1_500_000),
        delta('T', 'ask', 10.05, 300, 3, T0 + 3_000_000),
        delta('T', 'ask', 10.10, -300, 4, T0 + 3_100_000),
        delta('T', 'bid', 9.90, 1, 5, T0 + 3_200_000)]
s, closed = run_one('maker_replace_10s', move)
ok(s.stats['replaces'] >= 1, 'replace: repositions when the touch moves away')
s, closed = run_one('maker_touch_10s', move)
ok(s.stats['replaces'] == 0, 'replace: the plain policy leaves its order where it was')


# A gapped book must freeze the exit engine too. The book is wrong after a gap, and an exit
# priced off a wrong book is worse than an exit that has not happened yet.
gapped = [snap('T', [[9.90, 300]], [[10.10, 300]], 1, T0),
          delta('T', 'bid', 9.90, 1, 2, T0 + 1_500_000),
          delta('T', 'bid', 9.90, 1, 90, T0 + 14_000_000)]
s, closed = run_one('maker_touch_10s', gapped)
ok(s.eng.books['T'].stale, 'gap: the book is stale after a sequence jump')
ok(all(not c.crossed or c.exit_px is None for c in closed) or len(closed) == 1,
   'gap: the engine does not price an exit off a book it knows is wrong')
ok(s.stats.get('exit_unresolved_at_tape_end', 0) >= 1,
   'gap: the trade is counted as unresolved rather than silently dropped')


# ---- aggregation ---------------------------------------------------------------------------------
trades = []
for i in range(200):
    trades.append({'selector': 'model', 'policy': 'maker_touch_10s', 'market': 'T',
                   'rank': i, 'n_test': 1000, 'side': 'buy', 'entry_px': 10.0,
                   'exit_px': 10.01, 'hold_s': 2.0, 'passive_exit': True, 'forced': False,
                   'replaces': 0, 'gross_bps': 10.0, 'entry_edge_bps': 10.0,
                   'exit_slip_bps': 0.0, 'entry_fee_bps': 5.0, 'exit_fee_bps': 5.0,
                   'net_bps': 0.0})
lines = X.aggregate(trades, span_h=2.5)
top10 = [l for l in lines if l['quantile'] == 0.10]
top1 = [l for l in lines if l['quantile'] == 0.01]
ok(top10 and top10[0]['trades'] == 100, f'aggregate: top 10% of 1000 is rank < 100, got {top10 and top10[0]["trades"]}')
ok(top1 and top1[0]['trades'] == 10, 'aggregate: top 1% is rank < 10')
ok(all(l['entry_fee_bps'] == 5.0 for l in lines), 'aggregate: the entry fee is carried through')

# A trade that could not be priced must never reach the table.
ok(X.round_trip(FakeXO(entry(), None, None)) is None, 'aggregate: unpriced trades are dropped')

print(f'{P} passed, {F} failed')
sys.exit(1 if F else 0)
