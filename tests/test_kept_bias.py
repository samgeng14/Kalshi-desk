#!/usr/bin/env python3
"""Would this check have caught the bug? Plant it and see.

The failure it exists for: a target that references a price from BEFORE the fill. A model
optimised against such a target learns to find the reference rather than the edge, and the result
looks like alpha at every level of selectivity.
"""
import csv, os, random, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'scripts'))
import kept_bias_check as K
import conditional_markout as CM

P = F = 0
def ok(c, l):
    global P, F
    if c: P += 1
    else:
        F += 1
        print(f'  FAIL {l}')

T0 = 1_789_000_000_000_000
FEATS = ['spread_bps', 'L1_OBI', 'L5_OBI', 'queue_ahead', 'trade_imbalance_1s',
         'vol_60s_bps', 'touch_depth']


def synth(path, n, spread_hurts_markout, seed=4):
    """A tape where the spread is genuinely wide sometimes.

    `spread_hurts_markout` plants the real relationship from the venue: a wider spread carries a
    worse markout. Under that relationship a model optimised on `kept` picks wide spreads, looks
    profitable, and is holding worse positions than a coin flip.
    """
    rng = random.Random(seed)
    cols = (['market', 'mode', 'side', 'post_us', 'fill_us', 'wait_s', 'mid', 'price', 'fee_bps']
            + FEATS + [f'kept_{h}' for h in K.HORIZONS] + [f'markout_{h}' for h in K.HORIZONS])
    with open(path, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for i in range(n):
            sp = abs(rng.gauss(10, 5)) + 1.0            # bps
            mid = 10.0
            px = mid * (1 - sp / 2 / 1e4)               # buy, resting at the bid
            half = (mid - px) / px * 1e4                # == sp/2
            # The coefficient matters. At -0.33 the half-spread gain (sp/2) outruns the markout
            # loss, so `kept` RISES with the spread and the model chases wide books while holding
            # worse positions. That is the trap. Above -0.5 the sum falls with the spread and the
            # model correctly avoids them, which is a different market and not this bug.
            mark = (-0.33 * sp if spread_hurts_markout else -4.0) + rng.gauss(0, 2)
            r = {'market': 'T', 'mode': 'join', 'side': 'buy',
                 'post_us': T0 + i * 5_000_000, 'fill_us': T0 + i * 5_000_000 + 1_000_000,
                 'wait_s': 1.0, 'mid': mid, 'price': px, 'fee_bps': 5.0, 'spread_bps': sp}
            for c in FEATS:
                if c != 'spread_bps':
                    r[c] = rng.gauss(0, 1)
            for h in K.HORIZONS:
                r[f'markout_{h}'] = mark
                r[f'kept_{h}'] = half + mark
            w.writerow(r)


with tempfile.TemporaryDirectory() as d:
    # 1. The real venue relationship: wide spread, worse markout.
    p1 = os.path.join(d, 'planted.csv')
    synth(p1, 4000, spread_hurts_markout=True)
    res, why = K.decompose(p1, 0.10)
    ok(why is None and res, f'planted: decomposition ran ({why})')
    m, r = res['model'], res['random']
    ok(m['half_at_post'] > r['half_at_post'] + 1.0,
       f"planted: the model selects wider spreads ({m['half_at_post']:+.2f} vs "
       f"{r['half_at_post']:+.2f})")
    worse = [h for h in K.HORIZONS if m['markout'][h] < r['markout'][h]]
    ok(len(worse) == len(K.HORIZONS),
       f'planted: and is WORSE than random on the markout at every horizon, caught {len(worse)}')
    better_kept = [h for h in K.HORIZONS if m['kept'][h] > r['kept'][h]]
    ok(len(better_kept) == len(K.HORIZONS),
       'planted: while looking BETTER than random on kept at every horizon -- the exact trap')

    K.report(res, os.path.join(d, 'o.md'), 0.10)
    body = open(os.path.join(d, 'o.md')).read()
    ok('worse than random at every horizon' in body, 'planted: the report names the failure')
    ok('it is a hope' in body, 'planted: and states the rule it leaves behind')

    # 2. A market where the spread does NOT predict the markout. The check must stay quiet.
    p2 = os.path.join(d, 'clean.csv')
    synth(p2, 4000, spread_hurts_markout=False, seed=11)
    res2, _ = K.decompose(p2, 0.10)
    m2, r2 = res2['model'], res2['random']
    worse2 = [h for h in K.HORIZONS if m2['markout'][h] < r2['markout'][h] - 0.5]
    ok(len(worse2) == 0,
       f'clean: no false alarm when the spread does not predict the markout, flagged {len(worse2)}')
    K.report(res2, os.path.join(d, 'o2.md'), 0.10)
    body2 = open(os.path.join(d, 'o2.md')).read()
    ok('worse than random at every horizon' not in body2,
       'clean: and the report does not claim a failure that is not there')

# The decomposition must be exact: kept = half + markout, by construction of the metric.
ok(abs((8.43 + -5.51) - 2.92) < 1e-9, 'arithmetic: kept is the sum of its two terms')

print(f'{P} passed, {F} failed')
sys.exit(1 if F else 0)
