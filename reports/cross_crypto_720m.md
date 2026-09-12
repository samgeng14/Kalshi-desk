# Cross-crypto return predictability — replication at 720-minute bars

Sources, verified verbatim before any code: Jia, Wu, Yan & Liu, *J Empirical Finance* 74 (2023); Guo, Sang, Tu & Wang, *JEDC* 163 (2024). Specification in `research/literature/cross_crypto_replication.md`.

Panel: 8 assets, 785 aligned bars, 393 days. Chronological split 60% train / 20% validate / 20% test. The penalty is chosen on validation; the test slice is touched once.

## The headline is the economic ratio

`economic_ratio = |observed effect| / required effect`, where the required effect is the focal asset's OWN Kalshi round trip. A ratio of 1.0 means the signal is exactly worth what it costs to trade; below 1 it loses. Statistical columns are reported beside it, never instead of it.

This is a discovery screen. Clearing it makes a signal worth testing, not worth trading: the ratio cannot see tail concentration, instability through time, turnover cost, or whether a Kalshi contract tracks the underlying return at all.


### ADAUSDT — Kalshi round trip 35.4 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| own momentum | 157 | 79 | +0.1215 | +0.0719 | +69.12 | +38.03 | [+6.9, +70.0] | 0.49 | +2.65 | **1.0749x** |
| equal-weight basket | 157 | 79 | +0.0886 | -0.0050 | +188.44 | +9.70 | [-20.9, +42.3] | 0.51 | -25.68 | **0.2741x** |
| seesaw sign (minus basket) | 157 | 79 | -0.0886 | +0.0050 | -188.44 | -9.70 | [-42.2, +21.4] | 0.51 | -45.08 | **-0.2741x** |
| BTC lagged return | 157 | 79 | +0.0957 | -0.0549 | +78.14 | -10.67 | [-40.2, +19.1] | 0.53 | -46.05 | **-0.3016x** |
| LASSO (alpha=10.0, 1 nonzero) | 157 | 79 | -0.0203 | +0.0865 | -35.74 | -11.58 | [-44.5, +21.0] | 0.00 | -46.96 | **-0.3274x** |
| leader-only (BTC+ETH) | 157 | 79 | +0.0716 | -0.0515 | +105.48 | -29.45 | [-60.1, +1.0] | 0.56 | -64.83 | **-0.8324x** |


### BTCUSDT — Kalshi round trip 24.6 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| equal-weight basket | 157 | 79 | +0.1628 | +0.0818 | +98.32 | +13.73 | [-4.0, +33.3] | 0.51 | -10.91 | **0.5573x** |
| LASSO (alpha=3.0, 4 nonzero) | 157 | 79 | +0.2239 | +0.1047 | +93.43 | +3.35 | [-11.7, +17.9] | 0.03 | -21.29 | **0.1359x** |
| own momentum | 157 | 79 | +0.1450 | +0.0250 | +46.63 | -1.89 | [-16.4, +14.0] | 0.53 | -26.53 | **-0.0767x** |
| BTC lagged return | 157 | 79 | +0.1450 | +0.0250 | +46.63 | -1.89 | [-16.4, +14.0] | 0.53 | -26.53 | **-0.0767x** |
| leader-only (BTC+ETH) | 157 | 79 | +0.1411 | +0.0140 | +38.92 | -6.41 | [-20.4, +9.2] | 0.56 | -31.05 | **-0.2603x** |
| seesaw sign (minus basket) | 157 | 79 | -0.1628 | -0.0818 | -98.32 | -13.73 | [-33.2, +4.0] | 0.51 | -38.37 | **-0.5573x** |


### DOGEUSDT — Kalshi round trip 33.6 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| equal-weight basket | 157 | 79 | +0.1440 | -0.0046 | +159.14 | +13.84 | [-8.9, +38.2] | 0.51 | -19.74 | **0.4123x** |
| LASSO (alpha=10.0, 1 nonzero) | 157 | 79 | +0.1806 | -0.0445 | +70.12 | +2.92 | [-21.2, +25.2] | 0.00 | -30.66 | **0.0868x** |
| own momentum | 157 | 79 | +0.0819 | -0.0077 | +113.00 | -2.13 | [-24.2, +22.1] | 0.55 | -35.71 | **-0.0635x** |
| BTC lagged return | 157 | 79 | +0.1806 | -0.0445 | +70.12 | -4.98 | [-25.8, +16.9] | 0.53 | -38.56 | **-0.1484x** |
| leader-only (BTC+ETH) | 157 | 79 | +0.1043 | -0.0706 | +70.02 | -13.71 | [-34.0, +8.9] | 0.56 | -47.29 | **-0.4082x** |
| seesaw sign (minus basket) | 157 | 79 | -0.1440 | +0.0046 | -159.14 | -13.84 | [-37.8, +9.1] | 0.51 | -47.42 | **-0.4123x** |


### ETHUSDT — Kalshi round trip 25.0 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| equal-weight basket | 157 | 79 | +0.1035 | +0.0817 | +105.03 | +21.93 | [-1.7, +49.9] | 0.51 | -3.07 | **0.8774x** |
| own momentum | 157 | 79 | +0.0391 | -0.0022 | +35.46 | +10.30 | [-12.7, +37.9] | 0.53 | -14.70 | **0.4118x** |
| BTC lagged return | 157 | 79 | +0.1017 | +0.0324 | +52.15 | -5.12 | [-28.0, +18.3] | 0.53 | -30.12 | **-0.2048x** |
| leader-only (BTC+ETH) | 157 | 79 | +0.0660 | +0.0158 | +67.75 | -11.72 | [-35.5, +10.9] | 0.56 | -36.72 | **-0.4689x** |
| LASSO (alpha=10.0, 1 nonzero) | 157 | 79 | +0.1017 | +0.0324 | +52.15 | -18.71 | [-48.1, +6.8] | 0.00 | -43.71 | **-0.7485x** |
| seesaw sign (minus basket) | 157 | 79 | -0.1035 | -0.0817 | -105.03 | -21.93 | [-49.8, +1.7] | 0.51 | -46.93 | **-0.8774x** |


### LINKUSDT — Kalshi round trip 42.6 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| own momentum | 157 | 79 | +0.1278 | +0.0664 | +103.53 | +25.00 | [+3.4, +47.5] | 0.48 | -17.60 | **0.5869x** |
| equal-weight basket | 157 | 79 | +0.1071 | +0.0813 | +132.45 | +24.35 | [+1.3, +48.1] | 0.51 | -18.25 | **0.5717x** |
| BTC lagged return | 157 | 79 | +0.1068 | +0.0088 | +65.81 | +2.65 | [-19.5, +27.0] | 0.53 | -39.95 | **0.0623x** |
| leader-only (BTC+ETH) | 157 | 79 | +0.0639 | +0.0065 | +58.89 | -9.10 | [-31.2, +14.7] | 0.56 | -51.70 | **-0.2137x** |
| LASSO (alpha=10.0, 2 nonzero) | 157 | 79 | +0.0686 | -0.0259 | +129.97 | -19.63 | [-44.6, +6.0] | 0.00 | -62.23 | **-0.4609x** |
| seesaw sign (minus basket) | 157 | 79 | -0.1071 | -0.0813 | -132.45 | -24.35 | [-47.9, -1.3] | 0.51 | -66.95 | **-0.5717x** |


### SOLUSDT — Kalshi round trip 28.2 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| own momentum | 157 | 79 | +0.1226 | +0.0897 | +56.52 | +43.47 | [+20.7, +68.6] | 0.47 | +15.23 | **1.5394x** |
| equal-weight basket | 157 | 79 | +0.0823 | +0.0251 | +60.01 | +24.12 | [+0.7, +48.8] | 0.51 | -4.12 | **0.8539x** |
| BTC lagged return | 157 | 79 | +0.0835 | -0.0175 | +40.83 | +1.73 | [-20.0, +23.1] | 0.53 | -26.51 | **0.0612x** |
| leader-only (BTC+ETH) | 157 | 79 | +0.0594 | -0.0286 | +34.30 | -7.52 | [-28.9, +14.1] | 0.56 | -35.76 | **-0.2664x** |
| LASSO (alpha=10.0, 1 nonzero) | 157 | 79 | +0.0835 | -0.0175 | +40.83 | -12.54 | [-37.0, +11.1] | 0.04 | -40.78 | **-0.4440x** |
| seesaw sign (minus basket) | 157 | 79 | -0.0823 | -0.0251 | -60.01 | -24.12 | [-48.8, -0.5] | 0.51 | -52.36 | **-0.8539x** |


### SUIUSDT — Kalshi round trip 39.3 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| equal-weight basket | 157 | 79 | +0.1058 | +0.0185 | +132.62 | +20.81 | [-4.4, +48.3] | 0.51 | -18.49 | **0.5296x** |
| own momentum | 157 | 79 | +0.0921 | +0.0191 | +125.62 | +8.08 | [-17.0, +34.9] | 0.53 | -31.22 | **0.2056x** |
| LASSO (alpha=10.0, 3 nonzero) | 157 | 79 | +0.1532 | +0.0003 | +102.80 | +7.38 | [-17.3, +31.5] | 0.01 | -31.92 | **0.1877x** |
| BTC lagged return | 157 | 79 | +0.1027 | -0.0562 | +83.70 | -6.12 | [-30.0, +18.8] | 0.53 | -45.42 | **-0.1557x** |
| seesaw sign (minus basket) | 157 | 79 | -0.1058 | -0.0185 | -132.62 | -20.81 | [-47.9, +4.7] | 0.51 | -60.11 | **-0.5296x** |
| leader-only (BTC+ETH) | 157 | 79 | +0.0768 | -0.0577 | +74.92 | -23.95 | [-47.9, +1.8] | 0.56 | -63.25 | **-0.6095x** |


### XRPUSDT — Kalshi round trip 29.1 bp

| model | n | clusters | IC | rank IC | top−bottom | gross bps/trade | 90% interval | turnover | net bps/trade | economic ratio |
|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| equal-weight basket | 157 | 79 | +0.3731 | +0.1924 | +272.55 | +39.05 | [+12.0, +70.3] | 0.51 | +9.97 | **1.3427x** |
| own momentum | 157 | 79 | +0.4493 | +0.1588 | +243.34 | +38.64 | [+11.3, +69.2] | 0.46 | +9.56 | **1.3287x** |
| BTC lagged return | 157 | 79 | +0.3547 | +0.1190 | +219.40 | +18.32 | [-3.4, +42.9] | 0.53 | -10.76 | **0.6298x** |
| leader-only (BTC+ETH) | 157 | 79 | +0.3442 | +0.1195 | +207.77 | +8.98 | [-12.5, +35.3] | 0.56 | -20.10 | **0.3087x** |
| LASSO (alpha=10.0, 2 nonzero) | 157 | 79 | -0.2926 | -0.1689 | -139.59 | -7.67 | [-40.5, +19.6] | 0.00 | -36.75 | **-0.2639x** |
| seesaw sign (minus basket) | 157 | 79 | -0.3731 | -0.1924 | -272.55 | -39.05 | [-70.0, -11.8] | 0.51 | -68.13 | **-1.3427x** |


## Verdict

Best across every focal asset and model: **own momentum** on ? at **1.539x** of the required effect (+43.47 bp gross against a 28.2 bp round trip).

The point estimate clears the hurdle but the interval does not. That is a reason to record more data, not to trade.


## What this does not settle

- The papers are intraday on Binance with many more coins and, in the seesaw case, a long-short portfolio across the cross-section. This is a directional per-asset test on 8 coins, which is the version that maps onto a Kalshi position.
- The seesaw direction is large-predicts-small. The small coins are exactly the ones that cost 18-21 bp one way on Kalshi, so a replication that works on Binance can still fail on transfer, and that would be `FAILED_TRANSFER` rather than a contradiction of the paper.
- Costs here are the Kalshi round trip. The papers clear their own venues' costs, which are far lower. Both statements can be true at once.

