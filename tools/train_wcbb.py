#!/usr/bin/env python3
"""Train the women's college basketball win-probability model (docs/wcbb_model.md, plan 5(f)).

The model's fair for a KXNCAAWBGAME market is the probability that the market's YES team wins,
from the game state and the pregame Kalshi price: a logistic regression on features named in
docs/wcbb_model.md. This tool fits it, scores it on a held-out season against the Kalshi mid and
Stern's model, refits on every season and writes models/wcbb_logit_vN.json, which deskd
(strat/wcbb_model.h) loads and refuses unless its 10 golden rows reproduce to 1e-6.

Standard library only (no numpy, no pandas). No network. It reads only the --panel file it is given.

The panel (--panel FILE.csv): a header row, then one row per game state. Columns, in any order
(extra columns are ignored):

  game_id     any string; every row of one game shares it
  season      a label such as 2024-25; the held-out season is chosen by it. "Latest" is the
              greatest label in plain string order, so keep labels the same shape (2023-24, 2024-25)
  t_left_s    seconds left in regulation, 0 to 2400 (40 minutes); 0 in overtime
  margin      the YES team's score minus the opponent's
  possession  +1 the YES team has the ball, -1 the opponent, 0 unknown
  p0          the pregame YES probability (the Kalshi mid at the scheduled tip, spread 15c or
              less), as a probability; clamped here to [0.01, 0.99]
  yes_won     1 if the YES team won the game, else 0 (the same on every row of a game)
  kalshi_mid  optional: the Kalshi YES mid at that state, as a probability in [0, 1] (not cents);
              blank (or nan) where there was no two-sided book

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

What it does:

  1. Features (--features, the vocabulary of docs/wcbb_model.md), with tau = max(t_left_s, 0)/2400
     and logit(x) = ln(x/(1-x)): logit_p0, margin, margin_over_sd = margin/sqrt(tau+0.01),
     logit_p0_x_tau, possession, possession_over_sd = possession/sqrt(tau+0.01), tau.
  2. Standardization: each feature's mean and population standard deviation over the rows being
     fitted (scale 1 when the sd is 0: below 1e-12 * max(1, |mean|)).
  3. Fit: logistic regression by Newton's method (IRLS) with step halving, minimizing the mean log
     loss plus RIDGE/2 * sum(coef^2) on the standardized features, RIDGE = 1e-6, intercept not
     penalized. The ridge only keeps the Newton system well posed (near-separable late-game rows,
     collinear features); it is too small to move a real fit.
  4. Held out (--held-out SEASON, default the latest): fit on the other seasons and score that
     season with the Brier score (mean squared error of the probability) for the model, the Kalshi
     mid and Stern's model. All three are scored on the same rows: the held-out rows that have a
     kalshi_mid when any does, else every held-out row (the model's Brier on every held-out row
     is printed too).
  5. Shipped: refit on every season; those coefficients, means and scales go in the file, with
     the held-out Brier scores of step 4. Both coefficient sets are printed.

Stern's model (Stern 1994, "A Brownian motion model for the progression of sports scores"): the
remaining margin is normal with mean mu * tau and variance sigma^2 * tau, so

  P(YES wins) = Phi((margin + mu * tau) / (sigma * sqrt(tau + 1e-9))),  mu = sigma * Phi^-1(p0),

which is p0 at the tip (tau = 1, margin 0). It has no possession term. sigma (points per game) is
fitted on the training seasons by one-parameter maximum likelihood: a log-spaced grid over
[2, 80] then a golden-section search, with P clamped to [1e-6, 1 - 1e-6] inside the likelihood only
so a single near-certain miss (an overtime row, tau = 0) cannot decide sigma.

The file (--out, default models/wcbb_logit_v1.json; refuses to overwrite without --force) is
exactly the format of docs/wcbb_model.md, floats at full precision. Its 10 golden rows sit at
t_left_s 2400, 1800, 1200, 600 and 60 (two each) with margins, possessions and p0 varied, p from
the same feature code the fit used:

  p = 1 / (1 + exp(-(intercept + sum_i coef[i] * (f_i - mean[i]) / scale[i])))

TODO: an Elo prior for games whose pregame book is thin (plan 5(f)); XGBoost only if it beats
this logistic on held-out Brier.

Usage:
  python3 tools/train_wcbb.py --panel wbpanel.csv
  python3 tools/train_wcbb.py --panel wbpanel.csv --held-out 2024-25 \\
      --features logit_p0,margin_over_sd,logit_p0_x_tau,possession,possession_over_sd \\
      --out models/wcbb_logit_v2.json
  python3 tools/train_wcbb.py --self-test
"""
import argparse
import csv
import datetime
import json
import math
import os
import random
import re
import statistics
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

FEATURES = ("logit_p0", "margin", "margin_over_sd", "logit_p0_x_tau", "possession",
            "possession_over_sd", "tau")
DEFAULT_FEATURES = "logit_p0,margin_over_sd,logit_p0_x_tau,possession"
REQUIRED = ("game_id", "season", "t_left_s", "margin", "possession", "p0", "yes_won")
FULL_GAME_S = 2400.0
P0_MIN, P0_MAX = 0.01, 0.99
RIDGE = 1e-6
SD_ZERO = 1e-12
STERN_EPS = 1e-9          # inside sqrt(tau + eps), so tau = 0 does not divide by zero
STERN_LIK_CLAMP = 1e-6    # P clamped to [c, 1 - c] in the likelihood only
STERN_SIGMA_RANGE = (2.0, 80.0)
GOLDEN_INPUTS = [
    # (t_left_s, margin, possession, p0): the tip, both halves, the last minute; leads and deficits
    (2400, 0, 0, 0.5),
    (2400, 0, 1, 0.8),
    (1800, 4, -1, 0.35),
    (1800, -7, 1, 0.65),
    (1200, 10, 1, 0.55),
    (1200, -3, 0, 0.1),
    (600, 2, -1, 0.9),
    (600, -12, 1, 0.7),
    (60, 1, 1, 0.45),
    (60, -2, -1, 0.97),
]
NORMAL = statistics.NormalDist()


class PanelError(ValueError):
    pass


# ---------------------------------------------------------------- the feature code (one copy)

def clamp_p0(p0):
    return min(max(float(p0), P0_MIN), P0_MAX)


def logit(x):
    return math.log(x / (1.0 - x))


def tau_of(t_left_s):
    return max(float(t_left_s), 0.0) / FULL_GAME_S


def feature_row(names, t_left_s, margin, possession, p0):
    """The values of the named features for one game state (docs/wcbb_model.md)."""
    tau = tau_of(t_left_s)
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
    }
    return [v[n] for n in names]


def spec_sigmoid(z):
    """1 / (1 + exp(-z)) as written in the spec (exp overflowing to inf gives 0, as in C++)."""
    try:
        return 1.0 / (1.0 + math.exp(-z))
    except OverflowError:
        return 0.0


def predict(model, t_left_s, margin, possession, p0):
    """p from a model dict (the file's keys), exactly the spec's formula."""
    f = feature_row(model["features"], t_left_s, margin, possession, p0)
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


def read_panel(path):
    """Rows as dicts; checks every field and that each game has one season and one outcome."""
    rows = []
    games = {}
    with open(path, newline="", encoding="utf-8-sig") as fh:
        rd = csv.DictReader(fh)
        cols = [c.strip() for c in (rd.fieldnames or [])]
        missing = [c for c in REQUIRED if c not in cols]
        if missing:
            raise PanelError(f"{path}: no column {', '.join(missing)} (header: {', '.join(cols)})")
        rd.fieldnames = cols
        has_mid = "kalshi_mid" in cols
        for line, r in enumerate(rd, start=2):
            r = {k: (v.strip() if isinstance(v, str) else v) for k, v in r.items()}
            gid, season = r["game_id"], r["season"]
            if not gid or not season:
                raise PanelError(f"line {line}: empty game_id or season")
            t = _num(r["t_left_s"], "t_left_s", line)
            if t > FULL_GAME_S:
                raise PanelError(f"line {line}: t_left_s {t:g} is over 2400 (seconds left in regulation)")
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
            mid = None
            if has_mid and (r.get("kalshi_mid") or "").lower() not in MISSING:
                mid = _num(r["kalshi_mid"], "kalshi_mid", line)
                if not 0.0 <= mid <= 1.0:
                    raise PanelError(f"line {line}: kalshi_mid {mid:g} is not in [0, 1] (cents? divide by 100)")
            seen = games.setdefault(gid, (season, y, line))
            if seen[0] != season:
                raise PanelError(f"line {line}: game {gid} is in season {season} here, {seen[0]} on line {seen[2]}")
            if seen[1] != y:
                raise PanelError(f"line {line}: game {gid} has yes_won {y:g} here, {seen[1]:g} on line {seen[2]}")
            rows.append({"game_id": gid, "season": season, "t_left_s": t, "margin": margin,
                         "possession": poss, "p0": clamp_p0(p0), "yes_won": y, "kalshi_mid": mid})
    if not rows:
        raise PanelError(f"{path}: no rows")
    return rows, has_mid


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


def fit(rows, names):
    """The standardized logistic fit on rows: a model dict (the file's keys) plus diagnostics."""
    raw = [feature_row(names, r["t_left_s"], r["margin"], r["possession"], r["p0"]) for r in rows]
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


# ---------------------------------------------------------------- Stern's model

def _phi(x):
    return 0.5 * math.erfc(-x / math.sqrt(2.0))


def stern_p(sigma, t_left_s, margin, p0):
    tau = tau_of(t_left_s)
    mu = sigma * NORMAL.inv_cdf(clamp_p0(p0))
    return _phi((margin + mu * tau) / (sigma * math.sqrt(tau + STERN_EPS)))


def fit_stern_sigma(rows):
    """sigma maximizing the (clamped) Bernoulli likelihood of yes_won under Stern's model."""
    # x_i(sigma) = a_i / sigma + c_i, with a_i, c_i fixed per row
    pre = []
    for r in rows:
        tau = tau_of(r["t_left_s"])
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


def train(rows, names, held_out=None, has_mid=False):
    seasons = sorted({r["season"] for r in rows})
    if held_out is None:
        held_out = seasons[-1]
    if held_out not in seasons:
        raise PanelError(f"--held-out {held_out}: not a season in the panel ({', '.join(seasons)})")
    if len(seasons) < 2:
        raise PanelError(f"one season ({seasons[0]}): a held-out score needs at least two")
    tr = [r for r in rows if r["season"] != held_out]
    ho = [r for r in rows if r["season"] == held_out]
    m_ho = fit(tr, names)
    m_all = fit(rows, names)
    sigma, sigma_edge = fit_stern_sigma(tr)

    ys_all = [r["yes_won"] for r in ho]
    p_model_all = [predict(m_ho, r["t_left_s"], r["margin"], r["possession"], r["p0"]) for r in ho]
    scored = [r for r in ho if r["kalshi_mid"] is not None] if has_mid else []
    if not scored:
        scored = ho
    ys = [r["yes_won"] for r in scored]
    b_model = brier([predict(m_ho, r["t_left_s"], r["margin"], r["possession"], r["p0"]) for r in scored], ys)
    b_stern = brier([stern_p(sigma, r["t_left_s"], r["margin"], r["p0"]) for r in scored], ys)
    b_mid = brier([r["kalshi_mid"] for r in scored], ys) if scored is not ho else None
    b_p0 = brier([r["p0"] for r in scored], ys)
    return {
        "seasons": seasons, "held_out": held_out, "n_games": len({r["game_id"] for r in rows}),
        "n_rows": len(rows), "n_train_rows": len(tr), "n_held_rows": len(ho), "n_scored_rows": len(scored),
        "held_fit": m_ho, "all_fit": m_all, "stern_sigma": sigma, "stern_sigma_at_edge": sigma_edge,
        "brier": {"model": b_model, "kalshi_mid": b_mid, "stern": b_stern, "held_out": held_out},
        "brier_model_all_held": brier(p_model_all, ys_all), "brier_p0": b_p0,
    }


def model_json(res, version, trained):
    m = res["all_fit"]
    out = {
        "model": "wcbb_logit",
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
    out["golden"] = [{"in": {"t_left_s": t, "margin": mg, "possession": ps, "p0": p0},
                      "p": predict(out, t, mg, ps, p0)} for t, mg, ps, p0 in GOLDEN_INPUTS]
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


def version_from_path(path):
    m = re.search(r"_v(\d+)\.json$", os.path.basename(path))
    return int(m.group(1)) if m else 1


def report_text(res, out_path):
    def f(v):
        return "-" if v is None else f"{v:.5f}"
    b = res["brier"]
    lines = [
        f"panel: {res['n_rows']} rows, {res['n_games']} games, seasons {', '.join(res['seasons'])}",
        f"held out {res['held_out']}: fit on {res['n_train_rows']} rows of the other seasons, "
        f"scored on {res['n_scored_rows']} of its {res['n_held_rows']} rows"
        + (" (those with a kalshi_mid)" if b["kalshi_mid"] is not None else ""),
        f"  Brier  model {f(b['model'])}  kalshi_mid {f(b['kalshi_mid'])}  stern {f(b['stern'])}"
        f"  p0 {f(res['brier_p0'])}   (model on every held-out row {f(res['brier_model_all_held'])})",
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
    return "\n".join(lines)


def run(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--panel", help="the panel CSV (see the module docstring)")
    ap.add_argument("--features", default=DEFAULT_FEATURES, help=f"comma list from: {', '.join(FEATURES)}")
    ap.add_argument("--held-out", help="the season scored out of sample (default: the latest)")
    ap.add_argument("--out", default=os.path.join(ROOT, "models", "wcbb_logit_v1.json"))
    ap.add_argument("--version", type=int, help="the file's version (default: N of _vN.json in --out, else 1)")
    ap.add_argument("--force", action="store_true", help="overwrite --out if it exists")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.panel:
        ap.error("--panel is required (or --self-test)")
    try:
        names = parse_features(a.features)
        rows, has_mid = read_panel(a.panel)
        res = train(rows, names, a.held_out, has_mid)
        obj = model_json(res, a.version if a.version is not None else version_from_path(a.out),
                         datetime.date.today().isoformat())
        write_model(a.out, obj, a.force)
    except (PanelError, OSError, ArithmeticError) as e:
        print(f"train_wcbb: {e}", file=sys.stderr)
        return 2
    print(report_text(res, a.out))
    return 0


# ---------------------------------------------------------------- self-test

KEYS = ("model", "version", "trained", "seasons", "n_games", "brier", "features", "mean", "scale",
        "coef", "intercept", "golden")


def spec_loader_check(obj):
    """A loader written from docs/wcbb_model.md alone (not the trainer's feature code): the error
    for the first malformed key or golden row that does not reproduce to 1e-6, else None."""
    for k in KEYS:
        if k not in obj:
            return f"no key {k}"
    if obj["model"] != "wcbb_logit" or not isinstance(obj["version"], int):
        return "model/version"
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", obj["trained"]):
        return "trained"
    if not isinstance(obj["seasons"], list) or not all(isinstance(s, str) for s in obj["seasons"]):
        return "seasons"
    if not isinstance(obj["n_games"], int) or obj["n_games"] <= 0:
        return "n_games"
    br = obj["brier"]
    if set(br) != {"model", "kalshi_mid", "stern", "held_out"} or not isinstance(br["held_out"], str):
        return "brier keys"
    for k in ("model", "stern"):
        if not isinstance(br[k], float):
            return f"brier.{k}"
    if br["kalshi_mid"] is not None and not isinstance(br["kalshi_mid"], float):
        return "brier.kalshi_mid"
    n = len(obj["features"])
    if not n or any(f not in FEATURES for f in obj["features"]):
        return "features"
    if not (len(obj["mean"]) == len(obj["scale"]) == len(obj["coef"]) == n):
        return "lengths"
    if any(s <= 0 for s in obj["scale"]):
        return "scale"
    if len(obj["golden"]) != 10:
        return "golden count"
    for i, g in enumerate(obj["golden"]):
        x = g["in"]
        tau = max(x["t_left_s"], 0) / 2400.0
        p0 = min(max(x["p0"], 0.01), 0.99)
        lp = math.log(p0 / (1 - p0))
        vals = {"logit_p0": lp, "margin": x["margin"], "margin_over_sd": x["margin"] / math.sqrt(tau + 0.01),
                "logit_p0_x_tau": lp * tau, "possession": x["possession"],
                "possession_over_sd": x["possession"] / math.sqrt(tau + 0.01), "tau": tau}
        z = obj["intercept"] + sum(c * (vals[f] - m) / s for f, c, m, s in
                                   zip(obj["features"], obj["coef"], obj["mean"], obj["scale"]))
        p = 1.0 / (1.0 + math.exp(-z)) if z > -700.0 else 0.0
        if abs(p - g["p"]) > 1e-6:
            return f"golden row {i}: {p!r} vs {g['p']!r}"
    return None


TRUE_A = {"intercept": 0.05, "logit_p0": 0.45, "margin_over_sd": 0.13, "logit_p0_x_tau": 0.55, "possession": 0.2}
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


def write_csv(path, rows, with_mid):
    cols = list(REQUIRED) + (["kalshi_mid"] if with_mid else [])
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
                elif c in ("t_left_s", "possession", "yes_won") or (c == "margin" and float(v).is_integer()):
                    out.append(str(int(v)))
                else:
                    out.append(repr(v))
            w.writerow(out)


def self_test():
    results = []

    def check(name, ok, detail=""):
        results.append(ok)
        print(f"{'OK  ' if ok else 'FAIL'} {name}" + (f": {detail}" if detail else ""))

    def run_quiet(argv):
        import contextlib
        import io
        buf, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
            rc = run(argv)
        return rc, buf.getvalue() + err.getvalue()

    with tempfile.TemporaryDirectory() as d:
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
        rc, text = run_quiet(["--panel", csv_a, "--out", out_a])
        check("A: the CLI trains and writes", rc == 0 and os.path.exists(out_a), f"rc {rc}")
        print("\n".join("     " + ln for ln in text.splitlines()))
        res = train(read_a, parse_features(DEFAULT_FEATURES), None, True)
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
              and obj["n_games"] == 7500 and obj["brier"]["held_out"] == "2024-25"
              and obj["features"] == DEFAULT_FEATURES.split(",") and obj["coef"] == res["all_fit"]["coef"]
              and obj["brier"] == res["brier"] and obj["intercept"] == res["all_fit"]["intercept"])
        g_t = sorted({g["in"]["t_left_s"] for g in obj["golden"]})
        check("A: 10 golden rows spread over the inputs", len(obj["golden"]) == 10 and g_t == [60, 600, 1200, 1800, 2400]
              and {g["in"]["possession"] for g in obj["golden"]} == {-1, 0, 1}
              and min(g["p"] for g in obj["golden"]) < 0.3 < 0.7 < max(g["p"] for g in obj["golden"]),
              ", ".join(f"{g['p']:.3f}" for g in obj["golden"]))
        check("A: golden p is the trainer's predict, bit for bit",
              all(predict(obj, *(g["in"][k] for k in ("t_left_s", "margin", "possession", "p0"))) == g["p"]
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
        rc2, msg = run_quiet(["--panel", csv_a, "--out", out_a])
        check("A: an existing file is not overwritten without --force", rc2 == 2 and "exists" in msg)

        # -------- B: Stern's Brownian motion as the truth, 8 states per game, no kalshi_mid column
        rng = random.Random(7)
        rows_b, truth_b = synth_brownian(rng, ["2023-24", "2024-25", "2025-26"], 300)
        csv_b = os.path.join(d, "panel_b.csv")
        write_csv(csv_b, rows_b, False)
        out_b = os.path.join(d, "wcbb_logit_v1.json")
        feats_b = "logit_p0,margin_over_sd,logit_p0_x_tau,possession,tau"
        rc, text = run_quiet(["--panel", csv_b, "--out", out_b, "--features", feats_b, "--held-out", "2024-25"])
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

        # -------- the pieces
        ms = standardize([[3.0] * 5, [0.1] * 3, [1.0, 2.0, 3.0]])
        check("standardize: population sd, scale 1 for a constant column",
              ms[0] == (3.0, 1.0) and ms[1][1] == 1.0 and abs(ms[2][1] - math.sqrt(2.0 / 3.0)) < 1e-15, repr(ms))
        f = feature_row(list(FEATURES), 1200, 6, -1, 0.75)
        tau = 0.5
        want = [math.log(3.0), 6.0, 6.0 / math.sqrt(0.51), math.log(3.0) * tau, -1.0, -1.0 / math.sqrt(0.51), tau]
        check("feature_row matches the spec's formulas", all(abs(x - y) < 1e-15 for x, y in zip(f, want)))
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

    failed = results.count(False)
    print(f"train_wcbb self-test: {'ok' if not failed else f'FAILED {failed} of {len(results)}'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
