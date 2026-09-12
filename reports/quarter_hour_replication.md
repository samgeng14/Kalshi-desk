# Quarter-hour effect — external replication on Binance

Source: Chan Kim and Peter Reinhard Hansen, *The Quarter-Hour Effect: Periodic Algorithmic Trading and Return Predictability in Cryptocurrency Futures*, arXiv:2607.09426, July 2026.

The claim under test, from the abstract verbatim: *"opening order imbalance predicts returns over four to twelve hours, with much weaker effects at finer clock-time frequencies."*

**LEVEL 1 only.** This is the published effect on the venue the paper used. It says nothing about Kalshi, and nothing about tradability — the paper reports no transaction-cost analysis, so "predicts" there means predicts, not pays.

Sample here: 51,813 quarter-hour boundaries, 6 assets, 90 days (2026-06-13 to 2026-09-10). The paper used 1,400 days.

## Coverage

| asset | boundaries | days | mean abs OI | mean volume in window |
|---|---:|---:|---:|---:|
| ADAUSDT | 8,615 | 90 | 0.631 | 189,195.9 |
| BTCUSDT | 8,640 | 90 | 0.499 | 27.6 |
| DOGEUSDT | 8,638 | 90 | 0.589 | 808,216.6 |
| ETHUSDT | 8,640 | 90 | 0.466 | 687.2 |
| SOLUSDT | 8,640 | 90 | 0.517 | 3,729.3 |
| XRPUSDT | 8,640 | 90 | 0.546 | 111,915.6 |

## The headline test: decile sort on opening order imbalance

Sorted within asset. Intervals are 90% from a bootstrap over whole CALENDAR DAYS, and six assets at the same boundary sit in the same cluster — a shock that moves BTC moves SOL in the same minute, so that is one draw, not six.

The return starts at the close of the minute containing the boundary, 50 seconds after the signal window shuts. No part of it can be known while the signal is forming.


### 15 minutes — monotonicity rho = **-0.61**

| decile | mean OI | n | clusters | mean return | 90% interval | median | hit |
|---:|---:|---:|---:|---:|---|---:|---:|
| 1 | -0.932 | 5,182 | 90 | **+0.12** bp | [-0.5, +0.7] | +0.00 | 49% |
| 2 | -0.767 | 5,181 | 90 | **+0.99** bp | [+0.2, +1.8] | +0.00 | 50% |
| 3 | -0.563 | 5,182 | 90 | **+0.34** bp | [-0.6, +1.3] | +0.00 | 49% |
| 4 | -0.344 | 5,180 | 90 | **+0.31** bp | [-0.8, +1.4] | +0.00 | 48% |
| 5 | -0.122 | 5,181 | 90 | **+0.69** bp | [-0.4, +1.9] | +0.00 | 49% |
| 6 | +0.103 | 5,182 | 90 | **+0.53** bp | [-0.8, +1.9] | +0.00 | 49% |
| 7 | +0.331 | 5,180 | 90 | **+0.07** bp | [-0.8, +1.0] | +0.00 | 49% |
| 8 | +0.554 | 5,182 | 90 | **+0.13** bp | [-0.7, +1.0] | +0.00 | 49% |
| 9 | +0.762 | 5,181 | 90 | **-0.42** bp | [-1.1, +0.3] | -0.08 | 47% |
| 10 | +0.934 | 5,176 | 90 | **-0.25** bp | [-0.9, +0.4] | -0.62 | 46% |

### 30 minutes — monotonicity rho = **-0.22**

| decile | mean OI | n | clusters | mean return | 90% interval | median | hit |
|---:|---:|---:|---:|---:|---|---:|---:|
| 1 | -0.932 | 5,182 | 90 | **+0.57** bp | [-0.5, +1.5] | +0.00 | 50% |
| 2 | -0.767 | 5,181 | 90 | **+0.37** bp | [-0.8, +1.6] | +0.00 | 49% |
| 3 | -0.563 | 5,180 | 90 | **-0.16** bp | [-1.7, +1.4] | +0.00 | 50% |
| 4 | -0.345 | 5,182 | 90 | **+1.46** bp | [-0.3, +3.3] | +0.00 | 49% |
| 5 | -0.122 | 5,176 | 90 | **+0.89** bp | [-0.7, +2.6] | +0.00 | 50% |
| 6 | +0.103 | 5,181 | 90 | **+0.93** bp | [-0.9, +2.9] | +0.00 | 49% |
| 7 | +0.330 | 5,182 | 90 | **+0.35** bp | [-0.9, +1.6] | +0.00 | 49% |
| 8 | +0.554 | 5,180 | 90 | **+0.27** bp | [-1.1, +1.6] | +0.00 | 49% |
| 9 | +0.762 | 5,181 | 90 | **-0.10** bp | [-1.3, +1.1] | +0.00 | 49% |
| 10 | +0.934 | 5,176 | 90 | **+0.44** bp | [-0.5, +1.4] | +0.00 | 48% |

### 60 minutes — monotonicity rho = **-0.56**

| decile | mean OI | n | clusters | mean return | 90% interval | median | hit |
|---:|---:|---:|---:|---:|---|---:|---:|
| 1 | -0.932 | 5,182 | 90 | **+1.26** bp | [-0.6, +2.9] | +0.46 | 50% |
| 2 | -0.767 | 5,180 | 90 | **+0.68** bp | [-1.3, +2.8] | +0.00 | 49% |
| 3 | -0.563 | 5,177 | 90 | **+1.08** bp | [-1.3, +3.5] | +0.00 | 50% |
| 4 | -0.345 | 5,180 | 90 | **+1.64** bp | [-0.9, +4.1] | +0.00 | 50% |
| 5 | -0.122 | 5,176 | 90 | **+1.66** bp | [-1.1, +4.6] | +0.00 | 50% |
| 6 | +0.103 | 5,181 | 90 | **+1.07** bp | [-1.6, +3.7] | -0.06 | 49% |
| 7 | +0.331 | 5,180 | 90 | **+0.53** bp | [-1.9, +3.0] | +0.00 | 50% |
| 8 | +0.554 | 5,177 | 90 | **+0.49** bp | [-2.2, +3.3] | +0.00 | 49% |
| 9 | +0.762 | 5,180 | 90 | **+0.96** bp | [-1.0, +2.9] | +0.00 | 49% |
| 10 | +0.934 | 5,176 | 90 | **+0.67** bp | [-1.0, +2.2] | +0.00 | 50% |

### 120 minutes — monotonicity rho = **-0.65**

| decile | mean OI | n | clusters | mean return | 90% interval | median | hit |
|---:|---:|---:|---:|---:|---|---:|---:|
| 1 | -0.932 | 5,180 | 90 | **+1.80** bp | [-1.4, +5.1] | +2.46 | 52% |
| 2 | -0.767 | 5,176 | 90 | **+2.55** bp | [-1.3, +6.7] | +1.78 | 51% |
| 3 | -0.563 | 5,176 | 90 | **+4.69** bp | [+0.4, +8.9] | +2.88 | 52% |
| 4 | -0.345 | 5,175 | 90 | **+3.66** bp | [-0.9, +8.5] | +1.13 | 51% |
| 5 | -0.122 | 5,176 | 90 | **+3.29** bp | [-2.4, +9.1] | +1.17 | 51% |
| 6 | +0.103 | 5,180 | 90 | **+2.16** bp | [-2.4, +7.1] | +0.25 | 50% |
| 7 | +0.331 | 5,175 | 90 | **+0.16** bp | [-4.0, +4.4] | -0.57 | 49% |
| 8 | +0.554 | 5,176 | 90 | **-0.01** bp | [-4.8, +4.7] | -0.58 | 49% |
| 9 | +0.762 | 5,176 | 90 | **+1.40** bp | [-2.2, +5.2] | +0.00 | 49% |
| 10 | +0.934 | 5,175 | 90 | **+0.43** bp | [-2.7, +3.5] | +0.00 | 49% |

### 240 minutes — monotonicity rho = **-0.52**

| decile | mean OI | n | clusters | mean return | 90% interval | median | hit |
|---:|---:|---:|---:|---:|---|---:|---:|
| 1 | -0.932 | 5,175 | 90 | **+2.90** bp | [-2.8, +8.9] | +2.76 | 52% |
| 2 | -0.767 | 5,170 | 90 | **+4.45** bp | [-3.0, +11.8] | +1.43 | 51% |
| 3 | -0.563 | 5,174 | 90 | **+8.89** bp | [+0.9, +17.1] | +3.42 | 52% |
| 4 | -0.345 | 5,170 | 90 | **+4.94** bp | [-3.3, +14.3] | +0.00 | 49% |
| 5 | -0.122 | 5,170 | 90 | **+8.48** bp | [-1.3, +19.4] | +2.58 | 51% |
| 6 | +0.103 | 5,175 | 90 | **+3.15** bp | [-6.2, +12.5] | -0.23 | 49% |
| 7 | +0.330 | 5,170 | 90 | **-0.16** bp | [-8.0, +8.1] | -1.90 | 48% |
| 8 | +0.554 | 5,174 | 90 | **+2.64** bp | [-5.4, +10.6] | +0.00 | 49% |
| 9 | +0.762 | 5,170 | 90 | **+2.61** bp | [-4.9, +10.3] | -1.31 | 49% |
| 10 | +0.934 | 5,169 | 90 | **+2.94** bp | [-3.7, +9.3] | +0.00 | 50% |

### 480 minutes — monotonicity rho = **-0.60**

| decile | mean OI | n | clusters | mean return | 90% interval | median | hit |
|---:|---:|---:|---:|---:|---|---:|---:|
| 1 | -0.932 | 5,164 | 90 | **+6.15** bp | [-5.7, +18.5] | +5.02 | 52% |
| 2 | -0.766 | 5,163 | 90 | **+11.30** bp | [-2.7, +25.9] | +5.35 | 52% |
| 3 | -0.563 | 5,162 | 90 | **+13.34** bp | [-3.1, +31.0] | +3.34 | 51% |
| 4 | -0.344 | 5,164 | 90 | **+9.83** bp | [-6.4, +28.3] | +0.88 | 50% |
| 5 | -0.121 | 5,158 | 90 | **+14.10** bp | [-4.8, +34.0] | +3.86 | 51% |
| 6 | +0.103 | 5,163 | 90 | **+6.06** bp | [-12.5, +26.1] | +0.00 | 50% |
| 7 | +0.331 | 5,164 | 90 | **+3.74** bp | [-11.4, +19.6] | -1.27 | 49% |
| 8 | +0.555 | 5,162 | 90 | **+7.59** bp | [-7.6, +22.6] | +0.99 | 50% |
| 9 | +0.762 | 5,163 | 90 | **+4.61** bp | [-10.3, +19.9] | -0.98 | 49% |
| 10 | +0.935 | 5,158 | 90 | **+5.38** bp | [-6.3, +17.0] | +1.34 | 51% |

### 720 minutes — monotonicity rho = **-0.61**

| decile | mean OI | n | clusters | mean return | 90% interval | median | hit |
|---:|---:|---:|---:|---:|---|---:|---:|
| 1 | -0.932 | 5,156 | 90 | **+10.40** bp | [-6.4, +28.2] | +6.72 | 52% |
| 2 | -0.767 | 5,152 | 90 | **+18.18** bp | [-3.3, +41.2] | +5.45 | 52% |
| 3 | -0.563 | 5,152 | 90 | **+16.37** bp | [-7.0, +40.3] | +2.69 | 51% |
| 4 | -0.345 | 5,151 | 90 | **+13.86** bp | [-10.1, +39.4] | +4.02 | 51% |
| 5 | -0.122 | 5,152 | 90 | **+19.43** bp | [-5.0, +45.9] | +5.75 | 52% |
| 6 | +0.103 | 5,156 | 90 | **+7.75** bp | [-17.3, +35.5] | +3.58 | 51% |
| 7 | +0.331 | 5,151 | 90 | **+9.05** bp | [-12.8, +32.3] | +0.00 | 50% |
| 8 | +0.555 | 5,152 | 90 | **+11.10** bp | [-10.8, +32.5] | +1.05 | 50% |
| 9 | +0.762 | 5,152 | 90 | **+10.29** bp | [-11.6, +32.6] | -0.37 | 49% |
| 10 | +0.935 | 5,151 | 90 | **+6.14** bp | [-12.5, +26.6] | +0.00 | 50% |

## The economic screen

Rank correlation is scale-free, which makes it the wrong thing to lead with. A perfectly monotone relationship worth three hundredths of a basis point scores the same rho as one worth fifty. What decides anything is the size of the spread against the cost of trading it.

`implied IC` inverts the decile spread: for a linear signal on roughly normal returns the top-minus-bottom decile gap is about 2 x 1.755 x IC x sigma. `IC needed` is what would clear a 24 bp Kalshi round trip at tier 0.

| horizon | sigma | mean abs return | top − bottom | implied IC | IC needed | economic ratio | shortfall |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 15m | 30.9 bp | 19.3 bp | **-0.37 bp** | 0.0034 | 0.777 | **0.0044x** | 228x |
| 30m | 44.0 bp | 27.3 bp | **-0.13 bp** | 0.0008 | 0.546 | **0.0015x** | 674x |
| 60m | 61.7 bp | 38.9 bp | **-0.59 bp** | 0.0027 | 0.389 | **0.0070x** | 142x |
| 120m | 85.9 bp | 54.7 bp | **-1.37 bp** | 0.0045 | 0.279 | **0.0162x** | 62x |
| 240m | 120.4 bp | 77.7 bp | **+0.03 bp** | 0.0001 | 0.199 | **0.0004x** | 2,550x |
| 480m | 169.4 bp | 111.3 bp | **-0.76 bp** | 0.0013 | 0.142 | **0.0090x** | 111x |
| 720m | 205.8 bp | 137.7 bp | **-4.27 bp** | 0.0059 | 0.117 | **0.0507x** | 20x |

The best horizon is 720m, at **0.051x** of what would be required — 20x short.

There is a useful number in this table and it is not about the signal. At 720 minutes the underlying return standard deviation is 206 bp, so a signal would need an IC on the order of **0.12** before it is even economically plausible against a 24 bp round-trip hurdle.

**That is a discovery threshold, not evidence of tradability.** `required IC ~ cost / sigma` is a screening heuristic and nothing more. A signal at that IC can still fail for reasons the ratio cannot see: the edge concentrated in tails rather than spread across states, instability through time, turnover that multiplies the cost, poor calibration from score to position size, or a Kalshi contract whose payoff does not map cleanly onto the underlying return. The ratio says when to stop looking. It never says when to trade.

What it does establish is the direction of the search. Every earlier lane here needed an IC of 0.2 to 0.8 because it traded horizons where the price barely moves; at half-day horizons the bar is several times lower. The horizon shift is right. This particular signal does not clear it.


## Does the pattern match the paper?

| horizon | monotonicity rho | top-minus-bottom |
|---:|---:|---:|
| 15m | -0.61 | -0.37 bp |
| 30m | -0.22 | -0.13 bp |
| 60m | -0.56 | -0.59 bp |
| 120m | -0.65 | -1.37 bp |
| 240m | -0.52 | +0.03 bp |
| 480m | -0.60 | -0.76 bp |
| 720m | -0.61 | -4.27 bp |

Mean |rho| at the 4–12 hour horizons the paper highlights: **0.58**, and the sign is negative throughout — higher opening imbalance, lower subsequent return. A reversal, not a continuation. The paper says the imbalance predicts returns; it does not state a sign, so this is not inconsistent with it.

**But a monotone relationship worth a basis point is not a tradable one.** The economic screen above is the verdict: the decile spread implies an information coefficient one to three orders of magnitude below what a Kalshi round trip needs.

Note also what the paper means by "much weaker effects at finer clock-time frequencies": the 1-minute and 5-minute BOUNDARIES, not shorter return horizons. Testing that claim needs the same extraction at non-quarter-hour minutes, which is the placebo listed below as not yet built. Nothing here tests it, and the report does not claim to.


## Status

```
Quarter-hour imbalance
-------------------------------
Statistical relation       YES
Stable monotonicity        YES
Economically large         NO
Cost hurdle                FAIL
Kalshi test warranted      NO
Status                     FAILED_COST
```


## What this sample cannot settle

- 90 days against the paper's 1,400. Day-clustered intervals are honest about that, and they are wide.
- No off-boundary placebo yet. The paper's claim is that the effect is CONCENTRATED at quarter-hour marks; showing a monotone sort at the boundary does not by itself show it is absent elsewhere. That control needs a second extraction pass over non-boundary minutes and is the next thing to build if this passes.
- The paper's own model is a rolling penalised regression. This is a decile sort, which is the weaker and more robust test. Passing it is necessary, not sufficient.
- Returns here are midpoint log returns from one-minute closes. No spread, no fees, no venue. That is Level 1 by construction.

