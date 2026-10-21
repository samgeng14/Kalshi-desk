"""The game group's window: from REC_GAME_PRE_S before a game's scheduled start (occurrence_datetime
- 3 h, else 10:00 ET on the ticker's date) to REC_LINGER_S after its market closes; nothing once
settled."""
import unittest
from datetime import datetime, timezone

from recorder import games


def at(y, mo, d, h, mi=0):
    return datetime(y, mo, d, h, mi, tzinfo=timezone.utc).timestamp()


def iso(t):
    return datetime.fromtimestamp(t, timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


PRE, LINGER = 7200, 300
TIP = at(2026, 10, 21, 23, 30)   # 7:30 PM EDT


def nba(**kw):
    m = {'ticker': 'KXNBAGAME-26OCT21LALBOS-BOS', 'event_ticker': 'KXNBAGAME-26OCT21LALBOS', 'status': 'active',
         'open_time': iso(TIP - 5 * 86400), 'close_time': iso(TIP + 14 * 86400),
         'occurrence_datetime': iso(TIP + 3 * 3600)}
    m.update(kw)
    return {k: v for k, v in m.items() if v is not None}


class WindowTest(unittest.TestCase):
    def test_occurrence_time(self):
        m = nba()
        self.assertEqual(games.scheduled_start(m), TIP)
        self.assertEqual(games.game_window(m, PRE, LINGER), (TIP - PRE, TIP + 14 * 86400 + LINGER))
        # open for days before the game: not recorded until two hours before the tip
        self.assertFalse(games.wanted_game(m, TIP - 3 * 86400, PRE, LINGER))
        self.assertFalse(games.wanted_game(m, TIP - PRE - 1, PRE, LINGER))
        self.assertTrue(games.wanted_game(m, TIP - PRE, PRE, LINGER))
        self.assertTrue(games.wanted_game(m, TIP + 2 * 3600, PRE, LINGER))      # in the game

    def test_without_occurrence_time(self):
        # 10:00 ET on the ticker's date: EDT in October, EST in January
        self.assertEqual(games.scheduled_start(nba(occurrence_datetime=None)), at(2026, 10, 21, 14))
        jan = nba(occurrence_datetime=None, ticker='KXNBAGAME-27JAN15LALBOS-BOS', event_ticker='KXNBAGAME-27JAN15LALBOS')
        self.assertEqual(games.scheduled_start(jan), at(2027, 1, 15, 15))
        self.assertTrue(games.wanted_game(nba(occurrence_datetime=None), at(2026, 10, 21, 12), PRE, LINGER))
        self.assertFalse(games.wanted_game(nba(occurrence_datetime=None), at(2026, 10, 21, 11, 59), PRE, LINGER))
        # an unreadable occurrence time falls back too; no date at all is unbounded (recorded)
        self.assertEqual(games.scheduled_start(nba(occurrence_datetime='soon')), at(2026, 10, 21, 14))
        odd = nba(occurrence_datetime=None, ticker='KXNBAGAME-X', event_ticker='KXNBAGAME')
        self.assertIsNone(games.scheduled_start(odd))
        self.assertTrue(games.wanted_game(odd, TIP - 3 * 86400, PRE, LINGER))

    def test_after_the_game(self):
        # the game ended: the market closed early (lifecycle or listing), linger_s more, then off
        done = nba(status='closed', close_time=iso(TIP + 9000))
        self.assertTrue(games.wanted_game(done, TIP + 9000 + LINGER, PRE, LINGER))
        self.assertFalse(games.wanted_game(done, TIP + 9000 + LINGER + 1, PRE, LINGER))
        self.assertFalse(games.wanted_game(nba(status='settled'), TIP + 3600, PRE, LINGER))
        self.assertFalse(games.wanted_game(nba(status='finalized'), TIP + 3600, PRE, LINGER))
        self.assertTrue(games.wanted_game(nba(status='determined'), TIP + 3600, PRE, LINGER))
        self.assertTrue(games.wanted_game(nba(close_time=None), TIP + 30 * 86400, PRE, LINGER))

    def test_recordable_during(self):
        done = nba(status='closed', close_time=iso(TIP + 9000))
        now = TIP + 86400
        self.assertTrue(games.recordable_during(done, TIP + 3600, TIP + 7200, now, PRE, LINGER))     # in the game
        self.assertTrue(games.recordable_during(done, TIP - 3 * 3600, TIP - PRE, now, PRE, LINGER))  # its first second
        self.assertFalse(games.recordable_during(done, TIP - 3 * 3600, TIP - PRE - 1, now, PRE, LINGER))
        self.assertFalse(games.recordable_during(done, TIP + 9000 + LINGER + 1, now, now, PRE, LINGER))
        self.assertTrue(games.recordable_during(nba(close_time=None), TIP, TIP + 60, now, PRE, LINGER))
        self.assertFalse(games.recordable_during(nba(close_time=None, status='settled'), TIP, TIP + 60, now, PRE,
                                                 LINGER))


if __name__ == '__main__':
    unittest.main()
