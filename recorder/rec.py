"""The recorder (plan section 7): Kalshi's market socket and market listings, to disk, 24/7.

What it records, on a read-only key:
  - orderbook_delta and trade for every market of the book series (the ten 15-minute financial
    series and the weather highs by default; REC_BOOK_SERIES), each window subscribed REC_LEAD_S
    before it opens (the old desk's books started 31 s late) and dropped REC_LINGER_S after it
    closes. A sequence gap on the book subscription reconnects (fresh snapshots); one on trades
    is recorded, skipped, and backfilled by REST.
  - the same for every game of the college series (plan 5(d): women's basketball, volleyball,
    college hockey, table tennis, AHL, Liiga, SHL, CS2; REC_COLLEGE_SERIES), listed hourly, and
    for the thinnest tenth of the control series' games (men's basketball; college.py).
  - market_lifecycle_v2 for the recorded markets and the college games, an explicit list too.
  - pyth_value for every underlying: the 15-minute series settle on it (the replication gate).
  - each market's REST row whenever its status, times, strike or result changed: open and
    unopened windows every REC_DISCOVER_S, closed and settled ones every REC_SETTLED_S.
  - by public REST (polls.py): the college books every few seconds, and every recorded book once
    the socket's book subscription has been down REC_REST_FALLBACK_S; trades over every window
    the socket may have missed and every hour; incentive programs hourly; every open market daily.
  - ESPN's and NCAA.com's scoreboards for the college sports, each change once (scores.py).
  - gaps: disconnects and resumptions, sequence gaps, refused subscriptions, failed discovery and
    polls, clock steps, restarts, torn spool lines.

Health: systemd's watchdog (WATCHDOG=1) and the healthchecks.io ping go out only while the
socket is subscribed and discovery is succeeding; silence is the alarm. (A scoreboard or a REST
poll failing is a gap, not a restart: restarting would not bring ESPN back.)

    python -m recorder.rec            # settings from the environment: recorder/config.py
"""
import asyncio
import concurrent.futures
import json
import os
import signal
import socket
import sys
import time
import traceback
import urllib.request
from datetime import datetime, timezone
from urllib.parse import urlparse

from . import college, config, kalshi, polls, scores
from .store import State, Store, mono_us, now_us

BOOK_CHANNELS = ('orderbook_delta', 'trade')
LIFECYCLE = 'market_lifecycle_v2'
MARKET_FIELDS = ('status', 'open_time', 'close_time', 'strike_type', 'floor_strike', 'cap_strike', 'result',
                 'expiration_value')
KEEP_MARKET_S = 86400        # a closed market is forgotten a day after its close
COLLEGE_RETRY_S = 300        # a college series whose listing failed is tried again this soon
LIFECYCLE_HOLD_S = 600       # a refused lifecycle subscribe is not asked again for this long
SWEEP_RETRY_S = 600
STARTUP_DISCOVERY_S = 90     # under the unit's TimeoutStartSec=120
DONE = ('settled', 'finalized')


def log(msg):
    print('%s %s' % (datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'), msg), flush=True)


short = polls.short


def ts_s(rfc):
    """RFC 3339 to epoch seconds; None when absent or unreadable."""
    if not isinstance(rfc, str) or not rfc:
        return None
    try:
        return datetime.fromisoformat(rfc.replace('Z', '+00:00')).timestamp()
    except ValueError:
        return None


def rfc(epoch_s):
    return datetime.fromtimestamp(epoch_s, tz=timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def wanted(m, now_s, lead_s, linger_s):
    """Subscribe this market's book now: not settled, opening within lead_s, closed under linger_s ago."""
    if m.get('status') in DONE:
        return False
    open_s, close_s = ts_s(m.get('open_time')), ts_s(m.get('close_time'))
    if close_s is not None and now_s > close_s + linger_s:
        return False
    if open_s is not None and open_s - now_s > lead_s:
        return False
    return True


def recordable_during(m, a_s, b_s, now_s, lead_s, linger_s):
    """Was this market on the recording list at some time in [a_s, b_s]: does its span, lead_s
    before it opens to linger_s after it closes, overlap it? A missing open time is unbounded; a
    missing close time is now, unless the market is settled (when it stopped is then unknown)."""
    open_s, close_s = ts_s(m.get('open_time')), ts_s(m.get('close_time'))
    if close_s is None:
        if m.get('status') in DONE:
            return False
        close_s = now_s
    if close_s + linger_s < a_s:
        return False
    return open_s is None or open_s - lead_s <= b_s


def market_row(series, m, recv):
    def num(k):
        v = m.get(k)
        return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else None

    def text(k):
        v = m.get(k)
        return None if v is None or v == '' else str(v)

    return {'recv_us': recv, 'series': series, 'ticker': text('ticker'), 'status': text('status'),
            'open_time': text('open_time'), 'close_time': text('close_time'), 'strike_type': text('strike_type'),
            'floor_strike': num('floor_strike'), 'cap_strike': num('cap_strike'), 'result': text('result'),
            'expiration_value': text('expiration_value'), 'raw': json.dumps(m, separators=(',', ':'))}


def frame_row(text, recv, mono, conn):
    """A frame as the kalshi_ws row (raw kept whole) and its parsed JSON (None if unreadable)."""
    try:
        j = json.loads(text)
    except ValueError:
        j = None
    if not isinstance(j, dict):
        return None, {'recv_us': recv, 'mono_us': mono, 'conn': conn, 'type': None, 'sid': None, 'seq': None,
                      'ticker': None, 'raw': text}
    msg = j.get('msg') if isinstance(j.get('msg'), dict) else {}
    ticker = msg.get('market_ticker') or msg.get('underlying_ticker')

    def integer(v):
        return v if isinstance(v, int) and not isinstance(v, bool) else None

    return j, {'recv_us': recv, 'mono_us': mono, 'conn': conn,
               'type': j.get('type') if isinstance(j.get('type'), str) else None,
               'sid': integer(j.get('sid')), 'seq': integer(j.get('seq')),
               'ticker': ticker if isinstance(ticker, str) else None, 'raw': text}


def apply_lifecycle(m, f):
    """A cached REST row brought up to date by a lifecycle message, or None when the message does
    not change what the recorder subscribes (created, activated and deactivated are kept raw)."""
    et = f['event_type']
    m = dict(m)
    if et == 'close_date_updated' and f['close_ts'] is not None:
        m['close_time'] = rfc(f['close_ts'])   # a game that ended early: its linger counts from now
    elif et == 'determined':
        m['status'] = 'determined'
        if f['result']:
            m['result'] = f['result']
    elif et == 'settled':
        m['status'] = 'settled'
    else:
        return None
    return m


class Seqs:
    """Kalshi numbers each subscription's messages 1, 2, ... across all its markets (its ok
    replies spend numbers too). A break is a lost message."""

    def __init__(self):
        self.last = {}

    def see(self, sid, seq):
        prev = self.last.get(sid)
        self.last[sid] = seq
        expect = 1 if prev is None else prev + 1
        return None if seq == expect else (expect, seq)


class Reconnect(Exception):
    pass


class Session:
    """One socket's subscriptions."""

    def __init__(self, conn):
        self.conn = conn
        self.ids = 0
        self.pending = {}          # command id -> (channel, tickers, action)
        self.sids = {}             # channel -> sid
        self.subs = {}             # channel -> markets subscribed or asked for
        self.hold = {}             # channel -> monotonic s before which it is not asked again
        self.seqs = Seqs()
        self.last_us = {}          # sid -> its last frame's receipt

    def next_id(self):
        self.ids += 1
        return self.ids

    def channel_of(self, sid):
        for c, s in self.sids.items():
            if s == sid:
                return c
        return None


def sd_notify(state):
    addr = os.environ.get('NOTIFY_SOCKET')
    if not addr:
        return
    if addr.startswith('@'):
        addr = '\0' + addr[1:]
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as s:
            s.connect(addr)
            s.sendall(state.encode())
    except OSError as e:
        log('sd_notify: %s' % short(e))


def score_feeds(s):
    return ([scores.Feed('espn', spec, s.espn_url) for spec in s.espn_leagues] +
            [scores.Feed('ncaa', spec, s.ncaa_url) for spec in s.ncaa_sports])


class Recorder:
    def __init__(self, settings, store=None):
        self.s = settings
        self.signer = kalshi.Signer(settings.key_id, settings.key_pem)
        self.rest = kalshi.Rest(settings.rest_url, self.signer, settings.ca_file)
        self.store = store or Store(settings.data_dir, settings.bucket_s, log)
        self.state = State(settings.data_dir, log)
        # two budgets under one key: the books' polling cannot starve discovery, nor an hour's
        # trade backfill the books
        self.venue = polls.Venue(self.rest, polls.RateLimiter(settings.rest_rps))
        self.book_venue = polls.Venue(self.rest, polls.RateLimiter(settings.rest_book_rps))
        self.ws_path = urlparse(settings.ws_url).path
        self.ssl = kalshi.ssl_context(settings.ca_file) if settings.ws_url.startswith('wss') else None
        self.groups = {}
        for group, series in (('book', settings.book_series), ('college', settings.college_series),
                              ('control', settings.control_series)):
            for x in series:
                self.groups[x] = group
        self.markets = {}          # ticker -> (series, latest REST row, group)
        self.digests = {}          # ticker -> the fields last recorded
        self.desired = set()       # books and trades on the socket
        self.lifecycle = set()     # market_lifecycle_v2 on the socket
        self.college = set()       # the desired markets of college and control games: books by REST too
        self.recorded = {}         # ticker -> when it was last on the desired list (us), for backfill
        self.next_college = {}     # series -> epoch s its next listing is due
        self.reach_back = {}       # series -> epoch s: its next closed/settled listing reaches back here (a restart)
        self.control = college.ControlPicks(settings.control_frac, settings.control_pick_h, self.state)
        self.conn = 0
        self.ws_up = False
        self.book_live = False     # the orderbook_delta subscription is acknowledged on a live socket
        self.book_down_at = None   # monotonic s it went (or started) missing
        self.fallback_on = False
        self.frames = 0
        self.last_frame_us = 0
        self.discovery_ok = 0.0    # monotonic seconds of the last round with no failure
        self.last_settled = 0.0
        self.noted = {}            # gap key -> monotonic seconds last recorded (rate limit)
        self.down_since = 0        # the socket went down at (us); 0 while up
        self.down_from = 0         # the last frame before it went down (us): backfill from here
        self.stop = None
        self.changed = None
        self.rest_books = polls.RestBooks(self.book_venue, self.store, self.gap, self.rest_listing,
                                          settings.rest_book_s, settings.rest_book_hot_s)
        self.backfill = polls.TradeBackfill(self.venue, self.store, self.gap, self.recorded_between,
                                            save=self.save_backfill)
        feeds = score_feeds(settings)
        self.scores = scores.Scoreboards(feeds, self.store, self.gap, settings.scores_s,
                                         settings.scores_idle_s) if feeds else None

    # ---- gaps --------------------------------------------------------------------------------
    def gap(self, source, kind, detail, from_us=None, to_us=None, key=None, every_s=0):
        if key is not None:
            t = time.monotonic()
            if t - self.noted.get(key, -1e9) < every_s:
                return
            self.noted[key] = t
        log('gap: %s %s: %s' % (source, kind, detail))
        self.store.write('gaps', {'recv_us': now_us(), 'source': source, 'kind': kind, 'detail': detail,
                                  'from_us': from_us, 'to_us': to_us})

    # ---- discovery ---------------------------------------------------------------------------
    def see_market(self, series, m, group='book'):
        t = m.get('ticker')
        if not isinstance(t, str) or not t:
            return
        self.markets[t] = (series, m, group)
        digest = json.dumps([m.get(k) for k in MARKET_FIELDS], separators=(',', ':'), default=str)
        if self.digests.get(t) != digest:
            self.digests[t] = digest
            self.store.write('kalshi_markets', market_row(series, m, now_us()))

    async def list_markets(self, series, status, min_close_ts):
        out = []
        async for items, _ in self.venue.pages('/markets', {'series_ticker': series, 'status': status, 'limit': 1000,
                                                            'min_close_ts': min_close_ts}, 'markets', 10):
            out.extend(items)
        return out

    def due_series(self, now):
        """This round's listings: every book series; a college or control series once an hour."""
        out = [(x, 'book') for x in self.s.book_series]
        out += [(x, g) for x, g in self.groups.items() if g != 'book' and self.next_college.get(x, 0) <= now]
        return out

    async def discover_once(self):
        now = time.time()
        settled_due = now - self.last_settled >= self.s.settled_s
        ok, control_listed = True, None
        for series, group in self.due_series(now):
            statuses = ['open', 'unopened']
            if group != 'book' or settled_due:   # games settle within hours: their results hourly
                statuses += ['closed', 'settled']
            series_ok = True
            for st in statuses:
                # results of the last 6 h; after a restart, once, back to the start of its window
                since = int(min(now - 6 * 3600, self.reach_back.get(series, now))) \
                    if st in ('closed', 'settled') else None
                try:
                    rows = await self.list_markets(series, st, since)
                except Exception as e:   # one series' failure costs that series this round, not the rest
                    series_ok = False
                    self.gap('rest', 'discovery_failed', '%s %s: %s' % (series, st, short(e)),
                             key=('discovery', series, st), every_s=600)
                    continue
                for m in rows:
                    self.see_market(series, m, group)
            if series_ok and 'closed' in statuses:
                self.reach_back.pop(series, None)
            if group == 'book':
                ok = ok and series_ok
            else:
                self.next_college[series] = now + (self.s.college_discover_s if series_ok else COLLEGE_RETRY_S)
            if group == 'control':
                control_listed = series_ok if control_listed is None else control_listed and series_ok
        if settled_due:
            self.last_settled = now
        if control_listed:   # every control series listed in full this round: pick if it is the hour
            self.pick_control(now)
        for t in [t for t, (_, m, _) in self.markets.items()
                  if (ts_s(m.get('close_time')) or now) < now - KEEP_MARKET_S]:
            self.markets.pop(t, None)
            self.digests.pop(t, None)
        self.recompute(now)
        if ok:
            self.discovery_ok = time.monotonic()

    def pick_control(self, now):
        listed = {x: [] for x in self.s.control_series}
        for series, m, group in self.markets.values():
            if group == 'control':
                listed[series].append(m)
        rows = self.control.update(listed, now, now_us())
        for r in rows:
            self.store.write('college_control', r)
        if rows:
            picked = [r['event_ticker'] for r in rows if r['picked']]
            log('control: %d of %d games picked for %s: %s' % (len(picked), len(rows), rows[0]['game_date'],
                                                               ', '.join(picked[:10])))

    def recompute(self, now=None):
        """The socket's lists from the cached rows: books and trades for every wanted market (a
        control game only if picked), lifecycle for those and every unsettled college game."""
        now = time.time() if now is None else now
        desired, college_set, life = set(), set(), set()
        for t, (series, m, group) in self.markets.items():
            if group == 'control' and not self.control.picked(m.get('event_ticker')):
                continue
            if wanted(m, now, self.s.lead_s, self.s.linger_s):
                desired.add(t)
                if group != 'book':
                    college_set.add(t)
            if group != 'book' and m.get('status') not in DONE:
                life.add(t)
        life = (life | desired) if self.s.lifecycle else set()
        stamp = int(now * 1_000_000)
        for t in desired:
            self.recorded[t] = stamp
        for t in [t for t, u in self.recorded.items() if u < stamp - KEEP_MARKET_S * 1_000_000]:
            del self.recorded[t]
        self.college = college_set
        if desired != self.desired or life != self.lifecycle:
            log('discovery: %d markets to record (+%d -%d), %d college, lifecycle on %d' % (
                len(desired), len(desired - self.desired), len(self.desired - desired), len(college_set), len(life)))
            self.desired, self.lifecycle = desired, life
            if self.changed is not None:
                self.changed.set()

    def recorded_between(self, from_us, to_us):
        """The markets a trade backfill window reads: those on this process's recording list at
        some point since from_us, and every cached market (a control game only if picked) whose
        span from lead_s before its open to linger_s after its close overlaps the window. The
        second is a restart's: a window that closed, or opened and closed, while the recorder was
        down was never on this process's list, but the first listing after the restart has its
        row (note_restart sets how far back it reaches)."""
        out = {t for t, u in self.recorded.items() if u >= from_us} | set(self.desired)
        a, b, now = from_us / 1e6, to_us / 1e6, time.time()
        for t, (_, m, group) in self.markets.items():
            if t in out or (group == 'control' and not self.control.picked(m.get('event_ticker'))):
                continue
            if recordable_during(m, a, b, now, self.s.lead_s, self.s.linger_s):
                out.add(t)
        return out

    async def discovery_loop(self):
        while not self.stop.is_set():
            try:
                await self.discover_once()
            except Exception as e:
                self.gap('rest', 'discovery_failed', 'unexpected: ' + short(e), key=('discovery',), every_s=600)
            await self.sleep(self.s.discover_s)

    # ---- REST books: the college list, and every book once the socket's have been down -------
    def fallback(self):
        """Has the socket's book subscription been missing for REC_REST_FALLBACK_S, while there are
        books it should carry? (The day-one fallback, plan section 7: REST snapshots.)"""
        t = time.monotonic()
        if self.book_live or not (self.desired - self.college):
            self.book_down_at = None
            on = False
        else:
            if self.book_down_at is None:
                self.book_down_at = t
            on = t - self.book_down_at >= self.s.rest_fallback_s
        if on != self.fallback_on:
            self.fallback_on = on
            self.gap('kalshi_rest_books', 'fallback_on' if on else 'fallback_off',
                     'the socket books %s: REST polls %d book market(s)%s' % (
                         'are missing' if on else 'are back', len(self.desired - self.college),
                         '' if on else ' no longer'))
        return on

    def rest_listing(self):
        listed = {t: 'college' for t in self.college}
        if self.fallback():
            for t in self.desired - self.college:
                listed[t] = 'fallback'
        return listed

    # ---- the socket --------------------------------------------------------------------------
    def channel_sets(self):
        sets = {ch: self.desired for ch in BOOK_CHANNELS}
        if self.s.lifecycle:
            sets[LIFECYCLE] = self.lifecycle
        return sets

    async def sync(self, ws, sess):
        """Bring each channel's subscription to its list: subscribe a channel once it has markets
        (never with an empty list), then add and delete markets on its sid."""
        for ch, want in self.channel_sets().items():
            have = sess.subs.setdefault(ch, set())
            if sess.hold.get(ch, 0) > time.monotonic():
                continue
            add, drop = want - have, have - want
            if ch not in sess.sids:
                if any(p[0] == ch for p in sess.pending.values()) or not add:
                    continue   # its subscribe is in flight: synced again on the ack
                i = sess.next_id()
                sess.pending[i] = (ch, set(add), 'subscribe')
                await ws.send(kalshi.subscribe_cmd(i, ch, add))
                have |= add
                continue
            if add:
                i = sess.next_id()
                sess.pending[i] = (ch, set(add), 'add_markets')
                await ws.send(kalshi.update_cmd(i, sess.sids[ch], 'add_markets', add, ch == 'orderbook_delta'))
                have |= add
            if drop:
                i = sess.next_id()
                sess.pending[i] = (ch, set(drop), 'delete_markets')
                await ws.send(kalshi.update_cmd(i, sess.sids[ch], 'delete_markets', drop, False))
                have -= drop

    def on_lifecycle(self, j):
        f = kalshi.lifecycle_fields(j)
        ent = self.markets.get(f['ticker']) if f else None
        if ent is None:
            return
        m = apply_lifecycle(ent[1], f)
        if m is not None:
            self.markets[f['ticker']] = (ent[0], m, ent[2])
            self.recompute()

    async def read(self, ws, sess):
        async for text in ws:
            recv, mono = now_us(), mono_us()
            if isinstance(text, bytes):
                text = text.decode('utf-8', 'replace')
            j, row = frame_row(text, recv, mono, sess.conn)
            self.store.write('kalshi_ws', row)
            self.frames += 1
            self.last_frame_us = recv
            if j is None:
                continue
            typ = row['type']
            if typ == 'subscribed':
                msg = j.get('msg') if isinstance(j.get('msg'), dict) else {}
                ch, sid = msg.get('channel'), msg.get('sid')
                p = sess.pending.pop(j.get('id'), None)
                if isinstance(sid, int) and isinstance(ch, str):
                    sess.sids[ch] = sid
                    if ch == 'orderbook_delta':
                        self.book_live = True
                    if ch == 'pyth_value' and not self.ws_up:
                        self.ws_up = True
                        if self.down_since:
                            self.gap('kalshi_ws', 'resumed', 'subscribed again on connection %d' % sess.conn,
                                     from_us=self.down_since, to_us=recv)
                            # trades from the last frame before the drop to now, by REST
                            self.backfill.add(min(self.down_from or self.down_since, self.down_since), recv,
                                              'disconnect')
                            self.down_since = self.down_from = 0
                    if p and p[0] in sess.subs:
                        self.changed.set()   # markets that changed while the subscribe was in flight
                continue
            if typ == 'error':
                p = sess.pending.pop(j.get('id'), None)
                detail = json.dumps(j.get('msg'), separators=(',', ':'))[:300]
                if p:
                    ch, tickers, action = p
                    if action in ('subscribe', 'add_markets'):
                        sess.subs[ch] -= tickers   # tried again on the next change
                    if ch == LIFECYCLE and action == 'subscribe':
                        # a venue that refuses a market list on this channel refuses it every time
                        sess.hold[ch] = time.monotonic() + LIFECYCLE_HOLD_S
                    detail = '%s %s of %d market(s): %s' % (ch, action, len(tickers), detail)
                self.gap('kalshi_ws', 'subscribe_error', detail, key=('ws_error', detail[:80]), every_s=600)
                continue
            if typ == 'ok':
                sess.pending.pop(j.get('id'), None)
            if typ == LIFECYCLE:
                self.on_lifecycle(j)
            sid, seq = row['sid'], row['seq']
            if sid is not None and seq is not None:
                g = sess.seqs.see(sid, seq)
                if g:
                    ch = sess.channel_of(sid) or '?'
                    self.gap('kalshi_ws', 'seq_gap', '%s sid %d: expected %d, got %d' % (ch, sid, g[0], g[1]),
                             from_us=sess.last_us.get(sid), to_us=recv)
                    if ch == 'orderbook_delta':
                        raise Reconnect('a sequence gap on the book subscription')
                    if ch == 'trade':
                        self.backfill.add(sess.last_us.get(sid) or recv, recv, 'seq_gap')
                sess.last_us[sid] = recv

    async def session(self):
        from websockets.asyncio.client import connect
        self.conn += 1
        sess = Session(self.conn)
        headers = self.signer.headers('GET', self.ws_path)
        async with connect(self.s.ws_url, additional_headers=headers, ssl=self.ssl, ping_interval=10,
                           ping_timeout=20, open_timeout=10, close_timeout=3, max_size=1 << 24,
                           user_agent_header=kalshi.UA) as ws:
            log('socket: connection %d open' % sess.conn)
            i = sess.next_id()
            sess.pending[i] = ('pyth_value', set(), 'subscribe')
            await ws.send(kalshi.pyth_cmd(i))
            self.changed.clear()
            await self.sync(ws, sess)
            reader = asyncio.create_task(self.read(ws, sess))
            try:
                while not self.stop.is_set():
                    waits = {reader, asyncio.create_task(self.changed.wait()), asyncio.create_task(self.stop.wait())}
                    done, rest = await asyncio.wait(waits, return_when=asyncio.FIRST_COMPLETED)
                    for t in rest - {reader}:
                        t.cancel()
                    if reader in done:
                        reader.result()   # raises what ended it
                        return 'the venue closed the socket'
                    if self.changed.is_set():
                        self.changed.clear()
                        await self.sync(ws, sess)
                return 'stopping'
            finally:
                reader.cancel()
                self.ws_up = False
                self.book_live = False

    async def ws_loop(self):
        backoff = 1.0
        while not self.stop.is_set():
            began = time.monotonic()
            try:
                why = await self.session()
            except Reconnect as e:
                why = str(e)
            except asyncio.CancelledError:
                raise
            except Exception as e:
                why = '%s: %s' % (type(e).__name__, short(e))
            self.ws_up = False
            self.book_live = False
            if self.stop.is_set():
                break
            if not self.down_since:
                self.down_since = now_us()
                self.down_from = self.last_frame_us
            self.gap('kalshi_ws', 'disconnect', why, from_us=self.last_frame_us or None)
            backoff = 1.0 if time.monotonic() - began > 60 else min(backoff * 2, 30.0)
            await self.sleep(backoff)

    # ---- trade backfill and the sweeps ---------------------------------------------------------
    async def backfill_loop(self):
        """Every queued window read as soon as it is queued (a restart's at once); the last hour's
        every REC_BACKFILL_S; a failed pass tried again a minute later."""
        last_hour = now_us()
        while not self.stop.is_set():
            t = now_us()
            if t - last_hour >= self.s.backfill_s * 1_000_000:
                self.backfill.add(last_hour, t, 'hourly')
                last_hour = t
            if self.backfill.windows:
                try:
                    await self.backfill.drain()
                    self.backfill.outage.ok(now_us())
                except Exception as e:
                    self.backfill.outage.failed(short(e), now_us())
                    await self.sleep(60)
            await self.sleep(5)

    def daily_due(self, now):
        t = datetime.fromtimestamp(now, tz=timezone.utc)
        return t.hour >= self.s.markets_daily_h and self.state.get('markets_daily') != t.strftime('%Y-%m-%d')

    async def sweeps_loop(self):
        """GET /incentive_programs hourly, GET /markets once a day; a failed sweep is retried in
        ten minutes, its partial rows kept under their sweep_us and named in a gaps row."""
        next_inc = next_daily = 0.0
        while not self.stop.is_set():
            now = time.time()
            if self.s.incentive_types and now >= next_inc:
                r = await polls.incentives_sweep(self.venue, self.store, set(self.s.incentive_types),
                                                 self.s.incentive_status)
                self.swept('incentives', r, '%d kept, %d of other types' % (r['rows'], r['dropped']))
                next_inc = now + (self.s.incentives_s if r['error'] is None else SWEEP_RETRY_S)
            if now >= next_daily and self.daily_due(now):
                # mve_filter=exclude: the multivariate combinations are most of the open listing
                # and none is recorded (TO CONFIRM the parameter; the venue ignores an unknown one)
                r = await polls.markets_sweep(self.venue, self.store, {'status': 'open', 'mve_filter': 'exclude'},
                                              self.s.markets_daily_pages)
                self.swept('markets_daily', r, '%d markets' % r['rows'])
                if r['error'] is None:
                    self.state.set('markets_daily', datetime.fromtimestamp(now, tz=timezone.utc).strftime('%Y-%m-%d'))
                else:
                    next_daily = now + SWEEP_RETRY_S
            await self.sleep(30)

    def swept(self, source, r, what):
        if r['error'] is not None:
            self.gap(source, 'sweep_failed', 'sweep %d stopped after %d rows: %s' % (r['sweep_us'], r['rows'], r['error']),
                     from_us=r['sweep_us'], to_us=now_us())
        elif r['cut']:
            self.gap(source, 'sweep_cut', 'sweep %d hit its page limit after %d rows' % (r['sweep_us'], r['rows']),
                     from_us=r['sweep_us'], to_us=now_us())
        else:
            log('%s: %s' % (source, what))

    # ---- housekeeping ------------------------------------------------------------------------
    async def sleep(self, seconds):
        try:
            await asyncio.wait_for(self.stop.wait(), seconds)
        except asyncio.TimeoutError:
            pass

    def healthy(self):
        fresh = time.monotonic() - self.discovery_ok < 3 * self.s.discover_s + 30
        return self.ws_up and fresh

    async def housekeeping(self):
        last_roll = last_health = last_ping = last_status = 0.0
        skew0 = time.time() - time.monotonic()
        compacting = None
        while not self.stop.is_set():
            await self.sleep(1)
            self.store.flush()
            t = time.monotonic()
            if t - last_roll >= 10 and (compacting is None or compacting.done()):
                last_roll = t
                if compacting is not None:
                    for r in compacting.result():
                        self.store.note_compacted(r)
                ready = self.store.finished()
                compacting = asyncio.create_task(asyncio.to_thread(self._compact, ready)) if ready else None
            if t - last_health >= 15:
                last_health = t
                skew = time.time() - time.monotonic()
                if abs(skew - skew0) > 1.0:
                    self.gap('clock', 'clock_step', 'the wall clock moved %.3f s against the monotonic clock'
                             % (skew - skew0))
                skew0 = skew
                if self.healthy():
                    sd_notify('WATCHDOG=1')
                    if self.s.hc_url and t - last_ping >= 60:
                        last_ping = t
                        asyncio.create_task(asyncio.to_thread(self._ping))
            if t - last_status >= 60:
                last_status = t
                log('status: %s, %d markets (%d college), %d frames, %d rows (rest books %d, scores %d, '
                    'trades %d)%s' % (
                        'healthy' if self.healthy() else 'NOT HEALTHY (%s)' % (
                            'socket down' if not self.ws_up else 'discovery failing'),
                        len(self.desired), len(self.college), self.frames, self.store.rows_written,
                        self.rest_books.rows, self.scores.rows if self.scores else 0, self.backfill.rows,
                        ', REST FALLBACK' if self.fallback_on else ''))
        if compacting is not None:
            for r in await compacting:
                self.store.note_compacted(r)

    def _compact(self, ready):
        out = []
        for table, start, path in ready:
            try:
                r = self.store.compact(table, start, path)
            except Exception as e:   # the spool stays; tried again next roll
                log('store: compacting %s failed: %s' % (path, short(e)))
                continue
            if r:
                out.append(r)
        return out

    def _ping(self):
        try:
            urllib.request.urlopen(self.s.hc_url, timeout=10).read()
        except Exception as e:
            log('healthchecks: %s' % short(e))

    def save_backfill(self, windows):
        try:
            self.state.set('backfill', [list(w) for w in windows])
        except OSError as e:   # a backfill window lost on a restart is a smaller harm than a stop
            log('state: the backfill windows not saved: %s' % short(e))

    def note_restart(self):
        """A restart is a gap: from the socket's last frame on disk (read before this run writes
        any) to now, with its trades backfilled; the windows a previous run queued and never read
        are queued again. The socket's last frame, not the newest row of any table: with the
        socket down the recorder goes on writing disconnect rows, REST books and scores until the
        watchdog kills it, and the trades of that stretch were never on the socket. (Any table's
        newest row when the socket left none; nothing on a new data directory.)"""
        t = now_us()
        n = self.backfill.restore(self.state.get('backfill'), t)
        if n:
            log('backfill: %d window(s) a previous run had not read, queued again' % n)
        newest = self.store.last_recv_us()
        if newest is None:
            return
        last = self.store.last_recv_us('kalshi_ws') or newest
        detail = 'not recording for %.0f s (stopped, crashed or rebooted)' % ((t - newest) / 1e6)
        if newest - last > 5_000_000:
            detail += '; the socket silent for %.0f s' % ((t - last) / 1e6)
        self.gap('recorder', 'restart', detail, from_us=last, to_us=t)
        # trades back to the socket's last frame; two days at most (a socket down for days)
        last = max(last, t - polls.TradeBackfill.KEEP_US)
        self.backfill.add(last, t, 'restart')
        # the first listing of each series reads the closed and settled rows back to the window's
        # start, so the backfill knows the markets that closed while it was down (a day at most:
        # discovery forgets a market a day after its close)
        back = (last - polls.TradeBackfill.PAD_US) / 1e6 - self.s.linger_s
        self.reach_back = {x: max(back, t / 1e6 - KEEP_MARKET_S) for x in self.groups}

    async def run(self, run_s=0):
        self.stop = asyncio.Event()
        self.changed = asyncio.Event()
        loop = asyncio.get_running_loop()
        # every REST call, scoreboard fetch and compaction runs in a thread; the default pool on
        # the VM's single core would be 5 threads
        loop.set_default_executor(concurrent.futures.ThreadPoolExecutor(max_workers=16, thread_name_prefix='rec'))
        for sig in (signal.SIGTERM, signal.SIGINT):
            loop.add_signal_handler(sig, self.stop.set)
        log('recorder: %s and %s, %d book, %d college and %d control series, %d score feeds, data in %s, key %s' % (
            self.s.rest_url, self.s.ws_url, len(self.s.book_series), len(self.s.college_series),
            len(self.s.control_series), len(self.scores.feeds) if self.scores else 0, self.s.data_dir,
            self.signer.kind))
        self.note_restart()
        self.backfill.seed(self.store.read_recent('kalshi_trades_rest', now_us() - polls.TradeBackfill.KEEP_US))
        # spools a crash left behind
        for r in self._compact(self.store.finished()):
            self.store.note_compacted(r)
        try:
            # bounded: a venue that hangs must not hold READY past systemd's start timeout (the
            # loop goes on listing, and the watchdog waits for a discovery that succeeds)
            await asyncio.wait_for(self.discover_once(), STARTUP_DISCOVERY_S)
        except asyncio.TimeoutError:
            self.gap('rest', 'discovery_failed', 'the first listing took over %d s' % STARTUP_DISCOVERY_S)
        sd_notify('READY=1')
        tasks = [asyncio.create_task(c) for c in (
            self.discovery_loop(), self.ws_loop(), self.housekeeping(), self.backfill_loop(), self.sweeps_loop(),
            self.rest_books.run(self.stop, self.sleep))]
        if self.scores:
            tasks.append(asyncio.create_task(self.scores.run(self.stop, self.sleep)))
        if run_s:
            loop.call_later(run_s, self.stop.set)
        await self.stop.wait()
        sd_notify('STOPPING=1')
        log('recorder: stopping')
        done, pending = await asyncio.wait(tasks, timeout=10)
        for t in pending:
            t.cancel()
        for t in done:
            if t.exception() and not isinstance(t.exception(), asyncio.CancelledError):
                log('recorder: a task ended with %s' % short(t.exception()))
        self.store.close()
        log('recorder: stopped: %d frames, %d rows' % (self.frames, self.store.rows_written))


def main():
    try:
        s = config.load()
    except (ValueError, OSError) as e:
        print('recorder: %s' % e, file=sys.stderr)
        return 2
    r = Recorder(s)
    try:
        asyncio.run(r.run(float(os.environ.get('REC_RUN_S', '0') or 0)))
    except Exception:
        traceback.print_exc()
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
