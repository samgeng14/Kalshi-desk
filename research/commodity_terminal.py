#!/usr/bin/env python3
"""COM-1..COM-4: does a terminal-price distribution price Kalshi's 15-minute commodity strikes?

The model under test makes one claim and no others. At any moment inside a fifteen-minute window,
the settlement reference S_t is known, the strike K is known, and the contract pays when S_T is on
the right side of K at the close. If log(S_T/S_t) is normal with zero drift and a standard
deviation of sigma*sqrt(tau), every strike on that underlying is priced by one number. It does not
predict whether gold goes up. It says how far gold can travel in the time that is left.

This script replays that model against what the desk actually recorded, and compares it with what
Kalshi was charging at the same instant.

NO LOOK-AHEAD. Every probability is computed from reference observations stamped at or before its
own decision time, and the decision times are the market quote snapshots themselves. The replay
refuses to read a tick it could not have had: `--self-test` proves it by moving the reference 5%
from the middle of a window on, and checking that not one earlier probability changes while the
later ones do.

This is a PREDICTION-ONLY study (research/execution.py). The Brier comparison is its result. The
return column is what a taker would have paid at the recorded ask; it assumes a fill at the touch
and is not evidence that the order would have filled, so it is reported as an upper bound and must
not be quoted as a tradable edge.

Usage:
  python3 research/commodity_terminal.py                  # the desk database
  python3 research/commodity_terminal.py --db path.sqlite
  python3 research/commodity_terminal.py --self-test      # no database needed
"""
import argparse
import bisect
import collections
import math
import os
import random
import sqlite3
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import execution

HERE = os.path.dirname(os.path.abspath(__file__))
DB = os.path.join(HERE, '..', 'data', 'desk.sqlite')

# The four underlyings this model was built for, and the feed symbol patterns that identify them.
# Kept in step with CommodityTerminal::default_underlyings() in src/strategies/commodity_terminal.cpp.
UNDERLYINGS = {
    'KXGOLD15M': ('gold', ('metal.index.gold', 'gold', 'xau')),
    'KXSILVER15M': ('silver', ('metal.index.silver', 'silver', 'xag')),
    'KXWTI15M': ('wti', ('pythoil', 'wti', 'oil')),
    'KXNATGAS15M': ('natgas', ('natgas', 'gas')),
}

# Mirrors VolSettings in include/desk/models/terminal_price.h. If those defaults change, these
# must change with them or the replay is scoring a different model from the one that trades.
SAMPLE_SECONDS = 5
EWMA_HALFLIFE_SECONDS = 600
SHORT_FLOOR_MULTIPLIER = 1.0
MIN_SAMPLES = 60
MIN_SPAN_SECONDS = 300
MAX_RATIO_TO_HOUR = 4.0
MIN_RATIO_TO_HOUR = 0.25

SIGMA_BUCKETS = [(0, 0.5), (0.5, 1.0), (1.0, 1.5), (1.5, 2.5), (2.5, float('inf'))]

# The live model's own limits (CommodityTerminal in src/strategies/commodity_terminal.cpp). A
# replay scored outside them is scoring decisions the desk would never have made: it would not
# price a window with 10 seconds left, and it clamps what it publishes.
MIN_SECONDS_TO_CLOSE = 20
MAX_SECONDS_TO_CLOSE = 7200
REFERENCE_STALE_AFTER = 20
P_CLAMP = (0.001, 0.999)


# ---- the model, exactly as the desk computes it ------------------------------------------------

def norm_cdf(x):
    return 0.5 * math.erfc(-x / math.sqrt(2.0))


def sample_grid(ticks, now, frm, step=SAMPLE_SECONDS):
    """Last observed price on a fixed grid. `ticks` is [(ts, price)] sorted, ts <= now only."""
    out, i, last, have = [], 0, 0.0, False
    t = frm - (frm % step)
    while t <= now:
        while i < len(ticks) and ticks[i][0] <= t:
            if ticks[i][1] > 0:
                last, have = ticks[i][1], True
            i += 1
        if have and t >= frm:
            out.append((t, last))
        t += step
    return out


def realized(g, now, window):
    sum2, span, n = 0.0, 0, 0
    for i in range(1, len(g)):
        if g[i][0] < now - window:
            continue
        a, b = g[i - 1][1], g[i][1]
        dt = g[i][0] - g[i - 1][0]
        if not (a > 0 and b > 0) or dt <= 0:
            continue
        r = math.log(b / a)
        sum2 += r * r
        span += dt
        n += 1
    if n < 2 or span <= 0:
        return 0.0, n
    return math.sqrt(sum2 / span), n


def estimate_sigma(ticks, now):
    """Per-sqrt-second volatility, and whether it is warm. Returns (sigma, warm, detail)."""
    g = sample_grid(ticks, now, now - 3600)
    if len(g) < 2:
        return 0.0, False, {'reason': 'fewer than two sampled observations'}
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
    sigma = max(ewma, SHORT_FLOOR_MULTIPLIER * rv5)
    if rv60 > 0:
        sigma = min(sigma, MAX_RATIO_TO_HOUR * rv60)
        sigma = max(sigma, MIN_RATIO_TO_HOUR * rv60)
    span = g[-1][0] - g[0][0]
    warm = n60 >= MIN_SAMPLES and span >= MIN_SPAN_SECONDS and sigma > 0
    return sigma, warm, {'ewma': ewma, 'rv_1m': rv1, 'rv_5m': rv5, 'rv_15m': rv15, 'rv_60m': rv60,
                         'samples': n60, 'span_seconds': span}


def p_yes(spot, strike, sigma, tau, strike_type):
    """P(the contract pays YES), zero drift. The tie belongs to YES on greater_or_equal, which at
    tau = 0 is the whole answer and at tau > 0 is a set of measure zero."""
    inclusive = strike_type in ('greater_or_equal', 'less_or_equal')
    if tau <= 0 or sigma <= 0:
        if strike_type.startswith('greater'):
            return 1.0 if (spot > strike or (inclusive and spot == strike)) else 0.0
        return 1.0 if (spot < strike or (inclusive and spot == strike)) else 0.0
    sd = sigma * math.sqrt(tau)
    z = math.log(spot / strike) / sd
    above = norm_cdf(z)
    return above if strike_type.startswith('greater') else 1 - above


def taker_fee(price):
    """Kalshi's quadratic taker fee per contract, rounded the way the desk rounds it.

    src/fees.cpp rounds up to a hundredth of a cent, not to the cent. At p = 0.50 that is the
    difference between 1.75c and 2.00c, which on a study about whether an edge survives the fee is
    not a rounding detail.
    """
    return math.ceil(0.07 * price * (1 - price) * 10000 - 1e-9) / 10000.0


# ---- statistics ---------------------------------------------------------------------------------

def brier(pairs):
    return sum((p - o) ** 2 for p, o in pairs) / len(pairs) if pairs else float('nan')


def log_loss(pairs, eps=1e-6):
    if not pairs:
        return float('nan')
    total = 0.0
    for p, o in pairs:
        q = min(1 - eps, max(eps, p))
        total -= o * math.log(q) + (1 - o) * math.log(1 - q)
    return total / len(pairs)


def boot_mean(xs, n=2000, lo=5, hi=95, seed=7, clusters=None):
    """Bootstrap mean with a percentile interval.

    `clusters` is the key each observation belongs to, and when it is given the resampling is over
    CLUSTERS, not observations. That matters more than it sounds: one 15-minute contract
    contributes a quote snapshot every few seconds, every one of them carrying the same settled
    outcome and nearly the same probability. Resampling those as if they were independent shrinks
    the interval by roughly the square root of the number of snapshots per contract - five times
    or more - and an interval five times too narrow excludes zero on noise. The effective sample
    size here is the number of contracts, which is what COM-1 asks for 400 of.
    """
    if not xs:
        return float('nan'), float('nan'), float('nan')
    rng = random.Random(seed)
    m = sum(xs) / len(xs)
    if clusters is None:
        groups = [[x] for x in xs]
    else:
        by_key = collections.defaultdict(list)
        for x, k in zip(xs, clusters):
            by_key[k].append(x)
        groups = list(by_key.values())
    means = []
    for _ in range(n):
        total, count = 0.0, 0
        for _ in range(len(groups)):
            g = groups[rng.randrange(len(groups))]
            total += sum(g)
            count += len(g)
        means.append(total / count if count else 0.0)
    means.sort()
    return m, means[int(lo / 100 * n)], means[int(hi / 100 * n) - 1]


# ---- the replay ---------------------------------------------------------------------------------

class Decision:
    __slots__ = ('ticker', 'underlying', 'ts', 'seconds_left', 'spot', 'strike', 'sigma',
                 'p_model', 'p_market', 'ask', 'bid', 'outcome', 'distance_sigma', 'takeable')


def load_reference(con, symbols_wanted, plant_after=None, plant_move=0.0):
    """symbol -> [(ts, price)] sorted. Matched to an underlying by pattern, like the desk does.

    plant_after / plant_move are the look-ahead lever: every observation strictly after
    `plant_after` is moved by `plant_move`. A replay that cannot see the future must return exactly
    the same probability for every decision at or before that instant.
    """
    by_symbol = collections.defaultdict(list)
    for ts, symbol, price in con.execute(
            'SELECT ts_received, symbol, price FROM commodity_reference ORDER BY ts_received'):
        if price and price > 0:
            t = ts // 1000 if ts > 10 ** 12 else ts
            if plant_after is not None and t > plant_after:
                price *= 1 + plant_move
            by_symbol[symbol].append((t, price))
    out = {}
    for key, patterns in symbols_wanted.items():
        for symbol, ticks in by_symbol.items():
            low = symbol.lower()
            if any(pat in low for pat in patterns):
                out[key] = ticks
                break
    return out


def replay(con, plant_after=None, plant_move=0.0, max_decisions_per_market=0):
    """Every recorded quote of every settled 15-minute contract, priced from the feed as it stood.

    plant_after / plant_move move the reference after a given instant; see load_reference. They
    exist so the no-look-ahead guard can be proved rather than asserted.
    """
    execution.require_tables(con, ['commodity_reference', 'quotes', 'markets'])
    by_series = {series: (name, pats) for series, (name, pats) in UNDERLYINGS.items()}
    feeds = load_reference(con, {series: pats for series, (_n, pats) in by_series.items()},
                           plant_after, plant_move)

    rows = con.execute(
        "SELECT ticker, series_ticker, strike_type, floor_strike, cap_strike, open_time, close_time, result "
        "FROM markets WHERE series_ticker IN (%s) AND result IN ('yes','no')"
        % ','.join('?' * len(UNDERLYINGS)), list(UNDERLYINGS)).fetchall()

    decisions = []
    skipped = collections.Counter()
    for ticker, series, stype, floor_strike, cap_strike, open_time, close_time, result in rows:
        ticks = feeds.get(series)
        if not ticks:
            skipped['no reference feed recorded for this underlying'] += 1
            continue
        if stype not in ('greater', 'greater_or_equal', 'less', 'less_or_equal'):
            skipped['not a single-strike threshold contract'] += 1
            continue
        strike = floor_strike if stype.startswith('greater') else cap_strike
        if not strike or strike <= 0:
            skipped['no strike on the market row'] += 1
            continue
        close = iso_seconds(close_time)
        opened = iso_seconds(open_time)
        if not close:
            skipped['no close time'] += 1
            continue
        if not opened:
            skipped['no open time'] += 1
            continue
        outcome = 1 if result == 'yes' else 0
        # From the window's OPEN, not from the beginning of the quote history. The strike of these
        # contracts IS the reference's candle close at the open, so a quote recorded before the
        # open is being priced against a number that did not exist yet. That is look-ahead, in the
        # one place this file claims there is none.
        quotes = con.execute(
            'SELECT ts, yes_bid, yes_ask FROM quotes WHERE ticker=? AND ts>=? AND ts<? ORDER BY ts',
            (ticker, opened, close)).fetchall()
        if max_decisions_per_market:
            quotes = quotes[:max_decisions_per_market]
        tick_ts = [t for t, _ in ticks]
        for ts, yes_bid, yes_ask in quotes:
            tau = close - ts
            # The same window the live model will price in. Scoring the last ten seconds of a
            # contract - where the model is most confident and nothing could be sent anyway -
            # flatters the Brier with decisions the desk would not have made.
            if not (MIN_SECONDS_TO_CLOSE <= tau <= MAX_SECONDS_TO_CLOSE):
                skipped['outside the window the desk prices'] += 1
                continue
            # THE GUARD: only observations stamped at or before this decision exist here.
            cut = bisect.bisect_right(tick_ts, ts)
            visible = ticks[:cut]
            if not visible:
                continue
            spot = visible[-1][1]
            if ts - visible[-1][0] > REFERENCE_STALE_AFTER:   # the desk's own limit on the reference
                skipped['reference stale at the decision'] += 1
                continue

            sigma, warm, _detail = estimate_sigma(visible, ts)
            if not warm:
                skipped['volatility warm-up'] += 1
                continue
            d = Decision()
            d.ticker, d.underlying, d.ts = ticker, by_series[series][0], ts
            d.seconds_left, d.spot, d.strike, d.sigma = tau, spot, strike, sigma
            # tau runs from the OBSERVATION, not from the quote: the walk starts where the
            # reference was last seen. The live model does the same (commodity_terminal.cpp), and
            # a replay that does not cannot detect the difference.
            tau_model = close - visible[-1][0]
            d.p_model = min(P_CLAMP[1], max(P_CLAMP[0], p_yes(spot, strike, sigma, tau_model, stype)))
            d.bid, d.ask = yes_bid or 0.0, yes_ask or 0.0
            # A two-sided book for the Brier comparison: a mid needs two sides to be a price.
            d.p_market = (d.bid + d.ask) / 2 if (0 < d.bid < 1 and 0 < d.ask < 1) else None
            # COM-2 is about far strikes, which are exactly the ones quoted 0 bid / something ask.
            # Excluding them because there is no mid would drop the observations the hypothesis is
            # made of, so it is scored on whichever side can actually be taken.
            d.takeable = (0 < d.ask < 1) or (0 < d.bid < 1)
            d.outcome = outcome
            sd = sigma * math.sqrt(tau_model)
            d.distance_sigma = abs(math.log(spot / strike)) / sd if sd > 0 else 0.0
            decisions.append(d)
    return decisions, skipped


def iso_seconds(s):
    """2026-09-11T23:00:00Z -> epoch seconds. Returns 0 on anything else."""
    if not s or len(s) < 19:
        return 0
    try:
        import datetime as dt
        return int(dt.datetime.strptime(s[:19], '%Y-%m-%dT%H:%M:%S')
                   .replace(tzinfo=dt.timezone.utc).timestamp())
    except ValueError:
        return 0


# ---- reporting ----------------------------------------------------------------------------------

def report(decisions, skipped):
    print(f'{len(decisions)} decisions replayed on {len({d.ticker for d in decisions})} settled contracts')
    for reason, n in skipped.most_common():
        print(f'  skipped {n:>6}: {reason}')
    paired = [d for d in decisions if d.p_market is not None]
    if not paired:
        print('\nNothing to compare: no two-sided quote was recorded at any decision.')
        return
    print()
    print('COM-1  the terminal distribution against the tape')
    model = [(d.p_model, d.outcome) for d in paired]
    market = [(d.p_market, d.outcome) for d in paired]
    diffs = [(p - o) ** 2 - (q - o) ** 2 for (p, o), (q, _) in zip(model, market)]
    contracts = [d.ticker for d in paired]
    n_contracts = len(set(contracts))
    m, lo, hi = boot_mean(diffs, clusters=contracts)
    print(f'  {len(paired)} decisions on {n_contracts} settled contracts '
          f'({len(paired)/max(1,n_contracts):.0f} per contract)')
    print(f'  Brier model {brier(model):.5f}   market {brier(market):.5f}')
    print(f'  log loss model {log_loss(model):.5f}   market {log_loss(market):.5f}')
    print(f'  paired Brier difference (model - market) {m:+.5f}  90% CI [{lo:+.5f}, {hi:+.5f}]')
    print('  (resampled by contract, not by decision: the snapshots inside one 15-minute window'
          ' share its outcome)')
    verdict = ('the model is better and the interval excludes zero' if hi < 0 else
               'the market is better and the interval excludes zero' if lo > 0 else
               'INCONCLUSIVE: the interval contains zero')
    print(f'  -> {verdict}')
    if n_contracts < 400:
        print(f'  -> and REGISTERED SAMPLE NOT REACHED: COM-1 asks for 400 settled contracts, this is '
              f'{n_contracts}. Nothing above is a result yet.')

    print()
    print('COM-1  calibration, by the model\'s own decile')
    buckets = collections.defaultdict(list)
    for d in paired:
        buckets[min(9, int(d.p_model * 10))].append(d)
    print(f'  {"decile":>8} {"n":>7} {"mean p":>8} {"settled":>8} {"mean mid":>9}')
    for k in sorted(buckets):
        b = buckets[k]
        print(f'  {k/10:>8.1f} {len(b):>7} {sum(x.p_model for x in b)/len(b):>8.3f} '
              f'{sum(x.outcome for x in b)/len(b):>8.3f} {sum(x.p_market for x in b)/len(b):>9.3f}')

    print()
    print('COM-2  by distance from the strike, in sigma of the move still to come')
    # Not restricted to `paired`: a far strike is often quoted with one side empty, and those are
    # exactly the observations this hypothesis is about.
    takeable = [d for d in decisions if d.takeable]
    print(f'  {"bucket":>10} {"n":>7} {"contracts":>10} {"mean ask":>9} {"settled":>8} {"model":>7} '
          f'{"net@ask":>9} {"90% CI":>22}')
    for lo_b, hi_b in SIGMA_BUCKETS:
        b = [d for d in takeable if lo_b <= d.distance_sigma < hi_b]
        if not b:
            continue
        rets, keys = [], []
        for d in b:
            # The cheap side at the touch, fee included. An upper bound on what a taker could have
            # made: it assumes the touch was there for us, which no recorded quote can prove.
            if 0 < d.ask < 1 and d.p_model > d.ask:
                rets.append(d.outcome - d.ask - taker_fee(d.ask))
                keys.append(d.ticker)
            elif 0 < d.bid < 1 and d.p_model < d.bid:
                no_price = 1 - d.bid
                rets.append((1 - d.outcome) - no_price - taker_fee(no_price))
                keys.append(d.ticker)
        mr, rlo, rhi = boot_mean(rets, clusters=keys) if rets else (float('nan'),) * 3
        label = f'{lo_b:.1f}-{hi_b:.1f}' if hi_b != float('inf') else f'{lo_b:.1f}+'
        asks = [d.ask for d in b if 0 < d.ask < 1]
        print(f'  {label:>10} {len(b):>7} {len({d.ticker for d in b}):>10} '
              f'{(sum(asks)/len(asks) if asks else float("nan")):>9.3f} '
              f'{sum(x.outcome for x in b)/len(b):>8.3f} {sum(x.p_model for x in b)/len(b):>7.3f} '
              f'{mr:>9.4f} {f"[{rlo:+.4f}, {rhi:+.4f}]":>22}')

    print()
    print('COM-3  by time left in the window (a lag after a move shows up here first)')
    print(f'  {"minutes":>9} {"n":>7} {"Brier model":>12} {"market":>9} {"model-market":>13}')
    for lo_m, hi_m in [(0, 2), (2, 5), (5, 10), (10, 16)]:
        b = [d for d in paired if lo_m * 60 <= d.seconds_left < hi_m * 60]
        if not b:
            continue
        bm = brier([(d.p_model, d.outcome) for d in b])
        bk = brier([(d.p_market, d.outcome) for d in b])
        print(f'  {f"{lo_m}-{hi_m}":>9} {len(b):>7} {bm:>12.5f} {bk:>9.5f} {bm-bk:>+13.5f}')

    print()
    print('A lower Brier is better. This is a prediction study: the net column is an upper bound on')
    print('what a taker could have made at the recorded touch, not a fill anyone is entitled to.')


# ---- the look-ahead guard, provable without a database -------------------------------------------

def self_test():
    """Build a database in memory, replay it twice, and prove the replay cannot see the future."""
    con = sqlite3.connect(':memory:')
    con.execute('CREATE TABLE commodity_reference(ts_received INTEGER, symbol TEXT, price REAL)')
    con.execute('CREATE TABLE quotes(ts INTEGER, ticker TEXT, yes_bid REAL, yes_ask REAL)')
    con.execute('CREATE TABLE markets(ticker TEXT, series_ticker TEXT, strike_type TEXT, floor_strike REAL,'
                ' cap_strike REAL, open_time TEXT, close_time TEXT, result TEXT)')
    open_ts, close_ts = 1789000000, 1789000900
    rng = random.Random(11)
    price = 4350.0
    for t in range(open_ts - 3600, close_ts, 5):
        price *= math.exp(0.00004 * rng.uniform(-1, 1))
        con.execute('INSERT INTO commodity_reference VALUES(?,?,?)', (t, 'Metal.Index.GOLD/USD', price))
    for t in range(open_ts, close_ts, 30):
        con.execute('INSERT INTO quotes VALUES(?,?,?,?)', (t, 'KXGOLD15M-X', 0.48, 0.52))
    # Two quotes from BEFORE the window opened. The strike of these contracts is the reference's
    # own candle close at the open, so pricing a quote from before that against it is look-ahead.
    for t in (open_ts - 300, open_ts - 60):
        con.execute('INSERT INTO quotes VALUES(?,?,?,?)', (t, 'KXGOLD15M-X', 0.48, 0.52))
    con.execute("INSERT INTO markets VALUES('KXGOLD15M-X','KXGOLD15M','greater_or_equal',4350.0,0,"
                "'2026-09-11T22:45:00Z','2026-09-11T23:00:00Z','yes')")
    # The close_time above must be the one the replay uses, so state it as epoch too.
    con.execute("UPDATE markets SET close_time=?", (iso_text(close_ts),))
    con.execute("UPDATE markets SET open_time=?", (iso_text(open_ts),))

    plain, _ = replay(con)
    assert plain, 'the self-test replay produced no decisions'
    # Move the reference 5% from the middle of the window onwards. Every decision taken at or
    # before that instant must be untouched: it could not have known. Decisions after it must
    # change, or the lever is not reaching the replay at all and the guard proves nothing.
    cut_ts = plain[len(plain) // 2].ts
    planted, _ = replay(con, plant_after=cut_ts, plant_move=0.05)
    assert len(plain) == len(planted), 'the planted run changed the number of decisions'
    before = [(a, b) for a, b in zip(plain, planted) if a.ts <= cut_ts]
    after = [(a, b) for a, b in zip(plain, planted) if a.ts > cut_ts]
    assert before and after, 'the self-test window is too small to split'
    worst = max(abs(a.p_model - b.p_model) for a, b in before)
    assert worst == 0.0, f'LOOK-AHEAD: a move after the decision changed p by {worst}'
    assert any(a.p_model != b.p_model for a, b in after), \
        'the planted move changed nothing anywhere: the self-test is not testing the guard'

    # And the same in the other direction: a decision late in the window must see MORE of the feed
    # than an early one, or the guard is simply returning nothing.
    assert plain[-1].seconds_left < plain[0].seconds_left
    assert plain[-1].spot != plain[0].spot, 'the replay is not advancing the reference at all'

    # Nothing from before the window opened, and nothing outside the window the desk would price.
    assert all(d.ts >= open_ts for d in plain), \
        'a quote from before the window open was priced against a strike that did not exist yet'
    assert all(MIN_SECONDS_TO_CLOSE <= d.seconds_left <= MAX_SECONDS_TO_CLOSE for d in plain)

    # The interval has to be resampled by contract. Everything here is one contract, so a cluster
    # bootstrap of it has no sampling variation at all, while the naive one invents some.
    diffs = [(d.p_model - d.outcome) ** 2 - 0.25 for d in plain]
    _m, nlo, nhi = boot_mean(diffs)
    _m, clo, chi = boot_mean(diffs, clusters=[d.ticker for d in plain])
    assert chi - clo < 1e-12, 'a single-contract sample cannot have a non-degenerate interval'
    assert nhi - nlo > chi - clo, 'the naive interval should be the narrow, wrong one'

    # The fee is the desk's fee, to a hundredth of a cent (src/fees.cpp), not to the cent.
    assert abs(taker_fee(0.5) - 0.0175) < 1e-12, taker_fee(0.5)

    # The probability itself: at the money with time left it is near a half; far above the strike
    # with seconds left it is near one.
    sigma = plain[0].sigma
    assert abs(p_yes(100.0, 100.0, sigma, 300, 'greater_or_equal') - 0.5) < 1e-9
    assert p_yes(100.0, 90.0, sigma, 60, 'greater_or_equal') > 0.999
    assert p_yes(100.0, 110.0, sigma, 60, 'greater_or_equal') < 0.001
    # The tie pays YES, and only on the inclusive side.
    assert p_yes(100.0, 100.0, sigma, 0, 'greater_or_equal') == 1.0
    assert p_yes(100.0, 100.0, sigma, 0, 'greater') == 0.0
    # YES and NO on the same strike are one distribution seen from two sides.
    assert abs(p_yes(101.0, 100.0, sigma, 300, 'greater_or_equal')
               + p_yes(101.0, 100.0, sigma, 300, 'less') - 1.0) < 1e-12
    # A higher strike is never more likely to be exceeded.
    ladder = [p_yes(100.0, k, sigma, 300, 'greater_or_equal') for k in (98, 99, 100, 101, 102)]
    assert all(a >= b - 1e-12 for a, b in zip(ladder, ladder[1:])), ladder
    # Finally run the report over the synthetic sample, so the printing path is exercised too.
    import io, contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        report(plain, collections.Counter())
    text = buf.getvalue()
    for expect in ('COM-1', 'COM-2', 'COM-3', 'REGISTERED SAMPLE NOT REACHED', 'resampled by contract'):
        assert expect in text, f'the report never printed {expect}'

    print(f'self-test OK: {len(plain)} decisions. A 5% move in the reference from the middle of the '
          f'window on\n  changed none of the {len(before)} probabilities before it (worst {worst:g}) '
          f'and did change the ones after.')


def iso_text(ts):
    import datetime as dt
    return dt.datetime.fromtimestamp(ts, dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--db', default=DB)
    ap.add_argument('--self-test', action='store_true', help='prove the no-look-ahead guard; no database needed')
    ap.add_argument('--max-per-market', type=int, default=0, help='cap decisions per contract (a quick pass)')
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return
    execution.banner(execution.PREDICTION_ONLY,
                     "COM-1..COM-4. Probabilities are compared with recorded prices. The net column\n"
                     "assumes a fill at the recorded touch and is an upper bound, not an edge.")
    if not os.path.exists(args.db):
        raise SystemExit(f'no database at {args.db}')
    con = sqlite3.connect(f'file:{args.db}?mode=ro', uri=True)
    decisions, skipped = replay(con, max_decisions_per_market=args.max_per_market)
    report(decisions, skipped)


if __name__ == '__main__':
    main()
