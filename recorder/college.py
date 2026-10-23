"""College and thin-sport games (plan section 5(d)): which games of which series to record.

Every game of the college series is recorded (women's basketball, volleyball, college hockey,
table tennis, AHL, Liiga, SHL, CS2: REC_COLLEGE_SERIES). The men's college basketball series is
the control, and only its thinnest tenth by pregame volume is recorded (REC_CONTROL_SERIES):
recording every men's game would be ten times the rows for a comparison that needs the thin ones.

Pure functions and one small class; rec.py does the REST calls and the subscriptions.
"""
import math
from datetime import datetime, timezone

from .kalshi import count

MONTHS = {m: i + 1 for i, m in enumerate(('JAN', 'FEB', 'MAR', 'APR', 'MAY', 'JUN', 'JUL', 'AUG', 'SEP',
                                          'OCT', 'NOV', 'DEC'))}


def event_date(event_ticker):
    """The game date Kalshi writes in a game's event ticker, as YYYY-MM-DD:
    KXNCAAMBGAME-26NOV05DUKEUNC -> 2026-11-05 (deskd's ref/espn.cpp kalshi_event_date reads it the
    same way; a time may follow the date, as in KXTTSTARMATCH-26OCT070230FRALOS). It is the US
    Eastern date of the game. None when the ticker has no such date."""
    if not isinstance(event_ticker, str) or '-' not in event_ticker:
        return None
    d = event_ticker.split('-', 1)[1][:7]
    if len(d) < 7 or not (d[:2].isdigit() and d[5:7].isdigit()) or d[2:5] not in MONTHS:
        return None
    try:
        return datetime(2000 + int(d[:2]), MONTHS[d[2:5]], int(d[5:7])).strftime('%Y-%m-%d')
    except ValueError:
        return None


def event_volumes(markets):
    """Each event's volume, summed over its markets (a game is one event of two or three markets)."""
    out = {}
    for m in markets:
        ev = m.get('event_ticker')
        if not isinstance(ev, str) or not ev:
            continue
        out[ev] = out.get(ev, 0.0) + (count(m, 'volume') or 0.0)
    return out


def rank_thinnest(volumes, frac):
    """Events ranked thinnest first (ties by ticker, so a re-run ranks the same) and how many of
    them make the picked fraction: ceil(n * frac), at least one when there is any event."""
    ranked = sorted(volumes.items(), key=lambda kv: (kv[1], kv[0]))
    n = min(len(ranked), max(1, math.ceil(len(ranked) * frac))) if ranked else 0
    return ranked, n


class ControlPicks:
    """The control decile, picked once per game date and kept.

    The pick is made at the first discovery round at or after pick_h UTC on the game's own date
    (15 UTC is 10-11 ET: before the first tip-offs, so the volumes ranked are pregame; and from 5
    UTC on, the UTC date is the Eastern date the tickers carry). Games listed after the pick are
    not added, and the picks are saved in the recorder's state file, so a restart in the evening
    keeps the morning's picks instead of ranking again on in-game volume. A recorder first started
    after pick_h picks then; college_control's recv_us says when.
    """

    KEEP_DAYS = 7

    def __init__(self, frac, pick_h, state=None):
        self.frac = frac
        self.pick_h = pick_h
        self.state = state
        saved = state.get('control', {}) if state is not None else {}
        self.picks = {d: set(v) for d, v in saved.items() if isinstance(v, list)}

    def picked(self, event_ticker):
        d = event_date(event_ticker)
        return d is not None and event_ticker in self.picks.get(d, ())

    def due(self, now_s):
        """The game date to pick now, or None (already picked today, or before pick_h)."""
        t = datetime.fromtimestamp(now_s, tz=timezone.utc)
        day = t.strftime('%Y-%m-%d')
        return day if t.hour >= self.pick_h and day not in self.picks else None

    def update(self, series_markets, now_s, recv_us):
        """Pick the due date's thinnest events from {series: [market rows]}; the college_control
        rows to record (every ranked event, picked or not), [] when nothing was due."""
        day = self.due(now_s)
        if day is None:
            return []
        rows, chosen = [], set()
        for series in sorted(series_markets):
            todays = [m for m in series_markets[series] if event_date(m.get('event_ticker')) == day]
            ranked, n = rank_thinnest(event_volumes(todays), self.frac)
            for i, (ev, vol) in enumerate(ranked):
                rows.append({'recv_us': recv_us, 'series': series, 'game_date': day, 'event_ticker': ev,
                             'volume': vol, 'rank': i + 1, 'events': len(ranked), 'picked': i < n})
                if i < n:
                    chosen.add(ev)
        self.picks[day] = chosen
        for d in sorted(self.picks)[:-self.KEEP_DAYS]:
            del self.picks[d]
        if self.state is not None:
            self.state.set('control', {d: sorted(v) for d, v in self.picks.items()})
        return rows
