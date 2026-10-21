"""Polymarket's public books: the parsers on fixtures, which games are on the list and when, and the
poller against gateway.polymarket.us played by a stdlib server on loopback (fixture bodies, Age
and Date headers): the listing, a book written once per change with its headers, a failed read
an outage, a missing slug said once, only GETs with no key. Off unless configured. No network."""
import asyncio
import copy
import http.server
import tempfile
import threading
import unittest
from datetime import datetime, timezone
from urllib.parse import parse_qs, urlparse

from recorder import config, poly
from recorder.store import Store
from recorder.tests.util import fixture, gap_log, spooled


def at(y, mo, d, h, mi=0):
    return datetime(y, mo, d, h, mi, tzinfo=timezone.utc).timestamp()


LAL, PHX, NYK = 'aec-nba-lal-bos-2026-10-21', 'aec-nba-phx-gsw-2026-10-22', 'aec-nba-ny-cle-2026-10-21'
WX = 'wx-nyc-high-2026-10-21-b70'   # a named slug (weather, were it listed)
DATE = 'Wed, 21 Oct 2026 22:00:00 GMT'


class ParseTest(unittest.TestCase):
    def test_amounts(self):
        self.assertEqual([poly.amount(v) for v in ({'value': '0.385', 'currency': 'USD'}, '0.39', 0.4, 1)],
                         [0.385, 0.39, 0.4, 1.0])
        self.assertEqual([poly.amount(v) for v in (None, True, 'x', {'value': None}, [])], [None] * 5)

    def test_book(self):
        bids, asks = poly.book_levels(fixture('pm_book.json'))
        self.assertEqual(bids, [(0.385, 1200.0), (0.38, 500.0)])
        self.assertEqual(asks, [(0.39, 900.0), (0.4, 2500.0)])
        # best first whatever the order sent; a level without a price is dropped
        shuffled = {'bids': [{'px': '0.30', 'qty': '1'}, {'px': '0.31', 'qty': '2'}, {'qty': '5'}],
                    'asks': [{'px': '0.40', 'qty': '1'}, {'px': '0.35', 'qty': '3'}]}
        self.assertEqual(poly.book_levels(shuffled), ([(0.31, 2.0), (0.3, 1.0)], [(0.35, 3.0), (0.4, 1.0)]))
        self.assertEqual(poly.book_levels({'marketData': {}}), ([], []))     # an empty book is a book
        self.assertEqual(poly.book_levels({'marketData': {'bids': None, 'asks': []}}), ([], []))
        self.assertIsNone(poly.book_levels({'marketData': {'bids': 'x'}}))
        self.assertIsNone(poly.book_levels([]))

    def test_league_markets(self):
        ms = poly.league_markets(fixture('pm_league_events.json')['events'], 'nba')
        self.assertEqual([m['slug'] for m in ms], [LAL, PHX, NYK])      # the spread market is not a winner
        m = ms[0]
        self.assertEqual((m['league'], m['event_slug'], m['event_title'], m['sports_type'], m['start_time'],
                          m['active'], m['closed']),
                         ('nba', 'nba-lal-bos-2026-10-21', 'Lakers vs. Celtics', 'basketball_team_full_game_winner',
                          '2026-10-21T23:30:00Z', True, False))
        self.assertIn('"marketSides"', m['raw'])
        # the quotes move every second: they are not a change of the listing
        moved = copy.deepcopy(fixture('pm_league_events.json')['events'])
        moved[0]['markets'][0]['bestBidQuote'] = {'value': '0.40'}
        self.assertEqual(poly.league_markets(moved, 'nba')[0]['digest'], m['digest'])
        moved[0]['markets'][0]['closed'] = True
        self.assertNotEqual(poly.league_markets(moved, 'nba')[0]['digest'], m['digest'])
        self.assertTrue(poly.is_winner({'sportsMarketType': 'soccer_team_full_time_winner'}))
        self.assertFalse(poly.is_winner({'sportsMarketType': 'soccer_team_draw'}))
        self.assertEqual(poly.league_markets('x', 'nba'), [])

    def test_window(self):
        m = {'start_time': '2026-10-21T23:30:00Z', 'closed': False}
        tip = at(2026, 10, 21, 23, 30)
        self.assertFalse(poly.wanted_market(m, tip - 7201, 7200, 21600))
        self.assertTrue(poly.wanted_market(m, tip - 7200, 7200, 21600))
        self.assertTrue(poly.wanted_market(m, tip + 21600, 7200, 21600))
        self.assertFalse(poly.wanted_market(m, tip + 21601, 7200, 21600))
        self.assertFalse(poly.wanted_market(dict(m, closed=True), tip, 7200, 21600))
        self.assertFalse(poly.wanted_market({'start_time': None}, tip, 7200, 21600))

    def test_headers(self):
        self.assertEqual(poly.header_us(DATE), int(at(2026, 10, 21, 22) * 1e6))
        self.assertEqual((poly.header_us(None), poly.header_us('soon')), (None, None))
        self.assertEqual((poly.header_s('3'), poly.header_s(None), poly.header_s('x')), (3.0, None, None))


class Gateway(http.server.BaseHTTPRequestHandler):
    """gateway.polymarket.us on loopback: the nba listing and the books, every answer with Age
    and Date headers, every request kept with its headers."""
    requests = []
    books = {}
    listing = None      # the nba listing's first page (the fixture when None)
    fail = set()        # slugs (or 'nba' for the listing) answered 500

    def answer(self, code, body=None):
        import json
        data = json.dumps(body if body is not None else {'error': code}).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Age', '2')
        self.end_headers()
        self.wfile.write(data)

    def date_time_string(self, timestamp=None):   # the Date header send_response writes
        return DATE

    def do_GET(self):
        Gateway.requests.append(('GET', self.path, {k.lower(): v for k, v in self.headers.items()}))
        u = urlparse(self.path)
        if u.path == '/v2/leagues/nba/events':
            if 'nba' in self.fail:
                return self.answer(500)
            if parse_qs(u.query).get('offset') != ['0']:
                return self.answer(200, {'events': []})
            return self.answer(200, self.listing if self.listing is not None else fixture('pm_league_events.json'))
        if u.path.startswith('/v1/markets/') and u.path.endswith('/book'):
            slug = u.path[len('/v1/markets/'):-len('/book')]
            if slug in self.fail:
                return self.answer(500)
            if slug in self.books:
                return self.answer(200, self.books[slug])
        return self.answer(404)

    def do_POST(self):
        Gateway.requests.append(('POST', self.path, {}))
        self.answer(405)

    def log_message(self, *a):
        pass


class PollerTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.store = Store(self.dir.name, 3600, lambda m: None)
        Gateway.requests, Gateway.fail, Gateway.listing = [], set(), None
        Gateway.books = {LAL: fixture('pm_book.json'), PHX: fixture('pm_book.json'),
                         WX: {'marketData': {'bids': [{'px': {'value': '0.020'}, 'qty': '50'}], 'asks': []}}}
        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Gateway)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.clock = [at(2026, 10, 21, 22)]    # LAL-BOS tips at 23:30, PHX-GSW at 02:00
        self.gap = gap_log()
        self.pm = poly.PolyBooks(poly.Http('http://127.0.0.1:%d' % self.server.server_address[1]), self.store,
                                 self.gap, leagues=['nba'], slugs=[WX], book_s=3, list_s=600, pre_s=7200,
                                 game_s=21600, rps=15, clock=lambda: self.clock[0])

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.store.close()
        self.dir.cleanup()

    def paths(self):
        out = [p for _, p, _ in Gateway.requests]
        Gateway.requests.clear()
        return out

    def rows(self, table):
        self.store.flush()
        return spooled(self.dir.name, table)

    def test_listing_and_books(self):
        asyncio.run(self.pm.cycle())
        # the listing, then the explicit list: the game inside its window and the named slug; not
        # the later game, the finished one, or the spread market
        self.assertEqual(self.paths(), ['/v2/leagues/nba/events?limit=200&offset=0',
                                        '/v1/markets/%s/book' % LAL, '/v1/markets/%s/book' % WX])
        self.assertEqual([m['slug'] for m in self.rows('pm_markets')], [LAL, PHX, NYK])
        books = {r['slug']: r for r in self.rows('pm_books')}
        self.assertEqual(set(books), {LAL, WX})
        b = books[LAL]
        self.assertEqual((b['mode'], b['league'], b['bid'], b['bid_size'], b['ask'], b['ask_size'], b['age_s'],
                          b['date_us'], b['prev_us'], b['polls']),
                         ('game', 'nba', 0.385, 1200.0, 0.39, 900.0, 2.0, int(at(2026, 10, 21, 22) * 1e6), None, 0))
        self.assertLessEqual(b['sent_us'], b['recv_us'])
        self.assertIn('"bids"', b['raw'])
        self.assertEqual((books[WX]['mode'], books[WX]['league'], books[WX]['bid'], books[WX]['ask']),
                         ('listed', None, 0.02, None))
        # 3 s on: the books again (not the listing), unchanged (a new time in the body is no
        # change): no rows
        Gateway.books[LAL] = copy.deepcopy(Gateway.books[LAL])
        Gateway.books[LAL]['marketData']['timestamp'] = '2026-10-21T22:00:03Z'
        self.clock[0] += 3
        asyncio.run(self.pm.cycle())
        self.assertEqual(self.paths(), ['/v1/markets/%s/book' % LAL, '/v1/markets/%s/book' % WX])
        self.assertEqual(len(self.rows('pm_books')), 2)
        # the book moves: one row, after the two reads that saw the old one
        moved = copy.deepcopy(Gateway.books[LAL])
        moved['marketData']['asks'][0]['qty'] = '400.00'
        Gateway.books[LAL] = moved
        self.clock[0] += 3
        asyncio.run(self.pm.cycle())
        rows = [r for r in self.rows('pm_books') if r['slug'] == LAL]
        self.assertEqual(len(rows), 2)
        self.assertEqual((rows[1]['ask_size'], rows[1]['polls']), (400.0, 2))
        self.assertGreater(rows[1]['prev_us'], rows[0]['recv_us'])
        # four hours on the later game's window is open too; the listing is read again
        self.paths()
        self.clock[0] += 4 * 3600
        asyncio.run(self.pm.cycle())
        self.assertEqual(self.paths()[1:], ['/v1/markets/%s/book' % LAL, '/v1/markets/%s/book' % PHX,
                                            '/v1/markets/%s/book' % WX])
        self.assertEqual(len(self.rows('pm_markets')), 3)    # the listing did not change: no rows
        self.assertEqual(self.gap.rows, [])
        # only GETs, and no key: no signature, no X-PM-* header, no cookie
        self.pm.next_list = 0
        asyncio.run(self.pm.cycle())
        self.assertEqual(len(Gateway.requests), 4)
        for method, path, headers in Gateway.requests:
            self.assertEqual(method, 'GET')
            self.assertFalse([h for h in headers if h.startswith('x-pm') or h in ('authorization', 'cookie')], headers)
            self.assertEqual(headers['user-agent'], 'deskd-recorder/1')

    def test_outages(self):
        asyncio.run(self.pm.cycle())
        self.paths()
        # a book that fails is an outage until a cycle reads every book; the others are still read
        Gateway.fail = {WX}
        self.clock[0] += 3
        asyncio.run(self.pm.cycle())
        self.assertEqual(self.paths(), ['/v1/markets/%s/book' % LAL, '/v1/markets/%s/book' % WX])
        Gateway.fail = set()
        self.clock[0] += 3
        asyncio.run(self.pm.cycle())
        self.assertEqual([(g['source'], g['kind']) for g in self.gap.rows], [('pm_books', 'poll_failed'),
                                                                            ('pm_books', 'recovered')])
        self.assertIn(WX, self.gap.rows[0]['detail'])
        # a listing that fails keeps the league's markets, and is tried again a minute later
        self.gap.rows.clear()
        Gateway.fail = {'nba'}
        self.clock[0] += 600
        asyncio.run(self.pm.cycle())
        self.assertIn('/v1/markets/%s/book' % LAL, self.paths())
        self.assertEqual([(g['source'], g['kind']) for g in self.gap.rows], [('pm_listing', 'poll_failed')])
        self.assertEqual(self.pm.next_list, self.clock[0] + poly.LIST_RETRY_S)
        Gateway.fail = set()
        self.clock[0] += 60
        asyncio.run(self.pm.cycle())
        self.assertEqual(self.gap.rows[-1]['kind'], 'recovered')
        # a named slug that does not exist: said, not an outage; a game gone from the listing is
        # no longer read
        self.gap.rows.clear()
        self.paths()
        self.pm.slugs = [WX, 'wx-no-such-market']
        Gateway.listing = fixture('pm_league_events.json')
        del Gateway.listing['events'][0]
        self.clock[0] += 600
        asyncio.run(self.pm.cycle())
        self.assertEqual(self.paths()[1:], ['/v1/markets/wx-no-such-market/book', '/v1/markets/%s/book' % WX])
        self.assertEqual([(g['source'], g['kind']) for g in self.gap.rows], [('pm_books', 'not_found')])
        self.assertNotIn(LAL, self.pm.markets)

    def test_run_every_book_s(self):
        slept = []

        async def go():
            stop = asyncio.Event()

            async def sleep(s):
                slept.append(s)
                self.clock[0] += s
                if len(slept) == 3:
                    stop.set()
            await self.pm.run(stop, sleep)
        asyncio.run(go())
        self.assertEqual(slept, [3, 3, 3])
        self.assertEqual(sum(1 for p in self.paths() if p.endswith('/book')), 6)

    def test_nothing_named_reads_nothing(self):
        idle = poly.PolyBooks(self.pm.http, self.store, self.gap, leagues=[], slugs=[], rps=15,
                              clock=lambda: self.clock[0])
        asyncio.run(idle.cycle())
        self.assertEqual(Gateway.requests, [])


class SettingsTest(unittest.TestCase):
    def settings(self, **kw):
        return config.Settings(rest_url='https://127.0.0.1:9/trade-api/v2', ws_url='wss://127.0.0.1:9/trade-api/ws/v2',
                               data_dir='/nonexistent', **kw)

    def test_off_unless_configured(self):
        self.assertIsNone(poly.from_settings(self.settings(), None, gap_log()))
        p = poly.from_settings(self.settings(pm_leagues=['nba'], game_pre_s=3600, pm_rps=2), None, gap_log())
        self.assertEqual((p.http.base, p.leagues, p.slugs, p.book_s, p.pre_s), ('https://gateway.polymarket.us',
                                                                                ['nba'], [], 3.0, 3600))
        self.assertIsNotNone(poly.from_settings(self.settings(pm_slugs=[WX]), None, gap_log()))


if __name__ == '__main__':
    unittest.main()
