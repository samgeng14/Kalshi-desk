# Weather capacity, and the smallest edge that is real

Generated 2026-09-12 22:43:00Z. Settlement entries only: held through resolution, so the entry fee is paid once and there is no exit transaction. Depth is recorded top of book; the full ladder was never stored, so these sizes are a floor on what was available.

8549 candidate states, 179 contracts, **34 settled city-days**, 6 distinct dates.


## Section 25: the minimum detectable edge


| | value |
|---|---|
| contracts, one directional observation each | 179 |
| per-contract standard deviation of settlement P&L | 29.4c |
| independent clusters (settled city-days) | **34** |
| standard error of the mean, city-day clustered | 2.35c |
| **minimum detectable edge (95%, 50% power)** | **4.6c** |

That is the smallest per-contract edge this desk can currently tell apart from zero. It is a property of the binary payoff and the number of independent weather days, not of the model.


### How many settled city-days each floor would need

Standard error falls as the square root of the number of independent clusters, so the requirement is `clusters_now x (MDE_now / target)^2`. At roughly 18 cities a day, every city-day is one cluster - but cities on the same date share a weather pattern, so this column is optimistic.


| target edge | city-days needed | days at 18 cities/day |
|---|---|---|
| 0.2c | 18,041 | **1,002** |
| 0.5c | 2,887 | **160** |
| 1.0c | 722 | **40** |
| 2.0c | 180 | **10** |
| 3.0c | 80 | **4** |
| 5.0c | 29 | **2** |


## Section 24: the edge-floor race

`size` is min(recorded top-of-book depth, 500). `expected $` sums net edge x size; `realized $` sums the settlement P&L those same contracts actually earned. A floor is only meaningful if it sits above the minimum detectable edge above.


### maker entry

**Conditional on the order filling, which is not modelled here.** Section 11 of the brief is explicit that quoted spread must not be credited mechanically. A resting bid fills when someone crosses it, and the times that happens are disproportionately the times the crosser knows something - so the realised column below is an upper bound that a fill model will lower, by an amount nobody has measured yet.

| floor | signals | contracts | capital | expected $ | **realized $** | realized per $ |
|---|---|---|---|---|---|---|
| 0.2c | 2690 | 168,565 | $49,728 | $16,463 | **$6,600** | +0.133 |
| 0.3c | 2667 | 166,151 | $47,896 | $16,457 | **$6,278** | +0.131 |
| 0.5c | 2619 | 162,848 | $45,447 | $16,444 | **$6,421** | +0.141 |
| 0.8c | 2553 | 158,562 | $43,084 | $16,419 | **$5,564** | +0.129 |
| 1.0c | 2468 | 153,486 | $40,448 | $16,373 | **$4,704** | +0.116 |
| 2.0c | 2232 | 140,818 | $34,694 | $16,186 | **$3,477** | +0.100 |
| 3.0c | 2031 | 131,376 | $31,128 | $15,945 | **$3,636** | +0.117 |
| 5.0c | 1643 | 108,423 | $23,843 | $15,042 | **$2,472** | +0.104 |

### taker entry

| floor | signals | contracts | capital | expected $ | **realized $** | realized per $ |
|---|---|---|---|---|---|---|
| 0.2c | 2247 | 176,448 | $33,410 | $14,077 | **$-3,855** | -0.115 |
| 0.3c | 2231 | 175,449 | $33,197 | $14,074 | **$-4,112** | -0.124 |
| 0.5c | 2193 | 172,952 | $32,229 | $14,065 | **$-3,991** | -0.124 |
| 0.8c | 2132 | 169,912 | $30,933 | $14,046 | **$-4,207** | -0.136 |
| 1.0c | 2079 | 167,579 | $30,200 | $14,025 | **$-4,235** | -0.140 |
| 2.0c | 1847 | 152,084 | $25,999 | $13,792 | **$-3,674** | -0.141 |
| 3.0c | 1667 | 142,978 | $23,236 | $13,564 | **$-3,879** | -0.167 |
| 5.0c | 1287 | 108,921 | $18,288 | $12,194 | **$-4,005** | -0.219 |

### What lowering the floor actually buys

The question volume mode asks is whether accepting smaller edges produces more total dollars. Marginal expected value of each step down, maker entry:


| floor | signals | expected $ | gained by lowering to here |
|---|---|---|---|
| 5.0c | 1643 | $15,042 | - |
| 3.0c | 2031 | $15,945 | +$904 for +388 signals |
| 2.0c | 2232 | $16,186 | +$241 for +201 signals |
| 1.0c | 2468 | $16,373 | +$187 for +236 signals |
| 0.8c | 2553 | $16,419 | +$45 for +85 signals |
| 0.5c | 2619 | $16,444 | +$25 for +66 signals |
| 0.3c | 2667 | $16,457 | +$13 for +48 signals |
| 0.2c | 2690 | $16,463 | +$6 for +23 signals |


These are every candidate state, not a portfolio: the same contract appears at many timestamps and both sides appear separately, so `contracts` and `capital` are an upper bound on opportunity rather than a tradeable plan.


## Section 19: where the candidates actually sit


| net edge band | maker signals | taker signals | median depth |
|---|---|---|---|
| negative | 1522 | 1963 | 50 |
| 0.0-0.2c | 63 | 64 | 33 |
| 0.2-0.5c | 71 | 54 | 25 |
| 0.5-1.0c | 151 | 114 | 30 |
| 1.0-2.0c | 236 | 232 | 29 |
| 2.0-5.0c | 589 | 560 | 29 |
| 5c+ | 1643 | 1287 | 28 |
