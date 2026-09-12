#!/usr/bin/env python3
"""Unit tests for the streaming feature engine. No network, no recorder, no files.

Every case here is one the engine is supposed to survive, and most of them are cases that would
silently produce a wrong number rather than an error if the engine got them wrong. That is the
class of bug this suite exists for: not crashes, but plausible-looking output built on a book
that never existed.
"""
import os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(ROOT, 'scripts', 'feature_engine.py')).read()
src = src.split("if __name__ == '__main__':")[0]
E = {'__file__': 'fe', '__name__': 'fe'}
exec(compile(src, 'fe', 'exec'), E)
Book, Flow, Window, Feed = E['Book'], E['Flow'], E['Window'], E['Feed']
FeatureEngine, to_us, num = E['FeatureEngine'], E['to_us'], E['num']

checks = fails = 0
def check(c, what):
    global checks, fails
    checks += 1
    if not c:
        fails += 1
        print('FAIL: ' + what)

T0 = 1_789_000_000_000_000          # a plausible epoch in microseconds
def ev(**kw):
    d = {'recv_ts_us': T0}
    d.update(kw)
    return d
def snap(t, bids, asks, seq=1, ts=T0, sid=1):
    return ev(ev='orderbook_snapshot', seq=seq, sid=sid, src_ts_us=ts,
              msg={'market_ticker': t, 'bid': bids, 'ask': asks})
def delta(t, price, d, side, seq, reason='Decrease', ts=T0, sid=1):
    return ev(ev='orderbook_delta', seq=seq, sid=sid, src_ts_us=ts,
              msg={'market_ticker': t, 'price': str(price), 'delta': str(d),
                   'side': side, 'last_update_reason': reason})
def trade(t, price, count, taker_side, seq, ts=T0, sid=3):
    return ev(ev='trade', seq=seq, sid=sid, src_ts_us=ts,
              msg={'market_ticker': t, 'price': str(price), 'count': str(count),
                   'taker_side': taker_side})

# --- 1. units, and failing loudly on a mismatch ------------------------------------------------
check(to_us(1789000000, 's') == to_us(1789000000000, 'ms') == to_us(1789000000000000, 'us'),
      'all units converge on the same microsecond')
try:
    to_us(1, 'fortnights'); check(False, 'an unknown unit must raise')
except ValueError:
    check(True, 'an unknown unit raises rather than guessing')
try:
    E['check_epoch'](1789000000, 'seconds-read-as-us')       # seconds mistaken for microseconds
    check(False, 'a seconds value read as microseconds must raise')
except E['UnitMismatch']:
    check(True, 'a ms/s mismatch fails loudly, not silently')
check(E['check_epoch'](T0, 'ok') == T0, 'a real microsecond timestamp passes')
check(num('922337203685477.6') == 0.0, 'the INT64_MAX no-order sentinel is not a price')
check(num('2.4426') == 2.4426 and num('x') == 0.0, 'prices parse, garbage does not')

# --- 2. a clean book ---------------------------------------------------------------------------
b = Book('KXT')
b.snapshot([['1.00', '10'], ['0.99', '20']], [['1.02', '5'], ['1.03', '30']], 1, T0)
check(b.usable(), 'a two-sided book is usable')
check(abs(b.mid - 1.01) < 1e-9, f'mid is 1.01, got {b.mid}')
check(abs(b.spread_bps - (0.02 / 1.01 * 1e4)) < 1e-6, 'spread in bps')
check(abs(b.obi(1) - (10 - 5) / 15) < 1e-9, f'L1 OBI, got {b.obi(1)}')
check(abs(b.obi(2) - (30 - 35) / 65) < 1e-9, f'L2 OBI, got {b.obi(2)}')
# microprice leans toward the THIN side: ask size 5 < bid size 10, so it sits above the mid
check(b.microprice > b.mid, f'microprice leans to the thin ask, got {b.microprice} vs {b.mid}')
check(b.queue_ahead('1.00', 'bid') == 10, 'queue ahead is the size resting at that price')

# --- 3. sequence continuity is PER SUBSCRIPTION, not per market ---------------------------------
# The Kalshi margin socket interleaves every market on one sid with one shared counter, so a
# market's own sequence legitimately jumps. Checking it per market flagged 41,799 gaps in 97,940
# real BTC deltas, all of them false.
eng = FeatureEngine()
eng.on_event('kalshi_margin', snap('A', [['1.00', '10']], [['1.02', '10']], seq=100))
eng.on_event('kalshi_margin', snap('B', [['2.00', '10']], [['2.02', '10']], seq=101))
eng.on_event('kalshi_margin', delta('A', '1.00', '-1', 'bid', 102))
eng.on_event('kalshi_margin', delta('B', '2.00', '-1', 'bid', 103))
eng.on_event('kalshi_margin', delta('A', '1.00', '-1', 'bid', 104))
check(eng.events['sid_gap'] == 0,
      "interleaved markets on one sid are not a gap (A jumped 102->104)")
check(not eng.books['A'].stale and not eng.books['B'].stale, 'and no book goes stale')
check(abs(eng.books['A'].bids[1.00] - 8) < 1e-9, 'both A deltas applied')

# A true gap on the shared sid: every market on it is suspect, so they all go stale together.
eng.on_event('kalshi_margin', delta('A', '1.00', '-1', 'bid', 200))
check(eng.events['sid_gap'] == 1, 'a real jump on the sid counter is a gap')
check(eng.books['A'].stale and eng.books['B'].stale,
      'every market on the subscription goes stale, not just the one that carried the frame')
check(eng.books['B'].gap_count == 1, 'the gap is counted against every book on the sid')
eng.on_event('kalshi_margin', delta('B', '2.00', '-1', 'bid', 201))
check(abs(eng.books['B'].bids[2.00] - 9) < 1e-9,
      'a stale book stops applying deltas rather than silently patching over the hole')

# This feed never re-snapshots mid-stream, so only a reconnect clears it.
eng.on_event('kalshi_margin', {'ev': '_connect', 'recv_ts_us': T0 + 1_000_000})
check(eng.connect_us == T0 + 1_000_000, 'a connect is recorded')
check(eng.books['A'].bids == {} and eng.books['A'].seq is None,
      'a reconnect empties every book: the venue restarts the stream from nothing')
check(eng.sid_seq == {}, 'and resets every subscription counter, which restarts at 1')
eng.on_event('kalshi_margin', snap('A', [], [], seq=1, ts=T0 + 1_000_000))
check(not eng.books['A'].stale, 'the empty snapshot after a reconnect starts the book again')
check(not eng.books['A'].usable(), 'but an empty book is not usable until deltas arrive')

# --- 3b. warm-up after a connect ----------------------------------------------------------------
eng2 = FeatureEngine()
eng2.on_event('kalshi_margin', {'ev': '_connect', 'recv_ts_us': T0})
eng2.on_event('kalshi_margin', snap('A', [], [], seq=1, ts=T0))
eng2.on_event('kalshi_margin', delta('A', '1.00', '10', 'bid', 2, ts=T0 + 1000))
eng2.on_event('kalshi_margin', delta('A', '1.02', '10', 'ask', 3, ts=T0 + 1000))
check(eng2.books['A'].usable(), 'the book is two-sided within a millisecond of the deltas')
check(eng2.row('A', T0 + 2_000_000) is None, 'but no row is emitted during warm-up')
check(eng2.suppressed['book_warming_up'] == 1, 'and the suppression is counted')
# Keep the book fresh: a 90-second-old book is stale on its own terms and must stay suppressed.
check(eng2.row('A', T0 + 90_000_000) is None, 'a book nobody updated for 90s is stale, warm or not')
check(eng2.suppressed['kalshi_stale'] == 1, 'and that is what the suppression says')
eng2.on_event('kalshi_margin', delta('A', '1.00', '1', 'bid', 4, ts=T0 + 90_000_000))
r = eng2.row('A', T0 + 90_000_000)
check(r is not None, 'a row is emitted once the book has had time to fill')
check(r and abs(r['since_connect_s'] - 90.0) < 1e-6,
      'and it carries its own age so a study can demand a longer warm-up')
check(r and r['n_bid_levels'] == 1 and r['n_ask_levels'] == 1,
      'and the level counts, so a thin reconstruction is visible rather than assumed away')

# --- 3a2. a connect is scoped to the socket that sent it ----------------------------------------
# Every recorded source writes its own `_connect`. Treating them alike meant a Coinbase reconnect
# wiped the Kalshi books: eleven times in 400,000 events, leaving every book permanently stale and
# producing zero orders with no error anywhere.
engC = FeatureEngine()
engC.on_event('kalshi_margin', {'ev': '_connect', 'recv_ts_us': T0})
engC.on_event('kalshi_margin', snap('A', [['1.00', '10']], [['1.02', '10']], seq=1, ts=T0))
engC.on_event('kalshi_index', ev(ev='cfbenchmarks_value_5hz', src_ts_us=T0,
                                msg={'index_id': 'BRTI', 'value_usd': 100.0}))
engC.on_event('ext_coinbase', ev(ev='ticker', src_ts_us=T0,
                                 msg={'product_id': 'BTC-USD', 'price': 100.0}))
check(engC.books['A'].usable(), 'connect scope: the Kalshi book is built')
check(engC.cf['BRTI'].price == 100.0, 'connect scope: the index feed has a price')
engC.on_event('ext_coinbase', {'ev': '_connect', 'recv_ts_us': T0 + 1_000_000})
check(engC.books['A'].usable(),
      'connect scope: a Coinbase reconnect does NOT empty the Kalshi order book')
check(engC.cf['BRTI'].price == 100.0, 'connect scope: nor the index feed')
check(engC.ext['coinbase:BTC-USD'].price is None,
      'connect scope: it DOES clear the Coinbase feed, whose history is now discontinuous')
engC.on_event('kalshi_index', {'ev': '_connect', 'recv_ts_us': T0 + 2_000_000})
check(engC.cf['BRTI'].price is None, 'connect scope: an index reconnect clears the index feed')
check(engC.books['A'].usable(), 'connect scope: and still leaves the order book alone')
engC.on_event('kalshi_margin', {'ev': '_connect', 'recv_ts_us': T0 + 3_000_000})
check(not engC.books['A'].usable(), 'connect scope: a MARGIN reconnect does empty the book')
check(engC.connect_us == T0 + 3_000_000,
      'connect scope: and only a margin connect moves the warm-up clock')

# --- 3b2. a FULL snapshot needs no warm-up -------------------------------------------------------
# The first connect of the recorded session returned empty snapshots; the second returned real
# depth (BTC: 245 bid levels). Charging a warm-up to a book that arrived complete throws away
# good rows, so the gate is scoped to books that actually had to be rebuilt.
engF = FeatureEngine()
engF.on_event('kalshi_margin', {'ev': '_connect', 'recv_ts_us': T0})
engF.on_event('kalshi_margin', snap('A', [['1.00', '10'], ['0.99', '8']],
                                    [['1.02', '10'], ['1.03', '9']], seq=1, ts=T0))
check(not engF.books['A'].empty_snapshot, 'a snapshot carrying levels is not an empty one')
engF.on_event('kalshi_margin', delta('A', '1.00', '1', 'bid', 2, ts=T0 + 1_000_000))
r = engF.row('A', T0 + 1_000_000)
check(r is not None, 'a complete snapshot is usable one second after the connect')
check(engF.suppressed['book_warming_up'] == 0, 'and no warm-up is charged against it')
check(r and r['n_bid_levels'] == 2, 'the full depth is there from the snapshot')
# A reconnect returns it to the unproven state until the next snapshot says otherwise.
engF.on_event('kalshi_margin', {'ev': '_connect', 'recv_ts_us': T0 + 2_000_000})
check(engF.books['A'].empty_snapshot,
      'a reconnect resets the book to unproven until its next snapshot arrives')

# --- 3c. the trade channel, because this feed sends no last_update_reason -----------------------
eng3 = FeatureEngine()
eng3.on_event('kalshi_margin', {'ev': '_connect', 'recv_ts_us': T0})
eng3.on_event('kalshi_margin', snap('A', [], [], seq=1, ts=T0))
eng3.on_event('kalshi_margin', delta('A', '1.00', '100', 'bid', 2, reason='', ts=T0))
eng3.on_event('kalshi_margin', delta('A', '1.02', '100', 'ask', 3, reason='', ts=T0))
# taker_side 'bid' is an aggressive BUYER: it lifts the offer. The trade channel is sid 3 with
# its own counter, so its sequence numbers are unrelated to the book's.
eng3.on_event('kalshi_margin', trade('A', '1.02', '20', 'bid', 500, ts=T0 + 1_000_000))
now = T0 + 1_100_000
check(eng3.flow['A'].trade_buy['1s'].sum(now) == 20,
      "taker_side='bid' is aggressive BUYING, established from the data not the docs")
check(eng3.flow['A'].trade_sell['1s'].sum(now) == 0, 'and nothing lands on the sell side')
# The delta that carries that same fill must NOT also be counted as a cancel.
eng3.on_event('kalshi_margin', delta('A', '1.02', '-20', 'ask', 4, reason='',
                                     ts=T0 + 1_010_000))
check(eng3.flow['A'].cancel_ask['1s'].sum(now) == 0,
      'the delta matching a trade print is not double-counted as a cancel')
check(eng3.trade_match['delta_matched_to_trade'] == 1, 'and the match is counted')
# An unmatched negative delta IS a cancel.
eng3.on_event('kalshi_margin', delta('A', '1.00', '-30', 'bid', 5, reason='',
                                     ts=T0 + 1_020_000))
check(eng3.flow['A'].cancel_bid['1s'].sum(T0 + 1_100_000) == 30,
      'a negative delta with no trade print at that price is a cancel')
# A delta far outside the matching window is a cancel even at a traded price.
eng3.on_event('kalshi_margin', delta('A', '1.02', '-10', 'ask', 6, reason='',
                                     ts=T0 + 5_000_000))
check(eng3.flow['A'].cancel_ask['10s'].sum(T0 + 5_100_000) == 10,
      'the trade match is time-boxed: a late delta at a traded price is still a cancel')
# A feed that DOES publish reasons must still be believed over the matcher.
eng4 = FeatureEngine()
eng4.on_event('kalshi_margin', snap('A', [['1.00', '100']], [['1.02', '100']], seq=1))
eng4.on_event('kalshi_margin', delta('A', '1.00', '-5', 'bid', 2, reason='Trade'))
check(eng4.flow['A'].trade_sell['1s'].sum(T0 + 1000) == 5,
      'an explicit last_update_reason is authoritative where the feed sends one')

# --- 4. empty sides and a crossed book ----------------------------------------------------------
b = Book('KXT'); b.snapshot([], [['1.02', '5']], 1, T0)
check(not b.usable() and b.mid is None, 'an empty bid side is not usable and has no mid')
b.snapshot([['1.00', '5']], [], 1, T0)
check(not b.usable() and b.mid is None, 'an empty ask side is not usable')
b.snapshot([['1.05', '5']], [['1.02', '5']], 1, T0)
check(b.crossed and not b.usable(), 'a crossed book is not a tradable state')
check(b.mid is None, 'and yields no midpoint rather than a nonsense one')

# --- 5. trade vs cancel classification ----------------------------------------------------------
f = Flow()
f.on_delta(T0, 'bid', -5, 'Trade')            # someone SOLD into the bid
f.on_delta(T0, 'ask', -3, 'Trade')            # someone BOUGHT from the ask
f.on_delta(T0, 'bid', -7, 'MarginCancel')     # a bid cancelled
f.on_delta(T0, 'ask', +2, 'Amend')            # liquidity added to the ask
now = T0 + 100_000
check(f.trade_sell['1s'].sum(now) == 5, 'a trade hitting the bid is aggressive selling')
check(f.trade_buy['1s'].sum(now) == 3, 'a trade lifting the ask is aggressive buying')
check(f.cancel_bid['1s'].sum(now) == 7, 'a cancel on the bid is a cancel, not a trade')
check(f.add_ask['1s'].sum(now) == 2, 'a positive delta is an add')
check(f.cancel_ask['1s'].sum(now) == 0, 'an Amend that adds is not counted as a cancel')
check(abs(Flow.imbalance(f.trade_buy['1s'], f.trade_sell['1s'], now) - (3 - 5) / 8) < 1e-9,
      'trade imbalance is signed and normalised')
check(abs(Flow.imbalance(f.cancel_bid['1s'], f.cancel_ask['1s'], now) - 1.0) < 1e-9,
      'cancel imbalance saturates at 1 when only one side cancels')

# --- 6. windows evict, and a zero-volume interval reads zero rather than stale -------------------
w = Window(1_000_000)
w.add(T0, 5.0); w.add(T0 + 500_000, 5.0)
check(w.sum(T0 + 600_000) == 10.0, 'both observations are inside the window')
# at +1.2s the window covers [0.2s, 1.2s]: the entry at 0s is out, the one at 0.5s is in
check(w.sum(T0 + 1_200_000) == 5.0, 'the older one is evicted, the newer kept')
check(w.sum(T0 + 1_600_000) == 0.0, 'at +1.6s the window has moved past both')
check(w.sum(T0 + 3_000_000) == 0.0, 'a zero-volume interval reads 0, not the last value')
check(w.count(T0 + 3_000_000) == 0, 'and the count agrees')
# Writes alone must bound the structure. A consumer that never reads (the execution simulator
# needs the book, not the flow) would otherwise retain every entry until the process is killed
# by the OOM reaper, with no error and no output.
w2 = Window(1_000_000)
for i in range(50000):
    w2.add(T0 + i * 1000, 1.0)
check(len(w2.q) <= 1001, f'a write-only window stays bounded, got {len(w2.q)} entries')
check(abs(w2.sum(T0 + 49999 * 1000) - len(w2.q)) < 1e-9,
      'and the running total still agrees with what it holds')

# --- 7. out-of-order and duplicate frames --------------------------------------------------------
eng = FeatureEngine()
eng.on_event('kalshi_margin', snap('KXT', [['1.00', '10']], [['1.02', '10']], seq=100))
eng.on_event('kalshi_margin', delta('KXT', '1.00', '-1', 'bid', 101))
before = eng.books['KXT'].bids[1.00]
eng.on_event('kalshi_margin', delta('KXT', '1.00', '-1', 'bid', 101))    # duplicate
check(eng.books['KXT'].bids[1.00] == before, 'a duplicate frame is not applied twice')
check(eng.events['duplicate_seq'] == 1, 'and is counted')
eng.on_event('kalshi_margin', delta('KXT', '1.00', '-1', 'bid', 99))     # late
check(eng.books['KXT'].bids[1.00] == before, 'an out-of-order frame never rewinds the book')
check(eng.events['out_of_order'] == 1, 'and is counted')
check(eng.events['sid_gap'] == 0, 'neither is miscounted as a gap')

# --- 8. trade depletion clears a level ------------------------------------------------------------
b = Book('KXT'); b.snapshot([['1.00', '10'], ['0.99', '7']], [['1.02', '5']], 1, T0)
b.delta('1.00', '-10', 'bid', 2, T0, )
check(1.00 not in b.bids, 'a level fully consumed is removed, not left at zero')
check(b.best_bid == 0.99, 'and the touch moves to the next level')

# --- 9. cancels ahead of and behind a resting order ----------------------------------------------
b = Book('KXT'); b.snapshot([['1.00', '10'], ['0.99', '4']], [['1.02', '5']], 1, T0)
check(b.queue_ahead('1.00', 'bid') == 10, 'queue ahead at the touch')
b.delta('1.00', '-6', 'bid', 2, T0)
check(b.queue_ahead('1.00', 'bid') == 4, 'a cancel ahead reduces the queue we sit behind')
b.delta('0.99', '-2', 'bid', 3, T0)
check(b.queue_ahead('1.00', 'bid') == 4, 'a cancel BEHIND us at a worse price does not')

# --- 10. staleness suppresses the row rather than forward-filling ---------------------------------
eng = FeatureEngine()
eng.on_event('kalshi_margin', snap('KXBTCPERP', [['7.90', '100']], [['7.91', '100']], seq=1))
eng.on_event('kalshi_index', ev(ev='cfbenchmarks_value_5hz', src_ts_us=T0,
                                msg={'index_id': 'BRTI', 'value_usd': '79050.0'}))
r = eng.row('KXBTCPERP', T0 + 1_000_000)
check(r is not None, 'a fresh book and fresh CF produce a row')
check(r['cf_basis_bps'] is not None, 'and a CF basis')
# A perp price is dollars per contract and an index is dollars per underlying unit. BTC at 7.905
# with contract_size 0.0001 is an underlying of 79,050, which against an index of 79,050 is a
# basis of ZERO. Without the scaling this reads -9999 bps on every single row.
check(abs(r['cf_basis_bps']) < 1.0,
      f"basis is scaled by contract_size, got {r['cf_basis_bps']:.1f} bps (unscaled would be -9999)")
check(abs(r['underlying_px'] - 79050.0) < 1.0,
      f"underlying price reconstructed, got {r.get('underlying_px')}")
check(r['cf_age_us'] == 1_000_000, f"carrying the CF age, got {r['cf_age_us']}")
# keep the BOOK fresh so CF staleness is the thing under test, not book staleness
eng.on_event('kalshi_margin', snap('KXBTCPERP', [['7.90', '100']], [['7.91', '100']],
                                   seq=2, ts=T0 + 29_000_000))
r2 = eng.row('KXBTCPERP', T0 + 30_000_000)      # book 1s old, CF now 30s old past its 10s limit
check(r2 is None, 'a stale CF suppresses the row instead of forward-filling')
check(eng.suppressed['cf_stale'] == 1, 'and the suppression is counted by reason')

eng2 = FeatureEngine()
eng2.on_event('kalshi_margin', snap('KXBTCPERP', [['7.90', '100']], [['7.91', '100']], seq=1))
r3 = eng2.row('KXBTCPERP', T0 + 60_000_000)     # book itself 60s old
check(r3 is None, 'a stale BOOK suppresses the row')
check(eng2.suppressed['kalshi_stale'] == 1, 'counted as a book staleness')

# --- 11. never compute through a gap --------------------------------------------------------------
eng3 = FeatureEngine()
eng3.on_event('kalshi_margin', snap('KXT', [['1.00', '10']], [['1.02', '10']], seq=5))
check(eng3.row('KXT', T0 + 100_000) is not None, 'row before the gap')
eng3.on_event('kalshi_margin', ev(ev='_gap', msg={'market_ticker': 'KXT'}))
check(eng3.row('KXT', T0 + 200_000) is None, 'no row after a gap event')
check(eng3.suppressed['book_stale_or_gapped'] >= 1, 'suppressed for the right reason')

# --- 12. external feeds: age, and no invented return -----------------------------------------------
fd = Feed('coinbase:BTC-USD')
fd.update(T0, 79000.0)
fd.update(T0 + 5_000_000, 79100.0)
check(fd.age_us(T0 + 6_000_000) == 1_000_000, 'feed age is measured, not assumed')
r5 = fd.ret_bps(T0 + 5_000_000, 5_000_000)
check(r5 is not None and abs(r5 - (79100 - 79000) / 79000 * 1e4) < 1e-6,
      f'a 5s return over exactly 5s of history, got {r5}')
check(fd.ret_bps(T0 + 5_000_000, 3600_000_000) is None,
      'a 1-hour return is None when there is no hour of history, not the oldest point')

# --- 13. a clock jump backwards must not produce a negative window ----------------------------------
w2 = Window(1_000_000)
w2.add(T0, 3.0)
check(w2.sum(T0 - 10_000_000) == 3.0, 'a backwards clock does not evict into a negative sum')
check(w2.sum(T0 + 10_000_000) == 0.0, 'and forwards still evicts correctly')

# --- 14. partial vs full consumption at the touch ---------------------------------------------------
b = Book('KXT'); b.snapshot([['1.00', '10']], [['1.02', '10']], 1, T0)
b.delta('1.00', '-4', 'bid', 2, T0, )
check(abs(b.bids[1.00] - 6.0) < 1e-9, 'partial consumption leaves the remainder')
b.delta('1.00', '-6', 'bid', 3, T0)
check(1.00 not in b.bids, 'full consumption removes the level')
check(not b.usable(), 'and with no bid left the book is not usable')

print(f'{checks} checks, {fails} failures')
sys.exit(1 if fails else 0)
