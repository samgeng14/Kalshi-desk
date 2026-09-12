# Exit engine: the conditional maker round trip

1,946 selected entries, 10 exit policies each, 38,643 exit orders simulated over 0.8 hours of held-out tape.

Net is `(exit price - entry price) x side - entry fee - exit fee`, in bps of the entry price. Nothing is priced by assumption on either leg.

## The table

Intervals are 90% from a bootstrap that resamples whole ONE-MINUTE CLUSTERS. `clusters` is how many independent minutes the interval actually rests on; the trade count is not the sample size.

| entry | exit policy | trades | clusters | /hour | passive exit | median hold | entry edge | exit slip | exit fee | net | 90% interval | random null |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|
| top 10% | `maker_touch_30s` | 1910 | 50 | 2316 | 42% | 30.0s | -5.48 | -7.17 | 9.0 | **-26.69** | [-29.48, -24.19] | -21.66 |
| top 10% | `maker_improve_10s` | 1943 | 50 | 2356 | 27% | 10.0s | -5.13 | -6.99 | 10.1 | **-27.23** | [-28.83, -25.71] | -22.24 |
| top 10% | `maker_touch_10s` | 1942 | 50 | 2355 | 25% | 10.0s | -5.08 | -7.26 | 10.3 | **-27.59** | [-29.27, -25.99] | -22.63 |
| top 10% | `wait5_maker_10s` | 1931 | 50 | 2341 | 26% | 15.1s | -4.94 | -7.48 | 10.2 | **-27.63** | [-29.67, -25.84] | -22.57 |
| top 10% | `taker_now` | 1946 | 50 | 2360 | 0% | 0.0s | -3.72 | -7.35 | 12.0 | **-28.07** | [-28.59, -27.51] | -24.16 |
| top 10% | `maker_touch_5s` | 1942 | 50 | 2355 | 15% | 5.0s | -4.81 | -7.37 | 10.9 | **-28.11** | [-29.34, -26.92] | -22.97 |
| top 10% | `maker_30s_patient` | 1862 | 50 | 2258 | 76% | 38.1s | -11.09 | -6.52 | 6.7 | **-29.31** | [-37.77, -21.83] | -24.86 |
| top 10% | `maker_touch_1s` | 1946 | 50 | 2360 | 6% | 1.0s | -5.21 | -7.50 | 11.6 | **-29.31** | [-30.10, -28.55] | -23.99 |
| top 10% | `maker_replace_10s` | 1940 | 50 | 2352 | 5% | 10.0s | -5.11 | -7.57 | 11.7 | **-29.35** | [-30.75, -28.02] | -24.86 |
| top 10% | `maker_replace_30s` | 1904 | 49 | 2309 | 8% | 30.1s | -5.85 | -7.65 | 11.4 | **-29.91** | [-32.63, -27.33] | -24.81 |
| top 5% | `maker_improve_10s` | 971 | 49 | 1177 | 21% | 10.0s | -5.35 | -7.92 | 10.5 | **-28.81** | [-30.54, -27.16] | -22.49 |
| top 5% | `maker_touch_30s` | 953 | 49 | 1156 | 37% | 30.0s | -6.41 | -8.13 | 9.4 | **-28.92** | [-32.27, -25.69] | -21.85 |
| top 5% | `taker_now` | 973 | 49 | 1180 | 0% | 0.0s | -3.90 | -8.34 | 12.0 | **-29.24** | [-29.86, -28.60] | -24.27 |
| top 5% | `maker_touch_10s` | 971 | 49 | 1177 | 19% | 10.0s | -5.32 | -8.24 | 10.7 | **-29.26** | [-31.07, -27.50] | -22.95 |
| top 5% | `wait5_maker_10s` | 970 | 49 | 1176 | 18% | 15.1s | -5.34 | -8.54 | 10.8 | **-29.65** | [-32.03, -27.40] | -22.84 |
| top 5% | `maker_touch_5s` | 971 | 49 | 1177 | 10% | 5.0s | -5.06 | -8.29 | 11.3 | **-29.68** | [-31.17, -28.21] | -23.14 |
| top 5% | `maker_touch_1s` | 973 | 49 | 1180 | 3% | 1.0s | -5.52 | -8.44 | 11.8 | **-30.76** | [-31.59, -29.91] | -24.32 |
| top 5% | `maker_replace_10s` | 971 | 49 | 1177 | 1% | 10.1s | -5.36 | -8.63 | 11.9 | **-30.89** | [-32.47, -29.30] | -25.35 |
| top 5% | `maker_30s_patient` | 917 | 49 | 1112 | 75% | 42.4s | -12.04 | -7.22 | 6.7 | **-31.01** | [-42.22, -20.69] | -25.04 |
| top 5% | `maker_replace_30s` | 952 | 48 | 1154 | 2% | 30.1s | -6.74 | -8.79 | 11.9 | **-32.39** | [-35.85, -29.14] | -25.31 |
| top 2% | `maker_30s_patient` | 347 | 45 | 421 | 80% | 38.0s | -6.38 | -7.52 | 6.4 | **-25.33** | [-34.02, -17.46] | -24.67 |
| top 2% | `maker_touch_30s` | 373 | 46 | 452 | 38% | 30.0s | -3.95 | -8.39 | 9.3 | **-26.67** | [-30.33, -23.32] | -22.56 |
| top 2% | `maker_improve_10s` | 389 | 46 | 472 | 21% | 10.0s | -4.53 | -8.03 | 10.5 | **-28.06** | [-30.26, -25.92] | -23.12 |
| top 2% | `maker_touch_10s` | 389 | 46 | 472 | 20% | 10.0s | -4.55 | -8.43 | 10.6 | **-28.61** | [-30.95, -26.32] | -23.59 |
| top 2% | `maker_touch_5s` | 389 | 46 | 472 | 11% | 5.0s | -3.93 | -8.56 | 11.3 | **-28.76** | [-31.12, -26.62] | -23.66 |
| top 2% | `taker_now` | 389 | 46 | 472 | 0% | 0.0s | -3.13 | -8.63 | 12.0 | **-28.76** | [-29.83, -27.66] | -24.42 |
| top 2% | `wait5_maker_10s` | 388 | 46 | 470 | 19% | 15.1s | -4.35 | -8.81 | 10.7 | **-28.82** | [-31.47, -26.26] | -22.92 |
| top 2% | `maker_replace_30s` | 372 | 45 | 451 | 1% | 30.1s | -4.29 | -9.03 | 11.9 | **-30.26** | [-33.82, -27.04] | -24.90 |
| top 2% | `maker_replace_10s` | 389 | 46 | 472 | 1% | 10.1s | -4.44 | -9.06 | 11.9 | **-30.45** | [-32.55, -28.44] | -25.50 |
| top 2% | `maker_touch_1s` | 389 | 46 | 472 | 3% | 1.0s | -5.05 | -8.66 | 11.8 | **-30.53** | [-31.94, -29.19] | -24.34 |
| top 1% | `maker_30s_patient` | 165 | 33 | 200 | 79% | 37.5s | -4.96 | -7.71 | 6.4 | **-24.11** | [-34.02, -15.99] | -29.37 |
| top 1% | `maker_touch_30s` | 184 | 34 | 223 | 39% | 30.0s | -3.70 | -8.69 | 9.3 | **-26.64** | [-31.37, -22.70] | -22.76 |
| top 1% | `wait5_maker_10s` | 194 | 35 | 235 | 21% | 15.1s | -3.59 | -8.73 | 10.6 | **-27.88** | [-31.58, -24.86] | -22.70 |
| top 1% | `maker_improve_10s` | 194 | 35 | 235 | 22% | 10.0s | -4.24 | -8.28 | 10.4 | **-27.97** | [-31.24, -25.10] | -22.71 |
| top 1% | `maker_touch_10s` | 194 | 35 | 235 | 21% | 10.0s | -4.30 | -8.68 | 10.6 | **-28.54** | [-31.95, -25.56] | -22.94 |
| top 1% | `maker_touch_5s` | 194 | 35 | 235 | 12% | 5.0s | -3.92 | -8.55 | 11.1 | **-28.61** | [-32.10, -25.76] | -23.15 |
| top 1% | `taker_now` | 194 | 35 | 235 | 0% | 0.0s | -3.15 | -8.53 | 12.0 | **-28.67** | [-30.23, -27.28] | -24.19 |
| top 1% | `maker_replace_30s` | 184 | 34 | 223 | 1% | 30.1s | -4.21 | -8.81 | 12.0 | **-29.98** | [-34.21, -25.75] | -24.61 |
| top 1% | `maker_replace_10s` | 194 | 35 | 235 | 1% | 10.1s | -4.13 | -9.10 | 12.0 | **-30.20** | [-33.11, -27.48] | -25.03 |
| top 1% | `maker_touch_1s` | 194 | 35 | 235 | 3% | 1.0s | -4.99 | -8.72 | 11.8 | **-30.49** | [-32.76, -28.64] | -24.07 |

## Verdict

**Nothing is positive.** The best combination is `top 1%` exited by `maker_30s_patient` at -24.11 bps over 165 trades.

The exit is now measured rather than assumed, and it did not rescue the round trip.

## What the exit policy actually buys

Averaged across entry filters, so the comparison is between policies rather than between subsets.

| exit policy | passive exit | median hold | exit slip | exit fee | net |
|---|---:|---:|---:|---:|---:|
| `taker_now` | 0% | 0.0s | -8.21 | 12.0 | -28.68 |
| `maker_touch_1s` | 4% | 1.0s | -8.33 | 11.8 | -30.27 |
| `maker_touch_5s` | 12% | 5.0s | -8.19 | 11.2 | -28.79 |
| `maker_touch_10s` | 21% | 10.0s | -8.15 | 10.5 | -28.50 |
| `maker_touch_30s` | 39% | 30.0s | -8.09 | 9.3 | -27.23 |
| `maker_improve_10s` | 23% | 10.0s | -7.80 | 10.4 | -28.02 |
| `maker_replace_10s` | 2% | 10.1s | -8.59 | 11.9 | -30.22 |
| `maker_replace_30s` | 3% | 30.1s | -8.57 | 11.8 | -30.63 |
| `wait5_maker_10s` | 21% | 15.1s | -8.39 | 10.5 | -28.49 |
| `maker_30s_patient` | 77% | 39.0s | -7.24 | 6.6 | -27.44 |

`taker_now` is the policy the conditional study assumed for every exit. Every row above it in net terms is a measurement that assumption was getting wrong.

## Orders that did not resolve cleanly


An exit at `max_hold` is counted as `exit_forced` and is included in the numbers: a policy that quietly drops the trades it could not close would report the fills it liked and none of the ones that went wrong.

## What would change these numbers

- Inventory. Each trade is independent here. A real book holds several at once, cannot always exit them separately, and carries the correlation between them.
- Our orders are absent from the replayed book on both legs, so passive fill rates are upper bounds twice over.
- One session. Adverse selection and spread are both regime-dependent and this is one regime.
- The fee is tier 0. `reports/fee_ladder.md` has what each row looks like lower down.

