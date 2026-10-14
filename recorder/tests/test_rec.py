"""The recorder's pure parts: which markets to record, frame rows, sequence gaps."""
import json
import unittest

from recorder.rec import Seqs, frame_row, market_row, wanted

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


if __name__ == '__main__':
    unittest.main()
