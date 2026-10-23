"""Kalshi's REST replies and lifecycle frames read on fixtures: both number formats (fixed-point
dollars and the older cents), a missing value as None (never 0), the incentive filter done here."""
import unittest

from recorder import kalshi
from recorder.tests.util import fixture, fixture_lines


class NumberTest(unittest.TestCase):
    def test_num(self):
        self.assertEqual([kalshi.num(v) for v in ('0.4100', 12, 1.5, '', None, True, 'x', '  ')],
                         [0.41, 12.0, 1.5, None, None, None, None, None])

    def test_cursor(self):
        self.assertEqual(kalshi.cursor_of({'cursor': 'c1'}), 'c1')
        self.assertEqual(kalshi.cursor_of({'next_cursor': 'i2'}), 'i2')
        self.assertEqual(kalshi.cursor_of({'cursor': None}), '')
        self.assertEqual(kalshi.cursor_of({'cursor': 5}), '')


class BookTest(unittest.TestCase):
    def setUp(self):
        self.f = fixture('kalshi_rest.json')

    def test_fixed_point_and_cents_read_alike(self):
        a = kalshi.book_levels(self.f['orderbook_fp'])
        self.assertEqual(a[0], [(0.41, 12.0), (0.30, 5.0)])     # best first, whatever order sent
        self.assertEqual(a[1], [(0.55, 7.0), (0.50, 3.0)])
        b = kalshi.book_levels(self.f['orderbook_cents'])
        self.assertEqual(b, a[0:1] + ([(0.55, 7.0)],))
        best = kalshi.best_of(*a)
        self.assertEqual(best, {'yes_bid': 0.41, 'yes_bid_size': 12.0, 'yes_ask': 0.45, 'yes_ask_size': 7.0})

    def test_thin_and_empty(self):
        yes, no = kalshi.book_levels(self.f['orderbook_one_side'])
        self.assertEqual((yes, no), ([(0.01, 480.0)], []))
        self.assertEqual(kalshi.best_of(yes, no)['yes_ask'], None)   # no ask is not an ask at 0
        self.assertEqual(kalshi.book_levels(self.f['orderbook_empty']), ([], []))
        self.assertIsNone(kalshi.book_levels({'error': 'x'}))
        self.assertIsNone(kalshi.book_levels('not a dict'))

    def test_top_of_book(self):
        ms = self.f['markets_top']['markets']
        self.assertEqual(kalshi.top_of_book(ms[0]), {'yes_bid': 0.41, 'yes_bid_size': 12.0, 'yes_ask': 0.45,
                                                     'yes_ask_size': 7.0, 'last_price': 0.43, 'volume': 1520.0})
        old = kalshi.top_of_book(ms[1])
        self.assertEqual((old['yes_bid'], old['yes_ask'], old['last_price'], old['volume'], old['yes_bid_size']),
                         (0.55, 0.59, 0.57, 1490.0, None))


class TradeTest(unittest.TestCase):
    def test_fields(self):
        p1 = fixture('kalshi_rest.json')['trades_p1']['trades']
        a = kalshi.trade_fields(p1[0])
        self.assertEqual((a['trade_id'], a['yes_price'], a['no_price'], a['count'], a['taker_side']),
                         ('a1', 0.45, 0.55, 2.0, 'yes'))
        self.assertEqual(a['created_us'], 1793922062123000)
        b = kalshi.trade_fields(p1[1])   # cents, no NO price sent: derived
        self.assertEqual((b['yes_price'], b['no_price'], b['count']), (0.44, 0.56, 5.0))
        self.assertIsNone(kalshi.trade_fields({'trade_id': 'x'})['created_us'])


class IncentiveTest(unittest.TestCase):
    def test_types_filtered_here(self):
        f = fixture('kalshi_rest.json')
        progs = f['incentives_p1']['incentive_programs'] + f['incentives_p2']['incentive_programs']
        kept = [p['market_ticker'] for p in progs if kalshi.keep_incentive(p, {'liquidity', 'volume'})]
        self.assertEqual(kept, ['KXNATGAS15M-26NOV051500-00', 'KXHIGHNY-26NOV05-B60'])
        a = kalshi.incentive_fields(progs[0])
        self.assertEqual((a['period_reward'], a['target_size'], a['discount_factor_bps'], a['paid_out']),
                         (200000.0, 300.0, 5000.0, False))
        self.assertEqual(kalshi.incentive_fields(progs[2])['target_size'], 100.0)
        self.assertIsNone(kalshi.incentive_fields(progs[3])['paid_out'])
        self.assertFalse(kalshi.keep_incentive('junk', {'liquidity'}))


class LifecycleTest(unittest.TestCase):
    def test_frames(self):
        frames = fixture_lines('kalshi_lifecycle.jsonl')
        got = [kalshi.lifecycle_fields(j) for j in frames]
        self.assertEqual([g['event_type'] if g else None for g in got],
                         ['created', 'close_date_updated', 'determined', 'settled', 'deactivated', None])
        self.assertEqual((got[0]['open_ts'], got[0]['close_ts']), (1793750400, 1795046400))
        self.assertEqual((got[2]['result'], got[2]['determination_ts']), ('yes', 1793927100))
        self.assertIs(got[4]['is_deactivated'], True)
        self.assertIsNone(got[1]['result'])
        self.assertIsNone(kalshi.lifecycle_fields({'type': 'trade', 'msg': {'market_ticker': 'X'}}))
        self.assertIsNone(kalshi.lifecycle_fields({'type': 'market_lifecycle_v2', 'msg': {}}))

    def test_subscribe_is_an_explicit_list(self):
        with self.assertRaises(ValueError):
            kalshi.subscribe_cmd(1, 'market_lifecycle_v2', set())


if __name__ == '__main__':
    unittest.main()
