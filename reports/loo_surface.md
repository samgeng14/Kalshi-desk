# Leave-one-out partition arithmetic: the fast version, and why it fails

2026-09-12. `research/loo_surface.py`. The cheap test before any distribution fitting: on an event
whose brackets form a verified exhaustive partition, `p_loo(i) = 1 - sum of the others`. The target
is excluded by construction rather than by discipline, which is the cleanest leave-one-out there
is, and the self-test proves the target's own quote cannot move its own fair value.

Exhaustiveness is verified from the strikes, never from `event_meta.exhaustive`, which is 0 on all
813 events here and carries no information. A ladder is accepted only when bracket widths are
constant, the step between brackets is constant and no wider than a bracket, and both open ends
meet the ladder exactly. That handles integer temperature ladders (`[77,78]` then `[79,80]`) and
continuous price ladders (`[70200, 70299.99]` then `[70300, ...]`) without a tick fudge.

## Result

| | weather | crypto |
|---|---:|---:|
| verified settled partition events | 18 | 114 |
| brackets per event | 6 | 80-188 |
| fully synchronized snapshots (all brackets within 5s) | 4,908 | 381 |
| Brier, market mid | 0.0000 | 0.0057 |
| Brier, leave-one-out | 0.0001 | **0.0460** |
| difference (negative favours the model) | +0.0001 | **+0.0403** |
| 95% CI, event-clustered | [+0.0001, +0.0001] | **[+0.0048, +0.0840]** |
| trades, one per ticker | 105 | 1,000 |
| mean net P&L per trade | -1.31c | **-2.42c** |
| 95% CI | [-4.55, +1.69] | **[-3.16, -1.73]** |

On crypto the leave-one-out estimate is **eight times worse** than simply reading the contract's own
price, with the interval clear of zero on the wrong side. The weather Brier comparison is
degenerate rather than favourable: at the horizons sampled both scores are ~0 because the brackets
are already resolved, so that column carries no information either way.

## The edge buckets, which is the test that matters

| predicted edge | crypto trades | mean predicted | mean realized |
|---|---:|---:|---:|
| 0-1c | 102 | +0.93c | -0.05c |
| 1-2c | 66 | +1.86c | -7.23c |
| 2-3c | 40 | +2.79c | -4.71c |
| 3-5c | 121 | +4.40c | +1.09c |
| 5-8c | 132 | +6.38c | -3.83c |
| 8c+ | 539 | **+28.10c** | **-2.55c** |

A predicted twenty-eight cent edge that realizes minus two and a half cents is not a weak signal.
It is a number that is not measuring edge at all.

## Why, precisely

A partition's prices must sum to one. They do not, and **the entire discrepancy lands on whichever
contract is withheld**, because `p_loo = 1 - (sum - target)`. So the estimator's error is the
accumulated quote error of the other N-1 brackets, and it grows with N.

| | sum of all bracket mids | \|sum - 1\| median | \|sum - 1\| p90 |
|---|---|---:|---:|
| weather, 6 brackets | median 1.0200, sd 0.0157 | 0.0200 | 0.0300 |
| crypto, ~80 brackets | median 1.3250, **mean 2.8218**, sd 3.3177 | 0.3350 | **5.6850** |

On a six-bracket weather ladder the bias is two cents, the same order as the edge being hunted. On
a crypto ladder the sum is a third of a dollar off at the median and **five dollars and sixty-eight
cents off at the ninetieth percentile**. That is the +28c bucket, in full.

## A leak-free correction does not rescue it

The scale factor was estimated on the earliest 50% of events and applied to the untouched latest
25%, chronologically split by settlement date.

| | scale from TRAIN | held-out trades | mean net | 95% CI |
|---|---:|---:|---:|---|
| weather, raw | - | 29 | -0.51c | [-0.69, -0.27] |
| weather, corrected | 1.0200 | 30 | -4.45c | [-6.78, -2.13] |
| crypto, raw | - | 243 | -2.60c | [-3.86, -1.31] |
| crypto, corrected | 2.1050 | 1,566 | -1.50c | [-1.97, -1.23] |

Every variant is negative with an interval clear of zero, and no variant orders the edge buckets.

## Verdict against the pre-registered kill conditions

- **A - does not beat market Brier out of sample.** Fires. Crypto +0.0403, CI clear of zero.
- **B - executable P&L is not positive after fees.** Fires, on every variant and both categories.
- **C - predicted edge is not monotone with realized return.** Fires, most starkly in the 8c+ bucket.

Three of five, independently. The arithmetic baseline is dead.

## What this does not kill, and the one hurdle the next version has to clear

This kills summing point estimates, not the idea that siblings carry information. The defect is
specific and nameable: **an estimator must not let the accumulated error of N-1 quotes land on one
contract.** Any smooth-distribution version has to solve that, and one of the three proposed
estimators structurally does. Fitting a monotone CDF or a parametric family to sibling *mids* has
the same disease in milder form, because it still treats each mid as a point observation. The
max-entropy / discrete-mass version does not, provided it is built with each sibling's **bid and
ask as an inequality band** rather than its mid as an equality: it then asks which distributions
are consistent with every sibling's quoted range, and a wide or stale bracket widens the feasible
set instead of injecting a false point.

That is the version worth building, and it is worth building only because this cheap one failed in
a way that says exactly what to fix. Running it cost an afternoon, which was the point.

Nothing here traded and nothing was wired to MANUAL or AUTO.
