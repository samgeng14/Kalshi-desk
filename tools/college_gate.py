#!/usr/bin/env python3
"""C1's gate: do thin college games' stale quotes pay when taken exactly as the lane takes them?

Plan 5(d) records women's basketball and volleyball (and the thin men's decile as a control) and
then measures "what was knowable then: how often the best price sits 20c+ from a live fair, for
how long, at what size, and 60 s and 300 s markouts net of the fee", gated on "50+ cases with a
positive net markout". This tool reads the recorder's data and measures those cases with exactly
the rule C1 trades: tools/take_rule.py's decide(), deskd's take_decide in Python, asserted on the
same hand-worked cases (tests/deskd/fixtures/college_take_cases.json) as the C++ (plan 5(e): "the
S2 mistake was testing one order type and trading another").

This part reads the data; each reader keeps what deskd would have known and when.

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

Times are microseconds as the recorder writes them (recv_us); decide() takes ms. Prices are in
1/10,000 $ and sizes in 1/100 contract (deskd's Px and Qty, lip_join's UNIT and CT).

Input: the recorder's data directory (<data>/spool/<table>/*.jsonl, and <data>/parquet/<table>/**
when pyarrow is installed). Read-only. --data is required: there is no default (decisions 11).

Usage:
  python3 tools/college_gate.py --data DIR [--series S,..] [--json]
  python3 tools/college_gate.py --self-test
"""
import argparse
import bisect
import contextlib
import io
import json
import os
import sys
import tempfile
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import espn_match as em   # noqa: E402  (deskd's ESPN matching)
import lip_join as lj     # noqa: E402  (the reader and the book replay)
import take_rule as tr    # noqa: E402  (deskd's take_decide)
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
        by_league.setdefault((src, league_path(league) if src == 'ESPN' else league), {}).setdefault(d, []).append((g, key))
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
        prev = self.last()
        bs = (prev[4] if prev is not None and bid == prev[0] else t) if bid is not None else 0
        as_ = (prev[5] if prev is not None and ask == prev[2] else t) if ask is not None else 0
        top = (bid, bid_qty, ask, ask_qty, bs, as_)
        if prev is not None and top == prev:
            return
        if self.times and self.times[-1] == t:   # two changes at one instant: the later stands
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


# ---- the summary --------------------------------------------------------------------------------
def survey(data, series):
    """What the readers found, per series: events, two-market events, matched by ESPN and NCAA,
    markets with a book (socket or REST), settled."""
    gms = games(data, series)
    scores, outages = score_rows(data)
    matched = match_games(gms, scores)
    hist, _ = book_history(data, [tk for g in gms.values() for tk in g['tickers']])
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
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.data or not os.path.isdir(a.data):
        print("error: --data DIR (the recorder's data directory) is required", file=sys.stderr)
        return 2
    series = tuple(s.strip() for s in a.series.split(',') if s.strip()) if a.series else SERIES
    res = survey(a.data, series)
    if a.json:
        print(json.dumps(res, indent=1))
    else:
        print('%-16s %6s %6s %6s %6s %8s %10s %7s' % ('series', 'events', 'games', 'espn', 'ncaa', 'books_ws',
                                                    'books_rest', 'settled'))
        for ser, s in sorted(res['series'].items()):
            print('%-16s %6d %6d %6d %6d %8d %10d %7d' % (ser, s['events'], s['games'], s['espn'], s['ncaa'],
                                                       s['books_ws'], s['books_rest'], s['settled']))
        print('score outages: %s' % ', '.join('%s %d' % kv for kv in sorted(res['outages'].items())))
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
                c = {'homeAway': side, 'score': str(sc), 'team': {'id': abbr, 'abbreviation': abbr, 'displayName': full}}
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
        self.write('kalshi_trades_rest', {'recv_us': t_us, 'trade_id': tid, 'ticker': tk, 'created_time': rfc(created_us),
                                          'created_us': created_us, 'yes_price': 0.5, 'no_price': 0.5,
                                          'count': float(count), 'taker_side': 'yes', 'reason': 'hour', 'raw': '{}'})


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
              gms.get(vb, {}).get('tickers') == [vb + '-CCC', vb + '-DDD'] and len(gms.get(tri, {}).get('tickers', [])) == 3
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
              m.get(vb) == {'ESPN': (('ESPN', 'volleyball/womens-college-volleyball', '901'), True)} and tri not in m, m)

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
              hb is not None and hb.src == 'rest' and hb.at(T0 + 5 * s) == (4400, 600, 6000, 300, T0 + 5 * s, T0 + 5 * s)
              and hb.at(T0 + 15 * s) == (4400, 900, 5800, 300, T0 + 5 * s, T0 + 15 * s)
              and hb.at(T0 + 45 * s) == (None, None, 5800, 300, 0, T0 + 15 * s)
              and hb.at(T0 + 65 * s) is None and hb.at(T0 + 68 * s) == (None, None, 5800, 300, 0, T0 + 68 * s),
              hb and list(zip(hb.times, hb.tops)))
        check('volumes: socket prints and the backfill, each trade_id once; REST volume otherwise',
              vols[WB_A].at(T0 + 24 * s) == 0 and vols[WB_A].at(T0 + 60 * s) == 500 and vols[WB_A].at(T0 + 70 * s) == 900
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
    print('college_gate self-test: %s' % ('ok' if not fails else 'FAILED (%d)' % len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(run(sys.argv[1:]))
