#!/usr/bin/env python3
"""C1's gate: do thin college games' stale quotes pay when taken exactly as the lane takes them?

Plan 5(d) records women's basketball and volleyball (and the thin men's decile as a control) and
then measures "what was knowable then: how often the best price sits 20c+ from a live fair, for
how long, at what size, and 60 s and 300 s markouts net of the fee", gated on "50+ cases with a
positive net markout". This tool reads the recorder's data and measures those cases with exactly
the rule C1 trades: tools/take_rule.py's decide(), deskd's take_decide in Python, asserted on the
same hand-worked cases (tests/deskd/fixtures/college_take_cases.json) as the C++ (plan 5(e): "the
S2 mistake was testing one order type and trading another"). It then writes deskd's gate file
for C1, gates_dir/C1.json (strat/lane_gate.h), which deskd reads at start.

The readers keep what deskd would have known and when.

  games(data, series)    event -> its markets: kalshi_markets (the REST rows, raw kept) and
                         kalshi_markets_daily, the latest row of each market: its event, name
                         (yes_sub_title), result (the latest one given), close time, fee
                         multiplier (when a row carries one) and fractional flag. A is the first
                         market by ticker, as TakeEngine and GameFair take it.
  score_rows(data)       (src, league, game_id) -> rows by the time first seen (recv_us), src ESPN
                         (espn_scores) or NCAA (ncaa_scores); and the outages: when a source was not
                         polling. A feed's failures are gaps rows (poll_failed while down, one
                         recovered row spanning the outage); poll_failed names the feed's league
                         first in its detail, and recovered shares its from_us. A recorder restart
                         stops every source. A league that cannot be read stops every league.
  match_games(...)       event -> the ESPN and NCAA games it is, through tools/espn_match.py's
                         match_game (deskd's own rules), with the side market A plays: candidates
                         are the league's games dated within a day of the ticker's date.
  book_history(data, tickers)
                         ticker -> Hist: the top of each market's book each time it changed, and
                         since when each touch price was first seen. From kalshi_ws through
                         lip_join's Replay (deskd's frames, units, sequence, connection and gap
                         rules; the recorder's disconnects and restarts drop every book); a market
                         with no book frame there takes kalshi_rest_books' top rows instead (first
                         seen at recv_us), which an outage of that poll (gaps source
                         kalshi_rest_books, or a restart) drops until it is back. A book dropped
                         is no book (STALE_BOOK) until a new one, whose touches are then first seen
                         anew, as TakeEngine forgets a touch it saw stale; a snapshot that only
                         replaces a live book (a resubscription) keeps them. The since-time is the
                         engine's: a touch price's first sighting, kept while only its size moves.
  volumes                with book_history: each market's traded contracts, cumulative, from
                         kalshi_ws's prints and kalshi_trades_rest's backfill (one each by
                         trade_id), or the REST rows' own volume for a market without prints.
  decide                 tools/take_rule.py's decide (deskd's take_decide), and college_params()
                         its COLLEGE params with this run's flags.

The score a decision reads (Score: college.json's ScoreMerge, which C3 builds in deskd). Each
source's rows become deskd's score states: first_seen is the first row of a run with the same
(state, period, scores, linescores), so a row that only moves the clock keeps it. A source counts
as polled at the decision unless its feed was in an outage (then at the outage's start, or its last
row since); fresh is polled at most --max-score-age-s ago. Both fresh: their state, scores (from A's
side) and period (when both read one) must agree, else no score (NO_SCORE, the why names both),
and agreeing they give the earlier first sighting. One fresh: that one. Neither: the fresher,
which SCORE_OLD then pulls. Between rows a score is taken as polled: the recorder polls a game in
play every few seconds, and only the gaps table says when it did not.

The fair (GameFair's rules, strat/game_fair.h). p0 is the effective mid of A's YES over both books
(bid max(bid_A, 1 - ask_B), ask min(ask_A, 1 - bid_B)) at a spread of 15c or less, while the score
says pre and before the scheduled start: the last such mid, dropped after 12 h. In play the model
prices the home team (its p0 the home side's) and the away market takes 1 - that:
  KXNCAAWBGAME   the WCBB file (--wcbb) through tools/train_bball.py's own feature code (predict,
                 the clock by its t_left), refused unless its golden rows reproduce to 1e-6
  KXNCAAWVMATCH  the volleyball file (--volley) through tools/volleyball.py (volley_state from the
                 linescores as strat/volley_model.cpp reads them, then match_p at q_for_p0), its
                 golden rows to 1e-9; a set whose points were not read prices it 0-0 (points false)
  KXNCAAMBGAME   the control (--control, the decile college_control picked): Stern's model, sigma
                 11 (the old desk's men's college value), reported and never gated
The gate names each model by the sha256 of the file's bytes, which deskd compares with the model it
loads.

The cases (replay_game). For each game matched on a board, from its first score row to its last,
decide() runs at every instant something it reads changed (a book's top, a score row, an outage's
edge, a market's volume), at each cool-down's end and signal run's quiet end, and at each whole
minute in play, with TakeEngine's own bookkeeping: a new score state starts a signal run (fired
clears), and a decision with no edge 10 s after the last that had one ends it; the game's takes,
last take and worst case (each fill's cost and fee); each market's position from the fills, which
is Systematic's there too (no other lane trades college games) and which gw's inventory, 20
contracts a market, caps a take's count by (IOCs leave no working orders); volleyball's set end
(when the sets total first went up); the in-game volume (since each market's last reading while
the score said pre); touch ages from book_history. A take is a case. It is filled at its own limit
exec_lag later (--exec-lag-s, 2: detection, the network and the venue) if the touch is still there
at that price or better, up to its size there (taken), else not at all (missed): a missed take
still counts as the run's and the game's take, as the engine's would. A fill takes its contracts
out of that touch (its price and since): every later decision and fill sees the touch's recorded
size less what the replay's takes filled there, and a touch they emptied is no touch (that side
empty) until the recorded top's price or since changes, the level behind it not being known (Hist
keeps the top). A taken case's net markouts a contract, + in its favour: the traded market's
two-sided mid 60 s and 300 s after the fill, or its payout once the game is over and settled,
less the price, less the order's own fee (kalshi_fee_order_cent at its filled count) over its
contracts; and at settlement. A case also keeps its edge, fair, size shown (what the replay's
fills left), how long the touch stood, its touch's age, the lead, the volume and which board
scored it.

The verdict (decisions 8). Per series, over the taken cases marked at 300 s: at least --min-cases
(50), a positive contract-weighted mean net markout, and a game-clustered t of at least --min-t (2;
CR1: each game's net dollars less the mean times its contracts, squared, summed, times G / (G - 1),
over the contracts squared). Volleyball is measured at score level point (every decision needs the
set's points) and set (only within 120 s of a set's end, points or not); the series passes at the
first level that does. KXNCAAWBGAME also needs 5(f)'s gate (wcbb_f): 150+ settled games with the
model's fair, the model's Brier on their minute samples in play at least as good as the effective
mid's, and 20+ Manual fills on the series (the overlay clicks, from --ops-research, deskd's
ops-research.sqlite) with a positive net 60 s markout; it is folded into the series' pass and why.
Reported beside it: frequency, durations, sizes, 60 s and settlement means, and the 300 s mean by
side (leader, trailer), by route and by edge.

The gate file (--write-gate FILE, never over an existing one without --force; written whole, then
renamed): {lane C1, stage live, verdict PASS when any series passes, else FAIL and why, computed,
tool, params (edge in dollars, which C4 reads as the lane's, and the rest of the run's), series:
{S: {pass, why, level, leader_only, model, model_sha256}}, evidence (the numbers)}, with no top-
level model_sha256: each series names its model. lane_gate_check() holds deskd's loader's rules
and tradable_series() its per-series rule; the self-test reads every file it writes through them.
tests/deskd/fixtures/college_gate_ok.json is the gate it writes on its synthetic season, with
tests/deskd/fixtures/wcbb_synthetic.json and volleyball_golden.json (the bytes tools/volleyball.py
--write gives models/volley_markov_v1.json) as the models; --write-fixture rewrites it.

The fee multiplier: Kalshi lists it on the series, which the recorder does not keep; a market row
that carries one is used, else --fee-mult (1, the standard taker fee), ASSUMED and printed.

Times are microseconds as the recorder writes them (recv_us); decide() takes ms. Prices are in
1/10,000 $ and sizes in 1/100 contract (deskd's Px and Qty, lip_join's UNIT and CT).

Input: the recorder's data directory (<data>/spool/<table>/*.jsonl, and <data>/parquet/<table>/**
when pyarrow is installed): kalshi_markets, kalshi_markets_daily, kalshi_ws, kalshi_rest_books,
kalshi_trades_rest, espn_scores, ncaa_scores, gaps, college_control. Read-only. --data is required
and --ops-research has no default: neither defaults to a path under data/ (decisions 11).

Usage:
  python3 tools/college_gate.py --data DIR [--series S,..] [--control] [--edge-c 20] [--exec-lag-s 2]
      [--max-score-age-s 5] [--clip 2] [--game-cap 10] [--thin 20000] [--min-cases 50] [--min-t 2.0]
      [--leader-only] [--fee-mult 1] [--wcbb FILE] [--volley FILE] [--ops-research FILE]
      [--wcbb-games 150] [--overlay-fills 20] [--json] [--write-gate FILE [--force]]
  python3 tools/college_gate.py --write-fixture tests/deskd/fixtures/college_gate_ok.json --force
  python3 tools/college_gate.py --self-test
"""
import argparse
import bisect
import contextlib
import hashlib
import heapq
import io
import json
import math
import os
import random
import sqlite3
import sys
import tempfile
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import espn_match as em   # noqa: E402  (deskd's ESPN matching)
import lip_join as lj     # noqa: E402  (the reader and the book replay)
import take_rule as tr    # noqa: E402  (deskd's take_decide)
import train_bball as tb  # noqa: E402  (the WCBB model's feature code, Stern's model)
import volleyball as vb   # noqa: E402  (the volleyball Markov model)
from lip_join import CT, UNIT   # noqa: E402

S_US = 1_000_000
SERIES = ('KXNCAAWBGAME', 'KXNCAAWVMATCH')   # C1's: one gate result each
CONTROL_SERIES = ('KXNCAAMBGAME',)           # the men's thin decile: reported, never gated
# NCAA.com's scoreboard for each series (recorder/config.py NCAA_SPORTS; ref/ncaa.h's table)
NCAA_SPORTS = {'KXNCAAWBGAME': 'basketball-women/d1', 'KXNCAAWVMATCH': 'volleyball-women/d1',
               'KXNCAAMBGAME': 'basketball-men/d1'}
SCORE_TABLES = (('ESPN', 'espn_scores'), ('NCAA', 'ncaa_scores'))
GAP_SOURCES = {'espn': 'ESPN', 'ncaa': 'NCAA'}

rows = lj.rows
decide = tr.decide


def college_params(edge=2000, clip=2 * UNIT, game_cap=10_000_000, thin=20000 * CT, max_score_age_ms=5000,
                   leader_only=False, points_required=True):
    """take_params(COLLEGE) with this run's flags: edge and clip in Money, game_cap in micro-dollars,
    thin (the in-game volume a thin game stays under) in Qty."""
    p = tr.params('COLLEGE')
    p.update(edge=edge, clip=clip, game_cap=game_cap, max_volume=thin, max_score_age_ms=max_score_age_ms,
             leader_only=leader_only, points_required=points_required)
    return p


def _int(v):
    return v if isinstance(v, int) and not isinstance(v, bool) else None


def _num(v):
    if isinstance(v, bool):
        return None
    if isinstance(v, (int, float)):
        return float(v)
    if isinstance(v, str):
        try:
            return float(v)
        except ValueError:
            return None
    return None


def _raw(r):
    try:
        j = json.loads(r.get('raw') or '{}')
    except (ValueError, TypeError):
        return {}
    return j if isinstance(j, dict) else {}


def series_of(ticker):
    return ticker.split('-')[0]


def espn_path(series):
    """The recorder's ESPN league path for a series ('basketball/womens-college-basketball')."""
    lg = em.espn_league_for(series + '-X')
    return lg.sport + '/' + lg.league if lg else None


def league_path(spec):
    """A recorder feed's league as written ('basketball/womens-college-basketball:50') without its
    groups."""
    return (spec or '').partition(':')[0].strip('/')


# ---- the games ----------------------------------------------------------------------------------
def games(data, series):
    """event -> {event, series, date, tickers [A, B, ...] (sorted), markets {ticker: {name, result,
    close_ms, fee_mult, fractional, status}}}, for the markets of these series."""
    latest, result = {}, {}
    for table in ('kalshi_markets', 'kalshi_markets_daily'):
        for r in rows(data, table):
            tk = r.get('ticker')
            if not isinstance(tk, str) or series_of(tk) not in series:
                continue
            raw = _raw(r)
            t = _int(r.get('recv_us')) or 0
            res = r.get('result') or raw.get('result')
            if res in ('yes', 'no') and t >= result.get(tk, (-1, None))[0]:
                result[tk] = (t, res)
            prev = latest.get(tk)
            if prev is not None and prev[0] > t:
                continue
            ev = raw.get('event_ticker') or r.get('event_ticker')
            if not ev and prev is not None:
                ev = prev[1]['event']
            mult = _num(raw.get('fee_multiplier'))
            latest[tk] = (t, {'event': ev, 'name': raw.get('yes_sub_title') or (prev[1]['name'] if prev else ''),
                              'close_ms': em.iso_ms((r.get('close_time') or raw.get('close_time') or '').replace(
                                  '+00:00', 'Z')) or None,
                              'fee_mult': mult if mult is not None else (prev[1]['fee_mult'] if prev else None),
                              'fractional': bool(raw.get('fractional_trading_enabled')),
                              'status': r.get('status') or raw.get('status')})
    out = {}
    for tk, (_, m) in sorted(latest.items()):
        if not m['event']:
            continue
        m['result'] = result.get(tk, (0, None))[1]
        g = out.setdefault(m['event'], {'event': m['event'], 'series': series_of(tk),
                                        'date': em.kalshi_event_date(m['event']), 'tickers': [], 'markets': {}})
        g['tickers'].append(tk)
        g['markets'][tk] = m
    return out


# ---- the scores ---------------------------------------------------------------------------------
def score_rows(data):
    """((src, league, game_id) -> rows by recv_us, outages): outages is src -> [(from_us, to_us,
    league path or None for every league)], from the gaps table."""
    out = {}
    for src, table in SCORE_TABLES:
        for r in rows(data, table):
            t, gid = _int(r.get('recv_us')), r.get('game_id')
            if t is None or not gid or not r.get('league'):
                continue
            out.setdefault((src, r['league'], str(gid)), []).append(r)
    for v in out.values():
        v.sort(key=lambda r: r['recv_us'])
    failed, spans = {}, []
    for r in rows(data, 'gaps'):
        a, b = _int(r.get('from_us')), _int(r.get('to_us'))
        if a is None or b is None or b < a:
            continue
        if r.get('source') == 'recorder' and r.get('kind') == 'restart':
            spans.extend((src, a, b, None) for src, _ in SCORE_TABLES)
            continue
        src = GAP_SOURCES.get(r.get('source'))
        if src is None or r.get('kind') not in ('poll_failed', 'recovered'):
            continue
        if r['kind'] == 'poll_failed':   # "<league spec>: <error>": that feed's outage, from a
            spec = (r.get('detail') or '').split(': ')[0]
            failed[(src, a)] = league_path(spec) if spec else None
        spans.append((src, a, b, r['kind']))
    outages = {src: [] for src, _ in SCORE_TABLES}
    for src, a, b, kind in spans:
        league = None if kind is None else failed.get((src, a))   # a recovered row: its poll_failed's league
        outages[src].append((a, b, league))
    for v in outages.values():
        v.sort(key=lambda x: (x[0], x[1], x[2] or ''))
    return out, outages


def match_games(gms, scores):
    """event -> {src: (score key, a_home)}: each event's ESPN and NCAA game by match_game, from the
    first row of each recorded game of the series' league; an event without both markets named
    is not matched."""
    by_league = {}
    for key, rs in scores.items():
        src, league, _ = key
        g = em.game_from_score_row(rs[0])
        if g.start_ms <= 0:
            continue
        d = em.espn_et_date(g.start_ms)
        lg = league_path(league) if src == 'ESPN' else league
        by_league.setdefault((src, lg), {}).setdefault(d, []).append((g, key))
    out = {}
    for ev, gm in gms.items():
        if len(gm['tickers']) != 2:
            continue
        ks = em.kalshi_games([{'ticker': tk, 'event_ticker': ev, 'yes_sub_title': gm['markets'][tk]['name']}
                              for tk in gm['tickers']])
        k = ks.get(ev)
        if k is None or not k.date:
            continue
        day = em.day_ms(k.date)
        dates = [em.espn_et_date(day + d * em.DAY_MS) for d in (-1, 0, 1)]
        for src in ('ESPN', 'NCAA'):
            lg = espn_path(gm['series']) if src == 'ESPN' else NCAA_SPORTS.get(gm['series'])
            cands = [c for d in dates for c in by_league.get((src, lg), {}).get(d, [])]
            m = em.match_game(k, [g for g, _ in cands])
            if m is not None:
                key = next(key for g, key in cands if g.id == m.id)
                out.setdefault(ev, {})[src] = (key, m.a_home)
    return out


# ---- the books ----------------------------------------------------------------------------------
class Hist:
    """One market's top of book over time: entries (t_us, top) with top None (no book) or (bid,
    bid_qty, ask, ask_qty, bid_since_us, ask_since_us), a missing side None, since 0."""

    __slots__ = ('times', 'tops', 'src')

    def __init__(self, src):
        self.times, self.tops, self.src = [], [], src

    def last(self):
        return self.tops[-1] if self.tops else None

    def add(self, t, bid, bid_qty, ask, ask_qty):
        """A book seen at t (bid/ask None for an empty side): kept when the top changed, each
        touch dated from its price's first sighting."""
        same = bool(self.times) and self.times[-1] == t   # replacing a change at this instant: dated
        prev = (self.tops[-2] if len(self.tops) > 1 else None) if same else self.last()   # against the one before
        bs = (prev[4] if prev is not None and bid == prev[0] else t) if bid is not None else 0
        as_ = (prev[5] if prev is not None and ask == prev[2] else t) if ask is not None else 0
        top = (bid, bid_qty, ask, ask_qty, bs, as_)
        if not same and prev is not None and top == prev:
            return
        if same:   # two changes at one instant: the later stands
            self.tops[-1] = top
        else:
            self.times.append(t)
            self.tops.append(top)

    def gone(self, t):
        """No book from t."""
        if self.tops and self.tops[-1] is None:
            return
        if self.times and self.times[-1] == t:
            self.tops[-1] = None
        else:
            self.times.append(t)
            self.tops.append(None)

    def at(self, t):
        """The top in force at t (frames received at t count): None before the first or while gone."""
        i = bisect.bisect_right(self.times, t) - 1
        return self.tops[i] if i >= 0 else None

    def changes(self, a, b):
        """The times of the changes in [a, b]."""
        return self.times[bisect.bisect_left(self.times, a):bisect.bisect_right(self.times, b)]


class Vol:
    """One market's traded contracts so far: (t_us, cumulative Qty), oldest first."""

    __slots__ = ('times', 'cum')

    def __init__(self, points):
        self.times, self.cum = [], []
        total = 0
        for t, q in sorted(points):
            total += q
            self.times.append(t)
            self.cum.append(total)

    def at(self, t):
        i = bisect.bisect_right(self.times, t) - 1
        return self.cum[i] if i >= 0 else 0


class Tops(lj.Replay):
    """lip_join's replay, keeping each kept market's top every time it changes (Hist), and its
    prints by trade_id. A book replaced within one frame (a resubscription's snapshot) is not
    a drop; one dropped otherwise is gone until the next snapshot. downs: the recorder's
    disconnects and restarts (lip_join's down_instants), each dropping every book at its own
    instant before the first frame after it, as lip_join's drive does."""

    def __init__(self, keep, downs=()):
        super().__init__(keep)
        self.hist, self.prints = {}, {}
        self.now = 0
        self.downs, self.di = list(downs), 0
        self.in_row, self.row_drops = False, set()
        self.on_drop = self.dropped

    def dropped(self, tk):
        if self.in_row:
            self.row_drops.add(tk)
        elif tk in self.hist:
            self.hist[tk].gone(self.now)

    def note(self, tk, t):
        b = self.books.get(tk)
        h = self.hist.setdefault(tk, Hist('ws'))
        if b is None:
            if h.tops:
                h.gone(t)
            return
        bid, ask = b.best_bid(), b.best_ask()
        h.add(t, bid, b.bids[bid] if bid is not None else None, ask, b.asks[ask] if ask is not None else None)

    def end(self, t):
        """The data ends at t: no book is known after it."""
        self.now = t
        for h in self.hist.values():
            h.gone(t)

    def row(self, r):
        t = r.get('recv_us')
        if isinstance(t, int) and not isinstance(t, bool):
            while self.di < len(self.downs) and self.downs[self.di] < t:
                self.now = self.downs[self.di]
                self.drop_all()
                self.di += 1
            self.now = t
        self.in_row, self.row_drops = True, set()
        super().row(r)
        self.in_row = False
        tk = r.get('ticker')
        for d in self.row_drops - {tk}:
            if d in self.hist:
                self.hist[d].gone(self.now)
        if r.get('type') in ('orderbook_snapshot', 'orderbook_delta') and tk and (self.keep is None or self.keep(tk)):
            if tk in self.books or tk in self.row_drops or tk in self.hist:
                self.note(tk, self.now)
        elif r.get('type') == 'trade' and tk and (self.keep is None or self.keep(tk)):
            msg = _raw(r).get('msg')
            if isinstance(msg, dict):
                q = lj.qty_of(msg.get('count_fp'))
                tid = msg.get('trade_id')
                if q and q > 0 and tid:
                    self.prints.setdefault(tk, {}).setdefault(tid, (self.now, q))


def _rest_px(v):
    v = _num(v)
    if v is None:
        return None
    p = int(round(v * UNIT))
    return p if 0 < p < UNIT else None


def _rest_qty(v):
    v = _num(v)
    return int(round(v * CT)) if v is not None and v > 0 else None


def book_history(data, tickers):
    """(ticker -> Hist, ticker -> Vol) for these markets: see the module docstring."""
    want = set(tickers)
    rp = Tops(lambda tk: tk in want, lj.down_instants(data))
    last = None
    for r in rows(data, 'kalshi_ws'):   # in the order received
        t = r.get('recv_us')
        if isinstance(t, int) and not isinstance(t, bool):
            rp.row(r)
            last = t if last is None else max(last, t)
    if last is not None:
        rp.end(last + 1)
    hist = {tk: h for tk, h in rp.hist.items() if h.tops}
    prints = rp.prints
    for r in rows(data, 'kalshi_trades_rest'):   # the backfill: prints the socket missed
        tk, tid = r.get('ticker'), r.get('trade_id')
        q = _rest_qty(r.get('count'))
        t = _int(r.get('created_us')) or _int(r.get('recv_us'))
        if tk in want and tid and q and t is not None:
            prints.setdefault(tk, {}).setdefault(tid, (t, q))
    # REST tops for the markets the socket never showed, and their volumes
    rest, rest_vol = {}, {}
    for r in rows(data, 'kalshi_rest_books'):
        tk = r.get('ticker')
        if tk not in want or tk in hist or r.get('kind') != 'top' or _int(r.get('recv_us')) is None:
            continue
        bid, ask = _rest_px(r.get('yes_bid')), _rest_px(r.get('yes_ask'))
        bq, aq = _rest_qty(r.get('yes_bid_size')), _rest_qty(r.get('yes_ask_size'))
        rest.setdefault(tk, []).append((r['recv_us'], bid if bq else None, bq if bid else None,
                                        ask if aq else None, aq if ask else None))
        v = _rest_qty(r.get('volume'))
        if v is not None:
            rest_vol.setdefault(tk, []).append((r['recv_us'], v))
    if rest:
        downs = []
        for r in rows(data, 'gaps'):
            a, b = _int(r.get('from_us')), _int(r.get('to_us'))
            if a is None or b is None:
                continue
            if r.get('source') == 'kalshi_rest_books' and r.get('kind') in ('poll_failed', 'recovered') or (
                    r.get('source') == 'recorder' and r.get('kind') == 'restart'):
                downs.append((a, b))
        for tk, tops in rest.items():
            h = Hist('rest')
            ev = sorted([(t, 1, top) for t, *top in tops] + [(a, 0, None) for a, _ in downs] +
                        [(b, 2, None) for _, b in downs], key=lambda x: (x[0], x[1]))
            last, down = None, 0
            for t, kind, top in ev:
                if kind == 0:          # a poll outage starts: no book
                    down += 1
                    h.gone(t)
                elif kind == 2:        # it ends: the last book known, seen again now
                    down = max(0, down - 1)
                    if not down and last is not None:
                        h.add(t, *last)
                else:
                    last = top
                    if not down:
                        h.add(t, *top)
            hist[tk] = h
    vols = {}
    for tk in want:
        if prints.get(tk):
            vols[tk] = Vol(prints[tk].values())
        elif rest_vol.get(tk):   # cumulative already: as increments
            pts, prev = [], 0
            for t, v in sorted(rest_vol[tk]):
                pts.append((t, v - prev))
                prev = v
            vols[tk] = Vol(pts)
    return hist, vols


# ---- the score a decision reads (ScoreMerge) ------------------------------------------------------
def period_of(text):
    """A score row's period as a number: its leading digits, else 0 (ESPN's '2'; NCAA.com's
    currentPeriod '2nd', '1', 'OT', 'HALF')."""
    s = str(text or '').strip()
    n = 0
    for c in s:
        if not c.isdigit():
            break
        n = n * 10 + int(c)
    return n


def _lines(raw):
    """(home, away) linescores from an ESPN row's raw competitors: whole points, -1 where unread."""
    out = {'home': [], 'away': []}
    for c in raw.get('competitors') or []:
        if isinstance(c, dict) and c.get('homeAway') in out:
            vals = []
            for v in c.get('linescores') or []:
                v = _num(v)
                vals.append(int(v) if v is not None and v >= 0 and v == int(v) else -1)
            out[c['homeAway']] = vals
    return out['home'], out['away']


def score_states(rs, src):
    """A game's rows as deskd's score states, oldest first: t (the row, first seen), state, period,
    clock_s, scores and linescores (home, away), start_ms, and first_seen: the first row of the run
    of rows with the same (state, period, scores, linescores), deskd's first_seen_ms, which a row
    that only moves the clock does not change."""
    out = []
    for r in rs:
        raw = _raw(r)
        home_lines, away_lines = _lines(raw) if src == 'ESPN' else ([], [])
        st = {'t': r['recv_us'], 'state': r.get('state') or '', 'clock_s': em.clock_seconds(r.get('clock') or ''),
              'period': period_of(r.get('period')),
              'home_score': _int(r.get('home_score')) or 0, 'away_score': _int(r.get('away_score')) or 0,
              'home_lines': home_lines, 'away_lines': away_lines, 'start_ms': em.iso_ms(r.get('start_time') or '')}
        st['digest'] = (st['state'], st['period'], st['home_score'], st['away_score'], tuple(home_lines),
                        tuple(away_lines))
        prev = out[-1] if out else None
        st['first_seen'] = prev['first_seen'] if prev is not None and prev['digest'] == st['digest'] else st['t']
        out.append(st)
    return out


class Score:
    """One game's score as deskd would merge it (college.json's ScoreMerge, ref/scores.h): each
    source's latest state and when it was last polled; a source whose feed is in an outage was last
    polled at the outage's start (or at its last row since). The fresher of the two, once its last
    poll is at most max_age old; when both are, and their state, scores and (both read) period
    differ, no score: the why names both. When they agree, the earlier first sighting."""

    def __init__(self, sources, max_age_us):
        self.src = {}   # src -> (states, times, a_home, outages merged: disjoint, sorted)
        for src, (states, a_home, outs) in sources.items():
            merged = []
            for a, b in sorted(outs):
                if merged and a <= merged[-1][1]:
                    merged[-1] = (merged[-1][0], max(merged[-1][1], b))
                else:
                    merged.append((a, b))
            self.src[src] = (states, [s['t'] for s in states], a_home, merged)
        self.max_age = max_age_us

    def latest(self, src, t):
        """(state, last poll) of one source at t, or (None, 0)."""
        states, times, _, outs = self.src[src]
        i = bisect.bisect_right(times, t) - 1
        if i < 0:
            return None, 0
        j = bisect.bisect_right(outs, (t, float('inf'))) - 1   # the last outage begun by t
        if j >= 0 and outs[j][0] <= t < outs[j][1]:
            return states[i], max(outs[j][0], states[i]['t'])
        return states[i], t

    def final(self, t):
        """Does any source say the game is over by t?"""
        return any((self.latest(src, t)[0] or {}).get('state') == 'post' for src in self.src)

    def at(self, t):
        """A's score at t: a dict with state ('' for none), why, src, fetched and first seen (us),
        start_ms, a_home, a_score, b_score, the home side's period, clock_s, scores and lines."""
        got = []
        for src in ('ESPN', 'NCAA'):
            if src in self.src:
                st, polled = self.latest(src, t)
                if st is not None:
                    got.append((src, st, polled, self.src[src][2]))
        if not got:
            return {'state': '', 'why': ''}
        fresh = [g for g in got if t - g[2] <= self.max_age]
        pick, first_seen, why = None, None, ''
        if len(fresh) == 2:
            (_, e, _, eh), (_, n, _, nh) = fresh
            ea = (e['home_score'], e['away_score']) if eh else (e['away_score'], e['home_score'])
            na = (n['home_score'], n['away_score']) if nh else (n['away_score'], n['home_score'])
            same = e['state'] == n['state'] and ea == na and (
                not e['period'] or not n['period'] or e['period'] == n['period'])
            if not same:
                why = 'ESPN has %s %d-%d (period %d) and NCAA %s %d-%d (period %d)' % (
                    e['state'], ea[0], ea[1], e['period'], n['state'], na[0], na[1], n['period'])
                return {'state': '', 'why': why}
            pick, first_seen = fresh[0], min(e['first_seen'], n['first_seen'])
        elif fresh:
            pick = fresh[0]
        else:
            pick = max(got, key=lambda g: g[2])   # neither fresh: the fresher, which the age then pulls
        src, st, polled, a_home = pick
        hs, as_ = st['home_score'], st['away_score']
        return {'state': st['state'], 'why': '', 'src': src, 'fetched': polled,
                'first_seen': first_seen if first_seen is not None else st['first_seen'],
                'start_ms': st['start_ms'], 'a_home': a_home, 'a_score': hs if a_home else as_,
                'b_score': as_ if a_home else hs, 'period': st['period'], 'clock_s': st['clock_s'],
                'home_score': hs, 'away_score': as_, 'home_lines': st['home_lines'], 'away_lines': st['away_lines']}


# ---- the fairs (GameFair) -----------------------------------------------------------------------
def _won_at(w, l, target, win_by=2):
    return w >= target and w - l >= win_by and (w == target or w - l == win_by)


def _past(w, l, target, win_by=2):
    return w >= target and w - l >= win_by


def volley_state(period, lines, scores, rules=vb.RULES):
    """strat/volley_model.cpp volley_state, from one side (lines and scores as (yes, opp)): (state for
    volleyball.live_p, points read) or (None, why)."""
    n, need = period, (rules['best_of'] + 1) // 2
    y, o = lines
    if n < 1 or n > rules['best_of']:
        return None, 'set %d is not a set of a best of %d' % (n, rules['best_of'])
    if len(y) != len(o):
        return None, "the two teams' linescores differ in length"
    if len(y) < n - 1:
        return None, 'set %d with linescores for %d sets' % (n, len(y))
    sy = so = 0
    for k in range(n - 1):   # the sets played
        a, b, t = y[k], o[k], vb.set_target(k + 1, rules)
        if a < 0 or b < 0:
            return None, "set %d's points were not read" % (k + 1)
        if _won_at(a, b, t):
            sy += 1
        elif _won_at(b, a, t):
            so += 1
        else:
            return None, 'set %d has no winner at %d-%d' % (k + 1, a, b)
    if sy >= need or so >= need:
        return None, 'the match was decided before set %d' % n
    before, finished, points, pa, pb = (sy, so), False, False, 0, 0
    if len(y) >= n and y[n - 1] >= 0 and o[n - 1] >= 0:   # the set being played
        a, b, t = y[n - 1], o[n - 1], vb.set_target(n, rules)
        points = True
        if _past(a, b, t) or _past(b, a, t):
            if not _won_at(a, b, t) and not _won_at(b, a, t):
                return None, "set %d's %d-%d runs past its end" % (n, a, b)
            sy, so = (sy + 1, so) if a > b else (sy, so + 1)   # between sets: the next one at 0-0
            finished = True
        else:
            pa, pb = a, b
    for k in range(n, len(y)):
        if y[k] > 0 or o[k] > 0:
            return None, 'linescores past set %d have points' % n
    if tuple(scores) != before and not (finished and tuple(scores) == (sy, so)):
        return None, 'the linescores give %d-%d in sets, the score %d-%d' % (sy, so, scores[0], scores[1])
    return {'sets_yes': sy, 'sets_opp': so, 'pts_yes': pa, 'pts_opp': pb}, points


_Q = {}


def _q_for(p0, rules):
    """volleyball.q_for_p0, remembered: live_p is match_p at it, and the bisection is the cost."""
    key = (p0, tuple(sorted(rules.items())))
    if key not in _Q:
        _Q[key] = vb.q_for_p0(p0, rules)
    return _Q[key]


def mens_t_left(period, clock_s):
    """Men's college basketball, two halves of 1200 s: regulation time left; overtime 0; None
    without a clock."""
    if period < 1 or clock_s < 0 or clock_s > 1200:
        return None
    return (2 - period) * 1200.0 + clock_s if period <= 2 else 0.0


def fair_of(model, ref, p0):
    """GameFair's row for market A: (P(A's team wins) or None, points read, why). From the home
    team's side and 1 - that for the away team, as GameFair prices; p0 is A's."""
    if ref['state'] != 'in':
        return None, True, {'pre': 'not started', 'post': 'final'}.get(ref['state'], 'state "%s"' % ref['state'])
    if p0 is None:
        return None, True, 'no pregame price: none was captured before the start'
    p0h = p0 if ref['a_home'] else 1 - p0
    margin = ref['home_score'] - ref['away_score']
    kind, m = model['kind'], model['obj']
    points = True
    if kind == 'wcbb':
        c = tb.t_left(tb.LEAGUES['wcbb'], ref['period'], ref['clock_s'])
        if c is None:
            return None, True, 'no game clock in period %d' % ref['period']
        p = tb.predict(m, c[0], margin, 0, p0h, c[1])
    elif kind == 'volley':
        s, points = volley_state(ref['period'], (ref['home_lines'], ref['away_lines']),
                                 (ref['home_score'], ref['away_score']), m['rules'])
        if s is None:
            return None, True, points
        q = _q_for(p0h, m['rules'])
        p = None if q is None else vb.match_p(q, s['sets_yes'], s['sets_opp'], s['pts_yes'], s['pts_opp'], m['rules'])
    else:   # stern: the men's control, report only
        t = mens_t_left(ref['period'], ref['clock_s'])
        if t is None:
            return None, True, 'no game clock in period %d' % ref['period']
        p = tb.stern_p(m['sigma'], t, margin, p0h, 2400.0)
    if p is None or not math.isfinite(p):
        return None, points, 'the model gives no fair here'
    return (p if ref['a_home'] else 1 - p), points, ''


def to_px(p):
    """GameFair's to_px: llround(clamp(p) x 10,000)."""
    return int(math.floor(min(1.0, max(0.0, p)) * UNIT + 0.5))


def effective(top_a, top_b):
    """take_rule's effective_book over two Hist tops (None: no book)."""
    def leg(top):
        if top is None:
            return {'stale': True, 'bid': None, 'ask': None}
        return {'stale': False, 'bid': top[0], 'ask': top[2]}
    return tr.effective_book(leg(top_a), leg(top_b))


_MODELS = {}


def load_model(kind, path):
    """(model, None) or (None, why): the WCBB file through the trainer's feature code with its golden
    rows to 1e-6 (deskd's load_bball_model), the volleyball file with its golden rows to 1e-9
    (load_volley_model); sha256 of the bytes, as the gate names a model. Remembered by its bytes."""
    try:
        with open(path, 'rb') as fh:
            text = fh.read()
    except OSError as e:
        return None, '%s: %s' % (path, e)
    key = (kind, path, hashlib.sha256(text).hexdigest())
    if key not in _MODELS:
        _MODELS[key] = _load_model(kind, path, text)
    return _MODELS[key]


def _load_model(kind, path, text):
    try:
        obj = json.loads(text)
    except ValueError as e:
        return None, '%s: %s' % (path, e)
    if not isinstance(obj, dict):
        return None, '%s: not a JSON object' % path
    if kind == 'wcbb':
        if obj.get('model') != 'wcbb_logit':
            return None, '%s: model is not "wcbb_logit"' % path
        err = tb.spec_loader_check(obj, exact_keys=False)
        if err is None:
            worst = max(abs(tb.predict(obj, g['in']['t_left_s'], g['in']['margin'], g['in']['possession'],
                                       g['in']['p0'], bool(g['in'].get('ot', False))) - g['p']) for g in obj['golden'])
            err = None if worst <= 1e-6 else 'a golden row off by %.2e' % worst
    else:
        err = None
        if obj.get('model') != 'volley_markov' or obj.get('version') != 1:
            err = 'model is not volley_markov version 1'
        elif not isinstance(obj.get('rules'), dict) or len(obj.get('golden') or []) < 10:
            err = 'rules or 10 golden rows missing'
        else:
            err = vb.golden_error(obj)
    if err:
        return None, '%s: %s' % (path, err)
    return {'kind': kind, 'obj': obj, 'name': obj['model'], 'path': path,
            'sha256': hashlib.sha256(text).hexdigest()}, None


STERN = {'kind': 'stern', 'obj': {'sigma': 11.0}, 'name': 'stern', 'path': '', 'sha256': None}   # sports_live.cpp


# ---- the cases ----------------------------------------------------------------------------------
class Opts:
    """One run's rule and measurement: the COLLEGE params, the execution lag, the fee multiplier
    assumed where a market row gives none, and TakeEngine's run_quiet_ms."""

    def __init__(self, params, exec_lag_us=2 * S_US, fee_mult=1.0, run_quiet_us=10 * S_US):
        self.p, self.lag, self.fee_mult, self.quiet = params, exec_lag_us, fee_mult, run_quiet_us


def _left(took, tk, side, px, qty, since):
    """One side's touch, (price, Qty, since), less what the replay's own takes filled at it (took:
    (ticker, BUY or SELL, price, since) -> Qty); (None, None, 0) when it is empty or they emptied it."""
    q = qty - took.get((tk, side, px, since), 0) if px is not None else 0
    return (px, q, since) if q > 0 else (None, None, 0)


def _leg(g, tk, top, position, took):
    """take_rule's leg for one market from its Hist top (None: no book), each touch less the
    replay's own fills there (_left). C1's position from those fills is also Systematic's there (no
    other lane trades college games), which gw's inventory caps; its takes are IOCs, so no working
    orders."""
    m = g['markets'][tk]
    own = {'ticker': tk, 'fee_mult': m['fee_mult'], 'fractional': m['fractional'], 'position': position,
           'sys_position': position, 'sys_buying': 0, 'sys_selling': 0}
    if top is None:
        return dict(own, stale=True, bid=None, bid_qty=None, ask=None, ask_qty=None, bid_since_ms=0, ask_since_ms=0)
    bid, bid_qty, bs = _left(took, tk, 'SELL', top[0], top[1], top[4])
    ask, ask_qty, as_ = _left(took, tk, 'BUY', top[2], top[3], top[5])
    return dict(own, stale=False, bid=bid, bid_qty=bid_qty, ask=ask, ask_qty=ask_qty, bid_since_ms=bs // 1000,
                ask_since_ms=as_ // 1000)


def mark_at(hist, score, result, t):
    """The traded market's value at t, Px: its payout once the game is over and the result known,
    else its two-sided mid (a half-tick mid is kept: a float), else None (not marked)."""
    if result in ('yes', 'no') and score.final(t):
        return UNIT if result == 'yes' else 0
    top = hist.at(t) if hist is not None else None
    if top is None or top[0] is None or top[2] is None:
        return None
    return (top[0] + top[2]) / 2.0


def replay_game(g, score, hists, vols, model, o):
    """One game, as C1 would have traded it: every instant something deskd reads changed (a book, a
    score, an outage's edge), each cool-down's and signal run's end, and each whole minute in play,
    decide() on what was knowable then. Returns {cases, pulls, watched, p0, samples}."""
    p = o.p
    a, b = g['tickers']
    ha, hb = hists.get(a), hists.get(b)
    times = set()
    for h in (ha, hb):
        if h is not None:
            times.update(h.times)
    for tk in (a, b):   # the volume's readings: the last before the tip is the pregame one
        if vols.get(tk) is not None:
            times.update(vols[tk].times)
    first_in = None
    for src, (states, stimes, _, outs) in score.src.items():
        times.update(stimes)
        for x, y in outs:
            times.update((x, y))
        for s in states:
            if s['state'] == 'in' and (first_in is None or s['t'] < first_in):
                first_in = s['t']
    # from the game's first score row to its last (the final one, once it has one): before it the
    # board does not show the game, after it nothing is taken
    start = min((t for _, ts, _, _ in score.src.values() for t in ts), default=None)
    if start is None:
        return {'cases': [], 'pulls': {}, 'watched': False, 'p0': None, 'samples': []}
    end = max(t for _, ts, _, _ in score.src.values() for t in ts)
    if first_in is not None and model['kind'] == 'wcbb':   # the minute samples (wcbb_f)
        times.update(range(first_in + 60 * S_US, end, 60 * S_US))
    heap = sorted(t for t in times if start <= t <= end)
    seen = set(heap)
    heapq.heapify(heap)

    def at_later(t):
        if t not in seen and t <= end + 600 * S_US:
            seen.add(t)
            heapq.heappush(heap, t)
    st = {'run': None, 'edge_t': 0, 'fired': [False, False], 'noted': [False, False], 'last_take': 0, 'takes': 0,
          'used': 0, 'pos': {a: 0, b: 0}, 'sets': -1, 'set_end': 0, 'pre_vol': {}, 'p0': None, 'p0_t': 0}
    cases, pulls, samples, watched, fairs = [], {}, [], False, {}
    took = {}   # the contracts the replay's fills took from each touch (_left)
    result = g['markets'][a]['result']
    while heap:
        t = heapq.heappop(heap)
        ref = score.at(t)
        ta, tb_ = ha.at(t) if ha else None, hb.at(t) if hb else None
        # p0: the effective mid while the score says pre, before the start, at 15c or less
        if ref['state'] == 'pre' and ref['start_ms'] > 0 and t // 1000 < ref['start_ms']:
            e = effective(ta, tb_)
            if e is not None and e[1] - e[0] <= 1500:
                st['p0'], st['p0_t'] = (e[0] + e[1]) / 2.0 / UNIT, t
        p0 = st['p0'] if st['p0'] is not None and t - st['p0_t'] <= 12 * 3600 * S_US else None
        if ref['state']:
            key = (ref['state'], ref['period'], ref['clock_s'], ref['home_score'], ref['away_score'],
                   tuple(ref['home_lines']), tuple(ref['away_lines']), ref['a_home'], p0)
            if key not in fairs:
                fairs[key] = fair_of(model, ref, p0)
            fair, points, fair_why = fairs[key]
        else:
            fair, points, fair_why = None, True, ref['why']
        watched = watched or fair is not None
        # volleyball: when the sets total went up (its first sighting)
        if model['kind'] == 'volley' and ref['state']:
            sets = ref['a_score'] + ref['b_score']
            if st['sets'] >= 0 and sets > st['sets']:
                st['set_end'] = min(t, ref['first_seen'])
            st['sets'] = sets
        # the in-game volume: since each market's last pregame reading
        volume, known = 0, True
        for tk, h in ((a, ha), (b, hb)):
            v = vols.get(tk)
            if v is None and h is None:
                known = False
                continue
            now_v = v.at(t) if v is not None else 0
            if ref['state'] == 'pre':
                st['pre_vol'][tk] = now_v
            volume += now_v - min(st['pre_vol'].get(tk, 0), now_v)
        # a new score state starts a new signal run
        if ref['state'] and ref['first_seen'] != st['run']:
            st['run'] = ref['first_seen']
            st['fired'], st['noted'] = [False, False], [False, False]
        inp = {'a': _leg(g, a, ta, st['pos'][a], took), 'b': _leg(g, b, tb_, st['pos'][b], took),
               'state': ref['state'], 'src': ref.get('src', ''), 'score_why': ref['why'],
               'fetched_ms': ref.get('fetched', 0) // 1000, 'first_seen_ms': ref.get('first_seen', 0) // 1000,
               'start_ms': ref.get('start_ms', 0), 'a_score': ref.get('a_score', 0), 'b_score': ref.get('b_score', 0),
               't_left_s': None, 'ot': False, 'points': points, 'set_end_ms': st['set_end'] // 1000,
               'p0': to_px(p0) if p0 is not None else None, 'fair': to_px(fair) if fair is not None else None,
               'fair_why': fair_why, 'espn': None, 'espn_seen_ms': 0, 'now_ms': t // 1000,
               'last_take_ms': st['last_take'] // 1000, 'game_used': st['used'], 'game_takes': st['takes'],
               'fired': list(st['fired']), 'private_up': True, 'not_tradable': '',
               'volume': volume if known else p['max_volume']}
        for leg in (inp['a'], inp['b']):
            if leg['fee_mult'] is None:
                leg['fee_mult'] = o.fee_mult
        if ref['state'] == 'in' and fair is not None and result in ('yes', 'no') and model['kind'] == 'wcbb':
            e = effective(ta, tb_)
            if e is not None and first_in is not None and (t - first_in) % (60 * S_US) == 0:
                samples.append((fair, (e[0] + e[1]) / 2.0 / UNIT, 1.0 if result == 'yes' else 0.0))
        d = tr.decide(inp, p)
        # the run: its edge seen now, or a decision without one past the quiet time ends it
        if tr.has_edge(d, p['mode']):
            st['edge_t'] = t
        elif d.get('dir', -1) >= 0:
            if t - st['edge_t'] >= o.quiet:
                st['fired'], st['noted'] = [False, False], [False, False]
            else:   # the run ends then unless the edge is back
                at_later(st['edge_t'] + o.quiet)
        if 'take' not in d:
            if d.get('route') and d['pull'] in ('BAND', 'TOUCH_AFTER_SCORE', 'TRAILER') and not st['noted'][d['dir']]:
                pulls[d['pull']] = pulls.get(d['pull'], 0) + 1
                st['noted'][d['dir']] = True
            continue
        k = d['take']
        st['fired'][k['dir']] = True
        st['last_take'], st['takes'] = t, st['takes'] + 1
        at_later(t + p['cool_ms'] * 1000)
        cases.append(case_of(g, k, inp, ref, t, hists, score, o, took))
        c = cases[-1]
        fq = int(round(c['filled'] * CT))   # its worst case now counts in the game
        if fq:
            st['used'] += k['team_px'] * fq + 100 * tr.kalshi_fee_order_cent(
                k['team_px'], fq, inp['a' if k['ticker'] == a else 'b']['fee_mult'])
            st['pos'][k['ticker']] += fq if k['side'] == 'BUY' else -fq
    return {'cases': cases, 'pulls': pulls, 'watched': watched, 'p0': st['p0'], 'samples': samples}


def case_of(g, k, inp, ref, t, hists, score, o, took):
    """A take as a case: filled at the touch exec_lag later, up to what the replay's earlier fills
    left there (taken; the fill joins took), or not (missed), and its net markouts a contract at 60 s,
    300 s and settlement, at the order's own fee (its filled count's)."""
    tk, side, price = k['ticker'], k['side'], k['price']
    leg = inp['a'] if tk == inp['a']['ticker'] else inp['b']
    h = hists.get(tk)
    tf = t + o.lag
    top = h.at(tf) if h is not None else None
    filled = 0
    if top is not None:
        px, q, since = _left(took, tk, side, *((top[2], top[3], top[5]) if side == 'BUY' else (top[0], top[1], top[4])))
        if px is not None and (px <= price if side == 'BUY' else px >= price):
            step = 1 if leg['fractional'] else CT
            filled = min(k['count'], q // step * step)
            if filled:
                took[(tk, side, px, since)] = took.get((tk, side, px, since), 0) + filled
    # how long the touch stood at that price, from the decision
    dur, i = None, bisect.bisect_right(h.times, t) if h is not None else 0
    while h is not None and i < len(h.times):
        nt = h.tops[i]
        if nt is None or (nt[2] if side == 'BUY' else nt[0]) != price:
            dur = (h.times[i] - t) / S_US
            break
        i += 1
    result = g['markets'][tk]['result']
    fee = tr.kalshi_fee_order_cent(k['team_px'], filled, leg['fee_mult']) if filled else 0
    sign = 1 if side == 'BUY' else -1

    def net(mark):
        if not filled or mark is None:
            return None
        return round((sign * (mark - price) - fee * CT / filled) / UNIT, 6)
    settle = None
    if result in ('yes', 'no'):
        settle = net(UNIT if result == 'yes' else 0)
    team = inp['a'] if k['dir'] == 0 else inp['b']
    return {'event': g['event'], 'ts_ms': t // 1000, 'dir': 'AB'[k['dir']], 'team': team['ticker'], 'ticker': tk,
            'side': side, 'price': price / UNIT, 'count': k['count'] / CT, 'size_shown': (
                leg['ask_qty'] if side == 'BUY' else leg['bid_qty']) / CT, 'edge': k['edge'] / UNIT,
            'fair': k['ref'] / UNIT, 'team_px': k['team_px'] / UNIT, 'score_src': ref['src'],
            'score': '%d-%d' % (inp['a_score'], inp['b_score']), 'lead': (inp['a_score'] - inp['b_score']) * (
                1 if k['dir'] == 0 else -1), 'touch_age_s': (t // 1000 - k['touch_since_ms']) / 1000.0,
            'volume': inp['volume'] / CT, 'dur_s': dur, 'state': 'taken' if filled else 'missed',
            'filled': filled / CT, 'fee': fee / UNIT,
            'mo60_net': net(mark_at(h, score, result, tf + 60 * S_US)),
            'mo300_net': net(mark_at(h, score, result, tf + 300 * S_US)), 'settle_net': settle}


# ---- the verdict --------------------------------------------------------------------------------
def clustered(cases, key='mo300_net'):
    """The contract-weighted mean of a net markout over the taken cases marked at that horizon, and
    its standard error clustered by game (CR1: sum over games of (Y_g - mean N_g)^2 x G / (G - 1),
    over (sum N_g)^2, with Y_g a game's net dollars and N_g its contracts): {n, contracts, games,
    mean, se, t}; mean, se and t None without cases, se and t None with one game or no spread."""
    by = {}
    for c in cases:
        if c['state'] == 'taken' and c.get(key) is not None:
            y, n = by.get(c['event'], (0.0, 0.0))
            by[c['event']] = (y + c['filled'] * c[key], n + c['filled'])
    n_cases = sum(1 for c in cases if c['state'] == 'taken' and c.get(key) is not None)
    total_n = sum(n for _, n in by.values())
    out = {'n': n_cases, 'contracts': round(total_n, 2), 'games': len(by), 'mean': None, 'se': None, 't': None}
    if not by or total_n <= 0:
        return out
    mean = sum(y for y, _ in by.values()) / total_n
    out['mean'] = round(mean, 6)
    g = len(by)
    if g >= 2:
        var = g / (g - 1) * sum((y - mean * n) ** 2 for y, n in by.values()) / total_n ** 2
        if var > 0:
            out['se'] = round(math.sqrt(var), 6)
            out['t'] = round(mean / math.sqrt(var), 3)
    return out


def verdict(cases, min_cases=50, min_t=2.0, key='mo300_net'):
    """(pass, why, numbers): the plan's 50+ cases with a positive net markout (decisions 8: the
    mean 300 s net markout above 0 and a game-clustered t of at least 2)."""
    s = clustered(cases, key)
    if s['n'] < min_cases:
        return False, '%d cases, fewer than %d' % (s['n'], min_cases), s
    if not s['mean'] > 0:
        return False, 'the mean %s markout %.4f a contract is not positive' % (key.split('_')[0][2:] + ' s net',
                                                                               s['mean']), s
    if s['t'] is None:
        return False, 'the game-clustered t is undefined (one game, or no spread)', s
    if s['t'] < min_t:
        return False, 'the game-clustered t %.2f is under %.1f' % (s['t'], min_t), s
    return True, '', s


def _median(xs):
    xs = sorted(x for x in xs if x is not None)
    if not xs:
        return None
    k = len(xs) // 2
    return xs[k] if len(xs) % 2 else (xs[k - 1] + xs[k]) / 2.0


def describe(cases, watched):
    """The report's numbers beside the verdict: frequency, durations, sizes, and the 300 s mean by
    side (the leader or the trailer), by route and by edge."""
    taken = [c for c in cases if c['state'] == 'taken']
    out = {'signals': len(cases), 'taken': len(taken), 'missed': len(cases) - len(taken), 'watched_games': watched,
           'per_game': round(len(cases) / watched, 3) if watched else None,
           'dur_s_median': _median([c['dur_s'] for c in cases]),
           'size_shown_median': _median([c['size_shown'] for c in cases]),
           'mo60_net': clustered(cases, 'mo60_net')['mean'], 'settle_net': clustered(cases, 'settle_net')['mean']}
    groups = {'leader': lambda c: c['lead'] > 0, 'trailer': lambda c: c['lead'] < 0, 'tied': lambda c: c['lead'] == 0,
              'buy': lambda c: c['side'] == 'BUY', 'sell': lambda c: c['side'] == 'SELL',
              'edge_20_30': lambda c: c['edge'] < 0.30, 'edge_30_40': lambda c: 0.30 <= c['edge'] < 0.40,
              'edge_40_up': lambda c: c['edge'] >= 0.40}
    for name, f in groups.items():
        s = clustered([c for c in cases if f(c)])
        out[name] = {'n': s['n'], 'mo300_net': s['mean']}
    return out


def brier(samples):
    """(model, mid) Brier scores over (p_model, p_mid, outcome) samples."""
    if not samples:
        return None, None
    n = len(samples)
    return (round(sum((p - y) ** 2 for p, _, y in samples) / n, 6),
            round(sum((m - y) ** 2 for _, m, y in samples) / n, 6))


def overlay_fills(db):
    """The Manual fills on KXNCAAWBGAME with a 60 s markout in an ops-research.sqlite (deskd's store):
    (fills, the contract-weighted mean 60 s markout net of each fill's fee, a contract)."""
    con = sqlite3.connect('file:%s?mode=ro' % db, uri=True)
    try:
        got = con.execute("SELECT f.qty, f.fee, m.mo FROM fills f JOIN markouts m ON m.fid = f.fid AND m.h = '60s' "
                          "WHERE f.own = 'MAN' AND f.tk LIKE 'KXNCAAWBGAME-%' AND f.qty > 0 AND m.mo IS NOT NULL"
                          ).fetchall()
    finally:
        con.close()
    q = sum(r[0] for r in got)
    return len(got), (round((sum(r[0] * r[2] for r in got) - sum(r[1] or 0 for r in got)) / q, 6) if q else None)


def wcbb_f(samples_by_game, ops_research, min_games=150, min_fills=20):
    """5(f)'s gate for KXNCAAWBGAME (G3): 150+ settled games recorded with the model's fair, the
    model's Brier at least as good as the Kalshi mid's on their minute samples, and Manual fills on
    the series (the overlay clicks) with a positive net 60 s markout, 20 at least."""
    games_n = sum(1 for s in samples_by_game.values() if s)
    flat = [x for s in samples_by_game.values() for x in s]
    bm, bmid = brier(flat)
    out = {'pass': False, 'why': '', 'settled_games': games_n, 'samples': len(flat), 'brier_model': bm,
           'brier_mid': bmid, 'overlay_fills': None, 'overlay_mo60_net': None}
    if ops_research:
        out['overlay_fills'], out['overlay_mo60_net'] = overlay_fills(ops_research)
    if games_n < min_games:
        out['why'] = '5(f): %d settled games with the fair, fewer than %d' % (games_n, min_games)
    elif not bm <= bmid:
        out['why'] = "5(f): the model's Brier %.4f is worse than the mid's %.4f" % (bm, bmid)
    elif not ops_research:
        out['why'] = '5(f): no --ops-research, so the overlay clicks are not measured'
    elif out['overlay_fills'] < min_fills:
        out['why'] = '5(f): %d Manual fills on the series, fewer than %d' % (out['overlay_fills'], min_fills)
    elif not out['overlay_mo60_net'] > 0:
        out['why'] = '5(f): the overlay fills\' net 60 s markout %.4f is not positive' % out['overlay_mo60_net']
    else:
        out['pass'] = True
    return out


# ---- the gate file (strat/lane_gate.h) ----------------------------------------------------------
_HEX = set('0123456789abcdef')


def lane_gate_check(obj, lane, model_sha256='', need=()):
    """strat/lane_gate.cpp parse_lane_gate's rules, for a gate file this tool wrote: (ok, why).
    A FAIL, an unknown stage, no params or a param the lane needs, or a model_sha256 other than the
    loaded one's (absent and '' both mean none) is refused, as deskd refuses it."""
    def text(j, k):
        v = j.get(k)
        if v is None:
            return ''
        if not isinstance(v, str):
            raise TypeError('%s is not a string' % k)
        return v

    def sha(j, k):
        s = text(j, k).lower()
        if s and not (len(s) == 64 and set(s) <= _HEX):
            return None
        return s
    if not isinstance(obj, dict):
        return False, 'not a JSON object'
    try:
        if text(obj, 'lane') != lane:
            return False, ("is lane %s's gate, not %s's" % (text(obj, 'lane'), lane) if text(obj, 'lane')
                           else 'names no lane')
        if text(obj, 'verdict') == 'FAIL':
            return False, 'verdict FAIL' + (': ' + text(obj, 'why') if text(obj, 'why') else '')
        if text(obj, 'verdict') != 'PASS':
            return False, 'verdict "%s" is neither PASS nor FAIL' % text(obj, 'verdict')
        if text(obj, 'stage') not in ('shadow', 'live'):
            return False, 'unknown stage "%s" (shadow or live)' % text(obj, 'stage')
        if not isinstance(obj.get('params'), dict):
            return False, 'no params object'
        for n in need:
            if obj['params'].get(n) is None:
                return False, 'params has no ' + n
        s = sha(obj, 'model_sha256')
        if s is None:
            return False, 'model_sha256 is not 64 hex digits'
        if s != model_sha256.lower():
            return False, 'model_sha256 is not the loaded model\'s'
        series = obj.get('series')
        if series is not None:
            if not isinstance(series, dict):
                return False, 'series is not an object'
            for name, r in series.items():
                if not isinstance(r, dict) or not isinstance(r.get('pass'), bool):
                    return False, 'series %s has no pass (true or false)' % name
                text(r, 'why'), text(r, 'level')
                if r.get('leader_only') is not None and not isinstance(r['leader_only'], bool):
                    raise TypeError('leader_only is not a boolean')
                if sha(r, 'model_sha256') is None:
                    return False, 'series %s: model_sha256 is not 64 hex digits' % name
        text(obj, 'computed'), text(obj, 'tool')
    except TypeError as e:
        return False, 'malformed: %s' % e
    return True, ''


def tradable_series(obj, loaded):
    """strat/lane_gate.cpp tradable_series: series -> '' (C1 may trade it) or why not, over the
    gate's series and the loaded models' ({series: sha256})."""
    series = obj.get('series') or {}
    out = {}
    for name in sorted(set(series) | set(loaded)):
        s, m = series.get(name), loaded.get(name)
        if obj.get('stage') != 'live':
            out[name] = 'the gate is at stage shadow'
        elif s is None:
            out[name] = 'the gate did not measure it'
        elif not s['pass']:
            out[name] = 'did not pass the gate' + (': ' + s['why'] if s.get('why') else '')
        elif m is None:
            out[name] = 'no model loaded for it'
        elif m.lower() != (s.get('model_sha256') or '').lower():
            out[name] = 'the gate measured another model'
        else:
            out[name] = ''
    return out


def iso_ms(ms):
    return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def gate_file(res, now_ms):
    """gates_dir/C1.json (strat/lane_gate.h): verdict PASS when any series passes, stage live; per
    series its pass and why, the score level it measured, leader_only and the model's sha256; no
    top-level model_sha256 (each series names its own); the numbers under evidence."""
    series = {s: {'pass': r['pass'], 'why': r['why'], 'level': r['level'], 'leader_only': res['params']['leader_only'],
                  'model': r['model'], 'model_sha256': r['model_sha256']}
              for s, r in res['series'].items()}
    passed = any(r['pass'] for r in series.values())
    why = None if passed else '; '.join('%s: %s' % (s, r['why']) for s, r in series.items()) or 'no series measured'
    return {'lane': 'C1', 'stage': 'live', 'verdict': 'PASS' if passed else 'FAIL', 'why': why,
            'computed': iso_ms(now_ms), 'tool': 'tools/college_gate.py', 'params': res['params'], 'series': series,
            'evidence': {'data_from': res['data_from'], 'data_to': res['data_to'], 'fee_mult': res['fee_mult'],
                         'series': {s: r['evidence'] for s, r in res['series'].items()}, 'wcbb_f': res['wcbb_f'],
                         'control': res['control']}}


def dumps_gate(obj):
    return json.dumps(obj, indent=2) + '\n'


def write_gate(path, obj, force=False):
    """Atomically (a .tmp and a rename), never over an existing file without force."""
    if os.path.exists(path) and not force:
        raise OSError('%s exists; --force replaces it' % path)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + '.tmp'
    with open(tmp, 'w') as fh:
        fh.write(dumps_gate(obj))
    os.replace(tmp, path)


# ---- the analysis -------------------------------------------------------------------------------
def analyse(data, series=SERIES, control=False, edge=2000, exec_lag_s=2, max_score_age_s=5, clip=2 * UNIT,
            game_cap=10_000_000, thin=20000 * CT, min_cases=50, min_t=2.0, leader_only=False, fee_mult=1.0,
            wcbb=None, volley=None, ops_research=None, wcbb_games=150, overlay_min=20):
    """Every series' cases and verdict, wcbb_f, and (control) the men's decile with Stern's fair.
    wcbb and volley: model file paths (None: that series has no model and does not pass)."""
    want = tuple(series) + (CONTROL_SERIES if control else ())
    gms = games(data, want)
    if control:   # the control: only the decile the recorder picked
        picked = {r.get('event_ticker') for r in rows(data, 'college_control') if r.get('picked')}
        gms = {ev: g for ev, g in gms.items() if g['series'] not in CONTROL_SERIES or ev in picked}
    scores, outages = score_rows(data)
    matched = match_games(gms, scores)
    hists, vols = book_history(data, [tk for g in gms.values() for tk in g['tickers']])
    models, model_why = {}, {}
    for s, kind, path in (('KXNCAAWBGAME', 'wcbb', wcbb), ('KXNCAAWVMATCH', 'volley', volley)):
        if path is None:
            model_why[s] = 'no model file given'
            continue
        if not os.path.exists(path):
            model_why[s] = 'no model file %s' % path
            continue
        models[s], model_why[s] = load_model(kind, path)
    models.update({s: STERN for s in CONTROL_SERIES})
    params = {'edge': edge / UNIT, 'exec_lag_s': exec_lag_s, 'max_score_age_s': max_score_age_s, 'clip': clip / UNIT,
              'game_cap': game_cap / 1e6, 'inventory': tr.params('COLLEGE')['inventory'] // CT, 'thin': thin // CT,
              'min_cases': min_cases, 'min_t': min_t, 'horizon': '300s', 'leader_only': leader_only}
    levels = {'KXNCAAWVMATCH': ('point', 'set')}
    t_lo = t_hi = None
    out = {'params': params, 'fee_mult': fee_mult, 'series': {}, 'control': None, 'wcbb_f': None,
           'survey': _survey(gms, matched, hists, outages)['series']}
    samples = {}
    for s in want:
        m = models.get(s)
        res = {}
        for level in levels.get(s, ('point',)):
            o = Opts(college_params(edge, clip, game_cap, thin, max_score_age_s * 1000, leader_only, level == 'point'),
                     exec_lag_s * S_US, fee_mult)
            cases, pulls, watched = [], {}, 0
            for ev, g in sorted(gms.items()):
                if g['series'] != s or ev not in matched or m is None:
                    continue
                srcs = {}
                for src, (key, a_home) in matched[ev].items():
                    lg = league_path(key[1]) if src == 'ESPN' else key[1]
                    outs = [(x, y) for x, y, l in outages.get(src, []) if l is None or l == lg]
                    srcs[src] = (score_states(scores[key], src), a_home, outs)
                sc = Score(srcs, max_score_age_s * S_US)
                r = replay_game(g, sc, hists, vols, m, o)
                cases.extend(r['cases'])
                for k, v in r['pulls'].items():
                    pulls[k] = pulls.get(k, 0) + v
                watched += r['watched']
                if level == 'point' and s == 'KXNCAAWBGAME':
                    samples[ev] = r['samples']
                for st in sc.src.values():
                    for x in st[1]:
                        t_lo = x if t_lo is None else min(t_lo, x)
                        t_hi = x if t_hi is None else max(t_hi, x)
            ok, why, num = verdict(cases, min_cases, min_t)
            res[level] = {'pass': ok, 'why': why, 'verdict': num, 'describe': describe(cases, watched),
                          'pulls': dict(sorted(pulls.items())), 'cases': cases}
        # the level that passed (point before set), else the first measured
        level = next((lv for lv in res if res[lv]['pass']), next(iter(res)))
        r = res[level]
        why = r['why'] if m is not None else model_why.get(s, 'no model')
        if m is not None and not r['pass'] and len(res) > 1:
            why = '; '.join('%s level: %s' % (lv, res[lv]['why']) for lv in res)
        entry = {'pass': bool(m is not None and r['pass']), 'why': why, 'level': level,
                 'model': m['name'] if m else None, 'model_sha256': m['sha256'] if m else None,
                 'evidence': {lv: dict(verdict=res[lv]['verdict'], **res[lv]['describe'], pulls=res[lv]['pulls'])
                              for lv in res}, 'cases': {lv: res[lv]['cases'] for lv in res}}
        if s in CONTROL_SERIES:
            out['control'] = dict(entry['evidence'], series=s, model='stern sigma 11 (report only)')
            continue
        out['series'][s] = entry
    if 'KXNCAAWBGAME' in out['series']:
        f = wcbb_f(samples, ops_research, wcbb_games, overlay_min)
        out['wcbb_f'] = f
        e = out['series']['KXNCAAWBGAME']
        if e['pass'] and not f['pass']:   # 5(f)'s gate folds into the series (F9: no field of its own)
            e['pass'], e['why'] = False, f['why']
    out['data_from'] = iso_ms(t_lo // 1000) if t_lo is not None else None
    out['data_to'] = iso_ms(t_hi // 1000) if t_hi is not None else None
    return out


def report(res):
    p = res['params']
    lines = ['college_gate: %s to %s UTC; edge %.2f net, $%.2f clips, $%.0f a game, %d contracts a market, scores '
             '%d s old at most, filled %g s after the decision, thin under %d contracts; fee multiplier %g ASSUMED '
             'where a market row gives none' % (res['data_from'] or '-', res['data_to'] or '-', p['edge'], p['clip'],
                                                p['game_cap'], p['inventory'], p['max_score_age_s'], p['exec_lag_s'],
                                                p['thin'], res['fee_mult']),
             '%-16s %6s %6s %6s %6s %8s %10s %7s' % ('series', 'events', 'games', 'espn', 'ncaa', 'books_ws',
                                                    'books_rest', 'settled')]
    for ser, s in sorted(res['survey'].items()):
        lines.append('%-16s %6d %6d %6d %6d %8d %10d %7d' % (ser, s['events'], s['games'], s['espn'], s['ncaa'],
                                                          s['books_ws'], s['books_rest'], s['settled']))

    def num(v, fmt='%+.4f'):
        return '-' if v is None else fmt % v
    rows_ = [(s, r['evidence'], r['level'], 'PASS' if r['pass'] else 'FAIL', r['why'])
             for s, r in res['series'].items()]
    if res['control']:
        c = res['control']
        rows_.append((c['series'] + ' (control)', {k: v for k, v in c.items() if k in ('point', 'set')}, 'point',
                      'report', c['model']))
    lines.append('%-24s %-5s %7s %6s %6s %7s %8s %8s %8s %6s  %s' % (
        'series', 'level', 'signals', 'taken', 'games', 'mo60', 'mo300', 'settle', 'se300', 't300', 'verdict'))
    for name, ev, level, verdict_, why in rows_:
        for lv, e in ev.items():
            v = e['verdict']
            lines.append('%-24s %-5s %7d %6d %6d %7s %8s %8s %8s %6s  %s' % (
                name, lv, e['signals'], e['taken'], v['games'], num(e['mo60_net']), num(v['mean']),
                num(e['settle_net']), num(v['se'], '%.4f'), num(v['t'], '%.2f'),
                (verdict_ + (': ' + why if why else '')) if lv == level else ''))
    f = res['wcbb_f']
    if f:
        why = ': ' + f['why'].replace('5(f): ', '', 1) if f['why'] else ''
        lines.append('5(f) KXNCAAWBGAME: %s%s; %d settled games, Brier model %s mid %s; overlay fills %s, '
                     'net 60 s %s' % ('PASS' if f['pass'] else 'FAIL', why, f['settled_games'],
                                      num(f['brier_model'], '%.4f'), num(f['brier_mid'], '%.4f'),
                                      num(f['overlay_fills'], '%d'), num(f['overlay_mo60_net'])))
    return '\n'.join(lines)


# ---- the summary --------------------------------------------------------------------------------
def survey(data, series):
    """What the readers found, per series: events, two-market events, matched by ESPN and NCAA,
    markets with a book (socket or REST), settled."""
    gms = games(data, series)
    scores, outages = score_rows(data)
    matched = match_games(gms, scores)
    hist, _ = book_history(data, [tk for g in gms.values() for tk in g['tickers']])
    return _survey(gms, matched, hist, outages)


def _survey(gms, matched, hist, outages):
    out = {}
    for g in gms.values():
        s = out.setdefault(g['series'], {'events': 0, 'games': 0, 'espn': 0, 'ncaa': 0, 'books_ws': 0,
                                         'books_rest': 0, 'settled': 0})
        s['events'] += 1
        s['games'] += len(g['tickers']) == 2
        s['espn'] += 'ESPN' in matched.get(g['event'], {})
        s['ncaa'] += 'NCAA' in matched.get(g['event'], {})
        for tk in g['tickers']:
            h = hist.get(tk)
            s['books_ws'] += h is not None and h.src == 'ws'
            s['books_rest'] += h is not None and h.src == 'rest'
        s['settled'] += all(g['markets'][tk]['result'] for tk in g['tickers'])
    return {'series': out, 'outages': {src: len(v) for src, v in outages.items()}}


def run(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--data', help="the recorder's data directory (required: no default)")
    ap.add_argument('--series', help='comma-separated series (default %s)' % ','.join(SERIES))
    ap.add_argument('--control', action='store_true', help="also the men's thin decile, Stern's fair (report only)")
    ap.add_argument('--edge-c', type=float, default=20.0,
                    help='the edge net of the fee, cents (at least 10; default 20)')
    ap.add_argument('--exec-lag-s', type=float, default=2.0,
                    help='the take fills the touch this long after (default 2)')
    ap.add_argument('--max-score-age-s', type=int, default=5, help="the score's last poll at most this old (default 5)")
    ap.add_argument('--clip', type=float, default=2.0, help="a take's dollars, fee included (default 2)")
    ap.add_argument('--game-cap', type=float, default=10.0,
                    help="the lane's worst case in a game, dollars (default 10)")
    ap.add_argument('--thin', type=int, default=20000, help='a thin game trades under this many contracts in-game')
    ap.add_argument('--min-cases', type=int, default=50)
    ap.add_argument('--min-t', type=float, default=2.0,
                    help="the game-clustered t at least (0: the plan's literal rule)")
    ap.add_argument('--leader-only', action='store_true', help='take only the team that leads')
    ap.add_argument('--fee-mult', type=float, default=1.0, help="the fee multiplier where a market row gives none")
    ap.add_argument('--wcbb', default=os.path.join(ROOT, 'models', 'wcbb_logit_v1.json'), help='the WCBB model file')
    ap.add_argument('--volley', default=os.path.join(ROOT, 'models', 'volley_markov_v1.json'),
                    help='the volleyball model file')
    ap.add_argument('--ops-research', metavar='FILE', help="deskd's ops-research.sqlite: the overlay fills of 5(f)")
    ap.add_argument('--wcbb-games', type=int, default=150, help="5(f)'s settled games at least")
    ap.add_argument('--overlay-fills', type=int, default=20, help="5(f)'s Manual fills at least")
    ap.add_argument('--json', action='store_true', help='everything, the cases too, as JSON')
    ap.add_argument('--write-gate', metavar='FILE', help="write deskd's gate file (gates_dir/C1.json)")
    ap.add_argument('--write-fixture', metavar='FILE', help="write the self-test season's gate (tests/deskd/fixtures/"
                    "college_gate_ok.json) and stop")
    ap.add_argument('--force', action='store_true', help='replace an existing --write-gate or --write-fixture file')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if a.write_fixture:
        with tempfile.TemporaryDirectory() as d:
            _, gate = season_gate(d)
        try:
            write_gate(a.write_fixture, gate, a.force)
        except OSError as e:
            print('error: %s' % e, file=sys.stderr)
            return 2
        print('wrote %s' % a.write_fixture)
        return 0
    if not a.data or not os.path.isdir(a.data):
        print("error: --data DIR (the recorder's data directory) is required", file=sys.stderr)
        return 2
    if a.edge_c < 10 or a.clip <= 0 or a.game_cap <= 0 or a.exec_lag_s < 0 or a.max_score_age_s < 1:
        print('error: --edge-c is 10 or more (deskd never trades under 10c); --clip, --game-cap and '
              '--max-score-age-s above 0', file=sys.stderr)
        return 2
    if a.write_gate and os.path.exists(a.write_gate) and not a.force:
        print('error: %s exists; --force replaces it' % a.write_gate, file=sys.stderr)
        return 2
    series = tuple(s.strip() for s in a.series.split(',') if s.strip()) if a.series else SERIES
    res = analyse(a.data, series, a.control, int(round(a.edge_c * 100)), a.exec_lag_s, a.max_score_age_s,
                  int(round(a.clip * UNIT)), int(round(a.game_cap * 1e6)), a.thin * CT, a.min_cases, a.min_t,
                  a.leader_only, a.fee_mult, a.wcbb, a.volley, a.ops_research, a.wcbb_games, a.overlay_fills)
    print(json.dumps(res, indent=1) if a.json else report(res))
    if a.write_gate:
        gate = gate_file(res, int(datetime.now(timezone.utc).timestamp() * 1000))
        write_gate(a.write_gate, gate, a.force)
        print('wrote %s: %s' % (a.write_gate, gate['verdict']), file=sys.stderr if a.json else sys.stdout)
    return 0


# ---- self-test ----------------------------------------------------------------------------------
T0 = int(datetime(2026, 11, 14, 23, 0, tzinfo=timezone.utc).timestamp()) * S_US   # 6 pm ET, Nov 14
WB_EV = 'KXNCAAWBGAME-26NOV14AAABBB'
WB_A, WB_B = WB_EV + '-AAA', WB_EV + '-BBB'
WB_SPEC = 'basketball/womens-college-basketball:50'


def rfc(t_us):
    return datetime.fromtimestamp(t_us / 1e6, tz=timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def espn_min(t_us):
    """ESPN's start time: minutes, no seconds."""
    return datetime.fromtimestamp(t_us / 1e6, tz=timezone.utc).strftime('%Y-%m-%dT%H:%MZ')


class Rec(lj.Spool):
    """lip_join's synthetic recorder data, with the college tables in the recorder's shapes."""

    def kmarket(self, t_us, tk, ev, name, close_us, result=None, status='active', **raw):
        raw = dict(raw, ticker=tk, event_ticker=ev, yes_sub_title=name, close_time=rfc(close_us), status=status,
                   result=result or '')
        self.write('kalshi_markets', {'recv_us': t_us, 'series': series_of(tk), 'ticker': tk, 'status': status,
                                      'open_time': rfc(close_us - 86400 * S_US), 'close_time': rfc(close_us),
                                      'result': result, 'raw': json.dumps(raw, separators=(',', ':'))})

    def daily(self, t_us, tk, ev, name, close_us, volume=0.0):
        raw = {'ticker': tk, 'event_ticker': ev, 'yes_sub_title': name, 'close_time': rfc(close_us)}
        self.write('kalshi_markets_daily', {'recv_us': t_us, 'sweep_us': t_us, 'ticker': tk, 'event_ticker': ev,
                                            'status': 'active', 'open_time': None, 'close_time': rfc(close_us),
                                            'volume': volume, 'open_interest': 0.0, 'yes_bid': None, 'yes_ask': None,
                                            'raw': json.dumps(raw, separators=(',', ':'))})

    def score(self, src, t_us, prev_us, league, gid, start_us, state, period, clock, home, away, hs, as_,
              names=None, lines=None):
        """An espn_scores or ncaa_scores row (recorder/scores.py espn_games / ncaa_games)."""
        if src == 'ESPN':
            comps = []
            for side, abbr, sc in (('home', home, hs), ('away', away, as_)):
                full = (names or {}).get(side, abbr)
                c = {'homeAway': side, 'score': str(sc),
                     'team': {'id': abbr, 'abbreviation': abbr, 'displayName': full}}
                c['linescores'] = list((lines or {}).get(side, []))
                comps.append(c)
            raw = {'status': {'period': period, 'displayClock': clock, 'type': {'state': state}}, 'competitors': comps}
            table = 'espn_scores'
        else:
            raw = {'gameState': {'pre': 'pre', 'in': 'live', 'post': 'final'}[state], 'currentPeriod': str(period),
                   'contestClock': clock, 'startTimeEpoch': start_us // S_US,
                   'home': {'names': {'char6': home, 'short': (names or {}).get('home', home)}, 'score': str(hs)},
                   'away': {'names': {'char6': away, 'short': (names or {}).get('away', away)}, 'score': str(as_)}}
            table = 'ncaa_scores'
        self.write(table, {'recv_us': t_us, 'prev_us': prev_us, 'league': league, 'game_id': gid,
                           'start_time': espn_min(start_us), 'state': state, 'status': state, 'period': str(period),
                           'clock': clock, 'home': home, 'away': away, 'home_score': hs, 'away_score': as_,
                           'detail': None, 'raw': json.dumps(raw, separators=(',', ':'))})

    def gap(self, t_us, source, kind, detail, from_us, to_us):
        self.write('gaps', {'recv_us': t_us, 'source': source, 'kind': kind, 'detail': detail, 'from_us': from_us,
                            'to_us': to_us})

    def rest_top(self, t_us, tk, bid, bid_sz, ask, ask_sz, volume=None):
        self.write('kalshi_rest_books', {'recv_us': t_us, 'sent_us': t_us - 50000, 'prev_us': None, 'polls': 1,
                                         'kind': 'top', 'mode': 'college', 'ticker': tk, 'yes_bid': bid,
                                         'yes_bid_size': bid_sz, 'yes_ask': ask, 'yes_ask_size': ask_sz,
                                         'last_price': None, 'volume': volume, 'raw': '{}'})

    def trade_id(self, t_us, conn, sid, seq, tk, yes_price, count, taker_side, tid):
        self.frame(t_us, conn, 'trade', sid, seq,
                   {'trade_id': tid, 'market_ticker': tk, 'yes_price_dollars': '%.4f' % yes_price,
                    'no_price_dollars': '%.4f' % (1 - yes_price), 'count_fp': '%.2f' % count, 'taker_side': taker_side,
                    'ts': t_us // S_US})

    def trade_rest(self, t_us, created_us, tk, count, tid):
        self.write('kalshi_trades_rest', {'recv_us': t_us, 'trade_id': tid, 'ticker': tk,
                                          'created_time': rfc(created_us), 'created_us': created_us,
                                          'yes_price': 0.5, 'no_price': 0.5, 'count': float(count),
                                          'taker_side': 'yes', 'reason': 'hour', 'raw': '{}'})


class Frames:
    """Book frames buffered and written in the order received, each game on its own subscriptions
    (books sid, prints sid + 1000), numbered 1, 2, ... as the venue does."""

    def __init__(self):
        self.f, self.n = [], 0

    def add(self, t, kind, sid, *args):
        self.n += 1
        self.f.append((t, self.n, kind, sid, args))

    def flush(self, sp):
        """Every frame in the order received, then one more a day after the last: the socket
        records on past the games (its last frame ends what the books are known to be)."""
        seq = {}
        for t, _, kind, sid, args in sorted(self.f):
            seq[sid] = seq.get(sid, 0) + 1
            getattr(sp, kind)(t, 1, sid, seq[sid], *args)
        if self.f:
            sp.frame(max(self.f)[0] + 86400 * S_US, 1, 'ok', 0, 1, {})
        self.f = []


class Game:
    """One synthetic two-market game: its markets, books (market A's moved level by level, never
    crossed), prints and score rows. A (home) is the first market by ticker."""

    def __init__(self, sp, fr, sid, series, code, tip, a_wins=True, league=WB_SPEC, b_bid=0.30):
        self.sp, self.fr, self.sid, self.tip = sp, fr, sid, tip
        self.ev = '%s-%sH%sV%s' % (series, _yymondd(tip), code, code)
        self.a, self.b = self.ev + '-H' + code, self.ev + '-V' + code
        self.code, self.league, self.a_wins = code, league, a_wins
        close = tip + 30 * 3600 * S_US
        for tk, name in ((self.a, 'Home ' + code), (self.b, 'Visitors ' + code)):
            sp.kmarket(tip - 7200 * S_US, tk, self.ev, name, close)
        res = ('yes', 'no') if a_wins else ('no', 'yes')
        for tk, name, r in ((self.a, 'Home ' + code, res[0]), (self.b, 'Visitors ' + code, res[1])):
            sp.kmarket(tip + 7200 * S_US, tk, self.ev, name, close, result=r, status='finalized')
        self.bids, self.asks = {0.40: 10}, {0.52: 10}   # A's YES levels: 0.40/0.52
        fr.add(tip - 600 * S_US, 'snapshot', sid, self.a, [(0.40, 10)], [(0.48, 10)])
        fr.add(tip - 600 * S_US, 'snapshot', sid, self.b, [(b_bid, 10)], [(0.48, 10)])   # B: 0.30/0.52

    def book(self, dt, bids, asks):
        """A's book from tip + dt, by deltas at one instant: the sizes taken off first, then the asks
        put on, then the bids, so no frame crosses it."""
        t = self.tip + int(dt * S_US)
        moves = []
        for side, old, new in (('yes', self.bids, bids), ('no', self.asks, asks)):
            for p in sorted(set(old) | set(new)):
                d = new.get(p, 0) - old.get(p, 0)
                if d:
                    moves.append((d > 0, side == 'yes', side, p if side == 'yes' else round(1 - p, 4), d))
        for _, _, side, p, d in sorted(moves):
            self.fr.add(t, 'delta', self.sid, self.a, side, p, d)
        self.bids, self.asks = dict(bids), dict(asks)

    def stale_ask(self, dt=100, px=0.45):
        """A cheaper ask on A from tip + dt, the 0.52 behind it."""
        self.book(dt, self.bids, {**self.asks, px: 10})

    def later(self, mid300=0.78):
        """After the take: 0.58/0.66 at +230 s, then mid300 +- 2c at +400 s."""
        self.book(230, {0.58: 10}, {0.66: 10})
        self.book(400, {round(mid300 - 0.02, 4): 10}, {round(mid300 + 0.02, 4): 10})

    def trade(self, dt, count):
        t = self.tip + int(dt * S_US)
        self.fr.add(t, 'trade_id', self.sid + 1000, self.a, 0.5, count, 'yes', 'tr-%s-%d' % (self.code, t))

    def score(self, src, dt, state, period, clock, home, away, lines=None, prev=None):
        t = self.tip + int(dt * S_US)
        league = self.league if src == 'ESPN' else NCAA_SPORTS[series_of(self.a)]
        names = {'home': 'Home %s Hawks' % self.code, 'away': 'Visitors %s Bees' % self.code} if src == 'ESPN' else \
            {'home': 'Home ' + self.code, 'away': 'Visitors ' + self.code}
        gid = ('4%s' if src == 'ESPN' else '6%s') % self.code
        self.sp.score(src, t, prev, league, gid, self.tip, state, period, clock, 'H' + self.code, 'V' + self.code,
                      home, away, names, lines)

    def basic(self, src='ESPN', lead_at=200, lead=6, final=True, lines=None):
        """pre an hour before, in at 0-0 a minute after the tip, A ahead by lead at lead_at, final."""
        self.score(src, -3600, 'pre', 0, '0:00', 0, 0)
        self.score(src, 60, 'in', 1, '10:00', 0, 0)
        if lead_at is not None:
            self.score(src, lead_at, 'in', 1, '7:20', lead, 0)
        if final:
            self.score(src, 7000, 'post', 4, '0:00', 70 if self.a_wins else 60, 60 if self.a_wins else 70)


def _yymondd(t_us):
    d = em.et_date(t_us // 1000)   # YYYY-MM-DD, ET
    return '%s%s%s' % (d[2:4], ('JAN', 'FEB', 'MAR', 'APR', 'MAY', 'JUN', 'JUL', 'AUG', 'SEP', 'OCT', 'NOV',
                                'DEC')[int(d[5:7]) - 1], d[8:10])


def _ops_db(path, fills):
    """An ops-research.sqlite with deskd's fills and markouts tables (store.cpp), these fills:
    (fid, tk, own, qty, fee, mo60 or None)."""
    con = sqlite3.connect(path)
    con.execute('CREATE TABLE fills(fid TEXT PRIMARY KEY, cid TEXT, oid TEXT, tk TEXT, side TEXT, qty REAL, px REAL, '
                'fee REAL, taker INTEGER, ts_ms INTEGER, recv_ms INTEGER, shard INTEGER, own TEXT, lane TEXT, '
                'json TEXT NOT NULL)')
    con.execute('CREATE TABLE markouts(fid TEXT NOT NULL, h TEXT NOT NULL, mo REAL, mid REAL, json TEXT NOT NULL, '
                'PRIMARY KEY(fid, h))')
    for fid, tk, own, qty, fee, mo in fills:
        con.execute("INSERT INTO fills(fid, tk, own, qty, fee, json) VALUES (?, ?, ?, ?, ?, '{}')",
                    (fid, tk, own, qty, fee))
        if mo is not None:
            con.execute("INSERT INTO markouts VALUES (?, '60s', ?, 0.5, '{}')", (fid, mo))
        con.execute("INSERT INTO markouts VALUES (?, '10s', 9.0, 0.5, '{}')", (fid,))   # another horizon: not read
    con.commit()
    con.close()


FIXTURE = os.path.join(ROOT, 'tests', 'deskd', 'fixtures', 'college_gate_ok.json')
FIXTURE_WCBB = os.path.join(ROOT, 'tests', 'deskd', 'fixtures', 'wcbb_synthetic.json')
FIXTURE_VOLLEY = os.path.join(ROOT, 'tests', 'deskd', 'fixtures', 'volleyball_golden.json')
FIXTURE_NOW_MS = 1795200000000   # 2026-11-20, after the season


def season(d):
    """A synthetic season both C1 series pass on, with the fixtures' models: 160 women's basketball
    games and 60 volleyball matches, one stale ask taken in each, most marking up, the favourite
    winning most; and an ops-research.sqlite with 25 overlay fills. Returns that sqlite's path."""
    sp, fr = Rec(d), Frames()
    rng = random.Random(5)
    for i in range(220):
        vb_ = i >= 160
        tip = T0 + i * 1800 * S_US
        g = Game(sp, fr, i + 1, 'KXNCAAWVMATCH' if vb_ else 'KXNCAAWBGAME', 'S%03d' % i, tip, a_wins=i % 7 != 3,
                 league='volleyball/womens-college-volleyball' if vb_ else WB_SPEC)
        g.stale_ask()
        if vb_:
            g.score('ESPN', -3600, 'pre', 0, '', 0, 0, {'home': [], 'away': []})
            g.score('ESPN', 60, 'in', 1, '', 0, 0, {'home': [0], 'away': [0]})
            g.score('ESPN', 200, 'in', 1, '', 0, 0, {'home': [20], 'away': [10]})
            a = g.a_wins
            g.score('ESPN', 7000, 'post', 4, '', 3 if a else 1, 1 if a else 3,
                    {'home': [25, 23, 25, 25] if a else [25, 23, 23, 20],
                     'away': [20, 25, 20, 18] if a else [20, 25, 25, 25]})
        else:
            g.basic(lead=rng.choice((10, 12, 15)))
        g.later(0.40 if i % 10 == 9 else round(0.64 + 0.01 * rng.randrange(16), 2))
    fr.flush(sp)
    db = os.path.join(d, 'ops-research.sqlite')
    _ops_db(db, [('m%d' % k, 'KXNCAAWBGAME-26NOV14HS000VS000-HS000', 'MAN', 2.0, 0.02, 0.03 + 0.01 * (k % 3))
                 for k in range(25)])
    return db


def season_gate(d):
    db = season(d)
    res = analyse(d, wcbb=FIXTURE_WCBB, volley=FIXTURE_VOLLEY, ops_research=db)
    return res, gate_file(res, FIXTURE_NOW_MS)


def self_test_part2(check):
    s = S_US
    model = load_model('wcbb', FIXTURE_WCBB)[0]
    volley = load_model('volley', FIXTURE_VOLLEY)[0]
    check('load_model: the fixtures load, named by the sha256 of their bytes',
          model is not None and volley is not None and model['sha256'] == hashlib.sha256(
              open(FIXTURE_WCBB, 'rb').read()).hexdigest() and volley['name'] == 'volley_markov')

    # ---- the score a decision reads
    def st(t, state, hs, as_, period=1, first=None):
        return {'t': t, 'state': state, 'period': period, 'clock_s': 440.0, 'home_score': hs, 'away_score': as_,
                'home_lines': [], 'away_lines': [], 'start_ms': 0, 'first_seen': first if first is not None else t}
    sc = Score({'ESPN': ([st(100, 'in', 0, 0), st(200, 'in', 10, 0)], True, [(150 * s, 400 * s)]),
                'NCAA': ([st(110, 'in', 0, 0), st(260, 'in', 0, 10)], False, [])}, 5 * s)
    for x in sc.src.values():   # times in seconds above, microseconds below
        for y in x[0]:
            y['t'] *= s
            y['first_seen'] *= s
        x[1][:] = [y['t'] for y in x[0]]
    r = sc.at(120 * s)
    check('Score: both fresh and agreeing, the earlier first sighting', (r['state'], r.get('src'), r.get('first_seen'),
          r['a_score']) == ('in', 'ESPN', 100 * s, 0), r)
    r = sc.at(210 * s)
    check("Score: a source in its outage was last polled at the outage's start: the other, fresh, is used",
          (r.get('src'), r.get('fetched'), r.get('first_seen')) == ('NCAA', 210 * s, 110 * s)
          and sc.latest('ESPN', 210 * s)[1] == 200 * s, r)
    r = sc.at(152 * s)
    check('Score: within the max age of the outage start ESPN is still fresh, and both agree', r.get('src') == 'ESPN'
          and r.get('fetched') == 150 * s, r)
    sc2 = Score({'ESPN': ([st(200 * s, 'in', 10, 0)], True, []), 'NCAA': ([st(110 * s, 'in', 0, 0)], True, [])}, 5 * s)
    r = sc2.at(205 * s)
    check('Score: both fresh and different: no score, the why names both', r['state'] == ''
          and r['why'] == 'ESPN has in 10-0 (period 1) and NCAA in 0-0 (period 1)', r)
    sc3 = Score({'ESPN': ([st(200 * s, 'in', 10, 0)], True, [(201 * s, 900 * s)])}, 5 * s)
    r = sc3.at(300 * s)
    check('Score: one source, old: the age pulls it (SCORE_OLD)', r['fetched'] == 201 * s and r['state'] == 'in', r)
    rs = [{'recv_us': 1, 'state': 'in', 'period': '1', 'clock': '9:00', 'home_score': 0, 'away_score': 0, 'raw': '{}'},
          {'recv_us': 2, 'state': 'in', 'period': '1', 'clock': '8:30', 'home_score': 0, 'away_score': 0, 'raw': '{}'},
          {'recv_us': 3, 'state': 'in', 'period': '1', 'clock': '8:00', 'home_score': 2, 'away_score': 0, 'raw': '{}'}]
    ss = score_states(rs, 'ESPN')
    check("score_states: a clock row keeps the state's first sighting, a score starts a new one",
          [x['first_seen'] for x in ss] == [1, 1, 3] and ss[1]['clock_s'] == 510, ss)
    check('period_of: the leading digits', (period_of('2nd'), period_of('1'), period_of('HALF'), period_of(None))
          == (2, 1, 0, 0))

    # ---- volleyball's state from the linescores (volley_model.cpp's rules)
    vs = volley_state(2, ([25, 3], [20, 1]), (1, 0))
    check('volley_state: a set won, the next one being played', vs == ({'sets_yes': 1, 'sets_opp': 0, 'pts_yes': 3,
                                                                        'pts_opp': 1}, True), vs)
    check('volley_state: a set just won counts, the next at 0-0; unread points; bad lines',
          volley_state(1, ([25], [23]), (1, 0)) == ({'sets_yes': 1, 'sets_opp': 0, 'pts_yes': 0, 'pts_opp': 0}, True)
          and volley_state(2, ([25], [20]), (1, 0))
          == ({'sets_yes': 1, 'sets_opp': 0, 'pts_yes': 0, 'pts_opp': 0}, False)
          and volley_state(2, ([25, 3], [24, 1]), (1, 0))[0] is None
          and volley_state(1, ([27], [24]), (0, 0))[0] is None
          and volley_state(2, ([25, 3], [20, 1]), (0, 0))[1] == 'the linescores give 1-0 in sets, the score 0-0')
    ref = {'state': 'in', 'a_home': False, 'home_score': 10, 'away_score': 0, 'period': 1, 'clock_s': 440.0}
    f, _, _ = fair_of(model, ref, 0.7)
    check("fair_of: the home team's P from the home side (its p0 1 - A's), 1 - it for an away market A",
          abs(f - (1 - tb.predict(model['obj'], 2240, 10, 0, 0.3, False))) < 1e-15)
    check('fair_of: no p0, not in play, no clock', fair_of(model, ref, None)[2].startswith('no pregame price')
          and fair_of(model, dict(ref, state='pre'), 0.5)[2] == 'not started'
          and fair_of(model, dict(ref, clock_s=-1.0), 0.5)[2] == 'no game clock in period 1')
    check('to_px: rounds half away from zero, as llround',
          (to_px(0.80205), to_px(0.802049), to_px(1.2)) == (8021, 8020, UNIT))

    # ---- the clustered t, by hand: three games
    #   g1: 2 contracts at +0.10 and 2 at +0.20 -> Y 0.6, N 4;  g2: 4 at -0.05 -> Y -0.2, N 4;
    #   g3: 2 at +0.30 -> Y 0.6, N 2.  mean = 1.0 / 10 = 0.10; residuals Y - 0.1 N: 0.2, -0.6, 0.4;
    #   var = 3/2 x (0.04 + 0.36 + 0.16) / 10^2 = 0.0084; se = 0.0916515; t = 1.0911
    cs = [{'event': e, 'state': 'taken', 'filled': n, 'mo300_net': m} for e, n, m in
          (('g1', 2, 0.10), ('g1', 2, 0.20), ('g2', 4, -0.05), ('g3', 2, 0.30))]
    cs.append({'event': 'g4', 'state': 'missed', 'filled': 0, 'mo300_net': None})
    cs.append({'event': 'g5', 'state': 'taken', 'filled': 3, 'mo300_net': None})   # not marked
    c = clustered(cs)
    check('clustered: the contract-weighted mean and its game-clustered se and t, by hand',
          (c['n'], c['games'], c['contracts'], c['mean'], c['se'], c['t']) == (4, 3, 10.0, 0.1, 0.091652, 1.091), c)
    check('verdict: the thresholds, in order', verdict(cs, 5)[1] == '4 cases, fewer than 5'
          and verdict(cs, 4, 1.0)[0] and verdict(cs, 4, 1.1)[1] == 'the game-clustered t 1.09 is under 1.1'
          and verdict([dict(x, mo300_net=-0.1) for x in cs[:4]], 4)[1].startswith('the mean 300 s net markout -0.1000')
          and verdict(cs[:2], 2, 1.0)[1] == 'the game-clustered t is undefined (one game, or no spread)')
    check('brier: by hand', brier([(0.8, 0.5, 1.0), (0.4, 0.5, 0.0)]) == (0.1, 0.25))
    good = {'g%d' % i: [(0.8, 0.5, 1.0)] for i in range(150)}
    with tempfile.TemporaryDirectory() as d:
        db, neg = os.path.join(d, 'ok.sqlite'), os.path.join(d, 'neg.sqlite')
        _ops_db(db, [('m%d' % k, 'KXNCAAWBGAME-26NOV14X-A', 'MAN', 1.0, 0.01, 0.02) for k in range(20)])
        _ops_db(neg, [('m%d' % k, 'KXNCAAWBGAME-26NOV14X-A', 'MAN', 1.0, 0.02, 0.02) for k in range(20)])
        whys = [wcbb_f(good, db)['why'], wcbb_f(dict(good, g0=[]), db)['why'],
                wcbb_f({k: [(0.2, 0.5, 1.0)] for k in good}, db)['why'], wcbb_f(good, None)['why'],
                wcbb_f(good, db, min_fills=21)['why'], wcbb_f(good, neg)['why']]
    check("wcbb_f: 150 games, the model's Brier at least the mid's, 20 overlay fills with a positive net, each "
          "in turn", whys == ['', '5(f): 149 settled games with the fair, fewer than 150',
                              "5(f): the model's Brier 0.6400 is worse than the mid's 0.2500",
                              '5(f): no --ops-research, so the overlay clicks are not measured',
                              '5(f): 20 Manual fills on the series, fewer than 21',
                              "5(f): the overlay fills' net 60 s markout 0.0000 is not positive"], whys)

    # ---- the games, each a rule
    with tempfile.TemporaryDirectory() as d:
        sp, fr = Rec(d), Frames()
        G = {}

        def game(code, i, series='KXNCAAWBGAME', **kw):
            G[code] = Game(sp, fr, i, series, code, T0 + i * 1800 * s, **kw)
            return G[code]
        # G1: the scoring run after an older ask: taken, hand-worked below
        g = game('GA', 1)
        g.stale_ask(); g.basic(); g.later()
        # G2: the ask leaves 1 s after the decision, before the 2 s execution: missed
        g = game('GB', 2)
        g.stale_ask(); g.basic(); g.book(201, {0.40: 10}, {0.52: 10})
        # G3: the cheap ask comes 1 s after the score: not stale, no case
        g = game('GC', 3)
        g.stale_ask(201); g.basic()
        # G4: ESPN down from +150 s to +400 s, the ask gone at +300 s, no NCAA: no case
        g = game('GD', 4)
        g.stale_ask(); g.basic(lead_at=None); g.book(300, {0.48: 10}, {0.66: 10})
        g.score('ESPN', 400, 'in', 1, '7:20', 6, 0)
        sp.gap(T0 + (4 * 1800 + 150) * s, 'espn', 'poll_failed', WB_SPEC + ': timeout', T0 + (4 * 1800 + 150) * s,
               T0 + (4 * 1800 + 150) * s)
        sp.gap(T0 + (4 * 1800 + 400) * s, 'espn', 'recovered', 'down 250 s', T0 + (4 * 1800 + 150) * s,
               T0 + (4 * 1800 + 400) * s)
        # G5: the same with NCAA up: NCAA's 10-0 at +250 s is the score, taken
        g = game('GE', 5)
        g.stale_ask(); g.basic(lead_at=None); g.book(300, {0.48: 10}, {0.66: 10})
        g.score('ESPN', 400, 'in', 1, '7:20', 6, 0)
        g.basic('NCAA', lead_at=250, final=False)
        sp.gap(T0 + (5 * 1800 + 150) * s, 'espn', 'poll_failed', WB_SPEC + ': timeout', T0 + (5 * 1800 + 150) * s,
               T0 + (5 * 1800 + 150) * s)
        sp.gap(T0 + (5 * 1800 + 400) * s, 'espn', 'recovered', 'down 250 s', T0 + (5 * 1800 + 150) * s,
               T0 + (5 * 1800 + 400) * s)
        # G6: ESPN 10-0 at +200 s while NCAA still says 0-0: no score until NCAA agrees at +206 s
        g = game('GF', 6)
        g.stale_ask(); g.basic(); g.later()
        g.basic('NCAA', lead_at=206, final=False)
        # G7: 5,000 traded before the tip and 15,000 after: thin (under 20,000 in-game), taken
        g = game('GG', 7)
        g.stale_ask(); g.basic(); g.later(); g.trade(-300, 5000); g.trade(150, 15000)
        # G8: 20,000 traded in-game: not thin, no case
        g = game('GH', 8)
        g.stale_ask(); g.basic(); g.later(); g.trade(150, 20000)
        # G9: 8-0 at +210 s is a new score state, so a new signal run: the stale ask taken again
        g = game('GI', 13)
        g.stale_ask(); g.basic(); g.score('ESPN', 210, 'in', 1, '7:00', 8, 0); g.later()
        # G10: a run ends 10 s after its edge was last seen, with no new score state: up 3 with 5:00
        # left (fair 0.7546) the 0.45 ask is taken; it leaves at +205 s for 0.60 (no edge: 0.135),
        # so the run ends at +210 s; a clock row at +230 s (1:00 left, the same state: fair 0.9025)
        # gives SELL B at B's older bid 0.35 an edge (3 for 1.95 and a 5c fee: 0.9025 - 0.65 -
        # 0.0167 = 0.2358), taken because the run that took A's team is over
        g = game('GJ', 14, b_bid=0.35)
        g.stale_ask()
        g.score('ESPN', -3600, 'pre', 0, '0:00', 0, 0)
        g.score('ESPN', 60, 'in', 1, '10:00', 0, 0)
        g.score('ESPN', 200, 'in', 4, '5:00', 3, 0)
        g.book(205, {0.40: 10}, {0.60: 10})
        g.score('ESPN', 230, 'in', 4, '1:00', 3, 0)
        g.score('ESPN', 7000, 'post', 4, '0:00', 63, 60)
        # V1: volleyball, 20-10 in the first set: a case at point level, none at set level
        g = game('VA', 9, 'KXNCAAWVMATCH', league='volleyball/womens-college-volleyball')
        g.stale_ask()
        g.score('ESPN', -3600, 'pre', 0, '', 0, 0, {'home': [], 'away': []})
        g.score('ESPN', 60, 'in', 1, '', 0, 0, {'home': [0], 'away': [0]})
        g.score('ESPN', 200, 'in', 1, '', 0, 0, {'home': [20], 'away': [10]})
        g.score('ESPN', 7000, 'post', 4, '', 3, 1, {'home': [25, 23, 25, 25], 'away': [20, 25, 20, 18]})
        g.later()
        # V2: the first set won 25-20, seen at +1500 s: a case at both levels
        g = game('VB', 10, 'KXNCAAWVMATCH', league='volleyball/womens-college-volleyball')
        g.stale_ask()
        g.score('ESPN', -3600, 'pre', 0, '', 0, 0, {'home': [], 'away': []})
        g.score('ESPN', 60, 'in', 1, '', 0, 0, {'home': [0], 'away': [0]})
        g.score('ESPN', 1500, 'in', 2, '', 1, 0, {'home': [25, 0], 'away': [20, 0]})
        g.score('ESPN', 7000, 'post', 4, '', 3, 1, {'home': [25, 23, 25, 25], 'away': [20, 25, 20, 18]})
        g.book(1530, {0.58: 10}, {0.66: 10})
        # M1, M2: the men's control, M1 picked into the decile, M2 not
        for code, i, picked in (('MA', 11, True), ('MB', 12, False)):
            g = game(code, i, 'KXNCAAMBGAME', league='basketball/mens-college-basketball:50')
            g.stale_ask()
            g.score('ESPN', -3600, 'pre', 0, '0:00', 0, 0)
            g.score('ESPN', 60, 'in', 1, '20:00', 0, 0)
            g.score('ESPN', 200, 'in', 1, '16:00', 10, 0)
            g.score('ESPN', 7000, 'post', 2, '0:00', 70, 60)
            g.later()
            sp.write('college_control', {'recv_us': T0, 'series': 'KXNCAAMBGAME', 'game_date': '2026-11-14',
                                         'event_ticker': g.ev, 'volume': 10.0, 'rank': 1, 'events': 2,
                                         'picked': picked})
        fr.flush(sp)
        db = os.path.join(d, 'ops.sqlite')
        _ops_db(db, [('f1', 'KXNCAAWBGAME-26NOV14X-A', 'MAN', 2.0, 0.02, 0.05),
                     ('f2', 'KXNCAAWBGAME-26NOV14X-A', 'MAN', 3.0, 0.03, -0.01),
                     ('f3', 'KXNCAAWBGAME-26NOV14X-A', 'SYS', 5.0, 0.05, 0.50),     # Systematic's: not an overlay click
                     ('f4', 'KXNCAAWVMATCH-26NOV14X-A', 'MAN', 5.0, 0.05, 0.50),    # another series
                     ('f5', 'KXNCAAWBGAME-26NOV14X-A', 'MAN', 5.0, 0.05, None)])    # no 60 s markout
        check('overlay_fills: Manual fills on the series with a 60 s markout, net of their fees, by hand '
              '((2 x 0.05 - 0.02 + 3 x -0.01 - 0.03) / 5 = 0.004)', overlay_fills(db) == (2, 0.004), overlay_fills(db))

        res = analyse(d, control=True, wcbb=FIXTURE_WCBB, volley=FIXTURE_VOLLEY, ops_research=db, min_cases=2,
                      min_t=0.0)
        wb = res['series']['KXNCAAWBGAME']
        by = {c['event']: c for c in wb['cases']['point']}

        def case(code, cases=by):
            return cases.get(G[code].ev) or {}
        c1 = case('GA')
        fair = tb.predict(model['obj'], 2240, 6, 0, 0.5, False)
        check('G1: BUY A at the older ask 0.45 x 4 for a 7c fee, at the score, filled 2 s later',
              c1
              and (c1['side'], c1['price'], c1['count'], c1['fee'], c1['state'], c1['filled'], c1['ts_ms'])
              == ('BUY', 0.45, 4.0, 0.07, 'taken', 4.0, (T0 + (1800 + 200) * s) // 1000), c1)
        # the fair: the model at 7:20 of the first quarter (2,240 s left), up 6, p0 0.50 (the effective
        # pregame mid of A's 0.40/0.52 and B's 0.30/0.52: 0.48/0.52); edge (fair - 0.45) - 0.07 / 4.
        # A's 0.52 behind it had no edge: 3 for 1.56 and a 6c fee, 0.712 - 0.52 - 0.02 = 0.172
        check('G1: the fair and the edge (the model, p0 0.50 captured before the tip)',
              (c1.get('fair'), c1.get('edge')) == (to_px(fair) / UNIT, (to_px(fair) - 4500 - 175) / UNIT)
              and c1.get('fair') == 0.712, c1)
        # by hand: +60 s the book is 0.58/0.66, mid 0.62: 0.62 - 0.45 - 0.0175 = 0.1525; +300 s it is
        # 0.76/0.80, mid 0.78: 0.3125; A won: 1 - 0.45 - 0.0175 = 0.5325. The ask stood 30 s.
        check('G1: net markouts at the order\'s fee, by hand: 60 s 0.1525, 300 s 0.3125, settled 0.5325; 30 s long',
              tuple(c1.get(k) for k in ('mo60_net', 'mo300_net', 'settle_net', 'dur_s', 'touch_age_s', 'size_shown',
                                        'lead', 'score_src')) == (0.1525, 0.3125, 0.5325, 30.0, 100.0, 10.0, 6, 'ESPN'),
              c1)
        c2 = case('GB')
        check('G2: an ask gone before the execution lag is missed: no fill, no markouts',
              c2 and (c2['state'], c2['filled'], c2['mo300_net'], c2['dur_s'])
              == ('missed', 0.0, None, 1.0), c2)
        check('G3: an ask posted after the score is no case (counted as a TOUCH_AFTER_SCORE pull)',
              not case('GC') and wb['evidence']['point']['pulls'].get('TOUCH_AFTER_SCORE', 0) >= 1,
              wb['evidence']['point']['pulls'])
        check('G4: ESPN down through the score with no NCAA: no case', not case('GD'))
        c5 = case('GE')
        check("G5: NCAA's score while ESPN is down: taken at +250 s; 60 and 300 s at 0.48/0.66 (mid 0.57): 0.1025",
              c5
              and (c5['score_src'], c5['ts_ms'], c5['state'], c5['mo60_net'], c5['mo300_net'], c5['dur_s'])
              == ('NCAA', (T0 + (5 * 1800 + 250) * s) // 1000, 'taken', 0.1025, 0.1025, 50.0), c5)
        c6 = case('GF')
        check('G6: no score while ESPN and NCAA disagree; taken once they agree, on the earlier sighting',
              c6 and (c6['ts_ms'], c6['state'], c6['touch_age_s'])
              == ((T0 + (6 * 1800 + 206) * s) // 1000, 'taken', 106.0), c6)
        c7 = case('GG')
        check('G7: the volume before the tip does not count: 15,000 in-game is thin, taken',
              c7 and c7['volume'] == 15000.0 and c7['state'] == 'taken', c7)
        check('G8: 20,000 in-game is not a thin game: no case', not case('GH'))
        gi = [c for c in wb['cases']['point'] if c['event'] == G['GI'].ev]
        check('G9: a new score state starts a new run: the same stale ask taken again 10 s later, 4 each time',
              [(c['ts_ms'] - G['GI'].tip // 1000, c['count'], c['state']) for c in gi]
              == [(200000, 4.0, 'taken'), (210000, 4.0, 'taken')], gi)
        got = sorted(c['event'] for c in wb['cases']['point'])
        check('one case per game and run: G1, G2, G5, G6, G7, and G9 and G10 twice',
              got == sorted(G[k].ev for k in ('GA', 'GB', 'GE', 'GF', 'GG', 'GI', 'GI', 'GJ', 'GJ')), got)
        gj = [(c['ts_ms'] - G['GJ'].tip // 1000, c['side'], c['price'], c['count'], c['edge'], c['dir'], c['state'])
              for c in wb['cases']['point'] if c['event'] == G['GJ'].ev]
        check('G10: the run over 10 s after its edge left, a clock row later prices SELL B at its older bid',
              gj == [(200000, 'BUY', 0.45, 4.0, 0.2871, 'A', 'taken'),
                     (230000, 'SELL', 0.35, 3.0, 0.2358, 'A', 'taken')], gj)
        # the game's worst case counts each fill's cost and fee: under a $3 cap the second take has
        # $3 - $1.87 = $1.13 of room, 2 contracts (0.90 and a 4c fee; 3 would cost 1.41)
        capped = analyse(d, series=('KXNCAAWBGAME',), wcbb=FIXTURE_WCBB, game_cap=3_000_000, min_cases=1)
        gi = [c for c in capped['series']['KXNCAAWBGAME']['cases']['point'] if c['event'] == G['GI'].ev]
        check("G9 under a $3 game cap: the second take is what the game's room buys",
              [c['count'] for c in gi] == [4.0, 2.0], gi)
        vbr = res['series']['KXNCAAWVMATCH']
        vp = {c['event']: c for c in vbr['cases']['point']}
        vs_ = {c['event']: c for c in vbr['cases']['set']}
        va, vb2 = vp.get(G['VA'].ev), vp.get(G['VB'].ev)
        q_fair = vb.live_p({'sets_yes': 0, 'sets_opp': 0, 'pts_yes': 20, 'pts_opp': 10}, 0.5)
        check('V1: volleyball 20-10 in the first set, the Markov fair (0.6839): taken at point level, edge 0.2164',
              va is not None and va['fair'] == to_px(q_fair) / UNIT == 0.6839 and va['edge'] == 0.2164
              and va['mo300_net'] == 0.3125, va)
        check('V2: a set won is a case at both levels; V1 none at set level (no set had ended)',
              vb2 is not None and vb2['fair'] == 0.6875 and G['VB'].ev in vs_ and G['VA'].ev not in vs_, sorted(vs_))
        check("the volleyball series passes at point level, named by its model's sha256",
              vbr['level'] == 'point' and vbr['pass'] and vbr['model_sha256'] == volley['sha256'], vbr['why'])
        ctl = res['control']
        check('the control: Stern on the picked decile game only, reported and never a gate series',
              ctl is not None and ctl['point']['taken'] == 1 and set(res['series']) == set(SERIES)
              and 'KXNCAAMBGAME' not in gate_file(res, FIXTURE_NOW_MS)['series'], ctl and ctl['point']['taken'])
        f5 = res['wcbb_f']
        check('5(f): with 10 settled games it fails, and that fails basketball whatever its cases',
              not f5['pass'] and f5['why'] == '5(f): 10 settled games with the fair, fewer than 150'
              and f5['overlay_fills'] == 2 and not wb['pass'] and wb['why'] == f5['why'], f5)
        gate = gate_file(res, FIXTURE_NOW_MS)
        ok, why = lane_gate_check(gate, 'C1', '', ['edge'])
        check("the gate file: deskd's loader rules; a PASS on volleyball, basketball shut by 5(f)",
              ok and gate['verdict'] == 'PASS' and tradable_series(gate, {'KXNCAAWVMATCH': volley['sha256'],
                                                                           'KXNCAAWBGAME': model['sha256']})
              == {'KXNCAAWBGAME': 'did not pass the gate: ' + f5['why'], 'KXNCAAWVMATCH': ''}, why)
        out = os.path.join(d, 'gates', 'C1.json')
        args = ['--data', d, '--wcbb', FIXTURE_WCBB, '--volley', FIXTURE_VOLLEY, '--min-cases', '2', '--min-t', '0',
                '--write-gate', out]
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            rc1, rc2, rc3 = run(args), run(args), run(args + ['--force'])
        check('--write-gate writes the file, refuses to replace it without --force, replaces it with',
              (rc1, rc2, rc3) == (0, 2, 0) and lane_gate_check(json.load(open(out)), 'C1', '', ['edge'])[0])
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            check('--edge-c under 10 is refused (deskd never trades under 10c)',
                  run(['--data', d, '--edge-c', '9']) == 2)
            check('the report runs, and --json',
                  run(['--data', d, '--control']) == 0 and run(['--data', d, '--json']) == 0)

    # ---- the replay's own fills come out of the touch, and gw's inventory, C1's 20 contracts a market,
    # caps the position they build: an ask older than the score and a new score state every 10 s.
    # $2 buys 6 at 0.30 (1.80 and a 9c fee).
    #   IA  10 at 0.30: 6, then the 4 left (1.20 and a 6c fee: 0.0588), then none: the touch is empty
    #       and the 0.52 behind it is not known (Hist keeps the top), so no case
    #   IB  30 at 0.30: three takes hold 18 of A, so the fourth is the 2 that fit (college_take_cases.json's
    #       fourth_take_of_a_30c_team: fee 3c, edge fair - 0.30 - 0.015), with $4.33 of the game's $10
    #       left (13 contracts) and 12 at the touch; the fifth run finds no room (INVENTORY: no case)
    #   IC  4 at 0.45 with 0.48 behind it: taken once, 4 (deskd's IOC empties the level), not on each
    #       of the five score changes
    with tempfile.TemporaryDirectory() as d:
        sp, fr = Rec(d), Frames()
        gs = {}
        for i, (code, asks) in enumerate((('IA', {0.30: 10, 0.52: 10}), ('IB', {0.30: 30, 0.52: 10}),
                                          ('IC', {0.45: 4, 0.48: 10}))):
            g = gs[code] = Game(sp, fr, i + 1, 'KXNCAAWBGAME', code, T0 + i * 1800 * s)
            g.book(100, {0.25: 10}, asks)
            g.score('ESPN', -3600, 'pre', 0, '0:00', 0, 0)
            g.score('ESPN', 60, 'in', 1, '10:00', 0, 0)
            for k, clock in enumerate(('7:20', '7:10', '7:00', '6:50', '6:40')):
                g.score('ESPN', 200 + 10 * k, 'in', 1, clock, 6 + 2 * k, 0)
            g.score('ESPN', 7000, 'post', 4, '0:00', 70, 60)
        fr.flush(sp)
        res = analyse(d, series=('KXNCAAWBGAME',), wcbb=FIXTURE_WCBB, min_cases=1)
        cs = res['series']['KXNCAAWBGAME']['cases']['point']

        def took(code):
            return [(c['ts_ms'] - gs[code].tip // 1000, c['side'], c['price'], c['count'], c['fee'], c['size_shown'],
                     c['state']) for c in cs if c['event'] == gs[code].ev]
        check("the touch: a 10-lot at 0.30 gives 6, then the 4 left, then nothing", took('IA')
              == [(200000, 'BUY', 0.3, 6.0, 0.09, 10.0, 'taken'), (210000, 'BUY', 0.3, 4.0, 0.06, 4.0, 'taken')],
              took('IA'))
        check('the touch: a 4-lot at 0.45 is taken once in five score states', took('IC')
              == [(200000, 'BUY', 0.45, 4.0, 0.07, 4.0, 'taken')], took('IC'))
        ib = [c for c in cs if c['event'] == gs['IB'].ev]
        check("inventory: the fourth take of A at 0.30 is the 2 of C1's 20 left, the fifth run none", took('IB')
              == [(200000, 'BUY', 0.3, 6.0, 0.09, 30.0, 'taken'), (210000, 'BUY', 0.3, 6.0, 0.09, 24.0, 'taken'),
                  (220000, 'BUY', 0.3, 6.0, 0.09, 18.0, 'taken'), (230000, 'BUY', 0.3, 2.0, 0.03, 12.0, 'taken')]
              and res['params']['inventory'] == 20 and len(ib) == 4
              and abs(ib[3]['edge'] - (ib[3]['fair'] - 0.315)) < 1e-9, took('IB'))

    # ---- deskd's loader rules, as lane_gate.cpp has them
    base = {'lane': 'C1', 'stage': 'live', 'verdict': 'PASS', 'params': {'edge': 0.2}, 'series': {}}
    bad = [(dict(base, lane='X1'), "is lane X1's gate, not C1's"),
           (dict(base, verdict='FAIL', why='w'), 'verdict FAIL: w'),
           (dict(base, stage='paper'), 'unknown stage "paper" (shadow or live)'),
           (dict(base, params={}), 'params has no edge'),
           (dict(base, model_sha256='ab'), 'model_sha256 is not 64 hex digits'),
           (dict(base, model_sha256='a' * 64), "model_sha256 is not the loaded model's"),
           (dict(base, series={'S': {'why': 'x'}}), 'series S has no pass (true or false)'),
           (dict(base, series={'S': {'pass': True, 'model_sha256': 'zz'}}),
            'series S: model_sha256 is not 64 hex digits'),
           (dict(base, tool=3), 'malformed: tool is not a string')]
    got = [lane_gate_check(o, 'C1', '', ['edge']) for o, _ in bad]
    check("lane_gate_check: lane_gate.cpp's refusals", all(g == (False, w) for g, (_, w) in zip(got, bad)),
          [g for g, (_, w) in zip(got, bad) if g != (False, w)])

    # ---- the fixture deskd's C++ loads: what the tool writes on the synthetic season
    with tempfile.TemporaryDirectory() as d:
        res, gate = season_gate(d)
    ok, why = lane_gate_check(gate, 'C1', '', ['edge'])
    t = tradable_series(gate, {'KXNCAAWBGAME': model['sha256'], 'KXNCAAWVMATCH': volley['sha256']})
    check('the season: both series pass (50+ cases, a positive 300 s mean, t of 2 or more; 5(f) for basketball)',
          ok and gate['verdict'] == 'PASS' and t == {'KXNCAAWBGAME': '', 'KXNCAAWVMATCH': ''},
          {k: v['why'] for k, v in res['series'].items()})
    have = open(FIXTURE).read() if os.path.exists(FIXTURE) else None
    check('tests/deskd/fixtures/college_gate_ok.json is what the tool writes on the season (--write-fixture '
          'rewrites it)', have == dumps_gate(gate), 'differs' if have else 'missing')


def self_test():
    fails = []

    def check(name, ok, detail=''):
        print('%s %s%s' % ('OK  ' if ok else 'FAIL', name, (': ' + str(detail)) if detail else ''))
        if not ok:
            fails.append(name)

    # decide() is deskd's take_decide: the parity cases both languages assert
    path = os.path.join(ROOT, 'tests', 'deskd', 'fixtures', 'college_take_cases.json')
    bad = [n for n, _, i, p, w in tr.fixture_cases(path) if tr.got_of(decide(i, p)) != w]
    check('decide() gives every case of college_take_cases.json', not bad, bad)
    p = college_params(edge=2500, clip=5 * UNIT, leader_only=True, points_required=False)
    check("college_params: take_params(COLLEGE) with the run's flags", p['mode'] == 'COLLEGE' and p['edge'] == 2500
          and p['clip'] == 50000 and p['leader_only'] and not p['points_required'] and p['stale_only']
          and p['max_volume'] == 2000000 and p['cool_ms'] == 5000)

    s = S_US
    with tempfile.TemporaryDirectory() as d:
        sp = Rec(d)
        close = T0 + 30 * 3600 * s
        # a women's basketball game: AAA at home to BBB (ESPN home/away by homeAway), settled AAA
        sp.kmarket(T0 - 7200 * s, WB_A, WB_EV, 'Aaa State', close, fee_multiplier=1.0)
        sp.kmarket(T0 - 7200 * s, WB_B, WB_EV, 'Bbb Tech', close)
        sp.kmarket(T0 + 9000 * s, WB_A, WB_EV, 'Aaa State', close, result='yes', status='finalized')
        sp.kmarket(T0 + 9000 * s, WB_B, WB_EV, 'Bbb Tech', close, result='no', status='finalized')
        sp.kmarket(T0 + 9100 * s, WB_B, WB_EV, 'Bbb Tech', close, status='finalized')   # a later row without one
        # a volleyball match known only from the daily sweep, and a three-market event
        vb = 'KXNCAAWVMATCH-26NOV14CCCDDD'
        sp.daily(T0 - 9000 * s, vb + '-CCC', vb, 'Ccc', close)
        sp.daily(T0 - 9000 * s, vb + '-DDD', vb, 'Ddd', close)
        tri = 'KXNCAAWBGAME-26NOV14EEEFFF'
        for code in ('EEE', 'FFF', 'TIE'):
            sp.kmarket(T0 - 7200 * s, tri + '-' + code, tri, code, close)
        sp.kmarket(T0 - 7200 * s, 'KXHIGHNY-26NOV14-B50', 'KXHIGHNY-26NOV14', '50', close)   # not a college series
        gms = games(d, SERIES)
        g = gms.get(WB_EV, {})
        check('games: A and B by ticker, names, the latest result, the fee multiplier a row carries',
              g.get('tickers') == [WB_A, WB_B] and g['markets'][WB_A]['name'] == 'Aaa State'
              and g['markets'][WB_A]['result'] == 'yes' and g['markets'][WB_B]['result'] == 'no'
              and g['markets'][WB_A]['fee_mult'] == 1.0 and g['markets'][WB_B]['fee_mult'] is None
              and g['markets'][WB_B]['status'] == 'finalized' and g['date'] == '2026-11-14'
              and g['markets'][WB_A]['close_ms'] == close // 1000, g)
        check('games: the daily sweep alone names an event; a third market is kept (not a game); other series not',
              gms.get(vb, {}).get('tickers') == [vb + '-CCC', vb + '-DDD']
              and len(gms.get(tri, {}).get('tickers', [])) == 3
              and set(gms) == {WB_EV, vb, tri}, sorted(gms))

        # scores: ESPN's and NCAA's rows of the game, a feed outage and a recorder restart
        start = T0 + 60 * s
        names = {'home': 'Aaa State Owls', 'away': 'Bbb Tech Bees'}
        sp.score('ESPN', T0 - 3600 * s, None, WB_SPEC, '401', start, 'pre', 0, '0:00', 'AAA', 'BBB', 0, 0, names)
        sp.score('ESPN', T0 + 120 * s, T0 + 116 * s, WB_SPEC, '401', start, 'in', 1, '9:40', 'AAA', 'BBB', 2, 0, names)
        sp.score('ESPN', T0 + 90 * s, T0 + 86 * s, WB_SPEC, '401', start, 'in', 1, '10:00', 'AAA', 'BBB', 0, 0, names)
        sp.score('ESPN', T0 - 3600 * s, None, WB_SPEC, '402', T0 + 7200 * s, 'pre', 0, '0:00', 'XXX', 'YYY', 0, 0)
        sp.score('NCAA', T0 + 95 * s, None, 'basketball-women/d1', '6400', start, 'in', 1, '09:55', 'AAA', 'BBB', 0, 0,
                 {'home': 'Aaa State', 'away': 'Bbb Tech'})
        sp.score('ESPN', T0 - 3600 * s, None, 'volleyball/womens-college-volleyball', '901', start, 'pre', 0, '',
                 'CCC', 'DDD', 0, 0, {'home': 'Ccc', 'away': 'Ddd'})
        sp.gap(T0 + 300 * s, 'espn', 'poll_failed', WB_SPEC + ': HTTP 503', T0 + 200 * s, T0 + 300 * s)
        sp.gap(T0 + 330 * s, 'espn', 'recovered', 'down 130 s', T0 + 200 * s, T0 + 330 * s)
        sp.gap(T0 + 400 * s, 'ncaa', 'recovered', 'down 9 s', T0 + 391 * s, T0 + 400 * s)   # its poll_failed lost
        sp.gap(T0 + 500 * s, 'recorder', 'restart', 'not recording for 60 s', T0 + 440 * s, T0 + 500 * s)
        sp.gap(T0 + 600 * s, 'kalshi_ws', 'disconnect', 'test', T0 + 590 * s, None)
        scores, outages = score_rows(d)
        rs = scores.get(('ESPN', WB_SPEC, '401'), [])
        check('score_rows: by source, league and game, in the order first seen',
              [r['recv_us'] for r in rs] == [T0 - 3600 * s, T0 + 90 * s, T0 + 120 * s]
              and ('NCAA', 'basketball-women/d1', '6400') in scores and len(scores) == 4, sorted(scores))
        check("score_rows: an outage is its feed's league, the recovered row's too; a restart stops every source",
              outages['ESPN'] == [(T0 + 200 * s, T0 + 300 * s, 'basketball/womens-college-basketball'),
                                  (T0 + 200 * s, T0 + 330 * s, 'basketball/womens-college-basketball'),
                                  (T0 + 440 * s, T0 + 500 * s, None)]
              and outages['NCAA'] == [(T0 + 391 * s, T0 + 400 * s, None), (T0 + 440 * s, T0 + 500 * s, None)], outages)
        m = match_games(gms, scores)
        check("match_games: deskd's match_game finds the game on both boards, A at home",
              m.get(WB_EV) == {'ESPN': (('ESPN', WB_SPEC, '401'), True),
                               'NCAA': (('NCAA', 'basketball-women/d1', '6400'), True)}, m.get(WB_EV))
        check('match_games: the volleyball match on its own league; a three-market event is not a game',
              m.get(vb) == {'ESPN': (('ESPN', 'volleyball/womens-college-volleyball', '901'), True)}
              and tri not in m, m)

        # books: A on the socket, B by REST only
        # (the NO bids are YES asks at $1 - price: NO 0.45 is a YES ask at 0.55)
        sp.snapshot(T0, 1, 1, 1, WB_A, [(0.40, 10)], [(0.45, 10)])
        sp.delta(T0 + 10 * s, 1, 1, 2, WB_A, 'yes', 0.40, 5)        # size only: the bid's age stays
        sp.delta(T0 + 20 * s, 1, 1, 3, WB_A, 'no', 0.48, 7)         # a better ask (YES 0.52): a new touch
        sp.trade_id(T0 + 25 * s, 1, 2, 1, WB_A, 0.52, 3, 'yes', 'tr-1')
        sp.snapshot(T0 + 30 * s, 1, 1, 4, WB_A, [(0.40, 15)], [(0.48, 7), (0.45, 10)])   # a resubscription
        sp.delta(T0 + 40 * s, 1, 1, 6, WB_A, 'yes', 0.41, 1)        # seq 6 after 4: a gap, the book gone
        sp.snapshot(T0 + 50 * s, 1, 1, 7, WB_A, [(0.40, 15)], [(0.48, 7)])
        sp.trade_id(T0 + 55 * s, 1, 2, 2, WB_A, 0.40, 2, 'no', 'tr-2')
        sp.delta(T0 + 580 * s, 1, 1, 8, WB_A, 'yes', 0.42, 4)
        sp.trade_rest(T0 + 3600 * s, T0 + 25 * s, WB_A, 3, 'tr-1')   # the backfill saw tr-1 again
        sp.trade_rest(T0 + 3600 * s, T0 + 70 * s, WB_A, 4, 'tr-3')
        sp.rest_top(T0 + 5 * s, WB_B, 0.44, 6.0, 0.60, 3.0, volume=100.0)
        sp.rest_top(T0 + 15 * s, WB_B, 0.44, 9.0, 0.58, 3.0, volume=110.0)
        sp.rest_top(T0 + 45 * s, WB_B, 0.0, 0.0, 0.58, 3.0, volume=110.0)   # no bid: an empty side
        sp.gap(T0 + 70 * s, 'kalshi_rest_books', 'recovered', 'down 8 s', T0 + 60 * s, T0 + 68 * s)
        hist, vols = book_history(d, [WB_A, WB_B])
        ha, hb = hist.get(WB_A), hist.get(WB_B)
        check('the socket book: a size change keeps the touch age, a new price is a new touch',
              ha is not None and ha.src == 'ws' and ha.at(T0) == (4000, 1000, 5500, 1000, T0, T0)
              and ha.at(T0 + 10 * s) == (4000, 1500, 5500, 1000, T0, T0)
              and ha.at(T0 + 20 * s) == (4000, 1500, 5200, 700, T0, T0 + 20 * s), ha and list(zip(ha.times, ha.tops)))
        check("a resubscription's snapshot is no drop: the touches keep their age",
              ha.at(T0 + 30 * s) == (4000, 1500, 5200, 700, T0, T0 + 20 * s), ha.at(T0 + 30 * s))
        check('a sequence gap: no book until the next snapshot, whose touches are new',
              ha.at(T0 + 40 * s) is None and ha.at(T0 + 49 * s) is None
              and ha.at(T0 + 50 * s) == (4000, 1500, 5200, 700, T0 + 50 * s, T0 + 50 * s))
        check("the recorder's restart drops it from the socket's last frame until a snapshot; none before the "
              "first, none after the data", ha.at(T0 + 440 * s - 1) == (4000, 1500, 5200, 700, T0 + 50 * s, T0 + 50 * s)
              and ha.at(T0 + 440 * s) is None and ha.at(T0 + 580 * s) is None and ha.at(T0 - 1) is None
              and ha.at(T0 + 3600 * s) is None, list(zip(ha.times, ha.tops))[-3:])
        check('a market the socket never showed: REST tops, first seen at recv_us; an empty side; an outage',
              hb is not None and hb.src == 'rest'
              and hb.at(T0 + 5 * s) == (4400, 600, 6000, 300, T0 + 5 * s, T0 + 5 * s)
              and hb.at(T0 + 15 * s) == (4400, 900, 5800, 300, T0 + 5 * s, T0 + 15 * s)
              and hb.at(T0 + 45 * s) == (None, None, 5800, 300, 0, T0 + 15 * s)
              and hb.at(T0 + 65 * s) is None and hb.at(T0 + 68 * s) == (None, None, 5800, 300, 0, T0 + 68 * s),
              hb and list(zip(hb.times, hb.tops)))
        check('volumes: socket prints and the backfill, each trade_id once; REST volume otherwise',
              vols[WB_A].at(T0 + 24 * s) == 0 and vols[WB_A].at(T0 + 60 * s) == 500
              and vols[WB_A].at(T0 + 70 * s) == 900
              and vols[WB_B].at(T0 + 5 * s) == 10000 and vols[WB_B].at(T0 + 20 * s) == 11000,
              (vols[WB_A].times, vols[WB_A].cum))
        check('Hist.changes: the change times in a span (the resubscription changed nothing)',
              ha.changes(T0 + 10 * s, T0 + 30 * s) == [T0 + 10 * s, T0 + 20 * s])
        res = survey(d, SERIES)
        check('survey: per series, events, games, matched, books and settled',
              res['series'].get('KXNCAAWBGAME') == {'events': 2, 'games': 1, 'espn': 1, 'ncaa': 1, 'books_ws': 1,
                                                    'books_rest': 1, 'settled': 1}, res)
        with contextlib.redirect_stdout(io.StringIO()):
            rc = run(['--data', d])
        check('the CLI runs', rc == 0)
    with contextlib.redirect_stderr(io.StringIO()):
        check('the CLI refuses no --data: there is no default', run([]) == 2)
    self_test_part2(check)
    print('college_gate self-test: %s' % ('ok' if not fails else 'FAILED (%d)' % len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(run(sys.argv[1:]))
