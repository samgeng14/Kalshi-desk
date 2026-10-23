#!/usr/bin/env python3
"""How far behind the NBA is ESPN? The lag tools/nba_panel.py and nba_gate.py n1 must use (41.5 s until measured).

nba_panel.py makes a history play visible at the running maximum of its game's play wallclocks
plus --lag-s, and nba_gate.py n1 refuses a panel built with less lag than the measured one. The
41.5 s is the old desk's, from several leagues sampled every 30 s (plan 5(e)); this measures the
NBA's on the recorder's data (--data, recorder/store.py's tables), per ET night of the games:

  score lag    each score change in play (espn_scores, at the poll that first showed it: deskd
               reads the score from the scoreboard) less ESPN's wallclock of the play that made
               it (espn_plays: the first play first seen with those scores; a play first seen under
               a sequence number already shown was inserted late and does not count). This is what
               the panel's lag stands for: when deskd could have acted on a play.
  summary lag  each play's first sighting in the summaries (espn_plays) less its wallclock
  Kalshi lead  each score change's first sighting less the first instant the home team's
               effective mid over both books (kalshi_ws, replayed by lip_join; game_fair.h's
               effective_book) stood 3c or more toward the scoring team from where it was 60 s
               before that sighting, looked for within 60 s either side of it (positive: Kalshi
               moved first). A change with no such move, or no mid to start from, is counted.

Games are KXNBAGAME's events matched to ESPN by tools/espn_match.py (tools/nba_panel.py's reader).
Each measure as p50, p90 and p99 in seconds, nearest rank (the smallest value with at least that
share at or under it). The lag to use is the score lag's p90 over every night, rounded up to the
half second: the replay showing a state later than deskd would have seen it only makes the gate
harder, so the gates take the slow end, not the median.

cdn.nba.com is not recorded yet (its URL is TO CONFIRM on the VM): once the recorder has it, its
lag belongs beside ESPN's here.

Standard library only. No network. Read-only.

Usage:
  python3 tools/nba_latency.py --data DIR [--json]
  python3 tools/nba_latency.py --self-test
"""
import argparse
import contextlib
import io
import json
import math
import os
import sys
import tempfile
from collections import Counter, defaultdict
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import espn_match as em   # noqa: E402  (ESPN's times)
import lip_join as lj     # noqa: E402  (the recorder's reader and the book replay)
import nba_panel as nbp   # noqa: E402  (the recorder's NBA games, their books and the effective book)

MOVE = 3 * lj.CENT          # a move of the effective mid, in Px
BEFORE_MS = 60_000          # where the mid stood: this long before the sighting
AROUND_MS = 60_000          # the move looked for this long either side of it
QUANTILES = ((50, 0.5), (90, 0.9), (99, 0.99))
MEASURES = ('score', 'summary', 'lead')


def pct(vals, q):
    """The nearest-rank quantile: the smallest value with at least q of them at or under it."""
    s = sorted(vals)
    return s[max(1, math.ceil(q * len(s))) - 1] if s else None


def gate_lag(score_lags):
    """The lag the gates use: the score lag's p90, rounded up to the half second; None without one."""
    p = pct(score_lags, 0.9)
    return None if p is None else math.ceil(p * 2 - 1e-9) / 2


def first_plays(plays):
    """A game's plays at their first sighting (its espn_plays rows, oldest first), in the order
    seen; a play first seen under a sequence number already shown is dropped (inserted late):
    (rows, how many dropped)."""
    first = {}
    for r in plays:
        if r.get('play_id') and r['play_id'] not in first:
            first[r['play_id']] = r
    out, top, late = [], None, 0
    for r in sorted(first.values(), key=lambda r: (r['recv_us'], r.get('seq') if isinstance(r.get('seq'), int) else 0)):
        seq = r.get('seq')
        if isinstance(seq, int) and not isinstance(seq, bool):
            if top is not None and seq < top:
                late += 1
                continue
            top = seq
        out.append(r)
    return out, late


def score_changes(rows):
    """[(first seen ms, home score, away score, +1 the home team scored or -1 the away team)]: each
    espn_scores row in play whose scores differ from the last such row's (the margin unchanged,
    both scored between polls: no side, left out)."""
    out, last = [], None
    for r in rows:
        if r.get('state') != 'in':
            continue
        hs, as_ = nbp.as_int(r.get('home_score')), nbp.as_int(r.get('away_score'))
        if hs is None or as_ is None:
            continue
        if last is not None and (hs, as_) != last:
            d = (hs - as_) - (last[0] - last[1])
            if d:
                out.append((r['recv_us'] // 1000, hs, as_, 1 if d > 0 else -1))
        last = (hs, as_)
    return out


def kalshi_move(quotes, home, away, seen_ms, side):
    """(the first instant the home team's effective mid stood MOVE toward side from where it was
    BEFORE_MS before seen_ms, within AROUND_MS either side of seen_ms, or None; whether there was a
    mid to start from)."""
    def mid(t):
        bid, ask = nbp.effective(quotes.at(home, t), quotes.at(away, t))
        return None if bid is None or ask is None else (bid + ask) / 2
    m0 = mid(seen_ms - BEFORE_MS)
    if m0 is None:
        return None, False
    lo, hi = seen_ms - AROUND_MS, seen_ms + AROUND_MS
    for t in sorted(set(quotes.times(home, lo, hi)) | set(quotes.times(away, lo, hi))):
        m = mid(t)
        if m is not None and (m - m0) * side >= MOVE:
            return t, True
    return None, True


def measure(data):
    """{'nights': {ET date: {games, score, summary, lead}}, 'counts', 'lag_s'}: each measure the
    seconds measured, in the order found."""
    markets = nbp.recorder_markets(data)
    scores, plays = nbp.recorder_rows(data, 'espn_scores'), nbp.recorder_rows(data, 'espn_plays')
    counts = Counter()
    matched = nbp.match_events(markets, [em.game_from_score_row(rows[-1]) for rows in scores.values()], counts)
    quotes = nbp.Quotes()
    keep = {tk for k, _, _ in matched for tk in (k.ticker_a, k.ticker_b)}
    lj.drive(data, nbp.TopReplay(lambda tk: tk in keep, quotes), 3600 * 1_000_000, lambda t: None,
             lj.down_instants(data))
    nights = defaultdict(lambda: {'games': 0, 'score': [], 'summary': [], 'lead': []})
    for k, g, a_home in matched:
        home, away = nbp.sides(k, a_home)
        night = nights[k.date]
        night['games'] += 1
        fp, late = first_plays(plays.get(g.id, []))
        counts['late_plays'] += late
        for r in fp:
            w = em.iso_ms(r.get('wallclock') or '')
            if w:
                night['summary'].append((r['recv_us'] // 1000 - w) / 1000)
            else:
                counts['no_wallclock'] += 1
        for t, hs, as_, side in score_changes(scores[g.id]):
            counts['score_changes'] += 1
            play = next((r for r in fp if (nbp.as_int(r.get('home_score')), nbp.as_int(r.get('away_score')))
                         == (hs, as_)), None)
            w = em.iso_ms(play.get('wallclock') or '') if play else 0
            if w:
                night['score'].append((t - w) / 1000)
            else:
                counts['no_play'] += 1
            kt, had = kalshi_move(quotes, home, away, t, side)
            if kt is not None:
                night['lead'].append((t - kt) / 1000)
            else:
                counts['no_move' if had else 'no_book'] += 1
    every = {m: [x for n in nights.values() for x in n[m]] for m in MEASURES}
    return {'data': os.path.basename(os.path.normpath(data)), 'nights': dict(sorted(nights.items())),
            'all': dict(every, games=sum(n['games'] for n in nights.values())), 'counts': dict(counts),
            'lag_s': gate_lag(every['score'])}


def quantiles(vals):
    return {'n': len(vals), **{'p%d' % k: pct(vals, q) for k, q in QUANTILES}}


def summary(res):
    """The result with each night's measures as their quantiles (--json)."""
    def q(n):
        return dict({m: quantiles(n[m]) for m in MEASURES}, games=n['games'])
    return {'data': res['data'], 'nights': {d: q(n) for d, n in res['nights'].items()}, 'all': q(res['all']),
            'counts': res['counts'], 'lag_s': res['lag_s']}


def report(res):
    def cells(vals):
        q = quantiles(vals)
        return '%5d %6s %6s %6s' % (q['n'], *('-' if q[k] is None else '%.1f' % q[k] for k in ('p50', 'p90', 'p99')))
    head = '%-12s %5s  %-26s  %-26s  %-26s' % ('', '', 'score lag (s)', 'summary lag (s)', 'Kalshi lead (s)')
    sub = '%-12s %5s  ' % ('night', 'games') + '  '.join(['%5s %6s %6s %6s' % ('n', 'p50', 'p90', 'p99')] * 3)
    lines = ['nba_latency: ESPN and Kalshi on the recorded NBA games of %s: %d games over %d nights' % (
        res['data'], res['all']['games'], len(res['nights'])), head.rstrip(), sub]
    for d, n in list(res['nights'].items()) + [('all', res['all'])]:
        lines.append('%-12s %5d  ' % (d, n['games']) + '  '.join(cells(n[m]) for m in MEASURES))
    c = res['counts']
    lines.append('  score changes %d: without their play %d, without a 3c Kalshi move %d, without a book %d; '
                 'late plays dropped %d, plays without a wallclock %d' % (
                     c.get('score_changes', 0), c.get('no_play', 0), c.get('no_move', 0), c.get('no_book', 0),
                     c.get('late_plays', 0), c.get('no_wallclock', 0)))
    lines.append('the gates\' lag: %s' % (
        '--lag-s %.1f (the score lag\'s p90, rounded up to the half second) for nba_panel.py and nba_gate.py n1, '
        'in place of the assumed %.1f s' % (res['lag_s'], nbp.LAG_S) if res['lag_s'] is not None
        else 'none measured (no score change with its play): the gates keep the assumed %.1f s' % nbp.LAG_S))
    return '\n'.join(lines)


def run(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--data', help="the recorder's data directory (required: no default)")
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.data:
        print('nba_latency: --data DIR is required; there is no default (no tool reads data/ unless told to)',
              file=sys.stderr)
        return 2
    if not os.path.isdir(a.data):
        print('nba_latency: %s is not a directory' % a.data, file=sys.stderr)
        return 2
    res = measure(a.data)
    print(json.dumps(summary(res), indent=1) if a.json else report(res))
    return 0


# ---- self-test ----------------------------------------------------------------------------------
# Two nights, a game each. Per score change: (ESPN's wallclock, s after the tip; the scores after
# it; the scoreboard's first sighting, s after the wallclock; the summary's; Kalshi's moves, each
# (s from the sighting, cents toward the scorer)). Game 2's last moves 3c 90 s before its sighting,
# then 1c more: from where it stood 60 s before, no 3c move
GAMES = (
    (('BOS', 'Boston', 'Boston Celtics'), ('DEN', 'Denver', 'Denver Nuggets'),
     ((300, (2, 0), 38, 45, ((-20, 4),)), (900, (2, 3), 44, 50, ((5, 3),)), (1500, (4, 3), 40, 42, ((10, 2),)))),
    (('LAL', 'Los Angeles L', 'Los Angeles Lakers'), ('PHX', 'Phoenix', 'Phoenix Suns'),
     ((400, (2, 0), 30, 33, ((-10, 3),)), (1000, (2, 2), 50, 47, ((-15, 5),)),
      (1600, (4, 2), 45, 48, ((-90, 3), (20, 1))))),
)


def synth(root, day0='2026-11-04T00:00Z'):
    """GAMES as the recorder writes them (lip_join.Spool): the markets, the scoreboard (pre, the tip,
    each change, a clock-only row, post), the summaries' plays (each change's, a revision of the
    first, a play inserted late under the last one shown and a play without a wallclock in game
    1), and both markets' socket books, the home team's at 0.49/0.51 to start, moved by each change's
    Kalshi moves (the away market mirrors it)."""
    sp, frames, us = lj.Spool(root), [], 1000
    for n, (h, aw, changes) in enumerate(GAMES):
        tip = em.iso_ms(day0) + n * 86400000
        d = datetime.fromtimestamp(tip / 1000 - 5 * 3600, tz=timezone.utc)
        ev = 'KXNBAGAME-%s%s%02d%s%s' % (d.strftime('%y'), d.strftime('%b').upper(), d.day, aw[0], h[0])
        home, away = ev + '-' + h[0], ev + '-' + aw[0]
        for tk, name in ((home, h[1]), (away, aw[1])):
            sp.write('kalshi_markets', {'recv_us': (tip - 3600000) * us, 'series': 'KXNBAGAME', 'ticker': tk,
                                        'status': 'active',
                                        'raw': json.dumps({'event_ticker': ev, 'yes_sub_title': name})})
        start = datetime.fromtimestamp(tip / 1000, tz=timezone.utc).strftime('%Y-%m-%dT%H:%MZ')
        raw = json.dumps({'competitors': [
            {'homeAway': 'home', 'team': {'id': '1', 'abbreviation': h[0], 'displayName': h[2]}},
            {'homeAway': 'away', 'team': {'id': '2', 'abbreviation': aw[0], 'displayName': aw[2]}}]})
        gid = str(401970000 + n)

        def score(t, state, hs, as_):
            sp.write('espn_scores', {'recv_us': t * us, 'prev_us': None, 'league': 'basketball/nba', 'game_id': gid,
                                     'start_time': start, 'state': state, 'status': None, 'period': '1',
                                     'clock': '5:00', 'home': h[0], 'away': aw[0], 'home_score': hs,
                                     'away_score': as_, 'detail': None, 'raw': raw})

        def play(t, pid, seq, wall, sc, revised=False):
            sp.write('espn_plays', {'recv_us': t * us, 'prev_us': None, 'league': 'basketball/nba', 'game_id': gid,
                                    'play_id': pid, 'seq': seq, 'period': 1, 'clock': '5:00',
                                    'wallclock': None if wall is None else datetime.fromtimestamp(
                                        wall / 1000, tz=timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
                                    'home_score': sc[0], 'away_score': sc[1], 'team_id': None, 'scoring': True,
                                    'score_value': 2, 'type': 'x', 'text': 'x', 'wp_home': None, 'wp_tie': None,
                                    'revised': revised, 'raw': '{}'})
        score(tip - 600000, 'pre', 0, 0)
        score(tip + 60000, 'in', 0, 0)
        mid, last = 5000, (0, 0)
        frames.append((tip - 900000, home, away, mid))
        for i, (w_s, sc, sb_s, pl_s, moves) in enumerate(changes):
            wall = tip + w_s * 1000
            seen = wall + sb_s * 1000
            score(seen, 'in', *sc)
            play(wall + pl_s * 1000, 'p%d' % i, 10 * (i + 1), wall, sc)
            side = 1 if (sc[0] - sc[1]) > (last[0] - last[1]) else -1
            for k_s, k_c in moves:
                mid += side * k_c * 100
                frames.append((seen + k_s * 1000, home, away, mid))
            last = sc
        score(tip + 2000000, 'in', *last)   # the clock alone moved: no change
        if n == 0:
            w3 = tip + changes[-1][0] * 1000
            play(tip + (changes[0][0] + 55) * 1000, 'p0', 10, tip + changes[0][0] * 1000, changes[0][1], True)
            play(w3 + 52000, 'late', 25, w3 - 100000, changes[-1][1])
            play(w3 + 60000, 'nowall', 31, None, changes[-1][1])
        score(tip + 9000000, 'post', *last)
    rows = []
    for t, home, away, mid in frames:   # each market's YES touch at mid +- 1c (the away market 1 - that)
        rows.append((t, home, [((mid - 100) / 1e4, 10)], [((1e4 - mid - 100) / 1e4, 10)]))
        rows.append((t, away, [((1e4 - mid - 100) / 1e4, 10)], [((mid - 100) / 1e4, 10)]))
    for seq, (t, tk, yes, no) in enumerate(sorted(rows), start=1):
        sp.snapshot(t * us, 1, 1, seq, tk, yes, no)


def self_test():
    fails = []

    def check(name, ok, detail=''):
        print('%s %s%s' % ('OK  ' if ok else 'FAIL', name, (': ' + str(detail)) if detail else ''))
        if not ok:
            fails.append(name)

    check('pct: nearest rank (the 3rd of 5 the median, the 5th the 90th; 2 of 4 the median)',
          pct([50, 30, 44, 38, 40], 0.5) == 40 and pct([50, 30, 44, 38, 40], 0.9) == 50 and pct([1, 2, 3, 4], 0.5) == 2
          and pct([7], 0.99) == 7 and pct([], 0.5) is None)
    check('gate_lag: the p90 rounded up to the half second (the 9th of 10)', gate_lag([41.2] * 10) == 41.5
          and gate_lag([41.5] * 3) == 41.5 and gate_lag([10.0] * 8 + [41.6] * 2) == 42.0
          and gate_lag([10.0] * 9 + [41.6]) == 10.0 and gate_lag([]) is None)
    r1 = {'play_id': 'a', 'recv_us': 1, 'seq': 5}
    fp, late = first_plays([r1, dict(r1, recv_us=2), {'play_id': 'b', 'recv_us': 3, 'seq': 4},
                            {'play_id': 'c', 'recv_us': 3, 'seq': 6}])
    check('first_plays: a play at its first row; one first seen under a shown sequence dropped',
          [r['play_id'] for r in fp] == ['a', 'c'] and fp[0]['recv_us'] == 1 and late == 1)
    rows = [{'state': 'pre', 'recv_us': 0, 'home_score': 0, 'away_score': 0},
            {'state': 'in', 'recv_us': 1000, 'home_score': 0, 'away_score': 0},
            {'state': 'in', 'recv_us': 2000, 'home_score': '2', 'away_score': '0'},
            {'state': 'in', 'recv_us': 3000, 'home_score': 2, 'away_score': 0},
            {'state': 'in', 'recv_us': 4000, 'home_score': 4, 'away_score': 2},
            {'state': 'in', 'recv_us': 5000, 'home_score': 4, 'away_score': 5}]
    check("score_changes: each change in play, the scorer's side; the clock alone and a level change are not",
          score_changes(rows) == [(2, 2, 0, 1), (5, 4, 5, -1)], score_changes(rows))

    with tempfile.TemporaryDirectory() as d:
        synth(d)
        res = measure(d)
        n1, n2 = list(res['nights'].values())
        # night 1: the scoreboard 38, 44 and 40 s behind the plays, the summaries 45, 50 and 42 (the
        # late play's 152 s and the play without a wallclock left out); Kalshi 4c toward BOS 20 s
        # before the first sighting (+20), 3c toward DEN 5 s after the second (-5), 2c only on the third
        check('night 1: score lags 38, 44, 40; summary 45, 50, 42; Kalshi leads +20 and -5 (3c counts), the third '
              'has no 3c move', (n1['score'], n1['summary'], n1['lead']) == ([38.0, 44.0, 40.0], [45.0, 50.0, 42.0],
                                                                          [20.0, -5.0]),
              (n1['score'], n1['summary'], n1['lead']))
        check('night 2: score lags 30, 50, 45; summary 33, 47, 48; Kalshi +10, +15, and none from where the third '
              'stood 60 s before (3c 90 s before, 1c after)', (n2['score'], n2['summary'], n2['lead'])
              == ([30.0, 50.0, 45.0], [33.0, 47.0, 48.0], [10.0, 15.0]), (n2['score'], n2['summary'], n2['lead']))
        check('the counts: 6 changes, 2 without a 3c move, 1 late play, 1 play without a wallclock; both games matched',
              res['counts'] == {'events': 2, 'score_changes': 6, 'no_move': 2, 'late_plays': 1, 'no_wallclock': 1}
              and res['all']['games'] == 2, res['counts'])
        s = summary(res)
        a = s['all']
        # every night's score lags 30, 38, 40, 44, 45, 50: the 3rd the median, the 6th the 90th and 99th
        check('all nights: score p50 40, p90 50, p99 50; summary p50 45, p90 50; Kalshi p50 +10, p90 +20',
              (a['score']['p50'], a['score']['p90'], a['score']['p99'], a['summary']['p50'], a['summary']['p90'],
               a['lead']['p50'], a['lead']['p90']) == (40.0, 50.0, 50.0, 45.0, 50.0, 10.0, 20.0)
              and s['nights']['2026-11-03']['score'] == {'n': 3, 'p50': 40.0, 'p90': 44.0, 'p99': 44.0}, a)
        check("the gates' lag: 50.0 s, the score lag's p90", res['lag_s'] == 50.0, res['lag_s'])
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = run(['--data', d])
            rc_none = run([])
            rc_missing = run(['--data', os.path.join(d, 'none')])
        text = out.getvalue()
        check('the report: a row a night and all, and the lag to use', rc == 0 and '--lag-s 50.0' in text
              and '2026-11-03' in text and '2026-11-04' in text and '\nall ' in text, text.strip().splitlines()[-1:])
        check('--data is required, and a directory', rc_none == 2 and rc_missing == 2)
    print('nba_latency self-test: %s' % ('ok' if not fails else 'FAILED (%d)' % len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(run(sys.argv[1:]))
