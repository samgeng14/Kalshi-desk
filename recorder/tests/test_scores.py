"""The scoreboards: URL builders, the ESPN and NCAA parsers on fixtures, a change written once with
its first-seen time (the clock alone is no change; a game in progress's clock at most every
clock_s; line scores and, in the digest leagues, the last play are changes), and the poller's
cadence and outages; the summaries: the plays parser, each play's first-seen and revised rows,
and only the games in progress read, every every_s. A fake fetch throughout (no network)."""
import asyncio
import copy
import json
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

    def test_summary(self):
        self.assertEqual(scores.summary_url('https://site.api.espn.com/apis/site/v2/sports/', 'basketball/nba', '401810101'),
                         'https://site.api.espn.com/apis/site/v2/sports/basketball/nba/summary?event=401810101')
        self.assertEqual(scores.summary_url('https://e.test', 'basketball/womens-college-basketball:50', '4 01'),
                         'https://e.test/basketball/womens-college-basketball/summary?event=4%2001')
        self.assertTrue(scores.in_leagues('basketball/nba', ['basketball/nba']))
        self.assertTrue(scores.in_leagues('basketball/womens-college-basketball:50', ['basketball/womens-college-basketball']))
        self.assertFalse(scores.in_leagues('basketball/nba', []))
        self.assertFalse(scores.in_leagues('basketball/wnba', ['basketball/nba']))

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

    def test_espn_nba(self):
        g = scores.espn_games(fixture('espn_nba_scoreboard.json'), 'basketball/nba')
        self.assertEqual([(x['game_id'], x['state']) for x in g],
                         [('401810101', 'in'), ('401810102', 'pre'), ('401810100', 'post')])
        live = g[0]
        self.assertEqual((live['period'], live['clock'], live['home'], live['away'], live['home_score'],
                          live['away_score'], live['start_time']), ('2', '7:11', 'BOS', 'LAL', 38, 35, '2026-10-21T23:30Z'))
        raw = json.loads(live['raw'])
        last = raw['situation']['lastPlay']
        self.assertEqual((last['id'], last['probability']['homeWinPercentage']), ('4018101010142', 0.612))
        self.assertEqual(raw['situation']['possession'], '13')
        self.assertEqual([c['linescores'] for c in raw['competitors']], [[27.0, 11.0], [24.0, 11.0]])

    def test_espn_plays(self):
        p = scores.espn_plays(fixture('espn_nba_summary.json'), 'basketball/nba', '401810101')
        self.assertEqual([x['play_id'] for x in p], ['4018101010001', '4018101010004', '4018101010140',
                                                      '4018101010142'])   # the one without an id skipped
        lbj = p[1]
        self.assertEqual((lbj['seq'], lbj['period'], lbj['clock'], lbj['wallclock'], lbj['home_score'],
                          lbj['away_score'], lbj['team_id'], lbj['scoring'], lbj['score_value'], lbj['type'],
                          lbj['text'], lbj['wp_home'], lbj['wp_tie']),
                         (4, 1, '11:37', '2026-10-21T23:41:31Z', 0, 2, '13', True, 2, 'Jump Shot',
                          'LeBron James makes 21-foot two point shot', 0.555, 0.0))
        self.assertEqual((p[2]['scoring'], p[2]['wp_home']), (False, 0.598))   # a play that scores nothing
        self.assertIsNone(p[3]['wp_home'])                                      # the chart lags the last play
        self.assertEqual(json.loads(p[3]['raw'])['sequenceNumber'], '142')      # the play kept whole
        self.assertEqual(scores.espn_plays({}, 'x', '1'), [])
        self.assertEqual(scores.espn_plays({'plays': 'x', 'winprobability': 7}, 'x', '1'), [])
        self.assertEqual(scores.espn_plays([], 'x', '1'), [])


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

    def test_basketball_unchanged(self):
        # a basket moves the score and its period's line together: one row, as before the lines
        # were in the digest; NCAA.com's rows have no lines and change as before
        sb = fixture('espn_wcbb_scoreboard.json')
        book = scores.ScoreBook()
        book.changes(scores.espn_games(sb, 'wcbb'), 1000)
        basket = copy.deepcopy(sb)
        c = basket['events'][0]['competitions'][0]
        c['competitors'][0]['score'] = '36'
        c['competitors'][0]['linescores'][1]['value'] = 18.0
        c['status']['displayClock'] = '4:58'
        rows = book.changes(scores.espn_games(basket, 'wcbb'), 2000)
        self.assertEqual([(r['game_id'], r['home_score'], r['prev_us']) for r in rows], [('401820001', 36, 1000)])
        self.assertEqual(book.changes(scores.espn_games(basket, 'wcbb'), 3000), [])
        ncaa = fixture('ncaa_scoreboard.json')
        self.assertEqual(len(book.changes(scores.ncaa_games(ncaa, 'n'), 4000)), 3)
        tick = copy.deepcopy(ncaa)
        tick['games'][0]['game']['contestClock'] = '03:58'
        self.assertEqual(book.changes(scores.ncaa_games(tick, 'n'), 5000), [])

    def test_volleyball_points(self):
        # score counts sets; a point inside the set moves only that set's line: a row each point
        sb = fixture('espn_wvb_scoreboard.json')
        book = scores.ScoreBook()
        first = book.changes(scores.espn_games(sb, 'volleyball/womens-college-volleyball'), 1000)
        self.assertEqual([(r['home_score'], r['away_score'], r['period']) for r in first], [(1, 1, '3')])
        self.assertIn('"linescores":[25.0,21.0,12.0]', first[0]['raw'])
        t, prev = 2000, 1000
        for home, away in ((13, 10), (13, 11), (14, 11)):
            pt = copy.deepcopy(sb)
            comps = pt['events'][0]['competitions'][0]['competitors']
            comps[0]['linescores'][2]['value'], comps[1]['linescores'][2]['value'] = float(home), float(away)
            rows = book.changes(scores.espn_games(pt, 'volleyball/womens-college-volleyball'), t)
            self.assertEqual(len(rows), 1, (home, away))
            self.assertEqual((rows[0]['home_score'], rows[0]['away_score'], rows[0]['prev_us']), (1, 1, prev))
            self.assertIn('"linescores":[25.0,21.0,%.1f]' % home, rows[0]['raw'])
            self.assertEqual(book.changes(scores.espn_games(pt, 'volleyball/womens-college-volleyball'), t + 500), [])
            t, prev = t + 1000, t + 500

    def test_clock_rows(self):
        # a game in progress whose clock moves every 4 s poll: a row at most every 30 s
        sb = fixture('espn_nba_scoreboard.json')
        book = scores.ScoreBook(clock_s=30)
        self.assertEqual(len(book.changes(scores.espn_games(sb, 'basketball/nba'), 0)), 3)
        rows = []
        for i in range(1, 31):   # two minutes of polls, the clock running
            tick = copy.deepcopy(sb)
            for e in tick['events'][:2]:   # the game in progress, and the pregame one's clock too
                e['competitions'][0]['status']['displayClock'] = '%d:%02d' % divmod(431 - 4 * i, 60)
            rows += book.changes(scores.espn_games(tick, 'basketball/nba'), i * 4_000_000)
        self.assertEqual([(r['game_id'], r['recv_us']) for r in rows],
                         [('401810101', u) for u in (32_000_000, 64_000_000, 96_000_000)])
        self.assertTrue(all((r['home_score'], r['away_score']) == (38, 35) for r in rows))   # the scores as they stand
        self.assertEqual(rows[0]['clock'], '6:39')
        self.assertEqual(rows[0]['prev_us'], 28_000_000)   # prev_us is still the poll before
        # it moved since the last row (5:35 then, 5:11 now): one row; then it stopped (a timeout):
        # no rows, however long
        self.assertEqual([r['clock'] for r in book.changes(scores.espn_games(tick, 'basketball/nba'), 200_000_000)],
                         ['5:11'])
        self.assertEqual(book.changes(scores.espn_games(tick, 'basketball/nba'), 600_000_000), [])
        # off: the clock alone is never a change
        off = scores.ScoreBook(clock_s=0)
        off.changes(scores.espn_games(sb, 'basketball/nba'), 0)
        moved = copy.deepcopy(sb)
        moved['events'][0]['competitions'][0]['status']['displayClock'] = '1:00'
        self.assertEqual(off.changes(scores.espn_games(moved, 'basketball/nba'), 600_000_000), [])

    def test_last_play_digest(self):
        # in a digest league a new last play is a row, scoring or not; elsewhere it is not
        sb = fixture('espn_nba_scoreboard.json')
        nba = scores.ScoreBook(clock_s=0, digest_leagues=['basketball/nba'])
        other = scores.ScoreBook(clock_s=0)
        for b in (nba, other):
            b.changes(scores.espn_games(sb, 'basketball/nba'), 1000)
        foul = copy.deepcopy(sb)
        foul['events'][0]['competitions'][0]['situation']['lastPlay'] = {
            'id': '4018101010143', 'text': 'Austin Reaves personal foul', 'scoreValue': 0,
            'probability': {'homeWinPercentage': 0.618, 'tiePercentage': 0.0}}
        rows = nba.changes(scores.espn_games(foul, 'basketball/nba'), 2000)
        self.assertEqual([(r['game_id'], r['home_score']) for r in rows], [('401810101', 38)])
        self.assertIn('"homeWinPercentage":0.618', rows[0]['raw'])
        self.assertEqual(nba.changes(scores.espn_games(foul, 'basketball/nba'), 3000), [])
        self.assertEqual(other.changes(scores.espn_games(foul, 'basketball/nba'), 2000), [])

    def test_plays_first_seen_and_revised(self):
        sm = fixture('espn_nba_summary.json')
        book = scores.PlayBook()
        first = book.changes(scores.espn_plays(sm, 'basketball/nba', '401810101'), 1000)
        self.assertEqual([(r['play_id'][-3:], r['recv_us'], r['prev_us'], r['revised']) for r in first],
                         [('001', 1000, None, False), ('004', 1000, None, False), ('140', 1000, None, False),
                          ('142', 1000, None, False)])
        self.assertEqual(book.changes(scores.espn_plays(sm, 'basketball/nba', '401810101'), 2000, 1000), [])
        # the next read: a new play, the last one's chart point arrives, an old play corrected
        later = copy.deepcopy(sm)
        later['plays'][1]['text'] = 'LeBron James makes 22-foot two point shot'
        later['plays'].append({'id': '4018101010145', 'sequenceNumber': '145', 'type': {'text': 'Timeout'},
                               'text': 'Lakers Full timeout', 'homeScore': 38, 'awayScore': 35,
                               'period': {'number': 2}, 'clock': {'displayValue': '7:02'}, 'scoringPlay': False})
        later['winprobability'].append({'playId': '4018101010142', 'homeWinPercentage': 0.612, 'tiePercentage': 0.0})
        rows = book.changes(scores.espn_plays(later, 'basketball/nba', '401810101'), 3000, 2000)
        self.assertEqual([(r['play_id'][-3:], r['revised'], r['prev_us']) for r in rows],
                         [('004', True, 2000), ('142', True, 2000), ('145', False, 2000)])
        self.assertEqual((rows[0]['text'], rows[1]['wp_home'], rows[2]['wp_home']),
                         ('LeBron James makes 22-foot two point shot', 0.612, None))
        # a point already recorded that moves (ESPN redrew its chart) is not followed; a corrected score is
        redrawn = copy.deepcopy(later)
        for p in redrawn['winprobability']:
            p['homeWinPercentage'] += 0.01
        self.assertEqual(book.changes(scores.espn_plays(redrawn, 'basketball/nba', '401810101'), 4000, 3000), [])
        redrawn['plays'][3]['homeScore'] = 39
        rows = book.changes(scores.espn_plays(redrawn, 'basketball/nba', '401810101'), 5000, 4000)
        self.assertEqual([(r['play_id'][-3:], r['home_score'], r['revised']) for r in rows], [('142', 39, True)])
        # the same play id in another game is another play
        self.assertEqual(len(book.changes(scores.espn_plays(sm, 'basketball/nba', '401810999'), 6000)), 4)

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

    def test_summaries_read_only_games_in_progress(self):
        board = {'v': fixture('espn_nba_scoreboard.json')}
        summary = fixture('espn_nba_summary.json')
        state = {'urls': [], 'down': False}
        live_url = 'https://espn.test/sports/basketball/nba/summary?event=401810101'

        def fetch(url):
            state['urls'].append(url)
            if '/scoreboard?' in url:
                return board['v']
            if state['down']:
                raise urllib.error.URLError('timed out')
            if url == live_url:
                return summary
            raise urllib.error.HTTPError(url, 404, 'Not Found', {}, None)
        clock = [at(2026, 10, 21, 23, 50)]
        feed = scores.Feed('espn', 'basketball/nba', 'https://espn.test/sports')
        boards = scores.Scoreboards([feed], self.store, gap_log(), fetch=fetch, clock=lambda: clock[0])
        gap = gap_log()
        sm = scores.Summaries([feed], self.store, gap, every_s=10, fetch=fetch, clock=lambda: clock[0])
        asyncio.run(sm.cycle())
        self.assertEqual(state['urls'], [])              # no scoreboard yet: no game known to be on
        asyncio.run(boards.poll(feed))
        state['urls'].clear()
        # every every_s, only the game in progress: not the pregame game, not the finished one
        slept = []

        async def go():
            stop = asyncio.Event()

            async def sleep(s):
                slept.append(s)
                clock[0] += s
                if len(slept) == 3:
                    stop.set()
            await sm.run(stop, sleep)
        asyncio.run(go())
        self.assertEqual(slept, [10, 10, 10])
        self.assertEqual(state['urls'], [live_url] * 3)
        self.store.flush()
        rows = spooled(self.dir.name, 'espn_plays')
        self.assertEqual([(r['play_id'][-3:], r['prev_us'], r['revised']) for r in rows],
                         [('001', None, False), ('004', None, False), ('140', None, False), ('142', None, False)])
        self.assertEqual(sm.rows, 4)
        self.assertEqual(gap.rows, [])
        # a failed read is an outage until a cycle reads every game
        state['down'] = True
        asyncio.run(sm.cycle())
        state['down'] = False
        asyncio.run(sm.cycle())
        self.assertEqual([(g['source'], g['kind']) for g in gap.rows], [('espn_plays', 'poll_failed'),
                                                                       ('espn_plays', 'recovered')])
        # the game ends: read once more for its last plays, then no more
        final = copy.deepcopy(board['v'])
        final['events'][0]['competitions'][0]['status']['type'].update(state='post', name='STATUS_FINAL')
        board['v'] = final
        asyncio.run(boards.poll(feed))
        state['urls'].clear()
        asyncio.run(sm.cycle())
        asyncio.run(sm.cycle())
        self.assertEqual(state['urls'], [live_url])

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
