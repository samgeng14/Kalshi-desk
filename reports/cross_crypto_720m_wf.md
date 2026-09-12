# Cross-crypto return predictability — replication at 720-minute bars

Sources, verified verbatim before any code: Jia, Wu, Yan & Liu, *J Empirical Finance* 74 (2023); Guo, Sang, Tu & Wang, *JEDC* 163 (2024). Specification in `research/literature/cross_crypto_replication.md`.

Panel: 13 assets, 4,280 aligned bars, 2140 days. Chronological split 60% train / 20% validate / 20% test. The penalty is chosen on validation; the test slice is touched once.

## The headline is the economic ratio

`economic_ratio = gross bps per bar / trade cost per bar`, signed. The trade cost is `turnover x round_trip + round_trip / n`: you pay the spread when the position changes sign, plus the unavoidable opening and closing trade amortised over the slice. A ratio of 1.0 means the signal is exactly worth what it costs to trade; below 1 it loses. `economic_ratio_everybar` charges a full round trip on every bar instead -- a strategy nobody would trade, kept as an upper bound on the cost.

**Two gates beside it, both stricter.** `net_lo` is the block-bootstrap 90% lower bound on the gross return minus the trade cost: a specification whose `net_lo` is negative has a cost-adjusted return that is not distinguishable from zero. `excess_vs_constant_bps` is the return over the better of always-long and always-short on the same slice -- because turnover-adjusted costing rewards holding, and the cheapest strategy available is a constant position that pays one round trip in total and collects the drift.

This is a discovery screen. Clearing it makes a signal worth testing, not worth trading: the ratio cannot see tail concentration, instability through time, funding paid while the position is held, or whether a Kalshi contract tracks the underlying return at all.


### AAVEUSDT — Kalshi round trip 38.6 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| own momentum | 3,423 | +0.0132 | +9.90 | [-0.1, +20.0] | 0.50 | 19.22 | -9.33 | -19.34 | +8.64 [-6.0, +23.1] | **0.5148x** |
| LASSO (walk-forward) | 3,423 | -0.0041 | +7.09 | [-3.1, +17.3] | 0.36 | 13.93 | -6.84 | -17.00 | +5.84 [-11.5, +23.5] | **0.5091x** |
| equal-weight basket | 3,423 | -0.0008 | +6.62 | [-3.3, +16.5] | 0.50 | 19.33 | -12.70 | -22.66 | +5.37 [-8.8, +19.7] | **0.3426x** |
| BTC lagged return | 3,423 | -0.0060 | +2.09 | [-7.7, +12.1] | 0.52 | 20.16 | -18.07 | -27.88 | +0.84 [-13.2, +15.0] | **0.1038x** |
| leader-only (BTC+ETH) | 3,423 | -0.0052 | -0.36 | [-10.3, +9.5] | 0.51 | 19.78 | -20.14 | -30.10 | -1.62 [-15.4, +12.4] | **-0.0183x** |
| seesaw sign (minus basket) | 3,423 | +0.0008 | -6.62 | [-16.5, +3.3] | 0.50 | 19.33 | -25.95 | -35.81 | -7.87 [-22.8, +6.7] | **-0.3426x** |


### ADAUSDT — Kalshi round trip 35.4 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| own momentum | 3,423 | -0.0029 | +7.09 | [-1.7, +15.8] | 0.51 | 18.11 | -11.02 | -19.86 | +1.66 [-11.5, +15.1] | **0.3916x** |
| seesaw sign (minus basket) | 3,423 | +0.0183 | +1.44 | [-7.5, +10.3] | 0.50 | 17.73 | -16.29 | -25.26 | -3.99 [-16.2, +8.2] | **0.0812x** |
| leader-only (BTC+ETH) | 3,423 | -0.0148 | +0.01 | [-8.8, +8.9] | 0.51 | 18.14 | -18.13 | -26.92 | -5.42 [-18.5, +7.8] | **0.0008x** |
| equal-weight basket | 3,423 | -0.0183 | -1.44 | [-10.3, +7.5] | 0.50 | 17.73 | -19.17 | -28.00 | -6.87 [-20.2, +6.6] | **-0.0812x** |
| BTC lagged return | 3,423 | -0.0185 | -4.35 | [-13.3, +4.5] | 0.52 | 18.50 | -22.85 | -31.82 | -9.79 [-22.1, +2.5] | **-0.2354x** |
| LASSO (walk-forward) | 3,423 | -0.0005 | -3.78 | [-13.0, +5.0] | 0.36 | 12.83 | -16.61 | -25.88 | -9.21 [-24.0, +5.2] | **-0.2947x** |


### AVAXUSDT — Kalshi round trip 34.0 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| own momentum | 3,423 | -0.0220 | +9.97 | [+0.2, +19.8] | 0.50 | 16.96 | -6.99 | -16.81 | +1.88 [-11.0, +14.8] | **0.5879x** |
| LASSO (walk-forward) | 3,423 | +0.0004 | +1.58 | [-8.8, +11.9] | 0.36 | 12.16 | -10.58 | -20.97 | -6.51 [-24.6, +11.3] | **0.1301x** |
| equal-weight basket | 3,423 | -0.0155 | +0.25 | [-9.5, +10.3] | 0.50 | 17.04 | -16.79 | -26.50 | -7.84 [-21.2, +5.8] | **0.0147x** |
| BTC lagged return | 3,423 | -0.0090 | -0.00 | [-9.7, +9.8] | 0.52 | 17.77 | -17.78 | -27.52 | -8.09 [-21.4, +5.4] | **-0.0002x** |
| seesaw sign (minus basket) | 3,423 | +0.0155 | -0.25 | [-10.2, +9.5] | 0.50 | 17.04 | -17.29 | -27.27 | -8.34 [-23.1, +6.3] | **-0.0147x** |
| leader-only (BTC+ETH) | 3,423 | -0.0135 | -0.80 | [-10.6, +8.8] | 0.51 | 17.44 | -18.24 | -27.99 | -8.89 [-22.0, +4.1] | **-0.0459x** |


### BCHUSDT — Kalshi round trip 40.3 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| LASSO (walk-forward) | 3,423 | +0.0132 | +4.58 | [-3.8, +12.9] | 0.43 | 17.29 | -12.71 | -21.04 | +2.92 [-9.3, +15.5] | **0.2650x** |
| equal-weight basket | 3,423 | -0.0037 | +0.34 | [-7.8, +8.4] | 0.50 | 20.20 | -19.86 | -27.96 | -1.32 [-13.0, +10.8] | **0.0169x** |
| leader-only (BTC+ETH) | 3,423 | -0.0136 | +0.10 | [-8.1, +8.3] | 0.51 | 20.67 | -20.56 | -28.75 | -1.56 [-13.2, +10.6] | **0.0050x** |
| own momentum | 3,423 | +0.0140 | -0.28 | [-8.5, +7.9] | 0.52 | 20.94 | -21.22 | -29.39 | -1.95 [-13.6, +10.6] | **-0.0136x** |
| seesaw sign (minus basket) | 3,423 | +0.0037 | -0.34 | [-8.4, +7.8] | 0.50 | 20.20 | -20.54 | -28.62 | -2.00 [-13.5, +9.5] | **-0.0169x** |
| BTC lagged return | 3,423 | -0.0046 | -0.64 | [-8.7, +7.4] | 0.52 | 21.07 | -21.71 | -29.78 | -2.30 [-13.9, +9.8] | **-0.0304x** |


### BNBUSDT — Kalshi round trip 33.9 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| equal-weight basket | 3,423 | +0.0020 | +2.88 | [-2.8, +8.9] | 0.50 | 16.97 | -14.09 | -19.81 | +2.14 [-6.4, +10.8] | **0.1697x** |
| BTC lagged return | 3,423 | -0.0009 | +1.47 | [-4.2, +7.1] | 0.52 | 17.70 | -16.23 | -21.89 | +0.73 [-8.1, +9.6] | **0.0831x** |
| own momentum | 3,423 | -0.0101 | +1.37 | [-4.4, +7.3] | 0.50 | 17.09 | -15.71 | -21.50 | +0.63 [-7.9, +9.3] | **0.0804x** |
| leader-only (BTC+ETH) | 3,423 | -0.0012 | -0.40 | [-6.0, +5.3] | 0.51 | 17.37 | -17.76 | -23.36 | -1.14 [-10.1, +7.9] | **-0.0229x** |
| seesaw sign (minus basket) | 3,423 | -0.0020 | -2.88 | [-8.9, +2.8] | 0.50 | 16.97 | -19.85 | -25.82 | -3.62 [-12.0, +4.5] | **-0.1697x** |
| LASSO (walk-forward) | 3,423 | -0.0518 | -4.12 | [-10.0, +1.7] | 0.20 | 6.82 | -10.94 | -16.81 | -4.86 [-10.1, +0.2] | **-0.6042x** |


### BTCUSDT — Kalshi round trip 24.6 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| seesaw sign (minus basket) | 3,423 | +0.0083 | +0.22 | [-4.9, +5.3] | 0.50 | 12.35 | -12.13 | -17.29 | -1.28 [-8.6, +5.9] | **0.0177x** |
| equal-weight basket | 3,423 | -0.0083 | -0.22 | [-5.3, +4.9] | 0.50 | 12.35 | -12.57 | -17.67 | -1.72 [-9.2, +6.0] | **-0.0177x** |
| own momentum | 3,423 | -0.0019 | -0.43 | [-5.6, +4.7] | 0.52 | 12.88 | -13.31 | -18.47 | -1.93 [-9.5, +5.7] | **-0.0334x** |
| BTC lagged return | 3,423 | -0.0019 | -0.43 | [-5.6, +4.7] | 0.52 | 12.88 | -13.31 | -18.47 | -1.93 [-9.5, +5.7] | **-0.0334x** |
| leader-only (BTC+ETH) | 3,423 | -0.0026 | -0.65 | [-5.8, +4.5] | 0.51 | 12.64 | -13.28 | -18.46 | -2.15 [-9.6, +5.4] | **-0.0512x** |
| LASSO (walk-forward) | 3,423 | -0.0374 | -0.91 | [-6.2, +4.4] | 0.18 | 4.33 | -5.23 | -10.53 | -2.40 [-6.1, +1.3] | **-0.2093x** |


### DOGEUSDT — Kalshi round trip 33.6 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| LASSO (walk-forward) | 3,423 | +0.0447 | +7.58 | [-1.3, +16.7] | 0.45 | 15.22 | -7.64 | -16.49 | +5.46 [-9.2, +20.3] | **0.4983x** |
| BTC lagged return | 3,423 | +0.0117 | +7.39 | [-1.6, +16.3] | 0.52 | 17.56 | -10.17 | -19.15 | +5.26 [-7.5, +18.0] | **0.4207x** |
| equal-weight basket | 3,423 | +0.0061 | +7.00 | [-2.1, +16.2] | 0.50 | 16.83 | -9.83 | -18.90 | +4.88 [-8.6, +18.6] | **0.4160x** |
| leader-only (BTC+ETH) | 3,423 | +0.0068 | +6.22 | [-2.7, +15.4] | 0.51 | 17.22 | -11.00 | -19.94 | +4.09 [-9.0, +17.2] | **0.3610x** |
| own momentum | 3,423 | +0.0048 | +4.10 | [-4.7, +13.2] | 0.51 | 17.11 | -13.02 | -21.86 | +1.97 [-11.3, +15.3] | **0.2394x** |
| seesaw sign (minus basket) | 3,423 | -0.0061 | -7.00 | [-16.2, +2.1] | 0.50 | 16.83 | -23.83 | -33.05 | -9.13 [-21.6, +3.0] | **-0.4160x** |


### ETHUSDT — Kalshi round trip 25.0 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| equal-weight basket | 3,423 | +0.0132 | +2.81 | [-4.2, +9.8] | 0.50 | 12.53 | -9.72 | -16.72 | +1.42 [-8.3, +11.6] | **0.2244x** |
| leader-only (BTC+ETH) | 3,423 | +0.0160 | +1.97 | [-5.0, +9.0] | 0.51 | 12.82 | -10.85 | -17.86 | +0.58 [-9.2, +10.5] | **0.1535x** |
| BTC lagged return | 3,423 | +0.0136 | +1.79 | [-5.1, +8.6] | 0.52 | 13.07 | -11.28 | -18.19 | +0.40 [-9.6, +10.4] | **0.1371x** |
| own momentum | 3,423 | +0.0168 | +1.75 | [-5.3, +8.9] | 0.51 | 12.78 | -11.03 | -18.12 | +0.36 [-9.3, +10.2] | **0.1371x** |
| LASSO (walk-forward) | 3,423 | -0.0170 | -0.47 | [-7.6, +6.6] | 0.29 | 7.32 | -7.79 | -14.97 | -1.86 [-15.0, +11.1] | **-0.0642x** |
| seesaw sign (minus basket) | 3,423 | -0.0132 | -2.81 | [-9.8, +4.2] | 0.50 | 12.53 | -15.34 | -22.37 | -4.20 [-14.6, +6.0] | **-0.2244x** |


### LINKUSDT — Kalshi round trip 42.6 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| own momentum | 3,423 | +0.0086 | +4.26 | [-4.5, +13.2] | 0.50 | 21.23 | -16.97 | -25.77 | +2.68 [-10.0, +15.4] | **0.2005x** |
| equal-weight basket | 3,423 | -0.0091 | +0.42 | [-8.7, +9.3] | 0.50 | 21.35 | -20.93 | -30.03 | -1.16 [-13.5, +11.5] | **0.0199x** |
| seesaw sign (minus basket) | 3,423 | +0.0091 | -0.42 | [-9.3, +8.7] | 0.50 | 21.35 | -21.77 | -30.65 | -2.00 [-15.3, +11.2] | **-0.0199x** |
| LASSO (walk-forward) | 3,423 | -0.0083 | -2.11 | [-11.2, +7.1] | 0.43 | 18.39 | -20.50 | -29.58 | -3.69 [-17.9, +10.4] | **-0.1147x** |
| BTC lagged return | 3,423 | -0.0226 | -3.24 | [-12.2, +5.7] | 0.52 | 22.27 | -25.51 | -34.43 | -4.82 [-16.9, +7.6] | **-0.1455x** |
| leader-only (BTC+ETH) | 3,423 | -0.0213 | -3.44 | [-12.4, +5.7] | 0.51 | 21.85 | -25.28 | -34.24 | -5.02 [-17.1, +7.2] | **-0.1573x** |


### LTCUSDT — Kalshi round trip 31.2 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| seesaw sign (minus basket) | 3,423 | +0.0170 | +5.57 | [-2.4, +13.6] | 0.50 | 15.62 | -10.04 | -18.02 | +2.31 [-9.5, +13.9] | **0.3569x** |
| leader-only (BTC+ETH) | 3,423 | -0.0250 | -1.18 | [-9.1, +6.7] | 0.51 | 15.98 | -17.16 | -25.07 | -4.44 [-14.2, +5.8] | **-0.0738x** |
| LASSO (walk-forward) | 3,423 | +0.0037 | -0.99 | [-8.7, +6.6] | 0.39 | 12.03 | -13.01 | -20.76 | -4.25 [-17.8, +8.8] | **-0.0820x** |
| BTC lagged return | 3,423 | -0.0224 | -3.14 | [-11.0, +4.6] | 0.52 | 16.29 | -19.43 | -27.26 | -6.40 [-16.5, +3.9] | **-0.1927x** |
| equal-weight basket | 3,423 | -0.0170 | -5.57 | [-13.6, +2.4] | 0.50 | 15.62 | -21.19 | -29.20 | -8.84 [-19.2, +1.8] | **-0.3569x** |
| own momentum | 3,423 | -0.0248 | -6.07 | [-13.7, +1.8] | 0.52 | 16.11 | -22.18 | -29.84 | -9.33 [-20.1, +1.5] | **-0.3768x** |


### NEARUSDT — Kalshi round trip 39.5 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| equal-weight basket | 3,423 | -0.0012 | +5.26 | [-6.2, +16.4] | 0.50 | 19.79 | -14.53 | -25.96 | +0.73 [-14.7, +16.2] | **0.2657x** |
| own momentum | 3,423 | -0.0072 | +4.23 | [-7.1, +15.7] | 0.50 | 19.91 | -15.68 | -26.97 | -0.30 [-15.7, +15.4] | **0.2127x** |
| BTC lagged return | 3,423 | +0.0075 | -1.41 | [-12.5, +9.6] | 0.52 | 20.64 | -22.05 | -33.11 | -5.94 [-21.0, +9.1] | **-0.0682x** |
| leader-only (BTC+ETH) | 3,423 | +0.0073 | -4.26 | [-15.1, +6.8] | 0.51 | 20.25 | -24.51 | -35.37 | -8.79 [-23.4, +6.0] | **-0.2105x** |
| seesaw sign (minus basket) | 3,423 | +0.0012 | -5.26 | [-16.4, +6.2] | 0.50 | 19.79 | -25.04 | -36.17 | -9.79 [-26.5, +6.5] | **-0.2657x** |
| LASSO (walk-forward) | 3,423 | +0.0050 | -5.55 | [-17.3, +5.9] | 0.41 | 16.27 | -21.82 | -33.55 | -10.08 [-28.3, +7.7] | **-0.3413x** |


### SOLUSDT — Kalshi round trip 28.2 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| BTC lagged return | 3,423 | +0.0059 | +5.74 | [-3.9, +15.6] | 0.52 | 14.76 | -9.02 | -18.64 | +4.06 [-9.6, +17.6] | **0.3889x** |
| equal-weight basket | 3,423 | +0.0050 | +5.29 | [-4.2, +14.9] | 0.50 | 14.15 | -8.87 | -18.32 | +3.61 [-9.6, +17.0] | **0.3736x** |
| own momentum | 3,423 | +0.0220 | +4.96 | [-4.8, +14.7] | 0.50 | 14.05 | -9.08 | -18.87 | +3.28 [-10.3, +17.2] | **0.3532x** |
| leader-only (BTC+ETH) | 3,423 | +0.0064 | +4.22 | [-5.1, +14.0] | 0.51 | 14.48 | -10.26 | -19.61 | +2.55 [-10.7, +16.1] | **0.2917x** |
| LASSO (walk-forward) | 3,423 | -0.0239 | +0.38 | [-10.0, +10.7] | 0.33 | 9.47 | -9.08 | -19.47 | -1.29 [-20.0, +16.9] | **0.0405x** |
| seesaw sign (minus basket) | 3,423 | -0.0050 | -5.29 | [-14.9, +4.2] | 0.50 | 14.15 | -19.44 | -29.08 | -6.97 [-21.8, +7.3] | **-0.3736x** |


### XRPUSDT — Kalshi round trip 29.1 bp

| model | n | IC | gross bps/bar | 90% interval | turnover | trade cost | net bps/bar | net_lo | excess vs constant | economic ratio |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|
| own momentum | 3,423 | +0.0412 | +15.00 | [+7.1, +22.9] | 0.49 | 14.37 | +0.63 | -7.27 | +13.54 [+2.9, +23.9] | **1.0441x** |
| LASSO (walk-forward) | 3,423 | -0.0122 | +8.32 | [-0.0, +16.3] | 0.45 | 13.05 | -4.74 | -13.06 | +6.85 [-3.8, +16.8] | **0.6372x** |
| equal-weight basket | 3,423 | +0.0146 | +6.75 | [-1.1, +14.5] | 0.50 | 14.57 | -7.83 | -15.67 | +5.28 [-5.5, +15.9] | **0.4630x** |
| leader-only (BTC+ETH) | 3,423 | +0.0102 | +6.83 | [-1.1, +14.7] | 0.51 | 14.91 | -8.09 | -15.99 | +5.36 [-5.4, +15.9] | **0.4577x** |
| BTC lagged return | 3,423 | +0.0096 | +6.32 | [-1.5, +14.4] | 0.52 | 15.20 | -8.88 | -16.67 | +4.86 [-6.0, +15.7] | **0.4160x** |
| seesaw sign (minus basket) | 3,423 | -0.0146 | -6.75 | [-14.5, +1.1] | 0.50 | 14.57 | -21.32 | -29.11 | -8.21 [-20.2, +3.5] | **-0.4630x** |


## Verdict

Best across every focal asset and model: **own momentum** on ? at **1.044x** of the required effect (+15.00 bp gross against a 29.1 bp round trip).

| gate | specifications passing |
|---|---:|
| economic_ratio > 1 (point estimate) | 1 of 78 |
| and net_lo > 0 (cost-adjusted return distinguishable from zero) | 0 |
| and excess_lo > 0 (beats the better constant position) | 0 |

1 point estimate(s) clear the hurdle; none has a bootstrap lower bound above zero. That is a reason to record more data, not to trade.


## What this does not settle

- The papers are intraday on Binance with many more coins and, in the seesaw case, a long-short portfolio across the cross-section. This is a directional per-asset test on 13 coins, which is the version that maps onto a Kalshi position.
- The seesaw direction is large-predicts-small. The small coins are exactly the ones that cost 18-21 bp one way on Kalshi, so a replication that works on Binance can still fail on transfer, and that would be `FAILED_TRANSFER` rather than a contradiction of the paper.
- Costs here are the Kalshi round trip. The papers clear their own venues' costs, which are far lower. Both statements can be true at once.

