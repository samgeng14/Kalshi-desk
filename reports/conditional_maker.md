# Conditional adverse-selection maker study

48,663 passive fills over 2.2 hours. Fitted on the first 29,197, every number below measured on the held-out 19,466.

Features offered: 79; usable after dropping constants and mostly-missing columns: 66.

## RETRACTION -- read this before the tables

**The target below, `kept`, is not what a one-sided passive entry is worth, and the headline result of this study does not survive correcting it.**

`kept = half_spread_at_POST + markout_from_FILL`. The first term credits the spread against the mid at the moment the order was posted. You only own that spread if you fill without the mid moving -- and the reason you fill is that the mid came to you. The move between posting and filling is charged to neither term.

Measured on this same selection: the model picks states with a median spread of 18.3 bps against 8.2 for a random pick, worth +4.04 bps of post-time credit. On the **markout** -- what the position is worth once you actually hold it -- the model is WORSE than random at every horizon, with separated bootstrap intervals: -5.51 vs -3.86 at 1s, -4.80 vs -4.02 at 5s, -5.33 vs -4.24 at 10s, -7.56 vs -5.09 at 60s.

So the selection is not finding good fills. It is finding wide spreads, and wide spreads carry worse adverse selection -- `reports/execution_summary.md` measured that correlation at **-0.905**. Both halves were already in this repository and were not put together until the round trip forced real fill prices onto both legs.

The round trip itself: `reports/exit_engine.md`, 38,643 priced trades, best combination -24.11 bps, 0 of 40 positive. The decomposition: `reports/kept_bias_check.md`.

What stands from this study: the machinery, the nulls, and the finding that ranking by the fitted model is indistinguishable from ranking by spread alone. What does not stand: that selection finds better fills.

## The question

```
E[kept | state before fill] > maker fee + exit cost ?
```
where `kept = half-spread at entry + signed markout`. Not "will the price rise" -- "if someone hits me here, is that fill toxic".

## Selectivity ladder

`model` is the fitted ranking. `spread_only` ranks by the quoted spread with no fitting. `random` ranks by a random number. All three are applied to the same held-out fills at the same subset sizes, so the columns are directly comparable and the model has to beat both to have done anything.

Intervals are 90% from a moving-block bootstrap.


### kept at 1s

| selector | top | fills | fills/day | gross kept | 90% interval | maker fee | exit | net entry-only | net round trip |
|---|---:|---:|---:|---:|---|---:|---:|---:|---:|
| model | 50% | 9733 | 107191 | +1.09 | [+0.87, +1.21] | 5.0 | 17.2 | -3.91 | **-21.12** |
| model | 20% | 3893 | 42874 | +2.30 | [+1.90, +2.45] | 5.0 | 18.5 | -2.70 | **-21.19** |
| model | 10% | 1946 | 21432 | +3.20 | [+2.56, +3.39] | 5.0 | 19.8 | -1.80 | **-21.55** |
| model | 5% | 973 | 10716 | +4.36 | [+3.30, +4.58] | 5.0 | 20.0 | -0.64 | **-20.64** |
| model | 2% | 389 | 4284 | +6.16 | [+3.97, +6.24] | 5.0 | 20.2 | +1.16 | **-19.07** |
| model | 1% | 194 | 2137 | +8.52 | [+4.43, +8.55] | 5.0 | 20.2 | +3.52 | **-16.71** |
| random | 50% | 9733 | 107191 | +0.26 | [+0.14, +0.39] | 5.0 | 16.4 | -4.74 | **-21.19** |
| random | 20% | 3893 | 42874 | +0.23 | [+0.00, +0.53] | 5.0 | 16.5 | -4.77 | **-21.23** |
| random | 10% | 1946 | 21432 | +0.11 | [-0.10, +0.33] | 5.0 | 16.5 | -4.89 | **-21.36** |
| random | 5% | 973 | 10716 | -0.08 | [-0.32, +0.25] | 5.0 | 16.4 | -5.08 | **-21.46** |
| random | 2% | 389 | 4284 | -0.08 | [-0.46, +0.50] | 5.0 | 16.4 | -5.08 | **-21.49** |
| random | 1% | 194 | 2137 | -0.25 | [-0.99, +0.53] | 5.0 | 16.6 | -5.25 | **-21.87** |
| spread_only | 50% | 9733 | 107191 | +0.86 | [+0.58, +1.04] | 5.0 | 18.7 | -4.14 | **-22.80** |
| spread_only | 20% | 3893 | 42874 | +2.21 | [+1.75, +2.42] | 5.0 | 19.8 | -2.79 | **-22.57** |
| spread_only | 10% | 1946 | 21432 | +3.16 | [+2.43, +3.44] | 5.0 | 20.0 | -1.84 | **-21.82** |
| spread_only | 5% | 973 | 10716 | +4.12 | [+3.05, +4.53] | 5.0 | 20.1 | -0.88 | **-20.99** |
| spread_only | 2% | 389 | 4284 | +5.48 | [+2.97, +6.02] | 5.0 | 20.3 | +0.48 | **-19.79** |
| spread_only | 1% | 194 | 2137 | +8.00 | [+3.62, +8.53] | 5.0 | 20.4 | +3.00 | **-17.40** |

### kept at 5s

| selector | top | fills | fills/day | gross kept | 90% interval | maker fee | exit | net entry-only | net round trip |
|---|---:|---:|---:|---:|---|---:|---:|---:|---:|
| model | 50% | 9733 | 107191 | +1.03 | [+0.75, +1.22] | 5.0 | 17.1 | -3.97 | **-21.10** |
| model | 20% | 3893 | 42874 | +2.34 | [+1.81, +2.64] | 5.0 | 18.0 | -2.66 | **-20.65** |
| model | 10% | 1946 | 21432 | +3.53 | [+2.72, +3.97] | 5.0 | 19.0 | -1.47 | **-20.51** |
| model | 5% | 973 | 10716 | +4.79 | [+3.32, +5.27] | 5.0 | 19.9 | -0.21 | **-20.13** |
| model | 2% | 389 | 4284 | +6.93 | [+3.90, +7.60] | 5.0 | 20.2 | +1.93 | **-18.30** |
| model | 1% | 194 | 2137 | +8.95 | [+3.33, +10.25] | 5.0 | 20.2 | +3.95 | **-16.29** |
| random | 50% | 9733 | 107191 | +0.10 | [-0.04, +0.24] | 5.0 | 16.4 | -4.90 | **-21.35** |
| random | 20% | 3893 | 42874 | +0.14 | [-0.07, +0.41] | 5.0 | 16.5 | -4.86 | **-21.32** |
| random | 10% | 1946 | 21432 | -0.14 | [-0.45, +0.20] | 5.0 | 16.5 | -5.14 | **-21.60** |
| random | 5% | 973 | 10716 | -0.11 | [-0.51, +0.40] | 5.0 | 16.4 | -5.11 | **-21.48** |
| random | 2% | 389 | 4284 | -0.50 | [-0.93, +0.36] | 5.0 | 16.4 | -5.50 | **-21.92** |
| random | 1% | 194 | 2137 | -0.88 | [-1.74, +0.39] | 5.0 | 16.6 | -5.88 | **-22.50** |
| spread_only | 50% | 9733 | 107191 | +0.69 | [+0.36, +0.92] | 5.0 | 18.7 | -4.31 | **-22.98** |
| spread_only | 20% | 3893 | 42874 | +2.51 | [+1.89, +2.82] | 5.0 | 19.8 | -2.49 | **-22.27** |
| spread_only | 10% | 1946 | 21432 | +3.65 | [+2.75, +4.04] | 5.0 | 20.0 | -1.35 | **-21.33** |
| spread_only | 5% | 973 | 10716 | +4.68 | [+3.36, +5.21] | 5.0 | 20.1 | -0.32 | **-20.44** |
| spread_only | 2% | 389 | 4284 | +6.34 | [+3.41, +6.98] | 5.0 | 20.3 | +1.34 | **-18.94** |
| spread_only | 1% | 194 | 2137 | +9.28 | [+3.89, +10.45] | 5.0 | 20.4 | +4.28 | **-16.13** |

### kept at 10s

| selector | top | fills | fills/day | gross kept | 90% interval | maker fee | exit | net entry-only | net round trip |
|---|---:|---:|---:|---:|---|---:|---:|---:|---:|
| model | 50% | 9733 | 107191 | +0.65 | [+0.31, +0.89] | 5.0 | 17.2 | -4.35 | **-21.57** |
| model | 20% | 3893 | 42874 | +1.86 | [+1.20, +2.25] | 5.0 | 17.9 | -3.14 | **-20.99** |
| model | 10% | 1946 | 21432 | +2.68 | [+1.74, +3.18] | 5.0 | 18.1 | -2.32 | **-20.41** |
| model | 5% | 973 | 10716 | +3.79 | [+2.26, +4.40] | 5.0 | 18.7 | -1.21 | **-19.90** |
| model | 2% | 389 | 4284 | +6.07 | [+2.87, +7.04] | 5.0 | 19.2 | +1.07 | **-18.12** |
| model | 1% | 194 | 2137 | +8.77 | [+3.95, +10.77] | 5.0 | 19.4 | +3.77 | **-15.64** |
| random | 50% | 9733 | 107191 | -0.31 | [-0.51, -0.09] | 5.0 | 16.4 | -5.31 | **-21.76** |
| random | 20% | 3893 | 42874 | -0.22 | [-0.47, +0.12] | 5.0 | 16.5 | -5.22 | **-21.67** |
| random | 10% | 1946 | 21432 | -0.42 | [-0.78, +0.01] | 5.0 | 16.5 | -5.42 | **-21.89** |
| random | 5% | 973 | 10716 | -0.45 | [-0.89, +0.25] | 5.0 | 16.4 | -5.45 | **-21.82** |
| random | 2% | 389 | 4284 | -0.59 | [-1.11, +0.74] | 5.0 | 16.4 | -5.59 | **-22.00** |
| random | 1% | 194 | 2137 | -1.15 | [-2.15, +0.78] | 5.0 | 16.6 | -6.15 | **-22.78** |
| spread_only | 50% | 9733 | 107191 | +0.11 | [-0.28, +0.42] | 5.0 | 18.7 | -4.89 | **-23.55** |
| spread_only | 20% | 3893 | 42874 | +1.77 | [+1.03, +2.18] | 5.0 | 19.8 | -3.23 | **-23.02** |
| spread_only | 10% | 1946 | 21432 | +2.79 | [+1.69, +3.34] | 5.0 | 20.0 | -2.21 | **-22.19** |
| spread_only | 5% | 973 | 10716 | +4.21 | [+2.80, +4.95] | 5.0 | 20.1 | -0.79 | **-20.91** |
| spread_only | 2% | 389 | 4284 | +4.94 | [+1.70, +5.76] | 5.0 | 20.3 | -0.06 | **-20.33** |
| spread_only | 1% | 194 | 2137 | +8.35 | [+2.99, +9.77] | 5.0 | 20.4 | +3.35 | **-17.06** |

### kept at 60s

| selector | top | fills | fills/day | gross kept | 90% interval | maker fee | exit | net entry-only | net round trip |
|---|---:|---:|---:|---:|---|---:|---:|---:|---:|
| model | 50% | 9733 | 107191 | -1.57 | [-2.26, -1.01] | 5.0 | 17.3 | -6.57 | **-23.85** |
| model | 20% | 3893 | 42874 | -0.89 | [-1.97, +0.01] | 5.0 | 17.4 | -5.89 | **-23.32** |
| model | 10% | 1946 | 21432 | -1.14 | [-2.91, -0.00] | 5.0 | 17.5 | -6.14 | **-23.65** |
| model | 5% | 973 | 10716 | -0.53 | [-3.43, +0.99] | 5.0 | 17.8 | -5.53 | **-23.32** |
| model | 2% | 389 | 4284 | +0.78 | [-4.69, +3.20] | 5.0 | 18.5 | -4.22 | **-22.71** |
| model | 1% | 194 | 2137 | +6.33 | [+0.20, +10.40] | 5.0 | 19.2 | +1.33 | **-17.91** |
| random | 50% | 9733 | 107191 | -2.25 | [-2.71, -1.81] | 5.0 | 16.4 | -7.25 | **-23.70** |
| random | 20% | 3893 | 42874 | -2.10 | [-2.76, -1.34] | 5.0 | 16.5 | -7.10 | **-23.56** |
| random | 10% | 1946 | 21432 | -2.37 | [-3.37, -1.48] | 5.0 | 16.5 | -7.37 | **-23.84** |
| random | 5% | 973 | 10716 | -2.25 | [-3.54, -0.71] | 5.0 | 16.4 | -7.25 | **-23.62** |
| random | 2% | 389 | 4284 | -2.29 | [-3.89, -0.35] | 5.0 | 16.4 | -7.29 | **-23.71** |
| random | 1% | 194 | 2137 | -2.44 | [-5.08, +0.05] | 5.0 | 16.6 | -7.44 | **-24.07** |
| spread_only | 50% | 9733 | 107191 | -2.58 | [-3.37, -1.92] | 5.0 | 18.7 | -7.58 | **-26.25** |
| spread_only | 20% | 3893 | 42874 | -1.77 | [-3.24, -0.59] | 5.0 | 19.8 | -6.77 | **-26.56** |
| spread_only | 10% | 1946 | 21432 | -0.31 | [-2.47, +1.32] | 5.0 | 20.0 | -5.31 | **-25.29** |
| spread_only | 5% | 973 | 10716 | +3.04 | [+0.14, +5.10] | 5.0 | 20.1 | -1.96 | **-22.08** |
| spread_only | 2% | 389 | 4284 | +0.59 | [-4.81, +3.06] | 5.0 | 20.3 | -4.41 | **-24.68** |
| spread_only | 1% | 194 | 2137 | +4.64 | [-2.56, +7.84] | 5.0 | 20.4 | -0.36 | **-20.77** |

## Verdict

These are two different questions and they have different answers, so they are reported separately rather than collapsed into one.

### The entry leg: selection works

**7 of 24 model subsets keep more than the maker fee on the point estimate, but none has a 90% lower bound clear of it.** The best is the top 1% at 5s: +8.95 bps kept [+3.33, +10.25] against a 5 bps fee, over 194 fills.

That distinction is the finding, and it cuts both ways.

It is not nothing. The unconditional study measured this same quantity at about -2 bps averaged over every fill, and the random-selector column below sits at roughly -0.3 to -2.4 at every subset size. A selected subset keeping +6 to +9 bps against a null that keeps nothing is a real effect, it is monotone in selectivity, and it repeats across all four horizons rather than appearing in one lucky cell. The earlier claim that no model could close the gap was wrong.

It is also not enough to act on. The interval is wide because the top 1% is a couple of hundred fills from a single 2.5-hour session, and a moving-block bootstrap on correlated fills is honest about that. A point estimate that clears a fee by 3.9 bps, with a lower bound that does not, is a reason to record more tape, not a reason to quote.

### The round trip: it does not

**Nothing clears a full round trip.** The best is -15.64 bps at the top 1% of 10s fills, and the gap is the exit: 19.4 bps to cross out.

The asymmetry is the whole result. A maker ENTRY can be selected -- you choose whether to quote, and declining to quote costs nothing. A maker EXIT cannot: once a fill happens you hold inventory and have to get out of it, in whatever state the market is in, not in a state you waited for. Every round-trip figure here therefore prices the exit at the measured cost of crossing.

That is an assumption, not a measurement, and it is the single most valuable thing left to measure. If a position entered in a selected state can be exited passively often enough, the arithmetic changes sign. If it cannot, this lane is closed.

### Did the fitting earn its keep?

Against the one-feature `spread_only` null, the fitted model is ahead in 17 of 24 cells, by +0.14 bps on average.

**That is not a model, it is a spread filter.** Seventy-nine features and a ridge fit reproduce what ranking by the quoted spread alone already does. The finding is real and the machinery around it is not load-bearing: the rule is "quote only when the spread is unusually wide right now", and the ridge weights confirm it -- the largest ones are spread and level counts, which are the same thing said three ways.

That simplification matters for what comes next. A one-feature rule needs far less data to trust, is far harder to overfit, and can be checked by eye on a live book.

## Did the model beat the nulls?

| horizon | top | model | spread_only | random | model minus best null |
|---|---:|---:|---:|---:|---:|
| 1s | 50% | +1.09 | +0.86 | +0.26 | +0.23 |
| 1s | 20% | +2.30 | +2.21 | +0.23 | +0.09 |
| 1s | 10% | +3.20 | +3.16 | +0.11 | +0.04 |
| 1s | 5% | +4.36 | +4.12 | -0.08 | +0.23 |
| 1s | 2% | +6.16 | +5.48 | -0.08 | +0.68 |
| 1s | 1% | +8.52 | +8.00 | -0.25 | +0.52 |
| 5s | 50% | +1.03 | +0.69 | +0.10 | +0.35 |
| 5s | 20% | +2.34 | +2.51 | +0.14 | -0.17 |
| 5s | 10% | +3.53 | +3.65 | -0.14 | -0.12 |
| 5s | 5% | +4.79 | +4.68 | -0.11 | +0.11 |
| 5s | 2% | +6.93 | +6.34 | -0.50 | +0.60 |
| 5s | 1% | +8.95 | +9.28 | -0.88 | -0.33 |
| 10s | 50% | +0.65 | +0.11 | -0.31 | +0.54 |
| 10s | 20% | +1.86 | +1.77 | -0.22 | +0.10 |
| 10s | 10% | +2.68 | +2.79 | -0.42 | -0.11 |
| 10s | 5% | +3.79 | +4.21 | -0.45 | -0.42 |
| 10s | 2% | +6.07 | +4.94 | -0.59 | +1.14 |
| 10s | 1% | +8.77 | +8.35 | -1.15 | +0.43 |
| 60s | 50% | -1.57 | -2.58 | -2.25 | +0.68 |
| 60s | 20% | -0.89 | -1.77 | -2.10 | +0.88 |
| 60s | 10% | -1.14 | -0.31 | -2.37 | -0.83 |
| 60s | 5% | -0.53 | +3.04 | -2.25 | -3.57 |
| 60s | 2% | +0.78 | +0.59 | -2.29 | +0.19 |
| 60s | 1% | +6.33 | +4.64 | -2.44 | +1.69 |

## What the model leans on

Largest standardised ridge weights for `kept_60s`. Signs are readable: a positive weight means more of that feature predicts a fill that keeps more.

| feature | weight |
|---|---:|
| `n_ask_levels` | +2.948 |
| `cancel_vol_30s` | +2.399 |
| `spread_bps` | +2.198 |
| `n_bid_levels` | -2.077 |
| `add_imbalance_1m` | -1.774 |
| `cancel_imbalance_1m` | +1.385 |
| `depth_10_bid` | -1.348 |
| `update_rate_1m` | -1.205 |
| `update_rate_30s` | -1.173 |
| `add_imbalance_30s` | -1.030 |
| `cancel_vol_1m` | -0.995 |
| `cancel_imbalance_30s` | +0.989 |
| `n_mid_changes_60s` | +0.730 |
| `aggressive_buy_1m` | +0.659 |

## What would change this answer

- More tape. 2.5 hours is one session; the top 1% of it is a few dozen fills, and a moving-block interval on a few dozen correlated fills is wide for a good reason.
- A cheaper entry. The gap is measured in maker fee, and the fee is tiered: see `reports/fee_ladder.md` for the volume at which each market crosses zero.
- A cheaper exit. Every round-trip figure here prices the exit at the measured cost of crossing. A strategy that can exit passively pays less, but it also may not get out.
- Nothing here tests whether the SIGNAL changes which fills occur. The orders were placed unconditionally and selected afterwards; a quoting policy that only posts in the selected states faces a different, and probably worse, mix of counterparties.

