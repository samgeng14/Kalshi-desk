# Cross-crypto return predictability — replication at 240-minute bars

Sources, verified verbatim before any code: Jia, Wu, Yan & Liu, *J Empirical Finance* 74 (2023); Guo, Sang, Tu & Wang, *JEDC* 163 (2024). Specification in `research/literature/cross_crypto_replication.md`.

Panel: 14 assets, 2,399 aligned bars, 400 days. Chronological split 60% train / 20% validate / 20% test. The penalty is chosen on validation; the test slice is touched once.

## The headline is the economic ratio

`economic_ratio = |observed effect| / required effect`, where the required effect is the focal asset's OWN Kalshi round trip. A ratio of 1.0 means the signal is exactly worth what it costs to trade; below 1 it loses. Statistical columns are reported beside it, never instead of it.

This is a discovery screen. Clearing it makes a signal worth testing, not worth trading: the ratio cannot see tail concentration, instability through time, turnover cost, or whether a Kalshi contract tracks the underlying return at all.


### AAVEUSDT — Kalshi round trip 38.6 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 480 | 80 | +0.0241 | +0.0500 | -1.06 | +15.05 | [+3.3, +26.9] | 0.56 | -23.51 | **0.3903x** |
| own momentum | 480 | 80 | +0.0179 | -0.0628 | -16.44 | -3.40 | [-15.0, +9.7] | 0.51 | -41.96 | **-0.0880x** |
| leader-only (BTC+ETH) | 480 | 80 | -0.0284 | -0.0473 | -38.60 | -8.36 | [-21.2, +4.9] | 0.57 | -46.92 | **-0.2167x** |
| BTC lagged return | 480 | 80 | -0.0172 | -0.0339 | -21.83 | -9.15 | [-21.4, +3.1] | 0.54 | -47.71 | **-0.2372x** |
| LASSO (alpha=10.0, 1 nonzero) | 480 | 80 | -0.0504 | -0.0414 | -38.65 | -10.77 | [-25.2, +3.4] | 0.03 | -49.33 | **-0.2794x** |
| equal-weight basket | 480 | 80 | -0.0241 | -0.0500 | +1.06 | -15.05 | [-26.8, -3.3] | 0.56 | -53.61 | **-0.3903x** |


### ADAUSDT — Kalshi round trip 35.4 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 480 | 80 | +0.0512 | +0.1010 | +8.61 | +8.03 | [-4.0, +20.9] | 0.56 | -27.35 | **0.2270x** |
| own momentum | 480 | 80 | -0.0389 | -0.0685 | +6.39 | -3.62 | [-15.7, +8.9] | 0.52 | -39.00 | **-0.1022x** |
| LASSO (alpha=10.0, 2 nonzero) | 480 | 80 | -0.0241 | -0.0956 | -23.72 | -4.83 | [-16.9, +7.2] | 0.03 | -40.21 | **-0.1365x** |
| equal-weight basket | 480 | 80 | -0.0512 | -0.1010 | -8.61 | -8.03 | [-20.9, +4.2] | 0.56 | -43.41 | **-0.2270x** |
| BTC lagged return | 480 | 80 | -0.0140 | -0.0836 | -6.51 | -11.15 | [-22.9, +0.3] | 0.54 | -46.53 | **-0.3152x** |
| leader-only (BTC+ETH) | 480 | 80 | -0.0377 | -0.1154 | -26.23 | -12.53 | [-24.4, -1.0] | 0.57 | -47.91 | **-0.3542x** |


### AVAXUSDT — Kalshi round trip 34.0 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 480 | 80 | +0.0931 | +0.1232 | +41.96 | +13.33 | [+2.9, +24.1] | 0.56 | -20.67 | **0.3920x** |
| LASSO (alpha=3.0, 5 nonzero) | 480 | 80 | -0.1030 | -0.1387 | -68.82 | -8.87 | [-19.0, +0.5] | 0.20 | -42.87 | **-0.2608x** |
| leader-only (BTC+ETH) | 480 | 80 | -0.0747 | -0.1448 | -56.97 | -12.53 | [-24.0, -0.5] | 0.57 | -46.53 | **-0.3686x** |
| own momentum | 480 | 80 | -0.1131 | -0.1255 | -65.60 | -12.59 | [-22.9, -2.0] | 0.55 | -46.59 | **-0.3704x** |
| equal-weight basket | 480 | 80 | -0.0931 | -0.1232 | -41.96 | -13.33 | [-24.1, -2.6] | 0.56 | -47.33 | **-0.3920x** |
| BTC lagged return | 480 | 80 | -0.0614 | -0.1376 | -42.29 | -13.44 | [-24.9, -2.0] | 0.54 | -47.44 | **-0.3953x** |


### BCHUSDT — Kalshi round trip 40.3 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| LASSO (alpha=10.0, 1 nonzero) | 480 | 80 | +0.0562 | -0.0475 | +30.36 | +4.31 | [-6.0, +14.5] | 0.10 | -35.99 | **0.1070x** |
| equal-weight basket | 480 | 80 | -0.0037 | -0.0566 | +18.39 | +0.94 | [-11.3, +13.9] | 0.56 | -39.36 | **0.0232x** |
| seesaw sign (minus basket) | 480 | 80 | +0.0037 | +0.0566 | -18.39 | -0.94 | [-13.9, +11.3] | 0.56 | -41.24 | **-0.0232x** |
| BTC lagged return | 480 | 80 | +0.0562 | -0.0475 | +30.36 | -0.95 | [-12.1, +12.0] | 0.54 | -41.25 | **-0.0235x** |
| leader-only (BTC+ETH) | 480 | 80 | +0.0142 | -0.0626 | +17.16 | -3.14 | [-16.2, +10.0] | 0.57 | -43.44 | **-0.0780x** |
| own momentum | 480 | 80 | -0.0677 | -0.0874 | -26.15 | -4.08 | [-14.9, +6.6] | 0.54 | -44.38 | **-0.1012x** |


### BNBUSDT — Kalshi round trip 33.9 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 480 | 80 | +0.0052 | +0.0859 | +3.02 | +3.18 | [-2.4, +9.0] | 0.56 | -30.68 | **0.0939x** |
| own momentum | 480 | 80 | +0.0128 | -0.0374 | -7.77 | +0.40 | [-4.9, +5.9] | 0.53 | -33.46 | **0.0118x** |
| equal-weight basket | 480 | 80 | -0.0052 | -0.0859 | -3.02 | -3.18 | [-9.0, +2.4] | 0.56 | -37.04 | **-0.0939x** |
| LASSO (alpha=3.0, 3 nonzero) | 480 | 80 | -0.0266 | -0.0853 | -19.03 | -4.41 | [-9.8, +1.4] | 0.36 | -38.27 | **-0.1301x** |
| leader-only (BTC+ETH) | 480 | 80 | -0.0394 | -0.1271 | -30.10 | -5.99 | [-11.7, -0.3] | 0.57 | -39.85 | **-0.1769x** |
| BTC lagged return | 480 | 80 | -0.0334 | -0.1156 | -12.60 | -6.79 | [-12.1, -1.4] | 0.54 | -40.65 | **-0.2006x** |


### BTCUSDT — Kalshi round trip 24.6 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 480 | 80 | +0.0411 | +0.0641 | +5.83 | +0.87 | [-5.3, +7.5] | 0.56 | -23.77 | **0.0352x** |
| equal-weight basket | 480 | 80 | -0.0411 | -0.0641 | -5.83 | -0.87 | [-7.4, +5.4] | 0.56 | -25.51 | **-0.0352x** |
| LASSO (alpha=0.3, 13 nonzero) | 480 | 80 | +0.0061 | -0.0313 | +2.50 | -1.09 | [-6.7, +5.0] | 0.43 | -25.73 | **-0.0444x** |
| own momentum | 480 | 80 | -0.0296 | -0.0700 | -16.93 | -2.09 | [-7.9, +3.7] | 0.54 | -26.73 | **-0.0849x** |
| BTC lagged return | 480 | 80 | -0.0296 | -0.0700 | -16.93 | -2.09 | [-7.9, +3.7] | 0.54 | -26.73 | **-0.0849x** |
| leader-only (BTC+ETH) | 480 | 80 | -0.0578 | -0.0881 | -32.99 | -2.36 | [-8.5, +3.7] | 0.57 | -27.00 | **-0.0958x** |


### DOGEUSDT — Kalshi round trip 33.6 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 480 | 80 | +0.0119 | +0.0616 | +2.20 | +4.07 | [-5.4, +13.1] | 0.56 | -29.51 | **0.1213x** |
| LASSO (alpha=3.0, 5 nonzero) | 480 | 80 | -0.0067 | -0.0648 | -18.35 | +1.33 | [-7.1, +9.4] | 0.40 | -32.25 | **0.0395x** |
| own momentum | 480 | 80 | +0.0367 | -0.0362 | +8.67 | +1.21 | [-8.1, +11.4] | 0.53 | -32.37 | **0.0361x** |
| leader-only (BTC+ETH) | 480 | 80 | -0.0125 | -0.0774 | -24.49 | -3.91 | [-13.1, +5.3] | 0.57 | -37.49 | **-0.1163x** |
| equal-weight basket | 480 | 80 | -0.0119 | -0.0616 | -2.20 | -4.07 | [-13.0, +5.4] | 0.56 | -37.65 | **-0.1213x** |
| BTC lagged return | 480 | 80 | -0.0075 | -0.0593 | -5.68 | -6.25 | [-14.5, +2.2] | 0.54 | -39.83 | **-0.1862x** |


### ETHUSDT — Kalshi round trip 25.0 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 480 | 80 | +0.0506 | +0.1038 | +20.83 | +4.67 | [-3.7, +13.8] | 0.56 | -20.33 | **0.1866x** |
| own momentum | 480 | 80 | -0.0297 | -0.1184 | -37.38 | -3.57 | [-11.1, +3.7] | 0.56 | -28.57 | **-0.1430x** |
| leader-only (BTC+ETH) | 480 | 80 | -0.0193 | -0.1168 | -36.76 | -3.99 | [-12.0, +3.9] | 0.57 | -28.99 | **-0.1596x** |
| equal-weight basket | 480 | 80 | -0.0506 | -0.1038 | -20.83 | -4.67 | [-13.7, +3.7] | 0.56 | -29.67 | **-0.1866x** |
| BTC lagged return | 480 | 80 | -0.0036 | -0.0999 | -14.14 | -5.76 | [-13.0, +1.0] | 0.54 | -30.76 | **-0.2305x** |
| LASSO (alpha=10.0, 1 nonzero) | 480 | 80 | -0.0644 | -0.0866 | -27.40 | -9.29 | [-17.2, -1.5] | 0.16 | -34.29 | **-0.3717x** |


### LINKUSDT — Kalshi round trip 42.6 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 480 | 80 | +0.0621 | +0.1095 | +20.11 | +9.21 | [+0.8, +18.3] | 0.56 | -33.39 | **0.2161x** |
| LASSO (alpha=10.0, 1 nonzero) | 480 | 80 | -0.0437 | -0.0810 | -25.05 | -5.20 | [-14.3, +3.9] | 0.08 | -47.80 | **-0.1221x** |
| BTC lagged return | 480 | 80 | -0.0022 | -0.0991 | -17.03 | -7.75 | [-15.7, -0.2] | 0.54 | -50.35 | **-0.1820x** |
| leader-only (BTC+ETH) | 480 | 80 | -0.0192 | -0.1067 | -36.54 | -7.94 | [-17.0, +0.4] | 0.57 | -50.54 | **-0.1864x** |
| equal-weight basket | 480 | 80 | -0.0621 | -0.1095 | -20.11 | -9.21 | [-18.2, -0.8] | 0.56 | -51.81 | **-0.2161x** |
| own momentum | 480 | 80 | -0.0961 | -0.1176 | -21.16 | -11.21 | [-20.4, -2.9] | 0.55 | -53.81 | **-0.2631x** |


### LTCUSDT — Kalshi round trip 31.2 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 480 | 80 | +0.0905 | +0.0869 | +36.76 | +6.93 | [-0.3, +14.9] | 0.56 | -24.23 | **0.2225x** |
| own momentum | 480 | 80 | -0.0300 | -0.0471 | -30.92 | +3.04 | [-3.9, +9.6] | 0.52 | -28.12 | **0.0975x** |
| equal-weight basket | 480 | 80 | -0.0905 | -0.0869 | -36.76 | -6.93 | [-14.8, +0.4] | 0.56 | -38.09 | **-0.2225x** |
| LASSO (alpha=3.0, 3 nonzero) | 480 | 80 | -0.0727 | -0.0772 | -23.61 | -7.82 | [-13.8, -1.7] | 0.25 | -38.98 | **-0.2508x** |
| leader-only (BTC+ETH) | 480 | 80 | -0.0386 | -0.1019 | -27.33 | -10.22 | [-17.6, -3.2] | 0.57 | -41.38 | **-0.3281x** |
| BTC lagged return | 480 | 80 | -0.0228 | -0.1019 | -29.84 | -11.06 | [-18.5, -3.7] | 0.54 | -42.22 | **-0.3549x** |


### NEARUSDT — Kalshi round trip 39.5 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 480 | 80 | +0.0765 | +0.1057 | +6.78 | +17.41 | [+4.3, +31.0] | 0.56 | -22.07 | **0.4410x** |
| own momentum | 480 | 80 | -0.0515 | -0.0650 | +1.13 | -8.08 | [-21.0, +3.9] | 0.52 | -47.56 | **-0.2046x** |
| leader-only (BTC+ETH) | 480 | 80 | -0.0584 | -0.1077 | -77.42 | -14.38 | [-28.6, +0.1] | 0.57 | -53.86 | **-0.3642x** |
| LASSO (alpha=3.0, 4 nonzero) | 480 | 80 | -0.0705 | -0.0939 | -71.90 | -14.74 | [-27.9, -1.7] | 0.36 | -54.22 | **-0.3734x** |
| BTC lagged return | 480 | 80 | -0.0659 | -0.1069 | -58.89 | -15.07 | [-29.1, -1.3] | 0.54 | -54.55 | **-0.3816x** |
| equal-weight basket | 480 | 80 | -0.0765 | -0.1057 | -6.78 | -17.41 | [-30.8, -4.3] | 0.56 | -56.89 | **-0.4410x** |


### SOLUSDT — Kalshi round trip 28.2 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 480 | 80 | +0.0786 | +0.1106 | +29.38 | +8.61 | [-0.8, +19.0] | 0.56 | -19.63 | **0.3049x** |
| LASSO (alpha=3.0, 2 nonzero) | 480 | 80 | -0.0820 | -0.1047 | -44.75 | -4.30 | [-15.1, +6.2] | 0.47 | -32.54 | **-0.1521x** |
| own momentum | 480 | 80 | -0.0708 | -0.0896 | -22.74 | -7.65 | [-16.3, +0.7] | 0.56 | -35.89 | **-0.2709x** |
| BTC lagged return | 480 | 80 | -0.0429 | -0.0791 | -25.15 | -8.49 | [-17.4, +0.2] | 0.54 | -36.73 | **-0.3006x** |
| leader-only (BTC+ETH) | 480 | 80 | -0.0693 | -0.1044 | -55.17 | -8.51 | [-18.9, +1.3] | 0.57 | -36.75 | **-0.3013x** |
| equal-weight basket | 480 | 80 | -0.0786 | -0.1106 | -29.38 | -8.61 | [-19.0, +0.8] | 0.56 | -36.85 | **-0.3049x** |


### SUIUSDT — Kalshi round trip 39.3 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 480 | 80 | +0.0549 | +0.1057 | +26.26 | +7.83 | [-2.0, +18.8] | 0.56 | -31.47 | **0.1991x** |
| LASSO (alpha=10.0, 2 nonzero) | 480 | 80 | -0.0077 | -0.0926 | -18.15 | +1.91 | [-6.1, +10.0] | 0.04 | -37.39 | **0.0485x** |
| own momentum | 480 | 80 | -0.0995 | -0.1149 | -66.42 | -7.15 | [-17.9, +3.0] | 0.55 | -46.45 | **-0.1818x** |
| equal-weight basket | 480 | 80 | -0.0549 | -0.1057 | -26.26 | -7.83 | [-18.6, +2.0] | 0.56 | -47.13 | **-0.1991x** |
| leader-only (BTC+ETH) | 480 | 80 | -0.0211 | -0.1142 | -38.21 | -10.02 | [-21.3, +1.5] | 0.57 | -49.32 | **-0.2550x** |
| BTC lagged return | 480 | 80 | -0.0067 | -0.0917 | -21.87 | -10.41 | [-20.5, -0.2] | 0.54 | -49.71 | **-0.2649x** |


### XRPUSDT — Kalshi round trip 29.1 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| leader-only (BTC+ETH) | 480 | 80 | +0.0443 | -0.0614 | -8.14 | +3.62 | [-6.8, +15.0] | 0.57 | -25.46 | **0.1247x** |
| BTC lagged return | 480 | 80 | +0.0638 | -0.0300 | +19.38 | +2.43 | [-5.7, +10.9] | 0.54 | -26.65 | **0.0835x** |
| own momentum | 480 | 80 | +0.0402 | -0.0731 | -10.97 | +1.84 | [-7.6, +11.4] | 0.54 | -27.24 | **0.0631x** |
| equal-weight basket | 480 | 80 | +0.0682 | -0.0603 | +21.35 | +1.15 | [-8.0, +10.4] | 0.56 | -27.93 | **0.0397x** |
| seesaw sign (minus basket) | 480 | 80 | -0.0682 | +0.0603 | -21.35 | -1.15 | [-10.3, +8.1] | 0.56 | -30.23 | **-0.0397x** |
| LASSO (alpha=3.0, 3 nonzero) | 480 | 80 | +0.0644 | -0.0553 | -13.36 | -4.05 | [-13.2, +5.2] | 0.16 | -33.13 | **-0.1394x** |


## Verdict

Best across every focal asset and model: **seesaw sign (minus basket)** on ? at **0.441x** of the required effect (+17.41 bp gross against a 39.5 bp round trip).

**Nothing clears the hurdle.** The best specification is worth 44.1% of what it costs to trade on Kalshi.

The LASSO does not beat the simple baselines (0.107x against 0.441x for `seesaw sign (minus basket)`), so the failure is not a modelling failure. There is not enough in the cross-section at this bar length to pay for a Kalshi round trip.


## What this does not settle

- The papers are intraday on Binance with many more coins and, in the seesaw case, a long-short portfolio across the cross-section. This is a directional per-asset test on 14 coins, which is the version that maps onto a Kalshi position.
- The seesaw direction is large-predicts-small. The small coins are exactly the ones that cost 18-21 bp one way on Kalshi, so a replication that works on Binance can still fail on transfer, and that would be `FAILED_TRANSFER` rather than a contradiction of the paper.
- Costs here are the Kalshi round trip. The papers clear their own venues' costs, which are far lower. Both statements can be true at once.

