"""The scoreboards: URL builders, the ESPN and NCAA parsers on fixtures, a change written once with
its first-seen time (the clock alone is no change), and the poller's cadence and outages, with a
fake fetch (no network)."""
import asyncio
import copy
import tempfile
import unittest
import urllib.error
from datetime import date, datetime, timezone

from recorder import scores
from recorder.store import Store
from recorder.tests.util import fixture, gap_log, spooled


def at(y, mo, d, h, mi=0):
    return datetime(y, mo, d, h, mi, tzinfo=timezone.utc).timestamp()


class UrlTest(unittest.TestCase):
    def test_espn(self):
        base = 'https://site.api.espn.com/apis/site/v2/sports/'
        self.assertEqual(scores.espn_url(base, 'basketball/womens-college-basketball:50', date(2026, 11, 5)),
                         'https://site.api.espn.com/apis/site/v2/sports/basketball/womens-college-basketball/'
                         'scoreboard?dates=20261105&groups=50&limit=500')
        self.assertEqual(scores.espn_url(base, 'hockey/mens-college-hockey', date(2026, 1, 9)),
                         'https://site.api.espn.com/apis/site/v2/sports/hockey/mens-college-hockey/'
                         'scoreboard?dates=20260109&limit=500')

    def test_ncaa(self):
        self.assertEqual(scores.ncaa_url('https://data.ncaa.com/casablanca/scoreboard', 'basketball-women/d1',
                                         date(2026, 11, 5)),
                         'https://data.ncaa.com/casablanca/scoreboard/basketball-women/d1/2026/11/05/scoreboard.json')

    def test_days(self):
        # a US evening game runs past midnight UTC: yesterday's board is read until 10 UTC
        self.assertEqual(scores.poll_days(at(2026, 11, 6, 3)), [date(2026, 11, 5), date(2026, 11, 6)])
        self.assertEqual(scores.poll_days(at(2026, 11, 6, 9, 59)), [date(2026, 11, 5), date(2026, 11, 6)])
        self.assertEqual(scores.poll_days(at(2026, 11, 6, 10)), [date(2026, 11, 6)])


class ParseTest(unittest.TestCase):
    def test_espn(self):
        g = scores.espn_games(fixture('espn_wcbb_scoreboard.json'), 'wcbb')
        self.assertEqual([x['game_id'] for x in g], ['401820001', '401820002', '401820003'])   # the malformed one skipped
        live = g[0]
        self.assertEqual((live['state'], live['status'], live['period'], live['clock'], live['home'], live['away'],
                          live['home_score'], live['away_score'], live['detail'], live['start_time']),
                         ('in', 'STATUS_IN_PROGRESS', '2', '5:12', 'CONN', 'SC', 34, 31, '5:12 - 2nd',
                          '2026-11-05T23:00Z'))
        self.assertIn('"possession":"2579"', live['raw'])
        self.assertIn('"linescores":[18.0,16.0]', live['raw'])
        self.assertNotIn('logo', live['raw'])           # what a model reads, not the page around it
        self.assertEqual((g[1]['state'], g[1]['home_score']), ('pre', 0))
        self.assertEqual((g[2]['state'], g[2]['home'], g[2]['home_score'], g[2]['away_score']), ('post', 'UVM', 61, 58))
        self.assertEqual(scores.espn_games({}, 'x'), [])
        self.assertEqual(scores.espn_games({'events': 'nope'}, 'x'), [])

    def test_ncaa(self):
        g = scores.ncaa_games(fixture('ncaa_scoreboard.json'), 'basketball-women/d1')
        self.assertEqual([x['game_id'] for x in g], ['6305901', '6305902', '6305903'])
        self.assertEqual((g[0]['state'], g[0]['status'], g[0]['period'], g[0]['clock'], g[0]['home'], g[0]['away'],
                          g[0]['home_score'], g[0]['away_score'], g[0]['start_time']),
                         ('in', 'live', '2nd', '04:12', 'UCONN', 'S CAR', 34, 31, '2026-11-05T23:00Z'))
        self.assertEqual((g[1]['state'], g[1]['home_score'], g[1]['period']), ('pre', None, None))
        self.assertEqual((g[2]['state'], g[2]['detail']), ('post', 'FINAL'))
        self.assertEqual(scores.ncaa_games({'games': [{'game': None}, 'x']}, 'x'), [])


class ChangesTest(unittest.TestCase):
    def test_each_change_once(self):
        sb = fixture('espn_wcbb_scoreboard.json')
        book = scores.ScoreBook()
        first = book.changes(scores.espn_games(sb, 'wcbb'), 1000)
        self.assertEqual([(r['recv_us'], r['prev_us']) for r in first], [(1000, None)] * 3)
        self.assertEqual(book.changes(scores.espn_games(sb, 'wcbb'), 2000), [])   # nothing new
        tick = copy.deepcopy(sb)   # the clock ran: not a change
        tick['events'][0]['competitions'][0]['status']['displayClock'] = '4:40'
        self.assertEqual(book.changes(scores.espn_games(tick, 'wcbb'), 3000), [])
        score = copy.deepcopy(tick)
        score['events'][0]['competitions'][0]['competitors'][1]['score'] = '33'
        rows = book.changes(scores.espn_games(score, 'wcbb'), 4000)
        self.assertEqual(len(rows), 1)
        r = rows[0]
        # first seen at 4000; the poll before (3000) still showed 34-31: the basket fell in between
        self.assertEqual((r['game_id'], r['away_score'], r['recv_us'], r['prev_us'], r['clock']),
                         ('401820001', 33, 4000, 3000, '4:40'))
        half = copy.deepcopy(score)
        half['events'][0]['competitions'][0]['status']['type']['name'] = 'STATUS_HALFTIME'
        self.assertEqual(len(book.changes(scores.espn_games(half, 'wcbb'), 5000)), 1)
        # another league's game with the same id is another game
        self.assertEqual(len(book.changes(scores.espn_games(sb, 'mcbb'), 6000)), 3)

    def test_live(self):
        now = at(2026, 11, 5, 23, 50)
        self.assertTrue(scores.live([{'state': 'in'}], now))
        self.assertTrue(scores.live([{'state': 'pre', 'start_time': '2026-11-06T00:00Z'}], now))     # in 10 min
        self.assertFalse(scores.live([{'state': 'pre', 'start_time': '2026-11-06T01:00Z'}], now))
        self.assertTrue(scores.live([{'state': 'pre', 'start_time': '2026-11-05T23:00Z'}], now))     # delayed
        self.assertFalse(scores.live([{'state': 'post'}, {'state': 'pre', 'start_time': None}], now))


class PollerTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.store = Store(self.dir.name, 3600, lambda m: None)

    def tearDown(self):
        self.store.close()
        self.dir.cleanup()

    def test_rows_cadence_and_outages(self):
        espn_sb, ncaa_sb = fixture('espn_wcbb_scoreboard.json'), fixture('ncaa_scoreboard.json')
        state = {'espn_down': False, 'urls': []}

        def fetch(url):
            state['urls'].append(url)
            if 'espn' in url:
                if state['espn_down']:
                    raise urllib.error.URLError('timed out')
                return espn_sb
            if url.endswith('/2026/11/05/scoreboard.json'):
                return ncaa_sb
            raise urllib.error.HTTPError(url, 404, 'Not Found', {}, None)   # NCAA: no games that date

        clock = [at(2026, 11, 6, 3)]
        gap = gap_log()
        feeds = [scores.Feed('espn', 'basketball/womens-college-basketball:50', 'https://espn.test/sports'),
                 scores.Feed('ncaa', 'basketball-women/d1', 'https://ncaa.test/scoreboard')]
        sb = scores.Scoreboards(feeds, self.store, gap, fast_s=4, idle_s=60, fetch=fetch, clock=lambda: clock[0])

        async def go():
            for f in feeds:
                await sb.poll(f)
        asyncio.run(go())
        self.store.flush()
        self.assertEqual(len(spooled(self.dir.name, 'espn_scores')), 3)
        self.assertEqual(len(spooled(self.dir.name, 'ncaa_scores')), 3)
        self.assertEqual(gap.rows, [])           # NCAA's 404 for the 6th is no games, not an outage
        self.assertEqual(len(state['urls']), 4)  # two dates each before 10 UTC
        self.assertTrue(all(f.is_live for f in feeds))
        self.assertEqual(feeds[0].next_s, clock[0] + 4)   # a game is on: fast
        state['espn_down'] = True
        clock[0] += 4
        asyncio.run(sb.poll(feeds[0]))
        clock[0] += 4
        asyncio.run(sb.poll(feeds[0]))
        state['espn_down'] = False
        clock[0] += 4
        asyncio.run(sb.poll(feeds[0]))
        kinds = [(g['source'], g['kind']) for g in gap.rows]
        self.assertEqual(kinds, [('espn', 'poll_failed'), ('espn', 'poll_failed'), ('espn', 'recovered')])
        rec = gap.rows[-1]
        self.assertEqual(rec['to_us'] - rec['from_us'] > 0, True)   # the whole outage, for the report
        self.store.flush()
        self.assertEqual(len(spooled(self.dir.name, 'espn_scores')), 3)   # nothing changed while down
        # a day with no game on: idle
        quiet = scores.Scoreboards([scores.Feed('espn', 'x/y', 'https://e.test')], self.store, gap_log(), 4, 60,
                                   fetch=lambda u: {'events': []}, clock=lambda: at(2026, 11, 6, 12))
        asyncio.run(quiet.poll(quiet.feeds[0]))
        self.assertEqual(quiet.feeds[0].next_s, at(2026, 11, 6, 12) + 60)

    def test_a_hanging_feed_delays_only_itself(self):
        import time
        calls = {'slow': 0, 'fast': 0}

        def fetch(url):
            if 'slow' in url:
                calls['slow'] += 1
                time.sleep(0.6)
            else:
                calls['fast'] += 1
            return {'events': []}
        feeds = [scores.Feed('espn', 'x/slow', 'https://e.test'), scores.Feed('espn', 'x/fast', 'https://e.test')]
        sb = scores.Scoreboards(feeds, self.store, gap_log(), fast_s=0.05, idle_s=0.05, fetch=fetch,
                                clock=lambda: at(2026, 11, 6, 12) + time.monotonic())

        async def go():
            stop = asyncio.Event()
            asyncio.get_running_loop().call_later(0.4, stop.set)
            await sb.run(stop, asyncio.sleep)
        asyncio.run(go())
        self.assertEqual(calls['slow'], 1)
        self.assertGreaterEqual(calls['fast'], 3)


if __name__ == '__main__':
    unittest.main()
