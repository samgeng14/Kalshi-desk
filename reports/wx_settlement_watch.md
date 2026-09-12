# Weather settlement watch

Generated 2026-09-12 23:16:23Z. One row per station-day. `metar` is what `weather_nws` observes, `CLI` is the NWS climate maximum Kalshi's rules name, `Kalshi` is the interval the settled ladder pins the paid value to.


## Gate A: does The Weather Company settle on the NWS climate value?

**30 of 30** station-days agree (100.0%). Every disagreement is listed below; one is enough to stop graduation.


## Section 10: the settlement shift, per station, with its sample size

| series | station | n | mean shift | sd | observed distribution |
|---|---|---|---|---|---|
| `KXHIGHNY` | KNYC | **6** | +0.82 | 0.42 | +0:1 +1:5 |
| `KXHIGHAUS` | KAUS | **1** | +0.80 | 0.00 | +1:1 |
| `KXHIGHCHI` | KMDW | **1** | +0.04 | 0.00 | +0:1 |
| `KXHIGHLAX` | KLAX | **1** | +0.08 | 0.00 | +0:1 |
| `KXHIGHMIA` | KMIA | **1** | +0.02 | 0.00 | +0:1 |
| `KXHIGHPHIL` | KPHL | **1** | +0.04 | 0.00 | +0:1 |
| `KXLOWTCHI` | KMDW | **1** | +0.20 | 0.00 | +0:1 |
| `KXLOWTMIN` | KMSP | **1** | -0.06 | 0.00 | +0:1 |

The model currently ships `{+0: 0.60, +1: 0.40}` for dense stations and `{+0: 0.29, +1: 0.57, +2: 0.14}` for Central Park. Any station above with n in single digits cannot confirm or refute those, and the report says n so that a point estimate is never mistaken for evidence.


## The intraday CLI: when the settling value becomes readable

The NWS issues a CLI several times a day, each carrying the daily maximum so far from the station's 5-minute record. They are not corrections. Only the issuance after local midnight is complete - but the afternoon one usually already carries the final value, and these markets trade until 01:00 local.


| issuance (local) | already equals the final value | n |
|---|---|---|
| 07:00 | **0%** | 6 |
| 16:00 | **86%** | 28 |
| 17:00 | **79%** | 14 |
| 18:00 | **86%** | 7 |
| 19:00 | **100%** | 7 |
| 20:00 | **100%** | 1 |
| 01:00 next day | **100%** | 31 |
| 02:00 next day | **100%** | 12 |
| 03:00 next day | **100%** | 1 |
| 04:00 next day | **100%** | 7 |

An issuance that is not yet final can only be low, because a running maximum cannot fall. So the afternoon CLI is a hard floor on the settled value, and the only question left is how much further the day climbs.


## Days still running (recorded, not yet comparable)

7 station-days have a CLI value from an issuance before the local day ended. They are stored as provisional and excluded from every agreement and shift number above.


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
| `KXHIGHAUS` | 2026-09-11 | 102.2 | 277 | 103 | +0.8 | [102, 103] | yes |
| `KXHIGHCHI` | 2026-09-11 | 81.0 | 281 | 81 | +0.0 | [80, 81] | yes |
| `KXHIGHLAX` | 2026-09-11 | 84.9 | 305 | 85 | +0.1 | [84, 85] | yes |
| `KXHIGHMIA` | 2026-09-11 | 88.0 | 267 | 88 | +0.0 | [88, 89] | yes |
| `KXHIGHNY` | 2026-09-11 | 78.1 | 24 | 79 | +0.9 | [79, 80] | yes |
| `KXHIGHPHIL` | 2026-09-11 | 81.0 | 268 | 81 | +0.0 | [81, 82] | yes |
| `KXLOWTCHI` | 2026-09-11 | 60.8 | 281 | 61 | +0.2 | - | - |
| `KXLOWTMIN` | 2026-09-11 | 62.1 | 283 | 62 | -0.1 | - | - |
| `KXHIGHAUS` | 2026-09-12 | 100.4 | 238 | 101 | - | - | - |
| `KXHIGHCHI` | 2026-09-12 | 87.8 | 235 | 88 | - | - | - |
| `KXHIGHLAX` | 2026-09-12 | 82.4 | 211 | - | - | - | - |
| `KXHIGHMIA` | 2026-09-12 | 93.0 | 249 | 93 | - | - | - |
| `KXHIGHNY` | 2026-09-12 | 78.1 | 20 | 78 | - | - | - |
| `KXHIGHPHIL` | 2026-09-12 | 82.9 | 248 | 83 | - | - | - |
| `KXLOWTCHI` | 2026-09-12 | 68.0 | 235 | 68 | - | - | - |
| `KXLOWTMIN` | 2026-09-12 | 62.6 | 232 | 63 | - | - | - |
