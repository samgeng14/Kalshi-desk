# Weather settlement watch

Generated 2026-09-15 14:54:02Z. One row per station-day. `metar` is what `weather_nws` observes, `CLI` is the NWS climate maximum Kalshi's rules name, `Kalshi` is the interval the settled ladder pins the paid value to.


## Gate A: does The Weather Company settle on the NWS climate value?

**30 of 30** station-days agree (100.0%). Every disagreement is listed below; one is enough to stop graduation.


## Section 10: the settlement shift, per station, with its sample size

| series | station | n | mean shift | sd | observed distribution |
|---|---|---|---|---|---|
| `KXHIGHNY` | KNYC | **9** | +0.64 | 0.51 | +0:3 +1:6 |
| `KXHIGHAUS` | KAUS | **4** | +11.77 | 9.17 | +0:1 +11:1 +13:1 +22:1 |
| `KXHIGHCHI` | KMDW | **4** | +7.85 | 5.36 | +0:1 +8:1 +11:1 +12:1 |
| `KXHIGHLAX` | KLAX | **4** | +5.76 | 5.13 | -1:1 +4:1 +10:2 |
| `KXHIGHMIA` | KMIA | **4** | +6.55 | 5.44 | +0:1 +4:1 +10:1 +12:1 |
| `KXHIGHPHIL` | KPHL | **4** | +5.07 | 4.37 | -1:1 +4:1 +8:1 +9:1 |
| `KXLOWTCHI` | KMDW | **4** | -4.67 | 5.01 | -11:1 -7:1 -1:1 +0:1 |
| `KXLOWTMIN` | KMSP | **4** | -4.70 | 5.97 | -13:1 -5:1 -1:1 +0:1 |

The model currently ships `{+0: 0.60, +1: 0.40}` for dense stations and `{+0: 0.29, +1: 0.57, +2: 0.14}` for Central Park. Any station above with n in single digits cannot confirm or refute those, and the report says n so that a point estimate is never mistaken for evidence.


## The intraday CLI: when the settling value becomes readable

The NWS issues a CLI several times a day, each carrying the daily maximum so far from the station's 5-minute record. They are not corrections. Only the issuance after local midnight is complete - but the afternoon one usually already carries the final value, and these markets trade until 01:00 local.


| issuance (local) | already equals the final value | n |
|---|---|---|
| 07:00 | **0%** | 9 |
| 16:00 | **81%** | 43 |
| 17:00 | **85%** | 20 |
| 18:00 | **91%** | 11 |
| 19:00 | **100%** | 9 |
| 20:00 | **100%** | 2 |
| 01:00 next day | **100%** | 43 |
| 02:00 next day | **100%** | 18 |
| 03:00 next day | **100%** | 2 |
| 04:00 next day | **100%** | 10 |

An issuance that is not yet final can only be low, because a running maximum cannot fall. So the afternoon CLI is a hard floor on the settled value, and the only question left is how much further the day climbs.


## Days still running (recorded, not yet comparable)

1 station-days have a CLI value from an issuance before the local day ended. They are stored as provisional and excluded from every agreement and shift number above.


## Every row

| series | date | metar | obs | CLI | shift | Kalshi | agrees |
|---|---|---|---|---|---|---|---|
| `KXHIGHAUS` | 2026-09-05 | - | - | 100 | - | - | - |
| `KXHIGHCHI` | 2026-09-05 | - | - | 83 | - | - | - |
| `KXHIGHLAX` | 2026-09-05 | - | - | 81 | - | - | - |
| `KXHIGHMIA` | 2026-09-05 | - | - | 92 | - | - | - |
| `KXHIGHNY` | 2026-09-05 | - | 7 | 79 | - | - | - |
| `KXHIGHPHIL` | 2026-09-05 | - | - | 84 | - | - | - |
| `KXLOWTCHI` | 2026-09-05 | - | - | 73 | - | - | - |
| `KXLOWTMIN` | 2026-09-05 | - | - | 67 | - | - | - |
| `KXHIGHAUS` | 2026-09-06 | - | - | 96 | - | - | - |
| `KXHIGHCHI` | 2026-09-06 | - | - | 79 | - | - | - |
| `KXHIGHLAX` | 2026-09-06 | - | - | 76 | - | - | - |
| `KXHIGHMIA` | 2026-09-06 | - | - | 92 | - | - | - |
| `KXHIGHNY` | 2026-09-06 | 75.0 | 31 | 76 | +1.0 | - | - |
| `KXHIGHPHIL` | 2026-09-06 | - | - | 83 | - | - | - |
| `KXLOWTCHI` | 2026-09-06 | - | - | 71 | - | - | - |
| `KXLOWTMIN` | 2026-09-06 | - | - | 69 | - | - | - |
| `KXHIGHAUS` | 2026-09-07 | - | - | 101 | - | [101, 102] | yes |
| `KXHIGHCHI` | 2026-09-07 | - | - | 80 | - | [80, 81] | yes |
| `KXHIGHLAX` | 2026-09-07 | - | - | 85 | - | [84, +inf] | yes |
| `KXHIGHMIA` | 2026-09-07 | - | - | 92 | - | [92, 93] | yes |
| `KXHIGHNY` | 2026-09-07 | 77.0 | 24 | 78 | +1.0 | [77, 78] | yes |
| `KXHIGHPHIL` | 2026-09-07 | - | - | 82 | - | [82, 83] | yes |
| `KXLOWTCHI` | 2026-09-07 | - | - | 66 | - | - | - |
| `KXLOWTMIN` | 2026-09-07 | - | - | 69 | - | - | - |
| `KXHIGHAUS` | 2026-09-08 | - | - | 102 | - | [102, 103] | yes |
| `KXHIGHCHI` | 2026-09-08 | - | - | 87 | - | [87, 88] | yes |
| `KXHIGHLAX` | 2026-09-08 | - | - | 92 | - | [90, +inf] | yes |
| `KXHIGHMIA` | 2026-09-08 | - | - | 92 | - | [92, 93] | yes |
| `KXHIGHNY` | 2026-09-08 | 79.0 | 24 | 80 | +1.0 | [80, 81] | yes |
| `KXHIGHPHIL` | 2026-09-08 | - | - | 85 | - | [84, 85] | yes |
| `KXLOWTCHI` | 2026-09-08 | - | - | 66 | - | - | - |
| `KXLOWTMIN` | 2026-09-08 | - | - | 66 | - | - | - |
| `KXHIGHAUS` | 2026-09-09 | - | - | 100 | - | [100, 101] | yes |
| `KXHIGHCHI` | 2026-09-09 | - | - | 80 | - | [80, 81] | yes |
| `KXHIGHLAX` | 2026-09-09 | - | - | 99 | - | [93, +inf] | yes |
| `KXHIGHMIA` | 2026-09-09 | - | - | 90 | - | [89, 90] | yes |
| `KXHIGHNY` | 2026-09-09 | 81.0 | 24 | 82 | +1.0 | [-inf, 83] | yes |
| `KXHIGHPHIL` | 2026-09-09 | - | - | 89 | - | [88, 89] | yes |
| `KXLOWTCHI` | 2026-09-09 | - | - | 71 | - | - | - |
| `KXLOWTMIN` | 2026-09-09 | - | - | 64 | - | - | - |
| `KXHIGHAUS` | 2026-09-10 | - | - | 100 | - | [99, 100] | yes |
| `KXHIGHCHI` | 2026-09-10 | - | - | 76 | - | [75, 76] | yes |
| `KXHIGHLAX` | 2026-09-10 | - | - | 92 | - | [91, +inf] | yes |
| `KXHIGHMIA` | 2026-09-10 | - | - | 91 | - | [90, 91] | yes |
| `KXHIGHNY` | 2026-09-10 | 84.0 | 26 | 84 | -0.0 | [-inf, 84] | yes |
| `KXHIGHPHIL` | 2026-09-10 | - | - | 90 | - | [90, 91] | yes |
| `KXLOWTCHI` | 2026-09-10 | - | - | 65 | - | - | - |
| `KXLOWTMIN` | 2026-09-10 | - | - | 58 | - | - | - |
| `KXHIGHAUS` | 2026-09-11 | 80.6 | 277 | 103 | +22.4 | [102, 103] | yes |
| `KXHIGHCHI` | 2026-09-11 | 73.4 | 281 | 81 | +7.6 | [80, 81] | yes |
| `KXHIGHLAX` | 2026-09-11 | 75.2 | 305 | 85 | +9.8 | [84, 85] | yes |
| `KXHIGHMIA` | 2026-09-11 | 84.2 | 267 | 88 | +3.8 | [88, 89] | yes |
| `KXHIGHNY` | 2026-09-11 | 78.1 | 24 | 79 | +0.9 | [79, 80] | yes |
| `KXHIGHPHIL` | 2026-09-11 | 73.4 | 268 | 81 | +7.6 | [81, 82] | yes |
| `KXLOWTCHI` | 2026-09-11 | 71.6 | 281 | 61 | -10.6 | - | - |
| `KXLOWTMIN` | 2026-09-11 | 75.0 | 283 | 62 | -13.0 | - | - |
| `KXHIGHAUS` | 2026-09-12 | 87.8 | 315 | 101 | +13.2 | - | - |
| `KXHIGHCHI` | 2026-09-12 | 77.0 | 312 | 88 | +11.0 | - | - |
| `KXHIGHLAX` | 2026-09-12 | 72.0 | 316 | 82 | +10.0 | - | - |
| `KXHIGHMIA` | 2026-09-12 | 80.6 | 313 | 93 | +12.4 | - | - |
| `KXHIGHNY` | 2026-09-12 | 78.1 | 25 | 78 | -0.1 | - | - |
| `KXHIGHPHIL` | 2026-09-12 | 73.9 | 303 | 83 | +9.1 | - | - |
| `KXLOWTCHI` | 2026-09-12 | 75.0 | 312 | 68 | -7.0 | - | - |
| `KXLOWTMIN` | 2026-09-12 | 59.0 | 308 | 58 | -1.0 | - | - |
| `KXHIGHAUS` | 2026-09-13 | 89.6 | 315 | 101 | +11.4 | - | - |
| `KXHIGHCHI` | 2026-09-13 | 62.6 | 312 | 75 | +12.4 | - | - |
| `KXHIGHLAX` | 2026-09-13 | 75.2 | 315 | 79 | +3.8 | - | - |
| `KXHIGHMIA` | 2026-09-13 | 82.4 | 314 | 92 | +9.6 | - | - |
| `KXHIGHNY` | 2026-09-13 | 78.1 | 52 | 79 | +0.9 | - | - |
| `KXHIGHPHIL` | 2026-09-13 | 80.6 | 276 | 85 | +4.4 | - | - |
| `KXLOWTCHI` | 2026-09-13 | 57.0 | 312 | 56 | -1.0 | - | - |
| `KXLOWTMIN` | 2026-09-13 | 59.0 | 306 | 54 | -5.0 | - | - |
| `KXHIGHAUS` | 2026-09-14 | 100.9 | 313 | 101 | +0.1 | - | - |
| `KXHIGHCHI` | 2026-09-14 | 71.6 | 310 | 72 | +0.4 | - | - |
| `KXHIGHLAX` | 2026-09-14 | 80.6 | 311 | 80 | -0.6 | - | - |
| `KXHIGHMIA` | 2026-09-14 | 89.6 | 304 | 90 | +0.4 | - | - |
| `KXHIGHNY` | 2026-09-14 | 75.0 | 22 | 75 | -0.0 | - | - |
| `KXHIGHPHIL` | 2026-09-14 | 78.8 | 311 | 78 | -0.8 | - | - |
| `KXLOWTCHI` | 2026-09-14 | 53.1 | 310 | 53 | -0.1 | - | - |
| `KXLOWTMIN` | 2026-09-14 | 51.8 | 309 | 52 | +0.2 | - | - |
| `KXHIGHAUS` | 2026-09-15 | 84.2 | 128 | 82 | - | - | - |
| `KXHIGHCHI` | 2026-09-15 | 73.4 | 125 | - | - | - | - |
| `KXHIGHLAX` | 2026-09-15 | 71.6 | 99 | - | - | - | - |
| `KXHIGHMIA` | 2026-09-15 | 89.6 | 138 | - | - | - | - |
| `KXHIGHNY` | 2026-09-15 | - | 10 | - | - | - | - |
| `KXHIGHPHIL` | 2026-09-15 | 69.8 | 138 | - | - | - | - |
| `KXLOWTCHI` | 2026-09-15 | 68.0 | 125 | - | - | - | - |
| `KXLOWTMIN` | 2026-09-15 | 59.0 | 124 | - | - | - | - |
