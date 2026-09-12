# Live portfolio V1 — 2026-09-12T23:07:18Z

**Mode: PORTFOLIO_V1.** PnL from this deployment is labelled `PORTFOLIO_V1`.

```
ACCOUNT
-------
equity              $49.8588
available           $49.8588
margin used         $0.0000  (0.0% of equity, cap 35%)
gross exposure      $0.0000  (0.00x equity, cap 1.0x)
net exposure        $0.0000  (0.00x equity)
PC1 exposure        +0.0066  (weight units)
drawdown            0.00%  (warn 5%, halt 10%)

POSITIONS
---------
(flat)

MODEL
-----
spec                PORTFOLIO_V1   hash 71ecd439009947ad
mode                PORTFOLIO_V1
pnl label           PORTFOLIO_V1
last signal bar     2026-09-11 23:00 UTC (24.1 h old at decision time)
top longs           ETHUSDT, LTCUSDT, NEARUSDT
top shorts          BCHUSDT, BTCUSDT, DOGEUSDT
next rebalance      2026-09-19 23:07 UTC

PERFORMANCE
-----------
shadow V1 return    (accrues from the first completed rebalance)
live V1 return      (accrues from the first fill)
tracking difference (live minus shadow, reported once both exist)
signal PnL          )
execution drag      ) decomposed in reports/live_portfolio_v1.csv
fees                )
funding             )
```

## Feasibility against the pre-set thresholds

| check | value | threshold | verdict |
|---|---:|---:|---|
| relative_weight_tracking_error | 0.1574 | 0.3000 | PASS |
| net_exposure_over_gross | 0.0497 | 0.2000 | PASS |
| pc1_exposure_over_gross | 0.0247 | 0.2500 | PASS |
| representable_legs | 6.0000 | 4.0000 | PASS |

## No-trade test

| symbol | delta w | benefit bps | cost bps | ratio | required | trade |
|---|---:|---:|---:|---:|---:|---|
| BCHUSDT | -0.1357 | 20.554 | 2.734 | 7.52 | 1.50 | yes |
| BTCUSDT | -0.1550 | 3.488 | 1.909 | 1.83 | 1.50 | yes |
| DOGEUSDT | -0.1698 | 3.591 | 2.851 | 1.26 | 1.50 | no |
| ETHUSDT | +0.1519 | 4.380 | 1.898 | 2.31 | 1.50 | yes |
| LTCUSDT | +0.2147 | 8.224 | 3.346 | 2.46 | 1.50 | yes |
| NEARUSDT | +0.1420 | 22.644 | 2.804 | 8.08 | 1.50 | yes |

Ledger: `reports/live_portfolio_v1.csv`
