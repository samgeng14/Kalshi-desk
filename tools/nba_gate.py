#!/usr/bin/env python3
"""N1's and N2's gates (nba.json): does the NBA model beat Kalshi's price, and can N2's favourites be bought?

N1 is plan 5(f) generalised to the NBA: tools/train_bball.py --league nba's logistic, priced against
KXNBAGAME's books and taken as $2 IOCs. nba.json gates it in three stages, in order, each a
subcommand here writing gates_dir/N1.json (strat/lane_gate.h's format) for deskd to load:

  n1      G1 and G2, on tools/nba_panel.py's panel: both pass, PASS at stage shadow (deskd's N1
          engine writes shadow signals from the gate's params; gw keeps the lane gated)
  shadow  G3, on those shadow signals in ops.sqlite: passes, stage live (the lane may be switched on)
  n2      N2's gate (pregame 90-95c favourites, the sports hunt's forward test): gates_dir/N2.json

The sports hunt found the NBA's books efficient (docs/edge_results.json hunt:sports: "no logistic
recalibration beats the Kalshi mid out of sample"): the expected verdict is a FAIL at G1, and these
gates make that a measurement.

G1, held-out Brier (n1). The held-out season (--held-out, default the model's own brier.held_out)
is priced by the shipped model when it is not a season the model was fitted on (its seasons), else
by the same features refitted on the panel's other seasons (train_bball.fit: the trainer's own
held-out score), so no row is priced by coefficients that saw its outcome. The rows: the season's
rows with a kalshi_mid (the home team's effective mid). Brier(model) and Brier(mid) on those rows,
and an 80% upper bound of their difference by a paired bootstrap: --boot resamples of the season's
games with replacement (random.Random(--seed): the same panel gives the same bound), each the mean
difference over its games' rows; the bound is the smallest resample mean with at least 80% of
them at or under it. Passes with 150+ games (plan 5(f)'s 150+ settled games), Brier(model) under
Brier(mid) and the bound under 0 (nba.json: stricter than the plan's "at least the mid's").

G2, the executable replay (n1). The same held-out games, traded by N1's rule exactly:
tools/take_rule.py's decide (deskd's take_decide) in mode MODEL with take_params' defaults ($2
clips up to 4 contracts, 10 a market, 4 takes a game 10 s apart, $10 at risk, the score first seen
at most 30 s before, spreads of 3c or less, the team's price in 5-95c, no overtime nor the last
2:00) and --thr as its edge. A game is its two markets, A the first by ticker (as TakeEngine); the
fair for A's team is the model's home probability, or 1 minus it. A decision at each panel row, at
the instant its state was visible: history rows at the running maximum of the play wallclocks
plus the panel's lag, which must be at least --lag-s (the measured lag, tools/nba_latency.py;
41.5 s until measured), recorder rows at the poll that first showed them. The game runs as
TakeEngine runs it (x1_backtest.simulate: signal runs, the cool-down, the game's takes and worst
case, the lane's position under its inventory). The book is the row's touches (no sizes: taken as
deep enough). A take fills at the worse of the touch it saw and the same side a minute on (the
panel's _next columns, hunt:college's conservative next-minute price; that side empty: no fill,
counted) and is held to settlement: contracts x (1 if its team won) - contracts x price - the fee
(take_rule's cent fee, --fee-mult: 1, TO CONFIRM for KXNBAGAME). Per trade in dollars, t and an
80% lower bound clustered by game (x1_backtest.summarize). Reported beside it, the touch's own
settlement P&L and 60 s markout (the team's mid a minute on less that touch and the fee; a fill at
the minute-on price has no markout the panel can read). hunt:college's placebos are the same rule
with another fair: no score (the fair is p0), flipped margin (the model with the margin and
possession negated) and the price-only favourite (a fair of 1 for the team whose effective mid is
over 0.5). Passes with 50+ trades in 30+ games, a positive mean with its 80% lower bound above 0,
and no placebo's mean above 0 (a placebo that never takes passes).

Both pass: PASS at stage shadow, else FAIL with the reasons. N1.json carries the model file's
sha256 (a retrained model closes the gate), params {thr} (deskd reads thr: load_lane_gate(path,
"N1", the model's sha256, {"thr"})) and the numbers in evidence.

G3, the shadow signals (shadow). Deskd's N1 engine, shadowing at stage shadow, writes each take it
would make to ops.sqlite's signals table (store/store.h; the nightly ops-research.sqlite copy is
the one to read): lane N1, shadow 1, action SHADOW (PULL rows are not takes), the order's price
and contracts, the edge, the fee in the row's json, and mo60, the gross 60 s markout a contract at
the market's mid (+ in its favour). Counted: those rows from the shadow stage's start on (n1's
gate's computed time; a shadow run that leaves the stage at shadow carries it in evidence.g3.since),
with an edge at least the gate's thr. Per signal in dollars: the net 60 s markout, mo60 x contracts
- the fee, and the settlement P&L from the recorder's kalshi_markets results (--data). Passes with
50+ signals with a markout, on 14+ ET days, a positive mean net 60 s markout, and 50+ settled with
a mean settlement P&L of at least 0: N1.json at stage live, the shadow gate's params, model and
evidence kept, G3's added. Otherwise the stage stays shadow (still a PASS: G1 and G2 stand) with
G3's reason. The gate (--gate, gates_dir/N1.json) must load as deskd loads it with --model, at
stage shadow (a live one's engine no longer shadows: a re-run would close it).

N2 (n2). docs/edge_results.json hunt:sports' only NBA cell above the bar is pregame [90, 95)
favourites, +5.43 +- 1.78c (76 of 78 won); it advises a forward test at 5 contracts for 60 trades
with a -$15 stop, which is how N2 trades live (gw retires it). What this season's recorded books
must confirm is that the rule can be done: take_rule's FAV_PREGAME (5 contracts, the team's ask in
[0.90, 0.95), from 60 to 2 minutes before ESPN's scheduled tip, the score at most 60 s old and
still pre, spreads of 10c or less) replayed on the recorder's KXNBAGAME socket books, a decision
at the window's open and at each book or score change in it. Passes when --min-games (10) games
were bought to 5 contracts: N2.json at stage live, params {count, fav_lo, fav_hi}, no model. The
takes' settlement P&L is reported, not gated (the forward test is the measurement).

The gate files follow strat/lane_gate.h, checked against lane_gate.cpp's rules
(x1_backtest.gate_refusal) before writing; --write FILE refuses an existing file without --force.

Standard library only. No network. Reads only the files given (no default path: no tool reads
data/ unless told to) and writes only --write.

Usage:
  python3 tools/nba_gate.py n1 --panel PANEL.csv --model MODEL.json [--held-out SEASON] [--thr 0.10]
                               [--lag-s 41.5] [--fee-mult 1] [--boot 2000] [--seed 1] [--json]
                               [--write N1.json [--force]]
  python3 tools/nba_gate.py shadow --ops ops-research.sqlite --data DIR --gate N1.json --model MODEL.json
                                   [--json] [--write N1.json --force]
  python3 tools/nba_gate.py n2 --data DIR [--fee-mult 1] [--min-games 10] [--json] [--write N2.json [--force]]
  python3 tools/nba_gate.py --self-test
"""
import argparse
import bisect
import contextlib
import csv
import hashlib
import io
import json
import math
import os
import random
import re
import sqlite3
import sys
import tempfile
from collections import Counter, defaultdict
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import espn_match as em   # noqa: E402  (ET dates)
import lip_join as lj     # noqa: E402  (the recorder's reader, its prices and the book replay)
import nba_panel as nbp   # noqa: E402  (the panel's columns; the recorder's NBA games and results)
import take_rule as tr    # noqa: E402  (N1's and N2's rule: decide and the cent fee)
import train_bball as tb  # noqa: E402  (the model: its loader check, predict, the held-out refit)
import x1_backtest as x1  # noqa: E402  (Book, simulate, summarize, evaluate, the gate file's rules and writer)

ONE, CT = tr.ONE, tr.CT
THR = 0.10                  # N1's edge in dollars until a gate measures another (take_params)
LAG_S = nbp.LAG_S           # until tools/nba_latency.py measures the NBA's
BOOT, SEED = 2000, 1
MIN_G1_GAMES = 150          # plan 5(f): 150+ settled games
MIN_G2_TRADES, MIN_G2_GAMES = 50, 30
MIN_G3_SIGNALS, MIN_G3_DAYS = 50, 14
MIN_N2_GAMES = 10
N1_PARAMS = ('thr',)        # what deskd's N1 engine reads from its gate
N2_PARAMS = ('count', 'fav_lo', 'fav_hi')
PLACEBOS = ('no_score', 'flipped_margin', 'favourite')
FAV = tr.params('FAV_PREGAME')
# N2's study, the evidence its gate cites (docs/edge_results.json hunt:sports)
STUDY = {'source': 'docs/edge_results.json hunt:sports', 'cell': 'NBA pregame favourites, ask in [0.90, 0.95)',
         'net_c': 5.43, 'se_c': 1.78, 'won': 76, 'n': 78,
         'advice': 'forward test at 5-10 contracts for >= 60 trades with a -$15 stop'}

# the panel's columns the replay reads (tools/nba_panel.py writes them all)
TOUCHES = tuple('%s_%s%s' % (s, k, n) for n in ('', '_next') for s in ('home', 'away') for k in ('bid', 'ask'))
REPLAY = ('visible_ms', 'seen_ms', 'event', 'home_ticker', 'away_ticker', 'start_ms', 'home_score', 'away_score',
          'source', 'lag_s', 'kalshi_bid', 'kalshi_ask') + TOUCHES

# ops.sqlite's signals table, as src/deskd/store/store.cpp creates it (the self-test checks it there)
SIGNALS_DDL = (
    'CREATE TABLE IF NOT EXISTS signals(\n'
    '  id TEXT PRIMARY KEY, lane TEXT, mode TEXT, shadow INTEGER, ts_ms INTEGER, tk TEXT, ev TEXT, side TEXT,\n'
    '  px REAL, qty REAL, fair REAL, edge REAL, score TEXT, src TEXT, age_ms INTEGER, first_seen_ms INTEGER,\n'
    '  touch_since_ms INTEGER, action TEXT, why TEXT, cid TEXT, mo10 REAL, mo60 REAL, mo300 REAL, json TEXT NOT NULL)')


def _r(x):
    """A number for a gate's evidence, rounded so the file reads the same on any machine."""
    return None if x is None else round(x, 6)


def _stats(s):
    """summarize's numbers for a gate's evidence."""
    return {'n': s['n'], 'games': s['games'], 'mean': _r(s['mean']), 't': _r(s['t']), 'lower80': _r(s['lower80'])}


def _usd(x):
    return '-' if x is None else x1._usd(x)


def now_iso():
    return datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


# ---- the model and the panel ----------------------------------------------------------------------
def load_model(path):
    """(the model dict, the sha256 of its file's bytes): an nba_logit file whose golden rows
    reproduce (the trainer's spec check: deskd refuses a model whose rows do not)."""
    with open(path, 'rb') as fh:
        body = fh.read()
    try:
        obj = json.loads(body)
    except ValueError:
        raise ValueError('%s is not JSON' % path)
    if not isinstance(obj, dict) or obj.get('model') != 'nba_logit':
        raise ValueError('%s is not an nba_logit model' % path)
    err = tb.spec_loader_check(obj, exact_keys=False)
    if err:
        raise ValueError('%s: %s' % (path, err))
    return obj, hashlib.sha256(body).hexdigest()


def read_panel(path):
    """The panel's rows as train_bball.read_panel reads them (its checks), each with the replay's
    columns when the panel has them (nba_panel's: touches in Px, None for a blank side); and
    whether it has them."""
    rows, _ = tb.read_panel(path, 'nba')
    with open(path, newline='', encoding='utf-8-sig') as fh:
        rd = csv.DictReader(fh)
        rd.fieldnames = [c.strip() for c in rd.fieldnames or []]
        have = all(c in rd.fieldnames for c in REPLAY)
        raw = list(rd)
    if not have:
        return rows, False

    def whole(x, k, line):
        try:
            return int(x[k])
        except (TypeError, ValueError):
            raise tb.PanelError('line %d: %s %r is not a whole number' % (line, k, x[k]))

    def px(x, k, line):
        if not x[k]:
            return None
        p = lj.px_of(x[k])
        if p is None or not 0 < p < ONE:
            raise tb.PanelError('line %d: %s %r is not a price in dollars' % (line, k, x[k]))
        return p
    for line, (r, x) in enumerate(zip(rows, raw), start=2):
        x = {k: (v or '').strip() for k, v in x.items()}
        try:
            lag = float(x['lag_s']) if x['lag_s'] else None
        except ValueError:
            raise tb.PanelError('line %d: lag_s %r is not a number' % (line, x['lag_s']))
        r.update(visible_ms=whole(x, 'visible_ms', line), seen_ms=whole(x, 'seen_ms', line),
                 start_ms=whole(x, 'start_ms', line), home_score=whole(x, 'home_score', line),
                 away_score=whole(x, 'away_score', line), event=x['event'] or r['game_id'],
                 home=x['home_ticker'], away=x['away_ticker'], source=x['source'], lag_s=lag,
                 eff=(px(x, 'kalshi_bid', line), px(x, 'kalshi_ask', line)))
        for n in ('', '_next'):
            r['q' + n] = {s: (px(x, '%s_bid%s' % (s, n), line), px(x, '%s_ask%s' % (s, n), line))
                          for s in ('home', 'away')}
    return rows, True


def home_p(model, r, flip=False):
    """The model's P(the home team wins) at a row; flip negates the margin and the possession."""
    s = -1.0 if flip else 1.0
    return tb.predict(model, r['t_left_s'], s * r['margin'], s * r['possession'], r['p0'], r['ot'])


def favourite(r):
    """The price-only placebo's fair: 1 when the home team's effective mid is over 0.5, 0 under,
    None level or without one."""
    bid, ask = r['eff']
    if bid is None or ask is None or bid + ask == ONE:
        return None
    return 1.0 if bid + ask > ONE else 0.0


# ---- G1: the held-out Brier --------------------------------------------------------------------------
def upper80(games, boot=BOOT, seed=SEED):
    """The paired bootstrap's 80% upper bound of the mean difference: games [(the sum of its rows'
    differences, its rows)], boot resamples of them with replacement (random.Random(seed)), each
    resample's mean a row; the smallest resample mean with at least 80% of them at or under it."""
    rng = random.Random(seed)
    k = len(games)
    means = []
    for _ in range(boot):
        s = n = 0
        for _ in range(k):
            d, c = games[rng.randrange(k)]
            s += d
            n += c
        means.append(s / n)
    means.sort()
    return means[math.ceil(0.8 * boot) - 1]


def g1(rows, fair, boot=BOOT, seed=SEED):
    """G1 (module docstring) on the held-out season's rows, fair(row) the model's home probability."""
    per, bm, bk = {}, 0.0, 0.0
    for r in rows:
        if r['kalshi_mid'] is None:
            continue
        y = r['yes_won']
        e, k = (fair(r) - y) ** 2, (r['kalshi_mid'] - y) ** 2
        bm += e
        bk += k
        g = per.setdefault(r['game_id'], [0.0, 0])
        g[0] += e - k
        g[1] += 1
    n = sum(g[1] for g in per.values())
    out = {'games': len(per), 'rows': n, 'brier_model': bm / n if n else None, 'brier_mid': bk / n if n else None,
           'diff': (bm - bk) / n if n else None, 'boot': boot, 'seed': seed,
           'upper80': upper80([per[k] for k in sorted(per)], boot, seed) if per else None}
    if len(per) < MIN_G1_GAMES:
        why = 'G1: %d held-out games with a Kalshi mid, under %d' % (len(per), MIN_G1_GAMES)
    elif out['diff'] >= 0:
        why = "G1: Brier %.4f is not below the mid's %.4f" % (out['brier_model'], out['brier_mid'])
    elif out['upper80'] >= 0:
        why = "G1: Brier %.4f is below the mid's %.4f, but the difference's 80%% upper bound is %+.4f" % (
            out['brier_model'], out['brier_mid'], out['upper80'])
    else:
        why = ''
    out.update(passed=not why, why=why)
    return out


# ---- G2: the executable replay -------------------------------------------------------------------------
class PanelGame(x1.Game):
    """One held-out game of the panel, for x1_backtest.simulate: a decision at each row, at its
    visible_ms, from the row's touches (a Book: a market with neither side is no book) and
    self.fair(row), P(the home team wins) or None."""

    def __init__(self, rows, fee_mult):
        r0 = rows[0]
        home, away = r0['home'], r0['away']
        a, b = sorted((home, away))
        won = r0['yes_won'] == 1.0
        super().__init__(r0['event'], 'nba', em.et_date(r0['start_ms']) if r0['start_ms'] > 0 else '', (a, b),
                         a == home, {home: 'yes' if won else 'no', away: 'no' if won else 'yes'},
                         {a: fee_mult, b: fee_mult})
        self.rows = sorted(rows, key=lambda r: r['visible_ms'])
        self.at_t = {r['visible_ms']: r for r in self.rows}
        self.side = {home: 'home', away: 'away'}
        self.fair = None
        self.book = x1.Book()
        for r in self.rows:
            for tk in (home, away):
                bid, ask = r['q'][self.side[tk]]
                t = r['visible_ms']
                self.book.add(tk, t, None if bid is None and ask is None else x1.Quote(t, bid, None, ask, None))

    def instants(self, book):
        for r in self.rows:
            t = r['visible_ms']
            hs, as_ = r['home_score'], r['away_score']
            a_sc, b_sc = (hs, as_) if self.a_home else (as_, hs)
            point = (t, 'in', r['t_left_s'], r['ot'], a_sc, b_sc, None, r['seen_ms'], 0, t)
            _, inp, quotes = self.input_at(book, point)
            p = self.fair(r)
            inp['p0'] = x1.to_px(r['p0'] if self.a_home else 1 - r['p0'])
            inp['fair'] = None if p is None else x1.to_px(p if self.a_home else 1 - p)
            yield t, inp, quotes


def mid_of(q):
    """A (bid, ask) touch's mid in dollars, None unless two-sided."""
    bid, ask = q
    return None if bid is None or ask is None or bid >= ask else (bid + ask) / 2 / ONE


def score_take(trade, game, conservative=True):
    """A take's settlement P&L, in dollars, at its entry: the worse of the touch it saw and the
    same side a minute on (conservative; that side empty then: None, no fill), or the touch it
    saw, which also has its net 60 s markout (None conservative: the panel's quotes end there)."""
    r = game.at_t[trade['t']]
    buy = trade['side'] == 'BUY'
    price = trade['price']
    if conservative:
        bid, ask = r['q_next'][game.side[trade['ticker']]]
        nxt = ask if buy else bid
        if nxt is None:
            return None
        price = max(price, nxt) if buy else min(price, nxt)
    team_px = price if buy else ONE - price
    if not 0 < team_px < ONE:
        return None
    fee = tr.kalshi_fee_order_cent(team_px, trade['count'], trade['mult'])
    c, px, fee_d = trade['count'] / CT, team_px / ONE, fee / ONE
    team = game.tickers[trade['dir']]
    other = game.tickers[1 - trade['dir']]
    m = None if conservative else mid_of(r['q_next'][game.side[team]])
    if m is None and not conservative:
        o = mid_of(r['q_next'][game.side[other]])
        m = None if o is None else 1 - o
    won = game.team_result(trade['dir']) == 'yes'
    return {'entry': team_px, 'fee': fee, 'settle': (c if won else 0.0) - c * px - fee_d,
            'mo60': None if m is None else (m - px) * c - fee_d}


def replay(games, p, fair, conservative=True):
    """Every take of one rule over the games, each priced by fair: (its numbers, the takes)."""
    trades = []
    for g in games:
        g.fair = fair
        trades.extend(x1.simulate(g, g.book, p))
    by = {g.event: g for g in games}
    rows, unfilled = {'settle': [], 'mo60': []}, 0
    for t in trades:
        s = score_take(t, by[t['game']], conservative)
        if s is None:
            unfilled += 1
            continue
        for k in rows:
            if s[k] is not None:
                rows[k].append((t['game'], s[k]))
    return {'trades': len(trades) - unfilled, 'unfilled': unfilled,
            'settle': x1.summarize(rows['settle']), 'mo60': x1.summarize(rows['mo60'])}, trades


def n1_params(thr):
    """N1's TakeParams: take_params(MODEL), thr (dollars) as its edge."""
    p = tr.params('MODEL')
    p['edge'] = int(round(thr * ONE))
    return p


def g2(rows, model, thr=THR, fee_mult=x1.FEE_MULT):
    """G2 (module docstring) on the held-out season's rows."""
    by_game = defaultdict(list)
    for r in rows:
        by_game[r['game_id']].append(r)
    games = [PanelGame(rs, fee_mult) for _, rs in sorted(by_game.items())]
    p = n1_params(thr)
    fairs = {'no_score': lambda r: r['p0'], 'flipped_margin': lambda r: home_p(model, r, True), 'favourite': favourite}
    out = {'thr': thr, 'games': len(games)}
    out['rule'], out['trades'] = replay(games, p, lambda r: home_p(model, r))
    out['visible'] = replay(games, p, lambda r: home_p(model, r), False)[0]
    out['placebos'] = {k: replay(games, p, fairs[k])[0] for k in PLACEBOS}
    s = out['rule']['settle']
    if s['n'] < MIN_G2_TRADES:
        why = 'G2: %d trades, under %d' % (s['n'], MIN_G2_TRADES)
    elif s['games'] < MIN_G2_GAMES:
        why = 'G2: trades in %d games, under %d' % (s['games'], MIN_G2_GAMES)
    elif s['mean'] <= 0:
        why = 'G2: the mean settlement P&L is %s a trade' % _usd(s['mean'])
    elif s['lower80'] is None or s['lower80'] <= 0:
        why = 'G2: the mean settlement P&L is %s a trade, its 80%% lower bound %s' % (_usd(s['mean']),
                                                                                    _usd(s['lower80']))
    else:
        means = [(k, out['placebos'][k]['settle']['mean']) for k in PLACEBOS]
        bad = [(k, m) for k, m in means if m is not None and m > 0]
        why = 'G2: placebo %s makes %s a trade (it must not)' % (bad[0][0], _usd(bad[0][1])) if bad else ''
    out.update(passed=not why, why=why)
    return out


def n1(panel, model_path, held_out=None, thr=THR, lag_s=LAG_S, fee_mult=x1.FEE_MULT, boot=BOOT, seed=SEED):
    model, sha = load_model(model_path)
    rows, have = read_panel(panel)
    seasons = sorted({r['season'] for r in rows})
    season = held_out or (model.get('brier') or {}).get('held_out') or seasons[-1]
    if season not in seasons:
        raise ValueError('held-out season %s is not in the panel (%s)' % (season, ', '.join(seasons)))
    short = sorted({r['lag_s'] for r in rows if r.get('source') == 'history' and r['lag_s'] is not None
                    and r['lag_s'] < lag_s - 1e-9})
    if short:
        raise ValueError("the panel's history rows were built with --lag-s %g, under the measured %g: rebuild it with "
                         'tools/nba_panel.py --lag-s %g' % (short[0], lag_s, lag_s))
    if season in (model.get('seasons') or []):
        others = [r for r in rows if r['season'] != season]
        if not others:
            raise ValueError('the model was fitted on %s and the panel has no other season to refit on: --held-out '
                             'a season it was not fitted on' % season)
        fit = tb.fit(others, model['features'], tb.LEAGUES['nba'].regulation_s)
        fit['league'] = 'nba'
        how = 'refit on %s (the shipped model saw %s)' % (', '.join(sorted({r['season'] for r in others})), season)
    else:
        fit, how = model, 'the shipped model (%s is not among its seasons)' % season
    held = [r for r in rows if r['season'] == season]
    res = {'panel': os.path.basename(panel), 'model': os.path.basename(model_path), 'model_sha256': sha,
           'held_out': season, 'fair': how, 'thr': thr, 'lag_s': lag_s, 'fee_mult': fee_mult,
           'g1': g1(held, lambda r: home_p(fit, r), boot, seed)}
    if have:
        res['g2'] = g2(held, fit, thr, fee_mult)
    else:
        res['g2'] = {'thr': thr, 'passed': False, 'why': "G2: the panel has no replay columns (tools/nba_panel.py's)"}
    why = '; '.join(w for w in (res['g1']['why'], res['g2']['why']) if w)
    res['verdict'] = {'pass': not why, 'why': why, 'stage': 'shadow'}
    return res


def n1_gate(res, computed):
    """gates_dir/N1.json from an n1 run."""
    g, r2 = res['g1'], res['g2']
    ev = {k: res[k] for k in ('panel', 'model', 'held_out', 'fair', 'lag_s', 'fee_mult')}
    ev['g1'] = {'games': g['games'], 'rows': g['rows'], 'brier_model': _r(g['brier_model']),
                'brier_mid': _r(g['brier_mid']), 'diff': _r(g['diff']), 'upper80': _r(g['upper80']),
                'boot': g['boot'], 'seed': g['seed'], 'pass': g['passed']}
    ev['g2'] = {'pass': r2['passed']}
    if 'rule' in r2:
        ev['g2'].update(games=r2['games'], trades=r2['rule']['trades'], unfilled=r2['rule']['unfilled'],
                        settle=_stats(r2['rule']['settle']), settle_at_the_touch=_stats(r2['visible']['settle']),
                        mo60_at_the_touch=_stats(r2['visible']['mo60']),
                        placebos={k: _stats(v['settle']) for k, v in r2['placebos'].items()})
    v = res['verdict']
    return {'lane': 'N1', 'stage': v['stage'], 'verdict': 'PASS' if v['pass'] else 'FAIL',
            'why': None if v['pass'] else v['why'], 'computed': computed, 'tool': 'tools/nba_gate.py n1',
            'model_sha256': res['model_sha256'], 'params': {'thr': res['thr']}, 'evidence': ev}


# ---- G3: the shadow signals ----------------------------------------------------------------------------
def read_signals(path, lane):
    """lane's shadow rows of an ops.sqlite's signals table, oldest first: dicts with the row's
    numbers (px and edge in dollars, qty in contracts, mo60 a contract, fee the order's)."""
    con = sqlite3.connect('file:%s?mode=ro' % os.path.abspath(path), uri=True)
    try:
        cur = con.execute('SELECT id, action, ts_ms, tk, ev, side, px, qty, edge, mo60, json FROM signals '
                          'WHERE lane = ? AND shadow = 1 ORDER BY ts_ms, id', (lane,))
        out = []
        for i, action, ts, tk, ev, side, px, qty, edge, mo60, body in cur:
            try:
                fee = float((json.loads(body or '{}') or {}).get('fee'))
            except (TypeError, ValueError, AttributeError):
                fee = None
            out.append({'id': i, 'action': action, 'ts_ms': ts or 0, 'tk': tk or '', 'ev': ev or '',
                        'side': side, 'px': px, 'qty': qty, 'edge': edge, 'mo60': mo60, 'fee': fee})
    finally:
        con.close()
    return out


def g3(signals, results, thr, since_ms, fee_mult=x1.FEE_MULT):
    """G3 (module docstring) on read_signals' rows, results ticker -> 'yes', 'no' or ''."""
    counts, days = Counter(), set()
    rows = {'mo60': [], 'settle': []}
    for s in signals:
        if s['action'] != 'SHADOW':
            counts['not_a_take'] += 1
            continue
        if s['ts_ms'] < since_ms:
            counts['before_the_gate'] += 1
            continue
        if s['edge'] is None or s['edge'] < thr - 1e-9:
            counts['under_thr'] += 1
            continue
        if s['side'] not in ('BUY', 'SELL') or not s['px'] or not 0 < s['px'] < 1 or not s['qty'] or s['qty'] <= 0:
            counts['unreadable'] += 1
            continue
        counts['signals'] += 1
        days.add(em.et_date(s['ts_ms']))
        n, px, buy = s['qty'], s['px'], s['side'] == 'BUY'
        fee = s['fee']
        if fee is None:   # a row without the order's fee: the cent fee at the team's price
            team = int(round((px if buy else 1 - px) * ONE))
            fee = tr.kalshi_fee_order_cent(team, int(round(n * CT)), fee_mult) / ONE
        if s['mo60'] is not None:
            rows['mo60'].append((s['ev'], s['mo60'] * n - fee))
        else:
            counts['no_mo60'] += 1
        res = results.get(s['tk'])
        if res in ('yes', 'no'):
            yes = 1.0 if res == 'yes' else 0.0
            rows['settle'].append((s['ev'], ((yes - px) if buy else (px - yes)) * n - fee))
        else:
            counts['unsettled'] += 1
    out = {'thr': thr, 'since_ms': since_ms, 'counts': dict(counts), 'days': len(days),
           'mo60': x1.summarize(rows['mo60']), 'settle': x1.summarize(rows['settle'])}
    mo, st = out['mo60'], out['settle']
    if mo['n'] < MIN_G3_SIGNALS:
        why = 'G3: %d shadow signals with a 60 s markout, under %d' % (mo['n'], MIN_G3_SIGNALS)
    elif len(days) < MIN_G3_DAYS:
        why = 'G3: signals on %d days, under %d' % (len(days), MIN_G3_DAYS)
    elif mo['mean'] <= 0:
        why = 'G3: the mean net 60 s markout is %s a signal' % _usd(mo['mean'])
    elif st['n'] < MIN_G3_SIGNALS:
        why = 'G3: %d settled signals, under %d' % (st['n'], MIN_G3_SIGNALS)
    elif st['mean'] < 0:
        why = 'G3: the mean settlement P&L is %s a signal' % _usd(st['mean'])
    else:
        why = ''
    out.update(passed=not why, why=why)
    return out


def shadow(ops, data, gate_path, model_path, lane='N1', fee_mult=x1.FEE_MULT):
    _, sha = load_model(model_path)
    with open(gate_path, encoding='utf-8') as fh:
        try:
            gate = json.load(fh)
        except ValueError:
            gate = None
    why = x1.gate_refusal(gate, lane, sha, N1_PARAMS)
    if why is not None:
        raise ValueError('%s does not load as deskd would load it with %s: %s' % (gate_path, model_path, why))
    if gate['stage'] == 'live':
        raise ValueError('%s is already at stage live (G3 passed at %s): its engine no longer shadows' % (
            gate_path, gate.get('computed')))
    thr = float(gate['params']['thr'])
    # the shadow stage began when n1 wrote the gate; a shadow run that left it there carries that on
    since = (((gate.get('evidence') or {}).get('g3') or {}).get('since') or gate.get('computed') or '')
    results = {tk: m['result'] for tk, m in nbp.recorder_markets(data).items()}
    res = {'ops': os.path.basename(ops), 'data': os.path.basename(os.path.normpath(data)), 'lane': lane,
           'gate': gate, 'since': since, 'g3': g3(read_signals(ops, lane), results, thr, em.iso_ms(since), fee_mult)}
    res['stage'] = 'live' if res['g3']['passed'] else 'shadow'
    return res


def shadow_gate(res, computed):
    """gates_dir/N1.json from a shadow run: the shadow gate with G3's verdict and numbers."""
    g = res['g3']
    out = dict(res['gate'])
    ev = dict(out.get('evidence') or {})
    ev['g3'] = {'ops': res['ops'], 'data': res['data'], 'since': res['since'], 'thr': g['thr'],
                'signals': g['mo60']['n'], 'days': g['days'], 'counts': g['counts'], 'mo60_net': _stats(g['mo60']),
                'settle': _stats(g['settle']), 'pass': g['passed'], 'why': g['why'] or None}
    out.update(stage=res['stage'], verdict='PASS', why=None, computed=computed, tool='tools/nba_gate.py shadow',
               evidence=ev)
    return out


# ---- N2: the pregame favourites ------------------------------------------------------------------------
def espn_outages(data):
    """(from ms, to ms) of every ESPN outage in the gaps table, any feed's (a recovered row does not
    name its league; x1_backtest.read_data's rule) and every recorder restart."""
    out = []
    for r in lj.rows(data, 'gaps'):
        if (r.get('source'), r.get('kind')) in (('espn', 'poll_failed'), ('espn', 'recovered'),
                                                ('recorder', 'restart')) \
                and isinstance(r.get('from_us'), int) and isinstance(r.get('to_us'), int):
            out.append((r['from_us'] // 1000, r['to_us'] // 1000))
    return out


class PregameGame(x1.Game):
    """A recorded game before its tip, for x1_backtest.simulate in mode FAV_PREGAME: a decision at
    the window's open and at each book or score change in it, the score the last espn_scores row
    (polled then, but inside an ESPN outage: polled at its start)."""

    def __init__(self, event, date, tickers, a_home, results, fee_mult, start_ms, rows, outages):
        super().__init__(event, 'nba', date, tickers, a_home, results, fee_mult)
        self.start_ms, self.rows, self.outages = start_ms, rows, outages
        self.times = [r['recv_us'] // 1000 for r in rows]
        self.seen = x1.first_seen([(r.get('state'), r.get('period'), r.get('home_score'), r.get('away_score'))
                                   for r in rows], self.times)
        self.window = (start_ms - FAV['fav_from_ms'], start_ms - FAV['fav_to_ms'])
        self.next_ms = x1.DATA_NEXT_MS

    def instants(self, book):
        lo, hi = self.window
        ts = {lo} | {t for t in self.times if lo <= t <= hi}
        for tk in self.tickers:
            ts.update(t for t in book.ts.get(tk, ()) if lo <= t <= hi)
        for t in sorted(ts):
            i = bisect.bisect_right(self.times, t) - 1
            state, seen, fetched = ('', 0, 0) if i < 0 else (self.rows[i].get('state') or '', self.seen[i], t)
            for f, to in self.outages:
                if f <= t <= to:
                    fetched = min(fetched, f)
            _, inp, quotes = self.input_at(book, (t, state, None, False, 0, 0, None, seen, 0, fetched))
            inp['start_ms'] = self.start_ms
            yield t, inp, quotes


def read_pregame(data, fee_mult=x1.FEE_MULT):
    """The recorder's NBA games with a scheduled tip and both books: ({event: PregameGame}, Book,
    counts)."""
    markets = nbp.recorder_markets(data)
    scores = nbp.recorder_rows(data, 'espn_scores')
    counts = Counter()
    matched = nbp.match_events(markets, [em.game_from_score_row(rows[-1]) for rows in scores.values()], counts)
    book = x1.Book()
    keep = {tk for k, _, _ in matched for tk in (k.ticker_a, k.ticker_b)}
    lj.drive(data, x1.TopReplay(lambda tk: tk in keep, book), 3600 * 1_000_000, lambda t: None, lj.down_instants(data))
    outages, games = espn_outages(data), {}
    for k, g, a_home in matched:
        tickers = (k.ticker_a, k.ticker_b)
        if g.start_ms <= 0:
            counts['no_start'] += 1
            continue
        if not all(book.ts.get(tk) for tk in tickers):
            counts['no_book'] += 1
            continue
        games[k.event_ticker] = PregameGame(k.event_ticker, k.date, tickers, a_home,
                                            {tk: markets[tk]['result'] for tk in tickers},
                                            {tk: fee_mult for tk in tickers}, g.start_ms, scores[g.id], outages)
    counts['recorded'] = len(games)
    return games, book, dict(counts)


def n2(data, fee_mult=x1.FEE_MULT, min_games=MIN_N2_GAMES):
    games, book, counts = read_pregame(data, fee_mult)
    trades = []
    for _, g in sorted(games.items()):
        trades.extend(x1.simulate(g, book, FAV))
    bought = Counter()
    for t in trades:
        bought[t['game']] += t['count']
    full = sorted(ev for ev, c in bought.items() if c >= FAV['fixed_count'])
    res = {'data': os.path.basename(os.path.normpath(data)), 'fee_mult': fee_mult, 'min_games': min_games,
           'counts': counts, 'games': len(games), 'full': full,
           'partial': sorted(ev for ev, c in bought.items() if c < FAV['fixed_count']),
           'stats': x1.evaluate(trades, games, book, True), 'trades': trades}
    why = '' if len(full) >= min_games else (
        'N2: %d recorded games were bought to %d contracts at a [%.2f, %.2f) ask %d to %d min before the tip, under %d'
        % (len(full), FAV['fixed_count'] // CT, FAV['fav_lo'] / ONE, FAV['fav_hi'] / ONE, FAV['fav_from_ms'] // 60000,
           FAV['fav_to_ms'] // 60000, min_games))
    res.update(passed=not why, why=why)
    return res


def n2_gate(res, computed):
    """gates_dir/N2.json from an n2 run."""
    s = res['stats']
    ev = {'study': STUDY, 'data': res['data'], 'fee_mult': res['fee_mult'], 'min_games': res['min_games'],
          'counts': res['counts'], 'games': res['games'], 'bought_full': len(res['full']),
          'bought_partial': len(res['partial']), 'trades': s['trades'], 'unfilled': s['unfilled'],
          'settle': _stats(s['settle'])}
    return {'lane': 'N2', 'stage': 'live', 'verdict': 'PASS' if res['passed'] else 'FAIL',
            'why': None if res['passed'] else res['why'], 'computed': computed, 'tool': 'tools/nba_gate.py n2',
            'params': {'count': FAV['fixed_count'] // CT, 'fav_lo': FAV['fav_lo'] / ONE, 'fav_hi': FAV['fav_hi'] / ONE},
            'evidence': ev}


# ---- the reports ------------------------------------------------------------------------------------------
HEAD = '%-28s %5s %5s  %10s %6s %10s  %10s  %4s' % ('', 'n', 'games', 'settle', 't', 'lower80', 'mo60', 'miss')


def _t(s):
    return '-' if s['t'] is None else '%.2f' % s['t']


def _line(label, s):
    st, mo = s['settle'], s['mo60']
    return '%-28s %5d %5d  %10s %6s %10s  %10s  %4d' % (
        label, st['n'], st['games'], _usd(st['mean']), _t(st), _usd(st['lower80']), _usd(mo['mean']), s['unfilled'])


def report_n1(res):
    g, r2, v = res['g1'], res['g2'], res['verdict']
    out = ['nba_gate n1: N1 as deskd trades it (take_decide MODEL, thr %.2f, $2 clips); model %s (sha256 %s)'
           % (res['thr'], res['model'], res['model_sha256'][:12]),
           'held out %s of %s; fair: %s' % (res['held_out'], res['panel'], res['fair'])]
    if g['rows']:
        out.append('G1  Brier: model %.5f, Kalshi mid %.5f on %d rows of %d games; difference %+.5f, 80%% upper bound '
                   '%+.5f (%d resamples, seed %d): %s' % (g['brier_model'], g['brier_mid'], g['rows'], g['games'],
                                                         g['diff'], g['upper80'], g['boot'], g['seed'],
                                                         'pass' if g['passed'] else 'FAIL'))
    else:
        out.append(g['why'])
    if 'rule' in r2:
        out.append("G2  the replay: %d games, the panel's visible times (history lag at least %.1f s), entry the worse "
                   'of the touch and a minute on, fee multiplier %g; dollars a trade' % (r2['games'], res['lag_s'],
                                                                                      res['fee_mult']))
        out.append(HEAD)
        out.append(_line('N1', r2['rule']))
        out.append(_line('N1 at the touch it saw', r2['visible']))
        for k in PLACEBOS:
            out.append(_line('placebo ' + k.replace('_', ' '), r2['placebos'][k]))
    out.append('G2  pass' if r2['passed'] else r2['why'])
    out.append('verdict: %s' % ('PASS, stage shadow' if v['pass'] else 'FAIL: ' + v['why']))
    return '\n'.join(out)


def report_shadow(res):
    g = res['g3']
    c = ', '.join('%s %d' % kv for kv in sorted(g['counts'].items()))
    return '\n'.join([
        'nba_gate shadow: %s signals in %s (thr %.2f, from %s), results from %s' % (
            res['lane'], res['ops'], g['thr'], res['since'] or 'the start', res['data']),
        '  rows: %s; on %d ET days' % (c or 'none', g['days']),
        '  net 60 s markout: %d signals, %s a signal (t %s); settlement: %d, %s a signal (t %s)' % (
            g['mo60']['n'], _usd(g['mo60']['mean']), _t(g['mo60']), g['settle']['n'], _usd(g['settle']['mean']),
            _t(g['settle'])),
        'G3  pass' if g['passed'] else g['why'],
        'verdict: PASS, stage %s' % res['stage']])


def report_n2(res):
    s = res['stats']
    c = ', '.join('%s %d' % kv for kv in sorted(res['counts'].items()))
    return '\n'.join([
        'nba_gate n2: N2 as deskd trades it (take_decide FAV_PREGAME: 5 contracts at a [0.90, 0.95) ask, 60 to 2 min '
        'before the tip) on %s' % res['data'],
        '  games: %s' % (c or 'none'),
        '  bought to 5 contracts in %d games, partly in %d; %d takes (%d unfilled), settlement %s a take (t %s), '
        'reported only' % (len(res['full']), len(res['partial']), s['trades'], s['unfilled'], _usd(s['settle']['mean']),
                           _t(s['settle'])),
        '  the study: %s, %+.2f +- %.2fc (%d of %d won)' % (STUDY['cell'], STUDY['net_c'], STUDY['se_c'], STUDY['won'],
                                                          STUDY['n']),
        'verdict: %s' % ('PASS, stage live (the forward test: 5 contracts, retired after 60 trades or -$15)'
                         if res['passed'] else 'FAIL: ' + res['why'])])


def _public(res):
    """A result without its per-take lists and the gate it read."""
    out = {k: v for k, v in res.items() if k not in ('trades', 'gate')}
    if isinstance(out.get('g2'), dict):
        out['g2'] = {k: v for k, v in out['g2'].items() if k != 'trades'}
    return out


# ---- the command line --------------------------------------------------------------------------------------
def run(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--write-fixture', action='store_true', help=argparse.SUPPRESS)
    sub = ap.add_subparsers(dest='cmd')
    a1 = sub.add_parser('n1', help="G1 and G2 on nba_panel.py's panel: N1.json at stage shadow")
    a1.add_argument('--panel', help="the panel CSV (tools/nba_panel.py's)")
    a1.add_argument('--model', help='the NBA model file (models/nba_logit_vN.json)')
    a1.add_argument('--held-out', help="the season G1 and G2 score (default the model's brier.held_out)")
    a1.add_argument('--thr', type=float, default=THR, help='the edge in dollars (default %.2f)' % THR)
    a1.add_argument('--lag-s', type=float, default=LAG_S, help="the measured ESPN lag the panel's history rows must "
                    'have been built with at least (default %g, until tools/nba_latency.py measures it)' % LAG_S)
    a1.add_argument('--fee-mult', type=float, default=x1.FEE_MULT, help="KXNBAGAME's fee multiplier (default 1)")
    a1.add_argument('--boot', type=int, default=BOOT, help='G1 bootstrap resamples (default %d)' % BOOT)
    a1.add_argument('--seed', type=int, default=SEED)
    a3 = sub.add_parser('shadow', help="G3 on deskd's N1 shadow signals: N1.json at stage live")
    a3.add_argument('--ops', help="deskd's ops-research.sqlite (or ops.sqlite)")
    a3.add_argument('--data', help="the recorder's data directory (the markets' results)")
    a3.add_argument('--gate', help='the N1 gate deskd loads now (gates_dir/N1.json, stage shadow)')
    a3.add_argument('--model', help='the NBA model deskd loads')
    a3.add_argument('--lane', choices=('N1',), default='N1')
    a3.add_argument('--fee-mult', type=float, default=x1.FEE_MULT, help='for a row without its fee (default 1)')
    a2 = sub.add_parser('n2', help="N2's executability on the recorder's books: N2.json")
    a2.add_argument('--data', help="the recorder's data directory")
    a2.add_argument('--fee-mult', type=float, default=x1.FEE_MULT, help="KXNBAGAME's fee multiplier (default 1)")
    a2.add_argument('--min-games', type=int, default=MIN_N2_GAMES)
    for p in (a1, a3, a2):
        p.add_argument('--json', action='store_true')
        p.add_argument('--write', metavar='FILE', help='write the gate file (gates_dir/<lane>.json)')
        p.add_argument('--force', action='store_true', help='--write may replace an existing file')
    a = ap.parse_args(argv)
    if a.self_test or a.write_fixture:
        return self_test(write_fixture=a.write_fixture)

    def fail(msg):
        print('nba_gate: ' + msg, file=sys.stderr)
        return 2
    if a.cmd is None:
        return fail('n1, shadow or n2 (or --self-test)')
    need = {'n1': ('panel', 'model'), 'shadow': ('ops', 'data', 'gate', 'model'), 'n2': ('data',)}[a.cmd]
    missing = ['--' + k for k in need if not getattr(a, k)]
    if missing:
        return fail('%s %s required; there is no default (no tool reads data/ unless told to)' % (
            ' and '.join(missing), 'is' if len(missing) == 1 else 'are'))
    for k in need:
        v = getattr(a, k)
        if not (os.path.isdir(v) if k == 'data' else os.path.isfile(v)):
            return fail('no %s at %s' % ('directory' if k == 'data' else 'file', v))
    if a.write and os.path.exists(a.write) and not a.force:
        return fail('%s exists; --force replaces it' % a.write)
    if a.fee_mult < 0:
        return fail('--fee-mult must not be negative')
    try:
        if a.cmd == 'n1':
            if not 0 < a.thr < 1 or not 0 <= a.lag_s <= 600 or a.boot < 100:
                return fail('--thr must be in (0, 1), --lag-s 0 to 600 and --boot at least 100')
            res = n1(a.panel, a.model, a.held_out, a.thr, a.lag_s, a.fee_mult, a.boot, a.seed)
            gate, text = n1_gate, report_n1
        elif a.cmd == 'shadow':
            res = shadow(a.ops, a.data, a.gate, a.model, a.lane, a.fee_mult)
            gate, text = shadow_gate, report_shadow
        else:
            res = n2(a.data, a.fee_mult, a.min_games)
            gate, text = n2_gate, report_n2
    except (ValueError, OSError, sqlite3.Error, ArithmeticError) as e:
        return fail(str(e))
    if a.write:
        obj = gate(res, now_iso())
        why = x1.gate_refusal(obj, obj['lane'], obj.get('model_sha256') or '', N2_PARAMS if obj['lane'] == 'N2'
                              else N1_PARAMS)
        if why is not None and obj['verdict'] == 'PASS':
            return fail('the gate it would write does not load: %s' % why)
        x1.write_gate(a.write, obj, a.force)
    print(json.dumps(_public(res), indent=1, default=str) if a.json else text(res))
    if a.write:
        print('wrote %s' % a.write, file=sys.stderr)
    return 0


# ---- self-test -------------------------------------------------------------------------------------------
FIXTURE_N1 = os.path.join(ROOT, 'tests', 'deskd', 'fixtures', 'n1_gate.json')
FIXTURE_N2 = os.path.join(ROOT, 'tests', 'deskd', 'fixtures', 'n2_gate.json')
FIXTURE_COMPUTED = '2026-10-22T00:00:00Z'
SYNTH_TOOL = 'tools/nba_gate.py --self-test (SYNTHETIC: %s, not a measurement)'
MODEL_FIXTURE = os.path.join(ROOT, 'tests', 'deskd', 'fixtures', 'nba_synthetic.json')
P0S = (0.40, 0.50, 0.60, 0.45, 0.55)


def quiet(argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = run(argv)
    return rc, out.getvalue() + err.getvalue()


def _event(n, tip_ms):
    """Game n's event and its home and away markets (x1_backtest.TEAMS' codes, Kalshi's AWAYHOME)."""
    h, aw = x1.TEAMS[n % 8], x1.TEAMS[(n + 3) % 8]
    d = datetime.fromtimestamp(tip_ms / 1000 - 5 * 3600, tz=timezone.utc)
    ev = 'KXNBAGAME-%s%s%02d%s%s' % (d.strftime('%y'), d.strftime('%b').upper(), d.day, aw[0], h[0])
    return ev, ev + '-' + h[0], ev + '-' + aw[0], h, aw


def synth_panel(games=160, gid0=401900000, day0='2026-11-04T00:00Z', fav_runs=False):
    """nba_panel.py's panel for synthetic games, four a night three hours apart (gid0 + n ESPN's id).
    Each game: the home team's p0 from P0S; the run team (home in even games, or with fav_runs the
    pregame favourite, home at 0.50) scores 12 straight; it wins but in every sixth game (n % 6 ==
    5). Four rows, history rows built at lag 41.5 s:
      r0  the tip + 60 s, Q1 12:00, 0-0: both books at p0 +- 1c, the same a minute on
      r1  + 700 s, Q1 2:00, 12-0 for the run team: the books still at p0 +- 1c (stale), 5c toward
          the run team a minute on
      r1b + 720 s, Q1 1:40, the same score (its first sighting r1's): stale, and 5c on a minute on
      r3  + 1500 s, Q2 6:00, 18-10 for the run team: priced at its 5 in 6, 0.82/0.84 (the other
          market 0.16/0.18), the same a minute on
    The model (tests/deskd/fixtures/nba_synthetic.json) prices the run team at 0.81 to 0.92 at r1:
    it takes there; at r3 it is within 10c of the touch both ways, and the tip within 10c too."""
    tip0, rows = em.iso_ms(day0), []
    for n in range(games):
        tip = tip0 + (n // 4) * 86400000 + (n % 4) * 3 * 3600000
        ev, home, away, _, _ = _event(n, tip)
        p0 = P0S[n % len(P0S)]
        run_home, loses = (p0 >= 0.5 if fav_runs else n % 2 == 0), n % 6 == 5
        won = run_home != loses
        c = int(round(p0 * 100))
        s = 1 if run_home else -1

        def books(shift):   # the home and away touches at p0 +- 1c, moved shift cents toward home
            return ((c - 1 + shift) * 100, (c + 1 + shift) * 100), ((99 - c - shift) * 100, (101 - c - shift) * 100)
        stale, moved = books(0), books(5 * s)
        priced = ((8200, 8400), (1600, 1800)) if run_home else ((1600, 1800), (8200, 8400))
        lead = (12, 0) if run_home else (0, 12)
        later = (18, 10) if run_home else (10, 18)
        for dt, period, clock, (hs, as_), seen_dt, now, nxt in (
                (60, 1, '12:00', (0, 0), 60, stale, stale), (700, 1, '2:00', lead, 700, stale, moved),
                (720, 1, '1:40', lead, 700, stale, moved), (1500, 2, '6:00', later, 1500, priced, priced)):
            t_left, ot = tb.t_left(nbp.LEAGUE, period, em.clock_seconds(clock))
            bid, ask = nbp.effective(*now)
            row = {'game_id': str(gid0 + n), 'season': nbp.season_of(tip), 't_left_s': nbp.number(t_left),
                   'ot': '1' if ot else '0', 'margin': str(hs - as_), 'possession': '0', 'p0': '%.5f' % p0,
                   'yes_won': '1' if won else '0', 'kalshi_mid': '%.5f' % ((bid + ask) / 2 / ONE),
                   'kalshi_bid': nbp.dollars(bid), 'kalshi_ask': nbp.dollars(ask), 'espn_wp': '',
                   'visible_ms': str(tip + dt * 1000), 'source': 'history', 'lag_s': '41.5', 'event': ev,
                   'home_ticker': home, 'away_ticker': away, 'start_ms': str(tip), 'period': str(period),
                   'clock': clock, 'home_score': str(hs), 'away_score': str(as_), 'seen_ms': str(tip + seen_dt * 1000)}
            for side, q, qn in (('home', now[0], nxt[0]), ('away', now[1], nxt[1])):
                row[side + '_bid'], row[side + '_ask'] = nbp.dollars(q[0]), nbp.dollars(q[1])
                row[side + '_bid_next'], row[side + '_ask_next'] = nbp.dollars(qn[0]), nbp.dollars(qn[1])
            rows.append(row)
    return rows


def ops_db(path, rows):
    """An ops.sqlite with deskd's signals table (SIGNALS_DDL) and these rows, bound as store.cpp
    binds them (decimal strings in REAL columns as numbers, the whole row as json)."""
    con = sqlite3.connect(path)
    con.execute(SIGNALS_DDL)
    for r in rows:
        con.execute('INSERT INTO signals(id, lane, mode, shadow, ts_ms, tk, ev, side, px, qty, fair, edge, action, '
                    'why, mo10, mo60, json) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)',
                    (r['id'], r['lane'], r['mode'], 1 if r['shadow'] else 0, r['ts_ms'], r['tk'], r['ev'], r['side'],
                     float(r['px']), r['qty'], None, float(r['edge']), r['action'], r.get('why', ''), None,
                     None if r['mo60'] is None else float(r['mo60']), json.dumps(r)))
    con.commit()
    con.close()


def synth_signals(start_ms, days=15, per_day=4, mo60='0.0500'):
    """N1's shadow signals, per_day an ET night from start_ms: BUY 4 at 0.50 of game k's home market
    (game k = night x per_day + i), edge 0.12, fee 0.07, mo60 a contract; and the rows G3 must not
    count: a PULL, another lane's, one under the threshold and a live TAKE."""
    rows = []
    for d in range(days):
        for i in range(per_day):
            k = d * per_day + i
            ev = 'KXNBAGAME-SIG%03d' % k
            rows.append({'id': 'N1-%s-A' % ev, 'lane': 'N1', 'mode': 'MODEL', 'shadow': True,
                         'ts_ms': start_ms + d * 86400000 + i * 600000, 'tk': ev + '-HHH', 'ev': ev, 'side': 'BUY',
                         'px': '0.5000', 'qty': 4.0, 'edge': '0.1200', 'action': 'SHADOW', 'mo60': mo60,
                         'fee': '0.0700', 'dir': 'A'})
    t, ev = start_ms + 3600000, 'KXNBAGAME-SIG000'
    base = dict(rows[0], ts_ms=t)
    rows += [dict(base, id='pull', action='PULL'), dict(base, id='x1', lane='X1', mode='ESPN_FADE'),
             dict(base, id='thin', edge='0.0900'), dict(base, id='live', action='TAKE', shadow=False, ev=ev)]
    return rows


def synth_results(root, games):
    """kalshi_markets rows settling the signals' markets: game k's home market yes but in every
    third game (k % 3 == 2)."""
    sp = lj.Spool(root)
    for k in range(games):
        ev = 'KXNBAGAME-SIG%03d' % k
        sp.write('kalshi_markets', {'recv_us': 1_800_000_000_000_000 + k, 'series': 'KXNBAGAME', 'ticker': ev + '-HHH',
                                    'status': 'finalized', 'result': 'no' if k % 3 == 2 else 'yes',
                                    'raw': json.dumps({'event_ticker': ev, 'yes_sub_title': 'H'})})


# N2's synthetic games (synth_pregame): the favourite's YES (bid, ask) and the contracts at each, by
# minutes before the tip; the other market mirrors it. Every game opens at 0.86/0.88 70 min out.
#   full     0.91/0.92 x10 from 30 min: bought 5 at 0.92
#   band95   0.94/0.95 from 30 min: 0.95 is out of [0.90, 0.95)
#   thin     0.91/0.92 but 3 contracts on the ask (2 on the other's bid): bought 3, partly
#   late     0.91/0.92 from 1 min: past the window's close (2 min)
#   early    0.91/0.92 from 61 min, 0.95/0.96 from 60.5 min: in band only before the window
#   open     0.91/0.92 from 61 min and still at 60: bought at the window's open
#   outage   as full, but ESPN down from 65 to 1 min before: the score is too old
PREGAME = (('full', ((70, 8600, 8800, 10), (30, 9100, 9200, 10))),
           ('band95', ((70, 8600, 8800, 10), (30, 9400, 9500, 10))),
           ('thin', ((70, 8600, 8800, 10), (30, 9100, 9200, 3))),
           ('late', ((70, 8600, 8800, 10), (1, 9100, 9200, 10))),
           ('early', ((70, 8600, 8800, 10), (61, 9100, 9200, 10), (60.5, 9500, 9600, 10))),
           ('open', ((70, 8600, 8800, 10), (61, 9100, 9200, 10))),
           ('outage', ((70, 8600, 8800, 10), (30, 9100, 9200, 10))))


def synth_pregame(root, kinds, day0='2026-11-04T00:00Z'):
    """A recorder directory (lip_join.Spool) of KXNBAGAME games, one a night, each by its kind
    (PREGAME): both markets' socket books, ESPN's scoreboard (pre from 3 h out, in from the tip)
    and the results; the favourite is the home team in even games and wins but in game 3. Returns
    [(event, favourite's ticker)]."""
    sp, out, frames = lj.Spool(root), [], []
    us = 1000
    for n, kind in enumerate(kinds):
        tip = em.iso_ms(day0) + n * 86400000
        ev, home, away, h, aw = _event(n, tip)
        fav, dog = (home, away) if n % 2 == 0 else (away, home)
        fav_won = n != 3
        start = datetime.fromtimestamp(tip / 1000, tz=timezone.utc).strftime('%Y-%m-%dT%H:%MZ')
        for tk, name in ((home, h[1]), (away, aw[1])):
            won = (tk == fav) == fav_won
            sp.write('kalshi_markets', {'recv_us': (tip - 4 * 3600000) * us, 'series': 'KXNBAGAME', 'ticker': tk,
                                        'status': 'finalized', 'result': 'yes' if won else 'no',
                                        'raw': json.dumps({'event_ticker': ev, 'yes_sub_title': name})})
        for mins, bid, ask, size in dict(PREGAME)[kind]:
            t = int(tip - mins * 60000)
            frames.append((t, fav, [(bid / ONE, 10)], [((ONE - ask) / ONE, size)]))
            # the other market at the same instant: YES bid 1 - ask (the favourite's thin side), ask 1 - bid
            frames.append((t, dog, [((ONE - ask) / ONE, 2 if kind == 'thin' else size)], [(bid / ONE, 10)]))
        for t, state in ((tip - 3 * 3600000, 'pre'), (tip + 60000, 'in')):
            raw = {'competitors': [
                {'homeAway': 'home', 'team': {'id': '1', 'abbreviation': h[0], 'displayName': h[2]}},
                {'homeAway': 'away', 'team': {'id': '2', 'abbreviation': aw[0], 'displayName': aw[2]}}]}
            sp.write('espn_scores', {'recv_us': t * us, 'prev_us': None, 'league': 'basketball/nba',
                                     'game_id': str(401950000 + n), 'start_time': start, 'state': state,
                                     'status': None, 'period': '0' if state == 'pre' else '1',
                                     'clock': '0:00' if state == 'pre' else '12:00', 'home': h[0], 'away': aw[0],
                                     'home_score': 0, 'away_score': 0, 'detail': None, 'raw': json.dumps(raw)})
        if kind == 'outage':
            sp.write('gaps', {'recv_us': (tip - 60000) * us, 'source': 'espn', 'kind': 'recovered', 'detail': 'test',
                              'from_us': (tip - 65 * 60000) * us, 'to_us': (tip - 60000) * us})
        out.append((ev, fav))
    for seq, (t, tk, yes, no) in enumerate(sorted(frames), start=1):   # the socket's frames in the order received
        sp.snapshot(t * us, 1, 1, seq, tk, yes, no)
    return out


def self_test(write_fixture=False):
    fails = []

    def check(name, ok, detail=''):
        print('%s %s%s' % ('OK  ' if ok else 'FAIL', name, (': ' + str(detail)) if detail else ''))
        if not ok:
            fails.append(name)

    # ---- the rule is N1's and N2's: n1_golden.json through take_rule, its params take_params'
    gold_path = os.path.join(ROOT, 'tests', 'deskd', 'fixtures', 'n1_golden.json')
    cases, bad, bad_params = tr.run_cases(gold_path)
    check('n1_golden.json: every MODEL and FAV_PREGAME case, through take_rule', cases >= 40 and not bad
          and not bad_params, '%d cases, %d differ' % (cases, len(bad)))
    with open(gold_path, encoding='utf-8') as fh:
        gold = json.load(fh)['params']
    check("n1_params(0.10) is n1_golden's MODEL params, FAV n1_golden's FAV_PREGAME",
          n1_params(0.10) == tr.params_of(gold['MODEL'], 'MODEL')
          and FAV == tr.params_of(gold['FAV_PREGAME'], 'FAV_PREGAME') and n1_params(0.12)['edge'] == 1200)

    # ---- the bootstrap
    check('upper80: games of one mean difference give it in every resample',
          abs(upper80([(-0.03, 3), (-0.06, 6), (-0.01, 1)], 500, 3) + 0.01) < 1e-15)
    # two one-row games, -0.02 and +0.01: a resample's mean is -0.02 (1 in 4), -0.005 (2 in 4) or
    # +0.01 (1 in 4); 80% at or under first comes at +0.01
    check('upper80: two games, -0.02 and +0.01: the bound is +0.01', upper80([(-0.02, 1), (0.01, 1)]) == 0.01)
    check('upper80: the seed decides it, and the same seed gives the same bound',
          upper80([(-0.02, 1), (0.01, 1), (0.003, 2), (-0.004, 1)], 300, 7)
          == upper80([(-0.02, 1), (0.01, 1), (0.003, 2), (-0.004, 1)], 300, 7))

    # ---- store.cpp's signals table is SIGNALS_DDL
    src = os.path.join(ROOT, 'src', 'deskd', 'store', 'store.cpp')
    if os.path.exists(src):
        with open(src, encoding='utf-8') as fh:
            m = re.search(r'CREATE TABLE IF NOT EXISTS signals\(.*?\);', fh.read(), re.S)
        check("SIGNALS_DDL is store.cpp's signals table", m is not None
              and ' '.join(m.group(0).rstrip(';').split()) == ' '.join(SIGNALS_DDL.split()))
    else:
        print('     src/deskd/store/store.cpp: not present, the signals schema check skipped')

    model, sha = load_model(MODEL_FIXTURE)
    with tempfile.TemporaryDirectory() as d:
        # ---- n1: 160 games of 2026-27 (not among the model's seasons: the shipped model prices them)
        # and 40 of 2025-26 (one of them: a refit prices them)
        panel = os.path.join(d, 'nba_synthetic_panel.csv')
        nbp.write_panel(panel, synth_panel(160) + synth_panel(40, 401800000, '2026-02-04T00:00Z'), True)
        res = n1(panel, MODEL_FIXTURE, '2026-27')
        g, r2 = res['g1'], res['g2']
        check("n1 prices 2026-27 with the shipped model (not among its seasons)", res['fair'].startswith('the shipped')
              and res['model_sha256'] == sha, res['fair'])
        check('G1: 640 rows of 160 games; the stale r1 rows make the mid worse: a pass',
              (g['games'], g['rows']) == (160, 640) and g['diff'] < -0.03 and g['upper80'] < 0 and g['passed'],
              (g['brier_model'], g['brier_mid'], g['upper80']))
        rows, _ = read_panel(panel)
        held = [r for r in rows if r['season'] == '2026-27']
        again = g1(held, lambda r: home_p(model, r))
        check('G1 is deterministic (seeded): the same panel, the same bound', again['upper80'] == g['upper80'])
        worse = g1(held, lambda r: 1 - home_p(model, r))
        check("G1 fails a model worse than the mid, saying so", not worse['passed']
              and worse['why'].startswith("G1: Brier ") and "is not below the mid's" in worse['why'], worse['why'])
        # 150 one-row games the home team won, the mid 0.50 (0.25 each): the model 0.9 in 76 (0.01) and
        # 0.3 in 74 (0.49): Brier 0.2468, below the mid's by 0.0032 with an sd of 0.24 a game
        close = g1([{'game_id': 'g%03d' % i, 'kalshi_mid': 0.5, 'yes_won': 1.0, 'p': 0.9 if i < 76 else 0.3}
                    for i in range(150)], lambda r: r['p'], 500)
        check("G1 fails a model better than the mid by less than its bootstrap's 80% upper bound",
              not close['passed'] and close['why'].startswith("G1: Brier 0.2468 is below the mid's 0.2500, but the "
                                                              "difference's 80% upper bound is +0.0"), close['why'])
        # game 0 worked by hand: BOS at home (A), p0 0.40, 12-0 at Q1 2:00 (2280 s left): the model's
        # fair 0.8073; A at 0.39/0.41: $2 buys 4 (the max) at 0.41, fee 0.07, edge
        # 0.8073 - 0.41 - 0.0175 = 0.3798 (selling DEN at 0.59 is the same price: the first route
        # wins the tie). A minute on BOS asks 0.46: the entry; fee 0.07; BOS wins: 4 - 1.84 - 0.07 = 2.09.
        # At the touch it saw: 4 - 1.64 - 0.07 = 2.29, and its mid a minute on 0.45: (0.45 - 0.41) x 4 - 0.07
        ev0 = _event(0, em.iso_ms('2026-11-04T00:00Z'))[0]
        t0 = [t for t in r2['trades'] if t['game'] == ev0]
        game0 = PanelGame([r for r in held if r['event'] == ev0], 1.0)
        s0, at = (score_take(t0[0], game0), score_take(t0[0], game0, False)) if t0 else ({}, {})
        took = [(t['side'], t['ticker'], t['price'], t['count'], t['fee'], t['edge']) for t in t0]
        check('a trade worked by hand: BUY BOS 0.41 x4 at r1 (edge 0.3798), filled at 0.46: settles +$2.09; at the '
              'touch +$2.29, 60 s markout +$0.09', took == [('BUY', ev0 + '-BOS', 4100, 400, 700, 3798)]
              and abs(s0['settle'] - 2.09) < 1e-9 and s0['mo60'] is None and abs(at['settle'] - 2.29) < 1e-9
              and abs(at['mo60'] - 0.09) < 1e-9, (t0, s0, at))
        # the same team bought by selling DEN at its bid 0.59: a minute on DEN bids 0.54, BOS at 0.46 again
        sell = dict(t0[0], side='SELL', ticker=ev0 + '-DEN', price=5900) if t0 else {}
        s1 = score_take(sell, game0) if t0 else {}
        check('selling the other market: the worse of its bid and its bid a minute on (0.59, 0.54), BOS at 0.46: '
              '+$2.09', s1 and s1['entry'] == 4600 and abs(s1['settle'] - 2.09) < 1e-9, s1)
        st = r2['rule']['settle']
        check('G2: one take a game, at r1 (r1b is FIRED, r3 within 10c), every game; positive with its lower bound',
              r2['rule']['trades'] == 160 and st['games'] == 160 and st['mean'] > 0.5 and st['lower80'] > 0
              and all(t['t'] - game0.rows[0]['start_ms'] == 700000 for t in t0), (st['n'], st['mean'], st['lower80']))
        pl = r2['placebos']
        check('G2: every placebo loses (no score buys the trailer at r3, flipped margin the trailer at r1 and r3, '
              'the favourite the price favourite)', all(pl[k]['trades'] > 50 and pl[k]['settle']['mean'] < 0
                                                        for k in PLACEBOS) and r2['passed'],
              {k: (pl[k]['trades'], pl[k]['settle']['mean']) for k in PLACEBOS})
        check('verdict: PASS at stage shadow', res['verdict'] == {'pass': True, 'why': '', 'stage': 'shadow'},
              res['verdict'])
        fav = os.path.join(d, 'fav.csv')
        nbp.write_panel(fav, synth_panel(160, fav_runs=True), True)
        fr = n1(fav, MODEL_FIXTURE, '2026-27')
        check('a placebo that pays fails G2 though N1 pays too (the run team the price favourite: buying it pays)',
              fr['g2']['rule']['settle']['mean'] > 0 and fr['g2']['placebos']['favourite']['settle']['mean'] > 0
              and fr['verdict']['why'].startswith('G2: placebo favourite makes $0.'), fr['verdict']['why'])
        high = n1(panel, MODEL_FIXTURE, '2026-27', thr=0.50)
        check('a threshold no row reaches fails G2 for its trades', not high['verdict']['pass']
              and high['verdict']['why'] == 'G2: 0 trades, under 50', high['verdict']['why'])
        # the refit: 2025-26 is the model's own; its other season refits, as train_bball scores it
        rf = n1(panel, MODEL_FIXTURE)
        tres = tb.train(rows, model['features'], '2025-26', True, 'nba')
        check("--held-out defaults to the model's (2025-26), which it saw: refit on 2026-27, train_bball's own "
              'held-out Brier', rf['held_out'] == '2025-26' and rf['fair'].startswith('refit on 2026-27')
              and abs(rf['g1']['brier_model'] - tres['brier']['model']) < 1e-12
              and abs(rf['g1']['brier_mid'] - tres['brier']['kalshi_mid']) < 1e-12,
              (rf['fair'], rf['g1']['brier_model'], tres['brier']['model']))
        check('40 held-out games is under 150: a G1 FAIL', not rf['verdict']['pass']
              and rf['g1']['why'] == 'G1: 40 held-out games with a Kalshi mid, under 150', rf['g1']['why'])

        # the CLI and the gate file
        gate = os.path.join(d, 'gates', 'N1.json')
        base = ['n1', '--panel', panel, '--model', MODEL_FIXTURE, '--held-out', '2026-27']
        check('n1 needs --panel and --model, and files that exist', quiet(['n1'])[0] == 2
              and quiet(['n1', '--panel', panel])[0] == 2 and quiet([])[0] == 2
              and quiet(['n1', '--panel', panel, '--model', panel + 'x'])[0] == 2)
        rc, text = quiet(base + ['--lag-s', '45'])
        check('a panel built with less lag than the measured is refused', rc == 2 and 'rebuild it with' in text
              and '--lag-s 41.5, under the measured 45' in text, text.strip())
        check('--write writes N1.json', quiet(base + ['--write', gate])[0] == 0 and os.path.exists(gate))
        with open(gate, encoding='utf-8') as fh:
            gj = json.load(fh)
        check("it loads as deskd's load_lane_gate(path, 'N1', the model's sha256, {thr}): stage shadow",
              x1.gate_refusal(gj, 'N1', sha, N1_PARAMS) is None
              and x1.gate_why(gj, 'N1', sha, N1_PARAMS).startswith('stage shadow') and gj['params'] == {'thr': 0.1}
              and gj['model_sha256'] == sha, gj['params'])
        check('another model (a retrained one) does not open it, nor no model',
              x1.gate_refusal(gj, 'N1', 'a' * 64, N1_PARAMS).startswith('measured another model')
              and x1.gate_refusal(gj, 'N1', '', N1_PARAMS).endswith('and no model is loaded'))
        check('an existing file is refused without --force, replaced with it', quiet(base + ['--write', gate])[0] == 2
              and quiet(base + ['--write', gate, '--force'])[0] == 0)
        rc, _ = quiet(base + ['--thr', '0.5', '--write', gate, '--force'])
        with open(gate, encoding='utf-8') as fh:
            fj = json.load(fh)
        check('a FAIL is written with its why, and deskd refuses it with that why', rc == 0 and fj['verdict'] == 'FAIL'
              and x1.gate_refusal(fj, 'N1', sha, N1_PARAMS) == 'verdict FAIL: G2: 0 trades, under 50')
        rc, text = quiet(base + ['--json'])
        check('--json prints the result without the takes', rc == 0 and json.loads(text)['g1']['games'] == 160
              and 'trades' not in json.loads(text)['g2'])
        # the committed fixture is what n1 writes for this panel
        fx = n1_gate(res, FIXTURE_COMPUTED)
        fx['tool'] = SYNTH_TOOL % 'a planted stale-quote effect on nba_synthetic.json'
        if write_fixture:
            x1.write_gate(FIXTURE_N1, fx, force=True)
            print('     wrote %s' % FIXTURE_N1)
        try:
            with open(FIXTURE_N1, encoding='utf-8') as fh:
                committed = json.load(fh)
        except (OSError, ValueError):
            committed = None
        check('tests/deskd/fixtures/n1_gate.json is what n1 writes for the synthetic panel, and loads as N1 with '
              "nba_synthetic.json's sha256", committed == json.loads(json.dumps(fx))
              and x1.gate_refusal(committed, 'N1', sha, N1_PARAMS) is None)

        # ---- shadow: G3 on deskd's signals table
        gate_t = em.iso_ms(FIXTURE_COMPUTED)
        shadow_gate_path = os.path.join(d, 'gates', 'N1s.json')
        x1.write_gate(shadow_gate_path, fx)
        data = os.path.join(d, 'rec')
        synth_results(data, 60)
        start = gate_t + 23 * 3600000   # 23:00Z, the evening of 2026-10-22 ET
        db = os.path.join(d, 'ops.sqlite')
        sig = synth_signals(start)
        ops_db(db, sig + [dict(sig[0], id='early', ts_ms=gate_t - 1000)])
        sres = shadow(db, data, shadow_gate_path, MODEL_FIXTURE)
        g3r = sres['g3']
        # each signal: BUY 4 at 0.50, fee 0.07: markout 0.05 x 4 - 0.07 = +0.13; settled yes 4 - 2 - 0.07 =
        # +1.93, no -2.07, one game in three no: (40 x 1.93 - 20 x 2.07) / 60 = 0.596667
        check('G3: 60 signals on 15 days; a PULL, X1, a thin edge and a live TAKE are not counted, nor one before '
              'the gate', g3r['counts'] == {'signals': 60, 'not_a_take': 1, 'under_thr': 1, 'before_the_gate': 1}
              and g3r['days'] == 15, (g3r['counts'], g3r['days']))
        check('G3 worked by hand: net 60 s markout +$0.13 a signal, settlement (40 x 1.93 - 20 x 2.07) / 60',
              abs(g3r['mo60']['mean'] - 0.13) < 1e-9
              and abs(g3r['settle']['mean'] - (40 * 1.93 - 20 * 2.07) / 60) < 1e-9 and g3r['passed']
              and sres['stage'] == 'live', (g3r['mo60']['mean'], g3r['settle']['mean']))
        # a SELL of 2 at 0.60, fee 0.04, gross markout 0.02 a contract, settled no: 0.02 x 2 - 0.04 = 0 and
        # (0.60 - 0) x 2 - 0.04 = +1.16; with its fee missing, the cent fee at 0.40 for 2 (0.0336: 0.04)
        one = {'action': 'SHADOW', 'ts_ms': start, 'tk': 'T', 'ev': 'E', 'side': 'SELL', 'px': 0.6, 'qty': 2.0,
               'edge': 0.12, 'mo60': 0.02, 'fee': 0.04}
        gs = [g3([x], {'T': 'no'}, 0.10, 0) for x in (one, dict(one, fee=None))]
        check('G3 on a SELL worked by hand: net markout 0, settlement +$1.16; without its fee, the cent fee',
              all(abs(x['mo60']['mean']) < 1e-12 and abs(x['settle']['mean'] - 1.16) < 1e-12 for x in gs),
              [(x['mo60']['mean'], x['settle']['mean']) for x in gs])
        live = os.path.join(d, 'gates', 'N1live.json')
        rc, text = quiet(['shadow', '--ops', db, '--data', data, '--gate', shadow_gate_path, '--model', MODEL_FIXTURE,
                          '--write', live])
        with open(live, encoding='utf-8') as fh:
            lj_ = json.load(fh)
        check("shadow writes N1.json at stage live, which opens the lane for the same model, G1 and G2's evidence "
              'kept', rc == 0 and x1.gate_why(lj_, 'N1', sha, N1_PARAMS) == '' and lj_['params'] == {'thr': 0.1}
              and 'g1' in lj_['evidence'] and lj_['evidence']['g3']['signals'] == 60, rc)
        rc, text = quiet(['shadow', '--ops', db, '--data', data, '--gate', live, '--model', MODEL_FIXTURE])
        check('shadow refuses a gate already live (its engine no longer shadows: a re-run would close it)', rc == 2
              and 'already at stage live' in text, text.strip())
        ops_db(os.path.join(d, 'short.sqlite'), synth_signals(start, days=13, per_day=5))
        sh = shadow(os.path.join(d, 'short.sqlite'), data, shadow_gate_path, MODEL_FIXTURE)
        check('65 signals on 13 days stays at stage shadow, saying why', sh['stage'] == 'shadow'
              and sh['g3']['why'] == 'G3: signals on 13 days, under 14', sh['g3']['why'])
        # that shadow gate, written weeks later, still counts from n1's gate: the full table goes live
        later = os.path.join(d, 'gates', 'N1later.json')
        x1.write_gate(later, shadow_gate(sh, '2026-11-20T00:00:00Z'))
        sl = shadow(db, data, later, MODEL_FIXTURE)
        check("a shadow run's gate keeps the shadow stage's start: the next run counts from n1's gate",
              sl['since'] == FIXTURE_COMPUTED and sl['g3']['counts'].get('signals') == 60 and sl['stage'] == 'live',
              (sl['since'], sl['g3']['counts']))
        ops_db(os.path.join(d, 'neg.sqlite'), synth_signals(start, mo60='0.0100'))
        sh = shadow(os.path.join(d, 'neg.sqlite'), data, shadow_gate_path, MODEL_FIXTURE)
        check('a markout under the fee (0.01 x 4 - 0.07) stays at stage shadow', sh['stage'] == 'shadow'
              and sh['g3']['why'] == 'G3: the mean net 60 s markout is -$0.0300 a signal'
              and shadow_gate(sh, FIXTURE_COMPUTED)['stage'] == 'shadow', sh['g3']['why'])
        rc, text = quiet(['shadow', '--ops', db, '--data', data, '--gate', shadow_gate_path, '--model', panel])
        check('shadow refuses a model file that is not one', rc == 2, text.strip())
        x1.write_gate(os.path.join(d, 'gates', 'fail.json'), fj)
        rc, text = quiet(['shadow', '--ops', db, '--data', data, '--gate', os.path.join(d, 'gates', 'fail.json'),
                          '--model', MODEL_FIXTURE])
        check('shadow refuses a gate deskd would not load (a FAIL)', rc == 2 and 'verdict FAIL' in text, text.strip())

        # ---- n2: the recorder's pregame books
        rec2 = os.path.join(d, 'pregame')
        kinds = ['full'] * 9 + ['band95', 'thin', 'late', 'early', 'open', 'outage']
        favs = dict(synth_pregame(rec2, kinds))
        r = n2(rec2)
        evs = list(favs)
        by = defaultdict(list)
        for t in r['trades']:
            by[t['game']].append(t)
        got = {kinds[i]: [(t['side'], t['ticker'] == favs[evs[i]], t['price'], t['count']) for t in by[evs[i]]]
               for i in range(9, 15)}
        check('n2: 15 games recorded, each matched with its tip and both books', r['counts'] == {'events': 15,
              'recorded': 15} and r['games'] == 15, r['counts'])
        check('n2: the band, the size, the window and the outage decide (BUY the favourite 5 at 0.92; 0.95 out; 3 '
              "where 3 show; nothing past 2 min or before 60; at the window's open; nothing on a 60 s old score)",
              got == {'band95': [], 'thin': [('BUY', True, 9200, 300)], 'late': [], 'early': [],
                      'open': [('BUY', True, 9200, 500)], 'outage': []}
              and all([(t['side'], t['ticker'] == favs[evs[i]], t['price'], t['count']) for t in by[evs[i]]]
                      == [('BUY', True, 9200, 500)] for i in range(9)), got)
        # ten games bought 5 at 0.92 (fee 0.07 x 5 x 0.92 x 0.08 = 0.0258: 0.03), nine won: 5 - 4.60 - 0.03 =
        # +0.37, game 3 lost: -4.63; the thin game 3 at 0.92 (fee 0.02), won: 3 - 2.76 - 0.02 = +0.22
        s = r['stats']['settle']
        check('n2: 10 games bought to 5 contracts, 1 partly: PASS; the 11 takes settle (9 x 0.37 - 4.63 + 0.22) / 11',
              len(r['full']) == 10 and len(r['partial']) == 1 and r['passed'] and s['n'] == 11
              and abs(s['mean'] - (9 * 0.37 - 4.63 + 0.22) / 11) < 1e-9, (s['n'], s['mean']))
        rc, _ = quiet(['n2', '--data', rec2, '--min-games', '11'])
        check('n2 with --min-games 11 is a FAIL', rc == 0 and not n2(rec2, min_games=11)['passed'])
        n2path = os.path.join(d, 'gates', 'N2.json')
        rc, _ = quiet(['n2', '--data', rec2, '--write', n2path])
        with open(n2path, encoding='utf-8') as fh:
            n2j = json.load(fh)
        check("N2.json loads as deskd's load_lane_gate(path, 'N2', '', {count, fav_lo, fav_hi}) and opens the lane",
              rc == 0 and x1.gate_why(n2j, 'N2', '', N2_PARAMS) == '' and 'model_sha256' not in n2j
              and n2j['params'] == {'count': 5, 'fav_lo': 0.9, 'fav_hi': 0.95})
        check('n2 needs --data', quiet(['n2'])[0] == 2 and quiet(['n2', '--data', os.path.join(d, 'none')])[0] == 2)
        fx2 = n2_gate(dict(r, data='nba_pregame_synthetic'), FIXTURE_COMPUTED)
        fx2['tool'] = SYNTH_TOOL % 'recorded books made up for the test'
        if write_fixture:
            x1.write_gate(FIXTURE_N2, fx2, force=True)
            print('     wrote %s' % FIXTURE_N2)
        try:
            with open(FIXTURE_N2, encoding='utf-8') as fh:
                committed = json.load(fh)
        except (OSError, ValueError):
            committed = None
        check('tests/deskd/fixtures/n2_gate.json is what n2 writes for the synthetic recorder, and loads as N2',
              committed == json.loads(json.dumps(fx2)) and x1.gate_refusal(committed, 'N2', '', N2_PARAMS) is None)
    print('nba_gate self-test: %s' % ('ok' if not fails else 'FAILED (%d)' % len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(run(sys.argv[1:]))
