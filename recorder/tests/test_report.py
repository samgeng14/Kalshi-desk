"""The gaps report on synthetic data: gap minutes per source and day, silent socket minutes, the
share of each day's recorded minutes, the 24-hour milestone verdict, rows and bytes per table; the
same figures from the spool, from Parquet read by pyarrow and by duckdb."""
import contextlib
import io
import json
import os
import shutil
import tempfile
import unittest

from recorder import report
from recorder.store import Store
from recorder.tests.util import HAVE_DUCKDB, HAVE_PYARROW

T0 = 1793919600 * 1_000_000 - 3600 * 1_000_000   # 2026-11-05T22:00:00Z
MIN = 60_000_000


def ws(t):
    return {'recv_us': t, 'mono_us': 0, 'conn': 1, 'type': 'pyth_value', 'sid': 1, 'seq': 1, 'ticker': 'X', 'raw': '{}'}


def gap(t, source, kind, a=None, b=None):
    return {'recv_us': t, 'source': source, 'kind': kind, 'detail': '', 'from_us': a, 'to_us': b}


def two_hours(store):
    """22:00-24:00 on the 5th and on into 00:00-00:?? on the 6th: frames every 10 s, none in minutes
    30-32 (a disconnect the gaps table logs as 30:00-33:00), one ESPN failure at 50, a fallback."""
    for i in range(0, 3 * 3600, 10):
        if 30 * 60 <= i < 33 * 60:
            continue
        store.write('kalshi_ws', ws(T0 + i * 1_000_000))
    store.write('gaps', gap(T0 + 33 * MIN, 'kalshi_ws', 'resumed', T0 + 30 * MIN, T0 + 33 * MIN))
    store.write('gaps', gap(T0 + 50 * MIN + 5, 'espn', 'poll_failed'))
    store.write('gaps', gap(T0 + 70 * MIN, 'kalshi_rest_books', 'fallback_on'))
    store.flush(fsync=True)


class Checks:
    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def check_two_hours(self, rep):
        days = rep['days']
        self.assertEqual(list(days), ['2026-11-05', '2026-11-06'])
        d5, d6 = days['2026-11-05'], days['2026-11-06']
        self.assertEqual(d5['minutes'], 120)                          # 22:00-23:59
        self.assertEqual(d5['sources'], {'kalshi_ws': 4, 'espn': 1})  # 30-33 inclusive; 50
        self.assertEqual(d5['silent_ws'], 3)                          # 30, 31, 32
        self.assertEqual(d5['any'], 5)
        self.assertAlmostEqual(d5['pct'], 5 / 120)
        self.assertFalse(d5['full_day'])
        self.assertEqual((d6['minutes'], d6['any'], d6['sources']), (60, 0, {}))
        self.assertEqual(rep['info'], {'kalshi_rest_books': 1})       # a fallback is not a gap
        self.assertFalse(rep['last_24h']['complete'])
        self.assertEqual(rep['last_24h']['gap_minutes'], 5)
        rows = {t: sum(v['rows'] for v in byday.values()) for t, byday in rep['tables'].items()}
        self.assertEqual(rows, {'kalshi_ws': 3 * 360 - 18, 'gaps': 3})


class SpoolTest(Checks, unittest.TestCase):
    def test_spool_only(self):
        s = Store(self.dir, 3600, lambda m: None)
        two_hours(s)
        s.close()
        rep = report.build(self.dir, engine=None)   # no Parquet reader needed for spools
        self.check_two_hours(rep)
        text = report.render(rep)
        self.assertIn('2026-11-05', text)
        self.assertIn('not yet a full day', text)
        self.assertIn('not gaps: kalshi_rest_books 1', text)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(report.main(['--data', self.dir, '--json', '--days', '1']), 0)
        self.assertEqual(list(json.loads(out.getvalue())['days']), ['2026-11-06'])
        self.assertEqual(report.main(['--data', os.path.join(self.dir, 'missing')]), 2)

    def test_full_day_verdict(self):
        s = Store(self.dir, 3600, lambda m: None)
        day0 = T0 + 2 * 3600 * 1_000_000   # 2026-11-06T00:00Z
        for m in range(1440):
            if not 600 <= m < 610:
                s.write('kalshi_ws', ws(day0 + m * MIN + 1))
        # the VM died at 10:00 and nothing logged it until the restart at 10:10
        s.write('gaps', gap(day0 + 610 * MIN, 'recorder', 'restart', day0 + 599 * MIN + 1, day0 + 610 * MIN))
        s.close()
        rep = report.build(self.dir, engine=None)
        d = rep['days']['2026-11-06']
        self.assertEqual((d['minutes'], d['silent_ws'], d['sources'], d['any'], d['full_day']),
                         (1440, 10, {'recorder': 12}, 12, True))
        l24 = rep['last_24h']
        self.assertEqual((l24['complete'], l24['gap_minutes'], l24['pass']), (True, 12, True))   # 0.83%
        self.assertIn('PASS', report.render(rep))
        s = Store(self.dir, 3600, lambda m: None)
        s.write('gaps', gap(day0 + 900 * MIN, 'kalshi_ws', 'disconnect', day0 + 895 * MIN))
        s.close()
        rep = report.build(self.dir, engine=None)
        self.assertEqual((rep['last_24h']['gap_minutes'], rep['last_24h']['pass']), (18, False))  # 1.25%
        self.assertIn('FAIL', report.render(rep))

    def test_torn_line_is_not_an_hour_of_gaps(self):
        s = Store(self.dir, 3600, lambda m: None)
        day0 = T0 + 2 * 3600 * 1_000_000   # 2026-11-06T00:00Z
        for m in range(1440):
            if m != 600:
                s.write('kalshi_ws', ws(day0 + m * MIN + 1))
        # killed at 10:00 mid-line, back 10 s later: the restart row is the outage
        s.write('gaps', gap(day0 + 600 * MIN + 11_000_000, 'recorder', 'restart', day0 + 600 * MIN + 1,
                            day0 + 600 * MIN + 11_000_000))
        # the torn line, found when the 10:00 hour was compacted: its row spans the whole bucket
        # (Store.note_compacted), which says where the line was, not that the hour was lost
        s.write('gaps', gap(day0 + 661 * MIN, 'store', 'spool_torn', day0 + 600 * MIN, day0 + 660 * MIN))
        s.close()
        rep = report.build(self.dir, engine=None)
        d = rep['days']['2026-11-06']
        self.assertEqual((d['sources'], d['silent_ws'], d['any']), ({'recorder': 1}, 1, 1))
        l24 = rep['last_24h']
        self.assertEqual((l24['complete'], l24['gap_minutes'], l24['pass']), (True, 1, True))
        self.assertEqual(rep['info'], {'store': 1})
        self.assertIn('not gaps: store 1', report.render(rep))

    def test_gap_minutes(self):
        self.assertEqual(list(report.gap_minutes({'recv_us': 5 * MIN + 1})), [5])
        self.assertEqual(list(report.gap_minutes({'recv_us': 9 * MIN, 'from_us': 7 * MIN})), [7, 8, 9])
        self.assertEqual(list(report.gap_minutes({'recv_us': 9 * MIN, 'from_us': 9 * MIN, 'to_us': 7 * MIN})), [7, 8, 9])
        self.assertEqual(list(report.gap_minutes({})), [])
        huge = report.gap_minutes({'recv_us': 10 ** 12 * MIN, 'from_us': 0})
        self.assertEqual(len(huge), report.MAX_SPAN_MIN + 1)   # a bad from_us cannot claim the epoch


@unittest.skipUnless(HAVE_PYARROW, 'pyarrow is not installed (pip install -r recorder/requirements.txt)')
class ParquetTest(Checks, unittest.TestCase):
    def compacted(self):
        s = Store(self.dir, 3600, lambda m: None)
        two_hours(s)
        s.close()
        written = Store(self.dir, 3600, lambda m: None).roll(T0 + 10 * 3600 * 1_000_000)
        self.assertEqual(len(written), 5)   # three hours of frames, two of gaps (22:xx and 23:10)
        self.assertEqual(os.listdir(os.path.join(self.dir, 'spool', 'kalshi_ws')), [])

    def test_pyarrow(self):
        self.compacted()
        rep = report.build(self.dir, engine='pyarrow')
        self.assertEqual(rep['skipped_files'], 0)
        self.check_two_hours(rep)
        self.assertGreater(sum(v['bytes'] for v in rep['tables']['kalshi_ws'].values()), 0)

    @unittest.skipUnless(HAVE_DUCKDB, 'duckdb is not installed')
    def test_duckdb(self):
        self.compacted()
        self.check_two_hours(report.build(self.dir, engine='duckdb'))

    def test_no_reader(self):
        self.compacted()
        rep = report.build(self.dir, engine=None)
        self.assertEqual(rep['skipped_files'], 5)   # counted from the manifest, not read
        self.assertEqual(sum(v['rows'] for v in rep['tables']['kalshi_ws'].values()), 3 * 360 - 18)
        self.assertEqual(rep['days'], {})
        self.assertIn('counted from the manifest only', report.render(rep))


if __name__ == '__main__':
    unittest.main()
