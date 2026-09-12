# Target audit on recorded fills

# Target audit: `markout_5000ms`

## Structural

**PASS** -- clean: every price it reads is at or after the entry

## Empirical: scramble the future

**PASS**

| | best pre-entry feature | R^2 |
|---|---|---:|
| real target | `spread_at_signal` | 5.909% |
| future scrambled | `queue_ahead` | 0.022% |
| permutation noise floor | (95th pct of best-of-k) | 0.013% |

Ratio: 0%. Fails above 25% when the shuffled R^2 also clears the noise floor, which is measured for this sample size rather than assumed.

with the future scrambled, the best pre-entry feature explains 0.02% of the target, against a permutation noise floor of 0.01%. Nothing survives the shuffle, so the target is made of the future.

## Why both

The structural audit catches a target that declares itself honestly. The empirical one catches a target that does not, which is the case that matters: nobody writes down "reads a price from before the fill". They write down a formula that happens to.


# Target audit: `kept_5000ms`

## Structural

**FAIL** -- reads 1 price(s) BEFORE the entry, at offsets -1us -- these are known before the trade existed and cannot be part of what the trade earned

## Empirical: scramble the future

**FAIL -- CONTAMINATED**

| | best pre-entry feature | R^2 |
|---|---|---:|
| real target | `spread_at_signal` | 1.912% |
| future scrambled | `spread_at_signal` | 10.381% |
| permutation noise floor | (95th pct of best-of-k) | 0.014% |

Ratio: 543%. Fails above 25% when the shuffled R^2 also clears the noise floor, which is measured for this sample size rather than assumed.

a pre-entry feature (`spread_at_signal`) explains 10.38% of the target even after the future is scrambled -- 543% of what it explains on the real target, against a permutation noise floor of 0.01%. The target contains information that predates the trade.

## Why both

The structural audit catches a target that declares itself honestly. The empirical one catches a target that does not, which is the case that matters: nobody writes down "reads a price from before the fill". They write down a formula that happens to.

