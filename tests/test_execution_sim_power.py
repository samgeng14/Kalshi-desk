#!/usr/bin/env python3
"""Does the simulator actually detect adverse selection, and does it stay quiet when there is none?

A fill simulator that reports a plausible-looking cost on every input is useless -- it cannot tell
you whether the cost it reports is real. So: two synthetic tapes with known answers.

  UNINFORMED  trades arrive at random and the mid is a driftless random walk. A passive fill
              carries no information. Expected 1s markout: 0. Expected net: minus the fee.
  INFORMED    every trade is followed by a mid move in the direction of the aggressor. A passive
              fill is always on the wrong side. Expected 1s markout: clearly negative.

If the simulator reports the same thing on both, it is not measuring anything.
"""
import gzip, json, os, random, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'scripts'))
import execution_sim as X

P = F = 0
def ok(c, l):
    global P, F
    if c: P += 1
    else:
        F += 1
        print(f'  FAIL {l}')

T0 = 1_789_000_000_000_000
TICK = 0.01


def tape(informed, n=9000, seed=7):
    """Emit a recorder-shaped event list for one market.

    The generator tracks the exact quantity resting at every price it writes, because a tape that
    cancels 300 from a level holding 280 strands size at an old price and the book ends up
    permanently crossed. A crossed synthetic book tests nothing.
    """
    rng = random.Random(seed)
    evs, seq, ts = [], 1, T0
    bid, ask = 9.99, 10.01
    qb, qa = 300.0, 300.0
    evs.append({'ev': 'orderbook_snapshot', 'seq': seq, 'src_ts_us': ts, 'recv_ts_us': ts,
                'msg': {'market_ticker': 'T', 'bid': [[bid, qb]], 'ask': [[ask, qa]]}})

    def push(side, price, d, reason):
        nonlocal seq
        seq += 1
        evs.append({'ev': 'orderbook_delta', 'seq': seq, 'src_ts_us': ts, 'recv_ts_us': ts,
                    'msg': {'market_ticker': 'T', 'side': side, 'price': round(price, 2),
                            'delta': d, 'last_update_reason': reason}})

    for i in range(n):
        ts += 100_000
        drift = 0
        if rng.random() < 0.35:
            s_ = 'bid' if rng.random() < 0.5 else 'ask'
            px, q = (bid, qb) if s_ == 'bid' else (ask, qa)
            vol = min(q - 20.0, rng.choice([40.0, 90.0, 180.0, 320.0]))
            if vol > 0:
                push(s_, px, -vol, 'Trade')
                push(s_, px, vol, '')            # replenished by a new maker
            if informed:
                drift = -1 if s_ == 'bid' else 1
        else:
            s_ = 'bid' if rng.random() < 0.5 else 'ask'
            px, q = (bid, qb) if s_ == 'bid' else (ask, qa)
            d = rng.choice([-20.0, 20.0])
            if q + d >= 40.0:
                push(s_, px, d, 'MarginCancel' if d < 0 else '')
                if s_ == 'bid':
                    qb += d
                else:
                    qa += d
            drift = rng.choice([-1, 0, 0, 0, 1])
        if drift:
            nb, na = round(bid + drift * 0.01, 2), round(ask + drift * 0.01, 2)
            ts += 10_000
            # New levels first, then fully retire the old ones, so the book is never crossed and
            # never leaves a stray.
            push('bid', nb, 300.0, '')
            push('ask', na, 300.0, '')
            if abs(nb - bid) > 1e-9:
                push('bid', bid, -qb, 'MarginCancel')
            if abs(na - ask) > 1e-9:
                push('ask', ask, -qa, 'MarginCancel')
            bid, ask, qb, qa = nb, na, 300.0, 300.0
    return evs


def run(evs, d, tag):
    hd = os.path.join(d, tag, 'data', 'raw', 'source=kalshi_margin', 'hour=2026-09-11T15')
    os.makedirs(hd, exist_ok=True)
    with gzip.open(os.path.join(hd, 'events.jsonl.gz'), 'wt') as f:
        for e in evs:
            f.write(json.dumps(e) + '\n')
    old = X.ROOT
    X.ROOT = os.path.join(d, tag)
    try:
        s = X.Sim(1.0, 3.0, 10.0, 0.9, ['T'])
        for src, e in X.iter_events(['2026-09-11T15']):
            s.feed(src, e)
        for t, orders in s.open.items():
            for o in orders:
                if o.fill_opt_us is not None:
                    s.filled.append(o)
                else:
                    s.expired[(t, o.mode)] += 1
        rows = [r for o in s.filled for b in ('pess', 'opt') for r in [s.measure(o, b)] if r]
        return s, {(L['market'], L['mode'], L['queue_bound']): L for L in X.aggregate(s, rows)}
    finally:
        X.ROOT = old


with tempfile.TemporaryDirectory() as d:
    su, U = run(tape(False), d, 'unin')
    si, I = run(tape(True), d, 'inf')

    ju = U.get(('T', 'join', 'pess')); ji = I.get(('T', 'join', 'pess'))
    ok(ju is not None and ju['fills'] >= 10, f'uninformed: passive fills produced ({ju and ju["fills"]})')
    ok(ji is not None and ji['fills'] >= 10, f'informed: passive fills produced ({ji and ji["fills"]})')

    if ju and ji:
        mu, mi = ju['markout_1s_bps'], ji['markout_1s_bps']
        print(f'  uninformed join markout_1s = {X.fmt(mu)} bps over {ju["fills"]} fills')
        print(f'  informed   join markout_1s = {X.fmt(mi)} bps over {ji["fills"]} fills')
        ok(mu is not None and abs(mu) < 4.0,
           f'uninformed: 1s markout near zero, got {X.fmt(mu)}')
        ok(mi is not None and mi < -4.0,
           f'informed: 1s markout clearly negative, got {X.fmt(mi)}')
        ok(mi < mu - 4.0, 'power: the simulator separates informed from uninformed flow')

    tu = U.get(('T', 'taker', 'pess'))
    if tu:
        print(f'  uninformed taker entry cost = {X.fmt(tu["entry_cost_bps"])} bps '
              f'(half a 20 bps spread = 10)')
        ok(abs(tu['entry_cost_bps'] - 10.0) < 1.5,
           f'taker: crossing a 2-tick spread costs half of it, got {X.fmt(tu["entry_cost_bps"])}')
        ok(abs(tu['net_60s_bps'] - (tu['markout_60s_bps'] - tu['entry_cost_bps'] - 12.0)) < 1e-6,
           'taker: net = markout - entry cost - taker fee, arithmetic checks')

    ju_o = U.get(('T', 'join', 'opt'))
    if ju and ju_o:
        ok(ju_o['fills'] >= ju['fills'],
           'bounds: the optimistic queue bound never fills less than the pessimistic one')
        print(f'  fill rate pess {X.fmt(ju["fill_rate_pct"],1)}%  '
              f'opt {X.fmt(ju_o["fill_rate_pct"],1)}%')

print(f'{P} passed, {F} failed')
sys.exit(1 if F else 0)
