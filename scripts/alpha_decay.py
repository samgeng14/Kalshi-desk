#!/usr/bin/env python3
"""Univariate alpha audit: does any primitive feature predict the next midpoint move, before ML?

A gradient booster fed thirty features will report an in-sample edge whether or not one exists.
The way to not be fooled is to look at each feature on its own first and demand that the phenomenon
be visible: an information coefficient that survives autocorrelation-robust inference, a decay
profile that decays rather than jumping around, and deciles that are roughly monotone. A feature
that fails all three is not going to be rescued by being mixed with twenty-nine others -- it will
just be harder to see that it failed.

Target is the FUTURE MIDPOINT return, never last-trade-to-last-trade: trade-price labels carry
bid-ask bounce, which makes any signal built from a recent price appear to predict the reversal of
its own noise.

Inference. Overlapping forward windows make a naive t-statistic meaningless -- with 1-minute bars
and a 10-minute horizon each observation shares 90% of its window with its neighbour. Two
independent defences, both reported:
  * Newey-West (Bartlett) HAC standard errors on the univariate regression slope, lag = horizon
  * moving-block bootstrap on the IC, block length = horizon, which makes no parametric assumption

Regimes. Results are broken out by trade count (0, 1-5, >5). A minute with no print is not a
useless observation -- orders arriving and cancelling move the implied clearing price and that can
carry information -- but it is not the same data-generating process as a minute with flow, and
pooling them hides both.

  python3 scripts/alpha_decay.py
  python3 scripts/alpha_decay.py --tickers KXBTCPERP KXETHPERP --horizons 1 2 5 10
"""
import argparse, math, os, random, sqlite3, statistics, sys
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Kalshi encodes "no order on this side" as INT64_MAX, which arrives as 922337203685477.6 after the
# 10^4 fixed-point scaling. It is not a price. Left in, one such bar produces two astronomical
# returns -- into it and out of it -- which is enough to move a decile mean to 1e14 bps and make a
# feature look like a 300-trillion-basis-point edge. This exact sentinel was already fixed once in
# src/perps.cpp; it was not carried into the research path.
SENTINEL = 1e9


def px(v):
    """A price, or 0 if the field is the no-order sentinel, negative, or not finite."""
    try:
        f = float(v)
    except (TypeError, ValueError):
        return 0.0
    return f if (f == f and 0.0 < f < SENTINEL) else 0.0

ROUND_TRIP_BPS = 17.0          # maker 5 in + taker 12 out, this account's observed tier


# ---- statistics -------------------------------------------------------------------------------
# numpy when it is there, pure Python when it is not. The pure-Python versions are kept as the
# definition and tests/test_alpha_decay.py asserts the two agree to 1e-9: a fast path that quietly
# computes something slightly different is worse than a slow one.
try:
    import numpy as _np
except ImportError:
    _np = None
def mean(x): return sum(x) / len(x) if x else float('nan')


def _pearson_py(x, y, min_n=20):
    n = len(x)
    if n < min_n: return float('nan')
    mx, my = mean(x), mean(y)
    sxy = sum((a - mx) * (b - my) for a, b in zip(x, y))
    sxx = sum((a - mx) ** 2 for a in x); syy = sum((b - my) ** 2 for b in y)
    if sxx <= 0 or syy <= 0: return float('nan')
    return max(-1.0, min(1.0, sxy / math.sqrt(sxx * syy)))


def pearson(x, y, min_n=20):
    """min_n is a parameter because this is used for two different things: an IC over thousands of
    observations, where 20 is far too few to mean anything, and decile monotonicity over exactly
    ten points, where a hard floor of 20 silently returns nan for every row."""
    n = len(x)
    if n < min_n: return float('nan')
    if _np is None or n < 2000:
        return _pearson_py(x, y, min_n)
    xa = _np.asarray(x, dtype=_np.float64); ya = _np.asarray(y, dtype=_np.float64)
    xc = xa - xa.mean(); yc = ya - ya.mean()
    den = math.sqrt(float((xc * xc).sum()) * float((yc * yc).sum()))
    if den <= 0: return float('nan')
    return max(-1.0, min(1.0, float((xc * yc).sum()) / den))


def rank(v):
    """Average ranks, ties shared -- otherwise Spearman is wrong on the many equal values a
    thin market produces (whole minutes of identical quotes)."""
    order = sorted(range(len(v)), key=lambda i: v[i])
    r = [0.0] * len(v)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and v[order[j + 1]] == v[order[i]]: j += 1
        avg = (i + j) / 2.0 + 1.0
        for k in range(i, j + 1): r[order[k]] = avg
        i = j + 1
    return r


def spearman(x, y, min_n=20):
    return pearson(rank(x), rank(y), min_n)


def newey_west_t(x, y, lag):
    """t-statistic on the slope of y ~ a + b*x with Bartlett HAC errors.

    With overlapping forward returns the residuals are autocorrelated by construction out to the
    horizon, which is exactly what this corrects and what a naive t-stat ignores.
    """
    n = len(x)
    if n < 30: return float('nan'), float('nan')
    mx, my = mean(x), mean(y)
    sxx = sum((a - mx) ** 2 for a in x)
    if sxx <= 0: return float('nan'), float('nan')
    b = sum((a - mx) * (c - my) for a, c in zip(x, y)) / sxx
    a0 = my - b * mx
    L = max(1, min(int(lag), n // 4))
    if _np is not None and n >= 2000:
        xa = _np.asarray(x, dtype=_np.float64); ya = _np.asarray(y, dtype=_np.float64)
        u = (ya - a0 - b * xa) * (xa - mx)
        s = float((u * u).sum())
        for l in range(1, L + 1):
            s += 2.0 * (1.0 - l / (L + 1.0)) * float(_np.dot(u[l:], u[:-l]))
    else:
        u = [(c - a0 - b * a) * (a - mx) for a, c in zip(x, y)]  # score contributions
        s = sum(v * v for v in u)
        for l in range(1, L + 1):
            w = 1.0 - l / (L + 1.0)
            s += 2.0 * w * sum(u[t] * u[t - l] for t in range(l, n))
    if s <= 0: return b, float('nan')
    se = math.sqrt(s) / sxx
    return b, (b / se if se > 0 else float('nan'))


def block_bootstrap_ic(x, y, block, iters=1000, seed=7):
    """Moving-block bootstrap CI on the Pearson IC. Blocks preserve the autocorrelation that
    makes an i.i.d. bootstrap (and a naive t-stat) overstate significance."""
    n = len(x)
    if n < 60: return float('nan'), float('nan')
    block = max(2, min(int(block), n // 4))
    nb = n // block
    if _np is None:
        rnd = random.Random(seed)
        out = []
        for _ in range(iters):
            xs, ys = [], []
            for _ in range(nb):
                s = rnd.randrange(0, n - block)
                xs.extend(x[s:s + block]); ys.extend(y[s:s + block])
            ic = pearson(xs, ys)
            if ic == ic: out.append(ic)
    else:
        # Chunked so the index array stays small: 1,000 iterations over 140,000 observations
        # across 2,080 combinations is twenty hours in pure Python and minutes vectorised.
        xa = _np.asarray(x, dtype=_np.float64); ya = _np.asarray(y, dtype=_np.float64)
        rng = _np.random.default_rng(seed)
        offs = _np.arange(block)
        vals, CH = [], 32
        for s0 in range(0, iters, CH):
            k = min(CH, iters - s0)
            starts = rng.integers(0, n - block, size=(k, nb))
            idx = (starts[:, :, None] + offs[None, None, :]).reshape(k, nb * block)
            xs, ys = xa[idx], ya[idx]
            xc = xs - xs.mean(axis=1, keepdims=True)
            yc = ys - ys.mean(axis=1, keepdims=True)
            den = _np.sqrt((xc * xc).sum(axis=1) * (yc * yc).sum(axis=1))
            with _np.errstate(invalid='ignore', divide='ignore'):
                vals.append(_np.where(den > 0, (xc * yc).sum(axis=1) / den, _np.nan))
        arr = _np.concatenate(vals)
        out = [float(v) for v in arr[~_np.isnan(arr)]]
    if len(out) < 50: return float('nan'), float('nan')
    out.sort()
    return out[int(.025 * len(out))], out[int(.975 * len(out))]


# A feature whose values are mostly one number cannot be split into ten meaningful deciles: the
# boundaries fall inside a block of identical values, and whatever orders that block decides the
# result. Features like minutes_since_trade and volume are discrete and heavily tied.
MAX_TIED_SHARE = 0.50      # the modal value may not exceed half the sample
MIN_DISTINCT = 20


def deciles(x, y, k=10):
    """Mean forward return by feature decile, plus how monotone the progression is.

    The sort key matters more than it looks. `sorted(zip(x, y))` sorts TUPLES, which is
    lexicographic: when x ties it falls through to y -- the forward return being predicted. On a
    feature with heavy ties that orders the tied block by its own label and manufactures a clean
    monotonic decile ramp out of nothing. Measured on pure noise with 68% ties, that bug produced
    a +12.93 bps top-minus-bottom spread and a monotonicity of +0.66; sorting on x alone gave
    +0.18. It is exactly what reported a "+49.2 bps net" edge on KXNEARPERP.

    Returns ([], nan) when the feature is too tied for deciles to mean anything, rather than
    returning a number that looks like a measurement.
    """
    n = len(x)
    if n < k * 10: return [], float('nan')
    counts = {}
    for v in x:
        counts[v] = counts.get(v, 0) + 1
        if len(counts) > MIN_DISTINCT * 4: break
    distinct = len(set(x)) if len(counts) <= MIN_DISTINCT * 4 else MIN_DISTINCT * 4 + 1
    if distinct < MIN_DISTINCT: return [], float('nan')
    if max(counts.values()) / n > MAX_TIED_SHARE: return [], float('nan')
    pairs = sorted(zip(x, y), key=lambda p: p[0])      # by x ONLY; never fall through to the label
    out = []
    for i in range(k):
        chunk = pairs[i * n // k:(i + 1) * n // k]
        out.append(mean([b for _, b in chunk]) if chunk else float('nan'))
    good = [(i, v) for i, v in enumerate(out) if v == v]
    # ten deciles is the whole series here, so the floor has to come down to match
    mono = spearman([i for i, _ in good], [v for _, v in good], min_n=4) if len(good) > 3 else float('nan')
    return out, mono


# ---- features ---------------------------------------------------------------------------------
def build(bars, horizons):
    """bars: (ts, bid_close, ask_close, volume, notional, oi) ascending, one per minute.

    Every feature is measured at the close of bar i. The label is the mid return from the close of
    bar i to the close of bar i+h. Entry is therefore assumed at the same mid the feature was read
    from, which is optimistic -- the cost model below is what makes it honest.
    """
    mids, rows, dropped = [], [], 0
    for ts, b, k, v, nn, oi in bars:
        b, k = px(b), px(k)
        if not (b > 0 and k > 0 and k >= b):
            mids.append(None); dropped += 1; continue
        mids.append(.5 * (b + k))
    last_trade_i = None
    for i in range(len(bars)):
        ts, b, k, v, nn, oi = bars[i]
        m = mids[i]
        if m is None or m <= 0: continue
        if v: last_trade_i = i
        prev = mids[i - 1] if i >= 1 else None
        p5 = mids[i - 5] if i >= 5 else None
        p15 = mids[i - 15] if i >= 15 else None
        if not prev or not p5 or not p15: continue
        vols = [bars[j][3] or 0 for j in range(max(0, i - 20), i)]
        vbar = mean(vols) if vols else 0.0
        oi_prev = bars[i - 5][5] if i >= 5 else None
        f = {
            'mom_1m': (m - prev) / prev * 1e4,
            'mom_5m': (m - p5) / p5 * 1e4,
            'mom_15m': (m - p15) / p15 * 1e4,
            'revert_1m': -(m - prev) / prev * 1e4,
            'close_quote_gap_bps': (k - b) / m * 1e4,
            'volume': float(v or 0),
            'volume_shock': ((v or 0) / vbar) if vbar > 0 else 0.0,
            'oi_change_5m': ((oi - oi_prev) / oi_prev * 1e4) if (oi_prev and oi_prev > 0) else 0.0,
            'minutes_since_trade': float(i - last_trade_i) if last_trade_i is not None else 999.0,
            'notional': float(nn or 0),
        }
        lab = {}
        for h in horizons:
            j = i + h
            lab[h] = ((mids[j] - m) / m * 1e4) if (j < len(mids) and mids[j]) else None
        rows.append({'i': i, 'ts': ts, 'mid': m, 'trades': float(v or 0),
                     'gap_bps': f['close_quote_gap_bps'], 'f': f, 'lab': lab})
    return rows, dropped


REGIMES = [('all', lambda r: True),
           ('no_trade', lambda r: r['trades'] == 0),
           ('low_1_5', lambda r: 0 < r['trades'] <= 5),
           ('high_gt5', lambda r: r['trades'] > 5)]


def open_ro(path):
    """Open read-only, working around a database on a network or FUSE mount.

    SQLite in WAL mode needs to write a -shm file even to read, so `mode=ro` fails with "attempt
    to write a readonly database" on a filesystem that will not take those writes. `immutable=1`
    skips the WAL machinery, which is correct only if nothing is mid-write; if that fails too the
    file is copied somewhere local and read from there.
    """
    try:
        db = sqlite3.connect("file:%s?mode=ro" % path, uri=True)
        db.execute("select count(*) from perp_bars limit 1").fetchone()
        return db
    except sqlite3.OperationalError:
        pass
    try:
        db = sqlite3.connect("file:%s?immutable=1" % path, uri=True)
        db.execute("select count(*) from perp_bars limit 1").fetchone()
        print("  (opened immutable: the database is on a filesystem that refuses WAL writes)")
        return db
    except sqlite3.OperationalError:
        pass
    import shutil, tempfile
    tmp = os.path.join(tempfile.gettempdir(), 'perps_read_%d.sqlite' % os.getpid())
    print(f"  (copying to {tmp} to read: the mount will not take SQLite's WAL writes)")
    shutil.copy2(path, tmp)
    return sqlite3.connect(tmp)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--db', default=os.path.join(ROOT, 'data', 'perps_research.sqlite'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'reports', 'alpha_decay.md'))
    ap.add_argument('--tickers', nargs='*')
    ap.add_argument('--horizons', nargs='*', type=int, default=[1, 2, 5, 10])
    ap.add_argument('--min-bars', type=int, default=2000)
    a = ap.parse_args()
    if not os.path.exists(a.db):
        sys.exit(f"{a.db} not found -- run scripts/backfill_perps_history.py, then perps_audit.py")
    db = open_ro(a.db)
    tickers = a.tickers or [r[0] for r in db.execute(
        "select ticker from perp_bars where period=1 group by ticker "
        "having count(*)>=? order by count(*) desc", (a.min_bars,))]
    if not tickers: sys.exit("no ticker has enough 1-minute bars")

    results, tested = [], 0
    for t in tickers:
        bars = db.execute("select end_ts,bid_close,ask_close,volume,volume_notional,open_interest "
                          "from perp_bars where ticker=? and period=1 order by end_ts",
                          (t,)).fetchall()
        rows, dropped = build(bars, a.horizons)
        if len(rows) < a.min_bars:
            print(f"{t:14s} only {len(rows)} usable rows, skipped"); continue
        feats = sorted(rows[0]['f'].keys())
        pct = dropped / max(1, len(bars)) * 100
        print(f"{t:14s} {len(rows):,} rows"
              + (f"   ({dropped:,} bars dropped, {pct:.2f}%: one-sided book or no-order sentinel)"
                 if dropped else ""))
        for rname, rsel in REGIMES:
            sub = [r for r in rows if rsel(r)]
            if len(sub) < 300: continue
            for fname in feats:
                for h in a.horizons:
                    x = [r['f'][fname] for r in sub if r['lab'].get(h) is not None]
                    y = [r['lab'][h] for r in sub if r['lab'].get(h) is not None]
                    if len(x) < 300: continue
                    if len(set(x)) < 10: continue          # a constant predicts nothing
                    tested += 1
                    ic, sic = pearson(x, y), spearman(x, y)
                    _, tstat = newey_west_t(x, y, lag=h)
                    lo, hi = block_bootstrap_ic(x, y, block=h * 3)
                    dec, mono = deciles(x, y)
                    # top-minus-bottom DECILE spread, not a bid-ask spread: in this
                    # codebase "spread" without a qualifier has caused enough confusion already
                    decile_spread = ((dec[-1] - dec[0]) if len(dec) == 10 and dec[0] == dec[0]
                                     else float('nan'))
                    results.append({'ticker': t, 'regime': rname, 'feature': fname, 'h': h,
                                    'n': len(x), 'ic': ic, 'sic': sic, 'nw_t': tstat,
                                    'lo': lo, 'hi': hi, 'mono': mono, 'dec': dec,
                                    'decile_spread_bps': decile_spread,
                                    'net_bps': decile_spread - ROUND_TRIP_BPS})

    # A decile spread larger than this is not a finding, it is a data fault. Five-minute crypto
    # moves reach a few hundred basis points; a decile MEAN in the thousands means something
    # unpriced got into the series. The INT64_MAX no-order sentinel did exactly that once, and
    # reported a 300-trillion-basis-point edge before anyone looked at the data.
    IMPLAUSIBLE_BPS = 2000.0
    suspect = [r for r in results
               if r['decile_spread_bps'] == r['decile_spread_bps']
               and abs(r['decile_spread_bps']) > IMPLAUSIBLE_BPS]

    def survives(r):
        """Survival needs all three, not any one: an IC interval clear of zero, HAC significance,
        and deciles that actually progress. Any single one of these fires on noise. A decile
        spread in the thousands of basis points is excluded outright as a data fault."""
        if abs(r['decile_spread_bps']) > IMPLAUSIBLE_BPS: return False
        # The correlation and the decile progression must agree about the SIGN. When they do not,
        # one of them is not measuring the relationship, and a result nobody can interpret is not
        # a result. This is what would have caught the tie bug without anyone reading the table.
        if r['ic'] == r['ic'] and r['mono'] == r['mono'] and r['ic'] * r['mono'] < 0: return False
        return (r['lo'] == r['lo'] and r['hi'] == r['hi'] and r['lo'] * r['hi'] > 0
                and abs(r['nw_t']) > 2.0 and abs(r['mono']) > 0.6)

    surv = [r for r in results if survives(r)]
    econ = [r for r in surv if r['net_bps'] == r['net_bps'] and r['net_bps'] > 0]
    results.sort(key=lambda r: -abs(r['ic']) if r['ic'] == r['ic'] else 0)

    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, 'w') as f:
        w = f.write
        w("# Univariate alpha decay\n\n")
        w(f"Generated {datetime.now(timezone.utc):%Y-%m-%d %H:%M} UTC from `{a.db}`.\n\n")
        w("Target is the **future midpoint return**, never trade-price to trade-price: a "
          "trade-price label carries bid-ask bounce, and any feature built from a recent price "
          "then appears to predict the reversal of its own noise.\n\n")
        w("Inference is autocorrelation-robust twice over, because overlapping forward windows "
          "make a naive t-statistic meaningless: Newey-West HAC errors on the regression slope, "
          "and a moving-block bootstrap on the IC that assumes nothing parametric.\n\n")
        w(f"**{tested} feature/horizon/regime/ticker combinations tested.** At 5% roughly "
          f"{tested * 0.05:.0f} would pass a single test by chance, which is why survival here "
          f"requires all three: bootstrap CI clear of zero, |NW t| > 2, and |decile monotonicity| "
          f"> 0.6.\n\n")
        if suspect:
            w(f"## Data fault, not a finding\n\n**{len(suspect)} combinations produced a decile "
              f"spread above {IMPLAUSIBLE_BPS:.0f} bps and were excluded.** A five-minute crypto "
              f"move reaches a few hundred basis points; a decile mean in the thousands means "
              f"something unpriced is in the series. Check the source data before reading anything "
              f"else here. Worst offenders:\n\n")
            for r in sorted(suspect, key=lambda r: -abs(r['decile_spread_bps']))[:8]:
                w(f"- `{r['ticker']}` `{r['feature']}` {r['h']}m {r['regime']}: "
                  f"{r['decile_spread_bps']:+.3g} bps\n")
            w("\n")
        contradictory = [r for r in results if r['ic'] == r['ic'] and r['mono'] == r['mono']
                         and r['ic'] * r['mono'] < 0 and abs(r['mono']) > 0.6]
        if contradictory:
            w(f"**{len(contradictory)} combinations were excluded for disagreeing with "
              f"themselves**: the correlation said one direction and the decile progression said "
              f"the other. When those two disagree, one is not measuring the relationship.\n\n")
        w(f"- passed all three: **{len(surv)}**\n")
        w(f"- of those, top-minus-bottom decile spread still positive after the "
          f"{ROUND_TRIP_BPS:.0f} bps round trip: **{len(econ)}**\n\n")
        if not econ:
            w("**Nothing is executable.** A statistically real forecast that does not clear its "
              "own transaction costs is not a strategy, and this is the result that stops work "
              "rather than the result that starts it.\n\n")
        w("## Survivors\n\n")
        if surv:
            w("| ticker | regime | feature | h | n | IC | Spearman | NW t | IC 95% | decile mono | "
              "top-bot bps | net of 17bps |\n|" + "---|" * 12 + "\n")
            for r in sorted(surv, key=lambda r: -r['net_bps'])[:60]:
                w(f"| `{r['ticker']}` | {r['regime']} | `{r['feature']}` | {r['h']}m | {r['n']:,} | "
                  f"{r['ic']:+.3f} | {r['sic']:+.3f} | {r['nw_t']:+.1f} | "
                  f"[{r['lo']:+.3f}, {r['hi']:+.3f}] | {r['mono']:+.2f} | {r['decile_spread_bps']:+.1f} | "
                  f"{r['net_bps']:+.1f} |\n")
        else:
            w("_none_\n")
        w("\n## Every result, strongest IC first\n\n")
        w("| ticker | regime | feature | h | n | IC | Spearman | NW t | IC 95% | mono | top-bot bps |\n"
          "|" + "---|" * 11 + "\n")
        for r in results[:400]:
            w(f"| `{r['ticker']}` | {r['regime']} | `{r['feature']}` | {r['h']}m | {r['n']:,} | "
              f"{r['ic']:+.3f} | {r['sic']:+.3f} | {r['nw_t']:+.1f} | "
              f"[{r['lo']:+.3f}, {r['hi']:+.3f}] | {r['mono']:+.2f} | {r['decile_spread_bps']:+.1f} |\n")
        w("\n## Decay profiles\n\n")
        w("A real effect decays smoothly as the horizon lengthens. One that jumps around is "
          "usually the sample talking.\n\n")
        byf = {}
        for r in results:
            if r['regime'] != 'all': continue
            byf.setdefault((r['ticker'], r['feature']), {})[r['h']] = r['ic']
        w("| ticker | feature | " + " | ".join(f"IC {h}m" for h in a.horizons) + " |\n")
        w("|" + "---|" * (2 + len(a.horizons)) + "\n")
        for (t, fn), d in sorted(byf.items()):
            w(f"| `{t}` | `{fn}` | " + " | ".join(
                f"{d[h]:+.3f}" if h in d and d[h] == d[h] else "-" for h in a.horizons) + " |\n")
        w("\n## Decile tables, survivors only\n\n")
        for r in sorted(surv, key=lambda r: -abs(r['ic']))[:12]:
            w(f"### `{r['ticker']}` / `{r['feature']}` / {r['h']}m / {r['regime']}\n\n")
            w("| decile | mean fwd bps |\n|---|---|\n")
            for i, v in enumerate(r['dec'], 1):
                w(f"| {i} | {v:+.2f} |\n")
            w("\n")
    print(f"\n{tested} combinations tested")
    if suspect:
        print(f"  EXCLUDED as a data fault (decile spread over {IMPLAUSIBLE_BPS:.0f} bps): "
              f"{len(suspect)} -- check the source data")
    print(f"  passed all three tests: {len(surv)}")
    print(f"  still positive after {ROUND_TRIP_BPS:.0f} bps: {len(econ)}")
    for r in sorted(econ, key=lambda r: -r['net_bps'])[:10]:
        print(f"    {r['ticker']:14s} {r['feature']:22s} {r['h']}m {r['regime']:9s} "
              f"IC {r['ic']:+.3f}  net {r['net_bps']:+.1f} bps")
    print(f"\nwrote {a.out}")


main()
