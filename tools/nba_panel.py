#!/usr/bin/env python3
"""The NBA panel: one row per game state, from ESPN's play-by-play and Kalshi's KXNBAGAME books,
for tools/train_bball.py --league nba (the model) and tools/nba_gate.py n1 (its gates). Plan 5(f)
generalised to the NBA; docs/edge_results.json hunt:college is the method.

Two sources, the same rows:
  history   past seasons from caches on disk (--espn-dir, --kalshi-dir; --fetch fills them on the
            operator's Mac): ESPN's NBA scoreboards and game summaries, and Kalshi's markets and
            1-minute candlesticks. A play's state is visible only at the running maximum of its
            game's play wallclocks so far, plus --lag-s: ESPN's wallclock is its own time for the
            play and looks ahead, and ESPN runs behind the venue (41.5 s until tools/nba_latency.py
            measures the NBA's). A play whose wallclock falls under that maximum was inserted late
            and is dropped (the plays after it carry its score). Plays are taken in feed order.
  recorder  this season from the recorder's data directory (--recorder, recorder/store.py): each
            scoreboard state (espn_scores, a row at the poll that first showed a change: the score,
            the period, the last play, or the clock every 30 s) visible at its recv_us, the books
            replayed from kalshi_ws by tools/lip_join.py, results from kalshi_markets. A game in
            both sources keeps its recorder rows.
Games are matched to Kalshi's events by tools/espn_match.py (deskd's match_game): a game is its
event's two markets, each paying on one team, and every row is from the home team's side.

The panel (--out FILE.csv; an existing file is refused without --force), a header row and one row
per visible game state in play, games by tip then id, states in the order seen:

  game_id      ESPN's game id
  season       the NBA season of the tip's ET date, as 2025-26 (August on is the next season)
  t_left_s     regulation seconds left (4 x 720 s), from the period and the period's clock
               (train_bball.t_left, deskd's bball_t_left); in overtime the overtime clock (0-300)
  ot           1 in overtime, else 0
  margin       the home score minus the away score
  possession   +1 the home team has the ball, -1 the away team, 0 unknown. History: 0 (a play
               says who acted, not who has the ball next). Recorder: the scoreboard's
               situation.possession matched to the teams' ids (TO CONFIRM on a capture)
  p0           the home team's pregame price: the mid of its effective book (below) at the last
               quote at or before ESPN's scheduled tip, at most 12 h before it, whose effective
               spread is 15c or less (strat/game_fair.h's capture: history the 1-minute closes,
               recorder every book change). A game without one has no rows
  yes_won      1 if the home team won: Kalshi's settlement, else ESPN's final score
  kalshi_mid, kalshi_bid, kalshi_ask
               the home team's effective book at visible_ms over both markets, dollars: bid
               max(home bid, 1 - away ask), ask min(home ask, 1 - away bid) (game_fair.h's
               effective_book); blank where no side gives one, the mid blank unless both do
  espn_wp      ESPN's home win probability at visible_ms (blank: none yet). History: the summary
               chart's point of the latest play shown. Recorder: the latest chart point first
               seen (espn_plays; a play first seen under a sequence number already shown was
               inserted late and is dropped), else the scoreboard's last-play probability
               (situation.lastPlay.probability, TO CONFIRM), as deskd's TakeEngine reads them
  visible_ms   when the state was visible, epoch ms

and, for tools/nba_gate.py's executable replay:

  source       history or recorder
  lag_s        the lag a history row was built with (blank on recorder rows: they need none)
  event, home_ticker, away_ticker
               Kalshi's event and the home and away teams' markets (each one's YES pays on its team)
  start_ms     ESPN's scheduled tip
  period, clock, home_score, away_score   the state as ESPN showed it
  seen_ms      the score state's first sighting: the first row with this period and score, as
               deskd's EspnRef::first_seen_ms (the clock alone does not move it)
  home_bid, home_ask, away_bid, away_ask
               each market's own touch at visible_ms, dollars (blank: that side empty or no book)
  home_bid_next, home_ask_next, away_bid_next, away_ask_next
               the same 60 s later: the conservative fill takes the worse of the two

Quotes. History: a candle's close (yes_bid and yes_ask at end_period_ts) is the quote from that
instant until the next candle, carried at most 120 s (a minute missing from the candles; how
often is TO CONFIRM), never a candle that ends after the instant read. Recorder: the book as
replayed (deskd's units and rules), until it changes or is dropped (a sequence gap, a disconnect,
a restart: blank until its next snapshot).

The caches (history):
  ESPN_DIR/scoreboard-YYYYMMDD.json    ESPN's NBA scoreboard of that date
  ESPN_DIR/summary-<game id>.json      ESPN's summary of one game (plays, winprobability)
  KALSHI_DIR/markets*.json             {"markets": [...]}, GET /markets pages; a later file's row
                                       of a ticker wins (files read in name order)
  KALSHI_DIR/candles/<ticker>.json     GET .../candlesticks?period_interval=1 for one market
--fetch (the operator's Mac only: this tool refuses it elsewhere; public endpoints, no key, at
most 3 requests a second) fills them for --from to --to (ET game dates), skipping files already
there: each date's scoreboard, each finished game's summary, the KXNBAGAME markets closing from
--from to three days after --to, and each matched game's two markets' 1-minute candles from 13 h
before the tip to 5 h after it (the series path, then the historical one on a 404). The URL
shapes and the candle fields (close_dollars strings, else close in cents) are TO CONFIRM there.

Standard library only. Read-only, but for --out and the caches --fetch writes.

Usage:
  python3 tools/nba_panel.py --espn-dir D --kalshi-dir D [--recorder DIR] [--lag-s 41.5]
                             --out FILE.csv [--force] [--json]
  python3 tools/nba_panel.py --recorder DIR --out FILE.csv [--force]
  python3 tools/nba_panel.py --fetch --from YYYY-MM-DD --to YYYY-MM-DD --espn-dir D --kalshi-dir D
  python3 tools/nba_panel.py --self-test
"""
import argparse
import bisect
import csv
import datetime
import glob
import json
import os
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import espn_match as em   # noqa: E402  (deskd's matching, ESPN's times and clock)
import lip_join as lj     # noqa: E402  (the recorder's reader and the book replay)
import train_bball as tb  # noqa: E402  (the NBA clock: t_left)

UNIT, CENT = lj.UNIT, lj.CENT   # prices in 1/10,000 $ (deskd's Px)
SERIES = 'KXNBAGAME'
ESPN_PATH = 'basketball/nba'
LEAGUE = tb.LEAGUES['nba']
LAG_S = 41.5                    # until tools/nba_latency.py measures the NBA's
P0_SPREAD = 15 * CENT           # GameFair::kMaxSpread
P0_KEEP_MS = 12 * 3600_000      # GameFair::kKeepMs: a capture older than this before the tip is none
NEXT_MS = 60_000                # the conservative fill's quote: a minute on
CANDLE_AGE_MS = 120_000         # a candle's close is carried at most this long
ESPN_BASE = 'https://site.api.espn.com/apis/site/v2/sports'
KALSHI_BASE = 'https://api.elections.kalshi.com/trade-api/v2'
FETCH_EVERY_S = 1 / 3           # at most 3 requests a second

COLUMNS = ('game_id', 'season', 't_left_s', 'ot', 'margin', 'possession', 'p0', 'yes_won', 'kalshi_mid',
           'kalshi_bid', 'kalshi_ask', 'espn_wp', 'visible_ms', 'source', 'lag_s', 'event', 'home_ticker',
           'away_ticker', 'start_ms', 'period', 'clock', 'home_score', 'away_score', 'seen_ms', 'home_bid',
           'home_ask', 'away_bid', 'away_ask', 'home_bid_next', 'home_ask_next', 'away_bid_next', 'away_ask_next')


# ---- small things -------------------------------------------------------------------------------
def dollars(px):
    """Px as the contract's dollars string ('0.4500'); None as blank."""
    return '' if px is None else '%d.%04d' % divmod(px, UNIT)


def number(v):
    """'%g' to 10 digits (2880, 35.4); None as blank."""
    return '' if v is None else '%.10g' % v


def season_of(ms):
    """The NBA season of an instant's ET date: 2025-26 from August 2025 to July 2026."""
    d = datetime.date.fromisoformat(em.et_date(ms))
    y = d.year if d.month >= 8 else d.year - 1
    return '%d-%02d' % (y, (y + 1) % 100)


def as_int(v):
    """An int from an int or a digit string (ESPN writes scores and periods either way); else None."""
    if isinstance(v, bool):
        return None
    if isinstance(v, int):
        return v
    if isinstance(v, str) and v.strip().lstrip('-').isdigit():
        return int(v)
    return None


def as_prob(v):
    """A probability in [0, 1] from a number; else None."""
    if isinstance(v, (int, float)) and not isinstance(v, bool) and 0.0 <= v <= 1.0:
        return float(v)
    return None


def raw_of(row):
    try:
        v = json.loads(row.get('raw') or '{}')
    except ValueError:
        return {}
    return v if isinstance(v, dict) else {}


def load_json(path):
    with open(path, encoding='utf-8') as fh:
        return json.load(fh)


def write_json(path, body):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as fh:
        json.dump(body, fh, separators=(',', ':'))
    os.replace(tmp, path)


def effective(home, away):
    """game_fair.h effective_book for the home team: (bid, ask) in Px over both markets' touches
    ((bid, ask) or None for no book), each None when no side gives one."""
    bids, asks = [], []
    if home is not None:
        if home[0] is not None:
            bids.append(home[0])
        if home[1] is not None:
            asks.append(home[1])
    if away is not None:
        if away[1] is not None:
            bids.append(UNIT - away[1])
        if away[0] is not None:
            asks.append(UNIT - away[0])
    return (max(bids) if bids else None, min(asks) if asks else None)


# ---- the quotes ---------------------------------------------------------------------------------
class Quotes:
    """Each market's touch over time: entries (t ms, (bid, ask) or None), bid and ask in Px or None
    (that side empty), None a book dropped. at() reads the entry at or before t, never a later one,
    and nothing when it is older than max_age_ms (None: an entry lasts until the next)."""

    def __init__(self, max_age_ms=None):
        self.ts, self.vs = defaultdict(list), defaultdict(list)
        self.max_age_ms = max_age_ms

    def add(self, tk, t, v):
        """Entries come oldest first; one dated before the last (a recorder row after a clock step)
        is dated at the last."""
        ts, vs = self.ts[tk], self.vs[tk]
        if ts and t <= ts[-1]:
            vs[-1] = v        # the same instant: the later wins
        else:
            ts.append(t)
            vs.append(v)

    def at(self, tk, t):
        ts = self.ts.get(tk)
        if not ts:
            return None
        i = bisect.bisect_right(ts, t) - 1
        if i < 0 or (self.max_age_ms is not None and t - ts[i] > self.max_age_ms):
            return None
        return self.vs[tk][i]

    def times(self, tk, a, b):
        """The entries' times in [a, b]."""
        ts = self.ts.get(tk, [])
        return ts[bisect.bisect_left(ts, a):bisect.bisect_right(ts, b)]


def candle_px(side):
    """A candle's close for one side (its yes_bid or yes_ask) in Px: close_dollars ('0.4500'),
    else close in whole cents; None when absent or unreadable."""
    if not isinstance(side, dict):
        return None
    d = side.get('close_dollars')
    if isinstance(d, str):
        return lj.px_of(d)
    c = side.get('close')
    if isinstance(c, int) and not isinstance(c, bool) and 0 <= c <= 100:
        return c * CENT
    return None


def read_candles(body):
    """[(end ms, bid, ask)] from a candlesticks response, oldest first: a bid of 0 or an ask of $1
    is that side empty."""
    out = []
    for c in (body.get('candlesticks') if isinstance(body, dict) else None) or []:
        end = as_int(c.get('end_period_ts')) if isinstance(c, dict) else None
        if end is None:
            continue
        bid, ask = candle_px(c.get('yes_bid')), candle_px(c.get('yes_ask'))
        out.append((end * 1000, bid if bid else None, ask if ask is not None and ask < UNIT else None))
    out.sort(key=lambda x: x[0])
    return out


class TopReplay(lj.Replay):
    """lip_join's replay of kalshi_ws keeping each kept market's touch in a Quotes whenever it
    changes, and None when its book is dropped (a drop at a disconnect is dated by the last row
    before it: early, never late)."""

    def __init__(self, keep, quotes):
        super().__init__(keep)
        self.quotes = quotes
        self.t_ms = 0
        self.last = {}
        self.on_drop = self.top

    def top(self, tk):
        b = self.books.get(tk)
        v = None if b is None else (b.best_bid(), b.best_ask())
        if tk in self.last:
            if self.last[tk] == v:
                return   # the touch did not move (a level behind it did)
        elif v is None:
            return       # nothing known yet: no entry
        self.last[tk] = v
        self.quotes.add(tk, self.t_ms, v)

    def row(self, r):
        t = r.get('recv_us')
        if isinstance(t, int) and not isinstance(t, bool):
            self.t_ms = t // 1000
        super().row(r)
        tk = r.get('ticker')
        if tk and (self.keep is None or self.keep(tk)):
            self.top(tk)


# ---- the games ----------------------------------------------------------------------------------
class Game:
    """One matched game: its source, ESPN id, Kalshi event and the home and away markets, ESPN's
    scheduled tip, the home result (1, 0, None unknown), its visible states in order, and the
    quotes its rows read. A state: {visible_ms, period, clock, home_score, away_score,
    possession, wp} (wp ESPN's home number at that instant or None)."""

    def __init__(self, source, game_id, event, home_ticker, away_ticker, start_ms, yes_won, quotes, lag_s=None):
        self.source, self.game_id, self.event = source, game_id, event
        self.home_ticker, self.away_ticker = home_ticker, away_ticker
        self.start_ms, self.yes_won, self.quotes, self.lag_s = start_ms, yes_won, quotes, lag_s
        self.states = []

    def touch(self, t):
        return self.quotes.at(self.home_ticker, t), self.quotes.at(self.away_ticker, t)

    def p0(self):
        """The home team's effective mid at the last quote at or before the tip, at most
        P0_KEEP_MS before it, with an effective spread of 15c or less (module docstring)."""
        a = self.start_ms - P0_KEEP_MS
        times = sorted(set(self.quotes.times(self.home_ticker, a, self.start_ms))
                       | set(self.quotes.times(self.away_ticker, a, self.start_ms)), reverse=True)
        for t in times:
            bid, ask = effective(*self.touch(t))
            if bid is not None and ask is not None and ask - bid <= P0_SPREAD:
                return (bid + ask) / 2 / UNIT
        return None


def result_of(markets, home, away, final=None):
    """1 if the home team won: the home market settled yes (or the away market no), else ESPN's
    final (home score, away score) when given; None unknown."""
    r = (markets.get(home) or {}).get('result')
    if r in ('yes', 'no'):
        return 1 if r == 'yes' else 0
    r = (markets.get(away) or {}).get('result')
    if r in ('yes', 'no'):
        return 0 if r == 'yes' else 1
    if final is not None and final[0] != final[1]:
        return 1 if final[0] > final[1] else 0
    return None


def match_events(markets, espn_games, counts):
    """[(KalshiGame, EspnGame, a_home)] for the series' two-market events ESPN shows."""
    kgames = em.kalshi_games(sorted(markets.values(), key=lambda m: m['ticker']))
    by_id = {g.id: g for g in espn_games}
    out = []
    for ev, k in sorted(kgames.items()):
        counts['events'] += 1
        if not k.ticker_b:
            counts['one_market'] += 1
            continue
        m = em.match_game(k, espn_games)
        if m is None:
            counts['unmatched'] += 1
            continue
        out.append((k, by_id[m.id], m.a_home))
    return out


def sides(k, a_home):
    return (k.ticker_a, k.ticker_b) if a_home else (k.ticker_b, k.ticker_a)


# ---- history: ESPN's summaries and Kalshi's candles ------------------------------------------------
def summary_states(summary, lag_ms):
    """One game's visible states from its ESPN summary: ([state], plays dropped as late, plays
    without a wallclock or a score). Plays in feed order; a play is visible at the running maximum
    of the wallclocks so far plus the lag; one under that maximum is late and dropped."""
    chart = {}
    for p in (summary.get('winprobability') if isinstance(summary, dict) else None) or []:
        if isinstance(p, dict) and p.get('playId') not in (None, ''):
            chart[str(p['playId'])] = p
    plays = (summary.get('plays') if isinstance(summary, dict) else None) or []
    out, top, wp, late, unread = [], None, None, 0, 0
    for p in plays:
        if not isinstance(p, dict):
            continue
        w = em.iso_ms(p.get('wallclock') if isinstance(p.get('wallclock'), str) else '')
        period = p.get('period')
        period = as_int(period.get('number') if isinstance(period, dict) else period)
        clock = p.get('clock')
        clock = clock.get('displayValue') if isinstance(clock, dict) else clock
        hs, as_ = as_int(p.get('homeScore')), as_int(p.get('awayScore'))
        if not w or period is None or hs is None or as_ is None or not isinstance(clock, str):
            unread += 1
            continue
        if top is not None and w < top:
            late += 1
            continue
        top = w
        point = chart.get(str(p.get('id')))
        if point is not None and as_prob(point.get('homeWinPercentage')) is not None:
            wp = as_prob(point['homeWinPercentage'])
        out.append({'visible_ms': w + lag_ms, 'period': period, 'clock': clock, 'home_score': hs,
                    'away_score': as_, 'possession': 0, 'wp': wp})
    return out, late, unread


def history_markets(kalshi_dir):
    """ticker -> the API's market row, every markets*.json in name order (a later file's row wins)."""
    out = {}
    for path in sorted(glob.glob(os.path.join(kalshi_dir, 'markets*.json'))):
        body = load_json(path)
        for m in (body.get('markets') if isinstance(body, dict) else None) or []:
            tk = m.get('ticker') if isinstance(m, dict) else None
            if isinstance(tk, str) and tk.startswith(SERIES + '-'):
                out[tk] = {'ticker': tk, 'event_ticker': m.get('event_ticker') or tk[:tk.rfind('-')],
                           'yes_sub_title': m.get('yes_sub_title') or '', 'result': m.get('result') or ''}
    return out


def history_scoreboards(espn_dir):
    games = {}
    for path in sorted(glob.glob(os.path.join(espn_dir, 'scoreboard-*.json'))):
        for g in em.parse_scoreboard(load_json(path), 'nba', 0):
            games[g.id] = g
    return list(games.values())


def read_history(espn_dir, kalshi_dir, lag_s, counts):
    """The cached seasons' games ([Game]), counting what was left out."""
    markets = history_markets(kalshi_dir)
    lag_ms = int(round(lag_s * 1000))
    out = []
    for k, g, a_home in match_events(markets, history_scoreboards(espn_dir), counts):
        home, away = sides(k, a_home)
        spath = os.path.join(espn_dir, 'summary-%s.json' % g.id)
        if not os.path.exists(spath):
            counts['no_summary'] += 1
            continue
        quotes = Quotes(CANDLE_AGE_MS)
        missing = False
        for tk in (home, away):
            cpath = os.path.join(kalshi_dir, 'candles', tk + '.json')
            if not os.path.exists(cpath):
                missing = True
                continue
            for t, bid, ask in read_candles(load_json(cpath)):
                quotes.add(tk, t, (bid, ask))
        if missing:
            counts['no_candles'] += 1
            continue
        final = (g.home_score, g.away_score) if g.state == 'post' else None
        game = Game('history', g.id, k.event_ticker, home, away, g.start_ms, result_of(markets, home, away, final),
                    quotes, lag_s)
        game.states, late, unread = summary_states(load_json(spath), lag_ms)
        counts['late_plays'] += late
        counts['unread_plays'] += unread
        out.append(game)
    return out


# ---- the recorder's data --------------------------------------------------------------------------
def recorder_markets(data):
    """ticker -> the latest kalshi_markets row of the series (event, team name, result)."""
    out = {}
    for r in lj.rows(data, 'kalshi_markets'):
        tk, t = r.get('ticker') or '', r.get('recv_us') or 0
        if not tk.startswith(SERIES + '-') or (tk in out and out[tk]['recv_us'] > t):
            continue
        raw = raw_of(r)
        out[tk] = {'recv_us': t, 'ticker': tk, 'event_ticker': raw.get('event_ticker') or tk[:tk.rfind('-')],
                   'yes_sub_title': raw.get('yes_sub_title') or '',
                   'result': r.get('result') or raw.get('result') or ''}
    return out


def recorder_rows(data, table):
    """game_id -> the NBA's rows of an ESPN table (espn_scores, espn_plays), oldest first."""
    out = defaultdict(list)
    for r in lj.rows(data, table):
        if (r.get('league') or '').split(':')[0] == ESPN_PATH and r.get('game_id') not in (None, '') \
                and isinstance(r.get('recv_us'), int):
            out[str(r['game_id'])].append(r)
    for rows in out.values():
        rows.sort(key=lambda r: r['recv_us'])
    return out


def possession_of(raw):
    """+1 home, -1 away, 0 unknown: the scoreboard's situation.possession (a team id) against the
    competitors' ids (TO CONFIRM on a capture)."""
    who = (raw.get('situation') or {}).get('possession') if isinstance(raw.get('situation'), dict) else None
    if who in (None, ''):
        return 0
    for c in raw.get('competitors') or []:
        team = c.get('team') if isinstance(c, dict) and isinstance(c.get('team'), dict) else {}
        if team.get('id') not in (None, '') and str(team['id']) == str(who):
            return 1 if c.get('homeAway') == 'home' else -1 if c.get('homeAway') == 'away' else 0
    return 0


def scoreboard_wp(raw):
    """The scoreboard's last-play home probability (situation.lastPlay.probability, TO CONFIRM)."""
    sit = raw.get('situation') if isinstance(raw.get('situation'), dict) else {}
    last = sit.get('lastPlay') if isinstance(sit.get('lastPlay'), dict) else {}
    prob = last.get('probability') if isinstance(last.get('probability'), dict) else {}
    return as_prob(prob.get('homeWinPercentage'))


def play_points(plays):
    """[(first seen ms, home wp)]: each play's chart point at the first espn_plays row that had
    one, a play first seen under a sequence number already shown dropped (inserted late), and
    how many were dropped."""
    first = {}
    for r in plays:
        if r.get('play_id') and as_prob(r.get('wp_home')) is not None and r['play_id'] not in first:
            first[r['play_id']] = r
    out, top, late = [], None, 0
    for r in sorted(first.values(), key=lambda r: (r['recv_us'], r.get('seq') if isinstance(r.get('seq'), int) else 0)):
        seq = r.get('seq')
        if isinstance(seq, int) and not isinstance(seq, bool):
            if top is not None and seq < top:
                late += 1
                continue
            top = seq
        out.append((r['recv_us'] // 1000, as_prob(r['wp_home'])))
    return out, late


def scoreboard_states(rows, plays):
    """The visible in-play states of one recorded game, and the late plays dropped."""
    points, late = play_points(plays)
    pt = [p[0] for p in points]
    out = []
    for r in rows:
        if r.get('state') != 'in':
            continue
        t = r['recv_us'] // 1000
        hs, as_, period = as_int(r.get('home_score')), as_int(r.get('away_score')), as_int(r.get('period'))
        if hs is None or as_ is None or period is None or not isinstance(r.get('clock'), str):
            continue
        raw = raw_of(r)
        i = bisect.bisect_right(pt, t) - 1
        out.append({'visible_ms': t, 'period': period, 'clock': r['clock'], 'home_score': hs, 'away_score': as_,
                    'possession': possession_of(raw), 'wp': points[i][1] if i >= 0 else scoreboard_wp(raw)})
    return out, late


def read_recorder(data, counts):
    """This season's recorded games ([Game]), counting what was left out."""
    markets = recorder_markets(data)
    scores, plays = recorder_rows(data, 'espn_scores'), recorder_rows(data, 'espn_plays')
    espn = [em.game_from_score_row(rows[-1]) for rows in scores.values()]
    matched = match_events(markets, espn, counts)
    quotes = Quotes()
    keep = {tk for k, _, _ in matched for tk in (k.ticker_a, k.ticker_b)}
    lj.drive(data, TopReplay(lambda tk: tk in keep, quotes), 3600 * 1_000_000, lambda t: None, lj.down_instants(data))
    out = []
    for k, g, a_home in matched:
        home, away = sides(k, a_home)
        rows = scores[g.id]
        last = rows[-1]
        final = (as_int(last.get('home_score')), as_int(last.get('away_score')))
        final = final if last.get('state') == 'post' and None not in final else None
        game = Game('recorder', g.id, k.event_ticker, home, away, g.start_ms, result_of(markets, home, away, final),
                    quotes)
        game.states, late = scoreboard_states(rows, plays.get(g.id, []))
        counts['late_plays'] += late
        out.append(game)
    return out


# ---- the panel ----------------------------------------------------------------------------------
def game_rows(g, counts):
    """A game's panel rows (dicts of COLUMNS' text): one per visible state in play whose clock
    reads, the last of several visible at the same instant."""
    p0 = g.p0()
    if p0 is None:
        counts['no_p0'] += 1
        return []
    if g.yes_won is None:
        counts['no_result'] += 1
        return []
    out, key, seen = [], None, None
    for s in g.states:
        k = (s['period'], s['home_score'], s['away_score'])
        if k != key:
            key, seen = k, s['visible_ms']
        c = tb.t_left(LEAGUE, s['period'], em.clock_seconds(s['clock']))
        if c is None:
            counts['no_clock'] += 1
            continue
        v = s['visible_ms']
        (hq, aq), (hn, an) = g.touch(v), g.touch(v + NEXT_MS)
        bid, ask = effective(hq, aq)
        mid = '%.5f' % ((bid + ask) / 2 / UNIT) if bid is not None and ask is not None else ''
        row = {'game_id': g.game_id, 'season': season_of(g.start_ms), 't_left_s': number(c[0]),
               'ot': '1' if c[1] else '0', 'margin': str(s['home_score'] - s['away_score']),
               'possession': str(s['possession']), 'p0': '%.5f' % p0, 'yes_won': str(g.yes_won), 'kalshi_mid': mid,
               'kalshi_bid': dollars(bid), 'kalshi_ask': dollars(ask),
               'espn_wp': '' if s['wp'] is None else '%.6g' % s['wp'], 'visible_ms': str(v), 'source': g.source,
               'lag_s': number(g.lag_s), 'event': g.event, 'home_ticker': g.home_ticker, 'away_ticker': g.away_ticker,
               'start_ms': str(g.start_ms), 'period': str(s['period']), 'clock': s['clock'],
               'home_score': str(s['home_score']), 'away_score': str(s['away_score']), 'seen_ms': str(seen)}
        for side, nxt, q in (('home', '', hq), ('away', '', aq), ('home', '_next', hn), ('away', '_next', an)):
            row['%s_bid%s' % (side, nxt)] = dollars(q[0] if q else None)
            row['%s_ask%s' % (side, nxt)] = dollars(q[1] if q else None)
        if out and out[-1]['visible_ms'] == row['visible_ms']:
            out[-1] = row
            counts['same_instant'] += 1
        else:
            out.append(row)
    if out:
        counts['games'] += 1
    return out


def build(espn_dir=None, kalshi_dir=None, recorder=None, lag_s=LAG_S):
    """(rows, counts): the panel from the sources given (module docstring)."""
    counts = {}
    rec = read_recorder(recorder, counts.setdefault('recorder', Counter())) if recorder else []
    hist = read_history(espn_dir, kalshi_dir, lag_s, counts.setdefault('history', Counter())) if espn_dir else []
    have = {g.game_id for g in rec}
    rows = []
    for g in sorted(rec + hist, key=lambda g: (g.start_ms, g.game_id, g.source != 'recorder')):
        c = counts[g.source]
        if g.source == 'history' and g.game_id in have:
            c['recorded_instead'] += 1
            continue
        rows.extend(game_rows(g, c))
    for src, c in counts.items():
        c['rows'] = sum(1 for r in rows if r['source'] == src)
    return rows, {k: dict(v) for k, v in counts.items()}


def write_panel(path, rows, force=False):
    if os.path.exists(path) and not force:
        raise FileExistsError('%s exists; --force replaces it' % path)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + '.tmp'
    with open(tmp, 'w', newline='', encoding='utf-8') as fh:
        w = csv.DictWriter(fh, fieldnames=COLUMNS)
        w.writeheader()
        w.writerows(rows)
    os.replace(tmp, path)


def report(rows, counts, out):
    games = {r['game_id'] for r in rows}
    seasons = Counter()
    for g in {(r['game_id'], r['season']) for r in rows}:
        seasons[g[1]] += 1
    lines = ['nba_panel: %d row(s), %d game(s) (%s) -> %s' % (len(rows), len(games),
             ', '.join('%s %d' % kv for kv in sorted(seasons.items())) or 'none', out)]
    for src in ('history', 'recorder'):
        c = counts.get(src) or {}
        if c:
            lines.append('  %-9s %s' % (src, ', '.join('%s %d' % kv for kv in sorted(c.items()))))
    return '\n'.join(lines)


# ---- --fetch (the operator's Mac) ----------------------------------------------------------------
def scoreboard_url(day):
    return '%s/%s/scoreboard?dates=%s&limit=500' % (ESPN_BASE, ESPN_PATH, day.strftime('%Y%m%d'))


def summary_url(game_id):
    return '%s/%s/summary?event=%s' % (ESPN_BASE, ESPN_PATH, urllib.parse.quote(str(game_id), safe=''))


def markets_url(min_close_ts, max_close_ts, cursor=''):
    page = '&cursor=' + urllib.parse.quote(cursor, safe='') if cursor else ''
    return '%s/markets?series_ticker=%s&min_close_ts=%d&max_close_ts=%d&limit=1000%s' % (
        KALSHI_BASE, SERIES, min_close_ts, max_close_ts, page)


def candles_url(ticker, start_ts, end_ts, historical=False):
    path = 'historical/markets/%s' % ticker if historical else 'series/%s/markets/%s' % (SERIES, ticker)
    return '%s/%s/candlesticks?start_ts=%d&end_ts=%d&period_interval=1' % (KALSHI_BASE, path, start_ts, end_ts)


def http_json(url, timeout=20):
    req = urllib.request.Request(url, headers={'Accept': 'application/json', 'User-Agent': 'kalshi-desk nba_panel'})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


class Paced:
    """get(url) at most once every every_s seconds, both hosts together."""

    def __init__(self, get=http_json, clock=time.monotonic, sleep=time.sleep, every_s=FETCH_EVERY_S):
        self.get, self.clock, self.sleep, self.every_s = get, clock, sleep, every_s
        self.next_s = None
        self.calls = 0

    def __call__(self, url):
        if self.next_s is not None and self.clock() < self.next_s:
            self.sleep(self.next_s - self.clock())
        self.next_s = self.clock() + self.every_s
        self.calls += 1
        return self.get(url)


def fetch_refusal(platform):
    """None on the operator's Mac, else why --fetch refuses (this container reaches neither host)."""
    return None if platform == 'darwin' else ('--fetch runs on the operator\'s Mac only (sys.platform %r): '
                                              'it reads site.api.espn.com and Kalshi\'s public API' % platform)


def fetch(espn_dir, kalshi_dir, day_from, day_to, get, log=print):
    """Fill the caches for ET dates day_from..day_to (module docstring), skipping files already
    there. get(url) -> the JSON body (a Paced). Returns the counts; a failed file is counted and
    skipped, never fatal."""
    counts = Counter()

    def cached(path, url, fallback=None):
        if os.path.exists(path):
            counts['cached'] += 1
            return True
        try:
            body = get(url)
        except urllib.error.HTTPError as e:
            if e.code == 404 and fallback:
                return cached(path, fallback)
            counts['failed'] += 1
            log('fetch: %s: HTTP %d' % (url, e.code))
            return False
        except (OSError, ValueError) as e:
            counts['failed'] += 1
            log('fetch: %s: %s' % (url, e))
            return False
        write_json(path, body)
        counts['written'] += 1
        return True

    games, day = {}, day_from
    while day <= day_to:
        path = os.path.join(espn_dir, 'scoreboard-%s.json' % day.strftime('%Y%m%d'))
        if cached(path, scoreboard_url(day)):
            for g in em.parse_scoreboard(load_json(path), 'nba', 0):
                games[g.id] = g
        day += datetime.timedelta(days=1)
    for g in sorted(games.values(), key=lambda g: g.id):
        if g.state == 'post':
            cached(os.path.join(espn_dir, 'summary-%s.json' % g.id), summary_url(g.id))
    lo = int(datetime.datetime.combine(day_from, datetime.time(), datetime.timezone.utc).timestamp())
    hi = int(datetime.datetime.combine(day_to + datetime.timedelta(days=4), datetime.time(),
                                       datetime.timezone.utc).timestamp())
    mpath = os.path.join(kalshi_dir, 'markets-%s-%s.json' % (day_from.isoformat(), day_to.isoformat()))
    if not os.path.exists(mpath):
        rows, cursor = [], ''
        try:
            while True:
                body = get(markets_url(lo, hi, cursor))
                rows.extend(body.get('markets') or [])
                cursor = body.get('cursor') or ''
                if not cursor:
                    break
            write_json(mpath, {'markets': rows})
            counts['written'] += 1
        except (OSError, ValueError) as e:
            counts['failed'] += 1
            log('fetch: the markets list: %s' % e)
            return dict(counts)
    for k, g, a_home in match_events(history_markets(kalshi_dir), list(games.values()), counts):
        a, b = g.start_ms // 1000 - 13 * 3600, g.start_ms // 1000 + 5 * 3600
        for tk in (k.ticker_a, k.ticker_b):
            cached(os.path.join(kalshi_dir, 'candles', tk + '.json'), candles_url(tk, a, b),
                   candles_url(tk, a, b, historical=True))
    return dict(counts)


# ---- the command line --------------------------------------------------------------------------
def run(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--espn-dir', help="the ESPN cache: scoreboard-YYYYMMDD.json, summary-<id>.json")
    ap.add_argument('--kalshi-dir', help='the Kalshi cache: markets*.json, candles/<ticker>.json')
    ap.add_argument('--recorder', help="the recorder's data directory (this season)")
    ap.add_argument('--lag-s', type=float, default=LAG_S, help='seconds after the running-max wallclock a history '
                    'play is visible (default 41.5, until tools/nba_latency.py measures it)')
    ap.add_argument('--out', help='the panel CSV to write')
    ap.add_argument('--force', action='store_true', help='replace --out if it exists')
    ap.add_argument('--json', action='store_true', help='print the counts as JSON')
    ap.add_argument('--fetch', action='store_true', help="fill the caches (the operator's Mac only)")
    ap.add_argument('--from', dest='day_from', help='--fetch: the first ET game date, YYYY-MM-DD')
    ap.add_argument('--to', dest='day_to', help='--fetch: the last ET game date, YYYY-MM-DD')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()

    def fail(msg):
        print('nba_panel: ' + msg, file=sys.stderr)
        return 2
    if a.fetch:
        why = fetch_refusal(sys.platform)
        if why:
            return fail(why)
        if not (a.espn_dir and a.kalshi_dir and a.day_from and a.day_to):
            return fail('--fetch needs --from, --to, --espn-dir and --kalshi-dir')
        try:
            d0, d1 = datetime.date.fromisoformat(a.day_from), datetime.date.fromisoformat(a.day_to)
        except ValueError:
            return fail('--from and --to are dates, YYYY-MM-DD')
        if d1 < d0:
            return fail('--to is before --from')
        c = fetch(a.espn_dir, a.kalshi_dir, d0, d1, Paced())
        print('nba_panel --fetch: ' + ', '.join('%s %d' % kv for kv in sorted(c.items())))
        return 1 if c.get('failed') else 0
    if bool(a.espn_dir) != bool(a.kalshi_dir):
        return fail('--espn-dir and --kalshi-dir go together (the history source)')
    if not a.espn_dir and not a.recorder:
        return fail('a source is required: --espn-dir and --kalshi-dir, and/or --recorder (no default: '
                    'no tool reads data/ unless told to)')
    for d in (a.espn_dir, a.kalshi_dir, a.recorder):
        if d and not os.path.isdir(d):
            return fail('%s is not a directory' % d)
    if not a.out:
        return fail('--out FILE.csv is required')
    if not 0 <= a.lag_s <= 600:
        return fail('--lag-s must be 0 to 600 seconds')
    rows, counts = build(a.espn_dir, a.kalshi_dir, a.recorder, a.lag_s)
    try:
        write_panel(a.out, rows, a.force)
    except (FileExistsError, OSError) as e:
        return fail(str(e))
    print(json.dumps({'rows': len(rows), 'out': a.out, 'counts': counts}, indent=1) if a.json
          else report(rows, counts, a.out))
    return 0


# ---- self-test ----------------------------------------------------------------------------------
def _iso(ms):
    return datetime.datetime.fromtimestamp(ms / 1000, tz=datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def _team(ha, tid, abbr, name, score):
    return {'homeAway': ha, 'score': str(score), 'team': {'id': tid, 'abbreviation': abbr, 'displayName': name}}


def _event(gid, start_ms, state, away, home):
    return {'id': gid, 'date': _iso(start_ms)[:16] + 'Z',
            'competitions': [{'status': {'period': 4, 'displayClock': '0:00',
                                         'type': {'state': state, 'shortDetail': 'Final'}},
                              'competitors': [away, home]}]}


def _play(pid, seq, period, clock, wall_ms, hs, as_):
    return {'id': pid, 'sequenceNumber': str(seq), 'period': {'number': period}, 'clock': {'displayValue': clock},
            'wallclock': _iso(wall_ms) if wall_ms else None, 'homeScore': hs, 'awayScore': as_}


def _candle(end_s, bid_c, ask_c, dollars_form=False):
    if dollars_form:
        return {'end_period_ts': end_s, 'yes_bid': {'close_dollars': '%.4f' % (bid_c / 100)},
                'yes_ask': {'close_dollars': '%.4f' % (ask_c / 100)}}
    return {'end_period_ts': end_s, 'yes_bid': {'close': bid_c}, 'yes_ask': {'close': ask_c}}


def self_test():
    fails = []

    def check(name, ok, detail=''):
        print('%s %s%s' % ('OK  ' if ok else 'FAIL', name, (': ' + str(detail)) if detail else ''))
        if not ok:
            fails.append(name)

    # the small rules
    check('season_of: August on is the next season (ET dates)',
          [season_of(em.iso_ms(t)) for t in ('2026-10-22T23:30Z', '2026-06-10T00:30Z', '2026-08-01T04:30Z',
                                              '2026-08-01T03:30Z')] == ['2026-27', '2025-26', '2026-27', '2025-26'])
    check('effective: max(home bid, 1 - away ask), min(home ask, 1 - away bid), either book alone',
          effective((5500, 6000), (4200, 4700)) == (5500, 5800) and effective(None, (4200, 4700)) == (5300, 5800)
          and effective((None, 6000), None) == (None, 6000) and effective(None, None) == (None, None))
    check('candle_px: close_dollars, else whole cents', candle_px({'close_dollars': '0.4500'}) == 4500
          and candle_px({'close': 45}) == 4500 and candle_px({'close': 0.45}) is None and candle_px(None) is None)
    q = Quotes(CANDLE_AGE_MS)
    q.add('X', 1000, (4000, 4200))
    q.add('X', 61000, (4100, 4300))
    check('Quotes: the entry at or before t, never later, carried 120 s at most',
          q.at('X', 999) is None and q.at('X', 1000) == (4000, 4200) and q.at('X', 60999) == (4000, 4200)
          and q.at('X', 181000) == (4100, 4300) and q.at('X', 181001) is None)

    # ---- history: two scoreboards' games, one game's summary and both its markets' candles
    s = 1000
    tip = em.iso_ms('2026-11-03T00:00Z')        # 7 pm ET on Nov 2: ET date 2026-11-02
    lag = 41.5
    W = tip + 120 * s                            # the jump ball's wallclock
    plays = [_play('p1', 1, 1, '12:00', W, 0, 0),
             _play('p2', 2, 1, '11:40', W + 20 * s, 2, 0),        # home scores
             _play('p3', 3, 1, '11:20', W + 20 * s, 2, 0),        # same wallclock: same instant
             _play('p4', 4, 1, '11:05', W + 10 * s, 2, 3),        # under the running max: late, dropped
             _play('p5', 5, 1, '10:50', W + 50 * s, 2, 3),        # carries p4's away three
             _play('p6', 6, 1, '10:40', None, 2, 3),              # no wallclock: unread
             _play('p6b', 7, 1, '10:20', W + 80 * s, 2, 3),       # the clock alone moved
             _play('p7', 8, 3, '4:12', W + 3000 * s, 60, 64),
             _play('p8', 9, 5, '3:00', W + 9000 * s, 110, 110),   # overtime: its own clock
             _play('p9', 10, 4, '13:00', W + 9010 * s, 110, 110)]  # a clock past the period: no state
    summary = {'plays': plays, 'winprobability': [{'playId': 'p1', 'homeWinPercentage': 0.6},
                                                   {'playId': 'p5', 'homeWinPercentage': 0.55},
                                                   {'playId': 'p4', 'homeWinPercentage': 0.1}]}
    # Kalshi: BOS (home) and LAL; ticker order puts BOS first, so market a is home
    ev1, ev2 = 'KXNBAGAME-26NOV02LALBOS', 'KXNBAGAME-26NOV02MIANYK'
    H1, A1 = ev1 + '-BOS', ev1 + '-LAL'
    markets = [{'ticker': H1, 'event_ticker': ev1, 'yes_sub_title': 'Boston', 'result': 'no'},
               {'ticker': A1, 'event_ticker': ev1, 'yes_sub_title': 'Los Angeles L', 'result': 'yes'},
               {'ticker': ev2 + '-MIA', 'event_ticker': ev2, 'yes_sub_title': 'Miami', 'result': 'yes'},
               {'ticker': ev2 + '-NYK', 'event_ticker': ev2, 'yes_sub_title': 'New York K', 'result': 'no'}]
    tip_s = tip // 1000
    # minute closes: at the tip home 0.40/0.60 (20c wide: no capture); a minute before 0.55/0.60 with
    # LAL 0.42/0.47 (effective 0.55/0.58, 3c: p0 0.565); in play, closes every minute from the tip
    hc = [_candle(tip_s - 120, 50, 70), _candle(tip_s - 60, 55, 60), _candle(tip_s, 40, 60)]
    ac = [_candle(tip_s - 120, 30, 50), _candle(tip_s - 60, 42, 47), _candle(tip_s, 40, 60)]
    for i in range(1, 100):
        hc.append(_candle(tip_s + 60 * i, 50 + i % 3, 53 + i % 3, i % 2 == 0))
        ac.append(_candle(tip_s + 60 * i, 46 - i % 3, 49 - i % 3))
    with tempfile.TemporaryDirectory() as d:
        ed, kd = os.path.join(d, 'espn'), os.path.join(d, 'kalshi')
        write_json(os.path.join(ed, 'scoreboard-20261102.json'), {'events': [
            _event('401900001', tip, 'post', _team('away', '13', 'LAL', 'Los Angeles Lakers', 121),
                   _team('home', '2', 'BOS', 'Boston Celtics', 119)),
            _event('401900002', tip + 30 * 60 * s, 'post', _team('away', '18', 'NY', 'New York Knicks', 99),
                   _team('home', '14', 'MIA', 'Miami Heat', 101))]})
        write_json(os.path.join(ed, 'summary-401900001.json'), summary)
        write_json(os.path.join(kd, 'markets-a.json'), {'markets': markets})
        write_json(os.path.join(kd, 'candles', H1 + '.json'), {'ticker': H1, 'candlesticks': hc})
        write_json(os.path.join(kd, 'candles', A1 + '.json'), {'ticker': A1, 'candlesticks': ac})
        rows, counts = build(ed, kd, None, lag)
        h = counts['history']
        check('history: 2 events matched, the second without a summary', (h.get('events'), h.get('no_summary'),
              h.get('unmatched', 0), h.get('games')) == (2, 1, 0, 1), h)
        check('history: one late play dropped, one unread, one clock past the period, one same instant',
              (h.get('late_plays'), h.get('unread_plays'), h.get('no_clock'), h.get('same_instant')) == (1, 1, 1, 1), h)
        vis = [int(r['visible_ms']) for r in rows]
        check('visible at the running-max wallclock + 41.5 s', vis == [W + 41500, W + 61500, W + 91500, W + 121500,
                                                                       W + 3041500, W + 9041500], [v - W for v in vis])
        r1, r2, r3, r4, r7, r8 = rows if len(rows) == 6 else [{}] * 6
        check('home side: margin, yes_won from Kalshi (BOS settled no), p0 0.565 from the minute before the tip',
              (r2.get('margin'), r3.get('margin'), r1.get('yes_won'), r1.get('p0')) == ('2', '-1', '0', '0.56500'),
              (r2.get('margin'), r3.get('margin'), r1.get('yes_won'), r1.get('p0')))
        check('the clock: Q1 12:00 is 2880, Q3 4:12 is 972, OT 3:00 is 180 in overtime',
              [(r.get('t_left_s'), r.get('ot')) for r in (r1, r7, r8)] == [('2880', '0'), ('972', '0'), ('180', '1')])
        check('p3 replaced p2 at the same instant (11:20 shown)', r2.get('clock') == '11:20', r2.get('clock'))
        check("the late play's chart point is never used; the shown ones carry forward",
              [r.get('espn_wp') for r in rows] == ['0.6', '0.6', '0.55', '0.55', '0.55', '0.55'])
        # r1 at tip+161.5 s reads the close at tip+120 (i=2: home 0.52/0.55, LAL 0.44/0.47; effective
        # bid max(0.52, 0.53) = 0.53, ask min(0.55, 0.56) = 0.55); a minute on the close at tip+180
        # (i=3: home 0.50/0.53, LAL 0.46/0.49)
        check('the effective book and each touch at visible_ms, and a minute on',
              (r1.get('kalshi_bid'), r1.get('kalshi_ask'), r1.get('kalshi_mid'), r1.get('home_bid'), r1.get('away_ask'),
               r1.get('home_ask_next'), r1.get('away_bid_next')) == ('0.5300', '0.5500', '0.54000', '0.5200', '0.4700',
                                                                     '0.5300', '0.4600'),
              [r1.get(k) for k in ('kalshi_bid', 'kalshi_ask', 'kalshi_mid', 'home_bid', 'away_ask', 'home_ask_next',
                                   'away_bid_next')])
        check('seen_ms: the score state\'s first sighting, the clock alone does not move it',
              (r1.get('seen_ms'), r2.get('seen_ms'), r3.get('seen_ms'), r4.get('seen_ms'), r4.get('clock'))
              == (str(W + 41500), str(W + 61500), str(W + 91500), str(W + 91500), '10:20'))
        check('a quote is never read past 120 s: the overtime row has no book (candles end at tip + 99 min)',
              (r8.get('kalshi_bid'), r8.get('home_bid'), r8.get('kalshi_mid')) == ('', '', ''),
              (r8.get('kalshi_bid'), r8.get('home_bid'), r8.get('kalshi_mid')))
        check('the columns: source history, lag 41.5, event and tickers, the season',
              (r1.get('source'), r1.get('lag_s'), r1.get('event'), r1.get('home_ticker'), r1.get('away_ticker'),
               r1.get('season'), r1.get('game_id')) == ('history', '41.5', ev1, H1, A1, '2026-27', '401900001'),
              [r1.get(k) for k in ('source', 'lag_s', 'season', 'game_id')])
        out = os.path.join(d, 'panel.csv')
        check('the CLI writes the panel', run(['--espn-dir', ed, '--kalshi-dir', kd, '--out', out]) == 0)
        try:
            prow, has_mid = tb.read_panel(out, 'nba')
            ok = len(prow) == 6 and has_mid and prow[5]['ot'] and prow[5]['t_left_s'] == 180.0 \
                and prow[0]['p0'] == 0.565 and prow[5]['kalshi_mid'] is None and prow[0]['espn_wp'] == 0.6
        except tb.PanelError as e:
            ok, prow = False, str(e)
        check('train_bball.read_panel reads it as an NBA panel (the trainer\'s own checks)', ok, prow if not ok else '')
        hist = ['--espn-dir', ed, '--kalshi-dir', kd]
        check('an existing --out is refused without --force', run(hist + ['--out', out]) == 2
              and run(hist + ['--out', out, '--force']) == 0)
        check('no default source, no default --out', run(['--out', out]) == 2 and run(hist) == 2
              and run(['--espn-dir', ed, '--out', out]) == 2)
        rows60, _ = build(ed, kd, None, 60)
        check('--lag-s moves every visible time', [int(r['visible_ms']) - W for r in rows60][:2] == [60000, 80000])
        # the second game's market a (MIA) is ESPN's home team: matched the other way round, and its
        # summary now present but no candles
        write_json(os.path.join(ed, 'summary-401900002.json'),
                   {'plays': [_play('q1', 1, 1, '12:00', tip + 1900 * s, 0, 0)]})
        _, c2 = build(ed, kd, None, lag)
        check('a game without candles is counted, not guessed', c2['history'].get('no_candles') == 1, c2['history'])

        # ---- the recorder: the same night, recorded (BOS-LAL), plus a game the history lacks
        rd = os.path.join(d, 'rec')
        sp = lj.Spool(rd)
        us = 1000
        for tk, ev, name, res in ((H1, ev1, 'Boston', 'no'), (A1, ev1, 'Los Angeles L', 'yes')):
            sp.write('kalshi_markets', {'recv_us': (tip - 3600 * s) * us, 'series': SERIES, 'ticker': tk,
                                        'status': 'settled', 'result': res,
                                        'raw': json.dumps({'event_ticker': ev, 'yes_sub_title': name})})
        t0 = (tip - 600 * s) * us
        sp.snapshot(t0, 1, 1, 1, H1, [(0.55, 50)], [(0.38, 40)])        # BOS 0.55 / 0.62 (NO bid 0.38)
        sp.snapshot(t0 + 1, 1, 1, 2, A1, [(0.40, 50)], [(0.56, 40)])    # LAL 0.40 / 0.44
        sp.delta((tip - 1 * s) * us, 1, 1, 3, H1, 'yes', 0.30, 10)      # deeper, same touch: no new entry
        sp.delta((tip + 100 * s) * us, 1, 1, 4, H1, 'yes', 0.58, 10)    # BOS bid 0.58 from tip + 100 s
        sp.delta((tip + 170 * s) * us, 1, 1, 5, A1, 'no', 0.56, -40)    # LAL ask gone at tip + 170 s
        sp.frame((tip + 400 * s) * us, 1, 'ok', 1, 6, {})               # the last row before the disconnect
        sp.write('gaps', {'recv_us': (tip + 500 * s) * us, 'source': 'kalshi_ws', 'kind': 'disconnect', 'detail': 't',
                          'from_us': (tip + 400 * s) * us, 'to_us': None})
        sp.frame((tip + 600 * s) * us, 2, 'ok', 1, 1, {})

        def score(t_s, state, period, clock, hs, as_, extra=None):
            raw = {'competitors': [
                {'homeAway': 'home', 'team': {'id': '2', 'abbreviation': 'BOS', 'displayName': 'Boston Celtics'}},
                {'homeAway': 'away', 'team': {'id': '13', 'abbreviation': 'LAL', 'displayName': 'Los Angeles Lakers'}}]}
            raw.update(extra or {})
            sp.write('espn_scores', {'recv_us': (tip + t_s * s) * us, 'prev_us': None, 'league': 'basketball/nba',
                                     'game_id': '401900001', 'start_time': _iso(tip)[:16] + 'Z', 'state': state,
                                     'status': None, 'period': str(period), 'clock': clock, 'home': 'BOS',
                                     'away': 'LAL', 'home_score': hs, 'away_score': as_, 'detail': None,
                                     'raw': json.dumps(raw)})
        score(-900, 'pre', 0, '0:00', 0, 0)
        score(95, 'in', 1, '11:50', 0, 0,
              {'situation': {'possession': '13', 'lastPlay': {'probability': {'homeWinPercentage': 0.52}}}})
        score(150, 'in', 1, '11:20', 2, 0, {'situation': {'possession': '2'}})
        score(450, 'in', 1, '9:00', 2, 2)
        score(9000, 'post', 4, '0:00', 119, 121)

        def play(t_s, pid, seq, wp):
            sp.write('espn_plays', {'recv_us': (tip + t_s * s) * us, 'prev_us': None, 'league': 'basketball/nba',
                                    'game_id': '401900001', 'play_id': pid, 'seq': seq, 'period': 1, 'clock': '11:30',
                                    'wallclock': None, 'home_score': 2, 'away_score': 0, 'team_id': '2',
                                    'scoring': True, 'score_value': 2, 'type': 'x', 'text': 'x', 'wp_home': wp,
                                    'wp_tie': 0.0, 'revised': False, 'raw': '{}'})
        play(140, 'r1', 10, 0.58)
        play(160, 'r0', 9, 0.9)      # first seen after seq 10: inserted late, dropped
        rows, counts = build(None, None, rd)
        rc = counts['recorder']
        check('recorder: one game, three rows in play, one late play',
              (rc.get('games'), rc.get('rows'), rc.get('late_plays')) == (1, 3, 1), rc)
        a, b, c = rows if len(rows) == 3 else ({}, {}, {})
        check('visible at recv_us; source recorder; no lag', (a.get('visible_ms'), a.get('source'), a.get('lag_s'))
              == (str(tip + 95 * s), 'recorder', ''), (a.get('visible_ms'), a.get('source'), a.get('lag_s')))
        # p0: BOS 0.55/0.62 with LAL 0.40/0.44: effective 0.56/0.60, 4c: mid 0.58
        check('recorder p0 from the books before the tip; yes_won from Kalshi', (a.get('p0'), a.get('yes_won'))
              == ('0.58000', '0'), (a.get('p0'), a.get('yes_won')))
        check('possession from situation.possession (13 is LAL, away; 2 BOS)',
              (a.get('possession'), b.get('possession'), c.get('possession')) == ('-1', '1', '0'))
        check("wp: the scoreboard's before any chart point, then the first-seen chart point; the late one never",
              (a.get('espn_wp'), b.get('espn_wp'), c.get('espn_wp')) == ('0.52', '0.58', '0.58'))
        # a (tip + 95 s): BOS 0.55/0.62, LAL 0.40/0.44 -> effective 0.56/0.60; next (tip + 155 s): BOS bid 0.58
        # b (tip + 150 s): BOS 0.58/0.62 -> effective 0.58/0.60; next (tip + 210 s): LAL ask gone
        check('recorder touches now and a minute on', (a.get('home_bid'), a.get('kalshi_bid'), a.get('kalshi_ask'),
              a.get('home_bid_next'), b.get('kalshi_bid'), b.get('kalshi_ask'), b.get('away_ask_next')) ==
              ('0.5500', '0.5600', '0.6000', '0.5800', '0.5800', '0.6000', ''),
              (a.get('home_bid'), a.get('kalshi_bid'), a.get('kalshi_ask'), a.get('home_bid_next'), b.get('kalshi_bid'),
               b.get('kalshi_ask'), b.get('away_ask_next')))
        check('after the disconnect no book: blank until a snapshot',
              (c.get('home_bid'), c.get('kalshi_mid')) == ('', ''), (c.get('home_bid'), c.get('kalshi_mid')))
        rows, counts = build(ed, kd, rd, lag)
        srcs = Counter(r['source'] for r in rows if r['game_id'] == '401900001')
        check('a game in both sources keeps its recorder rows', srcs == Counter(recorder=3)
              and counts['history'].get('recorded_instead') == 1, (srcs, counts['history']))

        # ---- --fetch, against a fake: the URLs, the files, the pace
        calls, clock = [], [0.0]

        def fake(url):
            calls.append((clock[0], url))
            if '/scoreboard?' in url:
                return {'events': [_event('401900001', tip, 'post',
                                          _team('away', '13', 'LAL', 'Los Angeles Lakers', 121),
                                          _team('home', '2', 'BOS', 'Boston Celtics', 119))]}
            if '/summary?' in url:
                return summary
            if '/markets?' in url:   # two pages: the cursor's is the second
                if 'cursor=' in url:
                    return {'markets': markets[:2], 'cursor': ''}
                return {'markets': markets[2:], 'cursor': 'c2'}
            if '/series/' in url and A1 in url:
                raise urllib.error.HTTPError(url, 404, 'not found', None, None)
            return {'candlesticks': hc}

        def sleep(x):
            clock[0] += x
        fd = os.path.join(d, 'fetch')
        c = fetch(os.path.join(fd, 'e'), os.path.join(fd, 'k'), datetime.date(2026, 11, 2), datetime.date(2026, 11, 2),
                  Paced(fake, lambda: clock[0], sleep), log=lambda m: None)
        urls = [u for _, u in calls]
        check('fetch: the scoreboard, the summary, two market pages, the candles (a 404 tries the historical path)',
              len(urls) == 7 and urls[0] == ESPN_BASE + '/basketball/nba/scoreboard?dates=20261102&limit=500'
              and urls[1].endswith('/summary?event=401900001') and 'cursor=c2' in urls[3]
              and urls[4] == candles_url(H1, tip_s - 13 * 3600, tip_s + 5 * 3600)
              and urls[6] == candles_url(A1, tip_s - 13 * 3600, tip_s + 5 * 3600, historical=True), len(urls))
        check('fetch: at most 3 requests a second',
              all(b - a >= FETCH_EVERY_S - 1e-9 for (a, _), (b, _) in zip(calls, calls[1:])),
              [round(t, 3) for t, _ in calls])
        again = fetch(os.path.join(fd, 'e'), os.path.join(fd, 'k'), datetime.date(2026, 11, 2),
                      datetime.date(2026, 11, 2), Paced(fake, lambda: clock[0], sleep), log=lambda m: None)
        check('fetch: the files, then nothing again', c.get('written') == 5 and again.get('written') is None
              and len(calls) == 7 and os.path.exists(os.path.join(fd, 'k', 'candles', A1 + '.json')), (c, again))
        refused = sys.platform == 'darwin' or run(['--fetch', '--from', '2026-11-02', '--to', '2026-11-02'] + hist) == 2
        check("--fetch refuses anywhere but the operator's Mac",
              fetch_refusal('linux') and fetch_refusal('darwin') is None and refused)
    print('nba_panel self-test: %s' % ('ok' if not fails else 'FAILED (%d)' % len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(run(sys.argv[1:]))
