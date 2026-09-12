#!/usr/bin/env python3
"""Tests for the execution simulator.

Each test builds a book and an event sequence whose answer is known by hand, because a fill
simulator that is merely plausible is worse than none: it produces a confident cost number that
no one can check. Everything here is checked against arithmetic done on paper.
"""
import gzip, json, os, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'scripts'))

import execution_sim as X
from feature_engine import Book

P = F = 0
def ok(cond, label):
    global P, F
    if cond:
        P += 1
    else:
        F += 1
        print(f'  FAIL {label}')

T0 = 1_789_000_000_000_000


def ev(ticker, typ, seq, ts, sid=None, **msg):
    m = {'market_ticker': ticker}
    m.update(msg)
    # sid defaults to the real feed's layout: book on 1, trades on 3, each with its own counter.
    if sid is None:
        sid = 3 if typ == 'trade' else 1
    return {'ev': typ, 'seq': seq, 'sid': sid, 'src_ts_us': ts, 'recv_ts_us': ts, 'msg': m}


def trade_ev(ticker, price, count, taker_side, seq, ts):
    return ev(ticker, 'trade', seq, ts, price=price, count=count, taker_side=taker_side)


def snap(ticker, bids, asks, seq=1, ts=T0):
    return ev(ticker, 'orderbook_snapshot', seq, ts, bid=bids, ask=asks)


def delta(ticker, side, price, d, seq, ts, reason=''):
    return ev(ticker, 'orderbook_delta', seq, ts, side=side, price=price, delta=d,
              last_update_reason=reason)


# ---- tick sizer ------------------------------------------------------------------------------
def test_ticks():
    ts = X.TickSizer()
    ok(ts.tick('A') is None, 'tick: None before enough observations')
    for p in [7.90, 7.91, 7.92, 7.93, 7.94, 7.95, 7.96, 7.97]:
        ts.observe('A', p)
    ok(abs(ts.tick('A') - 0.01) < 1e-9, 'tick: 0.01 grid inferred')

    ts2 = X.TickSizer()
    for p in [100.0, 100.5, 101.0, 101.5, 102.0, 102.5, 103.0, 103.5]:
        ts2.observe('B', p)
    ok(abs(ts2.tick('B') - 0.5) < 1e-9, 'tick: 0.5 grid inferred')

    # One anomalous adjacent pair must not become the grid.
    ts3 = X.TickSizer()
    for p in [1.00, 1.0001, 1.01, 1.02, 1.03, 1.04, 1.05, 1.06, 1.07]:
        ts3.observe('C', p)
    ok(abs(ts3.tick('C') - 0.0001) > 1e-9, 'tick: single anomalous gap rejected as the grid')

    ts4 = X.TickSizer()
    ts4.observe('D', 0); ts4.observe('D', None)
    ok(ts4.tick('D') is None, 'tick: zero and None ignored')


# ---- book walking ----------------------------------------------------------------------------
def test_walk():
    b = Book('T')
    b.snapshot([[9.0, 10], [8.9, 20]], [[10.0, 5], [10.1, 5], [10.2, 100]], 1, T0)

    vwap, got, n = X.walk_book(b, 'buy', 5)
    ok(abs(vwap - 10.0) < 1e-9 and abs(got - 5) < 1e-9 and n == 1, 'walk: one level exact')

    vwap, got, n = X.walk_book(b, 'buy', 10)
    ok(abs(vwap - 10.05) < 1e-9 and n == 2, 'walk: VWAP across two levels = 10.05')

    vwap, got, n = X.walk_book(b, 'sell', 25)
    # 10@9.00 + 15@8.90 = 90 + 133.5 = 223.5 / 25 = 8.94
    ok(abs(vwap - 8.94) < 1e-9 and abs(got - 25) < 1e-9, 'walk: sell VWAP 8.94')

    vwap, got, n = X.walk_book(b, 'sell', 1000)
    ok(abs(got - 30) < 1e-9, 'walk: partial fill reported, not padded')

    empty = Book('E')
    empty.snapshot([], [], 1, T0)
    ok(X.walk_book(empty, 'buy', 1) == (None, 0.0, 0), 'walk: empty book fills nothing')


# ---- sign convention -------------------------------------------------------------------------
def test_signed():
    buy = X.Order('join', 'T', 'buy', 10.0, 1, T0, 10.0, 0, 1, 5.0)
    sell = X.Order('join', 'T', 'sell', 10.0, 1, T0, 10.0, 0, 1, 5.0)
    ok(buy.signed(3.0) == 3.0, 'sign: buy keeps sign')
    ok(sell.signed(3.0) == -3.0, 'sign: sell flips sign')


# ---- queue bounds ----------------------------------------------------------------------------
def run(events, **kw):
    kw.setdefault('notional', 10.0)
    kw.setdefault('sample_s', 1e9)       # sampling off unless a test wants it
    kw.setdefault('timeout_s', 1e6)
    kw.setdefault('max_size_frac', 1.0)
    kw.setdefault('markets', ['T'])
    s = X.Sim(**kw)
    for e in events:
        s.feed('kalshi_margin', e)
    return s


def test_queue_bounds():
    s = run([snap('T', [[9.0, 100]], [[10.0, 100]], 1, T0)])
    o = X.Order('join', 'T', 'buy', 9.0, 1.0, T0, 9.5, 100.0, 10_000_000, 105.0)
    s.open['T'].append(o)

    # 60 traded away at our price: both bounds advance by 60.
    s.feed('kalshi_margin', delta('T', 'bid', 9.0, -60, 2, T0 + 1_000_000, 'Trade'))
    ok(abs(o.q_pess - 40) < 1e-9, 'queue: trade advances pessimistic bound')
    ok(abs(o.q_opt - 40) < 1e-9, 'queue: trade advances optimistic bound')

    # 30 cancelled: pessimistic does not move; optimistic is re-anchored to the surviving 10.
    s.feed('kalshi_margin', delta('T', 'bid', 9.0, -30, 3, T0 + 2_000_000, 'MarginCancel'))
    ok(abs(o.q_pess - 40) < 1e-9, 'queue: cancel does NOT advance pessimistic bound')
    ok(abs(o.q_opt - 10) < 1e-9, 'queue: cancel re-anchors optimistic bound to level qty')
    ok(o.fill_pess_us is None, 'queue: no pessimistic fill yet')

    # Exactly the remaining 10 trade. That empties the queue ahead of us on the optimistic
    # bound but leaves nothing over, so we are at the FRONT, not filled.
    s.feed('kalshi_margin', delta('T', 'bid', 9.0, -10, 4, T0 + 3_000_000, 'Trade'))
    ok(o.fill_opt_us is None, 'queue: exhausting the queue exactly is not a fill')
    ok(abs(o.q_opt) < 1e-9, 'queue: optimistic queue now empty')
    ok(o.q_opt <= o.q_pess + 1e-9, 'queue: optimistic bound never exceeds pessimistic')

    # 5 more trade with nothing ahead on the optimistic bound: that volume reaches us.
    s.feed('kalshi_margin', delta('T', 'bid', 9.0, -5, 5, T0 + 4_000_000, 'Trade'))
    ok(o.fill_opt_us == T0 + 4_000_000, 'queue: optimistic fills first')
    ok(o.fill_pess_us is None, 'queue: pessimistic still unfilled -- bounds genuinely differ')

    # A cancel that wipes the level does NOT fill us; it only puts us at the front.
    s2 = run([snap('T', [[9.0, 100]], [[10.0, 100]], 1, T0)])
    o2 = X.Order('join', 'T', 'buy', 9.0, 1.0, T0, 9.5, 100.0, 10_000_000, 105.0)
    s2.open['T'].append(o2)
    s2.feed('kalshi_margin', delta('T', 'bid', 9.0, -100, 2, T0 + 1_000_000, 'MarginCancel'))
    ok(o2.fill_opt_us is None and o2.fill_pess_us is None,
       'queue: a level cancelled out from under us is not a fill on either bound')


def test_price_priority():
    s = run([snap('T', [[9.90, 200]], [[10.10, 200]], 1, T0)])
    inside = X.Order('improve', 'T', 'buy', 10.00, 1.0, T0, 10.0, 0.0, 10_000_000, 200.0)
    behind = X.Order('join', 'T', 'buy', 9.90, 1.0, T0, 10.0, 200.0, 10_000_000, 200.0)
    s.open['T'] += [inside, behind]
    # An aggressor sells at 9.90. Our 10.00 bid would have been hit first, at our price.
    s.feed('kalshi_margin', delta('T', 'bid', 9.90, -10, 2, T0 + 1_000_000, 'Trade'))
    ok(inside.fill_pess_us == T0 + 1_000_000,
       'priority: a print at a worse price fills our better-priced bid')
    ok(inside.fill_px == 10.00, 'priority: we fill at OUR price, not the print price')
    ok(behind.fill_pess_us is None, 'priority: the order at the print price still queues')

    s2 = run([snap('T', [[9.90, 200]], [[10.10, 200]], 1, T0)])
    sell = X.Order('improve', 'T', 'sell', 10.00, 1.0, T0, 10.0, 0.0, 10_000_000, 200.0)
    s2.open['T'].append(sell)
    s2.feed('kalshi_margin', delta('T', 'ask', 10.10, -10, 2, T0 + 1_000_000, 'Trade'))
    ok(sell.fill_pess_us == T0 + 1_000_000,
       'priority: a buy printing at 10.10 fills our better-priced 10.00 offer')


def test_cross_fills():
    s = run([snap('T', [[9.0, 100]], [[10.0, 100]], 1, T0)])
    o = X.Order('join', 'T', 'buy', 9.0, 1.0, T0, 9.5, 100.0, 10_000_000, 105.0)
    s.open['T'].append(o)
    # The ask collapses to our bid. A real resting bid would have been hit.
    s.feed('kalshi_margin', delta('T', 'ask', 9.0, 50, 2, T0 + 500_000, ''))
    ok(o.fill_pess_us == T0 + 500_000 and o.fill_opt_us == T0 + 500_000,
       'cross: book trading through our price fills both bounds')
    ok(o.done, 'cross: order marked done')


def test_left_touch():
    s = run([snap('T', [[9.0, 100]], [[10.0, 100]], 1, T0)])
    o = X.Order('join', 'T', 'buy', 9.0, 1.0, T0, 9.5, 100.0, 10_000_000, 105.0)
    s.open['T'].append(o)
    s.feed('kalshi_margin', delta('T', 'bid', 9.5, 40, 2, T0 + 500_000, ''))
    ok(o.left_touch and not o.done, 'touch: a better bid leaves us behind but does not fill us')


def test_timeout_and_cross():
    base = [snap('T', [[9.0, 100]], [[10.0, 100]], 1, T0)]
    s = run(base, timeout_s=2.0)
    jtc = X.Order('join_then_cross', 'T', 'buy', 9.0, 2.0, T0, 9.5, 100.0, 2_000_000, 105.0)
    plain = X.Order('join', 'T', 'buy', 9.0, 2.0, T0, 9.5, 100.0, 2_000_000, 105.0)
    s.open['T'] += [jtc, plain]
    s.feed('kalshi_margin', delta('T', 'ask', 10.0, 1, 2, T0 + 3_000_000, ''))
    ok(jtc.crossed_after_timeout and jtc.fill_px == 10.0,
       'timeout: join_then_cross pays the offer')
    ok(s.expired.get(('T', 'join')) == 1, 'timeout: plain join expires unfilled')
    ok(('T', 'join_then_cross') not in s.expired, 'timeout: crossed order is not also expired')


# ---- markouts --------------------------------------------------------------------------------
def test_markout():
    s = X.Sim(10.0, 1e9, 1e6, 1.0, ['T'])
    s.mid_ts['T'] = [T0, T0 + 1_000_000, T0 + 60_000_000]
    s.mid_px['T'] = [10.0, 10.01, 10.10]
    ok(s.mid_at('T', T0 + 500_000) == 10.0, 'markout: mid lookup takes the last value at or before')
    ok(s.mid_at('T', T0 - 1) is None, 'markout: no mid before the series starts')
    ok(s.mid_at('T', T0 + 200_000_000) is None,
       'markout: a mid more than 60s stale is not carried forward')

    buy = X.Order('join', 'T', 'buy', 10.0, 1.0, T0, 10.0, 0.0, 1, 5.0)
    buy.fill_pess_us = T0
    r = s.measure(buy, 'pess')
    # mid rose 10.00 -> 10.01 = +10 bps, and we were long.
    ok(abs(r['markout_1s'] - 10.0) < 1e-6, 'markout: buy into a rising mid is +10 bps')
    ok(abs(r['cost_bps']) < 1e-9, 'markout: filling at the decision mid costs 0')
    ok(r['fee_bps'] == X.MAKER_BPS, 'markout: a passive fill pays the maker fee')

    sell = X.Order('join', 'T', 'sell', 10.0, 1.0, T0, 10.0, 0.0, 1, 5.0)
    sell.fill_pess_us = T0
    r2 = s.measure(sell, 'pess')
    ok(abs(r2['markout_1s'] + 10.0) < 1e-6, 'markout: sell into a rising mid is -10 bps')

    taker = X.Order('taker', 'T', 'buy', 10.01, 1.0, T0, 10.0, 0.0, 1, 5.0)
    taker.fill_pess_us = T0
    r3 = s.measure(taker, 'pess')
    ok(abs(r3['cost_bps'] - 10.0) < 1e-6, 'markout: paying 1 tick above the mid costs +10 bps')
    ok(r3['fee_bps'] == X.TAKER_BPS, 'markout: a taker fill pays the taker fee')

    crossed = X.Order('join_then_cross', 'T', 'buy', 10.01, 1.0, T0, 10.0, 0.0, 1, 5.0)
    crossed.fill_pess_us = T0
    crossed.crossed_after_timeout = True
    ok(s.measure(crossed, 'pess')['fee_bps'] == X.TAKER_BPS,
       'markout: crossing after a timeout pays the TAKER fee, not the maker fee')

    unfilled = X.Order('join', 'T', 'buy', 10.0, 1.0, T0, 10.0, 0.0, 1, 5.0)
    ok(s.measure(unfilled, 'pess') is None, 'markout: an unfilled order produces no row')


# ---- sampling guards -------------------------------------------------------------------------
def test_size_guard():
    # Touch depth 2 contracts; $100 at a mid of 9.5 wants ~10.5 contracts. Must be dropped.
    s = X.Sim(100.0, 0.0, 30.0, 0.25, ['T'])
    s.feed('kalshi_margin', snap('T', [[9.0, 2]], [[10.0, 2]], 1, T0))
    s.feed('kalshi_margin', delta('T', 'bid', 9.0, 1, 2, T0 + 1_000_000, ''))
    ok(s.skipped.get('size_exceeds_touch', 0) > 0, 'guard: oversized order dropped and counted')
    ok(len(s.open['T']) == 0, 'guard: nothing rested')

    s2 = X.Sim(1.0, 0.0, 30.0, 0.25, ['T'])
    s2.feed('kalshi_margin', snap('T', [[9.0, 500]], [[10.0, 500]], 1, T0))
    s2.feed('kalshi_margin', delta('T', 'bid', 9.0, 1, 2, T0 + 1_000_000, ''))
    ok(len(s2.open['T']) > 0, 'guard: small order allowed through')


def test_no_room_to_improve():
    s = X.Sim(1.0, 0.0, 30.0, 0.5, ['T'])
    # One-tick spread on a 0.01 grid: there is nowhere to improve to.
    for p in [9.99, 10.00, 10.01, 10.02, 10.03, 10.04, 10.05, 10.06]:
        s.ticks.observe('T', p)
    s.feed('kalshi_margin', snap('T', [[10.00, 500]], [[10.01, 500]], 1, T0))
    s.feed('kalshi_margin', delta('T', 'bid', 10.00, 1, 2, T0 + 1_000_000, ''))
    modes = {o.mode for o in s.open['T']}
    ok('improve' not in modes, 'improve: not placed inside a one-tick spread')
    ok(s.skipped.get('no_room_to_improve', 0) > 0, 'improve: the skip is counted')


def test_improve_queue_is_zero():
    s = X.Sim(1.0, 0.0, 30.0, 0.5, ['T'])
    for p in [9.90, 9.91, 9.92, 9.93, 9.94, 9.95, 9.96, 9.97]:
        s.ticks.observe('T', p)
    s.feed('kalshi_margin', snap('T', [[9.90, 500]], [[10.10, 500]], 1, T0))
    s.feed('kalshi_margin', delta('T', 'bid', 9.90, 1, 2, T0 + 1_000_000, ''))
    imp = [o for o in s.open['T'] if o.mode == 'improve' and o.side == 'buy']
    ok(len(imp) == 1, 'improve: placed when the spread is wide')
    ok(imp[0].q_pess == 0 and imp[0].q_opt == 0,
       'improve: a new price level has an empty queue and the two bounds agree')
    ok(abs(imp[0].price - 9.91) < 1e-9, 'improve: posted one tick inside the bid')


def test_empty_touch_skipped():
    s = X.Sim(1.0, 0.0, 30.0, 0.5, ['T'])
    s.feed('kalshi_margin', snap('T', [[9.0, 100]], [], 1, T0))
    s.feed('kalshi_margin', delta('T', 'bid', 9.0, 1, 2, T0 + 1_000_000, ''))
    ok(len(s.open['T']) == 0, 'sample: one-sided book places nothing')


def test_gap_suppresses():
    # The gap must be on the SUBSCRIPTION counter, which is what the venue actually increments.
    s = X.Sim(1.0, 0.0, 30.0, 0.5, ['T'])
    s.feed('kalshi_margin', snap('T', [[9.0, 500]], [[10.0, 500]], 1, T0))
    s.feed('kalshi_margin', delta('T', 'bid', 9.0, 1, 2, T0 + 1_000_000, ''))
    n_before = len(s.open['T'])
    ok(n_before > 0 and not s.eng.books['T'].stale, 'gap: a contiguous sid sequence is fine')
    s.feed('kalshi_margin', delta('T', 'bid', 9.0, 1, 40, T0 + 2_000_000, ''))   # sid jump
    ok(s.eng.books['T'].stale, 'gap: book marked stale on a real sid gap')
    ok(len(s.open['T']) == n_before, 'gap: nothing new is sampled from a gapped book')


def test_reconnect_retires_open_orders():
    s = X.Sim(1.0, 1e9, 1e6, 1.0, ['T'])
    s.feed('kalshi_margin', snap('T', [[9.0, 500]], [[10.0, 500]], 1, T0))
    s.open['T'].append(X.Order('join', 'T', 'buy', 9.0, 1.0, T0, 9.5, 500.0, 10**9, 105.0))
    s.feed('kalshi_margin', {'ev': '_connect', 'recv_ts_us': T0 + 5_000_000})
    ok(len(s.open['T']) == 0, 'reconnect: no order is carried across the discontinuity')
    ok(s.expired[('T', 'join')] == 1, 'reconnect: it is retired as unfilled, not dropped')
    ok(s.eng.books['T'].bids == {}, 'reconnect: the book restarts empty')


def test_trade_channel_fills():
    s = X.Sim(1.0, 1e9, 1e6, 1.0, ['T'])
    s.feed('kalshi_margin', snap('T', [[9.90, 300]], [[10.10, 300]], 1, T0))
    buy = X.Order('join', 'T', 'buy', 9.90, 1.0, T0, 10.0, 100.0, 10**9, 200.0)
    sell = X.Order('join', 'T', 'sell', 10.10, 1.0, T0, 10.0, 100.0, 10**9, 200.0)
    s.open['T'] += [buy, sell]
    # An aggressive SELLER (taker_side 'ask') consumes the bid, so our resting BUY fills.
    s.feed('kalshi_margin', trade_ev('T', 9.90, 150, 'ask', 2, T0 + 1_000_000))
    ok(buy.fill_pess_us == T0 + 1_000_000,
       'trade channel: an aggressive seller fills our resting bid')
    ok(sell.fill_pess_us is None, 'trade channel: and does not touch our resting offer')
    # An aggressive BUYER lifts the offer.
    s.feed('kalshi_margin', trade_ev('T', 10.10, 150, 'bid', 3, T0 + 2_000_000))
    ok(sell.fill_pess_us == T0 + 2_000_000,
       'trade channel: an aggressive buyer fills our resting offer')


def test_unreasoned_delta_never_fills():
    # The whole failure mode this replaced: on a feed with no last_update_reason, a delta must
    # not be treated as a trade AND must not be silently ignored into a zero fill rate.
    s = X.Sim(1.0, 1e9, 1e6, 1.0, ['T'])
    s.feed('kalshi_margin', snap('T', [[9.90, 300]], [[10.10, 300]], 1, T0))
    o = X.Order('join', 'T', 'buy', 9.90, 1.0, T0, 10.0, 100.0, 10**9, 200.0)
    s.open['T'].append(o)
    s.feed('kalshi_margin', delta('T', 'bid', 9.90, -300, 2, T0 + 1_000_000, ''))
    ok(o.fill_pess_us is None,
       'unlabelled delta: a bare size decrease is not assumed to be a trade')
    s.feed('kalshi_margin', trade_ev('T', 9.90, 200, 'ask', 3, T0 + 2_000_000))
    ok(o.fill_pess_us == T0 + 2_000_000,
       'unlabelled delta: the trade channel is what fills it')


# ---- aggregation -----------------------------------------------------------------------------
def test_aggregate_counts_unfilled():
    s = X.Sim(1.0, 1e9, 1e6, 1.0, ['T'])
    s.mid_ts['T'] = [T0, T0 + 1_000_000, T0 + 60_000_000]
    s.mid_px['T'] = [10.0, 10.0, 10.0]
    f = X.Order('join', 'T', 'buy', 10.0, 1.0, T0, 10.0, 0.0, 1, 5.0)
    f.fill_pess_us = f.fill_opt_us = T0
    s.filled.append(f)
    s.expired[('T', 'join')] = 3
    rows = [r for r in (s.measure(f, b) for b in ('pess', 'opt')) if r]
    lines = X.aggregate(s, rows)
    pess = [L for L in lines if L['queue_bound'] == 'pess'][0]
    ok(pess['attempts'] == 4, 'aggregate: attempts include expired orders')
    ok(abs(pess['fill_rate_pct'] - 25.0) < 1e-9, 'aggregate: fill rate is 1/4, not 1/1')
    ok(pess['net_60s_bps'] is not None, 'aggregate: net computed')
    ok(abs(pess['net_60s_bps'] + X.MAKER_BPS) < 1e-9,
       'aggregate: a flat market nets exactly minus the maker fee')


def test_end_of_tape_counts_as_unfilled():
    with tempfile.TemporaryDirectory() as d:
        # An order still resting when the tape ends must not vanish from the denominator.
        s = X.Sim(1.0, 1e9, 1e6, 1.0, ['T'])
        s.open['T'].append(X.Order('join', 'T', 'buy', 9.0, 1.0, T0, 9.5, 100.0, 10**9, 5.0))
        for t, orders in s.open.items():
            for o in orders:
                if o.fill_opt_us is not None:
                    s.filled.append(o)
                else:
                    s.expired[(t, o.mode)] += 1
        ok(s.expired[('T', 'join')] == 1, 'tape end: unresolved order counted as unfilled')
        ok(len(s.filled) == 0, 'tape end: not counted as a fill')


# ---- end to end ------------------------------------------------------------------------------
def test_first_snapshot_seeds_the_clock():
    s = X.Sim(1.0, 5.0, 30.0, 0.5, ['T'])
    s.feed('kalshi_margin', snap('T', [[9.90, 500]], [[10.10, 500]], 1, T0))
    ok(len(s.open['T']) == 0, 'sample: the snapshot itself starts the clock, it is not a sample')
    s.feed('kalshi_margin', delta('T', 'bid', 9.90, 1, 2, T0 + 1_000_000, ''))
    ok(len(s.open['T']) == 0, 'sample: nothing before one full interval has passed')
    s.feed('kalshi_margin', delta('T', 'bid', 9.90, 1, 3, T0 + 6_000_000, ''))
    ok(len(s.open['T']) > 0, 'sample: fires after the interval')


def test_end_to_end():
    with tempfile.TemporaryDirectory() as d:
        hd = os.path.join(d, 'data', 'raw', 'source=kalshi_margin', 'hour=2026-09-11T15')
        os.makedirs(hd)
        evs = [snap('T', [[9.90, 200]], [[10.10, 200]], 1, T0)]
        seq, ts = 2, T0
        for i in range(400):
            ts += 200_000
            side = 'bid' if i % 2 else 'ask'
            px = 9.90 if side == 'bid' else 10.10
            evs.append(delta('T', side, px, -5 if i % 3 else 5, seq, ts,
                             'Trade' if i % 3 else ''))
            seq += 1
        with gzip.open(os.path.join(hd, 'events.jsonl.gz'), 'wt') as f:
            for e in evs:
                f.write(json.dumps(e) + '\n')

        old = X.ROOT
        X.ROOT = d
        try:
            s = X.Sim(1.0, 2.0, 5.0, 0.5, ['T'])
            n = 0
            for src, e in X.iter_events(['2026-09-11T15']):
                s.feed(src, e)
                n += 1
            ok(n == len(evs), f'e2e: all {len(evs)} events replayed')
            ok(len(s.filled) > 0, 'e2e: some orders filled')
            ok(any(o.mode == 'taker' for o in s.filled), 'e2e: taker orders resolve immediately')

            for t, orders in s.open.items():
                for o in orders:
                    if o.fill_opt_us is None:
                        s.expired[(t, o.mode)] += 1
            rows = [r for o in s.filled for b in ('pess', 'opt')
                    for r in [s.measure(o, b)] if r]
            lines = X.aggregate(s, rows)
            ok(len(lines) > 0, 'e2e: aggregate produced lines')

            csv = os.path.join(d, 'reports', 'execution_simulator.csv')
            md = os.path.join(d, 'reports', 'execution_summary.md')
            class A: notional, sample_s, timeout_s, max_size_frac = 1.0, 2.0, 5.0, 0.5
            X.write_csv(lines, csv)
            X.write_summary(s, lines, md, ['2026-09-11T15'], A())
            head = open(csv).read().splitlines()
            ok(head[0] == ','.join(X.CSV_COLS), 'e2e: csv header matches declared columns')
            ok(all(len(r.split(',')) == len(X.CSV_COLS) for r in head[1:]),
               'e2e: every csv row has the right width')
            body = open(md).read()
            ok('Execution simulator' in body, 'e2e: summary written')
            ok('What would change these numbers' in body,
               'e2e: summary states its own limitations')
            ok('queue bound' in body.lower() or 'bounds' in body,
               'e2e: summary reports both queue bounds')
        finally:
            X.ROOT = old


def test_summary_says_not_enough_data():
    with tempfile.TemporaryDirectory() as d:
        s = X.Sim(1.0, 1e9, 1e6, 1.0, ['T'])
        md = os.path.join(d, 'r', 'execution_summary.md')
        class A: notional, sample_s, timeout_s, max_size_frac = 25.0, 15.0, 30.0, 0.25
        X.write_summary(s, [], md, ['2026-09-11T15'], A())
        body = open(md).read()
        ok('not enough recorded data' in body,
           'summary: an empty run says so instead of printing a best strategy')


for fn in [test_ticks, test_walk, test_signed, test_queue_bounds, test_price_priority, test_cross_fills,
           test_left_touch, test_timeout_and_cross, test_markout, test_size_guard,
           test_no_room_to_improve, test_improve_queue_is_zero, test_empty_touch_skipped,
           test_gap_suppresses, test_reconnect_retires_open_orders,
           test_trade_channel_fills, test_unreasoned_delta_never_fills,
           test_first_snapshot_seeds_the_clock,
           test_aggregate_counts_unfilled,
           test_end_of_tape_counts_as_unfilled, test_end_to_end,
           test_summary_says_not_enough_data]:
    fn()

print(f'{P} passed, {F} failed')
sys.exit(1 if F else 0)
