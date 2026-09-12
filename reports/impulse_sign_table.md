# BTC/ETH impulse v1 — sign table and no-model baselines

131 events over 2.17 hours of tape (1 calendar day(s)). No model has been fitted.

## Power, before any table is read

| horizon | events after separation | independent clusters | markets |
|---:|---:|---:|---:|
| 120s | 131 | 75 | 2 |
| 300s | 44 | 34 | 2 |
| 600s | 27 | 21 | 2 |

**This is far too little data to conclude anything.** The tables below are printed so the pipeline is exercised end to end and the shape can be eyeballed, not because 27 events support an inference. Seven magnitude buckets over 27 events is roughly 3 events per cell.

What would be enough: the event rate is set by the cooldown, so at a 2-minute cooldown each market yields at most 30 events an hour and a 10-minute separation leaves 6. Reaching 50 events per bucket at the 10m horizon needs on the order of 2 days of continuous recording per market.

## The premise, tested first

IMPULSE-1 says the external venues take the information before Kalshi does, leaving a move still to come. That is a regression, not a model: `kalshi_return = beta x external_return` over the same window. A beta below 1 with real residual means Kalshi is behind. A beta of 1 with a high R-squared means it is already there.

| window | n | beta | R^2 | mean abs external | mean abs residual | residual / external |
|---|---:|---:|---:|---:|---:|---:|
| 1s | 128 | **+0.943** | 0.743 | 0.83 bp | 0.67 bp | 80% |
| 5s | 130 | **+1.075** | 0.943 | 2.46 bp | 0.77 bp | 31% |
| 30s | 129 | **+1.003** | 0.990 | 7.57 bp | 0.86 bp | 11% |
| 1m | 129 | **+1.005** | 0.997 | 11.12 bp | 0.79 bp | 7% |

**Kalshi has already incorporated the move.** At 1m the slope is +1.005 with an R-squared of 0.997: Kalshi moves one-for-one with the external composite and almost nothing is left unexplained. The residual averages 0.79 bp against an external move averaging 11.12 bp.

That is the hypothesis failing at its premise rather than at its model. There is no lag for a 2m, 5m or 10m trade to close, and no feature set recovers one that the regression says is not there.

The one place a residual survives is at 1 second: beta +0.943, R-squared 0.743, mean absolute residual 0.67 bp. Against a taker round trip of 24 bps, a perfect predictor of that residual still loses 23 bps. It is also the window where a clock offset of tens of milliseconds is a material fraction of the measurement, so part of it is not a real lead at all.


## KXBTCPERP


### 2m horizon — monotonicity (mid) rho=+1.00 over 3 buckets

| external impulse | mean impulse | mean Kalshi lag | events | clusters | mid move | net taker | 90% interval | hit rate |
|---|---:|---:|---:|---:|---:|---:|---|---:|
| 0-5 bp | 3.7 | -0.2 | 27 | 27 | -1.4 | **-26.1** | [-30.2, -22.1] | 0% |
| 5-10 bp | 7.2 | +0.0 | 15 | 15 | +0.1 | **-24.5** | [-33.2, -14.2] | 7% |
| 10-15 bp | 12.4 | -0.0 | 10 | 10 | +0.5 | **-24.1** | [-30.3, -17.6] | 0% |
| 15-20 bp | 17.8 | +0.2 | 7 | 0 |  | **** | n/a | 0% |
| 20-30 bp | 22.6 | +0.5 | 3 | 0 |  | **** | n/a | 33% |
| 30-50 bp | 38.4 | +3.3 | 1 | 0 |  | **** | n/a | 0% |
| 50+ bp | 59.2 | +1.4 | 1 | 0 |  | **** | n/a | 100% |

### 5m horizon — too few populated buckets to rank

| external impulse | mean impulse | mean Kalshi lag | events | clusters | mid move | net taker | 90% interval | hit rate |
|---|---:|---:|---:|---:|---:|---:|---|---:|
| 0-5 bp | 3.6 | +0.0 | 10 | 10 | -6.9 | **-31.6** | [-44.3, -19.7] | 0% |
| 5-10 bp | 7.5 | -0.1 | 5 | 0 |  | **** | n/a | 0% |
| 10-15 bp | 11.4 | +0.4 | 4 | 0 |  | **** | n/a | 25% |
| 15-20 bp | 17.5 | -0.7 | 1 | 0 |  | **** | n/a | 0% |
| 20-30 bp | | | 0 | | | | | |
| 30-50 bp | | | 0 | | | | | |
| 50+ bp | 59.2 | +1.4 | 1 | 0 |  | **** | n/a | 100% |

### 10m horizon — too few populated buckets to rank

| external impulse | mean impulse | mean Kalshi lag | events | clusters | mid move | net taker | 90% interval | hit rate |
|---|---:|---:|---:|---:|---:|---:|---|---:|
| 0-5 bp | 3.6 | -0.1 | 6 | 0 |  | **** | n/a | 33% |
| 5-10 bp | 7.7 | -0.0 | 3 | 0 |  | **** | n/a | 33% |
| 10-15 bp | 13.1 | -0.2 | 2 | 0 |  | **** | n/a | 0% |
| 15-20 bp | | | 0 | | | | | |
| 20-30 bp | | | 0 | | | | | |
| 30-50 bp | | | 0 | | | | | |
| 50+ bp | 59.2 | +1.4 | 1 | 0 |  | **** | n/a | 0% |

### KXBTCPERP — baselines at the 10m horizon

| baseline | events | clusters | mid move | gross taker | net taker | 90% interval | hit | note |
|---|---:|---:|---:|---:|---:|---|---:|---|
| B0 no trade | 13 | None | +0.0 | +0.0 | **+0.0** | [+0.0, +0.0] |  | the benchmark every other row must beat |
| B1 kalshi momentum | 10 | 10 | +14.6 | +13.9 | **-10.1** | [-26.0, +9.3] | 20% | trade the impulse direction only when Kalshi has already started moving that way |
| B2 external composite momentum | 12 | 12 | +12.6 | +11.9 | **-12.1** | [-26.6, +5.2] | 25% | trade the impulse direction on every event |
| B3 external-Kalshi residual | 10 | 10 | +5.1 | +4.4 | **-19.6** | [-30.6, -9.0] | 20% | trade only when Kalshi has NOT yet taken the external move |
| B4 CF-Kalshi residual | 8 | 0 |  |  | **** | n/a | 25% | trade toward the CF anchor |
| B6 shock + lag threshold | 3 | | | | | | | both conditions: a large external move AND Kalshi behind it |


## KXETHPERP


### 2m horizon — monotonicity (mid) rho=-1.00 over 4 buckets

| external impulse | mean impulse | mean Kalshi lag | events | clusters | mid move | net taker | 90% interval | hit rate |
|---|---:|---:|---:|---:|---:|---:|---|---:|
| 0-5 bp | 3.7 | +0.2 | 11 | 11 | +8.6 | **-16.1** | [-24.0, -7.9] | 9% |
| 5-10 bp | 7.5 | -0.4 | 20 | 20 | +1.0 | **-23.8** | [-31.7, -15.1] | 10% |
| 10-15 bp | 12.7 | +0.7 | 11 | 11 | -4.3 | **-29.3** | [-39.9, -19.4] | 0% |
| 15-20 bp | 17.0 | -0.1 | 10 | 10 | -6.7 | **-31.5** | [-44.3, -20.2] | 0% |
| 20-30 bp | 23.2 | -0.2 | 7 | 0 |  | **** | n/a | 0% |
| 30-50 bp | 37.8 | +1.1 | 4 | 0 |  | **** | n/a | 25% |
| 50+ bp | 79.7 | +1.7 | 2 | 0 |  | **** | n/a | 0% |

### 5m horizon — too few populated buckets to rank

| external impulse | mean impulse | mean Kalshi lag | events | clusters | mid move | net taker | 90% interval | hit rate |
|---|---:|---:|---:|---:|---:|---:|---|---:|
| 0-5 bp | 4.0 | -0.8 | 2 | 0 |  | **** | n/a | 0% |
| 5-10 bp | 8.0 | +0.2 | 7 | 0 |  | **** | n/a | 29% |
| 10-15 bp | 13.3 | +0.1 | 4 | 0 |  | **** | n/a | 25% |
| 15-20 bp | 17.5 | -0.4 | 5 | 0 |  | **** | n/a | 0% |
| 20-30 bp | 24.7 | +0.0 | 4 | 0 |  | **** | n/a | 25% |
| 30-50 bp | | | 0 | | | | | |
| 50+ bp | | | 0 | | | | | |

### 10m horizon — too few populated buckets to rank

| external impulse | mean impulse | mean Kalshi lag | events | clusters | mid move | net taker | 90% interval | hit rate |
|---|---:|---:|---:|---:|---:|---:|---|---:|
| 0-5 bp | 3.6 | +0.3 | 2 | 0 |  | **** | n/a | 0% |
| 5-10 bp | 7.8 | -0.4 | 4 | 0 |  | **** | n/a | 50% |
| 10-15 bp | 11.5 | -0.4 | 1 | 0 |  | **** | n/a | 0% |
| 15-20 bp | 17.2 | +0.2 | 2 | 0 |  | **** | n/a | 50% |
| 20-30 bp | 21.4 | -0.8 | 3 | 0 |  | **** | n/a | 0% |
| 30-50 bp | 41.0 | +3.1 | 1 | 0 |  | **** | n/a | 0% |
| 50+ bp | | | 0 | | | | | |

### KXETHPERP — baselines at the 10m horizon

| baseline | events | clusters | mid move | gross taker | net taker | 90% interval | hit | note |
|---|---:|---:|---:|---:|---:|---|---:|---|
| B0 no trade | 14 | None | +0.0 | +0.0 | **+0.0** | [+0.0, +0.0] |  | the benchmark every other row must beat |
| B1 kalshi momentum | 7 | 0 |  |  | **** | n/a | 29% | trade the impulse direction only when Kalshi has already started moving that way |
| B2 external composite momentum | 13 | 13 | +2.3 | +1.5 | **-22.5** | [-44.9, +3.7] | 23% | trade the impulse direction on every event |
| B3 external-Kalshi residual | 5 | | | | | | | trade only when Kalshi has NOT yet taken the external move |
| B4 CF-Kalshi residual | 2 | | | | | | | trade toward the CF anchor |
| B5 BTC shock -> ETH | 7 | | | | | | | ETH follows a large move; BTC cross-feature needs the paired BTC series |
| B6 shock + lag threshold | 4 | | | | | | | both conditions: a large external move AND Kalshi behind it |


## How to read this

- `mid move` is research only. `net taker` is the economics: enter at the ask, exit at the bid, two taker fees at 12 bps each.
- `clusters` is the sample size for the interval. The event count is not.
- Monotonicity is the thing to look for. A column that rises with the impulse bucket is worth modelling; one that wanders is not, and no model recovers a relationship that is not in the sign table.

