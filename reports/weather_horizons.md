# Which weather holding horizon actually has edge

Generated 2026-09-12 22:31:29Z. Every number is executable: entry crosses the spread, exit sells into the bid that existed at the horizon, and Kalshi's quadratic fee is charged on each leg. Holding to settlement pays the entry fee only.

17746 candidate states from 243 contracts over 46 event-days, sampled no more often than every 15 minutes per contract.


## The headline: mean executable P&L per contract, by horizon

Only states the desk would have called a signal (terminal edge >= 3c after fee).


| horizon | n | mean | median | win rate | 95%% CI (event-clustered) |
|---|---|---|---|---|---|
| 5m | 4554 | **-3.61c** | -3.37c | 1% | [-3.81, -3.39] |
| 10m | 4391 | **-3.64c** | -3.37c | 2% | [-3.87, -3.39] |
| 15m | 4169 | **-3.58c** | -3.34c | 3% | [-3.82, -3.31] |
| 30m | 3839 | **-3.61c** | -3.28c | 6% | [-3.86, -3.33] |
| 1h | 3414 | **-3.69c** | -3.19c | 8% | [-4.08, -3.27] |
| 2h | 3139 | **-3.20c** | -2.91c | 13% | [-3.88, -2.33] |
| 4h | 2537 | **-2.96c** | -3.19c | 19% | [-4.11, -1.53] |
| **settlement** | 4477 | **-0.77c** | -5.33c | 27% | [-8.12, +7.57] |

## Does more predicted edge produce more realized profit? (section 16)


| predicted edge | n | 15m | 1h | settlement |
|---|---|---|---|---|
| 3-4c | 533 | -3.57c | -3.67c | +7.36c |
| 4-5c | 540 | -3.12c | -3.52c | +1.80c |
| 5-7c | 819 | -3.63c | -3.71c | +3.56c |
| 7-10c | 1044 | -3.33c | -3.34c | -1.12c |
| 10-100c | 2009 | -3.82c | -3.91c | -5.27c |

## The 80c -> 90c question (section 32)

Entries taken between 78c and 82c where the desk fair said 90%% or better. `touch` means the executable bid actually reached that level, not the mid.


**No such state occurred in the recorded window.** Across 17746 candidate states over 46 event-days, the desk fair never reached 90% on a contract whose executable entry was near 80c. The trade this study was built to validate did not arise, so it cannot yet be made a rule.


The closest any state came: `KXHIGHMIA-26SEP07-T93` NO at 90c with a desk fair of 94.4%.


## Entry-price bands (section 32)


| entry | n | 15m | 1h | settlement | settled win rate |
|---|---|---|---|---|---|
| 5-30c | 3903 | -3.30c | -3.18c | +0.39c | 15% |
| 30-60c | 3760 | -5.30c | -5.23c | -6.87c | 40% |
| 60-70c | 1311 | -5.00c | -4.80c | +7.57c | 73% |
| 70-80c | 964 | -4.96c | -5.32c | -0.88c | 75% |
| 80-90c | 1031 | -3.85c | -3.90c | -5.92c | 80% |
| 90-96c | 1517 | -2.68c | -2.80c | -4.92c | 88% |

## Coverage


| horizon | states with no exitable bid at the horizon |
|---|---|
| 5m | 1449 of 17746 (8%) |
| 10m | 1926 of 17746 (11%) |
| 15m | 2763 of 17746 (16%) |
| 30m | 3924 of 17746 (22%) |
| 1h | 5315 of 17746 (30%) |
| 2h | 6237 of 17746 (35%) |
| 4h | 8117 of 17746 (46%) |

A missing horizon is a contract that either had no bid to sell into or ran past its close. Both are real constraints on a short-horizon sleeve, not gaps in the data.
