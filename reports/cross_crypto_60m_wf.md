# Cross-crypto return predictability — replication at 60-minute bars

Sources, verified verbatim before any code: Jia, Wu, Yan & Liu, *J Empirical Finance* 74 (2023); Guo, Sang, Tu & Wang, *JEDC* 163 (2024). Specification in `research/literature/cross_crypto_replication.md`.

Panel: 13 assets, 51,376 aligned bars, 2141 days. Chronological split 60% train / 20% validate / 20% test. The penalty is chosen on validation; the test slice is touched once.

## The headline is the economic ratio

`economic_ratio = gross bps per bar / trade cost per bar`, signed. The trade cost is `turnover x round_trip + round_trip / n`: you pay the spread when the position changes sign, plus the unavoidable opening and closing trade amortised over the slice. A ratio of 1.0 means the signal is exactly worth what it costs to trade; below 1 it loses. `economic_ratio_everybar` charges a full round trip on every bar instead -- a strategy nobody would trade, kept as an upper bound on the cost.

**Two gates beside it, both stricter.** `net_lo` is the block-bootstrap 90% lower bound on the gross return minus the trade cost: a specification whose `net_lo` is negative has a cost-adjusted return that is not distinguishable from zero. `excess_vs_constant_bps` is the return over the better of always-long and always-short on the same slice -- because turnover-adjusted costing rewards holding, and the cheapest strategy available is a constant position that pays one round trip in total and collects the drift.

This is a discovery screen. Clearing it makes a signal worth testing, not worth trading: the ratio cannot see tail concentration, instability through time, funding paid while the position is held, or whether a Kalshi contract tracks the underlying return at all.


### AAVEUSDT — Kalshi round trip 38.6 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| seesaw sign (minus basket) | 41,100 | +0.0053 | +0.43 | [-0.4, +1.2] | 0.52 | 20.15 | -19.73 | -20.52 | +0.32 [-0.9, +1.5] | **0.0212x** |
| own momentum | 41,100 | -0.0055 | -0.31 | [-1.1, +0.5] | 0.52 | 20.00 | -20.31 | -21.13 | -0.42 [-1.6, +0.7] | **-0.0156x** |
| equal-weight basket | 41,100 | -0.0053 | -0.43 | [-1.2, +0.4] | 0.52 | 20.15 | -20.58 | -21.37 | -0.54 [-1.7, +0.6] | **-0.0212x** |
| BTC lagged return | 41,100 | -0.0076 | -0.74 | [-1.6, +0.1] | 0.53 | 20.43 | -21.17 | -21.99 | -0.85 [-2.0, +0.3] | **-0.0362x** |
| leader-only (BTC+ETH) | 41,100 | -0.0065 | -0.76 | [-1.6, +0.0] | 0.53 | 20.51 | -21.27 | -22.09 | -0.87 [-2.0, +0.3] | **-0.0371x** |
| LASSO (walk-forward) | 41,100 | -0.0055 | -0.67 | [-1.5, +0.2] | 0.41 | 15.70 | -16.37 | -17.23 | -0.78 [-2.2, +0.6] | **-0.0429x** |


### ADAUSDT — Kalshi round trip 35.4 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| LASSO (walk-forward) | 41,100 | -0.0032 | +0.87 | [+0.2, +1.6] | 0.45 | 15.76 | -14.89 | -15.59 | +0.42 [-0.7, +1.5] | **0.0551x** |
| seesaw sign (minus basket) | 41,100 | +0.0099 | +0.44 | [-0.2, +1.1] | 0.52 | 18.49 | -18.05 | -18.70 | -0.01 [-1.0, +1.0] | **0.0238x** |
| equal-weight basket | 41,100 | -0.0099 | -0.44 | [-1.1, +0.2] | 0.52 | 18.49 | -18.93 | -19.61 | -0.89 [-1.9, +0.1] | **-0.0238x** |
| BTC lagged return | 41,100 | -0.0156 | -0.87 | [-1.6, -0.1] | 0.53 | 18.74 | -19.62 | -20.36 | -1.32 [-2.4, -0.3] | **-0.0466x** |
| own momentum | 41,100 | -0.0005 | -0.87 | [-1.6, -0.2] | 0.52 | 18.52 | -19.39 | -20.07 | -1.32 [-2.4, -0.3] | **-0.0469x** |
| leader-only (BTC+ETH) | 41,100 | -0.0150 | -1.10 | [-1.8, -0.4] | 0.53 | 18.81 | -19.91 | -20.62 | -1.55 [-2.6, -0.5] | **-0.0584x** |


### AVAXUSDT — Kalshi round trip 34.0 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| seesaw sign (minus basket) | 41,100 | +0.0038 | +0.47 | [-0.3, +1.2] | 0.52 | 17.77 | -17.30 | -18.03 | -0.21 [-1.3, +0.8] | **0.0263x** |
| LASSO (walk-forward) | 41,100 | +0.0223 | +0.17 | [-0.6, +1.0] | 0.45 | 15.38 | -15.21 | -15.98 | -0.50 [-1.8, +0.8] | **0.0113x** |
| own momentum | 41,100 | -0.0147 | -0.08 | [-0.9, +0.7] | 0.52 | 17.73 | -17.81 | -18.59 | -0.75 [-1.9, +0.4] | **-0.0044x** |
| equal-weight basket | 41,100 | -0.0038 | -0.47 | [-1.2, +0.3] | 0.52 | 17.77 | -18.24 | -18.96 | -1.14 [-2.2, -0.0] | **-0.0263x** |
| BTC lagged return | 41,100 | -0.0119 | -1.10 | [-1.9, -0.3] | 0.53 | 18.01 | -19.12 | -19.91 | -1.78 [-2.9, -0.6] | **-0.0612x** |
| leader-only (BTC+ETH) | 41,100 | -0.0108 | -1.15 | [-1.9, -0.4] | 0.53 | 18.08 | -19.23 | -20.00 | -1.83 [-3.0, -0.7] | **-0.0637x** |


### BCHUSDT — Kalshi round trip 40.3 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| LASSO (walk-forward) | 41,100 | +0.0138 | +0.18 | [-0.5, +0.9] | 0.46 | 18.71 | -18.53 | -19.23 | +0.04 [-0.9, +1.0] | **0.0096x** |
| seesaw sign (minus basket) | 41,100 | -0.0009 | +0.16 | [-0.5, +0.8] | 0.52 | 21.06 | -20.90 | -21.53 | +0.02 [-0.9, +0.9] | **0.0076x** |
| equal-weight basket | 41,100 | +0.0009 | -0.16 | [-0.8, +0.5] | 0.52 | 21.06 | -21.22 | -21.86 | -0.30 [-1.3, +0.7] | **-0.0076x** |
| own momentum | 41,100 | -0.0033 | -0.47 | [-1.2, +0.2] | 0.52 | 21.15 | -21.62 | -22.30 | -0.61 [-1.6, +0.4] | **-0.0222x** |
| BTC lagged return | 41,100 | -0.0003 | -0.58 | [-1.3, +0.1] | 0.53 | 21.35 | -21.93 | -22.61 | -0.71 [-1.7, +0.3] | **-0.0270x** |
| leader-only (BTC+ETH) | 41,100 | -0.0024 | -1.05 | [-1.7, -0.4] | 0.53 | 21.43 | -22.48 | -23.14 | -1.19 [-2.1, -0.3] | **-0.0491x** |


### BNBUSDT — Kalshi round trip 33.9 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| seesaw sign (minus basket) | 41,100 | +0.0093 | +0.64 | [+0.2, +1.1] | 0.52 | 17.70 | -17.06 | -17.51 | +0.57 [-0.1, +1.2] | **0.0361x** |
| own momentum | 41,100 | -0.0071 | -0.22 | [-0.7, +0.2] | 0.52 | 17.66 | -17.88 | -18.32 | -0.28 [-0.9, +0.4] | **-0.0125x** |
| equal-weight basket | 41,100 | -0.0093 | -0.64 | [-1.1, -0.2] | 0.52 | 17.70 | -18.34 | -18.78 | -0.70 [-1.4, -0.0] | **-0.0361x** |
| BTC lagged return | 41,100 | -0.0133 | -0.71 | [-1.2, -0.2] | 0.53 | 17.94 | -18.65 | -19.13 | -0.77 [-1.5, -0.1] | **-0.0396x** |
| leader-only (BTC+ETH) | 41,100 | -0.0121 | -0.81 | [-1.3, -0.3] | 0.53 | 18.01 | -18.81 | -19.29 | -0.87 [-1.6, -0.2] | **-0.0448x** |
| LASSO (walk-forward) | 41,100 | -0.0133 | -0.39 | [-0.9, +0.1] | 0.19 | 6.33 | -6.73 | -7.23 | -0.46 [-0.9, -0.0] | **-0.0622x** |


### BTCUSDT — Kalshi round trip 24.6 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| seesaw sign (minus basket) | 41,100 | +0.0015 | +0.25 | [-0.1, +0.6] | 0.52 | 12.88 | -12.63 | -13.03 | +0.12 [-0.5, +0.7] | **0.0193x** |
| LASSO (walk-forward) | 41,100 | -0.0037 | -0.05 | [-0.5, +0.4] | 0.36 | 8.96 | -9.01 | -9.45 | -0.18 [-0.7, +0.3] | **-0.0056x** |
| equal-weight basket | 41,100 | -0.0015 | -0.25 | [-0.6, +0.2] | 0.52 | 12.88 | -13.13 | -13.52 | -0.37 [-0.9, +0.2] | **-0.0193x** |
| own momentum | 41,100 | -0.0112 | -0.32 | [-0.7, +0.1] | 0.53 | 13.05 | -13.38 | -13.78 | -0.45 [-1.0, +0.1] | **-0.0248x** |
| BTC lagged return | 41,100 | -0.0112 | -0.32 | [-0.7, +0.1] | 0.53 | 13.05 | -13.38 | -13.78 | -0.45 [-1.0, +0.1] | **-0.0248x** |
| leader-only (BTC+ETH) | 41,100 | -0.0093 | -0.49 | [-0.9, -0.1] | 0.53 | 13.10 | -13.59 | -14.00 | -0.61 [-1.2, -0.0] | **-0.0372x** |


### DOGEUSDT — Kalshi round trip 33.6 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| seesaw sign (minus basket) | 41,100 | +0.0109 | +0.95 | [+0.2, +1.7] | 0.52 | 17.55 | -16.60 | -17.33 | +0.77 [-0.3, +1.9] | **0.0540x** |
| LASSO (walk-forward) | 41,100 | -0.0003 | -0.03 | [-0.8, +0.7] | 0.30 | 10.03 | -10.06 | -10.85 | -0.20 [-1.6, +1.1] | **-0.0026x** |
| equal-weight basket | 41,100 | -0.0109 | -0.95 | [-1.7, -0.2] | 0.52 | 17.55 | -18.50 | -19.23 | -1.13 [-2.2, -0.1] | **-0.0540x** |
| own momentum | 41,100 | -0.0159 | -1.10 | [-1.8, -0.4] | 0.53 | 17.67 | -18.77 | -19.50 | -1.28 [-2.3, -0.3] | **-0.0623x** |
| BTC lagged return | 41,100 | -0.0131 | -1.46 | [-2.3, -0.7] | 0.53 | 17.79 | -19.26 | -20.05 | -1.64 [-2.8, -0.5] | **-0.0823x** |
| leader-only (BTC+ETH) | 41,100 | -0.0137 | -1.53 | [-2.3, -0.8] | 0.53 | 17.86 | -19.39 | -20.15 | -1.71 [-2.7, -0.7] | **-0.0859x** |


### ETHUSDT — Kalshi round trip 25.0 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| equal-weight basket | 41,100 | +0.0079 | +0.27 | [-0.3, +0.8] | 0.52 | 13.07 | -12.79 | -13.32 | +0.16 [-0.7, +1.0] | **0.0208x** |
| LASSO (walk-forward) | 41,100 | -0.0029 | +0.05 | [-0.5, +0.6] | 0.37 | 9.26 | -9.22 | -9.80 | -0.07 [-1.1, +0.9] | **0.0053x** |
| own momentum | 41,100 | +0.0021 | +0.01 | [-0.5, +0.5] | 0.53 | 13.26 | -13.25 | -13.78 | -0.10 [-0.9, +0.7] | **0.0010x** |
| BTC lagged return | 41,100 | -0.0009 | -0.06 | [-0.6, +0.5] | 0.53 | 13.25 | -13.31 | -13.85 | -0.18 [-1.0, +0.6] | **-0.0047x** |
| seesaw sign (minus basket) | 41,100 | -0.0079 | -0.27 | [-0.8, +0.3] | 0.52 | 13.07 | -13.34 | -13.87 | -0.39 [-1.2, +0.4] | **-0.0208x** |
| leader-only (BTC+ETH) | 41,100 | +0.0008 | -0.29 | [-0.8, +0.2] | 0.53 | 13.29 | -13.58 | -14.13 | -0.40 [-1.2, +0.4] | **-0.0217x** |


### LINKUSDT — Kalshi round trip 42.6 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| seesaw sign (minus basket) | 41,100 | +0.0074 | +0.97 | [+0.3, +1.7] | 0.52 | 22.26 | -21.30 | -21.98 | +0.83 [-0.2, +1.8] | **0.0434x** |
| LASSO (walk-forward) | 41,100 | +0.0040 | -0.14 | [-0.9, +0.6] | 0.45 | 19.05 | -19.19 | -19.94 | -0.27 [-1.4, +0.8] | **-0.0072x** |
| equal-weight basket | 41,100 | -0.0074 | -0.97 | [-1.7, -0.3] | 0.52 | 22.26 | -23.23 | -23.92 | -1.10 [-2.1, -0.1] | **-0.0434x** |
| own momentum | 41,100 | -0.0076 | -1.10 | [-1.8, -0.4] | 0.52 | 22.30 | -23.40 | -24.12 | -1.24 [-2.3, -0.2] | **-0.0493x** |
| BTC lagged return | 41,100 | -0.0116 | -1.43 | [-2.2, -0.7] | 0.53 | 22.57 | -24.00 | -24.75 | -1.57 [-2.6, -0.5] | **-0.0636x** |
| leader-only (BTC+ETH) | 41,100 | -0.0121 | -1.51 | [-2.3, -0.8] | 0.53 | 22.65 | -24.17 | -24.90 | -1.65 [-2.7, -0.7] | **-0.0668x** |


### LTCUSDT — Kalshi round trip 31.2 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| seesaw sign (minus basket) | 41,100 | +0.0083 | +0.96 | [+0.3, +1.6] | 0.52 | 16.29 | -15.33 | -15.94 | +0.69 [-0.2, +1.6] | **0.0588x** |
| LASSO (walk-forward) | 41,100 | +0.0142 | +0.60 | [-0.1, +1.3] | 0.48 | 15.00 | -14.40 | -15.05 | +0.33 [-0.7, +1.3] | **0.0401x** |
| equal-weight basket | 41,100 | -0.0083 | -0.96 | [-1.6, -0.3] | 0.52 | 16.29 | -17.24 | -17.86 | -1.23 [-2.1, -0.3] | **-0.0588x** |
| own momentum | 41,100 | -0.0184 | -1.14 | [-1.8, -0.5] | 0.53 | 16.44 | -17.58 | -18.22 | -1.41 [-2.3, -0.5] | **-0.0692x** |
| BTC lagged return | 41,100 | -0.0124 | -1.43 | [-2.1, -0.8] | 0.53 | 16.51 | -17.94 | -18.57 | -1.71 [-2.6, -0.8] | **-0.0868x** |
| leader-only (BTC+ETH) | 41,100 | -0.0124 | -1.58 | [-2.2, -1.0] | 0.53 | 16.57 | -18.15 | -18.78 | -1.85 [-2.7, -1.0] | **-0.0952x** |


### NEARUSDT — Kalshi round trip 39.5 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| LASSO (walk-forward) | 41,100 | +0.0259 | +1.06 | [+0.2, +2.0] | 0.48 | 19.12 | -18.06 | -18.96 | +0.70 [-0.7, +2.1] | **0.0554x** |
| seesaw sign (minus basket) | 41,100 | +0.0127 | +0.65 | [-0.2, +1.5] | 0.52 | 20.63 | -19.98 | -20.85 | +0.29 [-1.0, +1.5] | **0.0315x** |
| equal-weight basket | 41,100 | -0.0127 | -0.65 | [-1.5, +0.2] | 0.52 | 20.63 | -21.28 | -22.14 | -1.01 [-2.3, +0.3] | **-0.0315x** |
| own momentum | 41,100 | -0.0172 | -0.85 | [-1.7, +0.0] | 0.52 | 20.46 | -21.32 | -22.19 | -1.21 [-2.5, +0.1] | **-0.0417x** |
| BTC lagged return | 41,100 | -0.0178 | -1.59 | [-2.5, -0.7] | 0.53 | 20.92 | -22.51 | -23.43 | -1.95 [-3.3, -0.6] | **-0.0761x** |
| leader-only (BTC+ETH) | 41,100 | -0.0164 | -1.68 | [-2.6, -0.8] | 0.53 | 21.00 | -22.67 | -23.58 | -2.04 [-3.3, -0.8] | **-0.0800x** |


### SOLUSDT — Kalshi round trip 28.2 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| seesaw sign (minus basket) | 41,100 | +0.0191 | +1.03 | [+0.3, +1.8] | 0.52 | 14.76 | -13.73 | -14.49 | +0.90 [-0.2, +2.0] | **0.0701x** |
| own momentum | 41,100 | -0.0121 | -0.27 | [-1.0, +0.5] | 0.52 | 14.75 | -15.02 | -15.80 | -0.40 [-1.5, +0.7] | **-0.0182x** |
| equal-weight basket | 41,100 | -0.0191 | -1.03 | [-1.8, -0.3] | 0.52 | 14.76 | -15.79 | -16.56 | -1.17 [-2.4, +0.0] | **-0.0701x** |
| LASSO (walk-forward) | 41,100 | -0.0110 | -0.60 | [-1.4, +0.2] | 0.28 | 7.83 | -8.44 | -9.25 | -0.74 [-2.2, +0.7] | **-0.0770x** |
| leader-only (BTC+ETH) | 41,100 | -0.0221 | -1.23 | [-2.0, -0.4] | 0.53 | 15.02 | -16.25 | -17.02 | -1.36 [-2.6, -0.2] | **-0.0817x** |
| BTC lagged return | 41,100 | -0.0210 | -1.30 | [-2.1, -0.5] | 0.53 | 14.96 | -16.27 | -17.02 | -1.44 [-2.7, -0.2] | **-0.0871x** |


### XRPUSDT — Kalshi round trip 29.1 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| seesaw sign (minus basket) | 41,100 | +0.0155 | +0.33 | [-0.3, +0.9] | 0.52 | 15.20 | -14.87 | -15.47 | +0.20 [-0.8, +1.2] | **0.0215x** |
| own momentum | 41,100 | -0.0099 | +0.01 | [-0.6, +0.6] | 0.52 | 15.09 | -15.08 | -15.73 | -0.11 [-1.0, +0.8] | **0.0007x** |
| LASSO (walk-forward) | 41,100 | +0.0099 | -0.26 | [-0.9, +0.4] | 0.47 | 13.74 | -14.00 | -14.67 | -0.38 [-1.4, +0.6] | **-0.0189x** |
| equal-weight basket | 41,100 | -0.0155 | -0.33 | [-0.9, +0.3] | 0.52 | 15.20 | -15.52 | -16.12 | -0.45 [-1.3, +0.4] | **-0.0215x** |
| leader-only (BTC+ETH) | 41,100 | -0.0174 | -0.66 | [-1.3, -0.0] | 0.53 | 15.46 | -16.12 | -16.77 | -0.78 [-1.7, +0.1] | **-0.0424x** |
| BTC lagged return | 41,100 | -0.0182 | -0.82 | [-1.5, -0.2] | 0.53 | 15.41 | -16.23 | -16.90 | -0.94 [-1.9, +0.0] | **-0.0533x** |


## Verdict

Best across every focal asset and model: **seesaw sign (minus basket)** on ? at **0.070x** of the required effect (+1.03 bp gross against a 28.2 bp round trip).

| gate | specifications passing |
|---|---:|
| economic_ratio > 1 (point estimate) | 0 of 78 |
| and net_lo > 0 (cost-adjusted return distinguishable from zero) | 0 |
| and excess_lo > 0 (beats the better constant position) | 0 |

**Nothing clears the hurdle.** The best specification is worth 7.0% of what it costs to trade on Kalshi.

The LASSO does not beat the simple baselines (0.055x against 0.070x for `seesaw sign (minus basket)`), so the failure is not a modelling failure. There is not enough in the cross-section at this bar length to pay for a Kalshi round trip.


## What this does not settle

- The papers are intraday on Binance with many more coins and, in the seesaw case, a long-short portfolio across the cross-section. This is a directional per-asset test on 13 coins, which is the version that maps onto a Kalshi position.
- The seesaw direction is large-predicts-small. The small coins are exactly the ones that cost 18-21 bp one way on Kalshi, so a replication that works on Binance can still fail on transfer, and that would be `FAILED_TRANSFER` rather than a contradiction of the paper.
- Costs here are the Kalshi round trip. The papers clear their own venues' costs, which are far lower. Both statements can be true at once.

