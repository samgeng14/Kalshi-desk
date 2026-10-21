#!/usr/bin/env python3
"""Train a basketball win-probability model, women's college or NBA (docs/wcbb_model.md, plan 5(f)).

The model's fair for a game market (KXNCAAWBGAME, KXNBAGAME) is the probability that the market's
YES team wins, from the game state and the pregame Kalshi price: a logistic regression on features
named in docs/wcbb_model.md. This tool fits it, scores it on a held-out season against the Kalshi
mid, ESPN's own number when the panel has it, Stern's model and Stern's model with overtime, refits
on every season and writes models/{wcbb,nba}_logit_vN.json, which deskd (strat/bball_model.h)
loads and refuses unless its 10 golden rows reproduce to 1e-6. tools/train_wcbb.py is this tool
with --league wcbb: the same flags, defaults and checks it always had.

Standard library only (no numpy, no pandas). No network. It reads only the --panel file it is given.

The leagues (--league; deskd's bball_league table):

  league  model       regulation          in overtime                                   tie width
  wcbb    wcbb_logit  4 x 600 s = 2400 s  t_left_s 0 (the overtime clock is not used)   1.99
  nba     nba_logit   4 x 720 s = 2880 s  t_left_s the overtime clock, 0 to 300 s       1.64

From ESPN's period and the period's clock (t_left below, deskd's bball_t_left): periods 1 to 4
with a clock of at most one period give (4 - period) x period length + clock; a later period is
overtime: NBA (clock, ot) with the clock at most 300 s, wcbb (0, ot) with the clock at most 600 s
(wcbb_t_left as it was); anything else (period 0, a clock past the period's length) is no state.
The tie width (one tie on the scoring lattice, docs/sports-totals.md) is the overtime baseline's:
NBA 1.64 as fitted there; wcbb takes the men's college value, 1.99, until women's games are fitted.

The panel (--panel FILE.csv): a header row, then one row per game state. Columns, in any order
(extra columns are ignored):

  game_id     any string; every row of one game shares it
  season      a label such as 2024-25; the held-out season is chosen by it. "Latest" is the
              greatest label in plain string order, so keep labels the same shape (2023-24, 2024-25)
  t_left_s    seconds left in regulation, 0 to the league's regulation (2400 wcbb, 2880 nba); in
              overtime 0 (wcbb) or the overtime clock, 0 to 300 (nba)
  ot          optional for wcbb (0 when absent), required for nba: 1 in overtime, else 0
              (true/false are read too). An NBA overtime row's t_left_s is the overtime clock, so
              without this column it could not say it was overtime
  margin      the YES team's score minus the opponent's
  possession  +1 the YES team has the ball, -1 the opponent, 0 unknown
  p0          the pregame YES probability (the Kalshi mid at the scheduled tip, spread 15c or
              less), as a probability; clamped here to [0.01, 0.99]
  yes_won     1 if the YES team won the game, else 0 (the same on every row of a game)
  kalshi_mid  optional: the Kalshi YES mid at that state, as a probability in [0, 1] (not cents);
              blank (or nan) where there was no two-sided book
  espn_wp     optional: ESPN's win probability for the YES team at that state, in [0, 1]; blank
              where ESPN had none

  game_id,season,t_left_s,margin,possession,p0,yes_won,kalshi_mid
  401713001,2024-25,2400,0,0,0.62,1,0.62
  401713001,2024-25,1815,-4,1,0.62,1,0.47
  401713001,2024-25,0,3,0,0.62,1,

Converting the Mac panel (wbpanel.pkl, a pandas DataFrame pickle; unpickling it needs pandas, so
this tool has no --from-pickle path). On the Mac, rename the panel's own columns to the names
above if they differ, then write the CSV (pandas writes NaN as an empty field, which is "missing"):

  python3 -c "import pandas as pd; df = pd.read_pickle('wbpanel.pkl'); \\
    cols = ['game_id','season','t_left_s','margin','possession','p0','yes_won']; \\
    cols += ['kalshi_mid'] if 'kalshi_mid' in df else []; df[cols].to_csv('wbpanel.csv', index=False)"

The NBA panel comes from tools/nba_panel.py, already in this shape (home team as YES).

What it does:

  1. Features (--features, the vocabulary of docs/wcbb_model.md), with R the league's regulation
     seconds, tau = max(t_left_s, 0) / R and logit(x) = ln(x/(1-x)): logit_p0, margin,
     margin_over_sd = margin/sqrt(tau+0.01), logit_p0_x_tau = logit(p0) * tau,
     logit_p0_x_sqrt_tau = logit(p0) * sqrt(tau) (the edge study's form), possession,
     possession_over_sd = possession/sqrt(tau+0.01), tau, and ot = 1 in overtime, else 0. In NBA
     overtime tau is the overtime clock over 2880; in wcbb overtime it is 0. The defaults: wcbb
     logit_p0,margin_over_sd,logit_p0_x_tau,possession (as it always was); nba
     logit_p0,margin_over_sd,logit_p0_x_tau,ot (no possession until deskd reads it live: ESPN's
     scoreboard does not say who has the ball).
  2. Standardization: each feature's mean and population standard deviation over the rows being
     fitted (scale 1 when the sd is 0: below 1e-12 * max(1, |mean|)).
  3. Fit: logistic regression by Newton's method (IRLS) with step halving, minimizing the mean log
     loss plus RIDGE/2 * sum(coef^2) on the standardized features, RIDGE = 1e-6, intercept not
     penalized. The ridge only keeps the Newton system well posed (near-separable late-game rows,
     collinear features, an ot column with no overtime rows); it is too small to move a real fit.
  4. Held out (--held-out SEASON, default the latest): fit on the other seasons and score that
     season with the Brier score (mean squared error of the probability) for the model, the Kalshi
     mid, ESPN, Stern's model and Stern's model with overtime. All are scored on the same rows: the
     held-out rows that have a kalshi_mid when any does, and of those the ones with an espn_wp when
     any does, else every held-out row (the model's Brier on every held-out row is printed too).
  5. Shipped: refit on every season; those coefficients, means and scales go in the file, with
     the held-out Brier scores of step 4. Both coefficient sets are printed.

Stern's model (Stern 1994, "A Brownian motion model for the progression of sports scores"): the
remaining margin is normal with mean mu * tau and variance sigma^2 * tau, so

  P(YES wins) = Phi((margin + mu * tau) / (sigma * sqrt(tau + 1e-9))),  mu = sigma * Phi^-1(p0),

which is p0 at the tip (tau = 1, margin 0). It has no possession term. sigma (points per game) is
fitted on the training seasons by one-parameter maximum likelihood: a log-spaced grid over
[2, 80] then a golden-section search, with P clamped to [1e-6, 1 - 1e-6] inside the likelihood only
so a single near-certain miss (an overtime row, tau = 0) cannot decide sigma.

Stern's model with overtime (stern_ot, a baseline only; ported from the old desk's
src/strategies/sports_live.cpp leadtime_prob and src/strategies/sports_totals.cpp
overtime_probability, pure functions here, nothing linked). Stern's P splits a tie at the horn
evenly; a tie goes to overtime, which the pregame favourite wins more often than not. With the
same sigma and mu, m = margin + mu * tau the expected final margin, f_ot = 300 / R an overtime's
share of a game and w the league's tie width:

  P = clamp(leadtime_prob(margin, mu, tau, sigma) + P(tie) * (q_ot - 1/2), 0, 1)
  P(tie) = overtime_probability(|m|, tau, sigma, w) = min(1, w * phi(m / sd) / sd), sd = sigma * sqrt(tau)
           (at tau = 0: 1 when level, else 0)
  q_ot = leadtime_prob(0, mu, f_ot, sigma) = Phi(Phi^-1(p0) * sqrt(f_ot)), an overtime from level
  leadtime_prob(lead, mu, f, s) = Phi((lead + mu * f) / (s * sqrt(f))); with f under 0.002 the lead
           decides (a tie 1/2); s <= 0 gives 1/2

which is Stern's P exactly at p0 = 1/2. In NBA overtime tau is the overtime clock's share and a
tie at its horn is one more overtime, so the same formula holds; in wcbb overtime (tau = 0) a
level game gets q_ot.

The file (--out, default models/{wcbb,nba}_logit_v1.json; refuses to overwrite without --force) is
exactly the format of docs/wcbb_model.md, floats at full precision, keys in this order: model
(wcbb_logit or nba_logit), league, regulation_s, version, trained, seasons, n_games, brier (model,
kalshi_mid, espn, stern, stern_ot, held_out; kalshi_mid and espn null when the panel has none),
features, mean, scale, coef, intercept, golden. Its 10 golden rows ({"in": {t_left_s, margin,
possession, p0, ot}, "p"}) sit at:

  wcbb  t_left_s 2400, 1800, 1200, 600 and 60, two each (the second 60 s row is an overtime row,
        t_left_s 0, when the model uses the ot feature)
  nba   t_left_s 2880, 2160, 1440 and 720, two each, then 60 s and an overtime row (3:00 left in
        overtime: t_left_s 180, ot true)

with margins, possessions and p0 varied, p from the same feature code the fit used:

  p = 1 / (1 + exp(-(intercept + sum_i coef[i] * (f_i - mean[i]) / scale[i])))

The report ends with the written file's sha256: the gate files (college_gate.json, N1.json) name a
model by it.

--write-synthetic FILE [--force] writes the self-test's synthetic NBA model (a seeded panel, every
new feature, an overtime golden row) marked "SYNTHETIC, for tests only": the fixture deskd's
bball tests can load. --self-test also checks the golden rows of tests/deskd/fixtures/
wcbb_synthetic.json and, when present, nba_synthetic.json with this file's feature code.

TODO: an Elo prior for games whose pregame book is thin (plan 5(f)); XGBoost only if it beats
this logistic on held-out Brier.

Usage:
  python3 tools/train_bball.py --league wcbb --panel wbpanel.csv
  python3 tools/train_bball.py --league nba --panel nbapanel.csv --held-out 2025-26 \\
      --features logit_p0,margin_over_sd,logit_p0_x_sqrt_tau,ot --out models/nba_logit_v2.json
  python3 tools/train_bball.py --write-synthetic FILE [--force]
  python3 tools/train_bball.py --self-test
"""
import argparse
import csv
import datetime
import hashlib
import json
import math
import os
import random
import re
import statistics
import sys
import tempfile
from collections import namedtuple

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

FEATURES = ("logit_p0", "margin", "margin_over_sd", "logit_p0_x_tau", "possession",
            "possession_over_sd", "tau", "logit_p0_x_sqrt_tau", "ot")
REQUIRED = ("game_id", "season", "t_left_s", "margin", "possession", "p0", "yes_won")
P0_MIN, P0_MAX = 0.01, 0.99
RIDGE = 1e-6
SD_ZERO = 1e-12
STERN_EPS = 1e-9          # inside sqrt(tau + eps), so tau = 0 does not divide by zero
STERN_LIK_CLAMP = 1e-6    # P clamped to [c, 1 - c] in the likelihood only
STERN_SIGMA_RANGE = (2.0, 80.0)
NORMAL = statistics.NormalDist()

League = namedtuple("League", "key model series regulation_s period_s periods ot_s ot_uses_clock tie_width "
                              "default_features golden golden_ot")
LEAGUES = {
    "wcbb": League(
        "wcbb", "wcbb_logit", "KXNCAAWBGAME", 2400.0, 600.0, 4, 300.0, False, 1.99,
        "logit_p0,margin_over_sd,logit_p0_x_tau,possession",
        # (t_left_s, ot, margin, possession, p0): the tip, both halves, the last minute; leads and deficits
        [(2400, False, 0, 0, 0.5), (2400, False, 0, 1, 0.8), (1800, False, 4, -1, 0.35), (1800, False, -7, 1, 0.65),
         (1200, False, 10, 1, 0.55), (1200, False, -3, 0, 0.1), (600, False, 2, -1, 0.9), (600, False, -12, 1, 0.7),
         (60, False, 1, 1, 0.45), (60, False, -2, -1, 0.97)],
        (0, True, -2, -1, 0.97)),   # the last row, when the model has the ot feature
    "nba": League(
        "nba", "nba_logit", "KXNBAGAME", 2880.0, 720.0, 4, 300.0, True, 1.64,
        "logit_p0,margin_over_sd,logit_p0_x_tau,ot",
        [(2880, False, 0, 0, 0.5), (2880, False, 0, 1, 0.8), (2160, False, 4, -1, 0.35), (2160, False, -7, 1, 0.65),
         (1440, False, 10, 1, 0.55), (1440, False, -3, 0, 0.1), (720, False, 2, -1, 0.9), (720, False, -12, 1, 0.7),
         (60, False, 1, 1, 0.45), (180, True, -2, -1, 0.97)],
        None),
}
LEAGUE_OF_MODEL = {lg.model: lg for lg in LEAGUES.values()}


class PanelError(ValueError):
    pass


def league_of(model):
    """The league of a model dict (the file's keys): its league key, else its model name (a file
    written before the league key existed is wcbb_logit)."""
    lg = LEAGUES.get(model.get("league")) or LEAGUE_OF_MODEL.get(model.get("model"))
    return lg or LEAGUES["wcbb"]


# ---------------------------------------------------------------- the clock and the feature code (one copy)

def t_left(lg, period, clock_s):
    """(t_left_s, ot) from ESPN's period and the period's clock in seconds; None when no state."""
    if period < 1 or clock_s < 0:
        return None
    if period <= lg.periods:
        return ((lg.periods - period) * lg.period_s + clock_s, False) if clock_s <= lg.period_s else None
    if lg.ot_uses_clock:
        return (float(clock_s), True) if clock_s <= lg.ot_s else None
    return (0.0, True) if clock_s <= lg.period_s else None


def clamp_p0(p0):
    return min(max(float(p0), P0_MIN), P0_MAX)


def logit(x):
    return math.log(x / (1.0 - x))


def tau_of(t_left_s, regulation_s=2400.0):
    return max(float(t_left_s), 0.0) / regulation_s


def feature_row(names, t_left_s, margin, possession, p0, ot=False, regulation_s=2400.0):
    """The values of the named features for one game state (docs/wcbb_model.md)."""
    tau = tau_of(t_left_s, regulation_s)
    lp = logit(clamp_p0(p0))
    sd = math.sqrt(tau + 0.01)
    margin = float(margin)
    possession = float(possession)
    v = {
        "logit_p0": lp,
        "margin": margin,
        "margin_over_sd": margin / sd,
        "logit_p0_x_tau": lp * tau,
        "possession": possession,
        "possession_over_sd": possession / sd,
        "tau": tau,
        "logit_p0_x_sqrt_tau": lp * math.sqrt(tau),
        "ot": 1.0 if ot else 0.0,
    }
    return [v[n] for n in names]


def spec_sigmoid(z):
    """1 / (1 + exp(-z)) as written in the spec (exp overflowing to inf gives 0, as in C++)."""
    try:
        return 1.0 / (1.0 + math.exp(-z))
    except OverflowError:
        return 0.0


def predict(model, t_left_s, margin, possession, p0, ot=False):
    """p from a model dict (the file's keys), exactly the spec's formula."""
    f = feature_row(model["features"], t_left_s, margin, possession, p0, ot, league_of(model).regulation_s)
    z = model["intercept"]
    for c, x, m, s in zip(model["coef"], f, model["mean"], model["scale"]):
        z += c * (x - m) / s
    return spec_sigmoid(z)


def parse_features(text):
    names = [n.strip() for n in text.split(",") if n.strip()]
    if not names:
        raise PanelError("--features: none given")
    bad = [n for n in names if n not in FEATURES]
    if bad:
        raise PanelError(f"--features: unknown {', '.join(bad)} (allowed: {', '.join(FEATURES)})")
    if len(set(names)) != len(names):
        raise PanelError("--features: a name is repeated")
    return names


# ---------------------------------------------------------------- the panel

def _num(text, col, line):
    try:
        v = float(text)
    except (TypeError, ValueError):
        raise PanelError(f"line {line}: {col} {text!r} is not a number")
    if math.isnan(v) or math.isinf(v):
        raise PanelError(f"line {line}: {col} is {text!r}")
    return v


MISSING = ("", "nan", "none", "null", "na")


def _prob(r, col, line):
    """An optional probability column: None when blank, else a number in [0, 1]."""
    if (r.get(col) or "").lower() in MISSING:
        return None
    v = _num(r[col], col, line)
    if not 0.0 <= v <= 1.0:
        raise PanelError(f"line {line}: {col} {v:g} is not in [0, 1] (cents? divide by 100)")
    return v


def read_panel(path, league="wcbb"):
    """Rows as dicts; checks every field and that each game has one season and one outcome."""
    lg = LEAGUES[league]
    rows = []
    games = {}
    with open(path, newline="", encoding="utf-8-sig") as fh:
        rd = csv.DictReader(fh)
        cols = [c.strip() for c in (rd.fieldnames or [])]
        missing = [c for c in REQUIRED if c not in cols]
        if missing:
            raise PanelError(f"{path}: no column {', '.join(missing)} (header: {', '.join(cols)})")
        has_ot = "ot" in cols
        if lg.ot_uses_clock and not has_ot:
            raise PanelError(f"{path}: no column ot ({league}'s overtime t_left_s is the overtime clock, "
                             "so each row must say whether it is overtime)")
        rd.fieldnames = cols
        has_mid = "kalshi_mid" in cols
        has_espn = "espn_wp" in cols
        for line, r in enumerate(rd, start=2):
            r = {k: (v.strip() if isinstance(v, str) else v) for k, v in r.items()}
            gid, season = r["game_id"], r["season"]
            if not gid or not season:
                raise PanelError(f"line {line}: empty game_id or season")
            ot = False
            if has_ot:
                text = (r["ot"] or "").lower()
                if text in ("1", "1.0", "true"):
                    ot = True
                elif text not in ("0", "0.0", "false"):
                    raise PanelError(f"line {line}: ot {r['ot']!r} is not 0 or 1")
            t = _num(r["t_left_s"], "t_left_s", line)
            if not ot and t > lg.regulation_s:
                raise PanelError(f"line {line}: t_left_s {t:g} is over {lg.regulation_s:g} (seconds left in regulation)")
            if ot and lg.ot_uses_clock and t > lg.ot_s:
                raise PanelError(f"line {line}: t_left_s {t:g} is over {lg.ot_s:g} in overtime (the overtime clock)")
            if ot and not lg.ot_uses_clock and t != 0:
                raise PanelError(f"line {line}: t_left_s {t:g} in overtime: {league} counts 0 left in overtime")
            margin = _num(r["margin"], "margin", line)
            poss = _num(r["possession"], "possession", line)
            if poss not in (-1.0, 0.0, 1.0):
                raise PanelError(f"line {line}: possession {r['possession']!r} is not -1, 0 or 1")
            p0 = _num(r["p0"], "p0", line)
            if not 0.0 < p0 < 1.0:
                raise PanelError(f"line {line}: p0 {p0:g} is not a probability in (0, 1)")
            y = _num(r["yes_won"], "yes_won", line)
            if y not in (0.0, 1.0):
                raise PanelError(f"line {line}: yes_won {r['yes_won']!r} is not 0 or 1")
            mid = _prob(r, "kalshi_mid", line) if has_mid else None
            espn = _prob(r, "espn_wp", line) if has_espn else None
            seen = games.setdefault(gid, (season, y, line))
            if seen[0] != season:
                raise PanelError(f"line {line}: game {gid} is in season {season} here, {seen[0]} on line {seen[2]}")
            if seen[1] != y:
                raise PanelError(f"line {line}: game {gid} has yes_won {y:g} here, {seen[1]:g} on line {seen[2]}")
            rows.append({"game_id": gid, "season": season, "t_left_s": t, "ot": ot, "margin": margin,
                         "possession": poss, "p0": clamp_p0(p0), "yes_won": y, "kalshi_mid": mid, "espn_wp": espn})
    if not rows:
        raise PanelError(f"{path}: no rows")
    return rows, has_mid


def panel_has(path, col):
    with open(path, newline="", encoding="utf-8-sig") as fh:
        return col in [c.strip() for c in (next(csv.reader(fh), None) or [])]


# ---------------------------------------------------------------- small linear algebra

def solve(a, b):
    """x with a x = b, Gaussian elimination with partial pivoting (a is small and dense)."""
    n = len(a)
    m = [list(a[i]) + [b[i]] for i in range(n)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(m[r][c]))
        if abs(m[p][c]) < 1e-300:
            raise ArithmeticError("singular system")
        m[c], m[p] = m[p], m[c]
        piv = m[c][c]
        for r in range(c + 1, n):
            f = m[r][c] / piv
            if f:
                mr, mc = m[r], m[c]
                for k in range(c, n + 1):
                    mr[k] -= f * mc[k]
    x = [0.0] * n
    for r in range(n - 1, -1, -1):
        s = m[r][n] - sum(m[r][k] * x[k] for k in range(r + 1, n))
        x[r] = s / m[r][r]
    return x


def inverse(a):
    n = len(a)
    cols = [solve(a, [1.0 if i == j else 0.0 for i in range(n)]) for j in range(n)]
    return [[cols[j][i] for j in range(n)] for i in range(n)]


# ---------------------------------------------------------------- the logistic fit

def standardize(columns):
    """Per column: (mean, population sd), the sd replaced by 1 when it is 0."""
    out = []
    for col in columns:
        n = len(col)
        mean = sum(col) / n
        sd = math.sqrt(sum((x - mean) ** 2 for x in col) / n)
        out.append((mean, sd if sd > SD_ZERO * max(1.0, abs(mean)) else 1.0))
    return out


def _sig(z):
    if z >= 0:
        return 1.0 / (1.0 + math.exp(-z))
    e = math.exp(z)
    return e / (1.0 + e)


def _softplus(z):
    return max(z, 0.0) + math.log1p(math.exp(-abs(z)))


def _objective(b, X, y, ridge):
    tot = 0.0
    for xi, yi in zip(X, y):
        z = 0.0
        for bj, xj in zip(b, xi):
            z += bj * xj
        tot += _softplus(z) - yi * z
    return tot / len(X) + 0.5 * ridge * sum(bj * bj for bj in b[1:])


def irls(X, y, ridge=RIDGE, max_iter=100, tol=1e-10):
    """Newton's method on the mean log loss + ridge/2 |b[1:]|^2. X rows start with the 1.0 of
    the intercept. Returns (b, information matrix n*H at b, iterations)."""
    n, d = len(X), len(X[0])
    b = [0.0] * d
    it = 0
    for it in range(1, max_iter + 1):
        g = [0.0] * d
        H = [[0.0] * d for _ in range(d)]
        for xi, yi in zip(X, y):
            z = 0.0
            for bj, xj in zip(b, xi):
                z += bj * xj
            p = _sig(z)
            r = p - yi
            w = p * (1.0 - p)
            for j in range(d):
                xj = xi[j]
                g[j] += r * xj
                wx = w * xj
                Hj = H[j]
                for k in range(j + 1):
                    Hj[k] += wx * xi[k]
        for j in range(d):
            g[j] /= n
            for k in range(j + 1):
                H[j][k] /= n
                H[k][j] = H[j][k]
        for j in range(1, d):
            g[j] += ridge * b[j]
            H[j][j] += ridge
        step = solve(H, g)
        dec = sum(gj * sj for gj, sj in zip(g, step))
        if dec <= 1e-24:
            break
        f0 = _objective(b, X, y, ridge)
        t = 1.0
        while True:
            bn = [bj - t * sj for bj, sj in zip(b, step)]
            if _objective(bn, X, y, ridge) <= f0 - 1e-4 * t * dec or t < 1e-10:
                break
            t *= 0.5
        b = bn
        if max(abs(t * s) for s in step) < tol:
            break
    # the information matrix at the solution, for standard errors
    info = [[0.0] * d for _ in range(d)]
    for xi in X:
        z = sum(bj * xj for bj, xj in zip(b, xi))
        p = _sig(z)
        w = p * (1.0 - p)
        for j in range(d):
            for k in range(j + 1):
                info[j][k] += w * xi[j] * xi[k]
    for j in range(d):
        for k in range(j + 1):
            info[k][j] = info[j][k]
        if j:
            info[j][j] += ridge * n
    return b, info, it


def fit(rows, names, regulation_s=2400.0):
    """The standardized logistic fit on rows: a model dict (the file's keys) plus diagnostics."""
    raw = [feature_row(names, r["t_left_s"], r["margin"], r["possession"], r["p0"], r.get("ot", False), regulation_s)
           for r in rows]
    ms = standardize([list(c) for c in zip(*raw)])
    X = [[1.0] + [(v - m) / s for v, (m, s) in zip(f, ms)] for f in raw]
    y = [r["yes_won"] for r in rows]
    b, info, it = irls(X, y)
    cov = inverse(info)
    mean = [m for m, _ in ms]
    scale = [s for _, s in ms]
    coef = b[1:]
    # the same model in raw feature units: z = raw_int + sum raw_coef * f
    raw_coef = [c / s for c, s in zip(coef, scale)]
    raw_int = b[0] - sum(c * m / s for c, m, s in zip(coef, mean, scale))
    a = [1.0] + [-m / s for m, s in zip(mean, scale)]
    raw_int_se = math.sqrt(max(0.0, sum(a[j] * cov[j][k] * a[k] for j in range(len(a)) for k in range(len(a)))))
    return {
        "features": list(names), "mean": mean, "scale": scale, "coef": coef, "intercept": b[0],
        "se": [math.sqrt(max(0.0, cov[j][j])) for j in range(1, len(b))],
        "raw_coef": raw_coef, "raw_coef_se": [math.sqrt(max(0.0, cov[j][j])) / scale[j - 1] for j in range(1, len(b))],
        "raw_intercept": raw_int, "raw_intercept_se": raw_int_se,
        "iterations": it, "n_rows": len(rows),
    }


# ---------------------------------------------------------------- Stern's model, and with overtime

def _phi(x):
    return 0.5 * math.erfc(-x / math.sqrt(2.0))


def stern_p(sigma, t_left_s, margin, p0, regulation_s=2400.0):
    tau = tau_of(t_left_s, regulation_s)
    mu = sigma * NORMAL.inv_cdf(clamp_p0(p0))
    return _phi((margin + mu * tau) / (sigma * math.sqrt(tau + STERN_EPS)))


def leadtime_prob(lead, mu, f, sigma):
    """sports_live.cpp SportsLive::leadtime_prob: P(win) with the remaining margin N(mu f, sigma^2 f)."""
    f = max(0.0, min(1.0, f))
    if sigma <= 0:
        return 0.5
    if f < 0.002:
        return 1.0 if lead > 0 else (0.0 if lead < 0 else 0.5)
    return _phi((lead + mu * f) / (sigma * math.sqrt(f)))


def overtime_probability(margin, fraction_left, margin_sd, tie_width):
    """sports_totals.cpp overtime_probability: the normal's density at the margin times one tie's
    width; NaN for an input out of range."""
    vals = (margin, fraction_left, margin_sd, tie_width)
    if (not all(math.isfinite(v) for v in vals) or margin < 0 or fraction_left < 0 or fraction_left > 1
            or margin_sd <= 0 or tie_width <= 0):
        return float("nan")
    if fraction_left <= 0:   # no regulation left: the margin on the board is the one that decides it
        return 1.0 if margin < 1e-9 else 0.0
    sd = margin_sd * math.sqrt(fraction_left)
    density = math.exp(-0.5 * (margin / sd) * (margin / sd)) / (sd * math.sqrt(2 * math.pi))
    return min(max(tie_width * density, 0.0), 1.0)


def stern_ot_p(sigma, lg, t_left_s, margin, p0):
    """Stern's P with a tie at the horn going to an overtime the favourite wins q_ot of the time."""
    tau = tau_of(t_left_s, lg.regulation_s)
    mu = sigma * NORMAL.inv_cdf(clamp_p0(p0))
    p_tie = overtime_probability(abs(margin + mu * tau), tau, sigma, lg.tie_width)
    q_ot = leadtime_prob(0.0, mu, lg.ot_s / lg.regulation_s, sigma)
    return min(max(leadtime_prob(margin, mu, tau, sigma) + p_tie * (q_ot - 0.5), 0.0), 1.0)


def fit_stern_sigma(rows, regulation_s=2400.0):
    """sigma maximizing the (clamped) Bernoulli likelihood of yes_won under Stern's model."""
    # x_i(sigma) = a_i / sigma + c_i, with a_i, c_i fixed per row
    pre = []
    for r in rows:
        tau = tau_of(r["t_left_s"], regulation_s)
        root = math.sqrt(tau + STERN_EPS)
        pre.append((r["margin"] / root, NORMAL.inv_cdf(r["p0"]) * tau / root, r["yes_won"]))
    lo_c, hi_c = STERN_LIK_CLAMP, 1.0 - STERN_LIK_CLAMP

    def nll(log_sigma):
        s = math.exp(log_sigma)
        tot = 0.0
        for a, c, yv in pre:
            p = min(max(_phi(a / s + c), lo_c), hi_c)
            tot -= math.log(p) if yv else math.log(1.0 - p)
        return tot

    lo, hi = math.log(STERN_SIGMA_RANGE[0]), math.log(STERN_SIGMA_RANGE[1])
    k = 40
    grid = [lo + (hi - lo) * i / (k - 1) for i in range(k)]
    vals = [nll(g) for g in grid]
    i = min(range(k), key=lambda j: vals[j])
    a, b = grid[max(i - 1, 0)], grid[min(i + 1, k - 1)]
    gr = (math.sqrt(5.0) - 1.0) / 2.0
    c, d = b - gr * (b - a), a + gr * (b - a)
    fc, fd = nll(c), nll(d)
    for _ in range(60):
        if fc <= fd:
            b, d, fd = d, c, fc
            c = b - gr * (b - a)
            fc = nll(c)
        else:
            a, c, fc = c, d, fd
            d = a + gr * (b - a)
            fd = nll(d)
    sigma = math.exp((a + b) / 2.0)
    at_edge = i in (0, k - 1)
    return sigma, at_edge


# ---------------------------------------------------------------- training, scoring, the file

def brier(ps, ys):
    return sum((p - yv) ** 2 for p, yv in zip(ps, ys)) / len(ys)


def train(rows, names, held_out=None, has_mid=False, league="wcbb"):
    lg = LEAGUES[league]
    seasons = sorted({r["season"] for r in rows})
    if held_out is None:
        held_out = seasons[-1]
    if held_out not in seasons:
        raise PanelError(f"--held-out {held_out}: not a season in the panel ({', '.join(seasons)})")
    if len(seasons) < 2:
        raise PanelError(f"one season ({seasons[0]}): a held-out score needs at least two")
    tr =[r for r in rows if r["season"] != held_out]
    ho = [r for r in rows if r["season"] == held_out]
    m_ho = fit(tr, names, lg.regulation_s)
    m_all = fit(rows, names, lg.regulation_s)
    m_ho["league"] = m_all["league"] = league
    sigma, sigma_edge = fit_stern_sigma(tr, lg.regulation_s)

    def p_model(r):
        return predict(m_ho, r["t_left_s"], r["margin"], r["possession"], r["p0"], r.get("ot", False))

    ys_all = [r["yes_won"] for r in ho]
    p_model_all = [p_model(r) for r in ho]
    scored = ho
    with_mid = [r for r in scored if r["kalshi_mid"] is not None] if has_mid else []
    if with_mid:
        scored = with_mid
    with_espn = [r for r in scored if r.get("espn_wp") is not None]
    if with_espn:
        scored = with_espn
    ys = [r["yes_won"] for r in scored]
    b_model = brier([p_model(r) for r in scored], ys)
    b_stern = brier([stern_p(sigma, r["t_left_s"], r["margin"], r["p0"], lg.regulation_s) for r in scored], ys)
    b_stern_ot = brier([stern_ot_p(sigma, lg, r["t_left_s"], r["margin"], r["p0"]) for r in scored], ys)
    b_mid = brier([r["kalshi_mid"] for r in scored], ys) if with_mid else None
    b_espn = brier([r["espn_wp"] for r in scored], ys) if with_espn else None
    b_p0 = brier([r["p0"] for r in scored], ys)
    return {
        "league": league, "seasons": seasons, "held_out": held_out, "n_games": len({r["game_id"] for r in rows}),
        "n_rows": len(rows), "n_train_rows": len(tr), "n_held_rows": len(ho), "n_scored_rows": len(scored),
        "n_ot_rows": sum(1 for r in rows if r.get("ot")),
        "held_fit": m_ho, "all_fit": m_all, "stern_sigma": sigma, "stern_sigma_at_edge": sigma_edge,
        "brier": {"model": b_model, "kalshi_mid": b_mid, "espn": b_espn, "stern": b_stern, "stern_ot": b_stern_ot,
                  "held_out": held_out},
        "brier_model_all_held": brier(p_model_all, ys_all), "brier_p0": b_p0,
    }


def golden_inputs(lg, features):
    rows = list(lg.golden)
    if lg.golden_ot is not None and "ot" in features:
        rows[-1] = lg.golden_ot
    return rows


def model_json(res, version, trained):
    lg = LEAGUES[res.get("league", "wcbb")]
    m = res["all_fit"]
    out = {
        "model": lg.model,
        "league": lg.key,
        "regulation_s": int(lg.regulation_s),
        "version": int(version),
        "trained": trained,
        "seasons": list(res["seasons"]),
        "n_games": int(res["n_games"]),
        "brier": dict(res["brier"]),
        "features": list(m["features"]),
        "mean": list(m["mean"]),
        "scale": list(m["scale"]),
        "coef": list(m["coef"]),
        "intercept": m["intercept"],
    }
    out["golden"] = [{"in": {"t_left_s": t, "margin": mg, "possession": ps, "p0": p0, "ot": ot},
                      "p": predict(out, t, mg, ps, p0, ot)} for t, ot, mg, ps, p0 in golden_inputs(lg, m["features"])]
    return out


def dumps_model(obj):
    """One key per line, arrays inline, one golden row per line; floats by repr (json's own)."""
    lines = []
    for k, v in obj.items():
        if k == "golden":
            inner = ",\n    ".join(json.dumps(g, allow_nan=False) for g in v)
            lines.append(f'  "golden": [\n    {inner}\n  ]')
        else:
            lines.append(f"  {json.dumps(k)}: {json.dumps(v, allow_nan=False)}")
    return "{\n" + ",\n".join(lines) + "\n}\n"


def write_model(path, obj, force=False):
    if os.path.exists(path) and not force:
        raise PanelError(f"{path} exists (a shipped version is never rewritten); --force or a new --out")
    d = os.path.dirname(os.path.abspath(path))
    os.makedirs(d, exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as fh:
        fh.write(dumps_model(obj))
    os.replace(tmp, path)


def sha256_file(path):
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def version_from_path(path):
    m = re.search(r"_v(\d+)\.json$", os.path.basename(path))
    return int(m.group(1)) if m else 1


def report_text(res, out_path):
    def f(v):
        return "-" if v is None else f"{v:.5f}"
    b = res["brier"]
    lines = [
        f"panel ({res['league']}): {res['n_rows']} rows ({res['n_ot_rows']} in overtime), {res['n_games']} games, "
        f"seasons {', '.join(res['seasons'])}",
        f"held out {res['held_out']}: fit on {res['n_train_rows']} rows of the other seasons, "
        f"scored on {res['n_scored_rows']} of its {res['n_held_rows']} rows"
        + (" (those with a kalshi_mid)" if b["kalshi_mid"] is not None and b["espn"] is None else "")
        + (" (those with an espn_wp)" if b["kalshi_mid"] is None and b["espn"] is not None else "")
        + (" (those with a kalshi_mid and an espn_wp)" if b["kalshi_mid"] is not None and b["espn"] is not None else ""),
        f"  Brier  model {f(b['model'])}  kalshi_mid {f(b['kalshi_mid'])}  espn {f(b['espn'])}  stern {f(b['stern'])}"
        f"  stern+ot {f(b['stern_ot'])}  p0 {f(res['brier_p0'])}   (model on every held-out row {f(res['brier_model_all_held'])})",
        f"  Stern sigma {res['stern_sigma']:.3f} points (training seasons)"
        + ("  WARNING: at the edge of [2, 80]" if res["stern_sigma_at_edge"] else ""),
    ]
    for title, m in (("held-out fit (other seasons)", res["held_fit"]), ("shipped fit (every season)", res["all_fit"])):
        lines.append(f"{title}: {m['n_rows']} rows, {m['iterations']} Newton steps")
        lines.append(f"  {'feature':<20}{'mean':>11}{'scale':>11}{'coef':>11}{'se':>9}{'raw coef':>12}{'raw se':>9}")
        for i, n in enumerate(m["features"]):
            lines.append(f"  {n:<20}{m['mean'][i]:>11.4f}{m['scale'][i]:>11.4f}{m['coef'][i]:>11.4f}"
                         f"{m['se'][i]:>9.4f}{m['raw_coef'][i]:>12.4f}{m['raw_coef_se'][i]:>9.4f}")
        lines.append(f"  {'intercept':<20}{'':>11}{'':>11}{m['intercept']:>11.4f}{'':>9}"
                     f"{m['raw_intercept']:>12.4f}{m['raw_intercept_se']:>9.4f}")
    if out_path:
        lines.append(f"wrote {out_path}")
        lines.append(f"  sha256 {sha256_file(out_path)}")
    return "\n".join(lines)


def run(argv, league=None, prog=None, description=None):
    """The CLI. train_wcbb.py calls it with league='wcbb': no --league flag, its old defaults."""
    prog = prog or ("train_bball" if league is None else f"train_{league}")
    ap = argparse.ArgumentParser(prog=prog, description=description or __doc__.split("\n")[0])
    if league is None:
        ap.add_argument("--league", choices=sorted(LEAGUES), help="wcbb (women's college) or nba")
    ap.add_argument("--panel", help="the panel CSV (see the module docstring)")
    ap.add_argument("--features", help="comma list from: " + ", ".join(FEATURES) + " (default: the league's)")
    ap.add_argument("--held-out", help="the season scored out of sample (default: the latest)")
    ap.add_argument("--out", help="the model file (default models/<league>_logit_v1.json)")
    ap.add_argument("--version", type=int, help="the file's version (default: N of _vN.json in --out, else 1)")
    ap.add_argument("--force", action="store_true", help="overwrite --out if it exists")
    if league is None:
        ap.add_argument("--write-synthetic", metavar="FILE", help="write the self-test's synthetic NBA model (tests only)")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test(("wcbb",) if league == "wcbb" else tuple(LEAGUES))
    if league is None and a.write_synthetic:
        try:
            write_model(a.write_synthetic, synthetic_nba_model(), a.force)
        except (PanelError, OSError, ArithmeticError) as e:
            print(f"{prog}: {e}", file=sys.stderr)
            return 2
        print(f"wrote {a.write_synthetic}\n  sha256 {sha256_file(a.write_synthetic)}")
        return 0
    lg_key = league or a.league
    if not lg_key:
        ap.error("--league is required (wcbb or nba)")
    lg = LEAGUES[lg_key]
    if not a.panel:
        ap.error("--panel is required (or --self-test)")
    out = a.out or os.path.join(ROOT, "models", f"{lg.model}_v1.json")
    try:
        names = parse_features(a.features or lg.default_features)
        rows, has_mid = read_panel(a.panel, lg_key)
        if "ot" in names and not panel_has(a.panel, "ot"):
            raise PanelError("--features ot: the panel has no ot column")
        res = train(rows, names, a.held_out, has_mid, lg_key)
        obj = model_json(res, a.version if a.version is not None else version_from_path(out),
                         datetime.date.today().isoformat())
        write_model(out, obj, a.force)
    except (PanelError, OSError, ArithmeticError) as e:
        print(f"{prog}: {e}", file=sys.stderr)
        return 2
    print(report_text(res, out))
    return 0


# ---------------------------------------------------------------- self-test

KEYS = ("model", "league", "regulation_s", "version", "trained", "seasons", "n_games", "brier", "features", "mean",
        "scale", "coef", "intercept", "golden")
SPEC_LEAGUES = {"wcbb_logit": ("wcbb", 2400), "nba_logit": ("nba", 2880)}


def spec_loader_check(obj, exact_keys=True):
    """A loader written from docs/wcbb_model.md alone (not the trainer's feature code): the error
    for the first malformed key or golden row that does not reproduce to 1e-6, else None. With
    exact_keys false it reads a file as deskd does: league and regulation_s default from the model
    name, other keys may be present, at least 10 golden rows."""
    for k in KEYS:
        if k not in obj and (exact_keys or k not in ("league", "regulation_s", "seasons", "n_games", "brier", "trained")):
            return f"no key {k}"
    if obj["model"] not in SPEC_LEAGUES or not isinstance(obj["version"], int):
        return "model/version"
    league, reg = SPEC_LEAGUES[obj["model"]]
    if obj.get("league", league) != league:
        return f"league {obj.get('league')!r} is not {obj['model']}'s {league}"
    if obj.get("regulation_s", reg) != reg:
        return f"regulation_s {obj.get('regulation_s')} is not {league}'s {reg}"
    if exact_keys:
        if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", obj["trained"]):
            return "trained"
        if not isinstance(obj["seasons"], list) or not all(isinstance(s, str) for s in obj["seasons"]):
            return "seasons"
        if not isinstance(obj["n_games"], int) or obj["n_games"] <= 0:
            return "n_games"
        br = obj["brier"]
        if set(br) != {"model", "kalshi_mid", "espn", "stern", "stern_ot", "held_out"} or not isinstance(br["held_out"], str):
            return "brier keys"
        for k in ("model", "stern", "stern_ot"):
            if not isinstance(br[k], float):
                return f"brier.{k}"
        for k in ("kalshi_mid", "espn"):
            if br[k] is not None and not isinstance(br[k], float):
                return f"brier.{k}"
    n = len(obj["features"])
    if not n or any(f not in FEATURES for f in obj["features"]):
        return "features"
    if not (len(obj["mean"]) == len(obj["scale"]) == len(obj["coef"]) == n):
        return "lengths"
    if any(s <= 0 for s in obj["scale"]):
        return "scale"
    if len(obj["golden"]) != 10 if exact_keys else len(obj["golden"]) < 10:
        return "golden count"
    for i, g in enumerate(obj["golden"]):
        x = g["in"]
        tau = max(x["t_left_s"], 0) / float(reg)
        p0 = min(max(x["p0"], 0.01), 0.99)
        lp = math.log(p0 / (1 - p0))
        vals = {"logit_p0": lp, "margin": x["margin"], "margin_over_sd": x["margin"] / math.sqrt(tau + 0.01),
                "logit_p0_x_tau": lp * tau, "possession": x["possession"],
                "possession_over_sd": x["possession"] / math.sqrt(tau + 0.01), "tau": tau,
                "logit_p0_x_sqrt_tau": lp * tau ** 0.5, "ot": 1.0 if x.get("ot", False) else 0.0}
        z = obj["intercept"] + sum(c * (vals[f] - m) / s for f, c, m, s in
                                   zip(obj["features"], obj["coef"], obj["mean"], obj["scale"]))
        p = 1.0 / (1.0 + math.exp(-z)) if z > -700.0 else 0.0
        if abs(p - g["p"]) > 1e-6:
            return f"golden row {i}: {p!r} vs {g['p']!r}"
    return None


TRUE_A = {"intercept": 0.05, "logit_p0": 0.45, "margin_over_sd": 0.13, "logit_p0_x_tau": 0.55, "possession": 0.2}
# the NBA truth: the edge study's shape (logit(p0) * sqrt(tau)), plus overtime
TRUE_N = {"intercept": -0.04, "logit_p0": 0.16, "margin_over_sd": 0.12, "logit_p0_x_sqrt_tau": 0.86, "ot": 0.6,
          "possession": 0.15}
SIGMA_B = 14.0


def synth_logistic(rng, seasons, games):
    """One state per game, yes_won drawn from a known logistic model in raw feature units; a noisy
    Kalshi mid (10% missing). Returns (rows, the true p per row)."""
    names = [n for n in TRUE_A if n != "intercept"]
    rows, truth = [], []
    gid = 0
    for s in seasons:
        for _ in range(games):
            gid += 1
            p0 = clamp_p0(_sig(rng.gauss(0.0, 1.2)))
            t = rng.randint(0, 2400)
            el = 1.0 - t / 2400.0
            margin = float(round(rng.gauss(13.0 * NORMAL.inv_cdf(p0) * el, 13.0 * math.sqrt(el) + 0.5)))
            u = rng.random()
            poss = -1.0 if u < 0.45 else (1.0 if u < 0.9 else 0.0)
            f = feature_row(names, t, margin, poss, p0)
            z = TRUE_A["intercept"] + sum(TRUE_A[n] * v for n, v in zip(names, f))
            pt = _sig(z)
            y = 1.0 if rng.random() < pt else 0.0
            mid = round(min(max(_sig(z + rng.gauss(0.0, 0.7)), 0.01), 0.99) * 200) / 200
            if rng.random() < 0.1:
                mid = None
            rows.append({"game_id": f"A{gid}", "season": s, "t_left_s": float(t), "margin": margin,
                         "possession": poss, "p0": p0, "yes_won": y, "kalshi_mid": mid})
            truth.append(pt)
    return rows, truth


def synth_nba(rng, seasons, games, ot_share=0.15):
    """The NBA logistic: one state per game, ot_share of them in overtime (t_left_s the overtime
    clock), a noisy Kalshi mid (10% missing) and a noisier ESPN number (20% missing)."""
    names = [n for n in TRUE_N if n != "intercept"]
    rows, truth = [], []
    gid = 0
    for s in seasons:
        for _ in range(games):
            gid += 1
            p0 = clamp_p0(_sig(rng.gauss(0.0, 1.2)))
            ot = rng.random() < ot_share
            if ot:
                t = rng.randint(0, 300)
                margin = float(round(rng.gauss(0.0, 4.0)))
            else:
                t = rng.randint(0, 2880)
                el = 1.0 - t / 2880.0
                margin = float(round(rng.gauss(14.0 * NORMAL.inv_cdf(p0) * el, 14.0 * math.sqrt(el) + 0.5)))
            u = rng.random()
            poss = -1.0 if u < 0.45 else (1.0 if u < 0.9 else 0.0)
            f = feature_row(names, t, margin, poss, p0, ot, 2880.0)
            z = TRUE_N["intercept"] + sum(TRUE_N[n] * v for n, v in zip(names, f))
            pt = _sig(z)
            y = 1.0 if rng.random() < pt else 0.0
            mid = round(min(max(_sig(z + rng.gauss(0.0, 0.6)), 0.01), 0.99) * 200) / 200
            espn = round(min(max(_sig(z + rng.gauss(0.0, 0.9)), 0.0), 1.0), 3)
            rows.append({"game_id": f"N{gid}", "season": s, "t_left_s": float(t), "ot": ot, "margin": margin,
                         "possession": poss, "p0": p0, "yes_won": y,
                         "kalshi_mid": None if rng.random() < 0.1 else mid,
                         "espn_wp": None if rng.random() < 0.2 else espn})
            truth.append(pt)
    return rows, truth


SYNTH_N_FEATURES = "logit_p0,margin_over_sd,logit_p0_x_tau,logit_p0_x_sqrt_tau,possession_over_sd,ot"
SYNTH_N_NOTE = ("SYNTHETIC, for tests only: tools/train_bball.py --write-synthetic (a seeded NBA panel with "
                "overtime rows; deskd's bball tests check the C++ loader reproduces the Python trainer). "
                "Never a real model.")


def synthetic_nba_model():
    """The fixture: the seeded NBA panel trained on six features (both new ones), dated as the
    self-test was written, a note after the model name as tests/deskd/fixtures/wcbb_synthetic.json has."""
    rows, _ = synth_nba(random.Random(11), ["2023-24", "2024-25", "2025-26"], 800)
    res = train(rows, parse_features(SYNTH_N_FEATURES), None, True, "nba")
    obj = model_json(res, 1, "2026-10-21")
    return {"model": obj["model"], "note": SYNTH_N_NOTE, **{k: v for k, v in obj.items() if k != "model"}}


def synth_brownian(rng, seasons, games, states=8):
    """Stern's model as the truth: the margin a Brownian motion with drift mu = sigma*Phi^-1(p0)
    and variance sigma^2 per game, several states per game, yes_won = final margin > 0."""
    rows, truth = [], []
    gid = 0
    for s in seasons:
        for _ in range(games):
            gid += 1
            p0 = clamp_p0(_sig(rng.gauss(0.0, 1.0)))
            mu = SIGMA_B * NORMAL.inv_cdf(p0)
            ts = sorted({rng.randint(0, 2400) for _ in range(states)}, reverse=True)
            m, el_prev, recs = 0.0, 0.0, []
            for t in ts:
                el = 1.0 - t / 2400.0
                dt = el - el_prev
                m += mu * dt + SIGMA_B * math.sqrt(dt) * rng.gauss(0.0, 1.0)
                el_prev = el
                u = rng.random()
                recs.append((float(t), m, -1.0 if u < 0.45 else (1.0 if u < 0.9 else 0.0)))
            dt = 1.0 - el_prev
            final = m + mu * dt + SIGMA_B * math.sqrt(dt) * rng.gauss(0.0, 1.0)
            y = 1.0 if final > 0 else 0.0
            for t, mg, poss in recs:
                rows.append({"game_id": f"B{gid}", "season": s, "t_left_s": t, "margin": mg,
                             "possession": poss, "p0": p0, "yes_won": y, "kalshi_mid": None})
                truth.append(stern_p(SIGMA_B, t, mg, p0))
    return rows, truth


def write_csv(path, rows, with_mid, with_ot=False, with_espn=False):
    cols = list(REQUIRED) + (["ot"] if with_ot else []) + (["kalshi_mid"] if with_mid else []) \
        + (["espn_wp"] if with_espn else [])
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(cols)
        for r in rows:
            out = []
            for c in cols:
                v = r[c]
                if v is None:
                    out.append("")
                elif isinstance(v, str):
                    out.append(v)
                elif c in ("t_left_s", "possession", "yes_won", "ot") or (c == "margin" and float(v).is_integer()):
                    out.append(str(int(v)))
                else:
                    out.append(repr(v))
            w.writerow(out)


def self_test(leagues=("wcbb", "nba")):
    results = []

    def check(name, ok, detail=""):
        results.append(ok)
        print(f"{'OK  ' if ok else 'FAIL'} {name}" + (f": {detail}" if detail else ""))

    def run_quiet(argv, league=None):
        import contextlib
        import io
        buf, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
            rc = run(argv, league)
        return rc, buf.getvalue() + err.getvalue()

    with tempfile.TemporaryDirectory() as d:
        if "wcbb" in leagues:
            wcbb_checks(d, check, run_quiet)
        if "nba" in leagues:
            nba_checks(d, check, run_quiet)

    # -------- the pieces
    ms = standardize([[3.0] * 5, [0.1] * 3, [1.0, 2.0, 3.0]])
    check("standardize: population sd, scale 1 for a constant column",
          ms[0] == (3.0, 1.0) and ms[1][1] == 1.0 and abs(ms[2][1] - math.sqrt(2.0 / 3.0)) < 1e-15, repr(ms))
    f = feature_row(list(FEATURES), 1200, 6, -1, 0.75)
    tau = 0.5
    want = [math.log(3.0), 6.0, 6.0 / math.sqrt(0.51), math.log(3.0) * tau, -1.0, -1.0 / math.sqrt(0.51), tau,
            math.log(3.0) * math.sqrt(tau), 0.0]
    check("feature_row matches the spec's formulas (wcbb, 2400 s)", all(abs(x - y) < 1e-15 for x, y in zip(f, want)))
    check("feature_row: tau = 0 below 0 s, p0 clamped",
          feature_row(["tau", "logit_p0"], -5, 0, 0, 0.999) == [0.0, logit(0.99)])
    check("Stern: p0 at the tip, margin decides at tau = 0",
          abs(stern_p(13.0, 2400, 0, 0.7) - 0.7) < 1e-9 and stern_p(13.0, 0, 1, 0.1) > 0.999999
          and stern_p(13.0, 0, -1, 0.9) < 1e-6)
    try:
        parse_features("logit_p0,elo")
        check("an unknown feature name is refused", False)
    except PanelError:
        check("an unknown feature name is refused", True)
    with tempfile.TemporaryDirectory() as d:
        bad_csv = os.path.join(d, "bad.csv")
        with open(bad_csv, "w") as fh:
            fh.write("game_id,season,t_left_s,margin,possession,p0,yes_won\ng1,2024-25,2400,0,0,0.5,1\n"
                     "g1,2024-25,1200,3,1,0.5,0\n")
        try:
            read_panel(bad_csv)
            check("a game with two outcomes is refused", False)
        except PanelError as e:
            check("a game with two outcomes is refused", "line 3" in str(e), str(e))
        with open(bad_csv, "w") as fh:
            fh.write("game_id,season,t_left_s,margin,possession,p0,yes_won,kalshi_mid\n"
                     "g1,2024-25,2400,0,0,0.5,1,55\n")
        try:
            read_panel(bad_csv)
            check("a kalshi_mid in cents is refused", False)
        except PanelError as e:
            check("a kalshi_mid in cents is refused", "cents" in str(e), str(e))

    # -------- the fixtures deskd's C++ tests load: their golden rows with this file's feature code
    for name in (f"{lg}_synthetic.json" for lg in leagues):
        path = os.path.join(ROOT, "tests", "deskd", "fixtures", name)
        if not os.path.exists(path):
            print(f"     {name}: not present, skipped")
            continue
        with open(path) as fh:
            obj = json.load(fh)
        worst, n = 0.0, 0
        for g in obj.get("golden", []):
            x = g["in"]
            worst = max(worst, abs(predict(obj, x["t_left_s"], x["margin"], x["possession"], x["p0"],
                                           bool(x.get("ot", False))) - g["p"]))
            n += 1
        err = spec_loader_check(obj, exact_keys=False)
        check(f"tests/deskd/fixtures/{name}: every golden row reproduces with the trainer's code and the spec's",
              n >= 10 and worst <= 1e-6 and err is None, f"{n} rows, worst {worst:.2e}" + (f"; {err}" if err else ""))

    failed = results.count(False)
    print(f"train_bball self-test ({', '.join(leagues)}): {'ok' if not failed else f'FAILED {failed} of {len(results)}'}")
    return 1 if failed else 0


def wcbb_checks(d, check, run_quiet):
    """train_wcbb.py's checks, as they always were (the wcbb CLI is train_wcbb.py's: no --league)."""
    # -------- A: a known logistic model, one state per game, a noisy Kalshi mid
    rng = random.Random(7)
    seasons = ["2022-23", "2023-24", "2024-25"]
    rows_a, truth_a = synth_logistic(rng, seasons, 2500)
    csv_a = os.path.join(d, "panel_a.csv")
    write_csv(csv_a, rows_a, True)
    read_a, has_mid = read_panel(csv_a)
    check("panel A reads back", has_mid and len(read_a) == len(rows_a)
          and all(abs(x["margin"] - y["margin"]) == 0 and x["kalshi_mid"] == y["kalshi_mid"]
                  for x, y in zip(read_a, rows_a)))
    out_a = os.path.join(d, "models", "wcbb_logit_v3.json")
    rc, text = run_quiet(["--panel", csv_a, "--out", out_a], "wcbb")
    check("A: the CLI trains and writes", rc == 0 and os.path.exists(out_a), f"rc {rc}")
    print("\n".join("     " + ln for ln in text.splitlines()))
    res = train(read_a, parse_features(LEAGUES["wcbb"].default_features), None, True)
    m = res["all_fit"]
    worst = 0.0
    parts = []
    for i, n in enumerate(m["features"]):
        zs = abs(m["raw_coef"][i] - TRUE_A[n]) / m["raw_coef_se"][i]
        worst = max(worst, zs)
        parts.append(f"{n} {m['raw_coef'][i]:.3f} (true {TRUE_A[n]}, se {m['raw_coef_se'][i]:.3f})")
    zs = abs(m["raw_intercept"] - TRUE_A["intercept"]) / m["raw_intercept_se"]
    worst = max(worst, zs)
    parts.append(f"intercept {m['raw_intercept']:.3f} (true {TRUE_A['intercept']}, se {m['raw_intercept_se']:.3f})")
    check("A: coefficients recovered within 4 standard errors", worst < 4.0, f"worst {worst:.2f} se; " + "; ".join(parts))
    check("A: the standard errors are tight enough to mean something",
          all(se < 0.25 * abs(TRUE_A[n]) for n, se in zip(m["features"], m["raw_coef_se"])))

    with open(out_a) as fh:
        obj = json.loads(fh.read())
    err = spec_loader_check(obj)
    check("A: the file parses, has every key, golden rows reproduce (spec loader)", err is None, err or "")
    check("A: the file's format", list(obj) == list(KEYS) and obj["version"] == 3 and obj["seasons"] == seasons
          and obj["model"] == "wcbb_logit" and obj["league"] == "wcbb" and obj["regulation_s"] == 2400
          and obj["n_games"] == 7500 and obj["brier"]["held_out"] == "2024-25" and obj["brier"]["espn"] is None
          and obj["features"] == LEAGUES["wcbb"].default_features.split(",") and obj["coef"] == res["all_fit"]["coef"]
          and obj["brier"] == res["brier"] and obj["intercept"] == res["all_fit"]["intercept"])
    g_t = sorted({g["in"]["t_left_s"] for g in obj["golden"]})
    check("A: 10 golden rows spread over the inputs", len(obj["golden"]) == 10 and g_t == [60, 600, 1200, 1800, 2400]
          and {g["in"]["possession"] for g in obj["golden"]} == {-1, 0, 1}
          and not any(g["in"]["ot"] for g in obj["golden"])
          and min(g["p"] for g in obj["golden"]) < 0.3 < 0.7 < max(g["p"] for g in obj["golden"]),
          ", ".join(f"{g['p']:.3f}" for g in obj["golden"]))
    check("A: golden p is the trainer's predict, bit for bit",
          all(predict(obj, *(g["in"][k] for k in ("t_left_s", "margin", "possession", "p0", "ot"))) == g["p"]
              for g in obj["golden"]))
    bad = json.loads(json.dumps(obj))
    bad["golden"][4]["p"] += 2e-6
    e2 = spec_loader_check(bad)
    check("A: a golden row off by 2e-6 is refused, naming the row", e2 is not None and e2.startswith("golden row 4"), e2 or "")

    b = res["brier"]
    ho_idx = [i for i, r in enumerate(read_a) if r["season"] == "2024-25" and r["kalshi_mid"] is not None]
    b_true = brier([truth_a[i] for i in ho_idx], [read_a[i]["yes_won"] for i in ho_idx])
    check("A: Brier scored on the held-out rows with a mid", res["n_scored_rows"] == len(ho_idx) < res["n_held_rows"] == 2500)
    check("A: Brier order: truth ~ model < stern < p0, model < noisy mid, all < 0.25",
          b["model"] <= b_true + 0.004 and b["model"] < b["stern"] < res["brier_p0"] < 0.25
          and b["model"] < b["kalshi_mid"] < 0.25,
          f"truth {b_true:.5f} model {b['model']:.5f} stern {b['stern']:.5f} mid {b['kalshi_mid']:.5f} p0 {res['brier_p0']:.5f}")
    rc2, msg = run_quiet(["--panel", csv_a, "--out", out_a], "wcbb")
    check("A: an existing file is not overwritten without --force", rc2 == 2 and "exists" in msg)
    out_a2 = os.path.join(d, "models", "wcbb_logit_v4.json")
    rc3, _ = run_quiet(["--league", "wcbb", "--panel", csv_a, "--out", out_a2])
    with open(out_a2) as fh:
        obj2 = json.load(fh)
    check("A: train_bball --league wcbb writes the same model", rc3 == 0 and obj2["coef"] == obj["coef"]
          and obj2["golden"] == obj["golden"] and obj2["version"] == 4)
    rc4, msg4 = run_quiet(["--panel", csv_a, "--out", out_a2, "--force", "--features", "logit_p0,ot"], "wcbb")
    check("A: --features ot needs an ot column", rc4 == 2 and "no ot column" in msg4, msg4.strip())

    # -------- B: Stern's Brownian motion as the truth, 8 states per game, no kalshi_mid column
    rng = random.Random(7)
    rows_b, truth_b = synth_brownian(rng, ["2023-24", "2024-25", "2025-26"], 300)
    csv_b = os.path.join(d, "panel_b.csv")
    write_csv(csv_b, rows_b, False)
    out_b = os.path.join(d, "wcbb_logit_v1.json")
    feats_b = "logit_p0,margin_over_sd,logit_p0_x_tau,possession,tau"
    rc, text = run_quiet(["--panel", csv_b, "--out", out_b, "--features", feats_b, "--held-out", "2024-25"], "wcbb")
    check("B: the CLI trains and writes", rc == 0, f"rc {rc}")
    print("\n".join("     " + ln for ln in text.splitlines()))
    read_b, has_mid_b = read_panel(csv_b)
    res_b = train(read_b, parse_features(feats_b), "2024-25", has_mid_b)
    with open(out_b) as fh:
        obj_b = json.load(fh)
    err = spec_loader_check(obj_b)
    check("B: the file passes the spec loader, kalshi_mid null, n_games by game",
          err is None and obj_b["brier"]["kalshi_mid"] is None and obj_b["n_games"] == 900
          and obj_b["brier"]["held_out"] == "2024-25" and obj_b["version"] == 1, err or "")
    sg = res_b["stern_sigma"]
    check("B: Stern sigma recovered within 10%", abs(sg - SIGMA_B) < 0.1 * SIGMA_B, f"{sg:.3f} vs {SIGMA_B}")
    idx = [i for i, r in enumerate(read_b) if r["season"] == "2024-25"]
    bt = brier([truth_b[i] for i in idx], [read_b[i]["yes_won"] for i in idx])
    bb = res_b["brier"]
    check("B: Brier order: truth ~ stern <= model + 0.003, model < p0",
          abs(bb["stern"] - bt) < 0.003 and bb["stern"] <= bb["model"] + 0.003 and bb["model"] < res_b["brier_p0"],
          f"truth {bt:.5f} stern {bb['stern']:.5f} model {bb['model']:.5f} p0 {res_b['brier_p0']:.5f}")

    # -------- the clock, overtime rows and the ot feature in wcbb
    wc = LEAGUES["wcbb"]
    check("wcbb t_left: wcbb_t_left as it was, overtime 0 with ot",
          t_left(wc, 1, 600) == (2400, False) and t_left(wc, 3, 252) == (852, False) and t_left(wc, 4, 0.5) == (0.5, False)
          and t_left(wc, 5, 120) == (0.0, True) and t_left(wc, 2, 601) is None and t_left(wc, 0, 10) is None
          and t_left(wc, 2, -1) is None)
    ot_csv = os.path.join(d, "ot.csv")
    with open(ot_csv, "w") as fh:
        fh.write("game_id,season,t_left_s,ot,margin,possession,p0,yes_won\ng1,2024-25,0,1,0,0,0.5,1\n"
                 "g2,2024-25,30,1,0,0,0.5,1\n")
    try:
        read_panel(ot_csv)
        check("wcbb: an overtime row with time left is refused (t_left 0 in overtime)", False)
    except PanelError as e:
        check("wcbb: an overtime row with time left is refused (t_left 0 in overtime)", "line 3" in str(e), str(e))
    res_ot = dict(res, all_fit=dict(res["all_fit"], features=["logit_p0", "ot"], mean=[0.0, 0.0], scale=[1.0, 1.0],
                                    coef=[0.5, -0.3], intercept=0.0))
    obj_ot = model_json(res_ot, 1, "2026-10-21")
    last = obj_ot["golden"][-1]["in"]
    check("wcbb with the ot feature: the last golden row is an overtime row (t_left_s 0)",
          last == {"t_left_s": 0, "margin": -2, "possession": -1, "p0": 0.97, "ot": True}
          and spec_loader_check(obj_ot) is None
          and abs(obj_ot["golden"][-1]["p"] - _sig(0.5 * logit(0.97) - 0.3)) < 1e-15)


def nba_checks(d, check, run_quiet):
    nba = LEAGUES["nba"]
    # -------- the clock: deskd-bball's cases
    check("nba t_left: Q1 12:00 2880, Q3 4:12 972, Q4 0:35.4, OT1 3:00 180 with ot, 13:00 in Q2 and period 0 none",
          t_left(nba, 1, 720) == (2880, False) and t_left(nba, 3, 252) == (972, False)
          and t_left(nba, 4, 35.4) == (35.4, False) and t_left(nba, 5, 180) == (180.0, True)
          and t_left(nba, 6, 300) == (300.0, True) and t_left(nba, 5, 301) is None
          and t_left(nba, 2, 780) is None and t_left(nba, 0, 100) is None)
    f = feature_row(list(FEATURES), 1440, 6, 1, 0.75, False, 2880.0)
    lp, tau = math.log(3.0), 0.5
    want = [lp, 6.0, 6.0 / math.sqrt(0.51), lp * tau, 1.0, 1.0 / math.sqrt(0.51), tau, lp * math.sqrt(tau), 0.0]
    check("nba features at 1440 s of 2880: tau 0.5", all(abs(x - y) < 1e-15 for x, y in zip(f, want)))
    f = feature_row(["tau", "ot", "margin_over_sd", "logit_p0_x_sqrt_tau"], 180, -2, 0, 0.25, True, 2880.0)
    tau = 180 / 2880
    want = [tau, 1.0, -2 / math.sqrt(tau + 0.01), -lp * math.sqrt(tau)]
    check("nba features in overtime: tau the overtime clock over 2880, ot 1", all(abs(x - y) < 1e-15 for x, y in zip(f, want)), f)

    # -------- the overtime baselines
    sd = 13.0
    check("overtime_probability: sports_totals.cpp's density x tie width",
          abs(overtime_probability(0.0, 1.0, sd, 1.64) - 1.64 / (sd * math.sqrt(2 * math.pi))) < 1e-15
          and abs(overtime_probability(5.0, 0.25, sd, 1.64)
                  - 1.64 * math.exp(-0.5 * (5 / 6.5) ** 2) / (6.5 * math.sqrt(2 * math.pi))) < 1e-15
          and overtime_probability(0.0, 0.0, sd, 1.64) == 1.0 and overtime_probability(2.0, 0.0, sd, 1.64) == 0.0
          and overtime_probability(0.0, 1e-9, sd, 1.64) == 1.0 and math.isnan(overtime_probability(-1.0, 0.5, sd, 1.64)))
    check("leadtime_prob: sports_live.cpp's, the lead decides under f 0.002",
          leadtime_prob(0, 0, 0.5, 13) == 0.5 and leadtime_prob(3, -50, 0.001, 13) == 1.0
          and leadtime_prob(0, 5, 0.001, 13) == 0.5 and leadtime_prob(1, 0, 0.5, 0) == 0.5
          and abs(leadtime_prob(4, 2, 0.25, 13) - _phi(4.5 / 6.5)) < 1e-15)
    sig = 13.0
    q_ot = _phi(NORMAL.inv_cdf(0.7) * math.sqrt(300 / 2880))
    check("stern_ot: Stern's P at p0 1/2 (to 1e-7: Stern's sqrt(tau + 1e-9)); a level game at the horn gets q_ot; "
          "a lead at the horn decides",
          all(abs(stern_ot_p(sig, nba, t, m, 0.5) - stern_p(sig, t, m, 0.5, 2880.0)) < 1e-7
              for t, m in ((2880, 0), (1000, 5), (30, -2), (0, 1)))
          and abs(stern_ot_p(sig, nba, 0, 0, 0.7) - q_ot) < 1e-12 and stern_ot_p(sig, nba, 0, 1, 0.3) == 1.0
          and stern_ot_p(sig, nba, 0, -1, 0.9) == 0.0)
    late = stern_ot_p(sig, nba, 3, 0, 0.7)
    check("stern_ot: a level game 3 s from the horn leans to the favourite by about q_ot",
          0.5 < late and abs(late - q_ot) < 0.02 and stern_p(sig, 3, 0, 0.7, 2880.0) < late, f"{late:.4f} q_ot {q_ot:.4f}")

    # -------- N: the NBA logistic with overtime rows, a mid and ESPN
    rng = random.Random(11)
    seasons = ["2023-24", "2024-25", "2025-26"]
    rows_n, truth_n = synth_nba(rng, seasons, 2000)
    csv_n = os.path.join(d, "panel_n.csv")
    write_csv(csv_n, rows_n, True, with_ot=True, with_espn=True)
    read_n, has_mid = read_panel(csv_n, "nba")
    check("panel N reads back, overtime and ESPN columns included", has_mid and len(read_n) == len(rows_n)
          and all(x["ot"] == y["ot"] and x["espn_wp"] == y["espn_wp"] and x["t_left_s"] == y["t_left_s"]
                  for x, y in zip(read_n, rows_n)))
    feats_n = ",".join(n for n in TRUE_N if n != "intercept")
    out_n = os.path.join(d, "models", "nba_logit_v2.json")
    rc, text = run_quiet(["--league", "nba", "--panel", csv_n, "--out", out_n, "--features", feats_n])
    check("N: the CLI trains and writes", rc == 0 and os.path.exists(out_n), f"rc {rc}")
    print("\n".join("     " + ln for ln in text.splitlines()))
    res = train(read_n, parse_features(feats_n), None, True, "nba")
    m = res["all_fit"]
    worst, parts = 0.0, []
    for i, n in enumerate(m["features"]):
        zs = abs(m["raw_coef"][i] - TRUE_N[n]) / m["raw_coef_se"][i]
        worst = max(worst, zs)
        parts.append(f"{n} {m['raw_coef'][i]:.3f} (true {TRUE_N[n]}, se {m['raw_coef_se'][i]:.3f})")
    zs = abs(m["raw_intercept"] - TRUE_N["intercept"]) / m["raw_intercept_se"]
    worst = max(worst, zs)
    check("N: coefficients recovered within 4 standard errors (ot and logit_p0_x_sqrt_tau among them)", worst < 4.0,
          f"worst {worst:.2f} se; " + "; ".join(parts))
    with open(out_n) as fh:
        obj = json.load(fh)
    err = spec_loader_check(obj)
    check("N: the file passes the spec loader", err is None, err or "")
    check("N: the file's format: nba_logit, league nba, regulation_s 2880, ESPN scored",
          list(obj) == list(KEYS) and obj["model"] == "nba_logit" and obj["league"] == "nba"
          and obj["regulation_s"] == 2880 and obj["version"] == 2 and obj["n_games"] == 6000
          and obj["features"] == feats_n.split(",") and obj["brier"] == res["brier"]
          and isinstance(obj["brier"]["espn"], float) and isinstance(obj["brier"]["kalshi_mid"], float))
    gi = [(g["in"]["t_left_s"], g["in"]["ot"]) for g in obj["golden"]]
    check("N: golden rows at 2880/2160/1440/720 (two each), 60 and an overtime row",
          gi == [(2880, False)] * 2 + [(2160, False)] * 2 + [(1440, False)] * 2 + [(720, False)] * 2
          + [(60, False), (180, True)] and min(g["p"] for g in obj["golden"]) < 0.3 < 0.7 < max(g["p"] for g in obj["golden"]),
          ", ".join(f"{g['p']:.3f}" for g in obj["golden"]))
    check("N: golden p is the trainer's predict, bit for bit",
          all(predict(obj, *(g["in"][k] for k in ("t_left_s", "margin", "possession", "p0", "ot"))) == g["p"]
              for g in obj["golden"]))
    off = json.loads(json.dumps(obj))
    off["golden"][9]["in"]["ot"] = False
    e1 = spec_loader_check(off)
    check("N: the overtime row read as regulation does not reproduce", e1 is not None and e1.startswith("golden row 9"), e1 or "")
    wrong = dict(obj, regulation_s=2400)
    e2 = spec_loader_check(wrong)
    check("N: nba_logit with regulation_s 2400 is refused naming the mismatch", e2 is not None and "2400" in e2
          and "2880" in e2, e2 or "")
    b = res["brier"]
    ho_idx = [i for i, r in enumerate(read_n) if r["season"] == "2025-26" and r["kalshi_mid"] is not None
              and r["espn_wp"] is not None]
    b_true = brier([truth_n[i] for i in ho_idx], [read_n[i]["yes_won"] for i in ho_idx])
    check("N: scored on the held-out rows with a mid and an ESPN number",
          res["n_scored_rows"] == len(ho_idx) < res["n_held_rows"] == 2000)
    check("N: Brier order: truth ~ model < mid < espn, model < stern, all < 0.25",
          b["model"] <= b_true + 0.004 and b["model"] < b["kalshi_mid"] < b["espn"] < 0.25 and b["model"] < b["stern"] < 0.25
          and b["stern_ot"] < 0.25,
          f"truth {b_true:.5f} model {b['model']:.5f} mid {b['kalshi_mid']:.5f} espn {b['espn']:.5f} "
          f"stern {b['stern']:.5f} stern+ot {b['stern_ot']:.5f}")
    rc_d, _ = run_quiet(["--league", "nba", "--panel", csv_n, "--out", os.path.join(d, "nba_default.json")])
    with open(os.path.join(d, "nba_default.json")) as fh:
        obj_d = json.load(fh)
    check("N: the NBA default features: no possession, with ot", rc_d == 0
          and obj_d["features"] == ["logit_p0", "margin_over_sd", "logit_p0_x_tau", "ot"])

    # -------- NBA panel refusals
    def refused(text, needle):
        path = os.path.join(d, "bad_n.csv")
        with open(path, "w") as fh:
            fh.write(text)
        try:
            read_panel(path, "nba")
            return False, "read"
        except PanelError as e:
            return needle in str(e), str(e)

    hdr = "game_id,season,t_left_s,ot,margin,possession,p0,yes_won\n"
    ok, why = refused("game_id,season,t_left_s,margin,possession,p0,yes_won\ng1,2024-25,100,0,0,0.5,1\n", "no column ot")
    check("nba: a panel without an ot column is refused", ok, why)
    ok, why = refused(hdr + "g1,2024-25,301,1,0,0,0.5,1\n", "over 300 in overtime")
    check("nba: an overtime clock over 300 s is refused", ok, why)
    ok, why = refused(hdr + "g1,2024-25,2881,0,0,0,0.5,1\n", "over 2880")
    check("nba: t_left_s over 2880 in regulation is refused", ok, why)
    ok, why = refused(hdr + "g1,2024-25,100,2,0,0,0.5,1\n", "ot '2'")
    check("nba: ot other than 0 or 1 is refused", ok, why)
    with open(os.path.join(d, "tf.csv"), "w") as fh:
        fh.write(hdr + "g1,2024-25,120,true,0,0,0.5,1\ng2,2024-25,120,False,0,0,0.5,0\n")
    tf, _ = read_panel(os.path.join(d, "tf.csv"), "nba")
    check("nba: ot read from true/false too", [r["ot"] for r in tf] == [True, False])

    # -------- the synthetic fixture deskd's bball tests can load
    fx = synthetic_nba_model()
    err = spec_loader_check({k: v for k, v in fx.items() if k != "note"})
    check("--write-synthetic: an NBA model with both new features and an overtime golden row, spec loader clean",
          err is None and fx["note"].startswith("SYNTHETIC") and list(fx)[:2] == ["model", "note"]
          and {"ot", "logit_p0_x_sqrt_tau"} <= set(fx["features"]) and fx["golden"][9]["in"]["ot"] is True,
          err or "")
    fx_path = os.path.join(d, "nba_synthetic.json")
    rc_w, _ = run_quiet(["--write-synthetic", fx_path])
    rc_w2, msg_w2 = run_quiet(["--write-synthetic", fx_path])
    with open(fx_path) as fh:
        fx2 = json.load(fh)
    check("--write-synthetic writes it once (never over a file without --force)",
          rc_w == 0 and fx2 == json.loads(json.dumps(fx)) and rc_w2 == 2 and "exists" in msg_w2)


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
