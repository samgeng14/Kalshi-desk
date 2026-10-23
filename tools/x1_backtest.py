#!/usr/bin/env python3
"""X1's backtest and gate (plan 5(e)): does it pay to follow ESPN's win probability, or to fade it?

"Backtest both directions on the ~203 recorded games in data/research/espn_leadlag.sqlite, aligning
ESPN by first-seen time or +41.5 s (its play timestamps look ahead) ... Trade the better direction
as $1-2 IOC takes, as backtested." This tool runs both directions over a threshold grid on the old
desk's recordings, chooses one rule on the earlier dates, measures it on the later ones, then
replays that frozen rule on the NBA games the recorder has caught, and writes gates_dir/X1.json
(strat/lane_gate.h's format) for deskd to load. reports/espn_leadlag.md found FOLLOW losing; the
expected verdict is FAIL or, at best, shadow.

Sources (one is required, and neither has a default: no tool reads data/ unless told to):

  --db FILE   the old desk's extract (research/espn_leadlag.py extract(), from its prod.sqlite;
              on the operator's Mac at data/research/espn_leadlag.sqlite), opened read-only:
                ext_wp(ts, event_ticker, league, state, period, clock, home_score, away_score,
                       wp_home, mkt_home, mkt_away, home_ticker, away_ticker)
                  a row per game in play every 30 s (src/strategies/sports_live.cpp): ts epoch
                  seconds, league ESPN's (nfl, college-football, mlb, nba, ...), clock ESPN's
                  display clock, wp_home ESPN's home win probability or -1 when it had none for
                  the state shown
                quotes(ts, ticker, yes_bid, yes_ask)   the top of book each desk cycle (15-25 s),
                  dollars, no sizes
                markets(ticker, series_ticker, result, status, close_time)
                series(ticker, fee_type, fee_multiplier)
  --data DIR  the recorder's data directory (recorder/store.py): kalshi_markets, kalshi_ws (the
              socket books, replayed by tools/lip_join.py's reader), espn_scores and espn_plays
              (each change at the poll that first showed it) and gaps. The game series of --league
              (default nba: KXNBAGAME), matched to ESPN by tools/espn_match.py, deskd's own rule.

The rule is X1's, exactly: tools/take_rule.py's decide (deskd's take_decide) in mode ESPN_FOLLOW
or ESPN_FADE with take_params' defaults ($2 clips up to 4 contracts, at most 4 takes a game 10 s
apart and $10 at risk, ESPN's number first seen at most 30 s before, spreads of 3c or less, the
team's price in 5-95c, no overtime nor the last 2:00 of regulation), the threshold from the grid
as its edge, and --stake as its clip. tests/deskd/fixtures/x1_golden.json pins it in both
languages.
  FOLLOW   ESPN's probability for a team - its price - the fee a contract >= thr: buy it
  FADE     Kalshi's effective mid for a team (both books) - ESPN's probability >= thr: buy the team
           Kalshi has moved toward
A game is its two markets, A the first by ticker (as TakeEngine); ESPN's number for A's team is its
home probability, or 1 minus it. Decisions run as TakeEngine runs them: a new score state starts a
signal run in which each team is taken once, a decision with no edge 10 s after the last that had
one ends the run, and the game's takes, cool-down and worst case carry over, as does X1's position
in each market, which gw's inventory (take_params': 4 contracts a market) caps a take's count by.
A take buys the team at its ask or sells the other market at its bid, whichever the rule picks.
The fee is take_rule.kalshi_fee_order_cent (gw/fees.h, rounded up to the cent per order;
research/espn_leadlag.py's taker_fee gives the same at cent prices); a market's multiplier is its
series' (the DB; 1.0 where it has none, as that study took it) or --fee-mult (the recorder has no
series rows: 1.0, TO CONFIRM for KXNBAGAME). A league without a game clock (mlb) cannot apply the
2:00 rule: its games run with min_t_left_s 0 and a period past the ninth counts as overtime.

When ESPN's number is visible (--align):
  first_seen  (the only alignment the DB supports) the DB: the first ext_wp row showing that
              number for that score state, rows with no number (-1) between not counting (deskd
              keeps a play's first sighting through a summary it missed), and the score state's own
              first row; decisions at the rows (the poll is the row: ESPN between them is unknown).
              The recorder: a play's win probability at the espn_plays row that first had it, a
              late-inserted play (a sequence number under one already seen) dropped; with no
              plays, the scoreboard's last-play probability dated by its score state's first
              sighting (TakeEngine's rule); decisions every second for 30 s from each number, the
              score's poll taken as fresh except inside an ESPN outage in the gaps table (any
              ESPN feed's: a recovered row does not name its league)
  wallclock   (--data only) a play at the running maximum of its game's play wallclocks + --lag-s
              (41.5 s), a play whose wallclock falls under that maximum dropped
--delay-s D shifts everything ESPN showed D seconds later against the books (a sensitivity).

Entries. A take fills at the touch it saw (an IOC; the DB's touch is taken as deep enough), the
quote at or before the decision. The conservative variant, which the verdict uses: the worse of
that touch and the market's next quote after it; a next quote without that side, or none, is no
fill (counted). Scores, each per trade in dollars, net of the fee: the 60 s and 300 s markouts
(the team's mid then, from its own book or 1 minus the other's, less its entry price, times the
contracts, less the fee; a DB mid at most 90 s old) and the settlement P&L. Means with t and an 80%
lower bound clustered by game.

Mapping repair (the DB): games whose ESPN home probability moved against the mid of the ticker
the desk called home (correlation under -0.3 over 10+ samples) had home and away swapped
(reports/espn_leadlag.md); they are corrected, and the chosen rule is also run as recorded.

Split and choice. Games split by date: the earliest dates holding two thirds of the games train,
the rest test. On train, the rule with the best mean 60 s markout of those with 10+ trades (ties:
more trades, then the lower threshold). On test it must have 30+ trades, a mean 60 s markout >= 0
and a mean settlement P&L >= 0 (plan 5(e)'s "a reading that is not negative"), else FAIL. With
--data too, the same frozen rule on the recorder's games: 30+ games with a number and both books,
30+ trades, both means >= 0, and the stage is live; otherwise shadow.

The gate file (--write-gate FILE, refuses an existing one without --force), as deskd's
load_lane_gate(path, "X1", "", {"direction", "thr"}) reads it: lane X1, verdict PASS or FAIL (with
why), stage live or shadow, params {direction: follow or fade, thr: dollars}, no model_sha256
(X1 has no model), the numbers in evidence (the recorder's fee multiplier among them). It measures
X1 as deskd trades it, so --write-gate refuses a --delay-s other than 0 (a negative one looks
ahead) and a --stake other than take_params' $2 clip: the params X1 reads carry neither.

Prints the games by league first (the NBA is expected to be 0 in the DB), the grid on train, the
choice, its test and the verdict; --json the whole result.

Usage:
  python3 tools/x1_backtest.py --db FILE [--league L] [--delay-s 0] [--stake 2] [--grid 0.03,...]
                               [--data DIR [--align first_seen|wallclock] [--lag-s 41.5]]
                               [--json] [--write-gate FILE [--force]]
  python3 tools/x1_backtest.py --data DIR --direction fade --thr 0.10 [--league nba] [...]
  python3 tools/x1_backtest.py --self-test
"""
import argparse
import bisect
import contextlib
import importlib.util
import io
import json
import math
import os
import random
import sqlite3
import statistics
import sys
import tempfile
from collections import Counter, defaultdict, namedtuple
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import espn_match as em   # noqa: E402  (deskd's matching and ESPN's clock)
import lip_join as lj     # noqa: E402  (the recorder's reader and the book replay)
import take_rule as tr    # noqa: E402  (X1's rule: decide and the cent fee)
import train_bball as tb  # noqa: E402  (the basketball clock)

ONE, CT = tr.ONE, tr.CT
DIRECTIONS = ('follow', 'fade')
MODE = {'follow': 'ESPN_FOLLOW', 'fade': 'ESPN_FADE'}
GRID = (0.03, 0.05, 0.08, 0.10, 0.12, 0.15, 0.20)
HORIZONS_S = (60, 300)
LAG_S = 41.5
STAKE = 2.0
FEE_MULT = 1.0             # --data: KXNBAGAME's multiplier until a series row says otherwise (TO CONFIRM)
DB_QUOTE_AGE_MS = 30_000   # a DB quote older than this is no book (the desk read them every 15-25 s)
DB_MID_AGE_MS = 90_000     # a markout's DB mid at most this old (research/espn_leadlag.py's Book.mid)
STEP_MS = 1000             # --data: a decision every second inside a number's signal window
DATA_NEXT_MS = 1000        # --data: the conservative entry's next quote is a change within a second
QUIET_MS = 10_000          # TakeOptions::run_quiet_ms
DEEP = 1_000_000 * CT      # the DB's touch size: not recorded, taken as deep enough
MIN_TRAIN, MIN_TEST = 10, 30
MIN_NBA_GAMES, MIN_NBA_TRADES = 30, 30
GATE_PARAMS = ('direction', 'thr')

# The extract's tables, as research/espn_leadlag.py extract() creates them (untyped: the values
# keep the types prod.sqlite gave them); the self-test checks this against extract() itself
EXTRACT_DDL = (
    'CREATE TABLE ext_wp(ts, event_ticker, league, state, period, clock, home_score, away_score, wp_home, '
    'mkt_home, mkt_away, home_ticker, away_ticker)',
    'CREATE TABLE quotes(ts, ticker, yes_bid, yes_ask)',
    'CREATE TABLE markets(ticker, series_ticker, result, status, close_time)',
    'CREATE TABLE series(ticker, fee_type, fee_multiplier)',
    'CREATE INDEX iq ON quotes(ticker, ts)')

# The game clock by ESPN league, for train_bball.t_left: periods, their length, overtime's.
# Leagues without a clock: their regulation periods (a later one is overtime).
_NBA = tb.LEAGUES['nba']
CLOCKS = {
    'nba': _NBA,
    'wnba': _NBA._replace(key='wnba', model=None, series='KXWNBAGAME', regulation_s=2400.0, period_s=600.0),
    'mens-college-basketball': _NBA._replace(key='mens-college-basketball', model=None, series='KXNCAAMBGAME',
                                             regulation_s=2400.0, period_s=1200.0, periods=2),
    'womens-college-basketball': tb.LEAGUES['wcbb'],
    'nfl': _NBA._replace(key='nfl', model=None, series='KXNFLGAME', regulation_s=3600.0, period_s=900.0, ot_s=600.0),
    'college-football': _NBA._replace(key='college-football', model=None, series='KXNCAAFGAME', regulation_s=3600.0,
                                      period_s=900.0, ot_uses_clock=False),
    'nhl': _NBA._replace(key='nhl', model=None, series='KXNHLGAME', regulation_s=3600.0, period_s=1200.0, periods=3),
}
NO_CLOCK = {'mlb': 9}


def clock_state(league, period, clock):
    """(t_left_s, ot) as take_decide reads them: regulation time left (None when unread) and
    whether the game is past regulation."""
    lg = CLOCKS.get(league)
    try:
        period = int(period or 0)
    except (TypeError, ValueError):
        period = 0
    if lg is not None:
        c = tb.t_left(lg, period, em.clock_seconds(clock or ''))
        return (None, False) if c is None else (float(c[0]), bool(c[1]))
    if league in NO_CLOCK:
        return None, period > NO_CLOCK[league]
    return None, False


def rule_params(direction, thr, stake=STAKE, league=None):
    """X1's TakeParams for one rule: take_params' defaults, thr (dollars) as the edge, the stake as
    the clip; a league without a clock without the 2:00 rule."""
    p = tr.params(MODE[direction])
    p['edge'] = int(round(thr * ONE))
    p['clip'] = int(round(stake * ONE))
    if league in NO_CLOCK:
        p['min_t_left_s'] = 0.0
    return p


def to_px(p):
    return int(round(min(1.0, max(0.0, float(p))) * ONE))


def first_seen(keys, times):
    """Each row's first sighting: the time of the first row of its run of equal keys."""
    out, prev, seen = [], object(), None
    for k, t in zip(keys, times):
        if k != prev:
            prev, seen = k, t
        out.append(seen)
    return out


def number_seen(numbers, states, times):
    """Each row's ESPN number's first sighting: the first row of its run of that number in its
    score state, where a row without a number (None) does not end the run, as deskd keeps a play's
    first sighting through a summary it missed; 0 for a row without one."""
    out, key, seen = [], None, 0
    for p, s, t in zip(numbers, states, times):
        if p is not None and (p, s) != key:
            key, seen = (p, s), t
        out.append(0 if p is None else seen)
    return out


# ---- the books ------------------------------------------------------------------------------
Quote = namedtuple('Quote', 't bid bid_qty ask ask_qty')   # t ms; bid/ask Px or None (that side empty)
_NOTHING = object()


class Book:
    """Each market's top of book over time (epoch ms), as recorded: the DB's quotes (no sizes) or
    the recorder's socket books. A None entry is a book dropped (a gap, a disconnect) until the
    next. Lookups take the last entry at or before t, never a later one."""

    def __init__(self):
        self.ts, self.qs = defaultdict(list), defaultdict(list)

    def add(self, tk, t, q):
        self.ts[tk].append(t)
        self.qs[tk].append(q)

    def last(self, tk):
        qs = self.qs.get(tk)
        return qs[-1] if qs else _NOTHING

    def at(self, tk, t, max_age_ms=None):
        ts = self.ts.get(tk)
        if not ts:
            return None
        i = bisect.bisect_right(ts, t) - 1
        if i < 0:
            return None
        q = self.qs[tk][i]
        if q is None or (max_age_ms is not None and t - q.t > max_age_ms):
            return None
        return q

    def next_after(self, tk, t):
        """(time, entry) of the first entry strictly after t (the entry None: a drop); None when
        nothing later was recorded."""
        ts = self.ts.get(tk)
        if not ts:
            return None
        i = bisect.bisect_right(ts, t)
        return (ts[i], self.qs[tk][i]) if i < len(ts) else None

    def mid(self, tk, t, max_age_ms=None):
        """The two-sided mid in dollars, else None."""
        q = self.at(tk, t, max_age_ms)
        if q is None or q.bid is None or q.ask is None or q.bid >= q.ask:
            return None
        return (q.bid + q.ask) / 2 / ONE

    @classmethod
    def from_db(cls, db):
        book = cls()
        for ts, tk, b, a in db.execute('SELECT ts, ticker, yes_bid, yes_ask FROM quotes ORDER BY ticker, ts'):
            t = int(ts) * 1000
            book.add(tk, t, Quote(t, db_px(b), None, db_px(a), None))
        return book


def db_px(v):
    """A DB price in dollars as Px; None for an empty side (a 0 bid, an ask of 0 or $1)."""
    if v is None:
        return None
    p = int(round(float(v) * ONE))
    return p if 0 < p < ONE else None


class TopReplay(lj.Replay):
    """lip_join's replay of kalshi_ws, keeping each kept market's top of book in a Book whenever
    it changes, and a None when its book is dropped. A drop at a disconnect is dated by the last
    row before it (early, never late)."""

    def __init__(self, keep, book):
        super().__init__(keep)
        self.book = book
        self.t_ms = 0
        self.on_drop = self._top

    def _top(self, tk):
        b = self.books.get(tk)
        q = None
        if b is not None:
            bid, ask = b.best_bid(), b.best_ask()
            q = Quote(self.t_ms, bid, b.bids[bid] if bid is not None else 0, ask, b.asks[ask] if ask is not None else 0)
        last = self.book.last(tk)
        if q is None and (last is _NOTHING or last is None):
            return
        if q is not None and last not in (_NOTHING, None) and tuple(last[1:]) == tuple(q[1:]):
            return
        self.book.add(tk, self.t_ms, q)

    def row(self, r):
        t = r.get('recv_us')
        if isinstance(t, int) and not isinstance(t, bool):
            self.t_ms = t // 1000
        super().row(r)
        tk = r.get('ticker')
        if tk and (self.keep is None or self.keep(tk)):
            self._top(tk)


# ---- the games --------------------------------------------------------------------------------
class Game:
    """One game: A and B (A's YES pays if A's team wins; A the first ticker), whether A is ESPN's
    home team, the settlements and fee multipliers, and its decision instants: (t, the input's
    parts that do not depend on the lane's run) from instants()."""

    def __init__(self, event, league, date, tickers, a_home, results, fee_mult):
        self.event, self.league, self.date = event, league, date
        self.tickers, self.a_home = tickers, a_home
        self.results, self.fee_mult = results, fee_mult
        self.points = []       # (t, state, t_left_s, ot, a_score, b_score, espn Px or None, first_seen, espn_seen, fetched)
        self.max_age_ms = None   # a quote older than this is no book (None: a book lasts until dropped)
        self.next_ms = None      # the conservative entry's next quote: within this (None: the next, whenever)

    def team_result(self, d):
        """'yes' or 'no' for team d's market, from either market's settlement; None unknown."""
        r = self.results.get(self.tickers[d])
        if r in ('yes', 'no'):
            return r
        o = self.results.get(self.tickers[1 - d])
        return {'yes': 'no', 'no': 'yes'}.get(o)

    def leg(self, book, i, t):
        tk = self.tickers[i]
        q = book.at(tk, t, self.max_age_ms)
        leg = {'ticker': tk, 'book': q is not None, 'stale': False, 'bid': None, 'bid_qty': 0, 'ask': None,
               'ask_qty': 0, 'fee_mult': self.fee_mult.get(tk), 'fractional': False, 'position': 0,
               'sys_position': 0, 'sys_buying': 0, 'sys_selling': 0, 'bid_since_ms': 0, 'ask_since_ms': 0}
        if q is not None:
            leg.update(bid=q.bid, ask=q.ask, bid_since_ms=q.t, ask_since_ms=q.t,
                       bid_qty=DEEP if q.bid_qty is None else q.bid_qty, ask_qty=DEEP if q.ask_qty is None else q.ask_qty)
        return leg, q

    def input_at(self, book, point):
        """(t, take_decide's input but the run's fields, the quote each leg read)."""
        t, state, t_left, ot, a_sc, b_sc, espn, seen, espn_seen, fetched = point
        (la, qa), (lb, qb) = self.leg(book, 0, t), self.leg(book, 1, t)
        inp = {'a': la, 'b': lb, 'state': state, 'src': 'ESPN', 'score_why': '', 'fetched_ms': fetched,
               'first_seen_ms': seen, 'prev_poll_ms': 0, 'start_ms': 0, 'a_score': a_sc, 'b_score': b_sc,
               't_left_s': t_left, 'ot': ot, 'points': True, 'set_end_ms': 0, 'p0': None, 'fair': None,
               'fair_why': '', 'espn': espn, 'espn_seen_ms': espn_seen, 'now_ms': t, 'private_up': True,
               'not_tradable': '', 'volume': 0}
        return t, inp, {self.tickers[0]: qa, self.tickers[1]: qb}

    def instants(self, book):
        for point in self.points:
            yield self.input_at(book, point)


class Run:
    """TakeEngine's per-game state for one lane: the signal run and what the game has used."""

    def __init__(self):
        self.run_state, self.edge_ms = None, 0
        self.fired = [False, False]
        self.last_take_ms, self.takes, self.used = 0, 0, 0
        self.pos = Counter()


def simulate(game, book, p):
    """Every take the rule makes in one game, TakeEngine's way (module docstring): [trade dict]."""
    return simulate_lanes(game, book, [('X1', p)])['X1']


def simulate_lanes(game, book, lanes):
    """Every take each lane's rule makes in one game, the lanes [(lane, params)] replayed together:
    at each instant each decides in turn, in the order given (strat steps X1's engine, then N1's,
    then N2's), each its own TakeEngine run and its own position. Systematic's position in each
    market, which gw's inventory caps (the legs' sys_position), is every replayed lane's takes:
    another lane that trades the game's markets and is not replayed is not in it. IOCs leave none
    working. {lane: [trade dict]}."""
    runs = {lane: Run() for lane, _ in lanes}
    held = Counter()   # Systematic's position by market, every lane's
    trades = {lane: [] for lane, _ in lanes}
    for t, inp, quotes in game.instants(book):
        for lane, p in lanes:
            run = runs[lane]
            if inp['first_seen_ms'] != run.run_state:   # a new score state starts a new run
                run.run_state = inp['first_seen_ms']
                run.fired = [False, False]
            x = dict(inp, a=dict(inp['a']), b=dict(inp['b']))
            for i, k in ((0, 'a'), (1, 'b')):
                x[k]['position'] = run.pos[game.tickers[i]]
                x[k]['sys_position'] = held[game.tickers[i]]
            x.update(fired=list(run.fired), last_take_ms=run.last_take_ms, game_takes=run.takes, game_used=run.used)
            d = tr.decide(x, p)
            if tr.has_edge(d, p['mode']):
                run.edge_ms = t
            elif d['dir'] >= 0 and t - run.edge_ms >= QUIET_MS:
                run.fired = [False, False]
            o = d.get('take')
            if o is None:
                continue
            q = quotes[o['ticker']]
            trades[lane].append(dict(o, game=game.event, league=game.league, date=game.date, t=t, quote_ms=q.t,
                                     mult=game.fee_mult.get(o['ticker'])))
            run.fired[o['dir']] = True
            run.last_take_ms, run.takes, run.used = t, run.takes + 1, run.used + o['cost']
            n = o['count'] if o['side'] == 'BUY' else -o['count']
            run.pos[o['ticker']] += n
            held[o['ticker']] += n
    return trades


# ---- scoring ------------------------------------------------------------------------------------
def score_trade(trade, game, book, conservative, mid_age_ms=None):
    """A trade's entry, its net 60 s and 300 s markouts and its settlement P&L, per trade in
    dollars (module docstring); None when the conservative entry finds no fill."""
    buy = trade['side'] == 'BUY'
    price = trade['price']
    if conservative:
        nq = book.next_after(trade['ticker'], trade['t'])
        if nq is not None and (game.next_ms is None or nq[0] <= trade['t'] + game.next_ms):
            nxt = None if nq[1] is None else (nq[1].ask if buy else nq[1].bid)
            if nxt is None:
                return None   # the side, or the book, gone before the order could arrive
            price = max(price, nxt) if buy else min(price, nxt)
        elif game.next_ms is None:
            return None       # the DB: no later quote bounds the fill
    team_px = price if buy else ONE - price
    if not 0 < team_px < ONE:
        return None
    count = trade['count']
    fee = tr.kalshi_fee_order_cent(team_px, count, trade['mult'])
    c, px, fee_d = count / CT, team_px / ONE, fee / ONE
    out = {'entry': team_px, 'fee': fee}
    d = trade['dir']
    for h in HORIZONS_S:
        t = trade['t'] + h * 1000
        m = book.mid(game.tickers[d], t, mid_age_ms)
        if m is None:
            o = book.mid(game.tickers[1 - d], t, mid_age_ms)
            m = None if o is None else 1 - o
        out['mo%d' % h] = None if m is None else (m - px) * c - fee_d
    res = game.team_result(d)
    out['settle'] = None if res is None else (c if res == 'yes' else 0.0) - c * px - fee_d
    return out


def summarize(rows):
    """rows [(game, dollars)]: n, games, total, mean, sd, and the standard error of the mean, t
    and 80% lower bound clustered by game (None where undefined)."""
    n = len(rows)
    if n == 0:
        return {'n': 0, 'games': 0, 'total': 0.0, 'mean': None, 'sd': None, 'se': None, 't': None, 'lower80': None}
    vals = [v for _, v in rows]
    mean = sum(vals) / n
    dev = defaultdict(float)
    for g, v in rows:
        dev[g] += v - mean
    k = len(dev)
    se = math.sqrt(k / (k - 1) * sum(x * x for x in dev.values())) / n if k > 1 else None
    return {'n': n, 'games': k, 'total': sum(vals), 'mean': mean, 'sd': statistics.stdev(vals) if n > 1 else 0.0,
            'se': se, 't': mean / se if se else None, 'lower80': mean - 1.2816 * se if se is not None else None}


def evaluate(trades, games, book, conservative=True, mid_age_ms=None):
    rows = {'mo60': [], 'mo300': [], 'settle': []}
    unfilled = 0
    for t in trades:
        s = score_trade(t, games[t['game']], book, conservative, mid_age_ms)
        if s is None:
            unfilled += 1
            continue
        for k in rows:
            if s[k] is not None:
                rows[k].append((t['game'], s[k]))
    out = {'trades': len(trades) - unfilled, 'unfilled': unfilled, 'games': len({t['game'] for t in trades})}
    out.update({k: summarize(v) for k, v in rows.items()})
    return out


def split_by_date(games):
    """(train dates, test dates): the earliest dates holding two thirds of the games train."""
    per = Counter(g.date for g in games)
    dates = sorted(per)
    need, cum, k = 2 * len(games) / 3, 0, 0
    while k < len(dates) and cum < need:
        cum += per[dates[k]]
        k += 1
    if k == len(dates) and len(dates) > 1:
        k -= 1
    return set(dates[:k]), set(dates[k:])


def choose(train):
    """The best rule on train: {(direction, thr): stats} -> (direction, thr) or None, and each
    direction's best."""
    best, per = None, {}
    for (d, thr), s in train.items():
        m = s['mo60']
        if m['n'] < MIN_TRAIN or m['mean'] is None:
            continue
        key = (m['mean'], m['n'], -thr)
        if best is None or key > best[0]:
            best = (key, (d, thr))
        if d not in per or key > per[d][0]:
            per[d] = (key, (d, thr))
    return (best[1] if best else None), {d: v[1] for d, v in per.items()}


def judge(s, min_n, what):
    """'' when the stats are not negative over min_n trades, else why not."""
    mo, st = s['mo60'], s['settle']
    if mo['n'] < min_n:
        return '%s: %d trades with a 60 s markout, under %d' % (what, mo['n'], min_n)
    if mo['mean'] < 0:
        return '%s: the mean net 60 s markout is %s a trade' % (what, _usd(mo['mean']))
    if st['n'] < min_n:
        return '%s: %d settled trades, under %d' % (what, st['n'], min_n)
    if st['mean'] < 0:
        return '%s: the mean settlement P&L is %s a trade' % (what, _usd(st['mean']))
    return ''


def _usd(x):
    return '%s$%.4f' % ('-' if x < 0 else '', abs(x))


# ---- the DB ---------------------------------------------------------------------------------------
def _corr(x, y):
    mx, my = sum(x) / len(x), sum(y) / len(y)
    sxy = sum((a - mx) * (b - my) for a, b in zip(x, y))
    sx, sy = math.sqrt(sum((a - mx) ** 2 for a in x)), math.sqrt(sum((b - my) ** 2 for b in y))
    return sxy / (sx * sy)


def swapped_games(samples, book):
    """research/espn_leadlag.py's: games where the desk's ESPN-to-Kalshi mapping had home and away
    the wrong way round, found as ESPN's home probability moving against the Kalshi mid of the
    ticker the desk called home (a within-game correlation under -0.3 over 10+ samples)."""
    out = {}
    for ev, rows in samples.items():
        pairs = []
        for r in rows:
            if r['state'] != 'in' or r['wp'] is None or r['wp'] < 0:
                continue
            m = book.mid(r['home_ticker'], r['ts'] * 1000, 60_000)
            if m is not None:
                pairs.append((r['wp'], m))
        if len(pairs) < 10:
            continue
        w, m = [a for a, _ in pairs], [b for _, b in pairs]
        if statistics.pstdev(w) < 1e-6 or statistics.pstdev(m) < 1e-6:
            continue
        r = _corr(w, m)
        if r < -0.3:
            out[ev] = r
    return out


def corrected(samples, swapped):
    """The samples with the swapped games' home and away tickers exchanged."""
    return {ev: [dict(r, home_ticker=r['away_ticker'], away_ticker=r['home_ticker']) for r in rows]
            if ev in swapped else rows for ev, rows in samples.items()}


def read_db(path):
    db = sqlite3.connect('file:%s?mode=ro' % os.path.abspath(path), uri=True)
    try:
        samples = defaultdict(list)
        for row in db.execute('SELECT ts, event_ticker, league, state, period, clock, home_score, away_score, wp_home, '
                              'home_ticker, away_ticker FROM ext_wp ORDER BY event_ticker, ts'):
            ts, ev, lg, st, per, clk, hs, as_, wp, ht, at = row
            if ts is None or not ev:
                continue
            samples[ev].append({'ts': int(ts), 'league': lg or '', 'state': st or '', 'period': per, 'clock': clk,
                                'home_score': int(hs or 0), 'away_score': int(as_ or 0),
                                'wp': None if wp is None else float(wp), 'home_ticker': ht, 'away_ticker': at})
        book = Book.from_db(db)
        results = {t: r for t, r in db.execute("SELECT ticker, result FROM markets WHERE result IN ('yes','no')")}
        by_series = {s: (m if m is not None else 1.0) for s, m in db.execute('SELECT ticker, fee_multiplier FROM series')}
        mult = {t: float(by_series.get(s, 1.0)) for t, s in db.execute('SELECT ticker, series_ticker FROM markets')}
    finally:
        db.close()
    return samples, book, results, mult


def game_date(event, t_ms):
    return em.kalshi_event_date(event) or em.et_date(t_ms)


def db_games(samples, results, mult, delay_ms):
    """A Game per event with both tickers; decisions at its ext_wp rows (shifted by the delay)."""
    games = {}
    for ev, rows in samples.items():
        pair = next(((r['home_ticker'], r['away_ticker']) for r in rows if r['home_ticker'] and r['away_ticker']), None)
        if pair is None or pair[0] == pair[1]:
            continue
        rows = [r for r in rows if (r['home_ticker'], r['away_ticker']) == pair]
        a, b = sorted(pair)
        a_home = a == pair[0]
        g = Game(ev, rows[0]['league'], game_date(ev, rows[0]['ts'] * 1000), (a, b), a_home,
                 {tk: results.get(tk) for tk in (a, b)}, {tk: mult.get(tk, 1.0) for tk in (a, b)})
        g.max_age_ms = DB_QUOTE_AGE_MS
        times = [r['ts'] * 1000 + delay_ms for r in rows]
        states = [(r['state'], r['period'], r['home_score'], r['away_score']) for r in rows]
        # -1 is no number: the old desk's summary did not match its scoreboard (the display clock
        # included, so often while it runs), which does not make the next row's number a new one
        espn = [None if r['wp'] is None or r['wp'] < 0 else to_px(r['wp'] if a_home else 1 - r['wp']) for r in rows]
        seen, espn_seen = first_seen(states, times), number_seen(espn, states, times)
        for i, r in enumerate(rows):
            t_left, ot = clock_state(g.league, r['period'], r['clock'])
            a_sc, b_sc = (r['home_score'], r['away_score']) if a_home else (r['away_score'], r['home_score'])
            g.points.append((times[i], r['state'], t_left, ot, a_sc, b_sc, espn[i], seen[i], espn_seen[i], times[i]))
        games[ev] = g
    return games


def league_counts(samples, results):
    """Games, rows with an ESPN number and settled tickers by league, the NBA always listed."""
    out = {'nba': {'games': 0, 'with_number': 0, 'settled': 0}}
    for ev, rows in samples.items():
        lg = rows[0]['league'] or '?'
        c = out.setdefault(lg, {'games': 0, 'with_number': 0, 'settled': 0})
        c['games'] += 1
        c['with_number'] += sum(1 for r in rows if r['wp'] is not None and r['wp'] >= 0)
        c['settled'] += len({t for r in rows for t in (r['home_ticker'], r['away_ticker']) if t in results})
    return dict(sorted(out.items(), key=lambda kv: (-kv[1]['games'], kv[0])))


def run_rule(games, book, rule, stake):
    """Every trade of one rule over the games: {event: [trade]}."""
    d, thr = rule
    return {ev: simulate(g, book, rule_params(d, thr, stake, g.league)) for ev, g in games.items()}


def stats_of(by_game, games, book, dates, conservative=True, mid_age_ms=None):
    trades = [t for ev, ts in by_game.items() if games[ev].date in dates for t in ts]
    return evaluate(trades, games, book, conservative, mid_age_ms)


def backtest_db(path, league=None, delay_s=0.0, stake=STAKE, grid=GRID):
    samples, book, results, mult = read_db(path)
    counts = league_counts(samples, results)
    if league:
        samples = {ev: rows for ev, rows in samples.items() if rows and rows[0]['league'] == league}
    swapped = swapped_games(samples, book)
    delay_ms = int(round(delay_s * 1000))
    games = db_games(corrected(samples, swapped), results, mult, delay_ms)
    recorded = db_games(samples, results, mult, delay_ms)
    train_d, test_d = split_by_date(list(games.values()))
    res = {'source': os.path.basename(path), 'leagues': counts, 'league': league, 'games': len(games),
           'swapped': {ev: round(r, 4) for ev, r in sorted(swapped.items())},
           'split': {'train': [min(train_d), max(train_d)] if train_d else None,
                     'test': [min(test_d), max(test_d)] if test_d else None,
                     'train_games': sum(1 for g in games.values() if g.date in train_d),
                     'test_games': sum(1 for g in games.values() if g.date in test_d)},
           'grid': [], 'chosen': None, 'best': {}, 'test': None}
    train, runs = {}, {}
    for d in DIRECTIONS:
        for thr in grid:
            runs[(d, thr)] = run_rule(games, book, (d, thr), stake)
            train[(d, thr)] = stats_of(runs[(d, thr)], games, book, train_d, True, DB_MID_AGE_MS)
            res['grid'].append({'direction': d, 'thr': thr, 'train': train[(d, thr)]})
    chosen, per = choose(train)
    for d, rule in per.items():   # each direction's train best on test: reported, never chosen by
        res['best'][d] = {'thr': rule[1], 'train': train[rule],
                          'test': stats_of(runs[rule], games, book, test_d, True, DB_MID_AGE_MS)}
    if chosen is None:
        res.update(passed=False, why='no rule made %d trades on the train dates' % MIN_TRAIN)
        return res
    res['chosen'] = {'direction': chosen[0], 'thr': chosen[1]}
    as_rec = run_rule(recorded, book, chosen, stake)
    test = {'conservative': stats_of(runs[chosen], games, book, test_d, True, DB_MID_AGE_MS),
            'visible': stats_of(runs[chosen], games, book, test_d, False, DB_MID_AGE_MS),
            'as_recorded': stats_of(as_rec, recorded, book, test_d, True, DB_MID_AGE_MS)}
    by_league = defaultdict(list)
    for ev, ts in runs[chosen].items():
        by_league[games[ev].league].extend(ts)
    test['by_league_all_dates'] = {lg: evaluate(ts, games, book, True, DB_MID_AGE_MS)['mo60']
                                   for lg, ts in sorted(by_league.items())}
    res['test'] = test
    why = judge(test['conservative'], MIN_TEST, 'the test dates')
    res.update(passed=not why, why=why)
    res['trades'] = runs[chosen]   # for the self-test; left out of --json
    return res


# ---- the recorder's data ----------------------------------------------------------------------
def _raw(r):
    try:
        v = json.loads(r.get('raw') or '{}')
    except ValueError:
        return {}
    return v if isinstance(v, dict) else {}


def _num(v):
    return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else None


def p_for_a(home, tie, a_home):
    """ESPN's P(A's team wins) from its home probability and the tie's (soccer; 0 elsewhere)."""
    return to_px(home if a_home else 1.0 - home - (tie or 0.0))


def play_numbers(plays, a_home, align, lag_ms):
    """[(visible ms, ESPN's P(A wins) as Px)] from a game's espn_plays rows, by --align (module
    docstring): a play's chart point at its first row with one; a late-inserted play dropped."""
    first = {}
    for r in sorted(plays, key=lambda r: r['recv_us']):
        if r.get('play_id') and _num(r.get('wp_home')) is not None and r['play_id'] not in first:
            first[r['play_id']] = r
    out = []
    if align == 'first_seen':
        top = None
        for r in sorted(first.values(), key=lambda r: (r['recv_us'], r.get('seq') or 0)):
            seq = r.get('seq')
            if isinstance(seq, int):
                if top is not None and seq < top:
                    continue   # inserted under a play already shown: not what the summary showed then
                top = seq
            out.append((r['recv_us'] // 1000, p_for_a(r['wp_home'], _num(r.get('wp_tie')), a_home)))
        return out
    seen, top = {}, None
    for r in sorted(plays, key=lambda r: r['recv_us']):
        seen.setdefault(r.get('play_id'), r)
    for r in sorted(seen.values(), key=lambda r: (r.get('seq') if isinstance(r.get('seq'), int) else 0, r['recv_us'])):
        w = em.iso_ms(r.get('wallclock') or '')
        if not w:
            continue
        if top is not None and w < top:
            continue   # its wallclock under the running maximum: late
        top = w
        f = first.get(r.get('play_id'))
        if f is not None:
            out.append((w + lag_ms, p_for_a(f['wp_home'], _num(f.get('wp_tie')), a_home)))
    out.sort()
    return out


def sb_number(row, a_home):
    """The scoreboard's last-play probability (situation.lastPlay.probability, TO CONFIRM), or None."""
    prob = ((_raw(row).get('situation') or {}).get('lastPlay') or {}).get('probability')
    home = _num(prob.get('homeWinPercentage')) if isinstance(prob, dict) else None
    if home is None or not 0 <= home <= 1:
        return None
    return p_for_a(home, _num(prob.get('tiePercentage')), a_home)


def grid_of(starts, length_ms, step_ms):
    """Decision instants: every step from each start to start + length, inclusive, merged."""
    out, last = [], None
    for s in sorted(starts):
        t = s if last is None or s > last else last + step_ms
        while t <= s + length_ms:
            out.append(t)
            last = t
            t += step_ms
    return out


class DataGame(Game):
    """A recorded game: its scoreboard rows (each first seen at its recv_us), ESPN's numbers and
    the ESPN outages; decisions every STEP_MS for signal_ms after each number."""

    def __init__(self, *a):
        super().__init__(*a)
        self.states = []    # (t, row, the score state's first sighting)
        self.numbers = []   # (t, Px, its first sighting, None: the score state's)
        self.outages = []   # (from ms, to ms)
        self.signal_ms = tr.params('ESPN_FOLLOW')['signal_ms']
        self.next_ms = DATA_NEXT_MS

    def instants(self, book):
        st_t, nu_t = [s[0] for s in self.states], [n[0] for n in self.numbers]
        for t in grid_of(nu_t, self.signal_ms, STEP_MS):
            i = bisect.bisect_right(st_t, t) - 1
            if i < 0:
                continue
            _, row, seen = self.states[i]
            j = bisect.bisect_right(nu_t, t) - 1
            espn, espn_seen = (None, 0) if j < 0 else (self.numbers[j][1], self.numbers[j][2] or seen)
            fetched = t
            for f, to in self.outages:
                if f <= t <= to:
                    fetched = min(fetched, f)
            t_left, ot = clock_state(self.league, row.get('period'), row.get('clock'))
            hs, as_ = int(row.get('home_score') or 0), int(row.get('away_score') or 0)
            a_sc, b_sc = (hs, as_) if self.a_home else (as_, hs)
            yield self.input_at(book, (t, row.get('state') or '', t_left, ot, a_sc, b_sc, espn, seen, espn_seen, fetched))


def read_data(data, league='nba', align='first_seen', lag_s=LAG_S, delay_s=0.0, fee_mult=FEE_MULT):
    """The recorder's games of one league, matched to ESPN: ({event: DataGame}, Book, counts)."""
    lg = next((x for x in em.LEAGUES if x.league == league), None)
    if lg is None:
        raise ValueError('unknown league %r (espn_match.LEAGUES: %s)' % (league, ', '.join(x.league for x in em.LEAGUES)))
    path, delay_ms, lag_ms = lg.sport + '/' + lg.league, int(round(delay_s * 1000)), int(round(lag_s * 1000))
    markets = {}
    for r in lj.rows(data, 'kalshi_markets'):
        tk, t = r.get('ticker') or '', r.get('recv_us') or 0
        if not tk.startswith(lg.series + '-') or (tk in markets and markets[tk]['recv_us'] > t):
            continue
        raw = _raw(r)
        markets[tk] = {'recv_us': t, 'ticker': tk, 'event_ticker': raw.get('event_ticker') or tk[:tk.rfind('-')],
                       'yes_sub_title': raw.get('yes_sub_title') or '', 'result': r.get('result') or raw.get('result')}
    kgames = em.kalshi_games(sorted(markets.values(), key=lambda m: m['ticker']))
    scores, plays = defaultdict(list), defaultdict(list)
    for table, into in (('espn_scores', scores), ('espn_plays', plays)):
        for r in lj.rows(data, table):
            if (r.get('league') or '').split(':')[0] == path and r.get('game_id') not in (None, '') \
                    and isinstance(r.get('recv_us'), int):
                into[str(r['game_id'])].append(r)
    for rows in scores.values():
        rows.sort(key=lambda r: r['recv_us'])
    espn_games = [em.game_from_score_row(rows[-1]) for rows in scores.values()]
    outages = []
    for r in lj.rows(data, 'gaps'):
        if (r.get('source'), r.get('kind')) in (('espn', 'poll_failed'), ('espn', 'recovered'), ('recorder', 'restart')) \
                and isinstance(r.get('from_us'), int) and isinstance(r.get('to_us'), int):
            outages.append((r['from_us'] // 1000 + delay_ms, r['to_us'] // 1000 + delay_ms))
    games, counts = {}, Counter(listed=len(kgames))
    for ev, k in sorted(kgames.items()):
        if not k.ticker_b:
            continue
        m = em.match_game(k, espn_games)
        if m is None:
            continue
        counts['matched'] += 1
        tickers = (k.ticker_a, k.ticker_b)
        g = DataGame(ev, league, k.date, tickers, m.a_home, {tk: markets[tk]['result'] for tk in tickers},
                     {tk: fee_mult for tk in tickers})
        g.outages = outages
        rows = scores[m.id]
        times = [r['recv_us'] // 1000 + delay_ms for r in rows]
        seen = first_seen([(r.get('state'), r.get('period'), r.get('home_score'), r.get('away_score')) for r in rows],
                          times)
        g.states = list(zip(times, rows, seen))
        g.numbers = [(t + delay_ms, p, t + delay_ms) for t, p in play_numbers(plays[m.id], m.a_home, align, lag_ms)]
        if not g.numbers and align == 'first_seen':   # no summary plays: the scoreboard's number
            prev = None
            for t, r in zip(times, rows):
                p = sb_number(r, m.a_home)
                if p is not None and p != prev:
                    g.numbers.append((t, p, None))
                prev = p
        if g.numbers:
            counts['with_numbers'] += 1
        games[ev] = g
    book = Book()
    keep = {tk for g in games.values() for tk in g.tickers}
    rp = TopReplay(lambda tk: tk in keep, book)
    lj.drive(data, rp, 3600 * 1_000_000, lambda t: None, lj.down_instants(data))
    usable = {ev: g for ev, g in games.items() if g.numbers and all(book.ts.get(tk) for tk in g.tickers)}
    counts['recorded'] = len(usable)
    return usable, book, dict(counts)


def backtest_data(data, league='nba', align='first_seen', lag_s=LAG_S, delay_s=0.0, stake=STAKE, fee_mult=FEE_MULT,
                  rule=None, grid=GRID):
    games, book, counts = read_data(data, league, align, lag_s, delay_s, fee_mult)
    out = {'source': os.path.basename(os.path.normpath(data)), 'league': league, 'align': align,
           'lag_s': lag_s if align == 'wallclock' else None, 'fee_mult': fee_mult, 'counts': counts}
    everything = {g.date for g in games.values()}
    rules = [rule] if rule else [(d, thr) for d in DIRECTIONS for thr in grid]
    out['rules'] = []
    for r in rules:
        by_game = run_rule(games, book, r, stake)
        s = stats_of(by_game, games, book, everything, True)
        s['visible'] = stats_of(by_game, games, book, everything, False)['mo60']
        out['rules'].append({'direction': r[0], 'thr': r[1], 'stats': s})
    if rule:
        s = out['rules'][0]['stats']
        why = ('%d recorded games, under %d' % (counts['recorded'], MIN_NBA_GAMES)
               if counts['recorded'] < MIN_NBA_GAMES else judge(s, MIN_NBA_TRADES, 'the recorded games'))
        out.update(passed=not why, why=why)
    return out


# ---- the run --------------------------------------------------------------------------------------
def backtest(db=None, data=None, league=None, align='first_seen', lag_s=LAG_S, delay_s=0.0, stake=STAKE,
             grid=GRID, fee_mult=FEE_MULT, rule=None):
    res = {'stake': stake, 'delay_s': delay_s, 'grid': list(grid), 'db': None, 'nba': None}
    if db:
        res['db'] = backtest_db(db, league, delay_s, stake, grid)
        if res['db']['chosen']:
            c = res['db']['chosen']
            rule = (c['direction'], c['thr'])
    if data and (rule or not db):
        res['nba'] = backtest_data(data, league if league and not db else 'nba', align, lag_s, delay_s, stake,
                                   fee_mult, rule, grid)
    d = res['db']
    if d is not None:
        nba = res['nba']
        res['verdict'] = {'pass': d['passed'], 'why': d['why'], 'rule': d['chosen'],
                          'stage': 'live' if d['passed'] and nba is not None and nba.get('passed') else 'shadow',
                          'nba_why': None if nba is None else nba.get('why')}
    return res


def gate_json(res, computed):
    """The X1 gate file (strat/lane_gate.h), from a run with --db."""
    v, d = res['verdict'], res['db']
    ev = {'db': {k: d[k] for k in ('source', 'leagues', 'league', 'games', 'swapped', 'split', 'chosen')},
          'stake': res['stake'], 'delay_s': res['delay_s'], 'grid': res['grid']}
    if d['test']:
        ev['db']['test'] = {k: _brief(d['test'][k]) for k in ('conservative', 'visible', 'as_recorded')}
    ev['db']['best'] = {k: {'thr': b['thr'], 'train': _brief(b['train']), 'test': _brief(b['test'])}
                        for k, b in d['best'].items()}
    if res['nba'] is not None:
        n = res['nba']
        ev['nba'] = {'source': n['source'], 'align': n['align'], 'lag_s': n['lag_s'], 'fee_mult': n['fee_mult'],
                     'counts': n['counts'], 'stats': _brief(n['rules'][0]['stats']), 'why': n.get('why')}
    return {'lane': 'X1', 'stage': v['stage'], 'verdict': 'PASS' if v['pass'] else 'FAIL',
            'why': None if v['pass'] else v['why'], 'computed': computed, 'tool': 'tools/x1_backtest.py',
            'params': {'direction': v['rule']['direction'], 'thr': v['rule']['thr']} if v['rule'] else {},
            'evidence': ev}


def _brief(s):
    """Stats for the gate's evidence, rounded so the file reads the same on any machine."""
    def r(x):
        return None if x is None else round(x, 6)
    out = {'trades': s['trades'], 'unfilled': s['unfilled'], 'games': s['games']}
    for k in ('mo60', 'mo300', 'settle'):
        out[k] = {'n': s[k]['n'], 'mean': r(s[k]['mean']), 't': r(s[k]['t']), 'lower80': r(s[k]['lower80'])}
    return out


def gate_refusal(obj, lane, model_sha256='', params=()):
    """strat/lane_gate.cpp parse_lane_gate's rules, in its order: None when deskd loads the file,
    else its reason. A loaded file opens the lane only at stage live (gate_why)."""
    if not isinstance(obj, dict):
        return 'not a JSON object'

    def text(j, k):
        v = j.get(k)
        if v is None:
            return ''
        if not isinstance(v, str):
            raise TypeError('%s is not a string' % k)
        return v

    def sha(j, k):
        s = text(j, k).lower()
        return s if not s or (len(s) == 64 and all(c in '0123456789abcdef' for c in s)) else None
    try:
        name = text(obj, 'lane')
        if name != lane:
            return "is lane %s's gate, not %s's" % (name, lane) if name else 'names no lane'
        verdict, why = text(obj, 'verdict'), text(obj, 'why')
        if verdict == 'FAIL':
            return 'verdict FAIL' + (': ' + why if why else '')
        if verdict != 'PASS':
            return 'verdict "%s" is neither PASS nor FAIL' % verdict
        stage = text(obj, 'stage')
        if stage not in ('shadow', 'live'):
            return 'unknown stage "%s" (shadow or live)' % stage
        if not isinstance(obj.get('params'), dict):
            return 'no params object'
        for p in params:
            if obj['params'].get(p) is None:
                return 'params has no ' + p
        s, loaded = sha(obj, 'model_sha256'), model_sha256.lower()
        if s is None:
            return 'model_sha256 is not 64 hex digits'
        if s != loaded:
            if not loaded:
                return 'measured model %s, and no model is loaded' % s[:12]
            if not s:
                return 'names no model, and model %s is loaded' % loaded[:12]
            return 'measured another model (%s, loaded %s)' % (s[:12], loaded[:12])
        series = obj.get('series')
        if series is not None:
            if not isinstance(series, dict):
                return 'series is not an object'
            for k, v in series.items():
                if not isinstance(v, dict) or not isinstance(v.get('pass'), bool):
                    return 'series %s has no pass (true or false)' % k
                text(v, 'why')
                text(v, 'level')
                if v.get('leader_only') is not None and not isinstance(v['leader_only'], bool):
                    raise TypeError('leader_only is not a boolean')
                if sha(v, 'model_sha256') is None:
                    return 'series %s: model_sha256 is not 64 hex digits' % k
        text(obj, 'computed')
        text(obj, 'tool')
    except TypeError as e:
        return 'malformed: %s' % e
    return None


def gate_why(obj, lane, model_sha256='', params=()):
    """lane_gate_why: '' when the gate opens the lane (a live PASS), else why not."""
    why = gate_refusal(obj, lane, model_sha256, params)
    if why is not None:
        return why
    return '' if obj['stage'] == 'live' else 'stage shadow (shadow signals only until its gate is live)'


def write_gate(path, obj, force=False):
    if os.path.exists(path) and not force:
        raise FileExistsError('%s exists; --force replaces it' % path)
    d = os.path.dirname(os.path.abspath(path))
    os.makedirs(d, exist_ok=True)
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as fh:
        fh.write(json.dumps(obj, indent=1) + '\n')
    os.replace(tmp, path)


def _f(x, money=True):
    if x is None:
        return '-'
    return _usd(x) if money else '%.2f' % x


def _line(label, s):
    return '%-34s %5d %5d  %10s %6s  %10s  %10s %6s  %4d' % (
        label, s['mo60']['n'], s['games'], _f(s['mo60']['mean']), _f(s['mo60']['t'], False), _f(s['mo300']['mean']),
        _f(s['settle']['mean']), _f(s['settle']['t'], False), s['unfilled'])


HEAD = '%-34s %5s %5s  %10s %6s  %10s  %10s %6s  %4s' % ('', 'n', 'games', 'mo60/trade', 't', 'mo300', 'settle', 't',
                                                          'miss')


def report(res):
    lines = []
    w = lines.append
    d, n = res['db'], res['nba']
    w('x1_backtest: X1 as deskd trades it (take_decide ESPN_FOLLOW / ESPN_FADE), $%.2f clips, ESPN shifted %+.1f s'
      % (res['stake'], res['delay_s']))
    if d is not None:
        w('games by league (%s): ' % d['source'] + ', '.join('%s %d' % (lg, c['games']) for lg, c in d['leagues'].items()))
        w('  ESPN rows with a number: ' + ', '.join('%s %d' % (lg, c['with_number']) for lg, c in d['leagues'].items())
          + '; settled tickers: ' + ', '.join('%s %d' % (lg, c['settled']) for lg, c in d['leagues'].items()))
    if n is not None:
        c = n['counts']
        w('games (%s, %s): %d listed, %d matched to ESPN, %d with ESPN numbers, %d recorded (numbers and both books)'
          % (n['source'], n['league'], c.get('listed', 0), c.get('matched', 0), c.get('with_numbers', 0),
             c.get('recorded', 0)))
    if d is not None:
        if d['league']:
            w('only league %s: %d games' % (d['league'], d['games']))
        w('mapping repair: %d game(s) had home and away swapped%s' % (
            len(d['swapped']), (' (' + ', '.join('%s %.2f' % kv for kv in d['swapped'].items()) + '): corrected')
            if d['swapped'] else ''))
        sp = d['split']
        w('split by date: train %s (%d games), test %s (%d games)' % (
            '..'.join(sp['train']) if sp['train'] else '-', sp['train_games'],
            '..'.join(sp['test']) if sp['test'] else '-', sp['test_games']))
        w('')
        w('train, each rule (entry: the worse of the touch and the next quote; dollars a trade, net of the fee)')
        w(HEAD)
        for g in d['grid']:
            w(_line('%-6s %.2f' % (g['direction'], g['thr']), g['train']))
        w('')
        for k, b in d['best'].items():
            w(_line('best %s (%.2f) on test' % (k, b['thr']), b['test']))
        if d['chosen']:
            c = d['chosen']
            t = d['test']
            w('chosen on train: %s %.2f' % (c['direction'], c['thr']))
            w(HEAD)
            w(_line('test, conservative entry', t['conservative']))
            w(_line('test, the touch it saw', t['visible']))
            w(_line('test, as recorded (no repair)', t['as_recorded']))
            w('  all dates by league (mo60/trade): ' + ', '.join(
                '%s %d %s' % (lg, s['n'], _f(s['mean'])) for lg, s in t['by_league_all_dates'].items()))
    if n is not None:
        w('')
        w('recorded games (%s), ESPN visible %s:' % (n['league'], 'at first sight' if n['align'] == 'first_seen'
                                                    else 'at its play wallclock + %.1f s' % n['lag_s']))
        w(HEAD)
        for r in n['rules']:
            w(_line('%-6s %.2f' % (r['direction'], r['thr']), r['stats']))
        if 'passed' in n:
            w('replication: %s' % ('not negative' if n['passed'] else 'NOT YET: ' + n['why']))
    if d is not None:
        v = res['verdict']
        w('')
        w('verdict: %s' % (('PASS, stage %s' % v['stage'] + ('' if v['stage'] == 'live' else
                            ' (%s)' % (v['nba_why'] or 'no NBA replication: --data'))) if v['pass']
                           else 'FAIL: ' + v['why']))
    return '\n'.join(lines)


def _public(res):
    """The result without the per-trade lists."""
    out = dict(res)
    if out.get('db'):
        out['db'] = {k: v for k, v in out['db'].items() if k != 'trades'}
    return out


def run(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--db', help="the old desk's extract (espn_leadlag.sqlite)")
    ap.add_argument('--data', help="the recorder's data directory (the NBA replication)")
    ap.add_argument('--league', help="--db: only this ESPN league's games (default all); --data alone: the "
                                     "recorder's league (default nba); with both, the replication is nba's")
    ap.add_argument('--align', choices=('first_seen', 'wallclock'), default='first_seen')
    ap.add_argument('--lag-s', type=float, default=LAG_S)
    ap.add_argument('--delay-s', type=float, default=0.0)
    ap.add_argument('--stake', type=float, default=STAKE)
    ap.add_argument('--grid', help='thresholds in dollars, comma-separated (default %s)' % ','.join('%g' % x for x in GRID))
    ap.add_argument('--fee-mult', type=float, default=FEE_MULT, help='--data: the markets\' fee multiplier')
    ap.add_argument('--direction', choices=DIRECTIONS, help='--data alone: the frozen rule\'s direction')
    ap.add_argument('--thr', type=float, help='--data alone: the frozen rule\'s threshold (dollars)')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--write-gate', metavar='FILE', help='write the X1 gate file (gates_dir/X1.json)')
    ap.add_argument('--force', action='store_true', help='--write-gate may replace an existing file')
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--write-fixture', action='store_true', help=argparse.SUPPRESS)
    a = ap.parse_args(argv)
    if a.self_test or a.write_fixture:
        return self_test(write_fixture=a.write_fixture)

    def fail(msg):
        print('error: ' + msg, file=sys.stderr)
        return 2
    if not a.db and not a.data:
        return fail('--db FILE (the extract) or --data DIR (the recorder\'s) is required; there is no default')
    if a.db and not os.path.isfile(a.db):
        return fail('no database at %s' % a.db)
    if a.data and not os.path.isdir(a.data):
        return fail('no directory at %s' % a.data)
    if a.db and a.align == 'wallclock':
        return fail('--align wallclock needs play wallclocks, which the DB has not: first_seen is its only alignment')
    if (a.direction is None) != (a.thr is None):
        return fail('--direction and --thr go together')
    if a.direction and a.db:
        return fail('--direction/--thr freeze a rule for --data alone; with --db the train dates choose it')
    if a.write_gate and not a.db:
        return fail('--write-gate needs --db: the rule is chosen on its train dates')
    if a.write_gate and (a.delay_s != 0 or a.stake != STAKE):
        return fail("--write-gate measures X1 as deskd trades it: --delay-s 0 (ESPN when first seen; a negative delay "
                    "looks ahead) and --stake %g (take_params' clip), which the gate's params cannot change" % STAKE)
    if a.write_gate and os.path.exists(a.write_gate) and not a.force:
        return fail('%s exists; --force replaces it' % a.write_gate)
    if a.stake <= 0 or a.lag_s < 0:
        return fail('--stake must be above 0 and --lag-s not below 0')
    try:
        grid = tuple(float(x) for x in a.grid.split(',')) if a.grid else GRID
    except ValueError:
        return fail('--grid takes dollars, comma-separated')
    if not grid or any(not 0 < x < 1 for x in grid):
        return fail('--grid thresholds must be in (0, 1)')
    rule = (a.direction, a.thr) if a.direction else None
    try:
        res = backtest(a.db, a.data, a.league, a.align, a.lag_s, a.delay_s, a.stake, grid, a.fee_mult, rule)
    except (sqlite3.Error, ValueError) as e:
        return fail(str(e))
    if a.write_gate:
        write_gate(a.write_gate, gate_json(res, datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')), a.force)
    print(json.dumps(_public(res), indent=1, default=str) if a.json else report(res))
    if a.write_gate:
        print('wrote %s' % a.write_gate, file=sys.stderr)
    return 0


# ---- self-test ------------------------------------------------------------------------------
FIXTURE = os.path.join(ROOT, 'tests', 'deskd', 'fixtures', 'x1_gate.json')
FIXTURE_COMPUTED = '2026-10-22T00:00:00Z'

# The old desk's tables as src/store.cpp made them (home_ticker and away_ticker by its migration):
# a synthetic prod.sqlite for research/espn_leadlag.py's own extract()
PROD_DDL = (
    'CREATE TABLE series(ticker TEXT PRIMARY KEY, category TEXT, title TEXT, fee_type TEXT, fee_multiplier REAL, '
    'frequency TEXT, updated_ts INTEGER)',
    'CREATE TABLE markets(ticker TEXT PRIMARY KEY, event_ticker TEXT, series_ticker TEXT, title TEXT, subtitle TEXT, '
    'yes_sub_title TEXT, no_sub_title TEXT, status TEXT, market_type TEXT, strike_type TEXT, floor_strike REAL, '
    'cap_strike REAL, open_time TEXT, close_time TEXT, expected_expiration_time TEXT, expiration_time TEXT, '
    'result TEXT, rules_primary TEXT, tick REAL, yes_bid REAL, yes_ask REAL, no_bid REAL, no_ask REAL, last REAL, '
    'volume REAL, open_interest REAL, custom_strike TEXT, updated_ts INTEGER)',
    'CREATE TABLE ext_wp(id INTEGER PRIMARY KEY, ts INTEGER, event_ticker TEXT, espn_id TEXT, league TEXT, state TEXT, '
    'period INTEGER, clock TEXT, home_score INTEGER, away_score INTEGER, wp_home REAL, wp_leadtime REAL, mu_home REAL, '
    'mkt_home REAL, mkt_away REAL, home_ticker TEXT, away_ticker TEXT)',
    'CREATE TABLE quotes(id INTEGER PRIMARY KEY, ts INTEGER, ticker TEXT, yes_bid REAL, yes_ask REAL, no_bid REAL, '
    'no_ask REAL, last REAL, yes_bid_size REAL, yes_ask_size REAL, volume REAL, open_interest REAL)')

# A game's Kalshi quotes by phase, the eventual winner's (bid, ask); the loser's mirror them.
# Event 1 moves the truth to 0.80 and Kalshi 80% of the way at once (0.74), the rest a little later
# (DB 80 s, recorder 40 s: past the next quote either way); event 2 to 0.97 (0.93, then 0.97).
# ESPN's number moves 20% of the way and stays: FADE pays, FOLLOW loses.
PHASES = ((4900, 5100), (7300, 7500), (7900, 8100), (9200, 9400), (9600, 9800))
ESPN = (0.50, 0.56, 0.83)


def synth_db(path, games_per_date=3, dates=36, swapped=(4, 31, 77), losers=(10, 52, 100), seed=11):
    """The synthetic extract: EXTRACT_DDL's tables, the desk's value types. Each game: samples every
    30 s for 20 minutes, both markets quoted every 20 s (offset 7 s), event 1 between samples 8 and
    9, event 2 between 24 and 25, Kalshi's second step 80 s after each (after event 1 to 0.80 in
    even games, 0.81 in odd ones). Leagues by game:
    college-football, nfl, mlb (no clock). The games numbered in `swapped` were recorded with home
    and away exchanged; those in `losers` were won by the other team. One trap quote 1 s after a
    sample offers the winner at 0.60 until the next quote: no entry may use it. Returns {event:
    winner's ticker}, in the order the games were made."""
    rng = random.Random(seed)
    db = sqlite3.connect(path)
    for s in EXTRACT_DDL:
        db.execute(s)
    db.executemany('INSERT INTO series VALUES(?,?,?)', [('KXNCAAFGAME', 'quadratic', 1.0), ('KXNFLGAME', 'quadratic', 1.0),
                                                       ('KXMLBGAME', 'quadratic', 1.0)])
    winners, n = {}, 0
    t_day0 = int(datetime(2026, 8, 29, 23, 0, tzinfo=timezone.utc).timestamp())
    for day in range(dates):
        for k in range(games_per_date):
            league, series, period, clock0 = (('college-football', 'KXNCAAFGAME', 2, 840), ('nfl', 'KXNFLGAME', 1, 900),
                                              ('mlb', 'KXMLBGAME', 5, None))[k % 3]
            t0 = t_day0 + day * 86400 + k * 1800
            date = datetime.fromtimestamp(t0 - 4 * 3600, tz=timezone.utc)   # the ET date
            ev = '%s-%s%s%02d%s' % (series, date.strftime('%y'), date.strftime('%b').upper(), date.day, 'AAABBB%d' % k)
            ta, tb_ = ev + '-AAA', ev + '-BBB'
            home, away = (ta, tb_) if rng.random() < 0.5 else (tb_, ta)
            win = ta if rng.random() < 0.5 else tb_
            lose = tb_ if win == ta else ta
            settled_win = lose if n in losers else win
            winners[ev] = win
            e1, e2 = t0 + 8 * 30 + 5, t0 + 24 * 30 + 5
            # the quotes: phase by time
            for j in range(62):
                tq = t0 - 20 + 20 * j + 7
                ph = 0 if tq < e1 else 1 if tq < e1 + 80 else 2 if tq < e2 else 3 if tq < e2 + 80 else 4
                b, a = (8000, 8200) if ph == 2 and n % 2 else PHASES[ph]
                db.execute('INSERT INTO quotes VALUES(?,?,?,?)', (tq, win, b / ONE, a / ONE))
                db.execute('INSERT INTO quotes VALUES(?,?,?,?)', (tq, lose, (ONE - a) / ONE, (ONE - b) / ONE))
            if n == 5:   # the trap
                db.execute('INSERT INTO quotes VALUES(?,?,?,?)', (t0 + 9 * 30 + 1, win, 0.58, 0.60))
            for tk in (ta, tb_):
                db.execute('INSERT INTO markets VALUES(?,?,?,?,?)', (tk, series, 'yes' if tk == settled_win else 'no',
                                                                    'finalized', None))
            rec_home, rec_away = (away, home) if n in swapped else (home, away)
            for i in range(40):
                ts = t0 + 30 * i
                stage = 0 if ts < e1 else 1 if ts < e2 else 2
                p_win = ESPN[stage]
                wp = p_win if win == home else 1 - p_win
                pts = (0, 7, 14)[stage] if league != 'mlb' else (0, 1, 3)[stage]
                hs, as_ = (pts, 0) if win == home else (0, pts)
                clk = None if clock0 is None else '%d:%02d' % divmod(max(0, clock0 - 15 * i), 60)
                db.execute('INSERT INTO ext_wp VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)',
                           (ts, ev, league, 'in', period, clk, hs, as_, round(wp, 4), None, None, rec_home, rec_away))
            n += 1
    db.commit()
    db.close()
    return winners


def extract_schema_check(check):
    """EXTRACT_DDL is what research/espn_leadlag.py's extract() writes, run on a prod.sqlite with
    src/store.cpp's tables (while that script is in the tree)."""
    src = os.path.join(ROOT, 'research', 'espn_leadlag.py')
    if not os.path.exists(src):
        print('     research/espn_leadlag.py: not present, the extract schema check skipped')
        return
    spec = importlib.util.spec_from_file_location('espn_leadlag', src)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    with tempfile.TemporaryDirectory() as d:
        live, out = os.path.join(d, 'prod.sqlite'), os.path.join(d, 'research', 'espn_leadlag.sqlite')
        db = sqlite3.connect(live)
        for s in PROD_DDL:
            db.execute(s)
        db.execute("INSERT INTO ext_wp(ts, event_ticker, league, state, wp_home, home_ticker, away_ticker) "
                   "VALUES(1, 'E', 'nfl', 'in', 0.5, 'E-H', 'E-A')")
        db.execute("INSERT INTO quotes(ts, ticker, yes_bid, yes_ask) VALUES(1, 'E-H', 0.49, 0.51)")
        db.execute("INSERT INTO markets(ticker, series_ticker, result) VALUES('E-H', 'S', 'yes')")
        db.execute("INSERT INTO series(ticker, fee_multiplier) VALUES('S', 1.0)")
        db.commit()
        db.close()
        with contextlib.redirect_stdout(io.StringIO()):
            mod.extract(live, out)
        got = sqlite3.connect(out)
        sql = [r[0] for r in got.execute("SELECT sql FROM sqlite_master ORDER BY rowid")]
        got.close()
    check("EXTRACT_DDL is research/espn_leadlag.py extract()'s schema, read from the file it writes",
          sql == list(EXTRACT_DDL), sql)


def quiet(argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = run(argv)
    return rc, out.getvalue() + err.getvalue()


# --- the recorder's synthetic NBA games
TEAMS = (('BOS', 'Boston', 'Boston Celtics'), ('LAL', 'Los Angeles L', 'Los Angeles Lakers'),
         ('MIA', 'Miami', 'Miami Heat'), ('DEN', 'Denver', 'Denver Nuggets'), ('PHX', 'Phoenix', 'Phoenix Suns'),
         ('DAL', 'Dallas', 'Dallas Mavericks'), ('CHI', 'Chicago', 'Chicago Bulls'), ('ATL', 'Atlanta', 'Atlanta Hawks'))


def synth_data(root, games=32, seed=5, outage_game=3, late_game=7):
    """A recorder directory (lip_join.Spool, the recorder's tables): KXNBAGAME games, the same
    phases as synth_db, each game's books by socket (a snapshot at each phase), its scoreboard rows
    and its summary plays, ESPN's play first seen 40 s after its wallclock. outage_game's scoreboard
    is down across its second event; late_game has a late-inserted play claiming 0.01. Returns
    {event: (winner's ticker, home's ticker)}."""
    rng = random.Random(seed)
    sp = lj.Spool(root)
    us = 1_000_000
    out, seq = {}, 0
    day0 = int(datetime(2026, 11, 1, 23, 30, tzinfo=timezone.utc).timestamp())
    for n in range(games):
        h, aw = TEAMS[n % 8], TEAMS[(n + 3) % 8]
        t0 = day0 + (n // 2) * 86400 + (n % 2) * 3600
        date = datetime.fromtimestamp(t0 - 5 * 3600, tz=timezone.utc)
        ev = 'KXNBAGAME-%s%s%02d%s%s' % (date.strftime('%y'), date.strftime('%b').upper(), date.day, aw[0], h[0])
        tk = {h[0]: ev + '-' + h[0], aw[0]: ev + '-' + aw[0]}
        win_home = rng.random() < 0.5
        win, lose = (tk[h[0]], tk[aw[0]]) if win_home else (tk[aw[0]], tk[h[0]])
        out[ev] = (win, tk[h[0]])
        start = datetime.fromtimestamp(t0, tz=timezone.utc).strftime('%Y-%m-%dT%H:%MZ')
        for code, name in ((h[0], h[1]), (aw[0], aw[1])):
            raw = {'ticker': tk[code], 'event_ticker': ev, 'yes_sub_title': name, 'result': 'yes' if tk[code] == win else 'no'}
            sp.write('kalshi_markets', {'recv_us': (t0 + 3000) * us, 'series': 'KXNBAGAME', 'ticker': tk[code],
                                        'status': 'finalized', 'open_time': start, 'close_time': start,
                                        'result': raw['result'], 'raw': json.dumps(raw)})
        e1, e2 = t0 + 245, t0 + 725
        for ph, at in enumerate((t0 - 60, e1, e1 + 40, e2, e2 + 40)):
            b, a = PHASES[ph]
            for mk, (bid, ask) in ((win, (b, a)), (lose, (ONE - a, ONE - b))):
                seq += 1
                sp.snapshot(at * us + 1000 * (mk == lose), 1, 1, seq, mk, [(bid / ONE, 50)], [(1 - ask / ONE, 50)])

        def score_row(t, state, period, clock, wpts, raw_extra=None):
            hs, as_ = (wpts, 0) if win_home else (0, wpts)
            raw = {'status': {}, 'competitors': [
                {'homeAway': 'home', 'score': str(hs), 'team': {'id': '1', 'abbreviation': h[0], 'displayName': h[2]}},
                {'homeAway': 'away', 'score': str(as_), 'team': {'id': '2', 'abbreviation': aw[0], 'displayName': aw[2]}}]}
            raw.update(raw_extra or {})
            sp.write('espn_scores', {'recv_us': t * us, 'prev_us': (t - 4) * us, 'league': 'basketball/nba',
                                     'game_id': str(401900000 + n), 'start_time': start, 'state': state,
                                     'status': 'STATUS_IN_PROGRESS', 'period': str(period), 'clock': clock,
                                     'home': h[0], 'away': aw[0], 'home_score': hs, 'away_score': as_,
                                     'detail': '', 'raw': json.dumps(raw)})
        score_row(t0, 'in', 1, '12:00', 0)
        score_row(e1 + 3, 'in', 1, '7:55', 3)
        score_row(e2 + 3, 'in', 2, '7:55', 6)
        score_row(e2 + 600, 'post', 4, '0:00', 6)
        plays = [(1, e1 - 37, e1 + 3, ESPN[1]), (2, e2 - 37, e2 + 3, ESPN[2])]
        if n == late_game:   # sequence 1.5 shown after play 2: a correction inserted under it
            plays.append((1.5, e2 - 30, e2 + 9, 0.01))
        for s, wall, seen, p_win in plays:
            wp = p_win if win_home else 1 - p_win
            sp.write('espn_plays', {'recv_us': seen * us, 'prev_us': (seen - 10) * us, 'league': 'basketball/nba',
                                    'game_id': str(401900000 + n), 'play_id': 'p%s' % s, 'seq': int(s * 10),
                                    'period': 1, 'clock': '7:55',
                                    'wallclock': datetime.fromtimestamp(wall, tz=timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
                                    'home_score': None, 'away_score': None, 'team_id': None, 'scoring': True,
                                    'score_value': 3, 'type': 'Jump Shot', 'text': 'x', 'wp_home': round(wp, 4),
                                    'wp_tie': 0.0, 'revised': False, 'raw': '{}'})
        if n == outage_game:
            sp.write('gaps', {'recv_us': (e2 + 40) * us, 'source': 'espn', 'kind': 'recovered', 'detail': 'test',
                              'from_us': (e2 - 5) * us, 'to_us': (e2 + 40) * us})
    return out


def self_test(write_fixture=False):
    fails = []

    def check(name, ok, detail=''):
        print('%s %s%s' % ('OK  ' if ok else 'FAIL', name, (': ' + str(detail)) if detail else ''))
        if not ok:
            fails.append(name)

    # ---- the rule is X1's: x1_golden.json, and the grid's params are take_params' with thr and stake
    cases, bad, bad_params = tr.run_cases(os.path.join(ROOT, 'tests', 'deskd', 'fixtures', 'x1_golden.json'))
    check('x1_golden.json: every case, through take_rule', cases >= 25 and not bad and not bad_params,
          '%d cases, %d differ' % (cases, len(bad)))
    with open(os.path.join(ROOT, 'tests', 'deskd', 'fixtures', 'x1_golden.json'), encoding='utf-8') as fh:
        gold = json.load(fh)['params']
    check("rule_params('fade', 0.10, $2) is x1_golden's ESPN_FADE params, follow its ESPN_FOLLOW",
          rule_params('fade', 0.10) == tr.params_of(gold['ESPN_FADE'], 'ESPN_FADE')
          and rule_params('follow', 0.10) == tr.params_of(gold['ESPN_FOLLOW'], 'ESPN_FOLLOW'))
    check('a league without a clock runs without the 2:00 rule', rule_params('follow', 0.1, 2, 'mlb')['min_t_left_s'] == 0
          and rule_params('follow', 0.1, 2, 'nfl')['min_t_left_s'] == 120)

    # ---- the pieces
    check('clock_state: NBA Q3 4:12 is 972 s, OT1 3:00 is 180 s in overtime, football Q2 14:00 is 2640 s, '
          'MLB has no clock and its 10th is overtime',
          clock_state('nba', 3, '4:12') == (972.0, False) and clock_state('nba', 5, '3:00') == (180.0, True)
          and clock_state('college-football', 2, '14:00') == (2640.0, False)
          and clock_state('mlb', 9, '') == (None, False) and clock_state('mlb', 10, None) == (None, True)
          and clock_state('nba', 2, '13:00') == (None, False) and clock_state('curling', 1, '1:00') == (None, False))
    check('first_seen: the first row of each run of equal keys',
          first_seen(['a', 'a', 'b', 'b', 'a'], [1, 2, 3, 4, 5]) == [1, 1, 3, 3, 5])
    check('number_seen: the first row of its run in its score state; a row without one (0) does not end the run, '
          'another number or another state does',
          number_seen([5600, None, 5600, 5800, 5600, 5600], ['s'] * 5 + ['t'], [1, 2, 3, 4, 5, 6]) == [1, 0, 1, 4, 5, 6])
    s = summarize([('g1', 1.0), ('g1', 3.0), ('g2', -1.0)])
    check('summarize: mean 1, se clustered by game sqrt(2/1 x (2^2 + 2^2)) / 3 = 4/3, t 0.75',
          s['n'] == 3 and s['games'] == 2 and abs(s['mean'] - 1) < 1e-12 and abs(s['se'] - 4 / 3) < 1e-12
          and abs(s['t'] - 0.75) < 1e-12, s)
    check('summarize: one game has no standard error', summarize([('g', 1.0), ('g', 2.0)])['se'] is None
          and summarize([])['mean'] is None)
    check('grid_of: each start to +30 s every second, overlaps merged',
          grid_of([0, 10_000, 100_000], 30_000, 1000) == list(range(0, 40_001, 1000)) + list(range(100_000, 130_001, 1000)))
    bk = Book()
    bk.add('X', 1000, Quote(1000, 4000, None, 4200, None))
    bk.add('X', 2000, None)
    bk.add('X', 3000, Quote(3000, 4100, None, 4300, None))
    check('Book: the last entry at or before t; a drop is no book; nothing after t; max age',
          bk.at('X', 999) is None and bk.at('X', 1999).ask == 4200 and bk.at('X', 2500) is None
          and bk.at('X', 3000).ask == 4300 and bk.at('X', 40000, 30000) is None and bk.mid('X', 1500) == 0.41
          and bk.next_after('X', 1000) == (2000, None) and bk.next_after('X', 2000)[1].t == 3000
          and bk.next_after('X', 3000) is None)
    games = [Game('E%d' % i, 'nfl', d, ('A', 'B'), True, {}, {}) for i, d in
             enumerate(['2026-09-01'] * 3 + ['2026-09-02'] * 3 + ['2026-09-03'] * 3)]
    check('split_by_date: two thirds of the games train, whole dates',
          split_by_date(games) == ({'2026-09-01', '2026-09-02'}, {'2026-09-03'}))
    extract_schema_check(check)

    # the signal run, TakeEngine's: FADE 0.12 takes A at 1 s (a gap of 0.18); the cool-down hides
    # the game until 11 s, when A is FIRED (an edge); the gap is gone from 11.5 s and back at
    # back_ms. A run ends at the first decision without edge 10 s after the last with one: back at
    # 14.5 s it is the same run, no second take; back at 25.5 s the run ended at 21 s, A again at 26 s
    def quiet_game(back_ms):
        a, b = 'KXNBAGAME-26NOV01AAABBB-AAA', 'KXNBAGAME-26NOV01AAABBB-BBB'
        g = Game('KXNBAGAME-26NOV01AAABBB', 'nba', '2026-11-01', (a, b), True, {}, {a: 1.0, b: 1.0})
        bk = Book()
        for t, (bid, ask) in ((0, (7300, 7500)), (11500, (5500, 5700)), (back_ms, (7300, 7500))):
            bk.add(a, t, Quote(t, bid, 5000, ask, 5000))
            bk.add(b, t, Quote(t, ONE - ask, 5000, ONE - bid, 5000))
        g.points = [(t, 'in', 2000.0, False, 10, 4, 5600, 1000, 1000, t) for t in range(1000, 30001, 1000)]
        return [x['t'] for x in simulate(g, bk, rule_params('fade', 0.12))]
    check('a signal run ends at a decision without edge 10 s after the last with one (run_quiet_ms)',
          quiet_game(14500) == [1000] and quiet_game(25500) == [1000, 26000], (quiet_game(14500), quiet_game(25500)))

    # the DB's -1 between two equal numbers (the old desk's summary missed): FADE 0.12 with ESPN at
    # 0.56 for A from 0 s, A at 0.55/0.57 and from 50 s 0.73/0.75 (a gap of 0.18). At 60 s the 0.56
    # is the one first seen at 0 s, 60 s old: SIGNAL_OLD, no take. With 0.58 at 30 s instead, the
    # 0.56 at 60 s is a new sighting and A is taken
    def miss_game(between):
        ev, t0 = 'KXNBAGAME-26NOV01AAABBB', 1_793_000_000
        a, b = ev + '-AAA', ev + '-BBB'
        rows = [{'ts': t0 + 30 * k, 'league': 'nba', 'state': 'in', 'period': 2, 'clock': '6:00', 'home_score': 40,
                 'away_score': 38, 'wp': wp, 'home_ticker': a, 'away_ticker': b}
                for k, wp in enumerate((0.56, between, 0.56))]
        g = db_games({ev: rows}, {}, {a: 1.0, b: 1.0}, 0)[ev]
        bk = Book()
        for t, (bid, ask) in ((t0 * 1000, (5500, 5700)), (t0 * 1000 + 50_000, (7300, 7500))):
            bk.add(a, t, Quote(t, bid, None, ask, None))
            bk.add(b, t, Quote(t, ONE - ask, None, ONE - bid, None))
        return [x['t'] - t0 * 1000 for x in simulate(g, bk, rule_params('fade', 0.12))]
    check("the DB: a -1 row does not make ESPN's next number new (deskd keeps a play's first sighting through a "
          "miss): 0.56, -1, 0.56 takes nothing at 60 s; 0.56, 0.58, 0.56 takes A at 60 s",
          miss_game(-1) == [] and miss_game(0.58) == [60_000], (miss_game(-1), miss_game(0.58)))

    # gw's inventory, X1's 4 contracts a market, over the position the replay's own takes built
    # (x1_golden.json's 60c cases): FOLLOW 0.10 with ESPN at 0.75 for A's team, A 0.59/0.60 and B
    # 0.39/0.41, a new score state every 11 s. $2 buys 3 of A at 0.60 (edge 0.13); the next run
    # buys the 1 left of A's 4; with A full the third sells B at 0.39 (A at 0.61: 3, edge 0.1233)
    # and the fourth the 1 left of B's; without the cap all four would buy 3 of A
    a, b = 'KXNBAGAME-26NOV01AAABBB-AAA', 'KXNBAGAME-26NOV01AAABBB-BBB'
    g = Game('KXNBAGAME-26NOV01AAABBB', 'nba', '2026-11-01', (a, b), True, {}, {a: 1.0, b: 1.0})
    bk = Book()
    bk.add(a, 0, Quote(0, 5900, 5000, 6000, 5000))
    bk.add(b, 0, Quote(0, 3900, 5000, 4100, 5000))
    g.points = [(t, 'in', 2000.0, False, 70 + k, 64, 7500, t, t, t) for k, t in enumerate(range(1000, 34001, 11000))]
    took = [(x['side'], x['ticker'], x['price'], x['count'], x['edge'])
            for x in simulate(g, bk, rule_params('follow', 0.10))]
    check("inventory: X1's takes fit its 4 a market, A's then B's, the rest through the other market",
          took == [('BUY', a, 6000, 300, 1300), ('BUY', a, 6000, 100, 1300), ('SELL', b, 3900, 300, 1233),
                   ('SELL', b, 3900, 100, 1200)], took)

    with tempfile.TemporaryDirectory() as d:
        db = os.path.join(d, 'espn_leadlag.sqlite')
        winners = synth_db(db)
        events = list(winners)
        swapped_ev = {events[i] for i in (4, 31, 77)}
        res = backtest(db=db)
        r = res['db']
        # per league first, the NBA listed at 0
        rc, text = quiet(['--db', db])
        first = text.splitlines()[1] if rc == 0 and len(text.splitlines()) > 1 else ''
        check('the report lists games by league first, the NBA at 0',
              rc == 0 and first.startswith('games by league') and 'nba 0' in first and 'college-football 36' in first
              and 'mlb 36' in first, first)
        check('mapping repair: the three planted games, and only they, each strongly negative',
              set(r['swapped']) == swapped_ev and all(v < -0.8 for v in r['swapped'].values()), r['swapped'])
        sp = r['split']
        check('split: 24 dates (72 games) train, 12 (36) test', (sp['train_games'], sp['test_games']) == (72, 36)
              and sp['train'] == ['2026-08-29', '2026-09-21'] and sp['test'] == ['2026-09-22', '2026-10-03'], sp)
        c = r['chosen'] or {}
        check('the planted FADE is chosen on train (0.12: event 1 alone, a gap of 0.18; ties to the lower threshold)',
              c == {'direction': 'fade', 'thr': 0.12}, c)
        bf = r['best'].get('follow')
        check('FOLLOW loses on train and on test', bf is not None and bf['train']['mo60']['mean'] < 0
              and bf['test']['mo60']['mean'] < 0 and bf['test']['settle']['mean'] < 0, bf and bf['thr'])
        t = r['test'] or {}
        tc = t.get('conservative', {})
        check('test: 36 trades; 60 s markout 2 x (0.80 or 0.81 - 0.75) - 3c, a mean of $0.08; settlement '
              '(35 x 0.47 - 1.53) / 36 with one game lost',
              tc.get('trades') == 36 and abs(tc['mo60']['mean'] - 0.08) < 1e-9 and tc['settle']['n'] == 36
              and abs(tc['settle']['mean'] - (35 * 0.47 - 1.53) / 36) < 1e-9 and tc['settle']['t'] > 0,
              (tc.get('mo60'), tc.get('settle')))
        check('verdict: PASS at stage shadow (no NBA replication)', res['verdict']['pass']
              and res['verdict']['stage'] == 'shadow' and not res['verdict']['why'], res['verdict'])
        ar = t.get('as_recorded', {})
        check("as recorded, the swapped test game reads ESPN's number for the other team: its event 2 gap is "
              "0.76, one more take", ar.get('trades') == 37, ar.get('trades'))
        trades = [x for ts in r['trades'].values() for x in ts]
        check('no entry uses a quote after the time it was decided', trades and all(x['quote_ms'] <= x['t'] for x in trades))
        trap = [x for x in trades if x['game'] == events[5]]
        check('the trap quote (0.60, 1 s after the sample) is never the entry', trap and all(x['price'] != 6000 and
              (x['side'] == 'SELL' or x['price'] == 7500) for x in trap), [(x['side'], x['price']) for x in trap])
        one = [x for x in trades if x['game'] == events[0]]
        check('one trade worked by hand: the winner at 0.75, 2 contracts, fee 3c, edge 0.18 (0.74 - 0.56)',
              len(one) == 1 and one[0]['team_px'] == 7500 and one[0]['count'] == 200 and one[0]['fee'] == 300
              and one[0]['edge'] == 1800 and one[0]['ticker'] == winners[events[0]], one)
        # the CLI's refusals and the gate file
        gate = os.path.join(d, 'gates', 'X1.json')
        check('--align wallclock is refused for --db', quiet(['--db', db, '--align', 'wallclock'])[0] == 2)
        check('no source is refused, and so is a missing one', quiet([])[0] == 2
              and quiet(['--db', os.path.join(d, 'none.sqlite')])[0] == 2)
        check('--write-gate writes X1.json', quiet(['--db', db, '--write-gate', gate])[0] == 0 and os.path.exists(gate))
        other = os.path.join(d, 'other', 'X1.json')
        check("--write-gate refuses a --delay-s (-40 s looks ahead, +10 s) or a --stake other than take_params' $2 "
              "clip, and writes nothing",
              all(quiet(['--db', db, '--write-gate', other] + x)[0] == 2
                  for x in (['--delay-s', '-40'], ['--delay-s', '10'], ['--stake', '1']))
              and not os.path.exists(other))
        with open(gate, encoding='utf-8') as fh:
            g = json.load(fh)
        check("the gate loads as deskd's load_lane_gate(path, 'X1', '', {direction, thr}): stage shadow",
              gate_refusal(g, 'X1', '', GATE_PARAMS) is None and gate_why(g, 'X1', '', GATE_PARAMS).startswith('stage shadow')
              and g['params'] == {'direction': 'fade', 'thr': 0.12} and 'model_sha256' not in g, g['params'])
        check('an existing gate file is refused without --force, replaced with it',
              quiet(['--db', db, '--write-gate', gate])[0] == 2 and quiet(['--db', db, '--write-gate', gate, '--force'])[0] == 0)
        check("lane_gate.cpp's refusals: another lane's file, a FAIL, a missing param, a model it should not name",
              gate_refusal(g, 'N1') == "is lane X1's gate, not N1's"
              and gate_refusal(dict(g, verdict='FAIL', why='both negative'), 'X1') == 'verdict FAIL: both negative'
              and gate_refusal(dict(g, params={'thr': 0.1}), 'X1', '', GATE_PARAMS) == 'params has no direction'
              and gate_refusal(dict(g, model_sha256='a' * 64), 'X1') == 'measured model aaaaaaaaaaaa, and no model is loaded'
              and gate_refusal(dict(g, model_sha256='abc'), 'X1') == 'model_sha256 is not 64 hex digits'
              and gate_refusal(dict(g, lane=1), 'X1').startswith('malformed: ')
              and gate_refusal(dict(g, stage='paper'), 'X1') == 'unknown stage "paper" (shadow or live)'
              and gate_why(dict(g, stage='live'), 'X1', '', GATE_PARAMS) == '')
        # a FAIL: FOLLOW's grid alone has no rule that is not negative on test
        fres = backtest(db=db, grid=(0.12,))
        fres['db'] = dict(fres['db'], best={'follow': fres['db']['best']['follow']})
        fres['db']['test'] = None
        why = judge(fres['db']['best']['follow']['test'], MIN_TEST, 'the test dates')
        fres['verdict'] = dict(fres['verdict'], rule={'direction': 'follow', 'thr': 0.12}, why=why, stage='shadow',
                               **{'pass': False})
        fg = gate_json(fres, FIXTURE_COMPUTED)
        check("FOLLOW 0.12 on test is a FAIL with its why, and deskd refuses the file with it",
              why.startswith('the test dates: the mean net 60 s markout is -$') and fg['verdict'] == 'FAIL'
              and gate_refusal(fg, 'X1', '', GATE_PARAMS) == 'verdict FAIL: ' + why, why)
        # the committed fixture is what this tool writes for the synthetic extract
        res['db']['source'] = 'x1_synthetic.sqlite'
        fx = gate_json(res, FIXTURE_COMPUTED)
        fx['tool'] = 'tools/x1_backtest.py --self-test (SYNTHETIC, a planted FADE effect: not a measurement)'
        if write_fixture:
            write_gate(FIXTURE, fx, force=True)
            print('     wrote %s' % FIXTURE)
        try:
            with open(FIXTURE, encoding='utf-8') as fh:
                committed = json.load(fh)
        except (OSError, ValueError):
            committed = None
        check('tests/deskd/fixtures/x1_gate.json is what this tool writes for the synthetic extract, and loads as X1',
              committed == json.loads(json.dumps(fx)) and gate_refusal(committed, 'X1', '', GATE_PARAMS) is None)

        # ---- the recorder: the NBA replication
        data = os.path.join(d, 'rec')
        nba = synth_data(data)
        rn = backtest_data(data, rule=('fade', 0.12))
        cn = rn['counts']
        check('the recorder: 32 games listed, matched through espn_match, numbers and books for each',
              (cn.get('listed'), cn.get('matched'), cn.get('with_numbers'), cn.get('recorded')) == (32, 32, 32, 32), cn)
        st = rn['rules'][0]['stats']
        check('the frozen FADE 0.12 on the recorded games: 32 trades in 32 games, not negative: replication passes',
              st['trades'] == 32 and st['games'] == 32 and st['mo60']['mean'] > 0 and st['settle']['mean'] > 0
              and rn['passed'], (st['trades'], st['mo60']['mean'], rn.get('why')))
        both = backtest(db=db, data=data)
        check('--db and --data: the DB chooses, the recorder replicates, stage live', both['verdict']['stage'] == 'live'
              and both['nba']['rules'][0]['direction'] == 'fade', both['verdict'])
        rc, text = quiet(['--db', db, '--data', data, '--write-gate', gate, '--force'])
        with open(gate, encoding='utf-8') as fh:
            gl = json.load(fh)
        check("its gate file opens X1 (live PASS) and carries the NBA evidence, the replication's fee multiplier too",
              rc == 0 and gate_why(gl, 'X1', '', GATE_PARAMS) == '' and gl['evidence']['nba']['counts']['recorded'] == 32
              and gl['evidence']['nba'].get('fee_mult') == FEE_MULT)
        # the late-inserted play and the outage
        lg = list(nba)[7]
        gms, bk2, _ = read_data(data)
        check('a late-inserted play (claiming 0.01) is dropped at first sight', [p for _, p, _ in gms[lg].numbers]
              == [p_for_a(ESPN[1] if nba[lg][0] == nba[lg][1] else 1 - ESPN[1], 0, gms[lg].a_home),
                  p_for_a(ESPN[2] if nba[lg][0] == nba[lg][1] else 1 - ESPN[2], 0, gms[lg].a_home)],
              gms[lg].numbers)
        og = list(nba)[3]
        fade10 = run_rule(gms, bk2, ('fade', 0.10), STAKE)
        check('FADE 0.10 takes both events, except across the ESPN outage (SCORE_OLD): 63 trades',
              len(fade10[og]) == 1 and sum(len(v) for v in fade10.values()) == 63,
              (len(fade10[og]), sum(len(v) for v in fade10.values())))
        # alignment: FOLLOW at first sight loses; at the wallclock with no lag it looks ahead and wins
        fs = backtest_data(data, rule=('follow', 0.03))['rules'][0]['stats']
        wc = backtest_data(data, align='wallclock', lag_s=0.0, rule=('follow', 0.03))['rules'][0]['stats']
        wl = backtest_data(data, align='wallclock', lag_s=41.5, rule=('follow', 0.03))['rules'][0]['stats']
        check('FOLLOW: loses at first sight, wins at the wallclock with no lag (40 s of look-ahead), loses at +41.5 s',
              fs['mo60']['mean'] < 0 and wc['mo60']['mean'] > 0 and wl['mo60']['mean'] < 0,
              (fs['mo60']['mean'], wc['mo60']['mean'], wl['mo60']['mean']))
        check('--data alone with a frozen rule runs; --write-gate without --db is refused',
              quiet(['--data', data, '--direction', 'fade', '--thr', '0.12'])[0] == 0
              and quiet(['--data', data, '--write-gate', gate, '--force'])[0] == 2
              and quiet(['--data', data, '--direction', 'fade'])[0] == 2)
    print('x1_backtest self-test: %s' % ('ok' if not fails else 'FAILED (%d)' % len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(run(sys.argv[1:]))
