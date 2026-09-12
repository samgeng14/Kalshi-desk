#!/usr/bin/env python3
"""Tests for the target audit. The audit is itself a claim, so it gets tested like one."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'scripts'))
import impulse_target_audit as A

P = F = 0
def ok(c, l):
    global P, F
    if c: P += 1
    else:
        F += 1
        print(f'  FAIL {l}')

# The packaged self-test covers the planted bug and the negative control.
ok(A.self_test() == 0, 'self-test: every packaged check passes')

# --- structural ---------------------------------------------------------------------------------
ok(A.structural_audit(A.TargetSpec('ok', [0, 5_000_000]))['passed'],
   'structural: offsets at or after the entry pass')
bad = A.structural_audit(A.TargetSpec('bad', [-250_000, 0, 5_000_000]))
ok(not bad['passed'] and bad['violations'] == [-250_000],
   'structural: a single pre-entry offset fails and is named')
ok('-0.250s' in bad['detail'], 'structural: and the offset is reported in seconds')
ok(A.structural_audit(A.TargetSpec('edge', [0]))['passed'],
   'structural: the entry price itself is allowed -- it is the price actually paid')

# --- empirical: the shuffle must swap shape, not price level --------------------------------------
rows, feats = A.synth(seed=3)
ok(all(abs(v) < 0.2 for r in rows for v in r['future'].values()),
   'shuffle: futures are stored as RETURNS, so swapping them cannot leak a price level')

f_clean, _ = A.markout_target(5_000_000)
f_kept, _ = A.kept_target(5_000_000)
ec, _ = A.empirical_audit(rows, feats, f_clean)
ek, _ = A.empirical_audit(rows, feats, f_kept)
ok(not ec['contaminated'], f"empirical: clean target collapses under shuffle "
   f"({ec['r2_shuffled']:.4f} vs floor {ec['noise_floor']:.4f})")
ok(ek['ratio'] > A.R2_RATIO_MAX, f"empirical: contaminated target survives it ({ek['ratio']:.2f})")

# Seed stability: an audit whose verdict depends on the shuffle draw is not an audit.
verdicts = []
for sd in (1, 2, 3, 4, 5):
    e, _ = A.empirical_audit(rows, feats, f_kept, seed=sd)
    verdicts.append(e['contaminated'])
ok(all(verdicts), 'empirical: the contaminated verdict holds across five shuffle seeds')
verdicts_c = []
for sd in (1, 2, 3, 4, 5):
    e, _ = A.empirical_audit(rows, feats, f_clean, seed=sd)
    verdicts_c.append(e['contaminated'])
ok(not any(verdicts_c), 'empirical: and the clean verdict holds across five too')

# Too little data must return a reason, not a verdict.
short, _ = A.synth(n=20, seed=5)
e, why = A.empirical_audit(short, feats, f_clean)
ok(e is None and why, 'empirical: too few rows returns a reason rather than a confident answer')

# --- a target built ONLY from a pre-entry quantity is the extreme case ---------------------------
def pure_leak(row, future):
    return row['x'].get('spread_at_signal')
el, _ = A.empirical_audit(rows, feats, pure_leak)
ok(el['contaminated'] and el['r2_shuffled'] > 0.9,
   f"extreme: a target that is ONLY a pre-entry feature is caught outright "
   f"({el['r2_shuffled']:.3f})")

# --- report -------------------------------------------------------------------------------------
body = A.audit_report(A.structural_audit(A.TargetSpec('bad', [-1, 0, 5_000_000])), ek, None, 'bad')
ok('FAIL' in body and 'CONTAMINATED' in body, 'report: a failing audit says so in both sections')
ok('before the trade existed' in body, 'report: and explains what the violation means')
ok('noise floor' in body, 'report: and states the floor it measured rather than assumed')

print(f'{P} passed, {F} failed')
sys.exit(1 if F else 0)
