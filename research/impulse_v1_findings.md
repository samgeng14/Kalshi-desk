# BTC/ETH impulse v1 — the premise fails

**Kalshi has already incorporated the external move. There is no lag to trade at 2m, 5m or 10m.**

This is the hypothesis failing at its premise rather than at its model, which is why it took one
regression rather than a ladder of them.

## The test

IMPULSE-1 says the external venues take the information before Kalshi does, leaving a move still
to come. That is a regression, not a model:

```
kalshi_return = beta x external_composite_return    (same window, same instant)
```

A beta below 1 with real residual means Kalshi is behind and the rest is still coming. A beta of
1 with a high R-squared means it is already there.

| window | n | beta | R² | mean abs external | mean abs residual | residual / external |
|---|---:|---:|---:|---:|---:|---:|
| 1s | 128 | **+0.943** | 0.743 | 0.83 bp | 0.67 bp | 80% |
| 5s | 130 | **+1.075** | 0.943 | 2.46 bp | 0.77 bp | 31% |
| 30s | 129 | **+1.003** | 0.990 | 7.57 bp | 0.86 bp | 11% |
| 1m | 129 | **+1.005** | 0.997 | 11.12 bp | 0.79 bp | **7%** |

At one minute, Kalshi moves one-for-one with the external composite and 99.7% of the variance is
explained. The residual is 0.79 bp against an 11.12 bp move.

The mean signed lag across all buckets and both markets is within ±0.1 bp of zero. The sign
table's lag column reads −0.2, +0.0, −0.0, +0.2, +0.5 across the impulse buckets it can populate.

## What that costs

A taker round trip is 24 bps at tier 0: enter at the ask, exit at the bid, two 12 bp fees, plus
the spread. The residual available to predict is **0.79 bp at one minute**.

A perfect oracle on that residual loses 23 bps.

This is the same arithmetic that ended the minute-bar study and the maker lane, arriving a third
time from a different direction: the effect is real, it is small, and it is far smaller than the
cost of acting on it.

## The one place a residual survives, and why it does not help

At 1 second the slope is 0.943 with R² 0.743 and the residual is 80% of the external move. Three
reasons that is not an opening:

1. The residual averages **0.67 bp**. Against 24 bps of round trip, a perfect predictor loses 23.
2. The recorder's clock runs **−2 to −49 ms** behind the exchange depending on the window
   sampled. At a 1-second horizon that is up to 5% of the measurement, so part of the apparent
   lead is timestamp error rather than information.
3. A 1-second holding period is not what this lane was about. The horizons pre-registered were
   2m, 5m and 10m, and at those the residual is 7–11%.

## What the sign table shows, and what it cannot

131 events over 2.17 hours, one calendar day, from a 2-minute emission cooldown. After enforcing
separation: 131 events at 2m (75 clusters), 44 at 5m (34), **27 at 10m (21)**.

Seven magnitude buckets over 27 events is about 3 per cell. **The 10m table does not populate at
all**, and the 2m table populates three buckets:

| BTC, 2m | mean impulse | mean lag | events | mid move | net taker |
|---|---:|---:|---:|---:|---:|
| 0–5 bp | 3.7 | −0.2 | 27 | −1.4 | −26.1 |
| 5–10 bp | 7.2 | +0.0 | 15 | +0.1 | −24.5 |
| 10–15 bp | 12.4 | −0.0 | 10 | +0.5 | −24.1 |

The mid move is +0.1 to +0.5 bp for impulses of 5–15 bp. The net taker is −24 to −26, which is
the round trip almost exactly — the move contributes nothing either way.

**What this sample cannot address is the tail.** The hypothesis is about *large synchronised*
moves, and there are 2 events above 30 bp in the entire sample. The regression above covers the
bulk of the distribution, where it is decisive; it says nothing about whether Kalshi lags on a
genuine shock. The distribution of 1-minute external composite moves, sampled during the replay:

| window | p50 | p90 | p95 | p99 |
|---|---:|---:|---:|---:|
| 1s | 0 bp | 2 | 3 | 6 |
| 5s | 1 bp | 6 | 8 | 14 |
| 30s | 5 bp | 15 | 20 | 37 |
| 1m | 7 bp | 22 | 30 | 55 |

A 30–50 bp one-minute move is a p99 event. At the observed rate of roughly 1 per hour above
30 bp, collecting 50 tail events takes on the order of **50 hours of continuous recording**, and
a 10-minute separation on those is the binding constraint, not the trigger.

## Status

`IMPULSE_V1_PREMISE_REJECTED` for the bulk of the distribution at 2m/5m/10m.

Not `ALPHA_REJECTED` outright, because the tail is untested and 27 separated 10m events cannot
reject anything. But the mechanism the lane was built on — Kalshi lagging the external venues —
is measured at beta 1.005, R² 0.997, and that is not a sample-size problem.

## What would reopen it

Only one thing: evidence that the beta falls below 1 **on large moves specifically**. That needs
the tail, which needs days of recording, and the pipeline now produces it automatically as the
tape accumulates. Re-run `impulse_events.py --all` and `impulse_sign_table.py` at 24h, 72h and
7 days; the premise table is the first thing to read each time.

What should NOT happen: fitting a ridge, a logistic and a LightGBM to a residual the regression
says is 0.79 bp. The stop rule in the pre-registration exists for exactly this and it applies.

## What was built

- `scripts/impulse_events.py` — event detection with cooldown, executable taker targets at
  recorded bid/ask, midpoint targets kept separate and labelled research-only, MFE/MAE, and the
  external-composite distribution survey
- `scripts/impulse_sign_table.py` — the premise regression, the sign table, and seven no-model
  baselines, all on cluster bootstraps with the cluster count printed beside the event count
- `tests/test_impulse_events.py` — 25 checks, including that a long exits at the bid and a short
  at the ask, that separation is per market, and that a missing future quote yields no target
  rather than a zero
