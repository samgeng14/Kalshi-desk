#!/usr/bin/env python3
"""Tests for the conditional maker study.

The failure mode this guards against is not a crash. It is a beautiful top-1% number produced by
leakage, by in-sample ranking, or by a bootstrap that ignores autocorrelation. Each of those is
tested by planting the failure and checking it is caught.
"""
import csv, math, os, random, statistics, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'scripts'))
import conditional_markout as C

P = F = 0
def ok(c, l):
    global P, F
    if c: P += 1
    else:
        F += 1
        print(f'  FAIL {l}')

T0 = 1_789_000_000_000_000

# ---- leakage guard ----------------------------------------------------------------------------
for c in ['kept_1s', 'kept_60s', 'markout_5s', 'markout_300s', 'fill_us', 'wait_s',
          'left_touch', 'fee_bps']:
    ok(C.is_leak(c), f'leak guard: {c} is excluded from the feature set')
for c in ['spread_bps', 'L1_OBI', 'cf_basis_bps', 'vol_60s_bps', 'queue_ahead',
          'trade_imbalance_1s', 'coinbase_basis_bps']:
    ok(not C.is_leak(c), f'leak guard: {c} is a legitimate pre-fill feature')

# ---- ridge ------------------------------------------------------------------------------------
rng = random.Random(3)
X = [[rng.gauss(0, 1), rng.gauss(0, 1)] for _ in range(400)]
y = [3 * a - 2 * b + rng.gauss(0, 0.1) for a, b in X]
w = C.ridge_fit(X, y, lam=1e-6)
ok(w and abs(w[0] - 3) < 0.1 and abs(w[1] + 2) < 0.1, f'ridge: recovers planted weights, got {w}')
ok(C.ridge_fit([], []) is None, 'ridge: empty input returns None')
Xs = [[1.0, 2.0] for _ in range(50)]
ok(C.ridge_fit(Xs, [1.0] * 50, lam=0.0) is None,
   'ridge: a singular system returns None rather than a plausible wrong answer')

# ---- standardiser -----------------------------------------------------------------------------
rows = [{'x': {'good': rng.gauss(0, 2), 'const': 1.0,
               'mostly_missing': (rng.gauss(0, 1) if i % 5 == 0 else None)}}
        for i in range(200)]
st = C.Standardiser()
keep = st.fit(rows, ['good', 'const', 'mostly_missing'])
ok(keep == ['good'], f'standardiser: drops constants and mostly-missing columns, kept {keep}')
v = st.vec(rows[0])
ok(len(v) == 1 and abs(v[0]) < 6, 'standardiser: produces a standardised vector')
ok(abs(st.vec({'x': {'good': None}})[0]) < 1e-9,
   'standardiser: a missing value becomes the mean, which is zero after standardising')

# ---- bootstrap --------------------------------------------------------------------------------
ok(C.block_bootstrap([1.0] * 5) == (None, None, None), 'bootstrap: too few values returns None')
m, lo, hi = C.block_bootstrap([rng.gauss(5, 1) for _ in range(2000)], n=300)
ok(abs(m - 5) < 0.2 and lo < m < hi, f'bootstrap: centred on the mean, got {m:.2f} [{lo:.2f},{hi:.2f}]')
# A block bootstrap on autocorrelated data must be WIDER than one that ignores the correlation.
ac, v_ = [], 0.0
for _ in range(3000):
    v_ = 0.97 * v_ + rng.gauss(0, 1)
    ac.append(v_)
_, lo_b, hi_b = C.block_bootstrap(ac, n=400, block=60)
_, lo_i, hi_i = C.block_bootstrap(ac, n=400, block=1)
ok((hi_b - lo_b) > (hi_i - lo_i),
   f'bootstrap: blocks widen the interval on autocorrelated data '
   f'({hi_b-lo_b:.2f} vs {hi_i-lo_i:.2f}) -- an iid bootstrap here is the classic false positive')

# ---- end to end: pure noise must not produce a winner -------------------------------------------
def synth(path, n, signal=0.0, kept_mean=-5.0, seed=9):
    rnd = random.Random(seed)
    feats = ['spread_bps', 'L1_OBI', 'L5_OBI', 'queue_ahead', 'trade_imbalance_1s',
             'vol_60s_bps', 'cf_basis_bps', 'touch_depth']
    cols = (['market', 'mode', 'side', 'post_us', 'fill_us', 'wait_s', 'mid', 'price', 'fee_bps']
            + feats + [f'kept_{h}' for h in C.HORIZONS] + [f'markout_{h}' for h in C.HORIZONS])
    with open(path, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for i in range(n):
            x = {c: rnd.gauss(0, 1) for c in feats}
            x['spread_bps'] = abs(rnd.gauss(10, 4))
            x['queue_ahead'] = abs(rnd.gauss(200, 80))
            kept = kept_mean + rnd.gauss(0, 6) + signal * x['L1_OBI']
            r = {'market': 'KXBTCPERP', 'mode': 'join', 'side': 'buy',
                 'post_us': T0 + i * 5_000_000, 'fill_us': T0 + i * 5_000_000 + 1_000_000,
                 'wait_s': 1.0, 'mid': 10.0, 'price': 9.99, 'fee_bps': 5.0}
            r.update(x)
            for h in C.HORIZONS:
                r[f'kept_{h}'] = kept
                r[f'markout_{h}'] = kept - 0.25
            w.writerow(r)

with tempfile.TemporaryDirectory() as d:
    fp = os.path.join(d, 'fills.csv')
    synth(fp, 3000, signal=0.0)
    rows, feats = C.load(fp)
    ok(len(rows) == 3000, 'e2e: fills loaded')
    ok('kept_60s' not in feats and 'spread_bps' in feats,
       'e2e: the feature list excludes the target and includes real features')
    old = C.ROOT
    C.ROOT = d
    try:
        res, why = C.run(rows, feats, os.path.join(d, 'o.md'), os.path.join(d, 'o.csv'))
    finally:
        C.ROOT = old
    ok(why is None and res, 'e2e: study ran')
    model = [r for r in res if r['selector'] == 'model' and 'note' not in r]
    pos = [r for r in model if r['net_round_trip_bps'] > 0]
    ok(len(pos) == 0, f'e2e: on pure noise nothing clears a round trip, got {len(pos)}')
    body = open(os.path.join(d, 'o.md')).read()
    ok('Nothing clears a full round trip' in body, 'e2e: and the report says so')
    ok('The entry leg' in body and 'The round trip' in body,
       'e2e: entry leg and round trip are answered separately, not collapsed')
    ok('random' in body and 'spread_only' in body, 'e2e: both nulls are reported alongside')
    ok('held-out' in body, 'e2e: the report states the split')

    # Plant a real signal and confirm the model FINDS it, or the test above proves nothing.
    fp2 = os.path.join(d, 'fills2.csv')
    synth(fp2, 3000, signal=14.0, kept_mean=-5.0, seed=11)
    rows2, feats2 = C.load(fp2)
    old = C.ROOT
    C.ROOT = d
    try:
        res2, _ = C.run(rows2, feats2, os.path.join(d, 'o2.md'), os.path.join(d, 'o2.csv'))
    finally:
        C.ROOT = old
    m2 = {(r['horizon'], r['quantile']): r for r in res2
          if r['selector'] == 'model' and 'note' not in r}
    r2 = {(r['horizon'], r['quantile']): r for r in res2
          if r['selector'] == 'random' and 'note' not in r}
    top = m2.get(('60s', 0.10))
    rnd_top = r2.get(('60s', 0.10))
    ok(top and rnd_top and top['gross_kept_bps'] > rnd_top['gross_kept_bps'] + 3,
       f"power: with a planted signal the model beats random "
       f"({top and top['gross_kept_bps']:.2f} vs {rnd_top and rnd_top['gross_kept_bps']:.2f})")
    ok(top and top['gross_kept_bps'] > m2[('60s', 0.50)]['gross_kept_bps'],
       'power: and selectivity helps -- the top 10% keeps more than the top 50%')

print(f'{P} passed, {F} failed')
sys.exit(1 if F else 0)
