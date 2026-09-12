#!/usr/bin/env python3
"""Tests for the signal screen. The screen's job is to refuse to produce a finding, so the tests
are mostly about whether the refusals actually fire."""
import csv, math, os, random, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'scripts'))
import btc_eth_screen as S

P = F = 0
def ok(c, l):
    global P, F
    if c: P += 1
    else:
        F += 1
        print(f'  FAIL {l}')

T0 = 1_789_000_000_000_000


# ---- statistics ------------------------------------------------------------------------------
xs = list(range(100))
ok(abs(S.pearson(xs, xs) - 1.0) < 1e-9, 'pearson: perfect correlation is 1')
ok(abs(S.pearson(xs, [-v for v in xs]) + 1.0) < 1e-9, 'pearson: perfect anticorrelation is -1')
ok(S.pearson([1, 2], [1, 2]) is None, 'pearson: too few points returns None, not 1.0')
ok(S.pearson([1.0] * 50, xs[:50]) is None, 'pearson: a constant has no correlation')
ok(abs(S.spearman(xs, [v ** 3 for v in xs]) - 1.0) < 1e-9,
   'spearman: monotone but nonlinear is still 1')
ok(abs(S.norm_cdf(0) - 0.5) < 1e-12 and S.norm_cdf(1.96) > 0.974, 'normal cdf')

# ---- power -----------------------------------------------------------------------------------
ok(S.min_detectable_ic(4) is None, 'power: four observations detect nothing')
m100 = S.min_detectable_ic(103)
ok(abs(m100 - S.POWER_Z / 10.0) < 1e-9, f'power: 103 obs detect {S.POWER_Z/10:.3f}')
ok(S.min_detectable_ic(10003) < m100, 'power: more observations detect smaller effects')
ok(abs(S.min_detectable_ic(10003) - m100 / 10) < 1e-6,
   'power: 100x the observations detects a 10x smaller effect (sqrt law)')

ok(abs(S.ic_needed_for(24.0, 48.0) - 0.5) < 1e-9,
   'hurdle: a 24 bps cost against a 48 bps move needs IC 0.5')
ok(S.ic_needed_for(24.0, 0) is None, 'hurdle: a market that never moves needs no IC, it is None')
ok(S.ic_needed_for(24.0, 4.0) > 1.0,
   'hurdle: a cost larger than the move needs an impossible IC, and says so rather than capping')

# ---- Benjamini-Hochberg ----------------------------------------------------------------------
ok(S.bh([]) is None, 'bh: nothing in, nothing out')
ok(S.bh([0.9, 0.8, 0.7]) is None, 'bh: no small p-values, no survivors')
t = S.bh([0.001] + [0.6] * 99, q=0.10)
ok(t is not None and t >= 0.001, 'bh: one strong result survives among 100')
# Uniform p-values are the null. BH must keep almost nothing.
rng = random.Random(3)
null = [rng.random() for _ in range(2000)]
th = S.bh(null, q=0.10)
kept = sum(1 for p in null if th is not None and p <= th)
ok(kept <= 20, f'bh: on pure noise it keeps at most ~q*m, kept {kept} of 2000')

# ---- forward returns -------------------------------------------------------------------------
rows = [{'feature_ts_us': str(T0 + i * 1_000_000), 'mid': '10.0'} for i in range(1200)]
pairs, n_indep = S.forward_returns(rows, 600)
ok(len(pairs) == 600, f'returns: 1200 one-second rows give 600 ten-minute returns, got {len(pairs)}')
ok(n_indep == 1, f'returns: but only ONE independent observation in 20 minutes, got {n_indep}')
ok(all(abs(r) < 1e-9 for _, r in pairs), 'returns: a flat mid gives zero return')

rows2 = [{'feature_ts_us': str(T0 + i * 1_000_000), 'mid': str(10.0 + i * 0.001)}
         for i in range(400)]
pairs2, _ = S.forward_returns(rows2, 120)
ok(len(pairs2) == 280, f'returns: horizon consumes the tail, got {len(pairs2)}')
ok(pairs2[0][1] > 0, 'returns: a rising mid gives a positive return')

# A hole in the tape must not become a longer return wearing a short label.
rows3 = ([{'feature_ts_us': str(T0 + i * 1_000_000), 'mid': '10.0'} for i in range(5)]
         + [{'feature_ts_us': str(T0 + 3600_000_000 + i * 1_000_000), 'mid': '11.0'}
            for i in range(5)])
pairs3, _ = S.forward_returns(rows3, 120)
ok(len(pairs3) == 0,
   'returns: a 1-hour gap does not silently become a 2-minute return across it')

# ---- the hurdle ------------------------------------------------------------------------------
with tempfile.TemporaryDirectory() as d:
    os.makedirs(os.path.join(d, 'reports'))
    old = S.ROOT
    S.ROOT = d
    try:
        h, src = S.measured_hurdle()
        ok(abs(h - (S.MAKER_BPS + S.TAKER_BPS)) < 1e-9,
           'hurdle: falls back to the fee schedule when nothing was measured')
        ok('NOT measured' in src, 'hurdle: and says loudly that it was not measured')

        with open(os.path.join(d, 'reports', 'execution_simulator.csv'), 'w') as f:
            f.write('market,mode,queue_bound,attempts,fills,entry_cost_bps,fee_bps\n')
            f.write('KXBTCPERP,join,pess,100,40,3.0,5.0\n')      # 8 one way
            f.write('KXBTCPERP,taker,pess,100,100,10.0,12.0\n')  # 22 one way
            f.write('KXBTCPERP,improve,pess,100,3,0.5,5.0\n')    # cheapest, but 3 fills
            f.write('KXBTCPERP,join,opt,100,90,1.0,5.0\n')       # optimistic bound, ignored
        h2, src2 = S.measured_hurdle()
        ok(abs(h2 - 16.0) < 1e-9, f'hurdle: 2 x the cheapest measured one-way, got {h2}')
        ok('measured' in src2 and 'join' in src2, 'hurdle: names where the number came from')
        ok('opt' not in src2, 'hurdle: uses the PESSIMISTIC queue bound, not the flattering one')
    finally:
        S.ROOT = old

# ---- the refusal, end to end -----------------------------------------------------------------
def synth(d, n, step_s, vol_bps, seed=1):
    """A random walk with no signal in it at all."""
    rng = random.Random(seed)
    os.makedirs(d, exist_ok=True)
    mid = 10.0
    cols = ['market', 'feature_ts_us', 'mid'] + S.FEATURES
    with open(os.path.join(d, 'f.csv'), 'w') as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for i in range(n):
            mid *= (1 + rng.gauss(0, vol_bps / 1e4))
            r = {'market': 'KXBTCPERP', 'feature_ts_us': T0 + i * step_s * 1_000_000, 'mid': mid}
            for c in S.FEATURES:
                r[c] = rng.gauss(0, 1)
            w.writerow(r)

with tempfile.TemporaryDirectory() as d:
    fd = os.path.join(d, 'feat')
    # Two and a half hours at one row a second, the tape that actually exists.
    synth(fd, 9000, 1, 1.0)
    rows, files = S.load_rows(fd, {'KXBTCPERP'})
    ok(len(files) == 1 and len(rows['KXBTCPERP']) == 9000, 'e2e: rows loaded')
    # A 24 bps round trip against a market that moves ~11 bps in two minutes: unreachable.
    res, powered = S.screen(rows, 24.0, 'test', os.path.join(d, 'o.csv'), os.path.join(d, 'o.md'))
    body = open(os.path.join(d, 'o.md')).read()
    ok(len(powered) == 0, f'e2e: nothing is tradable, got {len(powered)} powered pairs')
    ok(len(res) == 0, 'e2e: and no feature statistic is computed at all')
    ok('COST EXCEEDS MOVE' in body,
       'e2e: a hurdle larger than the move is named as such, not called underpowered')
    ok('perfect oracle still loses' in body, 'e2e: and the report explains why more tape is moot')
    ok('ranking of noise' in body, 'e2e: and why it refused to rank features')
    header = open(os.path.join(d, 'o.csv')).read().splitlines()
    ok(len(header) == 1, 'e2e: the csv holds a header and no rows, rather than noise')

    # Now make the move big enough that the hurdle is reachable, and the only obstacle is sample
    # size. The verdict must change from unreachable to underpowered.
    fd2 = os.path.join(d, 'feat2')
    synth(fd2, 9000, 1, 30.0, seed=5)
    rows2, _ = S.load_rows(fd2, {'KXBTCPERP'})
    res2, powered2 = S.screen(rows2, 24.0, 'test', os.path.join(d, 'o2.csv'),
                              os.path.join(d, 'o2.md'))
    body2 = open(os.path.join(d, 'o2.md')).read()
    ok('UNDERPOWERED' in body2,
       'e2e: a reachable hurdle on a short tape is UNDERPOWERED, a different diagnosis')
    ok('Hours of tape required' in body2, 'e2e: and it says how much tape would be enough')
    ok(len(powered2) == 0, 'e2e: still nothing powered on 2.5 hours')

    # And with enough independent observations it must actually run, and find nothing in noise.
    fd3 = os.path.join(d, 'feat3')
    synth(fd3, 40000, 30, 30.0, seed=9)          # 40k rows, 30s apart = ~333 hours
    rows3, _ = S.load_rows(fd3, {'KXBTCPERP'})
    res3, powered3 = S.screen(rows3, 24.0, 'test', os.path.join(d, 'o3.csv'),
                              os.path.join(d, 'o3.md'))
    ok(len(powered3) > 0, f'e2e: 333 hours is powered, got {len(powered3)}')
    ok(len(res3) > 0, 'e2e: and feature statistics are computed')
    promising = [r for r in res3 if r['status'] == 'PROMISING']
    ok(len(promising) == 0,
       f'e2e: but pure noise yields no promising signal, got {len(promising)}')

print(f'{P} passed, {F} failed')
sys.exit(1 if F else 0)
