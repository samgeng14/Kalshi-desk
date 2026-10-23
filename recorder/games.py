"""The game group (REC_GAME_SERIES, the NBA's KXNBAGAME by default): when to record a game's books.

A game market opens days before its game. Recording its socket book from then would be days of
quiet pregame frames for the few hours the NBA lanes study (the pregame hour, p0, the game), so a
game is recorded only inside a window around it: from pre_s before its scheduled start to
linger_s after its market closes (a game that ends closes early, by the lifecycle channel or the
next listing).

The scheduled start is occurrence_datetime - 3 h: the sports hunt (docs/edge_results.json) found
that equal to ESPN's start for 78-99% of games. TO CONFIRM on the first night's kalshi_markets
rows. A market without it starts at 10:00 ET on its ticker's game date: no NBA game tips earlier,
so the window opens early, never late.

Pure functions; rec.py does the listing and the subscriptions.
"""
from datetime import datetime, timedelta, timezone

from .college import event_date

OCCURRENCE_AFTER_S = 3 * 3600    # occurrence_datetime is this long after the scheduled start
FALLBACK_ET = 10                 # without it, the start is this hour, Eastern, on the game date
DONE = ('settled', 'finalized')

try:
    from zoneinfo import ZoneInfo
    ET = ZoneInfo('America/New_York')
except Exception:   # no tz database: EDT, the earlier of the two (the window opens early, not late)
    ET = timezone(timedelta(hours=-4))


def ts_s(rfc):
    """RFC 3339 to epoch seconds; None when absent or unreadable."""
    if not isinstance(rfc, str) or not rfc:
        return None
    try:
        return datetime.fromisoformat(rfc.replace('Z', '+00:00')).timestamp()
    except ValueError:
        return None


def scheduled_start(m):
    """The game's scheduled start (epoch s): occurrence_datetime - 3 h, else 10:00 ET on the date
    in its event ticker, else None."""
    occ = ts_s(m.get('occurrence_datetime'))
    if occ is not None:
        return occ - OCCURRENCE_AFTER_S
    day = event_date(m.get('event_ticker') or m.get('ticker'))
    if day is None:
        return None
    y, mo, d = (int(x) for x in day.split('-'))
    return datetime(y, mo, d, FALLBACK_ET, tzinfo=ET).timestamp()


def game_window(m, pre_s, linger_s):
    """(start_s, end_s): pre_s before the scheduled start to linger_s after the market closes.
    Either is None when unknown (no start or no close time): unbounded on that side."""
    start, close = scheduled_start(m), ts_s(m.get('close_time'))
    return (None if start is None else start - pre_s, None if close is None else close + linger_s)


def wanted_game(m, now_s, pre_s, linger_s):
    """Record this game market's book now: not settled, and inside its window."""
    if m.get('status') in DONE:
        return False
    a, b = game_window(m, pre_s, linger_s)
    return (a is None or a <= now_s) and (b is None or now_s <= b)


def recordable_during(m, a_s, b_s, now_s, pre_s, linger_s):
    """Was this game market inside its window at some time in [a_s, b_s] (a trade backfill's
    question, as rec.recordable_during for the other groups)? A missing close is now, unless the
    market is settled (when it stopped is then unknown)."""
    start, end = game_window(m, pre_s, linger_s)
    if end is None:
        if m.get('status') in DONE:
            return False
        end = now_s + linger_s
    return end >= a_s and (start is None or start <= b_s)
