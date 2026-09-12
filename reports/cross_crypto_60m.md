# Cross-crypto return predictability — replication at 60-minute bars

Sources, verified verbatim before any code: Jia, Wu, Yan & Liu, *J Empirical Finance* 74 (2023); Guo, Sang, Tu & Wang, *JEDC* 163 (2024). Specification in `research/literature/cross_crypto_replication.md`.

Panel: 14 assets, 9,599 aligned bars, 400 days. Chronological split 60% train / 20% validate / 20% test. The penalty is chosen on validation; the test slice is touched once.

## The headline is the economic ratio

`economic_ratio = |observed effect| / required effect`, where the required effect is the focal asset's OWN Kalshi round trip. A ratio of 1.0 means the signal is exactly worth what it costs to trade; below 1 it loses. Statistical columns are reported beside it, never instead of it.

This is a discovery screen. Clearing it makes a signal worth testing, not worth trading: the ratio cannot see tail concentration, instability through time, turnover cost, or whether a Kalshi contract tracks the underlying return at all.


### AAVEUSDT — Kalshi round trip 38.6 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 1,920 | 80 | +0.1014 | +0.0897 | +38.41 | +5.88 | [+2.1, +10.0] | 0.54 | -32.68 | **0.1526x** |
| own momentum | 1,920 | 80 | -0.0797 | -0.0877 | -25.63 | -3.69 | [-6.8, -0.6] | 0.52 | -42.25 | **-0.0958x** |
| LASSO (alpha=3.0, 1 nonzero) | 1,920 | 80 | -0.0687 | -0.0424 | -12.83 | -3.96 | [-7.3, -1.0] | 0.11 | -42.52 | **-0.1028x** |
| BTC lagged return | 1,920 | 80 | -0.0976 | -0.0867 | -38.01 | -5.63 | [-9.2, -2.2] | 0.53 | -44.19 | **-0.1459x** |
| equal-weight basket | 1,920 | 80 | -0.1014 | -0.0897 | -38.41 | -5.88 | [-9.9, -2.1] | 0.54 | -44.44 | **-0.1526x** |
| leader-only (BTC+ETH) | 1,920 | 80 | -0.1009 | -0.1039 | -40.74 | -6.76 | [-11.0, -2.9] | 0.54 | -45.32 | **-0.1754x** |


### ADAUSDT — Kalshi round trip 35.4 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 1,920 | 80 | +0.0251 | +0.0511 | +3.73 | +1.97 | [-1.4, +5.1] | 0.54 | -33.41 | **0.0557x** |
| BTC lagged return | 1,920 | 80 | -0.0140 | -0.0268 | -8.00 | -0.47 | [-3.4, +2.6] | 0.53 | -35.85 | **-0.0134x** |
| leader-only (BTC+ETH) | 1,920 | 80 | -0.0093 | -0.0399 | -4.00 | -1.30 | [-4.4, +2.0] | 0.54 | -36.68 | **-0.0367x** |
| equal-weight basket | 1,920 | 80 | -0.0251 | -0.0511 | -3.73 | -1.97 | [-5.1, +1.4] | 0.54 | -37.35 | **-0.0557x** |
| LASSO (alpha=3.0, 1 nonzero) | 1,920 | 80 | -0.0528 | -0.0360 | -0.35 | -2.35 | [-5.2, +0.7] | 0.06 | -37.73 | **-0.0664x** |
| own momentum | 1,920 | 80 | -0.0540 | -0.0816 | -13.45 | -3.20 | [-6.4, -0.1] | 0.54 | -38.58 | **-0.0904x** |


### AVAXUSDT — Kalshi round trip 34.0 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 1,920 | 80 | +0.0548 | +0.0858 | +9.09 | +2.96 | [+0.3, +5.7] | 0.54 | -31.04 | **0.0872x** |
| own momentum | 1,920 | 80 | -0.0351 | -0.0698 | -7.35 | -0.83 | [-3.4, +1.6] | 0.53 | -34.83 | **-0.0243x** |
| BTC lagged return | 1,920 | 80 | -0.0469 | -0.0738 | -15.82 | -1.30 | [-3.8, +1.2] | 0.53 | -35.30 | **-0.0383x** |
| leader-only (BTC+ETH) | 1,920 | 80 | -0.0355 | -0.0852 | -15.90 | -2.23 | [-4.8, +0.2] | 0.54 | -36.23 | **-0.0657x** |
| LASSO (alpha=3.0, 2 nonzero) | 1,920 | 80 | -0.0748 | -0.0701 | -10.47 | -2.42 | [-4.7, -0.3] | 0.12 | -36.42 | **-0.0712x** |
| equal-weight basket | 1,920 | 80 | -0.0548 | -0.0858 | -9.09 | -2.96 | [-5.7, -0.3] | 0.54 | -36.96 | **-0.0872x** |


### BCHUSDT — Kalshi round trip 40.3 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 1,920 | 80 | +0.0523 | +0.0659 | +4.67 | +2.04 | [-1.6, +5.1] | 0.54 | -38.26 | **0.0505x** |
| LASSO (alpha=3.0, 1 nonzero) | 1,920 | 80 | -0.0800 | -0.0884 | -8.95 | -0.69 | [-4.1, +2.2] | 0.00 | -40.99 | **-0.0171x** |
| BTC lagged return | 1,920 | 80 | -0.0249 | -0.0570 | -5.87 | -0.88 | [-3.3, +1.7] | 0.53 | -41.18 | **-0.0220x** |
| leader-only (BTC+ETH) | 1,920 | 80 | -0.0216 | -0.0614 | -1.90 | -1.18 | [-4.0, +1.8] | 0.54 | -41.48 | **-0.0292x** |
| own momentum | 1,920 | 80 | +0.0112 | -0.0663 | -5.07 | -1.68 | [-4.6, +1.8] | 0.53 | -41.98 | **-0.0418x** |
| equal-weight basket | 1,920 | 80 | -0.0523 | -0.0659 | -4.67 | -2.04 | [-5.1, +1.6] | 0.54 | -42.34 | **-0.0505x** |


### BNBUSDT — Kalshi round trip 33.9 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 1,920 | 80 | +0.0352 | +0.0390 | +3.14 | +1.15 | [-0.4, +2.6] | 0.54 | -32.71 | **0.0340x** |
| own momentum | 1,920 | 80 | -0.0215 | -0.0433 | +2.15 | -0.40 | [-1.9, +1.0] | 0.52 | -34.26 | **-0.0119x** |
| BTC lagged return | 1,920 | 80 | -0.0344 | -0.0475 | -4.71 | -0.94 | [-2.3, +0.5] | 0.53 | -34.80 | **-0.0277x** |
| LASSO (alpha=3.0, 1 nonzero) | 1,920 | 80 | -0.0573 | -0.0297 | -0.47 | -0.99 | [-2.4, +0.6] | 0.05 | -34.85 | **-0.0292x** |
| leader-only (BTC+ETH) | 1,920 | 80 | -0.0304 | -0.0568 | -5.04 | -1.12 | [-2.6, +0.4] | 0.54 | -34.98 | **-0.0330x** |
| equal-weight basket | 1,920 | 80 | -0.0352 | -0.0390 | -3.14 | -1.15 | [-2.6, +0.4] | 0.54 | -35.01 | **-0.0340x** |


### BTCUSDT — Kalshi round trip 24.6 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 1,920 | 80 | +0.0271 | +0.0631 | +0.27 | +1.43 | [-0.2, +3.1] | 0.54 | -23.21 | **0.0581x** |
| LASSO (alpha=0.3, 2 nonzero) | 1,920 | 80 | -0.0358 | -0.0477 | +3.21 | -0.31 | [-1.6, +1.0] | 0.10 | -24.95 | **-0.0125x** |
| own momentum | 1,920 | 80 | -0.0210 | -0.0594 | -0.53 | -0.61 | [-2.2, +1.1] | 0.53 | -25.25 | **-0.0246x** |
| BTC lagged return | 1,920 | 80 | -0.0210 | -0.0594 | -0.53 | -0.61 | [-2.2, +1.1] | 0.53 | -25.25 | **-0.0246x** |
| leader-only (BTC+ETH) | 1,920 | 80 | -0.0208 | -0.0603 | -0.26 | -0.66 | [-2.4, +1.1] | 0.54 | -25.30 | **-0.0269x** |
| equal-weight basket | 1,920 | 80 | -0.0271 | -0.0631 | -0.27 | -1.43 | [-3.1, +0.2] | 0.54 | -26.07 | **-0.0581x** |


### DOGEUSDT — Kalshi round trip 33.6 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 1,920 | 80 | +0.0277 | +0.0832 | +2.25 | +2.20 | [-0.2, +4.6] | 0.54 | -31.38 | **0.0656x** |
| own momentum | 1,920 | 80 | -0.0294 | -0.0693 | -0.43 | -0.15 | [-2.8, +2.5] | 0.53 | -33.73 | **-0.0044x** |
| BTC lagged return | 1,920 | 80 | -0.0142 | -0.0734 | -8.46 | -1.03 | [-3.2, +1.2] | 0.53 | -34.61 | **-0.0308x** |
| leader-only (BTC+ETH) | 1,920 | 80 | -0.0021 | -0.0811 | -4.47 | -1.20 | [-3.4, +1.1] | 0.54 | -34.78 | **-0.0357x** |
| equal-weight basket | 1,920 | 80 | -0.0277 | -0.0832 | -2.25 | -2.20 | [-4.6, +0.3] | 0.54 | -35.78 | **-0.0656x** |
| LASSO (alpha=0.03, 14 nonzero) | 1,920 | 80 | -0.0557 | -0.0464 | -3.35 | -2.60 | [-5.3, +0.1] | 0.49 | -36.18 | **-0.0773x** |


### ETHUSDT — Kalshi round trip 25.0 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 1,920 | 80 | +0.0159 | +0.0816 | +0.76 | +1.52 | [-0.7, +3.7] | 0.54 | -23.48 | **0.0609x** |
| own momentum | 1,920 | 80 | +0.0061 | -0.0891 | -3.27 | -0.80 | [-3.0, +1.4] | 0.54 | -25.80 | **-0.0320x** |
| leader-only (BTC+ETH) | 1,920 | 80 | -0.0063 | -0.0888 | -3.53 | -1.18 | [-3.5, +1.2] | 0.54 | -26.18 | **-0.0472x** |
| BTC lagged return | 1,920 | 80 | -0.0224 | -0.0798 | -7.06 | -1.23 | [-3.1, +0.7] | 0.53 | -26.23 | **-0.0491x** |
| LASSO (alpha=3.0, 2 nonzero) | 1,920 | 80 | -0.0420 | -0.0970 | -3.73 | -1.30 | [-3.1, +0.4] | 0.03 | -26.30 | **-0.0520x** |
| equal-weight basket | 1,920 | 80 | -0.0159 | -0.0816 | -0.76 | -1.52 | [-3.7, +0.7] | 0.54 | -26.52 | **-0.0609x** |


### LINKUSDT — Kalshi round trip 42.6 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 1,920 | 80 | +0.0685 | +0.0862 | +14.66 | +3.15 | [+0.4, +5.9] | 0.54 | -39.45 | **0.0740x** |
| own momentum | 1,920 | 80 | -0.0313 | -0.0676 | -1.56 | -1.13 | [-3.7, +1.5] | 0.53 | -43.73 | **-0.0265x** |
| leader-only (BTC+ETH) | 1,920 | 80 | -0.0391 | -0.0737 | -10.40 | -1.55 | [-4.1, +0.9] | 0.54 | -44.15 | **-0.0365x** |
| BTC lagged return | 1,920 | 80 | -0.0472 | -0.0681 | -12.02 | -1.89 | [-4.4, +0.5] | 0.53 | -44.49 | **-0.0445x** |
| equal-weight basket | 1,920 | 80 | -0.0685 | -0.0862 | -14.66 | -3.15 | [-5.9, -0.4] | 0.54 | -45.75 | **-0.0740x** |
| LASSO (alpha=3.0, 1 nonzero) | 1,920 | 80 | -0.1053 | -0.0705 | -13.64 | -3.96 | [-6.3, -1.5] | 0.21 | -46.56 | **-0.0930x** |


### LTCUSDT — Kalshi round trip 31.2 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 1,920 | 80 | +0.0627 | +0.0495 | +9.70 | +1.98 | [-0.1, +4.0] | 0.54 | -29.18 | **0.0637x** |
| BTC lagged return | 1,920 | 80 | -0.0524 | -0.0453 | -12.48 | -0.87 | [-2.9, +1.4] | 0.53 | -32.03 | **-0.0280x** |
| leader-only (BTC+ETH) | 1,920 | 80 | -0.0434 | -0.0541 | -11.39 | -1.39 | [-3.4, +0.7] | 0.54 | -32.55 | **-0.0447x** |
| equal-weight basket | 1,920 | 80 | -0.0627 | -0.0495 | -9.70 | -1.98 | [-4.0, +0.2] | 0.54 | -33.14 | **-0.0637x** |
| LASSO (alpha=3.0, 1 nonzero) | 1,920 | 80 | -0.0869 | -0.0219 | -11.10 | -2.25 | [-4.2, -0.4] | 0.22 | -33.41 | **-0.0721x** |
| own momentum | 1,920 | 80 | -0.1265 | -0.0880 | -20.01 | -3.36 | [-5.4, -1.3] | 0.53 | -34.52 | **-0.1079x** |


### NEARUSDT — Kalshi round trip 39.5 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 1,920 | 80 | +0.0671 | +0.0440 | +22.93 | +1.55 | [-1.6, +4.7] | 0.54 | -37.93 | **0.0393x** |
| LASSO (alpha=1.0, 4 nonzero) | 1,920 | 80 | -0.0770 | -0.0142 | -20.87 | -1.18 | [-4.3, +1.7] | 0.40 | -40.66 | **-0.0299x** |
| equal-weight basket | 1,920 | 80 | -0.0671 | -0.0440 | -22.93 | -1.55 | [-4.7, +1.6] | 0.54 | -41.03 | **-0.0393x** |
| BTC lagged return | 1,920 | 80 | -0.0488 | -0.0540 | -18.19 | -1.57 | [-4.8, +2.4] | 0.53 | -41.05 | **-0.0398x** |
| leader-only (BTC+ETH) | 1,920 | 80 | -0.0396 | -0.0531 | -15.11 | -3.34 | [-5.9, -0.6] | 0.54 | -42.82 | **-0.0847x** |
| own momentum | 1,920 | 80 | -0.0847 | -0.0453 | -23.62 | -3.80 | [-7.1, -0.7] | 0.51 | -43.28 | **-0.0962x** |


### SOLUSDT — Kalshi round trip 28.2 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 1,920 | 80 | +0.0597 | +0.0665 | +5.85 | +3.11 | [+0.8, +5.5] | 0.54 | -25.13 | **0.1100x** |
| BTC lagged return | 1,920 | 80 | -0.0494 | -0.0599 | -10.16 | -1.58 | [-3.8, +0.7] | 0.53 | -29.82 | **-0.0561x** |
| own momentum | 1,920 | 80 | -0.0596 | -0.0584 | -5.84 | -1.74 | [-3.8, +0.4] | 0.53 | -29.98 | **-0.0617x** |
| LASSO (alpha=3.0, 2 nonzero) | 1,920 | 80 | -0.0524 | -0.0496 | -8.82 | -2.02 | [-4.3, +0.3] | 0.03 | -30.26 | **-0.0715x** |
| leader-only (BTC+ETH) | 1,920 | 80 | -0.0393 | -0.0650 | -8.79 | -2.10 | [-4.4, +0.1] | 0.54 | -30.34 | **-0.0743x** |
| equal-weight basket | 1,920 | 80 | -0.0597 | -0.0665 | -5.85 | -3.11 | [-5.5, -0.8] | 0.54 | -31.35 | **-0.1100x** |


### SUIUSDT — Kalshi round trip 39.3 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 1,920 | 80 | +0.0707 | +0.0871 | +8.54 | +2.94 | [+0.5, +5.4] | 0.54 | -36.36 | **0.0749x** |
| LASSO (alpha=0.01, 14 nonzero) | 1,920 | 80 | -0.0903 | -0.0106 | -3.85 | +0.91 | [-1.8, +3.5] | 0.48 | -38.39 | **0.0232x** |
| BTC lagged return | 1,920 | 80 | -0.0457 | -0.0751 | -17.09 | -1.98 | [-4.6, +0.7] | 0.53 | -41.28 | **-0.0505x** |
| equal-weight basket | 1,920 | 80 | -0.0707 | -0.0871 | -8.54 | -2.94 | [-5.4, -0.5] | 0.54 | -42.24 | **-0.0749x** |
| leader-only (BTC+ETH) | 1,920 | 80 | -0.0473 | -0.0903 | -16.46 | -3.35 | [-5.8, -0.8] | 0.54 | -42.65 | **-0.0853x** |
| own momentum | 1,920 | 80 | -0.0882 | -0.1011 | -9.88 | -3.62 | [-5.7, -1.3] | 0.54 | -42.92 | **-0.0922x** |


### XRPUSDT — Kalshi round trip 29.1 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 1,920 | 80 | +0.0325 | +0.0611 | +0.32 | +1.60 | [-1.0, +3.9] | 0.54 | -27.48 | **0.0552x** |
| LASSO (alpha=1.0, 2 nonzero) | 1,920 | 80 | -0.0769 | -0.0567 | +4.70 | +0.64 | [-1.2, +2.6] | 0.10 | -28.44 | **0.0220x** |
| own momentum | 1,920 | 80 | -0.0729 | -0.0568 | +5.29 | -0.51 | [-3.1, +2.3] | 0.53 | -29.59 | **-0.0177x** |
| BTC lagged return | 1,920 | 80 | +0.0079 | -0.0459 | -4.40 | -0.79 | [-3.2, +2.0] | 0.53 | -29.87 | **-0.0272x** |
| leader-only (BTC+ETH) | 1,920 | 80 | +0.0061 | -0.0584 | -2.52 | -1.12 | [-3.6, +1.7] | 0.54 | -30.20 | **-0.0385x** |
| equal-weight basket | 1,920 | 80 | -0.0325 | -0.0611 | -0.32 | -1.60 | [-3.9, +1.0] | 0.54 | -30.68 | **-0.0552x** |


## Verdict

Best across every focal asset and model: **seesaw sign (minus basket)** on ? at **0.153x** of the required effect (+5.88 bp gross against a 38.6 bp round trip).

**Nothing clears the hurdle.** The best specification is worth 15.3% of what it costs to trade on Kalshi.

The LASSO does not beat the simple baselines (0.023x against 0.153x for `seesaw sign (minus basket)`), so the failure is not a modelling failure. There is not enough in the cross-section at this bar length to pay for a Kalshi round trip.


## What this does not settle

- The papers are intraday on Binance with many more coins and, in the seesaw case, a long-short portfolio across the cross-section. This is a directional per-asset test on 14 coins, which is the version that maps onto a Kalshi position.
- The seesaw direction is large-predicts-small. The small coins are exactly the ones that cost 18-21 bp one way on Kalshi, so a replication that works on Binance can still fail on transfer, and that would be `FAILED_TRANSFER` rather than a contradiction of the paper.
- Costs here are the Kalshi round trip. The papers clear their own venues' costs, which are far lower. Both statements can be true at once.

