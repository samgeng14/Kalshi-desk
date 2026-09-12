# Spread-matched control for the conditional maker score

Top 10% of held-out fills, compared against unselected fills from the SAME stratum: instrument x 5-minute bucket x spread decile x volatility quartile.

A positive difference means the score knows something after the spread is held fixed. Zero means the score is a spread filter and nothing else.

## Within-stratum difference in markout

| horizon | strata used | selected matched | clusters | weighted diff | 90% interval | unweighted |
|---|---:|---:|---:|---:|---|---:|
| 1s | 251 of 1147 | 1712 of 1946 | 46 | **-0.25** | [-0.90, +0.19] | -0.32 |
| 5s | 251 of 1147 | 1712 of 1946 | 46 | **+0.09** | [-0.63, +1.08] | +0.29 |
| 10s | 251 of 1147 | 1712 of 1946 | 46 | **+0.08** | [-0.88, +1.34] | +0.27 |
| 60s | 251 of 1147 | 1712 of 1946 | 46 | **+0.72** | [+0.11, +6.11] | +2.92 |

## Verdict

**1 of 4 horizons clears zero; 3 are flat.** The horizons are not independent tests -- they are four looks at the same fills -- so one marginal result among them is close to what chance produces.

The clearing horizon is 60s at +0.72 bps [+0.11, +6.11]. The interval is 9x longer above the estimate than below it, which is a distribution with a tail in it rather than a measurement with a floor under it.

Read together with the flat horizons, this is not evidence that the score knows anything beyond the spread. It is also not proof that it knows nothing. What it is, is too little data: 251 strata drawn from under an hour.

It does not reopen the strategy either way. The round trip is measured at fill prices and is negative regardless of what the score is responding to.

## How to read the matching

- A stratum contributes only if it holds at least one selected and one unselected fill. Strata that do not are excluded, and the counts above say how many selected fills survived that requirement.
- Matching removes the confound it is built for and no other. If the score selects on something correlated with spread that is not in the strata, this comparison still attributes it to the score.
- The comparison is of markouts, never of `kept`. `kept` is the quantity that created the original error.

