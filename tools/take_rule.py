#!/usr/bin/env python3
"""The IOC game lanes' take rule in Python: deskd's take_decide (strat/take.h), for the gate tools.

decisions 13 has one take engine for C1, X1, N1 and N2, and plan 5(e) says why its rule must be
measured exactly as it trades ("the S2 mistake was testing one order type and trading another").
This is that rule, once, for the offline tools that backtest it: tools/college_gate.py (C1,
COLLEGE) now, and the NBA tools (x1_backtest.py ESPN_FOLLOW and ESPN_FADE, nba_gate.py MODEL and
FAV_PREGAME) by import rather than a port of their own. include/deskd/strat/take.h states the rule
in full; this file follows src/deskd/strat/take.cpp check by check, in the same order, with the
same integer arithmetic, and its self-test asserts every hand-worked case of the three fixtures
the C++ suite (deskd-take) asserts too:

  tests/deskd/fixtures/college_take_cases.json   COLLEGE
  tests/deskd/fixtures/x1_golden.json            ESPN_FOLLOW, ESPN_FADE
  tests/deskd/fixtures/n1_golden.json            MODEL, FAV_PREGAME

A change to take.cpp is a change here, and the fixtures are what keep the two together.

Units are deskd's (bus/units.h), all integers: prices and money in 1/10,000 $ (Px, Money: "0.4500"
is 4500), sizes in 1/100 contract (Qty: 4 contracts is 400), a game's worst case and a route's
cost in micro-dollars, times in ms. The fee is gw/fees.h's (decisions 13: one fee module):

  kalshi_model_fee(mult, px, qty)       0.07 x mult x C x P x (1 - P), rounded up to $0.000001;
                                        mult in thousandths, rounded up (1e-6 slack, so 0.1 x 3 is
                                        300); a multiplier that is not a positive number counts 0
  kalshi_fee_order_cent(px, qty, mult)  that, rounded up to the cent: what the take engines size
                                        clips with and price edges at (five contracts at 40c: $0.09)
  clip_count(clip, px, mult, cap, whole)  the most contracts whose cost and fee fit the clip ($2 at
                                        48c: 4, for $1.92 and a 7c fee)

The decision (decide(inp, params)): inp is take.h's TakeInput, a dict with integer units

  a, b          the two markets, each {ticker, bid, bid_qty, ask, ask_qty (None: no such side),
                stale, book (optional; False: no book at all, which stale True also stands for),
                fee_mult (None: not known), fractional, position, bid_since_ms, ask_since_ms}
  state, src, score_why, fetched_ms, first_seen_ms, start_ms, a_score, b_score, t_left_s (None:
  no clock), ot, points, set_end_ms, p0, fair, fair_why, espn, espn_seen_ms (A's side; p0, fair
  and espn are Px or None), now_ms, last_take_ms, game_used (micro-dollars), game_takes, fired
  ([A, B]), private_up, not_tradable, volume (Qty)

and params is take.h's TakeParams (params(mode) gives take_params' defaults, in the same units).
It returns {'take': {ticker, side, price, count, fee, edge, team_px, ref, cost, dir, touch_since_ms}}
or {'pull': CODE, 'why', 'edge' (the pulled route's, or None), 'dir' (-1: the game's own checks),
'route' (the pulled route's order once it had a count, else None)}, edge rounded down to Money a
contract and dir 0 for A's team, 1 for B's. has_edge(decision, mode) is TakeEngine's rule for a
signal run: the decision took, or a route got past its edge (FAV_PREGAME: into its band).
fixture_cases(path) reads a take_cases file in deskd's units and run_cases(path) runs every case
of one (the gate tools' self-tests and --check).

Standard library only. Import it (sys.path.insert(0, <tools dir>); import take_rule).

Usage:
  python3 tools/take_rule.py --self-test
  python3 tools/take_rule.py --check FILE.json [FILE.json ...]   # any take_cases file
"""
import argparse
import importlib.util
import json
import math
import os
import sys
from decimal import Decimal, InvalidOperation
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

ONE = 10000            # $1 in Px / Money (kOneDollar)
CT = 100               # one contract in Qty (kOneContract)
CENT_US = 10000        # one cent in micro-dollars
MODES = ('COLLEGE', 'MODEL', 'ESPN_FOLLOW', 'ESPN_FADE', 'FAV_PREGAME')
PASSED = 100           # a route's stage once every check passed (kPassed)
FIXTURES = ('college_take_cases.json', 'x1_golden.json', 'n1_golden.json')

# every pull code, in the order of take.h's checks (take_pull_codes)
PULL_CODES = ('NOT_TRADABLE', 'PRIVATE_DOWN', 'STALE_BOOK', 'NO_FEE', 'NO_SCORE', 'SCORE_OLD', 'STATE', 'WINDOW',
              'NO_P0', 'NO_FAIR', 'NO_ESPN', 'NO_MID', 'NO_POINTS', 'SET_WINDOW', 'OVERTIME', 'LATE', 'SIGNAL_OLD',
              'VOLUME', 'SPREAD', 'COOL_DOWN', 'TAKES', 'GAME_CAP', 'NO_TOUCH', 'HELD', 'CLIP', 'SIZE', 'NO_EDGE',
              'BAND', 'TOUCH_AFTER_SCORE', 'TRAILER', 'FIRED')


# ---------------------------------------------------------------- the fee (gw/fees.h)

def fee_milli(mult):
    """A Kalshi fee multiplier in thousandths, rounded up (fees.cpp rate_steps); 0 when not a
    positive number."""
    if mult is None or isinstance(mult, bool) or not isinstance(mult, (int, float)) or not mult > 0:
        return 0
    return int(math.ceil(min(float(mult), 1e6) * 1000 - 1e-6))


def kalshi_model_fee(mult, px, qty):
    """One taker fill's model fee in micro-dollars: 7 q p (1e4 - p) milli / 1e9, rounded up."""
    milli = fee_milli(mult)
    if milli <= 0 or qty <= 0 or px <= 0 or px >= ONE:
        return 0
    return -(-(7 * qty * px * (ONE - px) * milli) // 10 ** 9)


def kalshi_fee_order_cent(px, qty, mult):
    """The take engines' fee for one order: the model fee rounded up to the cent, in Money."""
    return -(-kalshi_model_fee(mult, px, qty) // CENT_US) * CENT_US // 100


def fit(budget, px, mult, step, cap):
    """The most whole steps of contracts at px whose cost and fee come to at most budget
    micro-dollars, at most cap (0: none); take.cpp's fit, by halving (cost and fee only grow)."""
    if budget <= 0 or px <= 0 or px >= ONE or step <= 0:
        return 0
    lo, hi = 0, min(budget // px // step, 1000000 * CT // step)
    if cap > 0:
        hi = min(hi, cap // step)
    while lo < hi:
        mid = lo + (hi - lo + 1) // 2
        c = mid * step
        if px * c + 100 * kalshi_fee_order_cent(px, c, mult) <= budget:
            lo = mid
        else:
            hi = mid - 1
    return lo * step


def clip_count(clip, px, mult, max_count=0, whole_only=True):
    """Contracts (Qty) a clip of Money buys at px, fee included; 0 when not one step fits."""
    return fit(clip * 100, px, mult, CT if whole_only else 1, max_count)


# ---------------------------------------------------------------- the params (take_params)

def params(mode):
    """take.cpp take_params(mode): each lane's rule as its spec set it, in deskd's units."""
    if mode not in MODES:
        raise ValueError('unknown mode %r' % mode)
    p = {'mode': mode, 'edge': 2000, 'clip': 2 * ONE, 'max_count': 0, 'fixed_count': 0, 'game_cap': 10000000,
         'max_takes': 0, 'max_score_age_ms': 5000, 'signal_ms': 0, 'cool_ms': 5000, 'max_volume': 0,
         'stale_only': False, 'leader_only': False, 'points_required': True, 'set_window_ms': 120000,
         'max_spread': 0, 'min_px': 0, 'max_px': ONE, 'trade_ot': True, 'min_t_left_s': 0.0,
         'fav_lo': 9000, 'fav_hi': 9500, 'fav_from_ms': 3600000, 'fav_to_ms': 120000}
    if mode == 'COLLEGE':   # college.json's; the gate may raise edge, never below 10c
        p.update(edge=2000, cool_ms=5000, max_volume=20000 * CT, stale_only=True)
    elif mode in ('MODEL', 'ESPN_FOLLOW', 'ESPN_FADE'):   # nba.json's (thr 10c until the gate's)
        p.update(edge=1000, max_count=4 * CT, max_takes=4, signal_ms=30000, cool_ms=10000, max_spread=300,
                 min_px=500, max_px=9500, trade_ot=False, min_t_left_s=120.0)
    else:   # FAV_PREGAME: 5 contracts, hold to settlement
        p.update(edge=0, clip=0, fixed_count=5 * CT, max_score_age_ms=60000, cool_ms=10000, max_spread=1000)
    return p


# ---------------------------------------------------------------- the decision (take_decide)

def _ms(v):
    return '%d ms' % v


def _price(px):
    return '%.4f' % (px / ONE)


def _floor(e):
    """A route's edge (a Fraction of Money) rounded down."""
    return math.floor(e)


def _pull(code, why, edge=None, d=-1, route=None):
    return {'pull': code, 'why': why, 'edge': edge, 'dir': d, 'route': route}


def effective_book(a, b):
    """game_fair.cpp effective_book: market a's YES bid and ask over both of a game's markets
    (b pays on the other team), stale books left out; None when no bid or no ask is left."""
    bids, asks = [], []
    if not a['stale']:
        if a['bid'] is not None:
            bids.append(a['bid'])
        if a['ask'] is not None:
            asks.append(a['ask'])
    if not b['stale']:   # B's ask is a YES bid on A's team, B's bid an ask
        if b['ask'] is not None:
            bids.append(ONE - b['ask'])
        if b['bid'] is not None:
            asks.append(ONE - b['bid'])
    if not bids or not asks:
        return None
    return max(bids), min(asks)


def decide(i, P):
    """take_decide(i, P): see the module docstring for the shapes."""
    mode = P['mode']
    fav, fade = mode == 'FAV_PREGAME', mode == 'ESPN_FADE'
    espn = mode in ('ESPN_FOLLOW', 'ESPN_FADE')
    model = mode in ('COLLEGE', 'MODEL')
    now = i['now_ms']
    legs = (i['a'], i['b'])

    # ---- the game
    if i['not_tradable']:
        return _pull('NOT_TRADABLE', 'not tradable: ' + i['not_tradable'])
    if not i['private_up']:
        return _pull('PRIVATE_DOWN', 'the private feed is down')
    for l in legs:
        if not l.get('book', True):
            return _pull('STALE_BOOK', l['ticker'] + ' has no book')
        if l['stale']:
            return _pull('STALE_BOOK', l['ticker'] + "'s book is stale or missing")
        if l['bid'] is not None and l['ask'] is not None and l['bid'] >= l['ask']:
            return _pull('STALE_BOOK', l['ticker'] + "'s book is crossed")
    for l in legs:   # fee_ready() refuses the same multipliers
        m = l['fee_mult']
        if m is None or isinstance(m, bool) or not math.isfinite(m) or m < 0:
            return _pull('NO_FEE', l['ticker'] + "'s fee multiplier is not known yet")
    if i['score_why']:
        return _pull('NO_SCORE', 'no score: ' + i['score_why'])
    if not i['state']:
        return _pull('NO_SCORE', 'no score')
    if i['fetched_ms'] <= 0 or now - i['fetched_ms'] > P['max_score_age_ms']:
        return _pull('SCORE_OLD', 'the score is %s old (at most %s)' % (
            _ms(now - i['fetched_ms']) if i['fetched_ms'] > 0 else 'never', _ms(P['max_score_age_ms'])))
    if i['state'] != ('pre' if fav else 'in'):
        return _pull('STATE', 'the game is ' + i['state'] + (': only before the start' if fav else ': not in play'))
    if fav:
        if i['start_ms'] <= 0:
            return _pull('WINDOW', 'no scheduled start')
        if now < i['start_ms'] - P['fav_from_ms'] or now > i['start_ms'] - P['fav_to_ms']:
            return _pull('WINDOW', '%s before the start: outside %s to %s' % (
                _ms(i['start_ms'] - now), _ms(P['fav_from_ms']), _ms(P['fav_to_ms'])))
    if model and i['p0'] is None:
        return _pull('NO_P0', 'no pregame price')
    if model and i['fair'] is None:
        return _pull('NO_FAIR', 'no fair' + (': ' + i['fair_why'] if i['fair_why'] else ''))
    if espn and i['espn'] is None:
        return _pull('NO_ESPN', 'no ESPN win probability')
    eff = None
    if fade:
        eff = effective_book(i['a'], i['b'])
        if eff is None:
            return _pull('NO_MID', 'no two-sided effective book for a mid')
    if P['points_required'] and not i['points']:
        return _pull('NO_POINTS', "the set's points were not read")
    if not P['points_required'] and (i['set_end_ms'] <= 0 or now - i['set_end_ms'] > P['set_window_ms']):
        return _pull('SET_WINDOW', 'no set has ended' if i['set_end_ms'] <= 0 else '%s since a set ended (at most %s)' % (
            _ms(now - i['set_end_ms']), _ms(P['set_window_ms'])))
    if i['ot'] and not P['trade_ot']:
        return _pull('OVERTIME', 'in overtime')
    if P['min_t_left_s'] > 0 and (i['t_left_s'] is None or not i['t_left_s'] >= P['min_t_left_s']):
        return _pull('LATE', 'no game clock' if i['t_left_s'] is None else '%.1f s of regulation left (at least %.1f s)' % (
            i['t_left_s'], P['min_t_left_s']))
    if P['signal_ms'] > 0:
        seen = i['espn_seen_ms'] if espn else i['first_seen_ms']
        if seen <= 0 or now - seen > P['signal_ms']:
            return _pull('SIGNAL_OLD', "%s was first seen %s (at most %s)" % (
                "ESPN's number" if espn else 'the score', _ms(now - seen) + ' ago' if seen > 0 else 'never',
                _ms(P['signal_ms'])))
    if P['max_volume'] > 0 and i['volume'] >= P['max_volume']:
        return _pull('VOLUME', 'in-game volume %d contracts: not a thin game' % (i['volume'] // CT))
    if P['max_spread'] > 0:
        for l in legs:
            if l['bid'] is None or l['ask'] is None:
                return _pull('SPREAD', l['ticker'] + "'s book is one-sided")
            if l['ask'] - l['bid'] > P['max_spread']:
                return _pull('SPREAD', "%s's spread %s is over %s" % (l['ticker'], _price(l['ask'] - l['bid']),
                                                                       _price(P['max_spread'])))
    if i['last_take_ms'] > 0 and now - i['last_take_ms'] < P['cool_ms']:
        return _pull('COOL_DOWN', '%s since the last take in the game (at least %s)' % (
            _ms(now - i['last_take_ms']), _ms(P['cool_ms'])))
    if P['max_takes'] > 0 and i['game_takes'] >= P['max_takes']:
        return _pull('TAKES', '%d takes in the game' % i['game_takes'])
    if P['game_cap'] > 0 and i['game_used'] >= P['game_cap']:
        return _pull('GAME_CAP', "the game's worst case $%s is at its cap" % _price(i['game_used'] // 100))

    # ---- the routes: A BUY, A SELL (of B), B BUY, B SELL (of A)
    room = P['game_cap'] - i['game_used']
    score = (i['a_score'], i['b_score'])

    def price(d, buy):
        own, other = legs[d], legs[1 - d]
        r = {'dir': d, 'buy': buy, 'leg': own if buy else other, 'stage': 0, 'code': '', 'why': '', 'price': 0,
             'team_px': 0, 'ref': 0, 'count': 0, 'fee': 0, 'cost': 0, 'edge': None, 'since': 0}

        def fail(code, why):
            r['code'], r['why'] = code, why
            return r
        touch = own['ask'] if buy else other['bid']
        if touch is None:
            return fail('NO_TOUCH', r['leg']['ticker'] + (' has no ask' if buy else ' has no bid'))
        r['stage'] += 1
        r['price'] = touch
        r['team_px'] = touch if buy else ONE - touch
        r['since'] = own['ask_since_ms'] if buy else other['bid_since_ms']
        px_s = "the team's price " + _price(r['team_px'])
        if fav:
            if r['team_px'] < P['fav_lo'] or r['team_px'] >= P['fav_hi']:
                return fail('BAND', '%s is outside [%s, %s)' % (px_s, _price(P['fav_lo']), _price(P['fav_hi'])))
            r['stage'] += 1
        # the count
        mult = r['leg']['fee_mult']
        step = 1 if r['leg']['fractional'] else CT
        held = max(0, own['position']) + max(0, -other['position'])
        if fav:
            w = P['fixed_count'] - held
            want = (w // step if w >= 0 else -((-w) // step)) * step   # toward zero, as C++'s /
        else:
            want = fit(P['clip'] * 100, r['team_px'], mult, step, P['max_count'])
        by_room = fit(room, r['team_px'], mult, step, 0) if P['game_cap'] > 0 else 1 << 62
        size = (own['ask_qty'] if buy else other['bid_qty']) or 0
        r['count'] = min(want, by_room, size // step * step)
        if r['count'] <= 0:
            r['count'] = 0
            if fav and want <= 0:
                return fail('HELD', 'holds %d of %s' % (held // CT, own['ticker']))
            if by_room <= 0:
                return fail('GAME_CAP', "the game's room $%s buys no contract at %s" % (_price(room // 100),
                                                                                   _price(r['team_px'])))
            if want <= 0:
                return fail('CLIP', 'the clip $%s buys no contract at %s' % (_price(P['clip']), _price(r['team_px'])))
            return fail('SIZE', 'the touch is under one contract')
        r['stage'] += 1
        r['fee'] = kalshi_fee_order_cent(r['team_px'], r['count'], mult)
        r['cost'] = r['team_px'] * r['count'] + 100 * r['fee']
        # the edge, a contract, exactly
        if fade:
            gap_a = eff[0] + eff[1] - 2 * i['espn']   # twice A's effective mid - ESPN's
            r['ref'] = i['espn'] if d == 0 else ONE - i['espn']
            r['edge'] = Fraction(gap_a if d == 0 else -gap_a, 2)
        elif not fav:
            ref_a = i['fair'] if model else i['espn']
            r['ref'] = ref_a if d == 0 else ONE - ref_a
            r['edge'] = Fraction((r['ref'] - r['team_px']) * r['count'] - 100 * r['fee'], r['count'])
        else:
            r['edge'] = Fraction(0)
        if not fav:
            if r['edge'] < P['edge']:
                return fail('NO_EDGE', 'edge %.4f is under %.4f' % (_floor(r['edge']) / ONE, P['edge'] / ONE))
            r['stage'] += 1
            if r['team_px'] < P['min_px'] or r['team_px'] > P['max_px']:
                return fail('BAND', '%s is outside %s-%s' % (px_s, _price(P['min_px']), _price(P['max_px'])))
            r['stage'] += 1
        if P['stale_only'] and not (r['since'] > 0 and r['since'] < i['first_seen_ms']):
            return fail('TOUCH_AFTER_SCORE', 'the touch is %s newer than the score' % _ms(r['since'] - i['first_seen_ms'])
                        if r['since'] > 0 else "the touch's age is not known")
        r['stage'] += 1
        if P['leader_only'] and score[d] <= score[1 - d]:
            return fail('TRAILER', "%s's team does not lead (%d-%d)" % (own['ticker'], score[d], score[1 - d]))
        r['stage'] += 1
        if i['fired'][d]:
            return fail('FIRED', 'this run already took ' + own['ticker'])
        r['stage'] = PASSED
        return r

    routes = [price(d, buy) for d in (0, 1) for buy in (True, False)]

    def order(r):
        return {'ticker': r['leg']['ticker'], 'side': 'BUY' if r['buy'] else 'SELL', 'price': r['price'],
                'count': r['count'], 'fee': r['fee'], 'edge': _floor(r['edge']) if r['edge'] is not None else 0,
                'team_px': r['team_px'], 'ref': r['ref'], 'cost': r['cost'], 'dir': r['dir'],
                'touch_since_ms': r['since']}
    # the best route that passed: the largest edge, then the lowest cost a contract, then the first
    best = None
    for r in routes:
        if r['stage'] != PASSED:
            continue
        if best is None or best['edge'] < r['edge'] or (
                not r['edge'] < best['edge'] and r['cost'] * best['count'] < best['cost'] * r['count']):
            best = r
    if best is not None:
        return {'take': order(best)}
    # none: the reason of the route that got furthest (then the larger edge, then the first)
    far = routes[0]
    for r in routes:
        if r['stage'] > far['stage'] or (r['stage'] == far['stage'] and r['edge'] is not None and
                                        (far['edge'] is None or far['edge'] < r['edge'])):
            far = r
    return _pull(far['code'], far['why'], _floor(far['edge']) if far['edge'] is not None else None, far['dir'],
                 order(far) if far['count'] > 0 else None)


PAST_EDGE = ('BAND', 'TOUCH_AFTER_SCORE', 'TRAILER', 'FIRED')
PAST_BAND = ('HELD', 'GAME_CAP', 'CLIP', 'SIZE', 'TOUCH_AFTER_SCORE', 'TRAILER', 'FIRED')


def has_edge(d, mode):
    """TakeEngine's has_edge: the decision found a team worth taking (it takes, or a route got past
    its edge, FAV_PREGAME's into its band, before a later check pulled it)."""
    if 'take' in d:
        return True
    if d['dir'] < 0:
        return False
    return d['pull'] in (PAST_BAND if mode == 'FAV_PREGAME' else PAST_EDGE)


# ---------------------------------------------------------------- the fixtures' shape

def _dec(s, scale):
    """A fixture's decimal string (or number) at scale, exactly; None stays None."""
    if s is None:
        return None
    try:
        v = Decimal(str(s)) * scale
    except InvalidOperation:
        raise ValueError('not a number: %r' % (s,))
    if v != v.to_integral_value():
        raise ValueError('%r is not a whole number of units at %d' % (s, scale))
    return int(v)


def merge(base, over):
    """The fixtures' 'how': an object is merged key by key (recursively), any other value replaces."""
    if not isinstance(base, dict) or not isinstance(over, dict):
        return over
    out = dict(base)
    for k, v in over.items():
        out[k] = merge(base[k], v) if k in base else v
    return out


def params_of(fx, mode):
    """A fixture's params (decimal strings in dollars, sizes in contracts) in deskd's units."""
    p = dict(fx)
    for k in ('edge', 'clip', 'max_spread', 'min_px', 'max_px', 'fav_lo', 'fav_hi'):
        p[k] = _dec(p[k], ONE)
    for k in ('max_count', 'fixed_count', 'max_volume'):
        p[k] = _dec(p[k], CT)
    p['game_cap'] = _dec(p['game_cap'], 10 ** 6)
    p['min_t_left_s'] = float(p['min_t_left_s'])
    p['mode'] = mode
    return p


def input_of(fx):
    """A fixture's input in deskd's units."""
    i = dict(fx)
    for side in ('a', 'b'):
        l = dict(fx[side])
        for k in ('bid', 'ask'):
            l[k] = _dec(l[k], ONE)
        for k in ('bid_qty', 'ask_qty', 'position'):
            l[k] = _dec(l[k], CT) if l[k] is not None else 0
        i[side] = l
    for k in ('p0', 'fair', 'espn'):
        i[k] = _dec(i[k], ONE)
    i['game_used'] = _dec(i['game_used'], 10 ** 6)
    i['volume'] = _dec(i['volume'], CT)
    i['fired'] = list(i['fired'])
    return i


def want_of(w):
    """A case's 'want' in deskd's units, in the shape decide() is compared on."""
    if 'take' in w:
        t = w['take']
        return {'take': {'ticker': t['ticker'], 'side': t['side'], 'price': _dec(t['price'], ONE),
                         'count': _dec(t['count'], CT), 'fee': _dec(t['fee'], ONE), 'edge': _dec(t['edge'], ONE),
                         'dir': 'AB'.index(t['dir'])}}
    return {'pull': w['pull'], 'edge': _dec(w['edge'], ONE), 'dir': 'AB'.index(w['dir']) if w['dir'] else -1}


def got_of(d):
    """decide()'s answer in want_of's shape."""
    if 'take' in d:
        t = d['take']
        return {'take': {k: t[k] for k in ('ticker', 'side', 'price', 'count', 'fee', 'edge', 'dir')}}
    return {'pull': d['pull'], 'edge': d['edge'], 'dir': d['dir']}


def fixture_cases(path):
    """[(name, mode, input, params, want)] of one fixture file, in deskd's units."""
    with open(path, encoding='utf-8') as fh:
        f = json.load(fh)
    out = []
    for c in f['cases']:
        mode = c['mode']
        p = params_of(merge(f['params'][mode], c.get('params', {})), mode)
        i = input_of(merge(f['base'][mode], c.get('in', {})))
        out.append((c['name'], mode, i, p, want_of(c['want'])))
    return out


def run_cases(path):
    """Every case of one take_cases file: (cases run, [(name, got, want)] that differ or pull
    without a why, [modes whose params are not take_params' defaults])."""
    with open(path, encoding='utf-8') as fh:
        f = json.load(fh)
    if f.get('kind') != 'take_cases' or f.get('v') != 1:
        raise ValueError('%s is not a take_cases file (v 1)' % path)
    bad_params = [m for m in f['params'] if params_of(f['params'][m], m) != params(m)]
    cases, bad = fixture_cases(path), []
    for name, mode, i, p, want in cases:
        d = decide(i, p)
        got = got_of(d)
        if got != want or ('pull' in d and not d['why']):
            bad.append((name, got, want))
    return len(cases), bad, bad_params


# ---------------------------------------------------------------- self-test

def research_fee():
    """research/espn_leadlag.py's taker_fee (the float fee the old study used), None when that
    script is not in the tree."""
    src = os.path.join(ROOT, 'research', 'espn_leadlag.py')
    if not os.path.exists(src):
        return None
    spec = importlib.util.spec_from_file_location('espn_leadlag', src)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.taker_fee


def self_test():
    fails = []

    def check(name, ok, detail=''):
        print('%s %s%s' % ('OK  ' if ok else 'FAIL', name, (': ' + str(detail)) if detail else ''))
        if not ok:
            fails.append(name)

    # gw/fees.h's cent rule (deskd-fees' examples), the micro-dollar ceiling first
    check('fee: 5 contracts at 40c, mult 1: 8.4c rounds up to $0.09', kalshi_fee_order_cent(4000, 500, 1.0) == 900)
    check('fee: 1 at 50c: 1.75c -> $0.02', kalshi_fee_order_cent(5000, 100, 1.0) == 200)
    check('fee: exactly 7c stays 7c (4 at 50c); 5 at 50c ($0.0875) is 9c',
          kalshi_fee_order_cent(5000, 400, 1) == 700 and kalshi_fee_order_cent(5000, 500, 1) == 900)
    check('fee: mult 0.5 halves the raw fee before rounding (4 at 45c: 3.465c -> $0.04; 5 at 40c: $0.05)',
          kalshi_fee_order_cent(4500, 400, 0.5) == 400 and kalshi_model_fee(0.5, 4500, 400) == 34650
          and kalshi_fee_order_cent(4000, 500, 0.5) == 500)
    check('fee: 0.07 x 0.5 x 0.5 is 17,500 micro-dollars; 0.1c of a contract rounds up to one',
          kalshi_model_fee(1, 5000, 100) == 17500 and kalshi_model_fee(1, 10, 1) == 1)
    check('fee: the multiplier in thousandths, rounded up with 1e-6 slack', fee_milli(0.1 * 3) == 300
          and fee_milli(0.0001) == 1 and fee_milli(float('nan')) == 0 and fee_milli(-1) == 0 and fee_milli(None) == 0)
    check('fee: none at 0 or $1, or without a multiplier', kalshi_fee_order_cent(0, 500, 1) == 0
          and kalshi_fee_order_cent(ONE, 500, 1) == 0 and kalshi_fee_order_cent(4000, 500, 0) == 0)
    old = research_fee()
    if old is None:
        print('     research/espn_leadlag.py: not present, its taker_fee check skipped')
    else:   # the old study's float fee, which x1_backtest's numbers are compared with
        worst = max(abs(kalshi_fee_order_cent(c * 100, n * CT, m) / ONE - old(c / 100, n, m))
                    for c in range(1, 100) for n in range(1, 21) for m in (0.5, 1.0))
        check("research/espn_leadlag.py's taker_fee agrees at every cent price, 1 to 20 contracts", worst < 1e-12,
              worst)

    # clip_count (test_take.cpp's clip_counts)
    check('clip_count: $2 at 48c buys 4 ($1.92 and a 7c fee), at 97c 2; $1 at 99c 1; $0.50 at 60c none',
          clip_count(2 * ONE, 4800, 1.0) == 400 and clip_count(2 * ONE, 9700, 1.0) == 200
          and clip_count(ONE, 9900, 1.0) == 100 and clip_count(5000, 6000, 1.0) == 0)
    check('clip_count: fractions in 0.01 steps; max_count; the multiplier; 0 when none fits',
          clip_count(2 * ONE, 4500, 1.0, 0, False) == 426 and clip_count(2 * ONE, 3000, 1.0, 0, False) == 633
          and clip_count(2 * ONE, 3000, 1.0) == 600 and clip_count(2 * ONE, 3000, 1.0, 450) == 400
          and clip_count(2 * ONE, 4800, 0.5) == 400 and clip_count(2 * ONE, 4800, 0, 0, False) == 416
          and clip_count(4000, 4500, 1.0) == 0)
    check('clip_count: no price, no clip', clip_count(2 * ONE, 0, 1) == 0 and clip_count(2 * ONE, ONE, 1) == 0
          and clip_count(0, 4800, 1) == 0 and clip_count(-2 * ONE, 4800, 1) == 0)
    largest = all(px * c + 100 * kalshi_fee_order_cent(px, c, 1) <= clip * 100
                  and px * (c + step) + 100 * kalshi_fee_order_cent(px, c + step, 1) > clip * 100
                  for px in (100, 1700, 4500, 5000, 8300, 9900) for clip in (ONE, 2 * ONE, 5 * ONE)
                  for step in (CT, 1) for c in (clip_count(clip, px, 1, 0, step == CT),))
    check('clip_count: the count is the largest that fits, one more step never does', largest)

    # the defaults
    check('params: COLLEGE is 20c net, $2, $10 a game, thin under 20k, stale touches only',
          params('COLLEGE')['edge'] == 2000 and params('COLLEGE')['clip'] == 20000
          and params('COLLEGE')['game_cap'] == 10000000 and params('COLLEGE')['max_volume'] == 2000000
          and params('COLLEGE')['stale_only'] is True)
    n = params('MODEL')
    check("params: MODEL is nba.json's (10c, 4 contracts, 4 takes 10 s apart, 30 s, 3c, 5-95c, no OT, 2:00)",
          (n['edge'], n['max_count'], n['max_takes'], n['signal_ms'], n['cool_ms'], n['max_spread'], n['min_px'],
           n['max_px'], n['trade_ot'], n['min_t_left_s']) == (1000, 400, 4, 30000, 10000, 300, 500, 9500, False, 120.0))
    check('params: FOLLOW and FADE are MODEL with their own mode; $10 a game for every lane',
          dict(params('ESPN_FOLLOW'), mode='MODEL') == n == dict(params('ESPN_FADE'), mode='MODEL')
          and all(params(m)['game_cap'] == 10000000 for m in MODES))
    check('the pull codes: 31, each once', len(PULL_CODES) == 31 == len(set(PULL_CODES)))

    # every hand-worked case of the fixtures deskd-take asserts, and their params are take_params'
    total = 0
    for name, least in zip(FIXTURES, (40, 25, 35)):
        path = os.path.join(ROOT, 'tests', 'deskd', 'fixtures', name)
        if not os.path.exists(path):
            check('%s is present' % name, False, path)
            continue
        cases, bad, bad_params = run_cases(path)
        total += cases
        for b in bad[:5]:
            print('     %s %s: got %s, want %s' % (name, b[0], b[1], b[2]))
        check('%s: all %d cases give the recorded decision' % (name, cases), cases >= least and not bad,
              '%d differ' % len(bad))
        check("%s: its params are take_params' defaults" % name, not bad_params, bad_params)
    # what the gate tools read besides the take itself
    cases = {c[0]: c for c in fixture_cases(os.path.join(ROOT, 'tests', 'deskd', 'fixtures', FIXTURES[0]))}
    _, mode, i, p, _ = cases['buy_a_at_the_ask']
    t = decide(i, p).get('take', {})
    check('a take: team price, ref, cost and touch age', (t.get('team_px'), t.get('ref'), t.get('cost'),
          t.get('touch_since_ms')) == (4500, 7000, 4500 * 400 + 100 * 700, 9000000), t)
    _, mode, i, p, _ = cases['touch_after_score_pulls']
    d = decide(i, p)
    check('a pull past the count carries its route (the signal row)', d.get('route') is not None
          and d['route']['side'] == 'BUY' and d['route']['count'] == 400 and has_edge(d, mode), d)
    _, mode, i, p, _ = cases['no_edge']
    d = decide(i, p)
    check('has_edge: no for NO_EDGE, the game checks and FAV_PREGAME before its band',
          not has_edge(d, mode) and not has_edge(_pull('SCORE_OLD', ''), mode)
          and has_edge(_pull('SIZE', '', None, 0), 'FAV_PREGAME') and not has_edge(_pull('SIZE', '', None, 0), mode))
    # the same game listed the other way round is the same take, for the other team (mirrored())
    _, mode, base, p, _ = cases['buy_a_at_the_ask']
    m = dict(base, a=base['b'], b=base['a'], a_score=base['b_score'], b_score=base['a_score'],
             p0=ONE - base['p0'], fair=ONE - base['fair'])
    d, e = decide(base, p).get('take'), decide(m, p).get('take')
    check('mirrored: the same order for the other team', d is not None and e is not None and d['dir'] == 0
          and e['dir'] == 1 and all(d[k] == e[k] for k in ('ticker', 'side', 'price', 'count', 'fee', 'edge')))
    f = decide(dict(m, fired=[False, True]), p)
    check('mirrored: a pull names the mirrored team', f.get('pull') == 'FIRED' and f['dir'] == 1
          and 'AAA' in f['why'], f.get('why'))
    g = decide(dict(base, a=dict(base['a'], book=False)), p)
    check('no book at all is STALE_BOOK', g.get('pull') == 'STALE_BOOK' and 'has no book' in g['why'], g.get('why'))
    # the band is inclusive at both ends: N1's game, thr 1c, A at 0.95 and B's team at 0.05
    with open(os.path.join(ROOT, 'tests', 'deskd', 'fixtures', FIXTURES[2]), encoding='utf-8') as fh:
        nb = input_of(json.load(fh)['base']['MODEL'])
    pn = dict(params('MODEL'), edge=100)
    hi = decide(dict(nb, fair=9900, a=dict(nb['a'], bid=9400, ask=9500), b=dict(nb['b'], bid=400, ask=600)), pn)
    lo = decide(dict(nb, fair=100, a=dict(nb['a'], bid=9400, ask=9600), b=dict(nb['b'], bid=300, ask=500)), pn)
    check('the band [min_px, max_px] is inclusive: BUY A at 0.95, BUY B at 0.05',
          'take' in hi and (hi['take']['side'], hi['take']['price'], hi['take']['dir']) == ('BUY', 9500, 0)
          and 'take' in lo and (lo['take']['side'], lo['take']['price'], lo['take']['dir']) == ('BUY', 500, 1),
          (hi.get('pull'), lo.get('pull')))
    print('take_rule self-test: %s (%d fixture cases)' % ('ok' if not fails else 'FAILED (%d)' % len(fails), total))
    return 1 if fails else 0


def run(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--check', nargs='+', metavar='FILE', help='run every case of these take_cases files')
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.check:
        ap.error('nothing to do: this module is imported; --self-test runs its checks')
    rc = 0
    for path in a.check:
        cases, bad, bad_params = run_cases(path)
        for name, got, want in bad:
            print('%s %s: got %s, want %s' % (os.path.basename(path), name, got, want))
        for mode in bad_params:
            print("%s: params.%s are not take_params' defaults" % (os.path.basename(path), mode))
        print('%s: %d cases, %d differ' % (os.path.basename(path), cases, len(bad)))
        rc = rc or (1 if bad or bad_params else 0)
    return rc


if __name__ == '__main__':
    sys.exit(run(sys.argv[1:]))
