"""The recorder (plan section 7): Kalshi's market socket and market listings, to disk, 24/7.

What it records, on a read-only key:
  - orderbook_delta and trade for every market of the book series (the ten 15-minute financial
    series and the weather highs by default; REC_BOOK_SERIES), each window subscribed REC_LEAD_S
    before it opens (the old desk's books started 31 s late) and dropped REC_LINGER_S after it
    closes. A sequence gap on the book subscription reconnects (fresh snapshots); one on trades
    is recorded and skipped.
  - pyth_value for every underlying: the 15-minute series settle on it (the replication gate).
  - each market's REST row whenever its status, times, strike or result changed: open and
    unopened windows every REC_DISCOVER_S, closed and settled ones every REC_SETTLED_S.
  - gaps: disconnects and resumptions, sequence gaps, refused subscriptions, failed discovery,
    clock steps, torn spool lines.

Health: systemd's watchdog (WATCHDOG=1) and the healthchecks.io ping go out only while the
socket is subscribed and discovery is succeeding; silence is the alarm.

    python -m recorder.rec            # settings from the environment: recorder/config.py
"""
import asyncio
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

from . import config, kalshi
from .store import Store, mono_us, now_us

BOOK_CHANNELS = ('orderbook_delta', 'trade')
MARKET_FIELDS = ('status', 'open_time', 'close_time', 'strike_type', 'floor_strike', 'cap_strike', 'result',
                 'expiration_value')
KEEP_MARKET_S = 86400        # a closed market is forgotten a day after its close


def log(msg):
    print('%s %s' % (datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'), msg), flush=True)


def short(e):
    s = str(e) or type(e).__name__
    return s if len(s) <= 200 else s[:200] + '...'


def ts_s(rfc):
    """RFC 3339 to epoch seconds; None when absent or unreadable."""
    if not isinstance(rfc, str) or not rfc:
        return None
    try:
        return datetime.fromisoformat(rfc.replace('Z', '+00:00')).timestamp()
    except ValueError:
        return None


def wanted(m, now_s, lead_s, linger_s):
    """Subscribe this market's book now: not settled, opening within lead_s, closed under linger_s ago."""
    if m.get('status') in ('settled', 'finalized'):
        return False
    open_s, close_s = ts_s(m.get('open_time')), ts_s(m.get('close_time'))
    if close_s is not None and now_s > close_s + linger_s:
        return False
    if open_s is not None and open_s - now_s > lead_s:
        return False
    return True


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
        self.subs = {c: set() for c in BOOK_CHANNELS}   # subscribed or asked for
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


class Recorder:
    def __init__(self, settings, store=None):
        self.s = settings
        self.signer = kalshi.Signer(settings.key_id, settings.key_pem)
        self.rest = kalshi.Rest(settings.rest_url, self.signer, settings.ca_file)
        self.store = store or Store(settings.data_dir, settings.bucket_s, log)
        self.ws_path = urlparse(settings.ws_url).path
        self.ssl = kalshi.ssl_context(settings.ca_file) if settings.ws_url.startswith('wss') else None
        self.markets = {}          # ticker -> (series, latest REST row)
        self.digests = {}          # ticker -> the fields last recorded
        self.desired = set()
        self.conn = 0
        self.ws_up = False
        self.frames = 0
        self.last_frame_us = 0
        self.discovery_ok = 0.0    # monotonic seconds of the last round with no failure
        self.last_settled = 0.0
        self.noted = {}            # gap key -> monotonic seconds last recorded (rate limit)
        self.down_since = 0        # the socket went down at (us); 0 while up
        self.stop = None
        self.changed = None

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
    def see_market(self, series, m):
        t = m.get('ticker')
        if not isinstance(t, str) or not t:
            return
        self.markets[t] = (series, m)
        digest = json.dumps([m.get(k) for k in MARKET_FIELDS], separators=(',', ':'), default=str)
        if self.digests.get(t) != digest:
            self.digests[t] = digest
            self.store.write('kalshi_markets', market_row(series, m, now_us()))

    async def discover_once(self):
        now = time.time()
        settled_due = now - self.last_settled >= self.s.settled_s
        ok = True
        for series in self.s.book_series:
            statuses = ['open', 'unopened'] + (['closed', 'settled'] if settled_due else [])
            for st in statuses:
                since = int(now - 6 * 3600) if st in ('closed', 'settled') else None
                try:
                    rows = await asyncio.to_thread(self.rest.markets, series, st, since)
                except Exception as e:   # one series' failure costs that series this round, not the rest
                    ok = False
                    self.gap('rest', 'discovery_failed', '%s %s: %s' % (series, st, short(e)),
                             key=('discovery', series, st), every_s=600)
                    continue
                for m in rows:
                    self.see_market(series, m)
        if settled_due:
            self.last_settled = now
        for t in [t for t, (_, m) in self.markets.items()
                  if (ts_s(m.get('close_time')) or now) < now - KEEP_MARKET_S]:
            self.markets.pop(t, None)
            self.digests.pop(t, None)
        desired = {t for t, (_, m) in self.markets.items() if wanted(m, now, self.s.lead_s, self.s.linger_s)}
        if desired != self.desired:
            log('discovery: %d markets to record (+%d -%d)' % (len(desired), len(desired - self.desired),
                                                               len(self.desired - desired)))
            self.desired = desired
            self.changed.set()
        if ok:
            self.discovery_ok = time.monotonic()

    async def discovery_loop(self):
        while not self.stop.is_set():
            try:
                await self.discover_once()
            except Exception as e:
                self.gap('rest', 'discovery_failed', 'unexpected: ' + short(e), key=('discovery',), every_s=600)
            await self.sleep(self.s.discover_s)

    # ---- the socket --------------------------------------------------------------------------
    async def sync(self, ws, sess):
        """Bring the book subscriptions to the desired markets: subscribe a channel once it has
        markets (never with an empty list), then add and delete markets on its sid."""
        for ch in BOOK_CHANNELS:
            have = sess.subs[ch]
            add, drop = self.desired - have, have - self.desired
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
                    if ch == 'pyth_value' and not self.ws_up:
                        self.ws_up = True
                        if self.down_since:
                            self.gap('kalshi_ws', 'resumed', 'subscribed again on connection %d' % sess.conn,
                                     from_us=self.down_since, to_us=recv)
                            self.down_since = 0
                    if p and p[0] in BOOK_CHANNELS:
                        self.changed.set()   # markets that changed while the subscribe was in flight
                continue
            if typ == 'error':
                p = sess.pending.pop(j.get('id'), None)
                detail = json.dumps(j.get('msg'), separators=(',', ':'))[:300]
                if p:
                    ch, tickers, action = p
                    if action in ('subscribe', 'add_markets'):
                        sess.subs[ch] -= tickers   # tried again on the next change
                    detail = '%s %s of %d market(s): %s' % (ch, action, len(tickers), detail)
                self.gap('kalshi_ws', 'subscribe_error', detail, key=('ws_error', detail[:80]), every_s=600)
                continue
            if typ == 'ok':
                sess.pending.pop(j.get('id'), None)
            sid, seq = row['sid'], row['seq']
            if sid is not None and seq is not None:
                g = sess.seqs.see(sid, seq)
                if g:
                    ch = sess.channel_of(sid) or '?'
                    self.gap('kalshi_ws', 'seq_gap', '%s sid %d: expected %d, got %d' % (ch, sid, g[0], g[1]),
                             from_us=sess.last_us.get(sid), to_us=recv)
                    if ch == 'orderbook_delta':
                        raise Reconnect('a sequence gap on the book subscription')
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
            if self.stop.is_set():
                break
            if not self.down_since:
                self.down_since = now_us()
            self.gap('kalshi_ws', 'disconnect', why, from_us=self.last_frame_us or None)
            backoff = 1.0 if time.monotonic() - began > 60 else min(backoff * 2, 30.0)
            await self.sleep(backoff)

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
                log('status: %s, %d markets, %d frames, %d rows' % (
                    'healthy' if self.healthy() else 'NOT HEALTHY (%s)' % (
                        'socket down' if not self.ws_up else 'discovery failing'),
                    len(self.desired), self.frames, self.store.rows_written))
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

    async def run(self, run_s=0):
        self.stop = asyncio.Event()
        self.changed = asyncio.Event()
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGTERM, signal.SIGINT):
            loop.add_signal_handler(sig, self.stop.set)
        log('recorder: %s and %s, %d series, data in %s, key %s' % (
            self.s.rest_url, self.s.ws_url, len(self.s.book_series), self.s.data_dir, self.signer.kind))
        # spools a crash left behind
        for r in self._compact(self.store.finished()):
            self.store.note_compacted(r)
        await self.discover_once()
        sd_notify('READY=1')
        tasks = [asyncio.create_task(self.discovery_loop()), asyncio.create_task(self.ws_loop()),
                 asyncio.create_task(self.housekeeping())]
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
