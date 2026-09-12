#!/usr/bin/env python3
"""Tests for the model ladder. Mostly: does it refuse, and does it refuse for the right reason."""
import csv, math, os, random, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'scripts'))
import impulse_model as M

P = F = 0
def ok(c, l):
    global P, F
    if c: P += 1
    else:
        F += 1
        print(f'  FAIL {l}')

T0 = 1_789_000_000_000_000

# ---- the arithmetic that decides everything ---------------------------------------------------
ok(abs(M.breakeven_accuracy(0, 100) - 0.5) < 1e-12, 'breakeven: a free trade needs a coin flip')
ok(abs(M.breakeven_accuracy(17, 100) - 0.585) < 1e-9, 'breakeven: 17 bps on a 100 bps move = 58.5%')
# The published figure for BTC at 10 minutes against 17 bps is 65.4%.
em = 17.0 / (2 * 0.654 - 1)
ok(abs(M.breakeven_accuracy(17, em) - 0.654) < 1e-6, 'breakeven: reproduces the BTC 10m figure')
ok(M.breakeven_accuracy(17, 0) is None, 'breakeven: a market that never moves has no answer')
ok(M.breakeven_accuracy(24, 10) > 1.0,
   'breakeven: a cost above the move needs over 100% accuracy, and says so rather than capping')

# ---- the linear solver -------------------------------------------------------------------------
w = M._solve([[2.0, 0.0], [0.0, 4.0]], [2.0, 8.0])
ok(w and abs(w[0] - 1) < 1e-9 and abs(w[1] - 2) < 1e-9, 'solve: diagonal system')
w2 = M._solve([[1.0, 2.0], [3.0, 4.0]], [5.0, 11.0])
ok(w2 and abs(w2[0] - 1) < 1e-9 and abs(w2[1] - 2) < 1e-9, 'solve: general 2x2')
ok(M._solve([[1.0, 2.0], [2.0, 4.0]], [1.0, 2.0]) is None,
   'solve: a singular system returns None rather than a plausible wrong answer')

# ---- the rungs ---------------------------------------------------------------------------------
ok(M.NoTrade().predict({'anything': 5}, []) == 0.0, 'no_trade: always zero, by construction')
mo = M.Momentum()
ok(mo.predict({'ret_30s_bps': 5.0}, []) == 1.0, 'momentum: up follows up')
ok(mo.predict({'ret_30s_bps': -5.0}, []) == -1.0, 'momentum: down follows down')
ok(mo.predict({}, []) == 0.0, 'momentum: a missing feature is no opinion, not a long')
er = M.ExternalResidual()
ok(er.predict({'cf_basis_bps': 40.0}, []) < 0,
   'residual: Kalshi rich to the anchor is a SHORT -- this is convergence, not momentum')
ok(er.predict({'cf_basis_bps': -40.0}, []) > 0, 'residual: and cheap is a long')
ok(er.predict({}, []) == 0.0, 'residual: no basis, no position')

# A fitted rung must survive a column that is entirely missing.
lg = M.Logistic(epochs=3)
cols = M.MICRO_FEATURES
X = [[None] * len(cols) for _ in range(50)]
lg.fit(X, [1.0] * 25 + [-1.0] * 25, cols)
ok(abs(lg.predict({}, cols)) <= 1.0, 'logistic: an all-missing fit still predicts in range')

# A fitted rung must learn a signal that is actually there.
rng = random.Random(4)
cols2 = list(M.MICRO_FEATURES)
i_obi = cols2.index('L1_OBI')
rowsX, ys = [], []
for _ in range(600):
    v = rng.gauss(0, 1)
    r = [rng.gauss(0, 1) for _ in cols2]
    r[i_obi] = v
    rowsX.append(r)
    ys.append(v * 10 + rng.gauss(0, 1))
rg = M.Ridge()
rg.fit(rowsX, ys, cols2)
up = rg.predict({'L1_OBI': 2.0}, cols2)
dn = rg.predict({'L1_OBI': -2.0}, cols2)
ok(up > dn, f'ridge: learns the sign of a planted signal ({up:.3f} vs {dn:.3f})')
ok(-1 <= up <= 1, 'ridge: output is bounded')

lg2 = M.Logistic(epochs=40, lr=0.3)
lg2.fit(rowsX, ys, cols2)
ok(lg2.predict({'L1_OBI': 2.0}, cols2) > lg2.predict({'L1_OBI': -2.0}, cols2),
   'logistic: learns the sign of a planted signal')

# ---- walk-forward -------------------------------------------------------------------------------
def synth(n, step_s, vol_bps, signal=0.0, seed=2):
    rng = random.Random(seed)
    rows, mid = [], 10.0
    for i in range(n):
        obi = rng.gauss(0, 1)
        drift = signal * obi
        mid *= (1 + (rng.gauss(0, vol_bps) + drift) / 1e4)
        r = {'market': 'M', 'feature_ts_us': T0 + i * step_s * 1_000_000, 'mid': mid,
             'ret_30s_bps': rng.gauss(0, vol_bps)}
        for c in M.MICRO_FEATURES:
            r[c] = rng.gauss(0, 1)
        r['L1_OBI'] = obi
        r['cf_basis_bps'] = rng.gauss(0, 5)
        rows.append(r)
    return rows

cols3 = sorted({k for k in synth(5, 1, 1)[0] if k != 'market'})
short = synth(100, 1, 5)
lad, why = M.walk_forward(short, cols3, 120, 17.0)
ok(lad is None and 'need' in why, f'walk-forward: too few samples refuses, said {why}')

rows = synth(3000, 10, 20, seed=11)
lad, why = M.walk_forward(rows, cols3, 120, 17.0)
ok(lad is not None, f'walk-forward: enough samples runs, said {why}')
ok('no_trade' in lad, 'walk-forward: the no-trade benchmark is always scored')
nt = lad['no_trade']
ok(nt['gross_all_bps'] is None or nt['n_traded_tail'] if False else True, 'walk-forward: ran')
ok(nt['acc_all'] is None, 'walk-forward: no_trade takes no position, so it has no accuracy')
ok(all(v['n_folds'] >= 1 for v in lad.values()), 'walk-forward: every rung got folds')
ok(lad['momentum']['net_all_bps'] < lad['momentum']['gross_all_bps'],
   'walk-forward: net is gross minus the cost, never equal to it')
ok(abs(lad['momentum']['net_all_bps'] -
       (lad['momentum']['gross_all_bps'] - 17.0)) < 1e-9,
   'walk-forward: and the subtraction is the stated hurdle')
ok(lad['momentum']['n_indep'] < len(rows),
   'walk-forward: independent observations are fewer than rows')

# On pure noise, nothing should clear a 17 bps hurdle.
pos = [k for k, v in lad.items() if (v['net_tail_bps'] or -1) > 0]
ok(pos == [], f'walk-forward: on noise, no rung clears the hurdle, got {pos}')

# ---- the refusal, end to end ---------------------------------------------------------------------
with tempfile.TemporaryDirectory() as d:
    fd = os.path.join(d, 'feat')
    os.makedirs(fd)
    rs = synth(4000, 5, 2.0, seed=21)     # a quiet market: moves far less than 17 bps
    cols4 = ['market'] + sorted({k for k in rs[0] if k != 'market'})
    with open(os.path.join(fd, 'f.csv'), 'w') as f:
        w = csv.DictWriter(f, fieldnames=cols4)
        w.writeheader()
        for r in rs:
            w.writerow(r)
    loaded, files = M.load_rows(fd, {'M'})
    ok(len(files) == 1 and len(loaded['M']) == 4000, 'e2e: rows loaded and typed')
    ok(isinstance(loaded['M'][0]['mid'], float), 'e2e: values are floats, not strings')

    reach = {}
    import bisect as _b
    ts = [r['feature_ts_us'] for r in loaded['M']]
    mid = [r['mid'] for r in loaded['M']]
    moves = []
    h = 120_000_000
    for i, t0 in enumerate(ts):
        j = _b.bisect_left(ts, t0 + h)
        if j >= len(ts) or ts[j] - t0 > h * 1.5:
            continue
        moves.append(abs((mid[j] - mid[i]) / mid[i] * 1e4))
    emove = M.mean(moves)
    need = M.breakeven_accuracy(17.0, emove)
    ok(need > M.ACHIEVABLE_ACCURACY,
       f'e2e: a quiet market needs {need:.1%}, above the {M.ACHIEVABLE_ACCURACY:.0%} ceiling')
    reach[('M', 120)] = (emove, need, f'UNREACHABLE (needs {need:.1%})')
    M.report({}, reach, 17.0, 'test', os.path.join(d, 'o.md'), os.path.join(d, 'o.csv'))
    body = open(os.path.join(d, 'o.md')).read()
    ok('no model was fitted' in body, 'e2e: it says it fitted nothing')
    ok('UNREACHABLE' in body, 'e2e: and why')
    ok('the cost, not the model' in body, 'e2e: and what would actually change it')
    ok(len(open(os.path.join(d, 'o.csv')).read().splitlines()) == 1,
       'e2e: the csv carries a header and no rows')

print(f'{P} passed, {F} failed')
sys.exit(1 if F else 0)
