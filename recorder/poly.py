"""Polymarket US books, public data only (plan section 7: "Polymarket books for paired games and
weather every 2-5 s with the Age header"): the evidence for routing and the two-venue weather gate.

Off unless configured. REC_PM_LEAGUES (league slugs: every game-winner market of those leagues,
the games Kalshi lists too, around each game) and REC_PM_SLUGS (market slugs named one by one)
are both empty by default, and with both empty nothing is built and no request leaves. Weather
only once the operator has confirmed Polymarket US lists it (docs/polymarket.md, 2026-09-08: it
does not); its slugs then go in REC_PM_SLUGS. Pairing a Polymarket game with Kalshi's is done
afterwards, from pm_markets (teams, start) and kalshi_markets; the recorder only records.

Only Polymarket's public market-data host, gateway.polymarket.us: GETs with no key, no signature
and no X-PM-* header, never api.polymarket.us (config.py refuses every other host but loopback).

    GET {base}/v2/leagues/{league}/events?limit=200&offset=N   the league's events and markets, every REC_PM_LIST_S
    GET {base}/v1/markets/{slug}/book                          a market's book, every REC_PM_BOOK_S while listed

Every book read names its slug: the list is explicit, and an empty one reads nothing. A book is
written when it differs from the last one recorded for its slug (pm_books: recv_us first seen,
sent_us when that read left, prev_us the read before, polls how many reads saw the previous one)
with the response's Age and Date headers: a cached answer is Age seconds old, so the book held
from Date - Age at the latest. Failures are gaps (sources pm_listing, pm_books), never a restart.
"""
import asyncio
import json
import time
import urllib.error
import urllib.parse
import urllib.request
from email.utils import parsedate_to_datetime

from .games import ts_s
from .kalshi import UA, HttpError
from .polls import Outage, RateLimiter, short
from .store import now_us

PUBLIC_HOST = 'gateway.polymarket.us'
PAGE = 200            # events per listing page
MAX_PAGES = 5
LIST_RETRY_S = 60     # a listing that failed is tried again this soon
MISSING_EVERY_S = 3600


def amount(v):
    """A price as Polymarket sends it: {"value": "0.385", "currency": "USD"}, "0.385" or a number;
    None otherwise."""
    if isinstance(v, dict):
        v = v.get('value')
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


def book_levels(body):
    """A /book answer's (bids, asks) as [(price, qty)], best first; None when it is not a book."""
    md = body.get('marketData', body) if isinstance(body, dict) else None
    if not isinstance(md, dict) or not all(isinstance(md.get(side) or [], list) for side in ('bids', 'asks')):
        return None
    out = []
    for side in ('bids', 'asks'):
        levels = []
        for lv in md.get(side) or []:
            px, q = (amount(lv.get('px')), amount(lv.get('qty'))) if isinstance(lv, dict) else (None, None)
            if px is not None and q is not None:
                levels.append((px, q))
        levels.sort(key=lambda x: -x[0] if side == 'bids' else x[0])
        out.append(levels)
    return out[0], out[1]


def is_winner(m):
    """A game-winner market (the kind Kalshi lists): the old desk's filter (src/poly.cpp)."""
    t = m.get('sportsMarketType') if isinstance(m, dict) else None
    return isinstance(t, str) and (t == 'soccer_team_full_time_winner' or '_full_game_winner' in t)


def _text(m, k):
    v = m.get(k)
    return None if v is None or v == '' else str(v)


def league_markets(events, league):
    """The game-winner markets of a /v2/leagues/{league}/events page's events, as pm_markets rows
    without recv_us. A quote changes every second, so `digest` leaves the quotes out."""
    out = []
    for ev in events if isinstance(events, list) else []:
        if not isinstance(ev, dict):
            continue
        for m in ev.get('markets') or []:
            if not is_winner(m) or not _text(m, 'slug'):
                continue
            start = _text(m, 'gameStartTime') or _text(m, 'startDate')
            row = {'league': league, 'slug': m['slug'], 'event_slug': _text(ev, 'slug'),
                   'event_title': _text(ev, 'title'), 'question': _text(m, 'question'),
                   'sports_type': _text(m, 'sportsMarketType'), 'start_time': start,
                   'end_time': _text(m, 'endDate'),
                   'active': m['active'] if isinstance(m.get('active'), bool) else None,
                   'closed': m['closed'] if isinstance(m.get('closed'), bool) else None,
                   'raw': json.dumps(m, separators=(',', ':'), sort_keys=True)}
            row['digest'] = json.dumps([row[k] for k in ('event_slug', 'event_title', 'question', 'start_time',
                                                         'end_time', 'active', 'closed')] + [m.get('marketSides')],
                                       separators=(',', ':'), sort_keys=True, default=str)
            out.append(row)
    return out


def wanted_market(m, now_s, pre_s, game_s):
    """Record a listed game's book now: not closed, from pre_s before its start to game_s after
    it. A game with no start time is not placed in time, so not recorded (REC_PM_SLUGS can name it)."""
    start = ts_s(m.get('start_time'))
    if m.get('closed') or start is None:
        return False
    return start - pre_s <= now_s <= start + game_s


def header_us(date):
    """An HTTP Date header as epoch microseconds; None when absent or unreadable."""
    try:
        return int(parsedate_to_datetime(date).timestamp() * 1_000_000) if date else None
    except (TypeError, ValueError, IndexError):
        return None


def header_s(age):
    try:
        return float(age) if age not in (None, '') else None
    except ValueError:
        return None


class Http:
    """Blocking public GETs (run them in a thread): the JSON body and the response headers. No
    key, no signature, nothing but Accept and User-Agent."""

    def __init__(self, base_url, timeout=10):
        self.base = base_url.rstrip('/')
        self.timeout = timeout

    def get(self, path, params=None):
        url = self.base + path + ('?' + urllib.parse.urlencode(params) if params else '')
        req = urllib.request.Request(url, method='GET', headers={'Accept': 'application/json', 'User-Agent': UA})
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return json.loads(r.read()), {'Age': r.headers.get('Age'), 'Date': r.headers.get('Date')}
        except urllib.error.HTTPError as e:
            raise HttpError(e.code, e.read().decode('utf-8', 'replace')) from None


class PolyBooks:
    """The listing of the configured leagues every list_s (a failed league keeps its markets), and
    every cycle the book of each slug on the list: REC_PM_SLUGS, and each listed game-winner market
    from pre_s before its start to game_s after it, until the listing shows it closed. One request
    at a time, under the limiter: a cycle with more slugs than the budget covers in book_s takes
    longer, never faster."""

    def __init__(self, http, store, gap, leagues=(), slugs=(), book_s=3.0, list_s=600.0, pre_s=7200,
                 game_s=21600, rps=5, clock=time.time):
        self.http = http
        self.store = store
        self.gap = gap
        self.leagues = list(leagues)
        self.slugs = list(slugs)
        self.book_s = book_s
        self.list_s = list_s
        self.pre_s = pre_s
        self.game_s = game_s
        self.limiter = RateLimiter(rps)
        self.clock = clock
        self.markets = {}      # slug -> its pm_markets row (the leagues' listing)
        self.digests = {}      # slug -> the pm_markets digest last written
        self.last = {}         # slug -> [levels last recorded, the last read's recv_us, reads that saw them]
        self.next_list = 0.0
        self.rows = 0
        self.list_outage = Outage(gap, 'pm_listing')
        self.book_outage = Outage(gap, 'pm_books')

    async def get(self, path, params=None):
        await self.limiter.take()
        return await asyncio.to_thread(self.http.get, path, params)

    # ---- the listing ---------------------------------------------------------------------------
    async def list_league(self, league):
        out = []
        for page in range(MAX_PAGES):
            body, _ = await self.get('/v2/leagues/%s/events' % urllib.parse.quote(league, safe=''),
                                     {'limit': PAGE, 'offset': page * PAGE})
            events = body.get('events') if isinstance(body, dict) else None
            events = events if isinstance(events, list) else []
            out += league_markets(events, league)
            if len(events) < PAGE:
                break
        return out

    async def listing(self):
        """Every league listed; each market whose listing changed written to pm_markets; a market
        gone from its league's listing forgotten. True when every league was listed."""
        failed = None
        for league in self.leagues:
            try:
                found = await self.list_league(league)
            except Exception as e:   # this league keeps its markets until a listing succeeds
                failed = failed or '%s: %s' % (league, short(e))
                continue
            recv = now_us()
            slugs = {m['slug'] for m in found}
            for m in found:
                self.markets[m['slug']] = m
                if self.digests.get(m['slug']) != m['digest']:
                    self.digests[m['slug']] = m['digest']
                    self.store.write('pm_markets', dict({k: v for k, v in m.items() if k != 'digest'}, recv_us=recv))
            for s in [s for s, m in self.markets.items() if m['league'] == league and s not in slugs]:
                del self.markets[s]
                self.digests.pop(s, None)
        if failed:
            self.list_outage.failed(failed, now_us())
        else:
            self.list_outage.ok(now_us())
        return failed is None

    def listed(self, now_s):
        """This cycle's slugs, {slug: (mode, league)}: the named ones ('listed') and the listed games
        inside their window ('game')."""
        out = {s: ('listed', None) for s in self.slugs}
        for s, m in self.markets.items():
            if s not in out and wanted_market(m, now_s, self.pre_s, self.game_s):
                out[s] = ('game', m['league'])
        return out

    # ---- the books -----------------------------------------------------------------------------
    async def book(self, slug, mode, league):
        sent = now_us()
        try:
            body, headers = await self.get('/v1/markets/%s/book' % urllib.parse.quote(slug, safe=''))
        except HttpError as e:
            if e.status != 404:
                raise
            # no such market (a named slug mistyped, or gone): said once an hour, not an outage
            self.gap('pm_books', 'not_found', '%s: 404' % slug, key=('pm_404', slug), every_s=MISSING_EVERY_S)
            return
        recv = now_us()
        levels = book_levels(body)
        if levels is None:
            raise ValueError('%s: not a book' % slug)
        bids, asks = levels
        prev = self.last.get(slug)
        if prev is not None and prev[0] == levels:   # the levels, not raw: a time in it is no change
            prev[1] = recv
            prev[2] += 1
            return
        raw = json.dumps(body.get('marketData', body), separators=(',', ':'), sort_keys=True)
        self.store.write('pm_books', {
            'recv_us': recv, 'sent_us': sent, 'prev_us': prev[1] if prev else None, 'polls': prev[2] if prev else 0,
            'slug': slug, 'league': league, 'mode': mode,
            'bid': bids[0][0] if bids else None, 'bid_size': bids[0][1] if bids else None,
            'ask': asks[0][0] if asks else None, 'ask_size': asks[0][1] if asks else None,
            'age_s': header_s(headers.get('Age')), 'date_us': header_us(headers.get('Date')), 'raw': raw})
        self.last[slug] = [levels, recv, 1]
        self.rows += 1

    async def cycle(self):
        now = self.clock()
        if self.leagues and now >= self.next_list:
            self.next_list = now + (self.list_s if await self.listing() else LIST_RETRY_S)
        listed = self.listed(now)
        for s in [s for s in self.last if s not in listed]:   # off the list: its next row starts fresh
            del self.last[s]
        failed = None
        for slug in sorted(listed):
            try:
                await self.book(slug, *listed[slug])
            except Exception as e:   # HTTP errors, timeouts, a body that is not a book
                failed = failed or '%s: %s' % (slug, short(e))
        if failed:
            self.book_outage.failed(failed, now_us())
        else:
            self.book_outage.ok(now_us())

    async def run(self, stop, sleep):
        while not stop.is_set():
            began = self.clock()
            await self.cycle()
            await sleep(max(0.0, began + self.book_s - self.clock()))


def from_settings(s, store, gap):
    """The recorder's PolyBooks, or None when neither REC_PM_LEAGUES nor REC_PM_SLUGS names anything."""
    if not s.pm_leagues and not s.pm_slugs:
        return None
    return PolyBooks(Http(s.pm_url), store, gap, s.pm_leagues, s.pm_slugs, s.pm_book_s, s.pm_list_s, s.game_pre_s,
                     s.pm_game_s, s.pm_rps)
