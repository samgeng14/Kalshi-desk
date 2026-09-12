# Cross-crypto return predictability — replication at 720-minute bars

Sources, verified verbatim before any code: Jia, Wu, Yan & Liu, *J Empirical Finance* 74 (2023); Guo, Sang, Tu & Wang, *JEDC* 163 (2024). Specification in `research/literature/cross_crypto_replication.md`.

Panel: 14 assets, 799 aligned bars, 400 days. Chronological split 60% train / 20% validate / 20% test. The penalty is chosen on validation; the test slice is touched once.

## The headline is the economic ratio

`economic_ratio = |observed effect| / required effect`, where the required effect is the focal asset's OWN Kalshi round trip. A ratio of 1.0 means the signal is exactly worth what it costs to trade; below 1 it loses. Statistical columns are reported beside it, never instead of it.

This is a discovery screen. Clearing it makes a signal worth testing, not worth trading: the ratio cannot see tail concentration, instability through time, turnover cost, or whether a Kalshi contract tracks the underlying return at all.


### AAVEUSDT — Kalshi round trip 38.6 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| own momentum | 160 | 80 | +0.2704 | +0.2403 | +327.50 | +58.24 | [+22.7, +94.5] | 0.42 | +19.68 | **1.5104x** |
| equal-weight basket | 160 | 80 | +0.2273 | +0.1621 | +135.51 | +40.42 | [+7.0, +76.0] | 0.48 | +1.86 | **1.0483x** |
| BTC lagged return | 160 | 80 | +0.1748 | +0.0973 | +188.76 | +34.98 | [+2.4, +69.7] | 0.53 | -3.58 | **0.9070x** |
| leader-only (BTC+ETH) | 160 | 80 | +0.1197 | +0.0891 | +64.08 | +24.24 | [-7.9, +58.8] | 0.56 | -14.32 | **0.6285x** |
| LASSO (alpha=10.0, 5 nonzero) | 160 | 80 | +0.1276 | +0.0222 | +282.15 | -10.33 | [-48.3, +26.5] | 0.03 | -48.89 | **-0.2679x** |
| seesaw sign (minus basket) | 160 | 80 | -0.2273 | -0.1621 | -135.51 | -40.42 | [-75.4, -6.8] | 0.48 | -78.98 | **-1.0483x** |


### ADAUSDT — Kalshi round trip 35.4 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| own momentum | 160 | 80 | +0.1158 | +0.0566 | +53.25 | +32.46 | [+0.9, +65.1] | 0.50 | -2.92 | **0.9176x** |
| seesaw sign (minus basket) | 160 | 80 | -0.0862 | +0.0267 | -103.31 | +5.77 | [-26.6, +38.1] | 0.48 | -29.61 | **0.1630x** |
| equal-weight basket | 160 | 80 | +0.0862 | -0.0267 | +103.31 | -5.77 | [-37.9, +26.7] | 0.48 | -41.15 | **-0.1630x** |
| LASSO (alpha=0.01, 14 nonzero) | 160 | 80 | -0.0193 | -0.0549 | -13.00 | -12.40 | [-46.3, +19.4] | 0.34 | -47.78 | **-0.3505x** |
| BTC lagged return | 160 | 80 | +0.0725 | -0.0992 | +22.70 | -16.40 | [-46.8, +12.9] | 0.53 | -51.78 | **-0.4634x** |
| leader-only (BTC+ETH) | 160 | 80 | +0.0500 | -0.0919 | +77.89 | -34.82 | [-64.6, -5.4] | 0.56 | -70.20 | **-0.9843x** |


### AVAXUSDT — Kalshi round trip 34.0 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| seesaw sign (minus basket) | 160 | 80 | +0.0602 | +0.1366 | +36.52 | +26.82 | [-0.4, +55.5] | 0.48 | -7.18 | **0.7888x** |
| LASSO (alpha=3.0, 9 nonzero) | 160 | 80 | +0.0888 | +0.0356 | +18.20 | +8.51 | [-24.3, +39.2] | 0.33 | -25.49 | **0.2502x** |
| equal-weight basket | 160 | 80 | -0.0602 | -0.1366 | -36.52 | -26.82 | [-55.3, +0.5] | 0.48 | -60.82 | **-0.7888x** |
| BTC lagged return | 160 | 80 | -0.0137 | -0.1627 | -10.73 | -28.30 | [-56.6, -2.3] | 0.53 | -62.30 | **-0.8323x** |
| own momentum | 160 | 80 | -0.1339 | -0.1474 | -146.41 | -29.99 | [-57.9, -1.3] | 0.60 | -63.99 | **-0.8822x** |
| leader-only (BTC+ETH) | 160 | 80 | -0.0720 | -0.1839 | -80.54 | -44.14 | [-74.2, -17.1] | 0.56 | -78.14 | **-1.2981x** |


### BCHUSDT — Kalshi round trip 40.3 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| equal-weight basket | 160 | 80 | +0.1447 | -0.0079 | +204.55 | +19.99 | [-8.4, +52.3] | 0.48 | -20.31 | **0.4961x** |
| own momentum | 160 | 80 | +0.2107 | -0.0091 | +146.06 | +16.36 | [-14.3, +50.1] | 0.50 | -23.94 | **0.4059x** |
| LASSO (alpha=0.01, 14 nonzero) | 160 | 80 | +0.0595 | -0.0430 | +58.14 | +8.57 | [-21.5, +41.8] | 0.45 | -31.73 | **0.2128x** |
| BTC lagged return | 160 | 80 | +0.1289 | -0.0913 | +73.67 | +2.06 | [-25.8, +33.8] | 0.53 | -38.24 | **0.0510x** |
| leader-only (BTC+ETH) | 160 | 80 | +0.0600 | -0.0962 | +36.67 | +0.01 | [-27.4, +31.6] | 0.56 | -40.29 | **0.0002x** |
| seesaw sign (minus basket) | 160 | 80 | -0.1447 | +0.0079 | -204.55 | -19.99 | [-51.9, +8.4] | 0.48 | -60.29 | **-0.4961x** |


### BNBUSDT — Kalshi round trip 33.9 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| own momentum | 160 | 80 | +0.1686 | +0.1006 | +36.67 | +8.71 | [-5.1, +23.4] | 0.48 | -25.15 | **0.2573x** |
| equal-weight basket | 160 | 80 | +0.1231 | +0.0376 | +25.19 | +5.46 | [-9.4, +21.4] | 0.48 | -28.40 | **0.1613x** |
| LASSO (alpha=0.3, 11 nonzero) | 160 | 80 | +0.0612 | +0.0159 | +5.08 | -4.84 | [-19.1, +9.7] | 0.51 | -38.70 | **-0.1430x** |
| seesaw sign (minus basket) | 160 | 80 | -0.1231 | -0.0376 | -25.19 | -5.46 | [-21.3, +9.4] | 0.48 | -39.32 | **-0.1613x** |
| BTC lagged return | 160 | 80 | +0.0707 | -0.0176 | -3.56 | -7.94 | [-21.7, +5.6] | 0.53 | -41.80 | **-0.2345x** |
| leader-only (BTC+ETH) | 160 | 80 | +0.0755 | -0.0377 | -1.62 | -11.65 | [-25.3, +1.8] | 0.56 | -45.51 | **-0.3441x** |


### BTCUSDT — Kalshi round trip 24.6 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| equal-weight basket | 160 | 80 | +0.1770 | +0.0917 | +80.87 | +18.39 | [+0.9, +36.5] | 0.48 | -6.25 | **0.7464x** |
| own momentum | 160 | 80 | +0.1387 | +0.0144 | +36.91 | -2.10 | [-16.6, +12.5] | 0.53 | -26.74 | **-0.0854x** |
| BTC lagged return | 160 | 80 | +0.1387 | +0.0144 | +36.91 | -2.10 | [-16.6, +12.5] | 0.53 | -26.74 | **-0.0854x** |
| LASSO (alpha=3.0, 4 nonzero) | 160 | 80 | +0.1530 | +0.0332 | +45.05 | -2.71 | [-16.6, +13.0] | 0.09 | -27.35 | **-0.1101x** |
| leader-only (BTC+ETH) | 160 | 80 | +0.1346 | +0.0024 | +33.76 | -6.54 | [-21.1, +8.3] | 0.56 | -31.18 | **-0.2655x** |
| seesaw sign (minus basket) | 160 | 80 | -0.1770 | -0.0917 | -80.87 | -18.39 | [-36.4, -0.9] | 0.48 | -43.03 | **-0.7464x** |


### DOGEUSDT — Kalshi round trip 33.6 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| equal-weight basket | 160 | 80 | +0.1628 | +0.0034 | +80.47 | +15.66 | [-8.4, +40.1] | 0.48 | -17.92 | **0.4663x** |
| own momentum | 160 | 80 | +0.0827 | -0.0087 | +79.40 | +0.79 | [-22.7, +23.5] | 0.55 | -32.79 | **0.0237x** |
| LASSO (alpha=10.0, 1 nonzero) | 160 | 80 | +0.1424 | -0.0744 | +49.74 | -0.77 | [-26.4, +22.6] | 0.00 | -34.35 | **-0.0231x** |
| BTC lagged return | 160 | 80 | +0.1424 | -0.0744 | +49.74 | -10.97 | [-32.1, +11.0] | 0.53 | -44.55 | **-0.3266x** |
| seesaw sign (minus basket) | 160 | 80 | -0.1628 | -0.0034 | -80.47 | -15.66 | [-39.9, +8.5] | 0.48 | -49.24 | **-0.4663x** |
| leader-only (BTC+ETH) | 160 | 80 | +0.0772 | -0.0903 | +42.81 | -19.53 | [-40.9, +3.2] | 0.56 | -53.11 | **-0.5816x** |


### ETHUSDT — Kalshi round trip 25.0 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| equal-weight basket | 160 | 80 | +0.1350 | +0.1053 | +91.53 | +28.12 | [+4.2, +54.2] | 0.48 | +3.12 | **1.1250x** |
| own momentum | 160 | 80 | +0.0370 | -0.0165 | +37.22 | +8.02 | [-15.1, +35.1] | 0.55 | -16.98 | **0.3207x** |
| BTC lagged return | 160 | 80 | +0.0995 | +0.0141 | +39.39 | -6.17 | [-29.7, +16.7] | 0.53 | -31.17 | **-0.2469x** |
| leader-only (BTC+ETH) | 160 | 80 | +0.0640 | +0.0003 | +62.98 | -12.65 | [-36.1, +9.3] | 0.56 | -37.65 | **-0.5060x** |
| LASSO (alpha=10.0, 1 nonzero) | 160 | 80 | +0.0995 | +0.0141 | +39.39 | -20.80 | [-47.5, +3.7] | 0.00 | -45.80 | **-0.8318x** |
| seesaw sign (minus basket) | 160 | 80 | -0.1350 | -0.1053 | -91.53 | -28.12 | [-54.1, -4.1] | 0.48 | -53.12 | **-1.1250x** |


### LINKUSDT — Kalshi round trip 42.6 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| own momentum | 160 | 80 | +0.1740 | +0.1338 | +166.60 | +38.69 | [+13.8, +63.7] | 0.46 | -3.91 | **0.9083x** |
| equal-weight basket | 160 | 80 | +0.1612 | +0.1177 | +81.31 | +32.43 | [+6.5, +57.6] | 0.48 | -10.17 | **0.7613x** |
| BTC lagged return | 160 | 80 | +0.1117 | +0.0156 | +55.75 | +6.04 | [-18.2, +31.5] | 0.53 | -36.56 | **0.1419x** |
| leader-only (BTC+ETH) | 160 | 80 | +0.0692 | +0.0117 | +44.87 | -5.49 | [-30.0, +19.5] | 0.56 | -48.09 | **-0.1290x** |
| LASSO (alpha=10.0, 2 nonzero) | 160 | 80 | -0.0157 | -0.0781 | +29.69 | -23.82 | [-51.6, +1.8] | 0.00 | -66.42 | **-0.5592x** |
| seesaw sign (minus basket) | 160 | 80 | -0.1612 | -0.1177 | -81.31 | -32.43 | [-57.5, -6.4] | 0.48 | -75.03 | **-0.7613x** |


### LTCUSDT — Kalshi round trip 31.2 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| equal-weight basket | 160 | 80 | +0.1091 | +0.0828 | +54.19 | +14.19 | [-5.0, +32.2] | 0.48 | -16.97 | **0.4553x** |
| own momentum | 160 | 80 | +0.0261 | -0.0438 | -5.11 | -0.71 | [-17.9, +18.0] | 0.55 | -31.87 | **-0.0228x** |
| BTC lagged return | 160 | 80 | +0.0711 | -0.0167 | +7.92 | -7.42 | [-24.8, +10.9] | 0.53 | -38.58 | **-0.2382x** |
| LASSO (alpha=10.0, 3 nonzero) | 160 | 80 | -0.0391 | +0.0175 | -13.23 | -9.03 | [-27.3, +9.0] | 0.00 | -40.19 | **-0.2900x** |
| seesaw sign (minus basket) | 160 | 80 | -0.1091 | -0.0828 | -54.19 | -14.19 | [-32.2, +5.0] | 0.48 | -45.35 | **-0.4553x** |
| leader-only (BTC+ETH) | 160 | 80 | +0.0045 | -0.0489 | -18.14 | -14.94 | [-32.6, +4.2] | 0.56 | -46.10 | **-0.4796x** |


### NEARUSDT — Kalshi round trip 39.5 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| own momentum | 160 | 80 | +0.0811 | +0.0975 | +57.30 | +36.33 | [-0.8, +72.9] | 0.47 | -3.15 | **0.9202x** |
| equal-weight basket | 160 | 80 | +0.0930 | +0.0707 | +110.46 | +36.13 | [+4.3, +67.8] | 0.48 | -3.35 | **0.9152x** |
| BTC lagged return | 160 | 80 | +0.0972 | +0.0244 | +81.00 | +31.18 | [-4.2, +65.3] | 0.53 | -8.30 | **0.7899x** |
| LASSO (alpha=3.0, 11 nonzero) | 160 | 80 | +0.0770 | +0.0440 | +119.09 | +19.40 | [-16.3, +56.4] | 0.28 | -20.08 | **0.4915x** |
| leader-only (BTC+ETH) | 160 | 80 | +0.0837 | +0.0480 | +48.80 | +12.20 | [-22.3, +46.3] | 0.56 | -27.28 | **0.3090x** |
| seesaw sign (minus basket) | 160 | 80 | -0.0930 | -0.0707 | -110.46 | -36.13 | [-67.8, -4.3] | 0.48 | -75.61 | **-0.9152x** |


### SOLUSDT — Kalshi round trip 28.2 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| own momentum | 160 | 80 | +0.1027 | +0.0662 | +42.28 | +43.56 | [+20.1, +67.6] | 0.46 | +15.32 | **1.5424x** |
| equal-weight basket | 160 | 80 | +0.0856 | +0.0065 | +15.38 | +17.87 | [-6.4, +43.0] | 0.48 | -10.37 | **0.6327x** |
| BTC lagged return | 160 | 80 | +0.0572 | -0.0570 | +0.04 | -2.72 | [-24.3, +18.9] | 0.53 | -30.96 | **-0.0964x** |
| leader-only (BTC+ETH) | 160 | 80 | +0.0407 | -0.0645 | +15.42 | -11.80 | [-33.2, +10.0] | 0.56 | -40.04 | **-0.4178x** |
| LASSO (alpha=10.0, 1 nonzero) | 160 | 80 | +0.0572 | -0.0570 | +0.04 | -11.91 | [-35.4, +10.3] | 0.04 | -40.15 | **-0.4218x** |
| seesaw sign (minus basket) | 160 | 80 | -0.0856 | -0.0065 | -15.38 | -17.87 | [-42.7, +6.5] | 0.48 | -46.11 | **-0.6327x** |


### SUIUSDT — Kalshi round trip 39.3 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| equal-weight basket | 160 | 80 | +0.1054 | +0.0036 | +61.72 | +12.59 | [-13.1, +37.8] | 0.48 | -26.71 | **0.3204x** |
| LASSO (alpha=0.01, 14 nonzero) | 160 | 80 | +0.0247 | -0.0258 | -41.97 | +6.58 | [-20.0, +34.7] | 0.25 | -32.72 | **0.1675x** |
| own momentum | 160 | 80 | +0.0834 | +0.0061 | +72.81 | +4.85 | [-19.1, +30.8] | 0.52 | -34.45 | **0.1235x** |
| seesaw sign (minus basket) | 160 | 80 | -0.1054 | -0.0036 | -61.72 | -12.59 | [-37.6, +13.3] | 0.48 | -51.89 | **-0.3204x** |
| BTC lagged return | 160 | 80 | +0.0561 | -0.1079 | +11.89 | -14.00 | [-37.6, +7.9] | 0.53 | -53.30 | **-0.3563x** |
| leader-only (BTC+ETH) | 160 | 80 | +0.0344 | -0.1061 | +20.17 | -31.50 | [-54.5, -9.1] | 0.56 | -70.80 | **-0.8015x** |


### XRPUSDT — Kalshi round trip 29.1 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| own momentum | 160 | 80 | +0.4209 | +0.1490 | +229.64 | +34.14 | [+8.2, +63.2] | 0.47 | +5.06 | **1.1740x** |
| equal-weight basket | 160 | 80 | +0.3528 | +0.1741 | +223.19 | +32.06 | [+5.2, +60.4] | 0.48 | +2.98 | **1.1024x** |
| BTC lagged return | 160 | 80 | +0.3176 | +0.0778 | +182.15 | +12.58 | [-10.9, +36.0] | 0.53 | -16.50 | **0.4326x** |
| leader-only (BTC+ETH) | 160 | 80 | +0.3113 | +0.0829 | +179.92 | +3.42 | [-18.3, +27.4] | 0.56 | -25.66 | **0.1175x** |
| LASSO (alpha=10.0, 3 nonzero) | 160 | 80 | -0.2838 | -0.1513 | -122.49 | -11.46 | [-43.0, +18.4] | 0.00 | -40.54 | **-0.3940x** |
| seesaw sign (minus basket) | 160 | 80 | -0.3528 | -0.1741 | -223.19 | -32.06 | [-60.2, -5.0] | 0.48 | -61.14 | **-1.1024x** |


## Verdict

Best across every focal asset and model: **own momentum** on ? at **1.542x** of the required effect (+43.56 bp gross against a 28.2 bp round trip).

The point estimate clears the hurdle but the interval does not. That is a reason to record more data, not to trade.


## What this does not settle

- The papers are intraday on Binance with many more coins and, in the seesaw case, a long-short portfolio across the cross-section. This is a directional per-asset test on 14 coins, which is the version that maps onto a Kalshi position.
- The seesaw direction is large-predicts-small. The small coins are exactly the ones that cost 18-21 bp one way on Kalshi, so a replication that works on Binance can still fail on transfer, and that would be `FAILED_TRANSFER` rather than a contradiction of the paper.
- Costs here are the Kalshi round trip. The papers clear their own venues' costs, which are far lower. Both statements can be true at once.

