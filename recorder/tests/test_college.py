"""The college games: dates from event tickers, the control decile ranked by pregame volume, picked
once per date at the pick hour and kept across a restart."""
import tempfile
import unittest
from datetime import datetime, timezone

from recorder.college import ControlPicks, event_date, event_volumes, rank_thinnest
from recorder.store import State


def at(y, mo, d, h, mi=0):
    return datetime(y, mo, d, h, mi, tzinfo=timezone.utc).timestamp()


def game(ev, team, vol):
    return {'ticker': '%s-%s' % (ev, team), 'event_ticker': ev, 'volume_fp': '%.2f' % vol}


class DateTest(unittest.TestCase):
    def test_event_date(self):
        self.assertEqual(event_date('KXNCAAMBGAME-26NOV05DUKEUNC'), '2026-11-05')
        self.assertEqual(event_date('KXTTSTARMATCH-26OCT070230FRALOS'), '2026-10-07')   # a time follows
        self.assertEqual(event_date('KXNCAAWBGAME-27JAN31UCONNSC'), '2027-01-31')
        for bad in ('KXNCAAMBGAME', 'KXNCAAMBGAME-26XYZ05DUKE', 'KXNCAAMBGAME-26FEB30AB', 'X-2', None, 7):
            self.assertIsNone(event_date(bad), bad)


class RankTest(unittest.TestCase):
    def test_thinnest_tenth(self):
        ms = [game('E%02d' % i, t, i * 10 + (1 if t == 'A' else 0)) for i in range(25) for t in ('A', 'B')]
        vols = event_volumes(ms + [{'ticker': 'no-event', 'volume_fp': '9'}])
        self.assertEqual(len(vols), 25)
        self.assertEqual(vols['E03'], 61.0)   # both markets of the game
        ranked, n = rank_thinnest(vols, 0.1)
        self.assertEqual(n, 3)               # ceil(2.5)
        self.assertEqual([e for e, _ in ranked[:n]], ['E00', 'E01', 'E02'])
        # ties broken by ticker; one event still picks one; none picks none
        self.assertEqual(rank_thinnest({'B': 0.0, 'A': 0.0, 'C': 5.0}, 0.1), ([('A', 0.0), ('B', 0.0), ('C', 5.0)], 1))
        self.assertEqual(rank_thinnest({}, 0.1), ([], 0))
        self.assertEqual(rank_thinnest({'A': 1.0}, 0.5)[1], 1)


class PicksTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.dir.cleanup()

    def listed(self, extra=()):
        ms = [game('KXNCAAMBGAME-26NOV05G%02d' % i, t, 100 - i) for i in range(12) for t in ('A', 'B')]
        ms.append(game('KXNCAAMBGAME-26NOV06TMRW', 'A', 0))   # tomorrow's game: not in today's ranking
        return {'KXNCAAMBGAME': ms + list(extra)}

    def test_once_per_date_at_the_hour(self):
        p = ControlPicks(0.1, 15, State(self.dir.name))
        self.assertEqual(p.update(self.listed(), at(2026, 11, 5, 14, 59), 1), [])   # before the pick hour
        self.assertFalse(p.picked('KXNCAAMBGAME-26NOV05G11'))
        rows = p.update(self.listed(), at(2026, 11, 5, 15, 5), 2)
        self.assertEqual(len(rows), 12)    # every game of the date ranked and recorded
        picked = [r['event_ticker'] for r in rows if r['picked']]
        self.assertEqual(picked, ['KXNCAAMBGAME-26NOV05G11', 'KXNCAAMBGAME-26NOV05G10'])   # ceil(1.2) = 2
        self.assertEqual((rows[0]['rank'], rows[0]['events'], rows[0]['game_date'], rows[0]['volume']),
                         (1, 12, '2026-11-05', 178.0))
        self.assertTrue(p.picked('KXNCAAMBGAME-26NOV05G11'))
        self.assertFalse(p.picked('KXNCAAMBGAME-26NOV05G00'))
        self.assertFalse(p.picked('KXNCAAMBGAME-26NOV06TMRW'))
        # later the same day: a newly listed, thinner game is not added, nothing is re-ranked
        late = self.listed([game('KXNCAAMBGAME-26NOV05LATE', 'A', 0)])
        self.assertEqual(p.update(late, at(2026, 11, 5, 22), 3), [])
        self.assertFalse(p.picked('KXNCAAMBGAME-26NOV05LATE'))
        # a restart in the evening keeps the morning's picks (the state file), it does not re-rank
        p2 = ControlPicks(0.1, 15, State(self.dir.name))
        self.assertTrue(p2.picked('KXNCAAMBGAME-26NOV05G10'))
        self.assertEqual(p2.update(late, at(2026, 11, 5, 23), 4), [])
        # the next day picks again
        nxt = {'KXNCAAMBGAME': [game('KXNCAAMBGAME-26NOV06TMRW', 'A', 0), game('KXNCAAMBGAME-26NOV06BIG', 'A', 9)]}
        rows = p2.update(nxt, at(2026, 11, 6, 15), 5)
        self.assertEqual([r['event_ticker'] for r in rows if r['picked']], ['KXNCAAMBGAME-26NOV06TMRW'])
        self.assertTrue(p2.picked('KXNCAAMBGAME-26NOV05G11'))   # yesterday's stay recorded until they end

    def test_old_dates_forgotten(self):
        p = ControlPicks(0.1, 0)
        for d in range(1, 12):
            p.update({'S': [game('KXS-26NOV%02dAB' % d, 'A', 1)]}, at(2026, 11, d, 1), d)
        self.assertEqual(len(p.picks), ControlPicks.KEEP_DAYS)
        self.assertFalse(p.picked('KXS-26NOV01AB'))
        self.assertTrue(p.picked('KXS-26NOV11AB'))


if __name__ == '__main__':
    unittest.main()
