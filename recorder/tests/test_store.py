"""The store: spools to Parquet with a manifest, torn lines, a clock step, crash recovery."""
import hashlib
import json
import os
import tempfile
import importlib.util
import unittest

from recorder.store import Store, read_manifest

T0 = 1791648000 * 1_000_000   # 2026-10-10T16:00:00Z, an hour boundary


def row(t, n=0):
    return {'recv_us': t, 'mono_us': n, 'conn': 1, 'type': 'orderbook_delta', 'sid': 1, 'seq': n, 'ticker': 'TK', 'raw': '{}'}


HAVE_PYARROW = importlib.util.find_spec('pyarrow') is not None


@unittest.skipUnless(HAVE_PYARROW, 'pyarrow is not installed (pip install -r recorder/requirements.txt)')
class StoreTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = self.dir.name
        self.logs = []

    def tearDown(self):
        self.dir.cleanup()

    def store(self):
        return Store(self.root, 3600, self.logs.append)

    def test_hour_to_parquet(self):
        import pyarrow.parquet as pq
        s = self.store()
        for i in range(5):
            s.write('kalshi_ws', row(T0 + i * 1000, i))
        s.write('kalshi_ws', row(T0 + 3600 * 1_000_000, 9))   # the next hour
        s.write('gaps', {'recv_us': T0 + 10, 'source': 'kalshi_ws', 'kind': 'disconnect', 'detail': 'test',
                         'from_us': None, 'to_us': None})
        s.flush(fsync=True)
        # the hour is not over for 5 s after it ends: nothing yet
        self.assertEqual(s.roll(T0 + 3600 * 1_000_000 + 1_000_000), [])
        written = s.roll(T0 + 3600 * 1_000_000 + 6_000_000)
        self.assertEqual(len(written), 2)   # kalshi_ws and gaps of the first hour
        m = {e['table']: e for e in read_manifest(self.root)}
        self.assertEqual(m['kalshi_ws']['rows'], 5)
        self.assertEqual(m['kalshi_ws']['bucket'], '20261010T16')
        self.assertEqual(m['kalshi_ws']['t_min_us'], T0)
        self.assertEqual(m['kalshi_ws']['t_max_us'], T0 + 4000)
        path = os.path.join(self.root, m['kalshi_ws']['path'])
        self.assertEqual(m['kalshi_ws']['path'], 'parquet/kalshi_ws/date=2026-10-10/kalshi_ws-20261010T16.parquet')
        with open(path, 'rb') as f:
            self.assertEqual(hashlib.sha256(f.read()).hexdigest(), m['kalshi_ws']['sha256'])
        t = pq.read_table(path)
        self.assertEqual(t.num_rows, 5)
        self.assertEqual(t.column('seq').to_pylist(), [0, 1, 2, 3, 4])
        self.assertEqual(str(t.schema.field('recv_us').type), 'int64')
        # the spool is gone; the next hour's is still open
        self.assertFalse(os.path.exists(s.spool_path('kalshi_ws', T0)))
        self.assertTrue(os.path.exists(s.spool_path('kalshi_ws', T0 + 3600 * 1_000_000)))
        s.close()

    def test_crash_torn_line_and_recovery(self):
        s = self.store()
        s.write('kalshi_ws', row(T0, 1))
        s.write('kalshi_ws', row(T0 + 1, 2))
        s.close()
        with open(s.spool_path('kalshi_ws', T0), 'a') as f:
            f.write('{"recv_us": 17910')   # killed mid-line
        # a new process, an hour later: the crash's spool is compacted, the torn line counted
        s2 = self.store()
        written = s2.roll(T0 + 2 * 3600 * 1_000_000)
        self.assertEqual(len(written), 1)
        m = read_manifest(self.root)
        self.assertEqual((m[0]['rows'], m[0]['torn']), (2, 1))
        s2.flush(fsync=True)
        # the gap row lands in the current hour's spool
        spool_dir = os.path.join(self.root, 'spool', 'gaps')
        lines = []
        for name in os.listdir(spool_dir):
            with open(os.path.join(spool_dir, name)) as f:
                lines += [json.loads(l) for l in f]
        self.assertTrue(any(g['kind'] == 'spool_torn' for g in lines))
        s2.close()

    def test_clock_step_writes_a_part(self):
        s = self.store()
        s.write('kalshi_ws', row(T0, 1))
        s.flush(fsync=True)
        s.roll(T0 + 3600 * 1_000_000 + 6_000_000)
        # the wall clock steps back into the compacted hour
        s.write('kalshi_ws', row(T0 + 5, 2))
        s.flush(fsync=True)
        s.roll(T0 + 3600 * 1_000_000 + 7_000_000)
        paths = sorted(e['path'] for e in read_manifest(self.root))
        self.assertEqual(paths, ['parquet/kalshi_ws/date=2026-10-10/kalshi_ws-20261010T16.1.parquet',
                                 'parquet/kalshi_ws/date=2026-10-10/kalshi_ws-20261010T16.parquet'])
        s.close()

    def test_unknown_table(self):
        with self.assertRaises(KeyError):
            self.store().write('nope', {'recv_us': T0})

    def test_new_tables_to_parquet(self):
        import pyarrow.parquet as pq
        s = self.store()
        s.write('kalshi_rest_books', {'recv_us': T0, 'sent_us': T0 - 5, 'prev_us': None, 'polls': 0, 'kind': 'top',
                                      'mode': 'college', 'ticker': 'TK', 'yes_bid': 0.41, 'yes_bid_size': 12.0,
                                      'yes_ask': None, 'yes_ask_size': None, 'last_price': None, 'volume': 3.0,
                                      'raw': '{}'})
        s.write('kalshi_incentives', {'recv_us': T0, 'sweep_us': T0, 'market_ticker': 'TK', 'incentive_type': 'liquidity',
                                      'period_reward': 1.0, 'target_size': None, 'discount_factor_bps': None,
                                      'start_date': None, 'end_date': None, 'paid_out': True, 'raw': '{}'})
        s.write('espn_scores', {'recv_us': T0, 'prev_us': None, 'league': 'l', 'game_id': '1', 'start_time': None,
                                'state': 'in', 'status': 'S', 'period': '2', 'clock': '1:00', 'home': 'H', 'away': 'A',
                                'home_score': 3, 'away_score': None, 'detail': None, 'raw': '{}'})
        s.flush(fsync=True)
        self.assertEqual(len(s.roll(T0 + 3600 * 1_000_000 + 6_000_000)), 3)
        m = {e['table']: e for e in read_manifest(self.root)}
        t = pq.read_table(os.path.join(self.root, m['kalshi_incentives']['path']))
        self.assertEqual(str(t.schema.field('paid_out').type), 'bool')
        self.assertIsNone(pq.read_table(os.path.join(self.root, m['kalshi_rest_books']['path'])).column('yes_ask')[0].as_py())
        self.assertEqual(pq.read_table(os.path.join(self.root, m['espn_scores']['path'])).column('home_score').to_pylist(), [3])
        # read back (spool and Parquet), for the trade backfill's seed
        s.write('kalshi_rest_books', {'recv_us': T0 + 3600 * 1_000_000 + 7_000_000, 'ticker': 'TK2'})
        s.flush()
        got = sorted(r['ticker'] for r in s.read_recent('kalshi_rest_books', T0))
        self.assertEqual(got, ['TK', 'TK2'])
        self.assertEqual([r['ticker'] for r in s.read_recent('kalshi_rest_books', T0 + 1)], ['TK2'])
        self.assertEqual(s.last_recv_us(), T0 + 3600 * 1_000_000 + 7_000_000)
        s.close()


class ReadBackTest(unittest.TestCase):
    """What a restart reads before it writes: no pyarrow needed (spools are JSON lines)."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = self.dir.name

    def tearDown(self):
        self.dir.cleanup()

    def test_last_recv_and_state(self):
        from recorder.store import State
        s = Store(self.root, 3600, lambda m: None)
        self.assertIsNone(s.last_recv_us())                 # a new data directory
        s.write('kalshi_ws', row(T0 + 10, 1))
        s.write('gaps', {'recv_us': T0 + 20, 'source': 'x', 'kind': 'y', 'detail': '', 'from_us': None, 'to_us': None})
        s.write('kalshi_ws', row(T0 + 15, 2))
        s.close()
        with open(s.spool_path('gaps', T0), 'a') as f:
            f.write('{"recv_us": 99999999999999999')        # killed mid-line: the line before counts
        self.assertEqual(Store(self.root, 3600, lambda m: None).last_recv_us(), T0 + 20)
        self.assertEqual([r['seq'] for r in s.read_recent('kalshi_ws', T0 + 12)], [2])
        st = State(self.root)
        self.assertIsNone(st.get('x'))
        st.set('x', {'a': [1]})
        self.assertEqual(State(self.root).get('x'), {'a': [1]})
        with open(st.path, 'w') as f:
            f.write('{torn')
        logs = []
        self.assertEqual(State(self.root, logs.append).get('x'), None)   # unreadable: start empty, say so
        self.assertTrue(logs)


if __name__ == '__main__':
    unittest.main()
