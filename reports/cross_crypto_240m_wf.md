# Cross-crypto return predictability — replication at 240-minute bars

Sources, verified verbatim before any code: Jia, Wu, Yan & Liu, *J Empirical Finance* 74 (2023); Guo, Sang, Tu & Wang, *JEDC* 163 (2024). Specification in `research/literature/cross_crypto_replication.md`.

Panel: 13 assets, 12,843 aligned bars, 2141 days. Chronological split 60% train / 20% validate / 20% test. The penalty is chosen on validation; the test slice is touched once.

## The headline is the economic ratio

`economic_ratio = gross bps per bar / trade cost per bar`, signed. The trade cost is `turnover x round_trip + round_trip / n`: you pay the spread when the position changes sign, plus the unavoidable opening and closing trade amortised over the slice. A ratio of 1.0 means the signal is exactly worth what it costs to trade; below 1 it loses. `economic_ratio_everybar` charges a full round trip on every bar instead -- a strategy nobody would trade, kept as an upper bound on the cost.

**Two gates beside it, both stricter.** `net_lo` is the block-bootstrap 90% lower bound on the gross return minus the trade cost: a specification whose `net_lo` is negative has a cost-adjusted return that is not distinguishable from zero. `excess_vs_constant_bps` is the return over the better of always-long and always-short on the same slice -- because turnover-adjusted costing rewards holding, and the cheapest strategy available is a constant position that pays one round trip in total and collects the drift.

This is a discovery screen. Clearing it makes a signal worth testing, not worth trading: the ratio cannot see tail concentration, instability through time, funding paid while the position is held, or whether a Kalshi contract tracks the underlying return at all.


### AAVEUSDT — Kalshi round trip 38.6 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| LASSO (walk-forward) | 10,274 | +0.0279 | +3.96 | [+0.5, +7.3] | 0.46 | 17.90 | -13.94 | -17.37 | +3.52 [-2.5, +8.8] | **0.2214x** |
| seesaw sign (minus basket) | 10,274 | +0.0302 | +3.43 | [-0.1, +6.9] | 0.52 | 19.89 | -16.46 | -19.98 | +2.99 [-2.1, +7.6] | **0.1727x** |
| own momentum | 10,274 | -0.0276 | -2.87 | [-6.4, +0.5] | 0.52 | 20.21 | -23.09 | -26.57 | -3.32 [-8.4, +1.6] | **-0.1421x** |
| equal-weight basket | 10,274 | -0.0302 | -3.43 | [-6.8, +0.1] | 0.52 | 19.89 | -23.32 | -26.73 | -3.88 [-8.8, +0.9] | **-0.1727x** |
| BTC lagged return | 10,274 | -0.0363 | -5.45 | [-8.8, -2.0] | 0.54 | 20.94 | -26.39 | -29.77 | -5.90 [-10.9, -1.2] | **-0.2603x** |
| leader-only (BTC+ETH) | 10,274 | -0.0340 | -6.49 | [-9.9, -2.9] | 0.54 | 20.75 | -27.24 | -30.61 | -6.93 [-11.8, -2.3] | **-0.3128x** |


### ADAUSDT — Kalshi round trip 35.4 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| seesaw sign (minus basket) | 10,274 | +0.0199 | +0.59 | [-2.5, +3.6] | 0.52 | 18.25 | -17.66 | -20.76 | -1.21 [-5.3, +2.7] | **0.0322x** |
| equal-weight basket | 10,274 | -0.0199 | -0.59 | [-3.6, +2.5] | 0.52 | 18.25 | -18.84 | -21.82 | -2.38 [-6.7, +2.0] | **-0.0322x** |
| own momentum | 10,274 | -0.0130 | -1.34 | [-4.4, +1.8] | 0.52 | 18.55 | -19.89 | -22.91 | -3.13 [-7.2, +1.3] | **-0.0721x** |
| LASSO (walk-forward) | 10,274 | +0.0189 | -1.76 | [-4.8, +1.3] | 0.43 | 15.21 | -16.96 | -19.96 | -3.55 [-8.3, +1.0] | **-0.1154x** |
| BTC lagged return | 10,274 | -0.0225 | -2.23 | [-5.1, +0.7] | 0.54 | 19.21 | -21.44 | -24.32 | -4.02 [-8.2, +0.4] | **-0.1159x** |
| leader-only (BTC+ETH) | 10,274 | -0.0209 | -3.00 | [-5.9, +0.0] | 0.54 | 19.03 | -22.03 | -24.97 | -4.79 [-8.9, -0.5] | **-0.1576x** |


### AVAXUSDT — Kalshi round trip 34.0 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| equal-weight basket | 10,274 | -0.0243 | +0.04 | [-3.1, +3.3] | 0.52 | 17.54 | -17.49 | -20.59 | -2.66 [-7.1, +1.7] | **0.0025x** |
| seesaw sign (minus basket) | 10,274 | +0.0243 | -0.04 | [-3.3, +3.1] | 0.52 | 17.54 | -17.58 | -20.80 | -2.74 [-7.3, +1.6] | **-0.0025x** |
| own momentum | 10,274 | -0.0271 | -0.30 | [-3.4, +2.9] | 0.52 | 17.66 | -17.95 | -21.07 | -3.00 [-7.1, +1.2] | **-0.0168x** |
| LASSO (walk-forward) | 10,274 | +0.0331 | -0.59 | [-3.8, +2.5] | 0.45 | 15.15 | -15.75 | -18.95 | -3.30 [-8.4, +1.5] | **-0.0393x** |
| leader-only (BTC+ETH) | 10,274 | -0.0238 | -2.09 | [-5.4, +1.2] | 0.54 | 18.29 | -20.39 | -23.67 | -4.79 [-9.5, -0.2] | **-0.1144x** |
| BTC lagged return | 10,274 | -0.0280 | -3.02 | [-6.4, +0.3] | 0.54 | 18.46 | -21.49 | -24.87 | -5.72 [-10.4, -1.2] | **-0.1637x** |


### BCHUSDT — Kalshi round trip 40.3 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| LASSO (walk-forward) | 10,274 | +0.0176 | +0.30 | [-2.6, +3.2] | 0.47 | 19.09 | -18.79 | -21.69 | -0.25 [-4.5, +3.6] | **0.0159x** |
| seesaw sign (minus basket) | 10,274 | +0.0266 | +0.33 | [-2.6, +3.0] | 0.52 | 20.79 | -20.46 | -23.40 | -0.22 [-4.3, +3.5] | **0.0158x** |
| equal-weight basket | 10,274 | -0.0266 | -0.33 | [-3.0, +2.6] | 0.52 | 20.79 | -21.12 | -23.83 | -0.88 [-4.8, +3.1] | **-0.0158x** |
| BTC lagged return | 10,274 | -0.0210 | -1.39 | [-4.2, +1.4] | 0.54 | 21.89 | -23.28 | -26.08 | -1.94 [-6.0, +2.0] | **-0.0636x** |
| leader-only (BTC+ETH) | 10,274 | -0.0215 | -2.26 | [-5.0, +0.7] | 0.54 | 21.68 | -23.94 | -26.73 | -2.81 [-6.8, +1.3] | **-0.1042x** |
| own momentum | 10,274 | -0.0299 | -3.19 | [-6.1, -0.3] | 0.53 | 21.32 | -24.51 | -27.44 | -3.74 [-8.0, +0.5] | **-0.1496x** |


### BNBUSDT — Kalshi round trip 33.9 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| seesaw sign (minus basket) | 10,274 | +0.0312 | +0.33 | [-1.6, +2.2] | 0.52 | 17.47 | -17.13 | -19.05 | +0.07 [-2.5, +2.6] | **0.0191x** |
| LASSO (walk-forward) | 10,274 | +0.0104 | -0.11 | [-2.2, +1.9] | 0.28 | 9.45 | -9.55 | -11.66 | -0.36 [-2.3, +1.5] | **-0.0112x** |
| equal-weight basket | 10,274 | -0.0312 | -0.33 | [-2.2, +1.6] | 0.52 | 17.47 | -17.80 | -19.62 | -0.59 [-3.4, +2.3] | **-0.0191x** |
| leader-only (BTC+ETH) | 10,274 | -0.0337 | -1.28 | [-3.3, +0.8] | 0.54 | 18.22 | -19.50 | -21.51 | -1.54 [-4.4, +1.4] | **-0.0703x** |
| own momentum | 10,274 | -0.0479 | -1.53 | [-3.6, +0.5] | 0.52 | 17.63 | -19.17 | -21.19 | -1.79 [-4.7, +1.1] | **-0.0868x** |
| BTC lagged return | 10,274 | -0.0352 | -2.12 | [-4.1, -0.1] | 0.54 | 18.39 | -20.50 | -22.49 | -2.37 [-5.2, +0.6] | **-0.1151x** |


### BTCUSDT — Kalshi round trip 24.6 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| LASSO (walk-forward) | 10,274 | +0.0075 | +0.84 | [-1.0, +2.6] | 0.35 | 8.55 | -7.71 | -9.54 | +0.34 [-1.5, +2.2] | **0.0986x** |
| seesaw sign (minus basket) | 10,274 | +0.0192 | +0.86 | [-0.9, +2.5] | 0.52 | 12.71 | -11.85 | -13.63 | +0.35 [-2.2, +2.8] | **0.0674x** |
| equal-weight basket | 10,274 | -0.0192 | -0.86 | [-2.5, +0.9] | 0.52 | 12.71 | -13.57 | -15.25 | -1.36 [-3.8, +1.2] | **-0.0674x** |
| own momentum | 10,274 | -0.0321 | -1.84 | [-3.5, -0.1] | 0.54 | 13.38 | -15.22 | -16.90 | -2.34 [-4.7, +0.1] | **-0.1372x** |
| BTC lagged return | 10,274 | -0.0321 | -1.84 | [-3.5, -0.1] | 0.54 | 13.38 | -15.22 | -16.90 | -2.34 [-4.7, +0.1] | **-0.1372x** |
| leader-only (BTC+ETH) | 10,274 | -0.0278 | -2.29 | [-4.0, -0.6] | 0.54 | 13.26 | -15.54 | -17.26 | -2.79 [-5.2, -0.1] | **-0.1726x** |


### DOGEUSDT — Kalshi round trip 33.6 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| seesaw sign (minus basket) | 10,274 | +0.0319 | +1.50 | [-1.7, +4.5] | 0.52 | 17.32 | -15.82 | -19.00 | +0.79 [-3.8, +5.2] | **0.0865x** |
| equal-weight basket | 10,274 | -0.0319 | -1.50 | [-4.5, +1.7] | 0.52 | 17.32 | -18.82 | -21.86 | -2.21 [-6.6, +2.2] | **-0.0865x** |
| own momentum | 10,274 | -0.0362 | -1.73 | [-4.7, +1.4] | 0.52 | 17.49 | -19.23 | -22.21 | -2.45 [-7.0, +1.9] | **-0.0992x** |
| LASSO (walk-forward) | 10,274 | +0.0015 | -1.17 | [-4.2, +1.8] | 0.33 | 11.01 | -12.19 | -15.24 | -1.88 [-7.3, +3.4] | **-0.1066x** |
| BTC lagged return | 10,274 | -0.0297 | -4.08 | [-7.0, -1.2] | 0.54 | 18.24 | -22.32 | -25.27 | -4.79 [-9.3, -0.5] | **-0.2238x** |
| leader-only (BTC+ETH) | 10,274 | -0.0283 | -4.13 | [-7.1, -1.2] | 0.54 | 18.07 | -22.20 | -25.18 | -4.84 [-9.2, -0.7] | **-0.2288x** |


### ETHUSDT — Kalshi round trip 25.0 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| equal-weight basket | 10,274 | -0.0101 | +0.46 | [-1.9, +2.8] | 0.52 | 12.90 | -12.43 | -14.78 | +0.00 [-3.4, +3.5] | **0.0358x** |
| LASSO (walk-forward) | 10,274 | +0.0096 | -0.03 | [-2.4, +2.2] | 0.31 | 7.73 | -7.75 | -10.08 | -0.48 [-4.9, +3.5] | **-0.0034x** |
| own momentum | 10,274 | -0.0148 | -0.39 | [-2.7, +2.0] | 0.53 | 13.26 | -13.65 | -15.93 | -0.85 [-4.2, +2.5] | **-0.0297x** |
| seesaw sign (minus basket) | 10,274 | +0.0101 | -0.46 | [-2.8, +1.9] | 0.52 | 12.90 | -13.36 | -15.69 | -0.92 [-4.4, +2.4] | **-0.0358x** |
| leader-only (BTC+ETH) | 10,274 | -0.0167 | -0.83 | [-3.1, +1.5] | 0.54 | 13.45 | -14.28 | -16.54 | -1.28 [-4.6, +2.1] | **-0.0613x** |
| BTC lagged return | 10,274 | -0.0177 | -1.16 | [-3.5, +1.1] | 0.54 | 13.58 | -14.74 | -17.04 | -1.62 [-4.9, +1.6] | **-0.0858x** |


### LINKUSDT — Kalshi round trip 42.6 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| seesaw sign (minus basket) | 10,274 | +0.0369 | +3.73 | [+0.6, +6.8] | 0.52 | 21.97 | -18.24 | -21.39 | +3.19 [-1.3, +7.5] | **0.1697x** |
| LASSO (walk-forward) | 10,274 | +0.0347 | +2.95 | [+0.0, +5.9] | 0.46 | 19.61 | -16.66 | -19.59 | +2.41 [-2.4, +7.0] | **0.1505x** |
| equal-weight basket | 10,274 | -0.0369 | -3.73 | [-6.8, -0.6] | 0.52 | 21.97 | -25.70 | -28.73 | -4.27 [-8.4, -0.1] | **-0.1697x** |
| own momentum | 10,274 | -0.0328 | -4.62 | [-7.7, -1.6] | 0.53 | 22.40 | -27.02 | -30.08 | -5.16 [-9.4, -1.0] | **-0.2061x** |
| BTC lagged return | 10,274 | -0.0449 | -5.88 | [-8.8, -2.9] | 0.54 | 23.13 | -29.01 | -31.91 | -6.42 [-10.5, -2.4] | **-0.2540x** |
| leader-only (BTC+ETH) | 10,274 | -0.0458 | -6.51 | [-9.5, -3.4] | 0.54 | 22.92 | -29.43 | -32.43 | -7.06 [-11.1, -3.0] | **-0.2842x** |


### LTCUSDT — Kalshi round trip 31.2 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| seesaw sign (minus basket) | 10,274 | +0.0271 | +1.16 | [-1.5, +3.9] | 0.52 | 16.07 | -14.91 | -17.59 | +0.08 [-3.9, +4.0] | **0.0724x** |
| LASSO (walk-forward) | 10,274 | +0.0080 | +0.28 | [-2.5, +2.9] | 0.42 | 13.13 | -12.86 | -15.60 | -0.81 [-5.3, +3.3] | **0.0210x** |
| equal-weight basket | 10,274 | -0.0271 | -1.16 | [-3.9, +1.5] | 0.52 | 16.07 | -17.24 | -19.98 | -2.25 [-5.9, +1.3] | **-0.0724x** |
| BTC lagged return | 10,274 | -0.0297 | -2.39 | [-5.0, +0.2] | 0.54 | 16.92 | -19.31 | -21.92 | -3.48 [-7.0, -0.0] | **-0.1411x** |
| own momentum | 10,274 | -0.0284 | -2.50 | [-5.1, +0.3] | 0.52 | 16.24 | -18.74 | -21.35 | -3.59 [-7.3, +0.2] | **-0.1543x** |
| leader-only (BTC+ETH) | 10,274 | -0.0270 | -3.16 | [-5.8, -0.5] | 0.54 | 16.76 | -19.92 | -22.61 | -4.25 [-7.7, -0.7] | **-0.1884x** |


### NEARUSDT — Kalshi round trip 39.5 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| LASSO (walk-forward) | 10,274 | +0.0346 | +3.55 | [-0.4, +7.1] | 0.48 | 18.80 | -15.26 | -19.17 | +2.09 [-4.0, +7.6] | **0.1887x** |
| seesaw sign (minus basket) | 10,274 | +0.0393 | +2.39 | [-1.2, +6.0] | 0.52 | 20.36 | -17.98 | -21.60 | +0.92 [-4.7, +6.1] | **0.1172x** |
| own momentum | 10,274 | -0.0373 | -1.32 | [-4.9, +2.5] | 0.51 | 20.33 | -21.65 | -25.27 | -2.78 [-7.9, +2.6] | **-0.0650x** |
| equal-weight basket | 10,274 | -0.0393 | -2.39 | [-6.0, +1.3] | 0.52 | 20.36 | -22.75 | -26.35 | -3.85 [-8.8, +1.4] | **-0.1172x** |
| leader-only (BTC+ETH) | 10,274 | -0.0340 | -5.54 | [-9.3, -1.8] | 0.54 | 21.24 | -26.78 | -30.53 | -7.00 [-12.4, -1.8] | **-0.2610x** |
| BTC lagged return | 10,274 | -0.0389 | -5.72 | [-9.4, -2.0] | 0.54 | 21.44 | -27.16 | -30.84 | -7.18 [-12.5, -2.0] | **-0.2669x** |


### SOLUSDT — Kalshi round trip 28.2 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| seesaw sign (minus basket) | 10,274 | +0.0355 | +2.26 | [-1.1, +5.7] | 0.52 | 14.57 | -12.30 | -15.69 | +1.72 [-3.6, +6.7] | **0.1555x** |
| LASSO (walk-forward) | 10,274 | +0.0141 | -1.12 | [-4.7, +2.3] | 0.31 | 8.63 | -9.75 | -13.33 | -1.66 [-8.1, +4.4] | **-0.1293x** |
| own momentum | 10,274 | -0.0430 | -2.05 | [-5.2, +1.2] | 0.52 | 14.70 | -16.76 | -19.91 | -2.59 [-7.3, +2.0] | **-0.1396x** |
| equal-weight basket | 10,274 | -0.0355 | -2.26 | [-5.6, +1.1] | 0.52 | 14.57 | -16.83 | -20.22 | -2.81 [-7.5, +1.7] | **-0.1555x** |
| leader-only (BTC+ETH) | 10,274 | -0.0296 | -5.06 | [-8.5, -1.5] | 0.54 | 15.19 | -20.25 | -23.72 | -5.60 [-10.3, -0.7] | **-0.3330x** |
| BTC lagged return | 10,274 | -0.0345 | -5.29 | [-8.8, -1.9] | 0.54 | 15.34 | -20.63 | -24.12 | -5.83 [-10.6, -1.0] | **-0.3449x** |


### XRPUSDT — Kalshi round trip 29.1 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| LASSO (walk-forward) | 10,274 | +0.0228 | +1.16 | [-1.5, +3.8] | 0.44 | 12.81 | -11.65 | -14.31 | +0.66 [-2.8, +4.0] | **0.0908x** |
| seesaw sign (minus basket) | 10,274 | +0.0278 | +0.82 | [-2.0, +3.5] | 0.52 | 15.00 | -14.18 | -16.99 | +0.31 [-4.0, +4.6] | **0.0544x** |
| BTC lagged return | 10,274 | -0.0285 | -0.61 | [-3.3, +2.2] | 0.54 | 15.79 | -16.40 | -19.12 | -1.11 [-4.6, +2.7] | **-0.0388x** |
| equal-weight basket | 10,274 | -0.0278 | -0.82 | [-3.5, +2.0] | 0.52 | 15.00 | -15.82 | -18.54 | -1.32 [-4.9, +2.4] | **-0.0544x** |
| leader-only (BTC+ETH) | 10,274 | -0.0250 | -1.40 | [-4.1, +1.4] | 0.54 | 15.65 | -17.05 | -19.77 | -1.90 [-5.4, +1.9] | **-0.0895x** |
| own momentum | 10,274 | -0.0178 | -1.66 | [-4.6, +1.2] | 0.53 | 15.45 | -17.11 | -20.02 | -2.16 [-5.8, +1.6] | **-0.1072x** |


## Verdict

Best across every focal asset and model: **LASSO (walk-forward)** on ? at **0.221x** of the required effect (+3.96 bp gross against a 38.6 bp round trip).

| gate | specifications passing |
|---|---:|
| economic_ratio > 1 (point estimate) | 0 of 78 |
| and net_lo > 0 (cost-adjusted return distinguishable from zero) | 0 |
| and excess_lo > 0 (beats the better constant position) | 0 |

**Nothing clears the hurdle.** The best specification is worth 22.1% of what it costs to trade on Kalshi.


## What this does not settle

- The papers are intraday on Binance with many more coins and, in the seesaw case, a long-short portfolio across the cross-section. This is a directional per-asset test on 13 coins, which is the version that maps onto a Kalshi position.
- The seesaw direction is large-predicts-small. The small coins are exactly the ones that cost 18-21 bp one way on Kalshi, so a replication that works on Binance can still fail on transfer, and that would be `FAILED_TRANSFER` rather than a contradiction of the paper.
- Costs here are the Kalshi round trip. The papers clear their own venues' costs, which are far lower. Both statements can be true at once.

