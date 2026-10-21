"""The REST pollers against a fake REST and fixtures (no network): the rate limit, paging, thin books
written once per change with first-seen times and depth only for books that moved, trade backfill
deduplicated by trade_id, the incentive and daily sweeps."""
import asyncio
import json
import tempfile
import unittest

from recorder import kalshi, polls
from recorder.store import Store
from recorder.tests.util import fake_venue, fixture, gap_log, spooled

UCONN, SC = 'KXNCAAWBGAME-26NOV05UCONNSC-UCONN', 'KXNCAAWBGAME-26NOV05UCONNSC-SC'


class LimiterTest(unittest.TestCase):
    def test_bucket(self):
        t = [100.0]
        lim = polls.RateLimiter(5, clock=lambda: t[0])
        self.assertEqual([lim.delay() for _ in range(5)], [0.0] * 5)       # the burst
        self.assertAlmostEqual(lim.delay(), 0.2)                            # then one every 1/rate s,
        self.assertAlmostEqual(lim.delay(), 0.4)                            # queued in order
        t[0] += 10.0
        self.assertEqual(lim.delay(), 0.0)                                  # refilled, never past the burst
        self.assertEqual([lim.delay() for _ in range(4)], [0.0] * 4)
        self.assertGreater(lim.delay(), 0.0)


class Base(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.store = Store(self.dir.name, 3600, lambda m: None)

    def tearDown(self):
        self.store.close()
        self.dir.cleanup()

    def rows(self, table):
        self.store.flush()
        return spooled(self.dir.name, table)


class PagesTest(Base):
    def test_cursor_kinds_and_limits(self):
        pages = {'': {'markets': [1, 2], 'cursor': 'c2'}, 'c2': {'markets': [3], 'next_cursor': 'c3'},
                 'c3': {'markets': [], 'cursor': 'c4'}}
        venue, rest = fake_venue(lambda route, p: pages[p.get('cursor', '')])

        async def collect(n):
            return [x async for x in venue.pages('/markets', {'status': 'open'}, 'markets', n)]
        # an empty page ends it, even with a cursor
        got = asyncio.run(collect(10))
        self.assertEqual(got, [([1, 2], 'c2'), ([3], 'c3'), ([], '')])
        self.assertEqual(rest.calls[1], ('/markets', {'status': 'open', 'cursor': 'c2'}))
        rest.calls.clear()
        got = asyncio.run(collect(1))
        self.assertEqual(got, [([1, 2], 'c2')])   # cut short: the cursor left says so


class RestBooksTest(Base):
    def setUp(self):
        super().setUp()
        self.f = fixture('kalshi_rest.json')
        self.top = json.loads(json.dumps(self.f['markets_top']))
        self.book = json.loads(json.dumps(self.f['orderbook_fp']))
        self.t = [1000.0]
        self.listed = {UCONN: 'college', SC: 'college'}

        def handler(route, p):
            if route == '/markets':
                asked = p['tickers'].split(',')
                return {'markets': [m for m in self.top['markets'] if m['ticker'] in asked or m['ticker'] == 'NOT-ON-THE-LIST'],
                        'cursor': ''}
            if route.endswith('/orderbook'):
                if 'SC' in route.split('-')[-1]:
                    raise kalshi.HttpError(404, '{"error":{"code":"not_found"}}')
                return self.book
            raise AssertionError(route)
        self.venue, self.rest = fake_venue(handler)
        self.gap = gap_log()
        self.rb = polls.RestBooks(self.venue, self.store, self.gap, lambda: dict(self.listed), period_s=4, hot_s=300,
                                  clock=lambda: self.t[0])

    def cycle(self, budget=4.0):
        asyncio.run(self.rb.cycle(self.t[0] + budget))

    def test_first_seen_once_per_change(self):
        self.cycle()
        rows = self.rows('kalshi_rest_books')
        tops = [r for r in rows if r['kind'] == 'top']
        self.assertEqual(sorted(r['ticker'] for r in tops), [SC, UCONN])   # never a market off the list
        u = next(r for r in tops if r['ticker'] == UCONN)
        self.assertEqual((u['yes_bid'], u['yes_ask'], u['yes_ask_size'], u['volume'], u['mode'], u['polls'], u['prev_us']),
                         (0.41, 0.45, 7.0, 1520.0, 'college', 0, None))
        self.assertLessEqual(u['sent_us'], u['recv_us'])
        self.assertNotIn('rules_primary', u['raw'])
        depth = [r for r in rows if r['kind'] == 'depth']
        self.assertEqual([d['ticker'] for d in depth], [UCONN])   # SC's book is gone (404): skipped, not fatal
        self.assertEqual((depth[0]['yes_bid'], depth[0]['yes_ask']), (0.41, 0.45))
        self.assertEqual(json.loads(depth[0]['raw'])['yes_dollars'][1], ['0.4100', '12.00'])
        self.assertEqual(self.rest.calls[0][1]['tickers'], ','.join(sorted(self.listed)))
        # the same books again: no rows
        self.t[0] += 4
        self.cycle()
        self.assertEqual(len(self.rows('kalshi_rest_books')), 2 + 1)
        # UConn's ask moves: one top row, first seen now, with the poll before it and how many polls saw the old
        self.top['markets'][0]['yes_ask_dollars'] = '0.4400'
        self.book['orderbook_fp']['no_dollars'][1] = ['0.5600', '7.00']
        self.t[0] += 4
        self.cycle()
        new = self.rows('kalshi_rest_books')[3:]
        self.assertEqual(sorted((r['kind'], r['ticker']) for r in new), [('depth', UCONN), ('top', UCONN)])
        top = next(r for r in new if r['kind'] == 'top')
        self.assertEqual((top['yes_ask'], top['polls']), (0.44, 2))
        # prev_us is the second poll (the last to see the old ask), after the first row and before this one
        self.assertGreater(top['prev_us'], u['recv_us'])
        self.assertGreater(top['recv_us'], top['prev_us'])
        self.assertEqual(next(r for r in new if r['kind'] == 'depth')['yes_ask'], 0.44)
        self.assertEqual(self.gap.rows, [])

    def test_depth_only_while_moving_and_list_explicit(self):
        self.cycle()
        n = len(self.rest.calls)
        self.t[0] += 301   # nothing moved for longer than hot_s: tops only
        self.cycle()
        self.assertEqual([c[0] for c in self.rest.calls[n:]], ['/markets'])
        # a market leaves the list: forgotten, not polled; an empty list polls nothing at all
        self.listed = {UCONN: 'college'}
        self.cycle()
        self.assertEqual(self.rest.calls[-1][1]['tickers'], UCONN)
        self.assertNotIn(('top', SC), self.rb.last)
        self.listed = {}
        n = len(self.rest.calls)
        self.cycle()
        self.assertEqual(len(self.rest.calls), n)

    def test_budget_and_failure(self):
        self.cycle(budget=0)   # the next scan is already due: no depth this cycle
        self.assertEqual([c[0] for c in self.rest.calls], ['/markets'])

        async def run_once():
            stop = asyncio.Event()

            async def sleep(s):
                stop.set()
            await self.rb.run(stop, sleep)
        self.venue.rest.handler = lambda r, p: (_ for _ in ()).throw(OSError('connection reset'))
        asyncio.run(run_once())
        self.assertEqual([g['kind'] for g in self.gap.rows], ['poll_failed'])
        self.venue.rest.handler = lambda r, p: {'markets': []}
        asyncio.run(run_once())
        self.assertEqual([g['kind'] for g in self.gap.rows], ['poll_failed', 'recovered'])

    def test_batch_halves_on_refusal(self):
        many = {'KXT-%03d' % i: 'college' for i in range(250)}
        sizes = []

        def handler(route, p):
            n = len(p['tickers'].split(','))
            sizes.append(n)
            if n > 40:
                raise kalshi.HttpError(414, 'URI too long')
            return {'markets': []}
        venue, _ = fake_venue(handler)
        rb = polls.RestBooks(venue, self.store, gap_log(), lambda: many, clock=lambda: self.t[0])
        for _ in range(4):
            try:
                asyncio.run(rb.cycle(self.t[0]))
            except kalshi.HttpError:
                pass
        self.assertEqual(rb.batch, 25)                      # 100 -> 50 -> 25: then every batch is taken
        self.assertEqual(sizes[-10:], [25] * 10)
        self.assertEqual(sum(sizes[-10:]), 250)


class BackfillTest(Base):
    def test_merge(self):
        self.assertEqual(polls.merge_windows([(50, 60, 'hourly'), (0, 10, 'seq_gap'), (5, 20, 'disconnect'),
                                              (8, 9, 'seq_gap')]),
                         [(0, 20, 'seq_gap+disconnect'), (50, 60, 'hourly')])
        self.assertEqual(polls.merge_windows([]), [])

    def test_dedup_by_trade_id(self):
        f = fixture('kalshi_rest.json')
        calls = []

        def handler(route, p):
            calls.append(p)
            self.assertEqual(route, '/markets/trades')
            return f['trades_p2'] if p.get('cursor') == 't2' else f['trades_p1']
        venue, _ = fake_venue(handler)
        gap = gap_log()
        bf = polls.TradeBackfill(venue, self.store, gap, lambda a, b: {UCONN})
        bf.seed([{'trade_id': 'old', 'created_us': 1}])
        bf.add(1793920000_000000, 1793923000_000000, 'disconnect')
        bf.add(None, 5, 'x')                          # no window
        asyncio.run(bf.drain())
        rows = self.rows('kalshi_trades_rest')
        self.assertEqual([r['trade_id'] for r in rows], ['a1', 'a2', 'a3'])   # a1 came twice, written once
        self.assertEqual((rows[0]['ticker'], rows[0]['yes_price'], rows[0]['reason']), (UCONN, 0.45, 'disconnect'))
        # the window padded by a minute either side, in Unix seconds
        self.assertEqual((calls[0]['ticker'], calls[0]['min_ts'], calls[0]['max_ts']),
                         (UCONN, 1793920000 - 60, 1793923000 + 60 + 1))
        # the next hour returns the same prints: nothing new
        bf.add(1793920000_000000, 1793923000_000000, 'hourly')
        asyncio.run(bf.drain())
        self.assertEqual(len(self.rows('kalshi_trades_rest')), 3)
        # a restart seeded from disk does not write them again either
        bf2 = polls.TradeBackfill(venue, self.store, gap, lambda a, b: {UCONN})
        bf2.seed(self.rows('kalshi_trades_rest'))
        bf2.add(1793920000_000000, 1793923000_000000, 'restart')
        asyncio.run(bf2.drain())
        self.assertEqual(len(self.rows('kalshi_trades_rest')), 3)

    def test_failure_requeues(self):
        venue, _ = fake_venue(lambda r, p: (_ for _ in ()).throw(OSError('503')))
        bf = polls.TradeBackfill(venue, self.store, gap_log(), lambda a, b: {UCONN})
        bf.add(0, 10, 'seq_gap')
        with self.assertRaises(OSError):
            asyncio.run(bf.drain())
        self.assertEqual(len(bf.windows), 1)   # tried again on the next pass

    def test_unread_windows_saved(self):
        saves, at_read = [], {}

        def handler(route, p):
            at_read[p['min_ts']] = saves[-1]     # what a kill at this moment would leave on disk
            if p['min_ts'] == 940:
                raise OSError('503')
            return {'trades': [], 'cursor': ''}
        venue, _ = fake_venue(handler)
        bf = polls.TradeBackfill(venue, self.store, gap_log(), lambda a, b: {UCONN}, save=saves.append)
        bf.add(100_000_000, 200_000_000, 'restart')
        bf.add(1000_000_000, 1010_000_000, 'seq_gap')
        w1, w2 = (40_000_000, 260_000_000, 'restart'), (940_000_000, 1070_000_000, 'seq_gap')
        self.assertEqual(saves[-1], [w1, w2])
        with self.assertRaises(OSError):
            asyncio.run(bf.drain())
        self.assertEqual(at_read, {40: [w1, w2], 940: [w2]})   # the first read is off the list
        self.assertEqual(saves[-1], [w2])                          # the failed one stays
        # a new run queues again what was saved, not what is two days old or unreadable
        bf2 = polls.TradeBackfill(venue, self.store, gap_log(), lambda a, b: {UCONN})
        now = 3 * 86400 * 1_000_000
        self.assertEqual(bf2.restore([list(w2), [now - 10, now - 5, 'hourly'], ['x'], [5, 1, 'bad']], now), 1)
        self.assertEqual(bf2.windows, [(now - 10, now - 5, 'hourly')])
        self.assertEqual(bf2.restore(None, now), 0)


class SweepTest(Base):
    def test_incentives(self):
        f = fixture('kalshi_rest.json')
        seen = []

        def handler(route, p):
            seen.append(p)
            return f['incentives_p2'] if p.get('cursor') == 'i2' else f['incentives_p1']
        venue, _ = fake_venue(handler)
        r = asyncio.run(polls.incentives_sweep(venue, self.store, {'liquidity', 'volume'}))
        self.assertEqual((r['rows'], r['dropped'], r['cut'], r['error']), (2, 2, False, None))
        self.assertNotIn('type', seen[0])   # the venue ignores it; the filter is ours
        rows = self.rows('kalshi_incentives')
        self.assertEqual([x['incentive_type'] for x in rows], ['liquidity', 'volume'])
        self.assertEqual({x['sweep_us'] for x in rows}, {r['sweep_us']})
        self.assertEqual(rows[1]['paid_out'], True)

    def test_markets_daily_and_errors(self):
        pages = {'': {'markets': [{'ticker': 'A-1', 'event_ticker': 'A', 'status': 'active', 'volume_fp': '3.00',
                                   'rules_primary': 'long text', 'yes_bid_dollars': '0.1000'}], 'cursor': 'n'},
                 'n': {'markets': [{'ticker': 'B-1', 'status': 'active', 'open_interest': 7}], 'cursor': ''}}
        venue, rest = fake_venue(lambda route, p: pages[p.get('cursor', '')])
        r = asyncio.run(polls.markets_sweep(venue, self.store, {'status': 'open'}))
        self.assertEqual((r['rows'], r['cut'], r['error']), (2, False, None))
        self.assertEqual(rest.calls[0][1], {'status': 'open', 'limit': 1000})
        rows = self.rows('kalshi_markets_daily')
        self.assertEqual((rows[0]['ticker'], rows[0]['volume'], rows[0]['yes_bid'], rows[1]['open_interest']),
                         ('A-1', 3.0, 0.1, 7.0))
        self.assertNotIn('rules_primary', rows[0]['raw'])
        r = asyncio.run(polls.markets_sweep(venue, self.store, {'status': 'open'}, max_pages=1))
        self.assertTrue(r['cut'])
        bad, _ = fake_venue(lambda route, p: (_ for _ in ()).throw(kalshi.HttpError(429, 'too many requests')))
        r = asyncio.run(polls.markets_sweep(bad, self.store, {}))
        self.assertEqual((r['rows'], r['error']), (0, 'HTTP 429: too many requests'))
        r = asyncio.run(polls.incentives_sweep(bad, self.store, {'liquidity'}))
        self.assertIn('429', r['error'])


class OutageTest(unittest.TestCase):
    def test_one_span(self):
        gap = gap_log()
        o = polls.Outage(gap, 'espn')
        o.ok(5)
        self.assertEqual(gap.rows, [])
        o.failed('x', 10)
        o.failed('y', 20)
        o.ok(30)
        self.assertEqual([(g['kind'], g['from_us'], g['to_us']) for g in gap.rows],
                         [('poll_failed', 10, 10), ('poll_failed', 10, 20), ('recovered', 10, 30)])


if __name__ == '__main__':
    unittest.main()
