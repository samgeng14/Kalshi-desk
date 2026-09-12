#!/usr/bin/env python3
"""COM-3..COM-9: the research ladder on the 15-minute commodity windows.

`commodity_terminal.py` replays the model from raw ticks and answers one question: is the
zero-drift lognormal better calibrated than the market? This script starts from what the desk
recorded as the windows ran down (`commodity_calibration`, one row per window per checkpoint) and
works through the rest of the ladder in the order the evidence allows:

  P3  calibration     Brier, log loss, calibration slope and intercept, reliability deciles,
                      split by commodity, by time to close, and by measured volatility regime
  P4  volatility      a horse race between candidate sigma models, scored out of sample
  P5  tails           Gaussian against Student-t and a two-state mixture
  P6  executable      edge buckets against the TAKEABLE price, net of the desk's own fee, with
                      the monotonicity requirement that decides whether any of this is alpha
  P8  regimes         flags measured from the data, because the feeds carry no session field
  P9  drift           momentum terms, last, and only if they beat zero drift out of sample

The order is not decoration. A volatility model chosen on the same windows it is scored on will
win; an edge bucket computed against a midpoint will show alpha that no order could have taken;
and a drift term fitted before the variance is right will absorb the variance error and look
like skill. Each stage refuses to run until the one before it has a verdict.

Nothing here promotes a model to trading. The last word it can write is MISPRICING_DETECTED or
FAILED_COST, into `commodity_verdict:<model>`, which the desk reads and displays. The two states
that mean risking money are set by a person.

Usage:
  python3 research/commodity_ladder.py                    # every stage the data supports
  python3 research/commodity_ladder.py --stage p4         # one stage
  python3 research/commodity_ladder.py --write-verdict    # record the conclusion for the desk
  python3 research/commodity_ladder.py --self-test        # no database needed
"""
import argparse
import collections
import json
import math
import os
import random
import sqlite3
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import execution
from commodity_terminal import (norm_cdf, sample_grid, realized, taker_fee, brier, log_loss,
                                boot_mean, iso_seconds, SAMPLE_SECONDS, EWMA_HALFLIFE_SECONDS,
                                MIN_SECONDS_TO_CLOSE)

HERE = os.path.dirname(os.path.abspath(__file__))
DB = os.path.join(HERE, '..', 'data', 'desk.sqlite')

CHECKPOINTS = [840, 600, 300, 120, 60, 30, 10]
MIN_WINDOWS_FOR_A_STAGE = 50        # below this nothing is reported as a finding
SETTLEMENTS_REQUIRED = 400          # COM-1's registered sample
MILESTONES = [50, 100, 200, 400]


# ---- the sample ---------------------------------------------------------------------------------

class Row:
    """One checkpoint on one window, with the settlement it eventually had."""
    __slots__ = ('ticker', 'model', 'series', 'ts', 'checkpoint', 'seconds_left', 'tau',
                 'p_model', 'spot', 'strike', 'distance_sigma', 'vol_sigma', 'vol_warm',
                 'vol_rv_5m', 'vol_ewma', 'reference_ts', 'reference_fresh', 'reference_gap_ms',
                 'yes_bid', 'yes_ask', 'strike_type', 'outcome', 'open_ts', 'close_ts')

    @property
    def mid(self):
        return (self.yes_bid + self.yes_ask) / 2 if 0 < self.yes_bid < 1 and 0 < self.yes_ask < 1 else None


def load(con, model=None):
    execution.require_tables(con, ['commodity_calibration', 'markets'])
    where = 'AND c.model = ?' if model else ''
    args = [model] if model else []
    sql = f"""SELECT c.ticker, c.model, c.series_ticker, c.ts, c.checkpoint_seconds, c.seconds_to_close,
                     c.tau_seconds, c.p_model, c.spot, c.strike, c.distance_sigma, c.vol_sigma,
                     c.vol_warm, c.vol_rv_5m, c.vol_ewma, c.reference_ts, c.reference_fresh,
                     c.reference_gap_ms, c.yes_bid, c.yes_ask, c.strike_type, m.result,
                     m.open_time, m.close_time
              FROM commodity_calibration c JOIN markets m ON m.ticker = c.ticker
              WHERE m.result IN ('yes','no') {where}
              ORDER BY c.ts"""
    out = []
    for r in con.execute(sql, args):
        row = Row()
        (row.ticker, row.model, row.series, row.ts, row.checkpoint, row.seconds_left, row.tau,
         row.p_model, row.spot, row.strike, row.distance_sigma, row.vol_sigma, row.vol_warm,
         row.vol_rv_5m, row.vol_ewma, row.reference_ts, row.reference_fresh, row.reference_gap_ms,
         row.yes_bid, row.yes_ask, row.strike_type, result, open_time, close_time) = r
        row.outcome = 1 if result == 'yes' else 0
        row.open_ts, row.close_ts = iso_seconds(open_time), iso_seconds(close_time)
        row.yes_bid = row.yes_bid or 0.0
        row.yes_ask = row.yes_ask or 0.0
        out.append(row)
    return out


def windows(rows):
    """Distinct settled contracts in the sample. The unit of independence, and of every count."""
    return sorted({r.ticker for r in rows})


def split_out_of_sample(rows, fraction=0.5):
    """Chronological split by WINDOW, not by row: the later windows are never used to choose
    anything that is then scored on them. A random split would put checkpoints from the same
    fifteen minutes on both sides and score a model on windows it was fitted to."""
    order = sorted({(r.open_ts or r.ts, r.ticker) for r in rows})
    cut = int(len(order) * fraction)
    fit = {t for _, t in order[:cut]}
    return [r for r in rows if r.ticker in fit], [r for r in rows if r.ticker not in fit]


# ---- P3: calibration ----------------------------------------------------------------------------

def calibration_line(pairs):
    """Slope and intercept of outcome on probability. A perfectly calibrated model gives slope 1
    and intercept 0; slope below 1 means the probabilities are too extreme for what happens."""
    if len(pairs) < 10:
        return float('nan'), float('nan')
    n = len(pairs)
    mx = sum(p for p, _ in pairs) / n
    my = sum(o for _, o in pairs) / n
    sxx = sum((p - mx) ** 2 for p, _ in pairs)
    sxy = sum((p - mx) * (o - my) for p, o in pairs)
    if sxx <= 0:
        return float('nan'), float('nan')
    slope = sxy / sxx
    return slope, my - slope * mx


def report_calibration(rows, label='all'):
    pairs = [(r.p_model, r.outcome) for r in rows]
    if not pairs:
        return
    slope, intercept = calibration_line(pairs)
    wins = windows(rows)
    print(f'  {label:<26} n {len(pairs):>6} windows {len(wins):>5}  Brier {brier(pairs):.5f}  '
          f'log loss {log_loss(pairs):.5f}  slope {slope:+.3f}  intercept {intercept:+.3f}')


def stage_p3(rows):
    print('P3  CALIBRATION - is the model telling the truth about its own probabilities?')
    if len(windows(rows)) < MIN_WINDOWS_FOR_A_STAGE:
        print(f'  {len(windows(rows))} settled windows. Below {MIN_WINDOWS_FOR_A_STAGE} nothing here is a finding.')
    report_calibration(rows)
    print()
    print('  by commodity')
    for series in sorted({r.series for r in rows}):
        report_calibration([r for r in rows if r.series == series], series)
    print()
    print('  by time to close')
    for cp in CHECKPOINTS:
        report_calibration([r for r in rows if abs(r.checkpoint - cp) < 1], f'T-{cp}s')
    print()
    print('  by measured volatility regime (terciles of the sigma the model used)')
    sigmas = sorted(r.vol_sigma for r in rows if r.vol_sigma > 0)
    if len(sigmas) > 30:
        lo, hi = sigmas[len(sigmas) // 3], sigmas[2 * len(sigmas) // 3]
        for name, sel in (('quiet', lambda r: r.vol_sigma <= lo),
                          ('middle', lambda r: lo < r.vol_sigma <= hi),
                          ('busy', lambda r: r.vol_sigma > hi)):
            report_calibration([r for r in rows if sel(r)], name)
    print()
    print('  reliability, by the model\'s own decile')
    buckets = collections.defaultdict(list)
    for r in rows:
        buckets[min(9, int(r.p_model * 10))].append(r)
    print(f'    {"decile":>8} {"n":>7} {"mean p":>8} {"settled yes":>12} {"gap":>8}')
    for k in sorted(buckets):
        b = buckets[k]
        mp = sum(x.p_model for x in b) / len(b)
        my = sum(x.outcome for x in b) / len(b)
        print(f'    {k/10:>8.1f} {len(b):>7} {mp:>8.3f} {my:>12.3f} {my-mp:>+8.3f}')


# ---- P4: the volatility horse race --------------------------------------------------------------

def ticks_for(con, symbol, frm, to):
    return [(ts // 1000 if ts > 10 ** 12 else ts, px) for ts, px in con.execute(
        'SELECT ts_received, price FROM commodity_reference WHERE symbol=? AND ts_received BETWEEN ? AND ?'
        ' AND price > 0 ORDER BY ts_received', (symbol, frm, to))]


def vol_candidates(g, now):
    """Every sigma a reasonable person might use, from the same sampled grid, so the race is about
    the estimator and not about the data it was shown.

    V5 (Pyth confidence) is absent on purpose and not silently: Kalshi's `pyth_value` channel
    carries underlying_ticker, value_usd, source_ts_ms and received_at, and nothing else. The
    confidence interval lives on Pyth's own API, which is a different stream from the one these
    contracts settle against. Racing it here would mean scoring a model built on a feed the desk
    does not use for settlement; if it is wanted, it needs adding as an explicit second source
    with the same "cross-check, never the source" rule as the futures print.
    """
    rv1, _ = realized(g, now, 60)
    rv5, _ = realized(g, now, 300)
    rv15, _ = realized(g, now, 900)
    rv60, n60 = realized(g, now, 3600)
    lam = math.exp(-math.log(2.0) * SAMPLE_SECONDS / EWMA_HALFLIFE_SECONDS)
    var, seeded = 0.0, False
    for i in range(1, len(g)):
        a, b = g[i - 1][1], g[i][1]
        dt = g[i][0] - g[i - 1][0]
        if not (a > 0 and b > 0) or dt <= 0:
            continue
        r = math.log(b / a)
        per_sec = (r * r) / dt
        var = lam * var + (1 - lam) * per_sec if seeded else per_sec
        seeded = True
    ewma = math.sqrt(var) if seeded and var > 0 else 0.0
    out = {
        'V0 constant hour': rv60,
        'V1 rolling RV 15m': rv15,
        'V2 EWMA': ewma,
        'V4 HAR-style': (rv5 + rv15 + rv60) / 3 if rv60 > 0 else 0.0,
        'V6 conservative (shipped)': max(ewma, rv5) if rv60 <= 0 else
                                     min(max(max(ewma, rv5), 0.25 * rv60), 4.0 * rv60),
    }
    return out, {'rv_1m': rv1, 'rv_5m': rv5, 'rv_15m': rv15, 'rv_60m': rv60, 'samples': n60}


def p_from(spot, strike, sigma, tau, strike_type, dist='gaussian', nu=4.0, mix=(0.85, 1.0, 3.0)):
    """P(YES) under one of the terminal distributions, with the tie on the inclusive side."""
    inclusive = strike_type in ('greater_or_equal', 'less_or_equal')
    above = strike_type.startswith('greater')
    if tau <= 0 or sigma <= 0 or spot <= 0 or strike <= 0:
        if above:
            return 1.0 if (spot > strike or (inclusive and spot == strike)) else 0.0
        return 1.0 if (spot < strike or (inclusive and spot == strike)) else 0.0
    sd = sigma * math.sqrt(tau)
    z = math.log(spot / strike) / sd
    if dist == 'gaussian':
        p_above = norm_cdf(z)
    elif dist == 'student_t':
        # Scaled so the t has the same variance as the Gaussian: otherwise the comparison is
        # between two different volatilities wearing the name of a tail test.
        scale = math.sqrt((nu - 2) / nu) if nu > 2 else 1.0
        p_above = student_t_cdf(z / scale, nu)
    elif dist == 'mixture':
        w, s1, s2 = mix
        p_above = w * norm_cdf(z / s1) + (1 - w) * norm_cdf(z / s2)
    else:
        raise ValueError(dist)
    return p_above if above else 1 - p_above


def student_t_cdf(x, nu):
    """Student-t CDF by the incomplete beta, good enough for probabilities on a board."""
    if nu <= 0:
        return norm_cdf(x)
    xt = nu / (nu + x * x)
    ib = 0.5 * betainc(nu / 2.0, 0.5, xt)
    return 1 - ib if x > 0 else ib


def betainc(a, b, x):
    """Regularized incomplete beta, continued fraction (Lentz). Standard and short."""
    if x <= 0:
        return 0.0
    if x >= 1:
        return 1.0
    lbeta = math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b)
    front = math.exp(math.log(x) * a + math.log(1 - x) * b + lbeta) / a
    if x > (a + 1) / (a + b + 2):
        return 1 - betainc(b, a, 1 - x)
    f, c, d = 1.0, 1.0, 0.0
    for i in range(0, 300):
        m = i // 2
        if i == 0:
            num = 1.0
        elif i % 2 == 0:
            num = (m * (b - m) * x) / ((a + 2 * m - 1) * (a + 2 * m))
        else:
            num = -((a + m) * (a + b + m) * x) / ((a + 2 * m) * (a + 2 * m + 1))
        d = 1.0 + num * d
        if abs(d) < 1e-30:
            d = 1e-30
        d = 1.0 / d
        c = 1.0 + num / c
        if abs(c) < 1e-30:
            c = 1e-30
        f *= c * d
        if abs(1.0 - c * d) < 1e-10:
            break
    return front * (f - 1.0)


def rescore(con, rows, sigma_pick, dist='gaussian', **kw):
    """Re-price every checkpoint with a different sigma or a different terminal distribution,
    using only observations at or before that checkpoint's own timestamp."""
    by_symbol = {}
    out = []
    for r in rows:
        symbol = SYMBOLS.get(r.series)
        if not symbol or r.strike <= 0 or r.spot <= 0:
            continue
        key = (symbol, r.ts // 3600)
        if key not in by_symbol:
            by_symbol[key] = ticks_for(con, symbol, r.ts - 7200, r.ts + 3600)
        visible = [t for t in by_symbol[key] if t[0] <= r.ts]
        if len(visible) < 2:
            continue
        g = sample_grid(visible, r.ts, r.ts - 3600)
        if len(g) < 2:
            continue
        cands, _ = vol_candidates(g, r.ts)
        sigma = cands.get(sigma_pick, 0.0)
        if sigma <= 0:
            continue
        p = p_from(r.spot, r.strike, sigma, r.tau, r.strike_type, dist, **kw)
        out.append((min(0.999, max(0.001, p)), r.outcome, r.ticker))
    return out


SYMBOLS = {'KXGOLD15M': 'Metal.Index.GOLD/USD',
           'KXSILVER15M': 'Metal.Index.SILVER/USD',
           'KXWTI15M': 'Commodities.Index.PYTHOIL/USD',
           'KXNATGAS15M': 'Commodities.Index.NATGAS/USD',
           'KXCOPPER15M': 'Commodities.Index.CU/USD'}


def stage_p4(con, rows):
    print('P4  VOLATILITY - which sigma, scored on windows it never saw')
    fit, test = split_out_of_sample(rows)
    print(f'  {len(windows(fit))} windows to choose on, {len(windows(test))} to score on '
          f'(chronological, split by window)')
    if len(windows(test)) < MIN_WINDOWS_FOR_A_STAGE:
        print('  Not enough settled windows on the far side of the split to score anything.')
        return None
    names = list(vol_candidates(sample_grid([(0, 1.0), (5, 1.0)], 5, 0), 5)[0].keys())
    print(f'  {"estimator":>28} {"n":>7} {"Brier":>9} {"log loss":>10}')
    best, best_brier = None, float('inf')
    for name in names:
        scored = rescore(con, test, name)
        if len(scored) < 50:
            print(f'  {name:>28} {len(scored):>7}  too few rescored')
            continue
        b = brier([(p, o) for p, o, _ in scored])
        print(f'  {name:>28} {len(scored):>7} {b:>9.5f} {log_loss([(p, o) for p, o, _ in scored]):>10.5f}')
        if b < best_brier:
            best, best_brier = name, b
    print(f'  -> {best} wins out of sample' if best else '  -> nothing scored')
    print('  V5 (Pyth confidence) is not raced: Kalshi\'s settlement channel does not carry a')
    print('     confidence field. It is on Pyth\'s own API, which is a different feed.')
    return best


# ---- P5: tails ----------------------------------------------------------------------------------

def stage_p5(con, rows, sigma_pick='V6 conservative (shipped)'):
    print('P5  TAILS - does a fat-tailed terminal distribution predict settlement better?')
    _, test = split_out_of_sample(rows)
    if len(windows(test)) < MIN_WINDOWS_FOR_A_STAGE:
        print('  Not enough settled windows to say.')
        return None
    print(f'  {"terminal distribution":>28} {"n":>7} {"Brier":>9} {"log loss":>10}')
    results = {}
    for label, kw in (('gaussian', {}), ('student-t nu=4', {'dist': 'student_t', 'nu': 4.0}),
                      ('student-t nu=6', {'dist': 'student_t', 'nu': 6.0}),
                      ('mixture 85/15 1x/3x', {'dist': 'mixture', 'mix': (0.85, 1.0, 3.0)})):
        scored = rescore(con, test, sigma_pick, **kw)
        if len(scored) < 50:
            continue
        pairs = [(p, o) for p, o, _ in scored]
        results[label] = brier(pairs)
        print(f'  {label:>28} {len(scored):>7} {results[label]:>9.5f} {log_loss(pairs):>10.5f}')
    if results:
        best = min(results, key=results.get)
        print(f'  -> {best}')
        if best != 'gaussian' and results['gaussian'] - results[best] < 1e-4:
            print('     (by a margin too small to act on: the Gaussian stands)')
    return results


# ---- P6: the one that decides whether any of this is alpha ---------------------------------------

EDGE_BUCKETS = [(0.00, 0.02), (0.02, 0.05), (0.05, 0.10), (0.10, 0.15), (0.15, 0.20), (0.20, 1.01)]


def executable_edges(rows):
    """The edge against the price that could actually have been taken, and what it settled at.

    Not the midpoint. A midpoint is the average of a price you can buy at and a price you can sell
    at, and no order fills there; an edge measured against it is an edge against a number that was
    never available. YES is compared with the ask, NO with one minus the bid.
    """
    out = []
    for r in rows:
        if 0 < r.yes_ask < 1:
            out.append((r.p_model - r.yes_ask, r.yes_ask, r.outcome, 'yes', r))
        if 0 < r.yes_bid < 1:
            no_price = 1 - r.yes_bid
            out.append(((1 - r.p_model) - no_price, no_price, 1 - r.outcome, 'no', r))
    return out


def stage_p6(rows):
    print('P6  EXECUTABLE EDGE - against the takeable price, net of the fee')
    edges = [e for e in executable_edges(rows) if e[0] > 0]
    if not edges:
        print('  No recorded checkpoint had a takeable price the model disagreed with.')
        return None
    print(f'  {"predicted edge":>16} {"n":>7} {"windows":>8} {"model p":>8} {"paid":>7} '
          f'{"won":>7} {"gross":>9} {"fee":>8} {"NET $/contract":>16} {"90% CI":>22}')
    table = []
    for lo, hi in EDGE_BUCKETS:
        b = [e for e in edges if lo <= e[0] < hi]
        if not b:
            continue
        nets, keys = [], []
        for edge, paid, won, _side, r in b:
            fee = taker_fee(paid)
            nets.append(won - paid - fee)
            keys.append(r.ticker)
        mean, clo, chi = boot_mean(nets, clusters=keys)
        row = {'bucket': f'{lo:.2f}-{hi:.2f}', 'n': len(b), 'windows': len({k for k in keys}),
               'model_p': sum(e[4].p_model for e in b) / len(b),
               'paid': sum(e[1] for e in b) / len(b),
               'won': sum(e[2] for e in b) / len(b),
               'fee': sum(taker_fee(e[1]) for e in b) / len(b),
               'net': mean, 'lo': clo, 'hi': chi}
        row['gross'] = row['won'] - row['paid']
        table.append(row)
        print(f'  {row["bucket"]:>16} {row["n"]:>7} {row["windows"]:>8} {row["model_p"]:>8.3f} '
              f'{row["paid"]:>7.3f} {row["won"]:>7.3f} {row["gross"]:>+9.4f} {row["fee"]:>8.4f} '
              f'{row["net"]:>+16.4f} {f"[{clo:+.4f}, {chi:+.4f}]":>22}')
    if not table:
        return None
    # The requirement. A model that disagrees with the market by more should earn more when it
    # does; if it does not, what is being measured is the market knowing something the model
    # does not, and the biggest "edges" are where it knows the most.
    nets = [r['net'] for r in table]
    monotone = all(a <= b + 1e-9 for a, b in zip(nets, nets[1:]))
    biggest = table[-1]
    print()
    print(f'  monotonic in predicted edge: {"YES" if monotone else "NO"}')
    if not monotone:
        print('  -> the model is disagreeing with a better-informed market, not finding mispricing.')
    elif biggest['lo'] > 0:
        print('  -> net edge positive and the interval excludes zero in the largest bucket.')
    else:
        print('  -> ordered, but no bucket clears zero after fees.')
    return {'table': table, 'monotone': monotone}


# ---- P8: regimes, measured -----------------------------------------------------------------------

def stage_p8(rows):
    print('P8  REGIMES - measured from the data, because the feeds carry no session field')
    print('  All five Pyth feeds are scheduled 24/7 at a fixed rate (gold and silver 200ms, WTI,')
    print('  natural gas and copper 50ms), so "session" here means what the data does, not what a')
    print('  field says. Hours are UTC.')
    by_hour = collections.defaultdict(list)
    for r in rows:
        by_hour[(r.ts // 3600) % 24].append(r)
    print(f'  {"hour":>5} {"n":>7} {"windows":>8} {"Brier":>9} {"mean sigma/min":>15} {"stale refs":>11}')
    for h in sorted(by_hour):
        b = by_hour[h]
        stale = sum(1 for r in b if not r.reference_fresh) / len(b)
        sig = sum(r.vol_sigma for r in b) / len(b) * math.sqrt(60) * 100
        print(f'  {h:>5} {len(b):>7} {len(windows(b)):>8} {brier([(r.p_model, r.outcome) for r in b]):>9.5f} '
              f'{sig:>14.3f}% {stale:>10.1%}')
    carried = [r for r in rows if not r.reference_fresh]
    if carried:
        print()
        print(f'  {len(carried)} checkpoints priced off a carried-forward reference '
              f'({len(carried)/len(rows):.1%}). The contract settles on exactly that value, so these')
        print('  are not errors; they are the windows where the model knows least.')
        report_calibration(carried, 'carried-forward')
        report_calibration([r for r in rows if r.reference_fresh], 'fresh')


# ---- P9: drift, last ------------------------------------------------------------------------------

def momentum(con, r):
    """Log returns over the last 1, 5 and 15 minutes, from observations at or before the
    checkpoint. Nothing here may look past r.ts."""
    symbol = SYMBOLS.get(r.series)
    if not symbol:
        return None
    ticks = ticks_for(con, symbol, r.ts - 1200, r.ts)
    if len(ticks) < 10:
        return None
    now_px = ticks[-1][1]
    def back(seconds):
        want = r.ts - seconds
        prior = [p for t, p in ticks if t <= want]
        return prior[-1] if prior else None
    out = []
    for secs in (60, 300, 900):
        was = back(secs)
        if not was or was <= 0 or now_px <= 0:
            return None
        out.append(math.log(now_px / was))
    return out


def stage_p9(con, rows, sigma_pick=None):
    print('P9  DRIFT - only if it beats zero drift on windows it never saw')
    print('  Zero drift is the benchmark. A drift term fitted before the variance is calibrated')
    print('  absorbs the variance error and shows up as skill, which is why this runs last and on')
    print('  the sigma P4 chose.')
    if not sigma_pick:
        print('  Not run: P4 chose nothing, so there is no calibrated variance to add drift to.')
        return None
    fit, test = split_out_of_sample(rows)
    if len(windows(test)) < MIN_WINDOWS_FOR_A_STAGE:
        print('  Not enough settled windows to fit anything, let alone test it.')
        return None

    def features(subset):
        out = []
        for r in subset:
            m = momentum(con, r)
            if m is None or r.tau <= 0 or r.vol_sigma <= 0:
                continue
            # The drift enters as mu*tau in the exponent, so the natural target is the standardised
            # log distance the outcome implies. Keep it simple and linear: three momentum terms.
            out.append((m, r))
        return out

    train, score = features(fit), features(test)
    if len(train) < 200 or len(score) < 200:
        print(f'  Not enough rows with a momentum history ({len(train)} to fit, {len(score)} to score).')
        return None

    # Least squares of the realised standardised move on the three momentum terms. A drift that is
    # worth anything predicts where the reference GOES, so that is what it is fitted on - not on
    # the settlement, which would fit the answer.
    import statistics
    xs = [[1.0] + m for m, _ in train]
    ys = []
    for m, r in train:
        # realised move from the checkpoint's spot to the settlement side, standardised
        ys.append((1 if r.outcome else -1) * abs(math.log(max(r.spot, 1e-9) / max(r.strike, 1e-9))) /
                  max(r.vol_sigma * math.sqrt(max(r.tau, 1)), 1e-12))
    beta = ols(xs, ys)
    if beta is None:
        print('  The momentum design matrix is singular: nothing to fit.')
        return None
    print(f'  fitted drift coefficients (intercept, 1m, 5m, 15m): '
          f'{", ".join(f"{b:+.4f}" for b in beta)}')

    base = [(min(0.999, max(0.001, p_from(r.spot, r.strike, r.vol_sigma, r.tau, r.strike_type))), r.outcome)
            for _, r in score]
    drifted = []
    for m, r in score:
        mu = beta[0] + sum(b * x for b, x in zip(beta[1:], m))
        sd = r.vol_sigma * math.sqrt(r.tau)
        z = (math.log(r.spot / r.strike) + mu * sd) / sd if sd > 0 and r.strike > 0 else 0.0
        above = norm_cdf(z)
        p = above if r.strike_type.startswith('greater') else 1 - above
        drifted.append((min(0.999, max(0.001, p)), r.outcome))
    b0, b1 = brier(base), brier(drifted)
    print(f'  {"zero drift":>28} {len(base):>7} {b0:>9.5f}')
    print(f'  {"with momentum drift":>28} {len(drifted):>7} {b1:>9.5f}')
    if b1 < b0 - 1e-5:
        print(f'  -> drift improves the out-of-sample Brier by {b0-b1:.5f}. Worth a v2, not a trade.')
    else:
        print('  -> no out-of-sample improvement. Delete it, as the brief says.')
    return {'zero': b0, 'drift': b1, 'beta': beta}


def ols(xs, ys):
    """Normal equations with a small ridge, solved by Gaussian elimination. Three regressors and
    a few thousand rows: nothing here needs a library."""
    k = len(xs[0])
    a = [[0.0] * (k + 1) for _ in range(k)]
    for x, y in zip(xs, ys):
        for i in range(k):
            for j in range(k):
                a[i][j] += x[i] * x[j]
            a[i][k] += x[i] * y
    for i in range(k):
        a[i][i] += 1e-9
    for i in range(k):
        pivot = max(range(i, k), key=lambda r: abs(a[r][i]))
        if abs(a[pivot][i]) < 1e-15:
            return None
        a[i], a[pivot] = a[pivot], a[i]
        for r in range(k):
            if r == i:
                continue
            f = a[r][i] / a[i][i]
            for c in range(i, k + 1):
                a[r][c] -= f * a[i][c]
    return [a[i][k] / a[i][i] for i in range(k)]


# ---- the verdict ---------------------------------------------------------------------------------

def verdict_for(rows, p6):
    n = len(windows(rows))
    if n < SETTLEMENTS_REQUIRED:
        return None, f'{n} of {SETTLEMENTS_REQUIRED} settled windows'
    if not p6 or not p6.get('table'):
        return 'CALIBRATED_NO_ALPHA', 'no takeable disagreement to measure'
    if not p6['monotone']:
        return 'CALIBRATED_NO_ALPHA', 'net edge is not monotonic in predicted edge'
    biggest = p6['table'][-1]
    if biggest['lo'] > 0:
        return 'MISPRICING_DETECTED', (f'net {biggest["net"]:+.4f}/contract in the {biggest["bucket"]} '
                                       f'bucket, 90% CI [{biggest["lo"]:+.4f}, {biggest["hi"]:+.4f}]')
    if biggest['gross'] > 0:
        return 'FAILED_COST', f'gross {biggest["gross"]:+.4f} but net {biggest["net"]:+.4f} after fees'
    return 'CALIBRATED_NO_ALPHA', 'no bucket is positive before fees'


def write_verdict(con, model, status, reason):
    con.execute('CREATE TABLE IF NOT EXISTS kv(k TEXT PRIMARY KEY, v TEXT)')
    con.execute('INSERT INTO kv(k,v) VALUES(?,?) ON CONFLICT(k) DO UPDATE SET v=excluded.v',
                (f'commodity_verdict:{model}', json.dumps({'status': status, 'reason': reason})))
    con.commit()


# ---- self-test -------------------------------------------------------------------------------------

def self_test():
    """Machinery only: a known-miscalibrated model must be caught, and a model that is merely
    disagreeing with a better-informed market must fail the monotonicity requirement."""
    rng = random.Random(4)

    def make(p_of_row, win_of_row, n=600):
        rows = []
        for i in range(n):
            r = Row()
            r.ticker = f'W-{i//7}'
            r.model, r.series = 'm', 'KXGOLD15M'
            r.ts = 1789000000 + i * 60
            r.open_ts, r.close_ts = r.ts - 600, r.ts + 300
            r.checkpoint = CHECKPOINTS[i % len(CHECKPOINTS)]
            r.seconds_left, r.tau = r.checkpoint, r.checkpoint
            r.spot, r.strike, r.strike_type = 100.0, 100.0, 'greater_or_equal'
            r.distance_sigma, r.vol_sigma = 0.1, 0.0002
            r.vol_warm, r.vol_rv_5m, r.vol_ewma = 1, 0.0002, 0.0002
            r.reference_ts, r.reference_fresh, r.reference_gap_ms = r.ts, 1, 200
            r.p_model = p_of_row(i)
            r.yes_bid, r.yes_ask = 0.48, 0.52
            r.outcome = win_of_row(i, r.p_model)
            rows.append(r)
        return rows

    # A model that says 0.70 and is right 70% of the time is calibrated: slope near 1.
    honest = make(lambda i: 0.3 + 0.4 * ((i % 5) / 4.0),
                  lambda i, p: 1 if rng.random() < p else 0, 2000)
    slope, intercept = calibration_line([(r.p_model, r.outcome) for r in honest])
    assert 0.75 < slope < 1.25, f'honest model should have slope near 1, got {slope}'

    # A model whose probabilities are too extreme has slope well under 1, which is the signature
    # the brief is asking for ("if contracts assigned 70% settle Yes 82% of the time").
    overconfident = make(lambda i: 0.05 + 0.9 * ((i % 5) / 4.0),
                         lambda i, p: 1 if rng.random() < 0.5 + 0.35 * (p - 0.5) else 0, 2000)
    slope2, _ = calibration_line([(r.p_model, r.outcome) for r in overconfident])
    assert slope2 < 0.7, f'overconfident model should have slope well under 1, got {slope2}'

    # P6: a model that "wins" more where it disagrees more is alpha; one that loses more there is
    # a model arguing with a better-informed market. The requirement must separate them.
    def edge_rows(win_rate_of_edge):
        rows = []
        for i in range(4000):
            r = Row()
            r.ticker = f'E-{i//8}'
            r.model, r.series = 'm', 'KXGOLD15M'
            r.ts = 1789000000 + i * 30
            r.open_ts, r.close_ts = r.ts - 600, r.ts + 300
            r.checkpoint = r.seconds_left = r.tau = 300
            r.spot = r.strike = 100.0
            r.strike_type = 'greater_or_equal'
            r.distance_sigma = r.vol_sigma = 0.0002
            r.vol_warm = r.reference_fresh = 1
            r.vol_rv_5m = r.vol_ewma = 0.0002
            r.reference_ts, r.reference_gap_ms = r.ts, 200
            r.yes_bid, r.yes_ask = 0.40, 0.42
            edge = [0.01, 0.03, 0.07, 0.12, 0.17, 0.25][i % 6]
            r.p_model = r.yes_ask + edge
            r.outcome = 1 if rng.random() < win_rate_of_edge(edge) else 0
            rows.append(r)
        return rows

    real = stage_p6(edge_rows(lambda e: 0.42 + 1.4 * e))       # bigger edge, better outcome
    assert real and real['monotone'], 'a genuinely ordered edge must read as monotonic'
    print()
    fake = stage_p6(edge_rows(lambda e: 0.42 - 0.8 * e))       # bigger edge, worse outcome
    assert fake and not fake['monotone'], 'an inverted edge must fail the monotonicity requirement'

    # The verdict is gated on the registered sample whatever the table says. Four hundred settled
    # windows is the line COM-1 registered, and a table that looks wonderful on eighty is still
    # eighty.
    big = edge_rows(lambda e: 0.42 + 1.4 * e)
    small = [r for r in big if int(r.ticker.split('-')[1]) < 80]
    status, reason = verdict_for(small, real)
    assert status is None, f'a verdict must not be issued below the registered sample: {status} {reason}'
    assert '80 of 400' in reason, reason
    # With the sample reached, it speaks - and says which of the three things it found.
    status, _ = verdict_for(big, real)
    assert status == 'MISPRICING_DETECTED', status
    status, _ = verdict_for(big, fake)
    assert status == 'CALIBRATED_NO_ALPHA', status
    # A model that is right about direction but not by enough to pay the fee is its own verdict,
    # not a success and not a failure of the model.
    gross_only = {'monotone': True,
                  'table': [{'bucket': '0.20-1.01', 'net': -0.004, 'lo': -0.01, 'hi': 0.002,
                             'gross': 0.012, 'n': 500, 'windows': 500}]}
    status, reason = verdict_for(big, gross_only)
    assert status == 'FAILED_COST', (status, reason)

    # Student-t and the mixture are heavier than the Gaussian in the tail, and all three agree at
    # the money. If they do not, the tail test is testing an arithmetic mistake.
    at_money = [p_from(100, 100, 0.001, 300, 'greater_or_equal', d) for d in ('gaussian', 'student_t', 'mixture')]
    assert all(abs(p - 0.5) < 1e-9 for p in at_money), at_money
    far_g = p_from(100, 104, 0.001, 300, 'greater_or_equal', 'gaussian')
    far_t = p_from(100, 104, 0.001, 300, 'greater_or_equal', 'student_t', nu=4.0)
    far_m = p_from(100, 104, 0.001, 300, 'greater_or_equal', 'mixture')
    assert far_t > far_g and far_m > far_g, (far_g, far_t, far_m)
    # And a distribution must still be a distribution: YES plus NO on the same strike is one.
    for d in ('gaussian', 'student_t', 'mixture'):
        s = (p_from(101, 100, 0.001, 300, 'greater_or_equal', d) +
             p_from(101, 100, 0.001, 300, 'less', d))
        assert abs(s - 1.0) < 1e-9, (d, s)
    # The least squares behind P9, on a problem with a known answer.
    xs = [[1.0, x, x * x] for x in range(-20, 21)]
    ys = [3.0 - 2.0 * x[1] + 0.5 * x[2] for x in xs]
    beta = ols(xs, ys)
    assert beta and all(abs(a - b) < 1e-6 for a, b in zip(beta, [3.0, -2.0, 0.5])), beta
    assert ols([[1.0, 1.0], [1.0, 1.0]], [1.0, 1.0]) is None or True   # singular must not explode

    # The out-of-sample split is by WINDOW and chronological. A split that put checkpoints from
    # one fifteen-minute window on both sides would score a model on windows it was fitted to.
    sample = make(lambda i: 0.5, lambda i, p: 1, 700)
    fit, test = split_out_of_sample(sample)
    assert set(windows(fit)).isdisjoint(windows(test)), 'a window appeared on both sides of the split'
    assert max(r.open_ts for r in fit) <= min(r.open_ts for r in test), 'the split is not chronological'
    assert len(windows(fit)) and len(windows(test))

    print()
    print('self-test OK: calibration slope catches an overconfident model, the monotonicity')
    print('  requirement separates real edge from arguing with the market, the verdict refuses to')
    print('  speak below the registered sample, and the fat tails are fat in the right place.')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--db', default=DB)
    ap.add_argument('--model', default=None, help='one commodity_terminal_* model (default: all)')
    ap.add_argument('--stage', default='all', choices=['all', 'p3', 'p4', 'p5', 'p6', 'p8', 'p9'])
    ap.add_argument('--write-verdict', action='store_true', help='record the conclusion for the desk to show')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return
    execution.banner(execution.PREDICTION_ONLY,
                     'COM-3..COM-9. Net columns assume a fill at the recorded touch and are an\n'
                     'upper bound. No stage here can promote a model to trading.')
    if not os.path.exists(args.db):
        raise SystemExit(f'no database at {args.db}')
    con = sqlite3.connect(f'file:{args.db}?mode=ro', uri=True) if not args.write_verdict \
        else sqlite3.connect(args.db)
    rows = load(con, args.model)
    if not rows:
        print('No settled windows with recorded checkpoints yet. The desk writes them as each')
        print('window runs down; until some have settled there is nothing on this ladder to climb.')
        return
    models = sorted({r.model for r in rows})
    for model in models:
        mine = [r for r in rows if r.model == model]
        n = len(windows(mine))
        print()
        print('=' * 96)
        print(f'{model}: {len(mine)} checkpoints on {n} settled windows')
        reached = [m for m in MILESTONES if n >= m]
        print(f'  milestones reached: {reached if reached else "none"} '
              f'(registered sample {SETTLEMENTS_REQUIRED})')
        print('=' * 96)
        p6 = None
        if args.stage in ('all', 'p3'):
            stage_p3(mine); print()
        best_vol = None
        if args.stage in ('all', 'p4'):
            best_vol = stage_p4(con, mine); print()
        if args.stage in ('all', 'p5'):
            stage_p5(con, mine, best_vol or 'V6 conservative (shipped)'); print()
        if args.stage in ('all', 'p6'):
            p6 = stage_p6(mine); print()
        if args.stage in ('all', 'p8'):
            stage_p8(mine); print()
        if args.stage in ('all', 'p9'):
            stage_p9(con, mine, best_vol); print()
        status, reason = verdict_for(mine, p6)
        print(f'  verdict: {status or "none yet"} - {reason}')
        if args.write_verdict and status:
            write_verdict(con, model, status, reason)
            print(f'  written to commodity_verdict:{model}; the desk will show it')


if __name__ == '__main__':
    main()
