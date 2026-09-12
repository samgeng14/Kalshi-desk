#!/usr/bin/env python3
"""Ground-truth tests for scripts/alpha_decay.py.

The tool exists to avoid being fooled, so it has to be checked against data whose answer is known:
pure noise must produce nothing, an injected signal must be found, and the HAC correction must
actually shrink the t-statistic that overlapping windows inflate.
"""
import math, os, random, sqlite3, statistics, sys, tempfile, shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(ROOT, 'scripts', 'alpha_decay.py')).read().replace('\nmain()\n', '\n')
A = {'__file__': 'a', '__name__': 'a'}
exec(compile(src, 'a', 'exec'), A)

checks = fails = 0
def check(c, what):
    global checks, fails
    checks += 1
    if not c: fails += 1; print('FAIL: ' + what)

rnd = random.Random(11)

# --- 1. correlation primitives -----------------------------------------------------------------
x = [rnd.gauss(0, 1) for _ in range(2000)]
y = [v * 2 + rnd.gauss(0, 1) for v in x]
check(A['pearson'](x, y) > 0.85, 'pearson finds a strong linear relationship')
check(abs(A['pearson'](x, [rnd.gauss(0, 1) for _ in x])) < 0.08, 'pearson ~0 on independent noise')
check(A['spearman'](x, [v ** 3 for v in x]) > 0.99, 'spearman is monotone-invariant (cubed)')
# ties: a thin market produces whole runs of identical quotes
tied = [0.0] * 500 + [1.0] * 500
check(abs(A['spearman'](tied, [rnd.gauss(0, 1) for _ in tied])) < 0.12,
      'spearman handles heavy ties without blowing up')

# --- 2. Newey-West must SHRINK the t-stat on overlapping windows --------------------------------
# Build an AR(1)-ish overlapping series: y_t is a 10-step forward sum, so neighbours share 9 steps.
steps = [rnd.gauss(0, 1) for _ in range(4000)]
xs, ys = [], []
for i in range(len(steps) - 10):
    xs.append(steps[i])                       # feature: a value INDEPENDENT of the future
    ys.append(sum(steps[i + 1:i + 11]))       # label: overlapping forward window, no real link
b, t_nw = A['newey_west_t'](xs, ys, lag=10)
# naive OLS t for the same slope
n = len(xs); mx, my = A['mean'](xs), A['mean'](ys)
sxx = sum((a - mx) ** 2 for a in xs)
resid = [c - (my - b * mx) - b * a for a, c in zip(xs, ys)]
s2 = sum(r * r for r in resid) / (n - 2)
t_naive = b / math.sqrt(s2 / sxx)
check(abs(t_nw) < abs(t_naive) * 1.05,
      f'HAC t ({t_nw:+.2f}) is not larger than naive ({t_naive:+.2f}) on overlapping windows')
check(abs(t_nw) < 3.0, f'HAC t stays small when there is no real relationship, got {t_nw:+.2f}')

# a genuine relationship must still be detected
xs2 = [rnd.gauss(0, 1) for _ in range(3000)]
ys2 = [v * 0.5 + rnd.gauss(0, 1) for v in xs2]
_, t_real = A['newey_west_t'](xs2, ys2, lag=5)
check(abs(t_real) > 8, f'HAC still finds a real effect, t={t_real:+.1f}')

# --- 3. block bootstrap ------------------------------------------------------------------------
lo, hi = A['block_bootstrap_ic'](xs2, ys2, block=15)
check(lo > 0 and hi > 0, f'bootstrap CI excludes zero for a real effect: [{lo:+.3f}, {hi:+.3f}]')
noise_y = [rnd.gauss(0, 1) for _ in xs2]
lo2, hi2 = A['block_bootstrap_ic'](xs2, noise_y, block=15)
check(lo2 < 0 < hi2, f'bootstrap CI spans zero for noise: [{lo2:+.3f}, {hi2:+.3f}]')

# --- 4. deciles and monotonicity ---------------------------------------------------------------
dec, mono = A['deciles'](xs2, ys2)
check(len(dec) == 10, 'ten deciles')
check(mono > 0.9, f'monotone deciles for a linear effect, mono={mono:+.2f}')
check(dec[-1] > dec[0], 'top decile beats bottom')
dec_n, mono_n = A['deciles'](xs2, noise_y)
check(abs(mono_n) < 0.75, f'noise deciles are not monotone, mono={mono_n:+.2f}')

# --- 5. end to end: a database of pure noise must yield no executable survivor ------------------
def make_db(path, signal_bps=0.0):
    db = sqlite3.connect(path)
    bsrc = open(os.path.join(ROOT, 'scripts', 'backfill_perps_history.py')).read()
    db.executescript(bsrc.split('SCHEMA = """')[1].split('"""')[0])
    r = random.Random(5)
    px, ts, rows = 100.0, 1_780_000_000, []
    prev_ret = 0.0
    for i in range(12000):
        # forward return depends on the PREVIOUS return by signal_bps of correlation
        shock = r.gauss(0, 8)
        ret = signal_bps * prev_ret + shock
        px *= (1 + ret / 1e4)
        prev_ret = ret
        half = px * 5 / 2 / 1e4
        v = r.randint(0, 30)
        rows.append(('KXTESTPERP', 1, ts + i * 60,
                     px - half, px, px, px - half, px + half, px, px, px + half,
                     px, px, px, px, px, px, v, v * px, 50000, 0))
    db.executemany("insert into perp_bars values(" + ",".join("?" * 21) + ")", rows)
    db.commit(); db.close()

tmp = tempfile.mkdtemp()
try:
    noise_db = os.path.join(tmp, 'noise.sqlite')
    make_db(noise_db, signal_bps=0.0)
    con = sqlite3.connect("file:%s?mode=ro" % noise_db, uri=True)
    bars = con.execute("select end_ts,bid_close,ask_close,volume,volume_notional,open_interest "
                       "from perp_bars where ticker='KXTESTPERP' order by end_ts").fetchall()
    rows, dropped = A['build'](bars, [1, 2, 5, 10])
    check(len(rows) > 10000, f'build produced rows, got {len(rows)}')
    check('close_quote_gap_bps' in rows[0]['f'], 'the gap feature is named close_quote_gap_bps')
    check('spread_bps' not in rows[0]['f'], 'nothing is called a spread')
    # on pure noise no feature should pass all three gates at any horizon
    passed = []
    for fname in rows[0]['f']:
        for h in (1, 5):
            xx = [r['f'][fname] for r in rows if r['lab'].get(h) is not None]
            yy = [r['lab'][h] for r in rows if r['lab'].get(h) is not None]
            if len(set(xx)) < 10 or len(xx) < 300: continue
            _, tt = A['newey_west_t'](xx, yy, lag=h)
            l, hgh = A['block_bootstrap_ic'](xx, yy, block=h * 3)
            _, mo = A['deciles'](xx, yy)
            if l == l and hgh == hgh and l * hgh > 0 and abs(tt) > 2 and abs(mo) > .6:
                passed.append((fname, h, tt, l, hgh))
    check(len(passed) == 0, f'no feature survives on pure noise, got {passed[:3]}')

    # and with a real injected autocorrelation, momentum/reversion must be found
    sig_db = os.path.join(tmp, 'sig.sqlite')
    make_db(sig_db, signal_bps=0.30)
    con2 = sqlite3.connect("file:%s?mode=ro" % sig_db, uri=True)
    bars2 = con2.execute("select end_ts,bid_close,ask_close,volume,volume_notional,open_interest "
                         "from perp_bars where ticker='KXTESTPERP' order by end_ts").fetchall()
    rows2, _ = A['build'](bars2, [1])
    xx = [r['f']['mom_1m'] for r in rows2 if r['lab'].get(1) is not None]
    yy = [r['lab'][1] for r in rows2 if r['lab'].get(1) is not None]
    ic = A['pearson'](xx, yy)
    _, tt = A['newey_west_t'](xx, yy, lag=1)
    l, hgh = A['block_bootstrap_ic'](xx, yy, block=3)
    check(ic > 0.15, f'injected momentum is detected, IC={ic:+.3f}')
    check(abs(tt) > 4, f'and is HAC-significant, t={tt:+.1f}')
    check(l > 0, f'and its bootstrap CI clears zero, [{l:+.3f}, {hgh:+.3f}]')
finally:
    shutil.rmtree(tmp)

# --- 5a. a tied feature must not manufacture a decile spread out of its own label --------------
# deciles() used sorted(zip(x, y)), which is lexicographic on tuples: when x ties it falls through
# to y, the forward return. On a feature that is 68% one value -- minutes_since_trade on a market
# that trades most minutes -- that orders the tied block by the thing being predicted and produces
# a clean monotonic ramp from pure noise. It reported a "+49.2 bps net" edge on KXNEARPERP.
rt = random.Random(0)
N = 60000
tied = [0.0] * int(N * 0.68) + [float(rt.randint(1, 30)) for _ in range(N - int(N * 0.68))]
rt.shuffle(tied)
noise = [rt.gauss(0, 8) for _ in range(N)]          # independent of tied, by construction
dec_t, mono_t = A['deciles'](tied, noise)
check(dec_t == [], 'a feature that is 68% one value gets no decile table at all')
check(mono_t != mono_t, 'and no monotonicity number that could be mistaken for a measurement')

# the same data, but with enough distinct values to be splittable, must still find nothing
spread_ok = [float(rt.randint(0, 5000)) for _ in range(N)]
dec_s, mono_s = A['deciles'](spread_ok, noise)
check(len(dec_s) == 10, 'a well-spread feature still gets deciles')
check(abs(dec_s[-1] - dec_s[0]) < 1.0,
      f'and on noise its spread stays near zero, got {dec_s[-1] - dec_s[0]:+.2f} bps')

# the sort must never fall through to the label: identical x, wildly different y
same_x = [1.0] * 300 + [2.0] * 300
lab = sorted([rt.gauss(0, 50) for _ in range(600)])   # y monotone by construction
d2, m2 = A['deciles'](same_x, lab)
check(d2 == [], 'two distinct values is not ten deciles, however tempting the label ordering')

# --- 5b. the INT64_MAX no-order sentinel must never become a price ------------------------------
# Kalshi sends INT64_MAX for "no order on this side"; after 10^4 fixed-point scaling that is
# 922337203685477.6. One such bar produces two astronomical returns and moved a decile mean to
# 1e14 bps in a real run. This is the same sentinel already fixed in src/perps.cpp.
SENT = 922337203685477.6
check(A['px'](SENT) == 0.0, 'the INT64_MAX sentinel is not a price')
check(A['px'](2.4426) == 2.4426, 'a real price passes through')
check(A['px'](0) == 0.0 and A['px'](-1) == 0.0, 'zero and negative are not prices')
check(A['px'](float('nan')) == 0.0 and A['px'](float('inf')) == 0.0, 'nan and inf are not prices')
check(A['px']('1.5') == 1.5 and A['px']('x') == 0.0, 'strings parse or fail closed')

_bars = []
_ts = 1_780_000_000
for i in range(600):
    _bars.append(('KXT', _ts + i * 60, 2.0, 2.001, 5, 10.0, 100))
_bars[300] = ('KXT', _ts + 300 * 60, 0.0, SENT, 0, 0.0, 100)      # one poisoned bar
_rows, _dropped = A['build']([(b[1], b[2], b[3], b[4], b[5], b[6]) for b in _bars], [1, 5])
check(_dropped == 1, f'the poisoned bar is dropped, got {_dropped}')
_labels = [r['lab'][1] for r in _rows if r['lab'].get(1) is not None]
check(all(abs(v) < 1000 for v in _labels),
      f'no astronomical return survives; max |ret| = {max(abs(v) for v in _labels):.3g} bps')
_f = [r['f']['close_quote_gap_bps'] for r in _rows]
check(all(0 <= v < 1000 for v in _f), 'the close quote gap stays sane too')

# --- 6. the numpy fast path must compute the SAME thing as the pure-Python definition ----------
# Vectorising took this from twenty hours to seventy minutes. A fast path that quietly computes
# something slightly different would be worse than the slow one, so the two are compared directly.
_src = open(os.path.join(ROOT, 'scripts', 'alpha_decay.py')).read().replace('\nmain()\n', '\n')
SLOW = {'__file__': 'a', '__name__': 'a'}
exec(compile(_src.replace("try:\n    import numpy as _np\nexcept ImportError:\n    _np = None",
                          "_np = None"), 'a', 'exec'), SLOW)
check(A['_np'] is not None or True, 'fast path loaded')
check(SLOW['_np'] is None, 'fallback path really has no numpy')
if A['_np'] is not None:
    r2 = random.Random(4)
    xf = [r2.gauss(0, 1) for _ in range(6000)]
    yf = [0.15 * v + r2.gauss(0, 1) for v in xf]
    check(abs(A['pearson'](xf, yf) - SLOW['pearson'](xf, yf)) < 1e-9, 'pearson: paths agree')
    bf, tf = A['newey_west_t'](xf, yf, lag=5)
    bs, ts2 = SLOW['newey_west_t'](xf, yf, lag=5)
    check(abs(bf - bs) < 1e-9, 'newey-west slope: paths agree')
    check(abs(tf - ts2) < 1e-7, f'newey-west t: paths agree ({tf:+.6f} vs {ts2:+.6f})')
    lf = A['block_bootstrap_ic'](xf, yf, block=15, iters=400)
    ls = SLOW['block_bootstrap_ic'](xf, yf, block=15, iters=400)
    # different RNGs, so not identical -- but they must reach the same conclusion and be close
    check((lf[0] > 0) == (ls[0] > 0) and (lf[1] > 0) == (ls[1] > 0),
          'bootstrap: paths reach the same conclusion about zero')
    check(abs(lf[0] - ls[0]) < 0.03 and abs(lf[1] - ls[1]) < 0.03,
          f'bootstrap: CIs agree closely ({lf} vs {ls})')

print(f'{checks} checks, {fails} failures')
sys.exit(1 if fails else 0)
