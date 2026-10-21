"""The recorder's pure parts: which markets to record, frame rows, sequence gaps; and the Recorder's
lists and switches driven by hand (no socket, a fake REST): the college and control games, the
lifecycle list, the REST fallback, restart gaps."""
import asyncio
import json
import os
import tempfile
import time
import types
import unittest
from datetime import datetime, timezone
from unittest import mock

from recorder import config, polls, rec
from recorder.rec import Recorder, Seqs, apply_lifecycle, frame_row, market_row, wanted
from recorder.store import Store
from recorder.tests.util import HAVE_CRYPTO, fake_venue, fixture_lines, spooled

NOW = 1791648000.0   # 2026-10-10T16:00:00Z


def iso(t):
    import datetime
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


class WantedTest(unittest.TestCase):
    def test_windows(self):
        lead, linger = 1200, 300
        open_now = {'status': 'active', 'open_time': iso(NOW - 60), 'close_time': iso(NOW + 840)}
        next_one = {'status': 'initialized', 'open_time': iso(NOW + 840), 'close_time': iso(NOW + 1740)}
        far = {'status': 'initialized', 'open_time': iso(NOW + 9000), 'close_time': iso(NOW + 9900)}
        just_closed = {'status': 'closed', 'open_time': iso(NOW - 1000), 'close_time': iso(NOW - 100)}
        long_closed = {'status': 'closed', 'open_time': iso(NOW - 2000), 'close_time': iso(NOW - 1000)}
        settled = {'status': 'settled', 'open_time': iso(NOW - 1000), 'close_time': iso(NOW - 100)}
        weather = {'status': 'active', 'open_time': iso(NOW - 40000), 'close_time': iso(NOW + 30000)}
        got = [wanted(m, NOW, lead, linger) for m in (open_now, next_one, far, just_closed, long_closed, settled, weather)]
        self.assertEqual(got, [True, True, False, True, False, False, True])


class FrameTest(unittest.TestCase):
    def test_rows(self):
        text = json.dumps({'type': 'orderbook_delta', 'sid': 3, 'seq': 9, 'msg': {'market_ticker': 'TK', 'price_dollars': '0.41'}})
        j, row = frame_row(text, 10, 20, 2)
        self.assertEqual((row['type'], row['sid'], row['seq'], row['ticker'], row['conn'], row['raw']),
                         ('orderbook_delta', 3, 9, 'TK', 2, text))
        _, row = frame_row(json.dumps({'type': 'pyth_value', 'sid': 4, 'msg': {'underlying_ticker': 'X/USD'}}), 1, 2, 1)
        self.assertEqual((row['ticker'], row['seq']), ('X/USD', None))
        j, row = frame_row('not json', 1, 2, 1)
        self.assertIsNone(j)
        self.assertEqual((row['type'], row['raw']), (None, 'not json'))   # kept whole, never dropped

    def test_market_row(self):
        m = {'ticker': 'TK', 'status': 'settled', 'floor_strike': 3, 'result': 'no', 'expiration_value': 3.1}
        r = market_row('KXNATGAS15M', m, 5)
        self.assertEqual((r['floor_strike'], r['cap_strike'], r['result'], r['expiration_value']), (3.0, None, 'no', '3.1'))
        self.assertEqual(json.loads(r['raw']), m)


class SeqTest(unittest.TestCase):
    def test_gaps(self):
        s = Seqs()
        self.assertIsNone(s.see(1, 1))
        self.assertIsNone(s.see(1, 2))
        self.assertEqual(s.see(1, 4), (3, 4))
        self.assertIsNone(s.see(1, 5))
        self.assertEqual(s.see(2, 3), (1, 3))   # a subscription starts at 1


class LifecycleTest(unittest.TestCase):
    def test_apply(self):
        frames = fixture_lines('kalshi_lifecycle.jsonl')
        from recorder.kalshi import lifecycle_fields
        m = {'status': 'active', 'close_time': '2026-11-20T00:00:00Z'}
        self.assertIsNone(apply_lifecycle(m, lifecycle_fields(frames[0])))   # created: kept raw only
        closed = apply_lifecycle(m, lifecycle_fields(frames[1]))
        self.assertEqual(closed['close_time'], '2026-11-06T01:00:00Z')        # the game ended early (1793926800)
        self.assertEqual(m['close_time'], '2026-11-20T00:00:00Z')             # the cached row is not edited
        self.assertEqual(apply_lifecycle(m, lifecycle_fields(frames[2])), dict(m, status='determined', result='yes'))
        self.assertEqual(apply_lifecycle(m, lifecycle_fields(frames[3]))['status'], 'settled')
        self.assertIsNone(apply_lifecycle(m, lifecycle_fields(frames[4])))


def iso(t):
    import datetime
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


class FakeWS:
    def __init__(self):
        self.sent = []

    async def send(self, text):
        self.sent.append(json.loads(text))


@unittest.skipUnless(HAVE_CRYPTO, 'cryptography is not installed (pip install -r recorder/requirements.txt)')
class RecorderListsTest(unittest.TestCase):
    def setUp(self):
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric import ed25519
        self.dir = tempfile.TemporaryDirectory()
        pem = ed25519.Ed25519PrivateKey.generate().private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
        self.s = config.Settings(rest_url='https://127.0.0.1:9/trade-api/v2', ws_url='wss://127.0.0.1:9/trade-api/ws/v2',
                                 data_dir=self.dir.name, book_series=['KXNATGAS15M'], college_series=['KXNCAAWBGAME'],
                                 control_series=['KXNCAAMBGAME'], key_id='k', key_pem=pem, rest_fallback_s=0,
                                 control_pick_h=0)
        self.r = Recorder(self.s, Store(self.dir.name, 3600, lambda m: None))
        self.now = time.time()
        t = time.gmtime(self.now)   # the event tickers' date, KXNCAAMBGAME-26OCT21..., in any locale
        self.day = '%02d%s%02d' % (t.tm_year % 100, 'JAN FEB MAR APR MAY JUN JUL AUG SEP OCT NOV DEC'.split()[t.tm_mon - 1],
                                   t.tm_mday)
        self.window = {'ticker': 'KXNATGAS15M-W1', 'status': 'active', 'open_time': iso(self.now - 60),
                       'close_time': iso(self.now + 840)}
        self.wb = [{'ticker': 'KXNCAAWBGAME-%sUCONNSC-%s' % (self.day, t), 'event_ticker': 'KXNCAAWBGAME-%sUCONNSC' % self.day,
                    'status': 'active', 'open_time': iso(self.now - 86400), 'close_time': iso(self.now + 7 * 86400)}
                   for t in ('UCONN', 'SC')]
        self.mb = [{'ticker': 'KXNCAAMBGAME-%s%s-A' % (self.day, g), 'event_ticker': 'KXNCAAMBGAME-%s%s' % (self.day, g),
                    'status': 'active', 'volume_fp': v, 'close_time': iso(self.now + 86400)}
                   for g, v in (('THIN', '1.00'), ('BIG', '900.00'))]

    def tearDown(self):
        self.r.store.close()
        self.dir.cleanup()

    def see_all(self):
        self.r.see_market('KXNATGAS15M', self.window, 'book')
        for m in self.wb:
            self.r.see_market('KXNCAAWBGAME', m, 'college')
        for m in self.mb:
            self.r.see_market('KXNCAAMBGAME', m, 'control')

    def test_lists(self):
        self.see_all()
        self.r.recompute(self.now)
        wb = {m['ticker'] for m in self.wb}
        self.assertEqual(self.r.desired, {'KXNATGAS15M-W1'} | wb)      # no control game before the pick
        self.assertEqual(self.r.college, wb)
        self.assertEqual(self.r.lifecycle, self.r.desired)
        self.r.pick_control(self.now)
        self.r.recompute(self.now)
        thin = 'KXNCAAMBGAME-%sTHIN-A' % self.day
        self.assertIn(thin, self.r.desired)
        self.assertNotIn('KXNCAAMBGAME-%sBIG-A' % self.day, self.r.desired)
        self.r.store.flush()
        ranked = spooled(self.dir.name, 'college_control')
        self.assertEqual([(x['event_ticker'].split('-')[1][7:], x['picked']) for x in ranked],
                         [('THIN', True), ('BIG', False)])
        # REST books: the college list; the book series only in fallback
        self.r.book_live = True
        self.assertEqual(self.r.rest_listing(), {t: 'college' for t in wb | {thin}})
        self.r.book_live = False                     # REC_REST_FALLBACK_S=0 here: at once
        listed = self.r.rest_listing()
        self.assertEqual(listed['KXNATGAS15M-W1'], 'fallback')
        self.r.book_live = True
        self.assertNotIn('KXNATGAS15M-W1', self.r.rest_listing())
        self.r.store.flush()
        self.assertEqual([g['kind'] for g in spooled(self.dir.name, 'gaps')], ['fallback_on', 'fallback_off'])
        # the game ends: settled by the lifecycle channel, it leaves the lists without waiting an hour
        uconn = self.wb[0]['ticker']
        self.r.on_lifecycle({'type': 'market_lifecycle_v2', 'msg': {'market_ticker': uconn, 'event_type': 'settled'}})
        self.assertNotIn(uconn, self.r.desired)
        self.assertNotIn(uconn, self.r.lifecycle)
        self.assertIn(uconn, self.r.recorded_between(int((self.now - 60) * 1e6), int(self.now * 1e6)))

    def test_sync_explicit_lists(self):
        self.see_all()
        self.r.recompute(self.now)
        from recorder.rec import Session
        ws, sess = FakeWS(), Session(1)
        asyncio.run(self.r.sync(ws, sess))
        by = {c['params']['channels'][0]: c['params'] for c in ws.sent}
        self.assertEqual(set(by), {'orderbook_delta', 'trade', 'market_lifecycle_v2'})
        self.assertEqual(set(by['market_lifecycle_v2']['market_tickers']), self.r.lifecycle)
        self.assertTrue(all(p['market_tickers'] for p in by.values()))   # never an empty list
        # a venue that refuses the lifecycle list: not asked again for ten minutes
        lid = next(i for i, p in sess.pending.items() if p[0] == 'market_lifecycle_v2')
        self.r.changed = asyncio.Event()

        async def refuse():
            class Once:
                def __init__(self, frames):
                    self.frames = frames

                def __aiter__(self):
                    return self

                async def __anext__(self):
                    if not self.frames:
                        raise StopAsyncIteration
                    return self.frames.pop(0)
            await self.r.read(Once([json.dumps({'id': lid, 'type': 'error', 'msg': {'code': 11, 'msg': 'x'}})]), sess)
        asyncio.run(refuse())
        self.assertGreater(sess.hold['market_lifecycle_v2'], time.monotonic())
        ws.sent.clear()
        asyncio.run(self.r.sync(ws, sess))
        self.assertNotIn('market_lifecycle_v2', [c['params'].get('channels', [None])[0] for c in ws.sent])
        # with no markets at all, nothing is subscribed: an empty list is never sent
        r2 = Recorder(self.s, self.r.store)
        ws2 = FakeWS()
        asyncio.run(r2.sync(ws2, Session(2)))
        self.assertEqual(ws2.sent, [])

    def test_discovery_hourly_for_college(self):
        def handler(route, p):
            self.assertEqual(route, '/markets')
            src = {'KXNATGAS15M': [self.window], 'KXNCAAWBGAME': self.wb, 'KXNCAAMBGAME': self.mb}[p['series_ticker']]
            return {'markets': src if p['status'] == 'open' else [], 'cursor': ''}
        self.r.venue, rest = fake_venue(handler)
        self.r.changed = asyncio.Event()
        asyncio.run(self.r.discover_once())
        listed = {(p['series_ticker'], p['status']) for _, p in rest.calls}
        self.assertIn(('KXNCAAWBGAME', 'settled'), listed)
        self.assertIn(('KXNCAAMBGAME', 'open'), listed)
        self.assertIn('KXNCAAMBGAME-%sTHIN-A' % self.day, self.r.desired)   # listed and picked in one round
        self.assertTrue(self.r.changed.is_set())
        rest.calls.clear()
        asyncio.run(self.r.discover_once())
        self.assertEqual({p['series_ticker'] for _, p in rest.calls}, {'KXNATGAS15M'})   # the games: hourly

    def test_restart_is_a_gap(self):
        st = self.r.store
        st.write('kalshi_ws', {'recv_us': int((self.now - 90) * 1e6), 'mono_us': 0, 'conn': 1, 'type': 'x',
                               'sid': None, 'seq': None, 'ticker': None, 'raw': '{}'})
        st.flush()
        self.r.note_restart()
        st.flush()
        g = [x for x in spooled(self.dir.name, 'gaps') if x['kind'] == 'restart']
        self.assertEqual(len(g), 1)
        self.assertEqual(g[0]['from_us'], int((self.now - 90) * 1e6))
        self.assertEqual(len(self.r.backfill.windows), 1)
        self.assertEqual(self.r.backfill.windows[0][2], 'restart')

    def test_restart_starts_at_the_socket_last_frame(self):
        # the socket dropped 150 s ago; until the watchdog killed the recorder 30 s ago it went on
        # writing disconnect rows on its backoff and REST books
        st = self.r.store
        drop = int((self.now - 150) * 1e6)
        st.write('kalshi_ws', {'recv_us': drop, 'mono_us': 0, 'conn': 1, 'type': 'trade', 'sid': 2, 'seq': 9,
                               'ticker': None, 'raw': '{}'})
        for d in (0, 1, 3, 7, 15, 31, 61, 91):
            st.write('gaps', {'recv_us': drop + d * 1_000_000, 'source': 'kalshi_ws', 'kind': 'disconnect',
                              'detail': '', 'from_us': drop, 'to_us': None})
        st.write('kalshi_rest_books', {'recv_us': drop + 120_000_000, 'kind': 'top', 'ticker': 'X'})
        st.flush()
        self.r.note_restart()
        st.flush()
        g = [x for x in spooled(self.dir.name, 'gaps') if x['kind'] == 'restart']
        self.assertEqual(g[0]['from_us'], drop)                  # the whole stretch since the socket's last frame
        self.assertIn('not recording for 30 s', g[0]['detail'])
        self.assertEqual(self.r.backfill.windows, [(drop - polls.TradeBackfill.PAD_US,
                                                    g[0]['to_us'] + polls.TradeBackfill.PAD_US, 'restart')])

    def test_unread_backfill_windows_survive_a_restart(self):
        # a run restarted 10 minutes ago after an hour's outage, had a sequence gap on trades, and
        # was killed before either window was read
        st = self.r.store
        self.r.backfill.add(int((self.now - 3600) * 1e6), int((self.now - 600) * 1e6), 'restart')
        self.r.backfill.add(int((self.now - 400) * 1e6), int((self.now - 390) * 1e6), 'seq_gap')
        st.write('kalshi_ws', {'recv_us': int((self.now - 30) * 1e6), 'mono_us': 0, 'conn': 2, 'type': 'trade',
                               'sid': 2, 'seq': 10, 'ticker': None, 'raw': '{}'})
        st.flush()
        r2 = Recorder(self.s, st)
        r2.note_restart()
        got = polls.merge_windows(r2.backfill.windows)
        pad = polls.TradeBackfill.PAD_US
        self.assertEqual([(a, w) for a, _, w in got], [(int((self.now - 3600) * 1e6) - pad, 'restart'),
                                                      (int((self.now - 400) * 1e6) - pad, 'seq_gap'),
                                                      (int((self.now - 30) * 1e6) - pad, 'restart')])
        # once read, nothing is left to queue again
        r2.backfill.venue, _ = fake_venue(lambda route, p: {'trades': [], 'cursor': ''})
        asyncio.run(r2.backfill.drain())
        r3 = Recorder(self.s, st)
        r3.note_restart()
        self.assertEqual([w for _, _, w in r3.backfill.windows], ['restart'])

    # ---- a restart's trade backfill: the markets that were recordable while it was down -------
    STATUS = {'active': 'open', 'initialized': 'unopened', 'closed': 'closed', 'determined': 'closed',
              'settled': 'settled', 'finalized': 'settled'}

    def restart(self, down_s, listing):
        """The recorder restarted down_s after its last frame: the venue lists `listing` ({series:
        [rows]}, closed and settled rows from min_close_ts on, as Kalshi does). One discovery round,
        then the restart's backfill drained: (the tickers whose trades it read, the listing calls)."""
        read = []

        def handler(route, p):
            if route == '/markets/trades':
                read.append(p['ticker'])
                return {'trades': [], 'cursor': ''}
            rows = [m for m in listing.get(p['series_ticker'], []) if self.STATUS[m['status']] == p['status']]
            if 'min_close_ts' in p:
                rows = [m for m in rows if iso(p['min_close_ts']) <= m['close_time']]
            return {'markets': rows, 'cursor': ''}
        self.r.venue, rest = fake_venue(handler)
        self.r.backfill.venue = self.r.venue
        self.r.changed = asyncio.Event()
        self.r.store.write('kalshi_ws', {'recv_us': int((self.now - down_s) * 1e6), 'mono_us': 0, 'conn': 1,
                                         'type': 'trade', 'sid': 2, 'seq': 9, 'ticker': None, 'raw': '{}'})
        self.r.store.flush()
        self.r.note_restart()
        asyncio.run(self.r.discover_once())
        asyncio.run(self.r.backfill.drain())
        return set(read), rest.calls

    def market(self, ticker, opened_s, closed_s, status, **kw):
        return dict({'ticker': ticker, 'event_ticker': ticker.rsplit('-', 1)[0], 'status': status,
                     'open_time': iso(self.now + opened_s), 'close_time': iso(self.now + closed_s)}, **kw)

    def test_restart_backfills_markets_closed_while_down(self):
        # down from 20 minutes ago: W0 traded for 8 minutes of that and closed 12 minutes ago
        w0 = self.market('KXNATGAS15M-W0', -27 * 60, -12 * 60, 'closed')
        old = self.market('KXNATGAS15M-WOLD', -4 * 3600, -3 * 3600, 'settled')   # over before the outage
        game = dict(self.wb[0], status='settled', close_time=iso(self.now - 600))  # a game that ended in it
        thin, big = [dict(m, status='closed', close_time=iso(self.now - 600)) for m in self.mb]
        read, _ = self.restart(1200, {'KXNATGAS15M': [w0, self.window, old], 'KXNCAAWBGAME': [game, self.wb[1]],
                                      'KXNCAAMBGAME': [thin, big]})
        self.assertNotIn(w0['ticker'], self.r.desired)          # not wanted now, but recorded while down
        self.assertEqual(read, {w0['ticker'], self.window['ticker'], game['ticker'], self.wb[1]['ticker'],
                                thin['ticker']})               # the thin control game is picked; BIG is not

    def test_restart_lists_closed_markets_back_to_its_window(self):
        # down for 8 hours: the first listing reaches back past the usual 6 h, to the window's start
        w = self.market('KXNATGAS15M-W7H', -7 * 3600 - 900, -7 * 3600, 'settled')
        read, calls = self.restart(8 * 3600, {'KXNATGAS15M': [w, self.window]})
        self.assertIn(w['ticker'], read)
        since = [p['min_close_ts'] for _, p in calls if p.get('series_ticker') == 'KXNATGAS15M' and 'min_close_ts' in p]
        self.assertEqual(len(since), 2)                          # closed and settled
        self.assertTrue(all(s <= self.now - 8 * 3600 - self.s.linger_s for s in since), since)
        # only once: the next listing of results is the usual 6 h
        self.r.venue.rest.calls.clear()
        self.r.last_settled = 0
        asyncio.run(self.r.discover_once())
        since = [p['min_close_ts'] for _, p in self.r.venue.rest.calls if 'min_close_ts' in p]
        self.assertTrue(since and all(s >= self.now - 6 * 3600 - 5 for s in since), since)


@unittest.skipUnless(HAVE_CRYPTO, 'cryptography is not installed (pip install -r recorder/requirements.txt)')
class ControlPickHourTest(unittest.TestCase):
    """The control decile is picked in the first discovery round at or after the pick hour, not
    at the control series' next hourly listing (the wall clock is the test's)."""

    def setUp(self):
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric import ed25519
        self.dir = tempfile.TemporaryDirectory()
        pem = ed25519.Ed25519PrivateKey.generate().private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
        self.s = config.Settings(rest_url='https://127.0.0.1:9/trade-api/v2', ws_url='wss://127.0.0.1:9/trade-api/ws/v2',
                                 data_dir=self.dir.name, book_series=['KXNATGAS15M'], college_series=[],
                                 control_series=['KXNCAAMBGAME'], key_id='k', key_pem=pem, control_pick_h=15)
        self.r = Recorder(self.s, Store(self.dir.name, 3600, lambda m: None))
        self.r.changed = asyncio.Event()
        self.clock = [0.0]
        self.fail = set()   # the times the control listing fails
        self.listed = []

        def handler(route, p):
            self.listed.append(p['series_ticker'])
            if p['series_ticker'] == 'KXNCAAMBGAME' and self.clock[0] in self.fail:
                raise OSError('503')
            if p['series_ticker'] != 'KXNCAAMBGAME' or p['status'] != 'open':
                return {'markets': [], 'cursor': ''}
            return {'markets': [{'ticker': 'KXNCAAMBGAME-26NOV%02d%s-A' % (d, g),
                                 'event_ticker': 'KXNCAAMBGAME-26NOV%02d%s' % (d, g), 'status': 'active',
                                 'volume_fp': v, 'close_time': iso(self.clock[0] + 86400)}
                                for d in (5, 6) for g, v in (('THIN', '1.00'), ('BIG', '900.00'))], 'cursor': ''}
        self.r.venue, _ = fake_venue(handler)

    def tearDown(self):
        self.r.store.close()
        self.dir.cleanup()

    def round_at(self, day, h, m, s=0):
        """One discovery round at that UTC time: whether it listed the control series."""
        self.clock[0] = datetime(2026, 11, day, h, m, s, tzinfo=timezone.utc).timestamp()
        self.listed.clear()
        with mock.patch.object(rec, 'time', types.SimpleNamespace(time=lambda: self.clock[0],
                                                                    monotonic=time.monotonic)):
            asyncio.run(self.r.discover_once())
        return 'KXNCAAMBGAME' in self.listed

    def test_picked_at_the_pick_hour(self):
        self.assertTrue(self.round_at(5, 14, 59))                     # the hourly listing, a minute early
        self.assertFalse(self.r.control.picked('KXNCAAMBGAME-26NOV05THIN'))
        self.assertTrue(self.round_at(5, 15, 0, 30))                  # listed again: the pick is due
        self.assertTrue(self.r.control.picked('KXNCAAMBGAME-26NOV05THIN'))
        self.assertIn('KXNCAAMBGAME-26NOV05THIN-A', self.r.desired)
        self.assertNotIn('KXNCAAMBGAME-26NOV05BIG-A', self.r.desired)
        self.assertFalse(self.round_at(5, 15, 1))                     # picked: hourly again
        # a failed listing at the pick hour is tried again COLLEGE_RETRY_S later, not every round
        self.assertTrue(self.round_at(6, 14, 59))
        self.fail.add(datetime(2026, 11, 6, 15, 0, 30, tzinfo=timezone.utc).timestamp())
        self.assertTrue(self.round_at(6, 15, 0, 30))
        self.assertFalse(self.r.control.picked('KXNCAAMBGAME-26NOV06THIN'))
        self.assertFalse(self.round_at(6, 15, 1))
        self.assertTrue(self.round_at(6, 15, 5, 30))
        self.assertTrue(self.r.control.picked('KXNCAAMBGAME-26NOV06THIN'))


if __name__ == '__main__':
    unittest.main()
