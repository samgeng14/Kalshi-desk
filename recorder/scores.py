"""Live scores for the college games (plan sections 5(d) and 7): ESPN's and NCAA.com's scoreboards,
polled every few seconds while a game is on, each change of state or score written once with
the local time it was first seen (espn_scores, ncaa_scores).

First-seen is the point: ESPN runs about 41.5 s behind the venue and its own play timestamps look
ahead (plan 5(e)), so the only honest clock for "what was knowable then" is ours.

    ESPN    {base}/{sport}/{league}/scoreboard?dates=YYYYMMDD[&groups=G]&limit=500
            base https://site.api.espn.com/apis/site/v2/sports; groups=50 is NCAA Division I for
            basketball (without it ESPN lists only featured games), as deskd's ref/espn.cpp asks
    NCAA    {base}/{sport}/{division}/YYYY/MM/DD/scoreboard.json
            base https://data.ncaa.com/casablanca/scoreboard (NCAA.com's own scoreboard JSON, the
            source henrygd/ncaa-api reads): unofficial and undocumented, TO CONFIRM on the VM

URL builders and parsers are pure; the fetch is one blocking GET run in a thread.
"""
import asyncio
import json
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone

from .kalshi import UA
from .polls import Outage, short
from .store import now_us

LATE_H = 10   # until 10 UTC the previous US date's late games may still be on (Hawaii, the West)
PRE_S = 900   # a game this close to its start is polled fast, to catch the tip


def poll_days(now_s, late_h=LATE_H):
    """The dates whose scoreboards to read: today (UTC), and before late_h UTC also yesterday,
    whose evening games in the US run past midnight UTC."""
    t = datetime.fromtimestamp(now_s, tz=timezone.utc)
    days = [t.date()]
    if t.hour < late_h:
        days.insert(0, (t - timedelta(days=1)).date())
    return days


def espn_spec(spec):
    """'basketball/womens-college-basketball:50' -> ('basketball/womens-college-basketball', '50')."""
    path, _, groups = spec.partition(':')
    return path.strip('/'), groups or None


def espn_url(base, spec, day, limit=500):
    path, groups = espn_spec(spec)
    q = 'dates=%s' % day.strftime('%Y%m%d') + ('&groups=%s' % groups if groups else '') + '&limit=%d' % limit
    return '%s/%s/scoreboard?%s' % (base.rstrip('/'), path, q)


def ncaa_url(base, spec, day):
    """spec 'basketball-women/d1'."""
    return '%s/%s/%s/scoreboard.json' % (base.rstrip('/'), spec.strip('/'), day.strftime('%Y/%m/%d'))


def _int(v):
    if isinstance(v, bool):
        return None
    if isinstance(v, int):
        return v
    if isinstance(v, str) and v.strip().isdigit():
        return int(v)
    return None


def _str(v):
    return None if v is None or v == '' else str(v)


def _compact(o):
    return json.dumps(o, separators=(',', ':'), sort_keys=True)


def espn_games(sb, league):
    """An ESPN scoreboard's games. Status from the competition (the event's own as a fallback),
    teams by homeAway; raw keeps what a model reads (status, scores by period, the situation's
    possession and last play), not the logos and links around them."""
    out = []
    events = sb.get('events') if isinstance(sb, dict) else None
    for e in events if isinstance(events, list) else []:
        if not isinstance(e, dict) or e.get('id') in (None, ''):
            continue
        comps = e.get('competitions')
        c = comps[0] if isinstance(comps, list) and comps and isinstance(comps[0], dict) else {}
        status = c.get('status') if isinstance(c.get('status'), dict) else e.get('status')
        status = status if isinstance(status, dict) else {}
        typ = status.get('type') if isinstance(status.get('type'), dict) else {}
        side = {}
        for t in c.get('competitors') or []:
            if isinstance(t, dict) and t.get('homeAway') in ('home', 'away'):
                side[t['homeAway']] = t
        if set(side) != {'home', 'away'}:
            continue

        def team(s):
            tm = side[s].get('team') if isinstance(side[s].get('team'), dict) else {}
            return _str(tm.get('abbreviation')) or _str(tm.get('displayName'))

        raw = {'status': status, 'competitors': [
            {'homeAway': s, 'score': side[s].get('score'),
             'team': {k: (side[s].get('team') or {}).get(k) for k in ('id', 'abbreviation', 'displayName')},
             'linescores': [ls.get('value') for ls in side[s].get('linescores') or [] if isinstance(ls, dict)]}
            for s in ('home', 'away')]}
        sit = c.get('situation')
        if isinstance(sit, dict):
            raw['situation'] = {k: sit[k] for k in ('possession', 'lastPlay', 'homeTimeouts', 'awayTimeouts')
                                if k in sit}
        period = status.get('period')
        out.append({'league': league, 'game_id': str(e['id']), 'start_time': _str(e.get('date')),
                    'state': _str(typ.get('state')), 'status': _str(typ.get('name')),
                    'period': None if period is None else str(period), 'clock': _str(status.get('displayClock')),
                    'home': team('home'), 'away': team('away'),
                    'home_score': _int(side['home'].get('score')), 'away_score': _int(side['away'].get('score')),
                    'detail': _str(typ.get('shortDetail') or typ.get('detail')), 'raw': _compact(raw)})
    return out


NCAA_STATES = {'pre': 'pre', 'live': 'in', 'final': 'post'}


def ncaa_games(sb, league):
    """NCAA.com's scoreboard.json games ({"games": [{"game": {...}}]}): gameState pre / live /
    final read as ESPN's pre / in / post (another value is kept as it is)."""
    out = []
    games = sb.get('games') if isinstance(sb, dict) else None
    for g in games if isinstance(games, list) else []:
        game = g.get('game') if isinstance(g, dict) else None
        if not isinstance(game, dict):
            continue
        gid = _str(game.get('gameID')) or _str(game.get('url'))
        home, away = game.get('home'), game.get('away')
        if not gid or not isinstance(home, dict) or not isinstance(away, dict):
            continue

        def name(t):
            n = t.get('names') if isinstance(t.get('names'), dict) else {}
            return _str(n.get('char6')) or _str(n.get('short')) or _str(n.get('seo'))

        start = _int(game.get('startTimeEpoch'))
        state = _str(game.get('gameState'))
        out.append({'league': league, 'game_id': gid,
                    'start_time': datetime.fromtimestamp(start, tz=timezone.utc).strftime('%Y-%m-%dT%H:%MZ')
                    if start else None,
                    'state': NCAA_STATES.get((state or '').lower(), state), 'status': state,
                    'period': _str(game.get('currentPeriod')), 'clock': _str(game.get('contestClock')),
                    'home': name(home), 'away': name(away),
                    'home_score': _int(home.get('score')), 'away_score': _int(away.get('score')),
                    'detail': _str(game.get('finalMessage')),
                    'raw': _compact({k: game.get(k) for k in ('gameState', 'currentPeriod', 'contestClock',
                                                              'finalMessage', 'startTimeEpoch', 'home', 'away')})})
    return out


class ScoreBook:
    """Each game's last recorded state: a change of state, status, period, score or scheduled
    start is written once, at the poll that first showed it. The clock alone is not a change
    (it would write every poll); it is recorded with each change."""

    KEYS = ('state', 'status', 'period', 'home_score', 'away_score', 'start_time')
    KEEP_US = 2 * 86400 * 1_000_000

    def __init__(self):
        self.last = {}   # (league, game_id) -> [digest, the last poll's recv_us]

    def changes(self, games, recv_us):
        out = []
        for g in games:
            key = (g['league'], g['game_id'])
            digest = tuple(g.get(k) for k in self.KEYS)
            prev = self.last.get(key)
            if prev is None or prev[0] != digest:
                out.append(dict(g, recv_us=recv_us, prev_us=prev[1] if prev else None))
                self.last[key] = [digest, recv_us]
            else:
                prev[1] = recv_us
        for key in [k for k, v in self.last.items() if v[1] < recv_us - self.KEEP_US]:
            del self.last[key]
        return out


def live(games, now_s, pre_s=PRE_S):
    """Is a game on, or about to start (or past its start and not yet begun: a delay)?"""
    for g in games:
        if g.get('state') == 'in':
            return True
        if g.get('state') == 'pre' and g.get('start_time'):
            try:
                start = datetime.fromisoformat(g['start_time'].replace('Z', '+00:00')).timestamp()
            except ValueError:
                continue
            if start - pre_s <= now_s <= start + 3 * 3600:
                return True
    return False


def fetch_json(url, timeout=10):
    req = urllib.request.Request(url, method='GET', headers={'Accept': 'application/json', 'User-Agent': UA})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


class Feed:
    """One scoreboard: espn or ncaa, a league spec, the table it writes."""

    def __init__(self, source, spec, base):
        self.source = source
        self.spec = spec
        self.base = base
        self.table = source + '_scores'
        self.parse = espn_games if source == 'espn' else ncaa_games
        self.next_s = 0.0
        self.is_live = False
        self.outage = None

    def urls(self, now_s):
        if self.source == 'espn':
            return [espn_url(self.base, self.spec, d) for d in poll_days(now_s)]
        return [ncaa_url(self.base, self.spec, d) for d in poll_days(now_s)]


class Scoreboards:
    """Every feed polled every fast_s while one of its games is on (or within PRE_S of its start),
    else every idle_s; each feed on its own (a scoreboard that hangs delays only itself), its
    dates in turn. A failed feed is an outage in the gaps table (source espn or ncaa), never a
    reason to stop the others."""

    def __init__(self, feeds, store, gap, fast_s=4.0, idle_s=60.0, fetch=fetch_json, clock=time.time):
        self.feeds = feeds
        self.store = store
        self.fast_s = fast_s
        self.idle_s = idle_s
        self.fetch = fetch
        self.clock = clock
        self.book = ScoreBook()
        self.rows = 0
        for f in feeds:
            f.outage = Outage(gap, f.source)

    def day(self, feed, url):
        try:
            return self.fetch(url)
        except urllib.error.HTTPError as e:
            # NCAA.com has no scoreboard file for a date without games: that is no games, not an
            # outage (ESPN answers such a date with an empty list, so a 404 there is a failure)
            if feed.source == 'ncaa' and e.code == 404:
                return {}
            raise

    async def poll(self, feed):
        now = self.clock()
        games = []
        try:
            for url in feed.urls(now):
                sb = await asyncio.to_thread(self.day, feed, url)
                games.extend(feed.parse(sb, feed.spec))
        except Exception as e:   # HTTP errors, timeouts, a body that is not JSON
            feed.outage.failed('%s: %s' % (feed.spec, short(e)), now_us())
            feed.next_s = now + (self.fast_s if feed.is_live else self.idle_s)
            return
        recv = now_us()
        feed.outage.ok(recv)
        for row in self.book.changes(games, recv):
            self.store.write(feed.table, row)
            self.rows += 1
        feed.is_live = live(games, now)
        feed.next_s = now + (self.fast_s if feed.is_live else self.idle_s)

    async def feed_loop(self, feed, stop, sleep):
        while not stop.is_set():
            await self.poll(feed)
            await sleep(max(0.0, feed.next_s - self.clock()))

    async def run(self, stop, sleep):
        await asyncio.gather(*(self.feed_loop(f, stop, sleep) for f in self.feeds))
