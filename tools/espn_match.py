#!/usr/bin/env python3
"""Which ESPN game is this Kalshi game? The one Python port of deskd's matching (src/deskd/ref/espn.cpp).

The offline tools (tools/college_gate.py, tools/nba_panel.py) map recorded ESPN and NCAA games to
Kalshi events with exactly the rules deskd's EspnFeed uses live, so a case measured offline is a
game deskd would have matched, on the same side. Ported function by function, same names, same
arithmetic, same order of operations; tests/deskd/test_espn.cpp's parsing and matching cases are
this file's self-test. A change to the C++ is a change here.

  kalshi_event_date(event_ticker)           "KXNBAGAME-26OCT10LALBOS" -> "2026-10-10"; "" when the
                                            ticker carries no YYMONDD after its first '-'
  kalshi_event_start_minutes(event_ticker)  ET minutes after midnight from an HHMM right after the
                                            date ("KXMLBGAME-26OCT101905NYYBOS" -> 1145); -1 when none
  team_tokens(name)                         lower-case ASCII words; "a's" as a word is "athletics";
                                            la, ny, sf, kc, tb, gb, lv become their two city words
  team_name_overlap(a, b)                   the share of the shorter name's tokens found in the
                                            other: whole words (2+ characters) first, then a
                                            one-letter initial takes an unused word it starts
                                            ("Chicago C" is the Cubs, never the White Sox)
  espn_league_for(ticker)                   the ESPN league row of a game market's series
  parse_scoreboard(body, league, now_ms)    an ESPN scoreboard's games (pre, in and post only)
  match_game(k, games)                      EspnMatch(id, a_home), or None
  clock_seconds(clock)                      espn_ref's clock read: "7:42" -> 462, "0:35.4" -> 35.4,
                                            "12" -> 12; -1 when blank or unreadable
  et_date(ms), et_midnight_ms(ms), iso_ms(text)   web/contract.cpp's ET day and ESPN's UTC times

and two helpers deskd has inline: kalshi_games(markets), EspnFeed::step's grouping of market rows
(ticker, event_ticker, yes_sub_title) into a KalshiGame per event, each market paying on the team
named by its ticker's last code; and game_from_score_row(row), a recorder espn_scores or
ncaa_scores row (recorder/scores.py) as an EspnGame (NCAA's char6 and short names as the
abbreviation and the name, as deskd's NCAA feed reads them).

match_game's rule. A candidate needs a start time whose ET date is within a day of the ticker's
date. When the ticker has a start time, only that date's games within 90 minutes of it count,
closeness 1 - |minutes off| / 90 (a doubleheader's two games differ only in the time). Each
orientation is scored: s1 with market a the away team, s2 with a at home, 2 for each ticker code
equal to ESPN's abbreviation (any case) plus each team-name overlap. a_home = s2 > s1. In that
orientation each team needs evidence of its own (2 for its code, plus its name overlap) of 0.75
or more; |s1 - s2| must be 1 or more (a side that barely beats its mirror image is a guess); the
score, max(s1, s2) + 0.5 on the ticker's own date + closeness, must be 2 or more. The best game
wins only if it beats the second best by more than 0.25 (two games we cannot tell apart: None).

Character handling is the C++'s, in the C locale: only ASCII letters are lowered and only ASCII
letters and digits make words, so an accented letter splits a word as punctuation does ("San
José" is "san jos"), byte for byte what deskd does with the UTF-8 name.

Standard library only. Import it (sys.path.insert(0, <tools dir>); import espn_match).

Usage:
  python3 tools/espn_match.py --self-test
"""
import argparse
import calendar
import datetime
import json
import re
import sys
from collections import namedtuple
from dataclasses import dataclass

# ---------------------------------------------------------------- ET days and ESPN's times

DAY_MS = 24 * 3600000
_EPOCH = datetime.date(1970, 1, 1)


def _ymd(days):
    return (_EPOCH + datetime.timedelta(days=days)).isoformat()


def _nth_sunday(y, m, n):
    """The n-th Sunday (1-based) of month m of year y, as days since the epoch."""
    first = datetime.date(y, m, 1)
    return (first - _EPOCH).days + (6 - first.weekday()) % 7 + 7 * (n - 1)


def et_date(ms):
    """YYYY-MM-DD of the US Eastern day holding wall_ms: EDT from 02:00 EST (07:00 UTC) on the
    second Sunday of March to 02:00 EDT (06:00 UTC) on the first Sunday of November."""
    utc_s = ms // 1000
    y = (_EPOCH + datetime.timedelta(days=utc_s // 86400)).year
    dst_on = _nth_sunday(y, 3, 2) * 86400 + 7 * 3600
    dst_off = _nth_sunday(y, 11, 1) * 86400 + 6 * 3600
    offset = -4 * 3600 if dst_on <= utc_s < dst_off else -5 * 3600
    return _ymd((utc_s + offset) // 86400)


def et_midnight_ms(ms):
    """The start of ms's ET day, epoch ms. Local midnight is never inside a DST change (those
    happen at 02:00), so the offset in force at midnight maps the candidate back to the same date."""
    utc_s = ms // 1000
    day = et_date(ms)
    for offset in (-4 * 3600, -5 * 3600):
        midnight = (utc_s + offset) // 86400 * 86400 - offset
        if midnight <= utc_s and et_date(midnight * 1000) == day and et_date((midnight - 1) * 1000) != day:
            return midnight * 1000
    return utc_s // 86400 * 86400 * 1000   # unreachable for valid times


_ISO = re.compile(r'\s*([+-]?\d{1,4})-\s*([+-]?\d{1,2})-\s*([+-]?\d{1,2})T\s*([+-]?\d{1,2}):\s*([+-]?\d{1,2})')
_FLOAT = re.compile(r'\s*[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?')


def iso_ms(s):
    """ESPN's times, "2026-10-10T23:30Z" with or without seconds and fractions, as epoch ms; 0
    when unreadable or not UTC (no 'Z')."""
    m = _ISO.match(s or '')
    if not m:
        return 0
    y, mo, d, h, mi = (int(x) for x in m.groups())
    sec = 0.0
    if len(s) > 16 and s[16] == ':':
        f = _FLOAT.match(s, 17)
        sec = float(f.group(0)) if f else 0.0
    if 'Z' not in s:
        return 0
    try:
        t = calendar.timegm((y, mo, d, h, mi, 0, 0, 0, 0))
    except (ValueError, OverflowError):
        return 0
    if t <= 0:
        return 0
    return t * 1000 + int(sec * 1000)


def espn_et_date(ms):
    noon = (et_midnight_ms(ms) + 12 * 3600000) // 1000
    return datetime.datetime.fromtimestamp(noon, tz=datetime.timezone.utc).date().isoformat()


def day_ms(ymd):
    return iso_ms(ymd + 'T12:00Z')


# ---------------------------------------------------------------- Kalshi tickers

_MONTHS = ('JAN', 'FEB', 'MAR', 'APR', 'MAY', 'JUN', 'JUL', 'AUG', 'SEP', 'OCT', 'NOV', 'DEC')
_DIGITS = '0123456789'


def kalshi_event_date(ev):
    p = ev.find('-')
    if p < 0 or len(ev) < p + 8:
        return ''
    d = ev[p + 1:p + 8]   # 26OCT10
    if not all(c in _DIGITS for c in (d[0], d[1], d[5], d[6])):
        return ''
    month = 0
    for i, name in enumerate(_MONTHS):
        if d[2:5] == name:
            month = i + 1
    if not month:
        return ''
    return '20%s-%02d-%s' % (d[0:2], month, d[5:7])


def kalshi_event_start_minutes(ev):
    p = ev.find('-')
    if p < 0 or len(ev) < p + 1 + 11:
        return -1
    hhmm = ev[p + 8:p + 12]
    if not all(c in _DIGITS for c in hhmm):
        return -1
    hh, mm = int(hhmm[:2]), int(hhmm[2:])
    return hh * 60 + mm if hh < 24 and mm < 60 else -1


# ---------------------------------------------------------------- team names

_CITY = {'la': ['los', 'angeles'], 'ny': ['new', 'york'], 'sf': ['san', 'francisco'], 'kc': ['kansas', 'city'],
         'tb': ['tampa', 'bay'], 'gb': ['green', 'bay'], 'lv': ['las', 'vegas']}


def _lower(s):
    """std::tolower in the C locale: ASCII letters only."""
    return ''.join(chr(ord(c) + 32) if 'A' <= c <= 'Z' else c for c in s)


def _alnum(c):
    return 'a' <= c <= 'z' or 'A' <= c <= 'Z' or '0' <= c <= '9'


def team_tokens(name):
    s = _lower(name)
    p = s.find("a's")
    while p >= 0:
        if (p == 0 or not _alnum(s[p - 1])) and (p + 3 >= len(s) or not _alnum(s[p + 3])):
            s = s[:p] + 'athletics' + s[p + 3:]
        p = s.find("a's", p + 9)
    out, cur = [], ''
    for c in s + ' ':
        if _alnum(c):
            cur += c
        elif cur:
            out.extend(_CITY.get(cur, [cur]))
            cur = ''
    return out


def team_name_overlap(a, b):
    ta, tb = team_tokens(a), team_tokens(b)
    if not ta or not tb:
        return 0.0
    small, big = (ta, tb) if len(ta) <= len(tb) else (tb, ta)
    used, done = [False] * len(big), [False] * len(small)
    hit = 0
    for i, w in enumerate(small):   # whole words first
        if len(w) < 2:
            continue
        for j, v in enumerate(big):
            if not used[j] and v == w:
                used[j] = done[i] = True
                hit += 1
                break
    for i, w in enumerate(small):   # then an initial takes a word it starts
        if done[i] or len(w) != 1:
            continue
        for j, v in enumerate(big):
            if not used[j] and v[0] == w[0]:
                used[j] = True
                hit += 1
                break
    return hit / len(small)


def _code_match(code, abbr):
    return 2.0 if code and _lower(code) == _lower(abbr) else 0.0


def team_evidence(code, name, abbr, full):
    return _code_match(code, abbr) + team_name_overlap(name, full)


# ---------------------------------------------------------------- ESPN's leagues and games

EspnLeague = namedtuple('EspnLeague', 'series sport league groups periods period_label')
LEAGUES = (   # ref/espn.cpp espn_leagues(), in its order
    EspnLeague('KXNFLGAME', 'football', 'nfl', (), 4, 'Q'),
    EspnLeague('KXNCAAFGAME', 'football', 'college-football', ('80', '81'), 4, 'Q'),
    EspnLeague('KXNBAGAME', 'basketball', 'nba', (), 4, 'Q'),
    EspnLeague('KXWNBAGAME', 'basketball', 'wnba', (), 4, 'Q'),
    EspnLeague('KXNCAAMBGAME', 'basketball', 'mens-college-basketball', ('50',), 2, 'H'),
    EspnLeague('KXNCAAWBGAME', 'basketball', 'womens-college-basketball', ('50',), 4, 'Q'),
    EspnLeague('KXMLBGAME', 'baseball', 'mlb', (), 9, ''),
    EspnLeague('KXNHLGAME', 'hockey', 'nhl', (), 3, 'P'),
)


def espn_league_for(ticker):
    for lg in LEAGUES:
        n = len(lg.series)
        if len(ticker) > n and ticker.startswith(lg.series) and ticker[n] == '-':
            return lg
    return None


@dataclass
class EspnGame:
    id: str = ''
    league: str = ''
    state: str = ''          # pre, in, post
    detail: str = ''
    clock: str = ''
    last_play_id: str = ''
    start_ms: int = 0
    fetched_ms: int = 0
    period: int = 0
    home_score: int = 0
    away_score: int = 0
    home_abbr: str = ''
    home_name: str = ''
    away_abbr: str = ''
    away_name: str = ''


def _jget(j, key):
    return j.get(key) if isinstance(j, dict) else None


def _jstr(j, key):
    v = _jget(j, key)
    if isinstance(v, str):
        return v
    if isinstance(v, int) and not isinstance(v, bool):
        return str(v)
    return ''


_STRTOL = re.compile(r'\s*[+-]?\d+\Z')


def _jint(j, key, default):
    v = _jget(j, key)
    if isinstance(v, (int, float)) and not isinstance(v, bool):
        return int(float(v))
    if isinstance(v, str) and _STRTOL.match(v):
        return int(v)
    return default


def parse_scoreboard(body, league, now_ms):
    out = []
    events = _jget(body, 'events')
    if not isinstance(events, list):
        return out
    for e in events:
        comps = _jget(e, 'competitions')
        if not isinstance(comps, list) or not comps or not isinstance(comps[0], dict):
            continue
        c = comps[0]
        g = EspnGame(id=_jstr(e, 'id'))
        if not g.id:
            continue
        g.league = league
        g.start_ms = iso_ms(_jstr(e, 'date'))
        status = _jget(c, 'status')
        typ = _jget(status, 'type')
        g.state = _jstr(typ, 'state')
        g.detail = _jstr(typ, 'shortDetail')
        g.period = _jint(status, 'period', 0)
        g.clock = _jstr(status, 'displayClock')
        g.last_play_id = _jstr(_jget(_jget(c, 'situation'), 'lastPlay'), 'id')
        teams = _jget(c, 'competitors')
        if not isinstance(teams, list):
            continue
        for t in teams:
            home = _jstr(t, 'homeAway') == 'home'
            team = _jget(t, 'team')
            if home:
                g.home_abbr, g.home_name, g.home_score = _jstr(team, 'abbreviation'), _jstr(team, 'displayName'), _jint(t, 'score', 0)
            else:
                g.away_abbr, g.away_name, g.away_score = _jstr(team, 'abbreviation'), _jstr(team, 'displayName'), _jint(t, 'score', 0)
        if not g.home_abbr or not g.away_abbr or g.state not in ('pre', 'in', 'post'):
            continue
        g.fetched_ms = now_ms
        out.append(g)
    return out


def _stoi_prefix(s):
    m = re.match(r'\s*[+-]?\d+', s)
    if not m:
        raise ValueError(s)
    return int(m.group(0))


def _stod_prefix(s):
    m = re.match(r'\s*[+-]?((\d+\.?\d*|\.\d+)([eE][+-]?\d+)?|inf(inity)?|nan)', s, re.I)
    if not m:
        raise ValueError(s)
    return float(m.group(0))


def clock_seconds(clock):
    """espn_ref's clock_s: "7:42", "0:35.4" or "12" (seconds), the period's time left; -1 unread."""
    colon = clock.find(':')
    try:
        if colon >= 0:
            return _stoi_prefix(clock[:colon]) * 60.0 + _stod_prefix(clock[colon + 1:])
        if clock:
            return _stod_prefix(clock)
    except ValueError:
        return -1.0
    return -1.0


def game_from_score_row(row):
    """A recorder espn_scores / ncaa_scores row (recorder/scores.py espn_games / ncaa_games) as an
    EspnGame. ESPN rows: the raw competitors' abbreviation and displayName; NCAA rows: the raw
    teams' names.char6 and names.short. Either falls back to the row's home / away text."""
    try:
        raw = json.loads(row.get('raw') or '{}')
    except ValueError:
        raw = {}
    raw = raw if isinstance(raw, dict) else {}
    names = {}
    for t in raw.get('competitors') or []:   # ESPN
        if isinstance(t, dict) and t.get('homeAway') in ('home', 'away'):
            team = t.get('team') if isinstance(t.get('team'), dict) else {}
            names[t['homeAway']] = (_jstr(team, 'abbreviation'), _jstr(team, 'displayName'))
    for side in ('home', 'away'):            # NCAA
        t = raw.get(side)
        if side not in names and isinstance(t, dict) and isinstance(t.get('names'), dict):
            n = t['names']
            names[side] = (_jstr(n, 'char6'), _jstr(n, 'short'))
    g = EspnGame(id=str(row.get('game_id') or ''), league=row.get('league') or '', state=row.get('state') or '',
                 detail=row.get('detail') or '', clock=row.get('clock') or '',
                 start_ms=iso_ms(row.get('start_time') or ''), period=_jint(row, 'period', 0),
                 home_score=_jint(row, 'home_score', 0), away_score=_jint(row, 'away_score', 0))
    for side in ('home', 'away'):
        abbr, name = names.get(side, ('', ''))
        text = row.get(side) or ''
        setattr(g, side + '_abbr', abbr or text)
        setattr(g, side + '_name', name or text)
    return g


# ---------------------------------------------------------------- the match

@dataclass
class KalshiGame:
    event_ticker: str = ''
    series: str = ''
    date: str = ''        # YYYY-MM-DD (ET), from the event ticker
    start_min: int = -1   # ET minutes after midnight when the ticker says
    ticker_a: str = ''
    code_a: str = ''
    name_a: str = ''
    ticker_b: str = ''
    code_b: str = ''
    name_b: str = ''


EspnMatch = namedtuple('EspnMatch', 'id a_home')


def kalshi_games(markets):
    """EspnFeed::step's grouping: market rows (dicts with ticker, event_ticker, yes_sub_title) of
    ESPN's leagues, by event; the first market seen is a, the second b, any further one ignored."""
    events = {}
    for m in markets:
        tk, ev = m.get('ticker') or '', m.get('event_ticker') or ''
        lg = espn_league_for(tk)
        if not lg or not ev:
            continue
        k = events.get(ev)
        if k is None:
            k = events[ev] = KalshiGame(event_ticker=ev, series=lg.series, date=kalshi_event_date(ev),
                                        start_min=kalshi_event_start_minutes(ev))
        code = tk[tk.rfind('-') + 1:]
        if not k.ticker_a or k.ticker_a == tk:
            k.ticker_a, k.code_a, k.name_a = tk, code, m.get('yes_sub_title') or ''
        elif not k.ticker_b or k.ticker_b == tk:
            k.ticker_b, k.code_b, k.name_b = tk, code, m.get('yes_sub_title') or ''
    return events


def match_game(k, games):
    """The ESPN game this Kalshi game is, and whether its market a is the home team; None when no
    game has evidence for both teams, or two games are equally good (see the module docstring)."""
    if not k.date:
        return None
    t_info = day_ms(k.date)
    best = second = 0.0
    found = None
    for g in games:
        if g.start_ms <= 0:
            continue
        gd = espn_et_date(g.start_ms)
        if abs(day_ms(gd) - t_info) > DAY_MS:
            continue
        closeness = 0.0
        if k.start_min >= 0:   # the two games of a doubleheader differ only in the time
            if gd != k.date:
                continue
            m = (g.start_ms - et_midnight_ms(g.start_ms)) // 60000
            if abs(m - k.start_min) > 90:
                continue
            closeness = 1.0 - abs(m - k.start_min) / 90.0
        s1 = (_code_match(k.code_a, g.away_abbr) + _code_match(k.code_b, g.home_abbr)
              + team_name_overlap(k.name_a, g.away_name) + team_name_overlap(k.name_b, g.home_name))
        s2 = (_code_match(k.code_a, g.home_abbr) + _code_match(k.code_b, g.away_abbr)
              + team_name_overlap(k.name_a, g.home_name) + team_name_overlap(k.name_b, g.away_name))
        a_home = s2 > s1
        # both teams, in the winning orientation, need their own evidence
        ea = team_evidence(k.code_a, k.name_a, g.home_abbr if a_home else g.away_abbr,
                           g.home_name if a_home else g.away_name)
        eb = team_evidence(k.code_b, k.name_b, g.away_abbr if a_home else g.home_abbr,
                           g.away_name if a_home else g.home_name)
        if min(ea, eb) < 0.75:
            continue
        # a side that barely beats its mirror image is a guess; a wrong side is worse than none
        if abs(s1 - s2) < 1.0:
            continue
        score = max(s1, s2) + (0.5 if gd == k.date else 0.0) + closeness
        if score < 2.0:
            continue
        if score > best:
            second, best, found = best, score, EspnMatch(g.id, a_home)
        elif score > second:
            second = score
    if found is None or not found.id or second >= best - 0.25:   # two games we cannot tell apart
        return None
    return found


# ---------------------------------------------------------------- self-test (test_espn.cpp's cases)

def _team(ha, abbr, name, score):
    return {'homeAway': ha, 'score': str(score), 'team': {'abbreviation': abbr, 'displayName': name}}


def _event(gid, date, state, period, clock, away, home, last_play=''):
    c = {'status': {'period': period, 'displayClock': clock, 'type': {'state': state, 'shortDetail': 'detail'}},
         'competitors': [away, home]}
    if last_play:
        c['situation'] = {'lastPlay': {'id': last_play}}
    return {'id': gid, 'date': date, 'competitions': [c]}


def self_test():
    fails = []

    def check(name, ok, detail=''):
        print('%s %s%s' % ('OK  ' if ok else 'FAIL', name, (': ' + str(detail)) if detail else ''))
        if not ok:
            fails.append(name)

    # ---- parsing() in test_espn.cpp
    check('kalshi_event_date', kalshi_event_date('KXNBAGAME-26OCT10LALBOS') == '2026-10-10'
          and kalshi_event_date('KXNBAGAME-26XYZ10LALBOS') == '' and kalshi_event_date('KXNBAGAME') == '')
    check('kalshi_event_start_minutes', kalshi_event_start_minutes('KXMLBGAME-26OCT101905NYYBOS') == 19 * 60 + 5
          and kalshi_event_start_minutes('KXNBAGAME-26OCT10LALBOS') == -1)
    check('"Chicago C" is the Cubs, never the White Sox',
          team_name_overlap('Chicago C', 'Chicago Cubs') == 1.0 and team_name_overlap('Chicago C', 'Chicago White Sox') < 1.0)
    check('LA and A\'s', team_name_overlap('LA Rams', 'Los Angeles Rams') == 1.0
          and team_name_overlap("A's", 'Athletics') == 1.0)
    lg = espn_league_for('KXNCAAWBGAME-26NOV05UCONNSC-UCONN')
    check('espn_league_for', lg is not None and lg.league == 'womens-college-basketball'
          and espn_league_for('KXNBAGAMEX-1') is None and espn_league_for('KXHIGHNY-26OCT10-B70') is None)
    now = 1791676700000
    sb = {'events': [_event('401', '2026-10-10T23:30Z', 'in', 3, '4:12', _team('away', 'LAL', 'Los Angeles Lakers', 70),
                            _team('home', 'BOS', 'Boston Celtics', 74), 'p9'),
                     _event('402', '2026-10-11T00:00Z', 'pre', 0, '0:00', _team('away', 'NY', 'New York Knicks', 0),
                            _team('home', 'MIA', 'Miami Heat', 0)),
                     {'id': '403'}]}   # no competitions: skipped
    games = parse_scoreboard(sb, 'nba', now)
    check('parse_scoreboard: two games', len(games) == 2, len(games))
    if len(games) == 2:
        g = games[0]
        check('parse_scoreboard: the fields', g.id == '401' and g.state == 'in' and g.period == 3 and g.clock == '4:12'
              and g.home_abbr == 'BOS' and g.away_score == 70 and g.home_score == 74 and g.last_play_id == 'p9'
              and g.fetched_ms == now, g)
        check('parse_scoreboard: start_ms', g.start_ms == 1791675000000, g.start_ms)
        check('the pre game at 00:00Z is 8 pm Eastern the day before',
              espn_et_date(games[1].start_ms) == '2026-10-10'
              and (games[1].start_ms - et_midnight_ms(games[1].start_ms)) // 60000 == 20 * 60)
    check('clock_seconds: espn_ref\'s clock_s', clock_seconds('4:12') == 252 and clock_seconds('0:35.4') == 35.4
          and clock_seconds('12') == 12 and clock_seconds('') == -1 and clock_seconds('x:y') == -1)

    # ---- matching() in test_espn.cpp
    sb = {'events': [_event('401', '2026-10-10T23:30Z', 'in', 3, '4:12', _team('away', 'LAL', 'Los Angeles Lakers', 70),
                            _team('home', 'BOS', 'Boston Celtics', 74)),
                     _event('501', '2026-10-10T17:05Z', 'in', 3, '', _team('away', 'CHC', 'Chicago Cubs', 1),
                            _team('home', 'CHW', 'Chicago White Sox', 2)),
                     _event('502', '2026-10-10T23:10Z', 'pre', 0, '', _team('away', 'CHC', 'Chicago Cubs', 0),
                            _team('home', 'CHW', 'Chicago White Sox', 0))]}
    games = parse_scoreboard(sb, 'x', now)
    k = KalshiGame('KXNBAGAME-26OCT10LALBOS', 'KXNBAGAME', '2026-10-10', -1,
                   'KXNBAGAME-26OCT10LALBOS-BOS', 'BOS', 'Boston', 'KXNBAGAME-26OCT10LALBOS-LAL', 'LAL', 'Los Angeles L')
    m = match_game(k, games)
    check('market a (BOS) is the home team, whatever the ticker order', m == EspnMatch('401', True), m)
    k.date = '2026-10-13'
    check('three days off: not this game', match_game(k, games) is None)
    dh = KalshiGame('KXMLBGAME-26OCT101905CHCCWS', 'KXMLBGAME', '2026-10-10', 19 * 60 + 5,
                    'KXMLBGAME-26OCT101905CHCCWS-CHC', 'CHC', 'Chicago C', 'KXMLBGAME-26OCT101905CHCCWS-CHW', 'CHW', 'Chicago WS')
    m = match_game(dh, games)
    check('a doubleheader: the start time in the ticker decides (evening)', m == EspnMatch('502', False), m)
    dh.start_min = 13 * 60 + 5
    m = match_game(dh, games)
    check('a doubleheader: the start time in the ticker decides (afternoon)', m is not None and m.id == '501', m)
    dh.start_min = -1
    check('no time and the same two teams twice that day: two games we cannot tell apart', match_game(dh, games) is None)
    one = KalshiGame('KXNBAGAME-26OCT10LALXYZ', 'KXNBAGAME', '2026-10-10', -1,
                     'KXNBAGAME-26OCT10LALXYZ-XYZ', 'XYZ', 'Nowhere', 'KXNBAGAME-26OCT10LALXYZ-LAL', 'LAL', 'Los Angeles L')
    check('only one team has evidence: no match', match_game(one, games) is None)

    # ---- the pieces this port adds around the C++'s
    check('et_date: EDT and EST, and the DST changes (2026: Mar 8, Nov 1)',
          et_date(iso_ms('2026-10-11T03:59Z')) == '2026-10-10' and et_date(iso_ms('2026-10-11T04:00Z')) == '2026-10-11'
          and et_date(iso_ms('2026-12-01T04:59Z')) == '2026-11-30' and et_date(iso_ms('2026-12-01T05:00Z')) == '2026-12-01'
          and et_midnight_ms(iso_ms('2026-11-01T12:00Z')) == iso_ms('2026-11-01T04:00Z')
          and et_midnight_ms(iso_ms('2026-11-02T12:00Z')) == iso_ms('2026-11-02T05:00Z')
          and et_midnight_ms(iso_ms('2026-03-08T12:00Z')) == iso_ms('2026-03-08T05:00Z')
          and et_midnight_ms(iso_ms('2026-03-09T03:00Z')) == iso_ms('2026-03-08T05:00Z'))
    check('iso_ms: seconds and fractions, UTC only', iso_ms('2026-10-10T23:58:01Z') == 1791676681000
          and iso_ms('2026-10-10T23:58:01.250Z') == 1791676681250 and iso_ms('2026-10-10T23:58') == 0
          and iso_ms('junk') == 0)
    check('team_tokens: city codes, a\'s as a word only, ASCII words', team_tokens('NY Mets') == ['new', 'york', 'mets']
          and team_tokens("Texas A&M's") == ['texas', 'a', 'm', 's'] and team_tokens('San José St.') == ['san', 'jos', 'st']
          and team_tokens('LA') == ['los', 'angeles'])
    rows = [{'ticker': 'KXNCAAWBGAME-26NOV05UCONNSC-UCONN', 'event_ticker': 'KXNCAAWBGAME-26NOV05UCONNSC', 'yes_sub_title': 'UConn'},
            {'ticker': 'KXNCAAWBGAME-26NOV05UCONNSC-SC', 'event_ticker': 'KXNCAAWBGAME-26NOV05UCONNSC', 'yes_sub_title': 'South Carolina'},
            {'ticker': 'KXHIGHNY-26NOV05-B70', 'event_ticker': 'KXHIGHNY-26NOV05', 'yes_sub_title': '70'}]
    ks = kalshi_games(rows)
    kg = ks.get('KXNCAAWBGAME-26NOV05UCONNSC')
    check('kalshi_games: one event, a and b by ticker code, other series left out', list(ks) == ['KXNCAAWBGAME-26NOV05UCONNSC']
          and kg.code_a == 'UCONN' and kg.name_b == 'South Carolina' and kg.date == '2026-11-05' and kg.start_min == -1, ks)
    espn_row = {'league': 'womens-college-basketball', 'game_id': '401800001', 'start_time': '2026-11-06T00:00Z',
                'state': 'in', 'period': '2', 'clock': '3:10', 'home': 'SC', 'away': 'CONN', 'home_score': 30,
                'away_score': 28, 'raw': json.dumps({'competitors': [
                    {'homeAway': 'home', 'team': {'abbreviation': 'SC', 'displayName': 'South Carolina Gamecocks'}},
                    {'homeAway': 'away', 'team': {'abbreviation': 'CONN', 'displayName': 'UConn Huskies'}}]})}
    ncaa_row = {'league': 'basketball-women/d1', 'game_id': '6400001', 'start_time': '2026-11-06T00:00Z', 'state': 'in',
                'period': '2', 'clock': '03:10', 'home': 'SC', 'away': 'UCONN',
                'raw': json.dumps({'home': {'names': {'char6': 'SC', 'short': 'South Carolina'}},
                                   'away': {'names': {'char6': 'UCONN', 'short': 'UConn'}}})}
    ge, gn = game_from_score_row(espn_row), game_from_score_row(ncaa_row)
    check('game_from_score_row: ESPN and NCAA rows', ge.home_name == 'South Carolina Gamecocks' and ge.away_abbr == 'CONN'
          and ge.period == 2 and ge.home_score == 30 and gn.away_abbr == 'UCONN' and gn.home_name == 'South Carolina'
          and gn.start_ms == iso_ms('2026-11-06T00:00Z'), (ge, gn))
    m1, m2 = match_game(kg, [ge]), match_game(kg, [gn])
    check('a recorded game matches the event the same way from either source (market a away)',
          m1 == EspnMatch('401800001', False) and m2 == EspnMatch('6400001', False), (m1, m2))
    print('espn_match self-test: %s' % ('ok' if not fails else 'FAILED (%d)' % len(fails)))
    return 1 if fails else 0


def run(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args(argv)
    if not a.self_test:
        ap.error('nothing to do: this module is imported; --self-test runs its checks')
    return self_test()


if __name__ == '__main__':
    sys.exit(run(sys.argv[1:]))
