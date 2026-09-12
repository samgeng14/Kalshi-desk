# Weekly Kalshi perp portfolio — v1

Project reset from single-signal alpha to a portfolio with positive net expected return and
controlled downside. This is the first pass: feasibility, baselines, and one book that survives
its gates. No fitted model yet, deliberately.

Venue: Kalshi, measured fee schedule. Rebalance: weekly. Both decided before any code, for
reasons in section 1.

## 1. The turnover budget forced the frequency

A 3-long/3-short cross-sectional book turns over about 1.51 in gross weight per rebalance —
measured on the panel, not assumed, because top-3/bottom-3 membership almost fully reshuffles.
At Kalshi's per-coin one-way costs (12.32 bp for BTC to 21.30 for LINK, of which 12.00 is the
tier-0 taker fee):

| rebalance | cost %/yr | gross return needed to break even |
|---|---:|---:|
| 4h | 562% | 562% |
| 8h | 280% | 280% |
| 12h | 187% | 187% |
| 24h | 93% | 93% |
| 48h | 46% | 46% |
| **168h (weekly)** | **13%** | **13%** |
| 336h | 6.5% | 6.5% |

No fee tier rescues the fast end. At $100m/30d volume with maker fills — 1.2 bp, the bottom of
the verified ladder — 12-hour rebalancing still costs **67.5% a year**, because the non-fee half
of the spread does not scale with the fee. The original 4h/8h/12h specification was not viable at
this venue at any volume tier, and weekly is the first frequency where a plausible gross return
clears its own costs.

## 2. Panel and construction

| | |
|---|---|
| Universe | 13 perps: AAVE ADA AVAX BCH BNB BTC DOGE ETH LINK LTC NEAR SOL XRP |
| Prices | Binance USD-M hourly klines, 2020-10-16 .. 2026-08-31 (51,376 bars) |
| PCA | trailing 720h window, 3 components, refit daily, warm-up dropped rather than zero-filled |
| Book | rank on score, 3 long / 3 short, equal weight, gross 1.0 |
| Neutralisation | PC1 loading projected out, gross restored |
| Rebalance | every 168h |
| Cost | charged on weight *changes*: `Σ |Δw_i| × oneway_i`, never per bar |
| Marking | hourly, so the bootstrap sees ~46,000 observations in ~2,000 day-clusters |

PC1 is 67% of variance, PC2 4.5%, PC3 4.2%. Residual standard deviation against the round trip it
costs to trade is 1.48× at 1h bars and 4.73× at 12h — another reason the slow end is where the
room is.

## 3. Baselines, before any model

| result | evidence |
|---|---|
| **Momentum yes, reversal no** | cross-sectional momentum at 336h/504h returns +24% to +30%/yr; every reversal variant loses, −10% to −36% |
| **PCA residual reversion fails** | −28.7%/yr, worse than the random control at −21.3% |
| **Residual momentum beats raw momentum** | +28.1% vs +25.8% at the same lookback, at lower volatility |
| **PC1-neutralisation helps, modestly** | +25.5% → +28.1%, Sharpe 0.89 → 1.00 |
| **Rank hysteresis hurts** | saves 2 points of cost, loses 8 to 20 points of return |
| **Vol targeting does not help here** | Sharpe 0.73 unscaled vs 0.70–0.71 at 15/25/35% targets, and Calmar drops from 0.65 to 0.45 |
| **Random control behaves** | −20%/yr against 13%/yr of costs, i.e. the cost model bites |

The reversal result agrees with the SSRN paper finding the classic PCA/OU stat-arb framework not
robust in crypto. Model 4 is not carried forward.

## 4. Selection destroyed the edge; compositing restored it

The baseline table had a sharp peak — residual momentum returns +5%/yr at a 168h lookback, +28%
at 336h, +30% at 504h, +1% at 1440h. Choosing that peak with hindsight is not a result, so the
lookback and the model were selected out of sample: every 26 weeks, pick the best candidate by
trailing Sharpe on everything seen so far, trade it for the next block, pool the blocks.

| on the same out-of-sample span | ann % | vol % | Sharpe | maxDD |
|---|---:|---:|---:|---:|
| walk-forward selector | **−3.2** | 23.6 | **−0.14** | 70.0 |
| oracle (best rule with hindsight) | +22.4 | 26.5 | 0.84 | 25.7 |
| best fixed rule held throughout | +22.4 | 26.5 | 0.84 | 25.7 |
| worst fixed rule held throughout | −6.2 | 23.9 | −0.26 | 63.9 |

The selector performed worse than most of the rules it was choosing among. It spent 2021 in the
worst candidate in the set and never recovered. Trailing Sharpe over a five-point grid is not
enough signal to choose on.

The standard answer is not to choose. Z-score each candidate's cross-sectional score and average.

## 5. The v1 book

Composite of all ten candidates — cross-sectional and residual momentum at 168/336/504/720/1440h
— equally weighted, no subsetting, no discretion. Out-of-sample span 2021-05 to 2026-08.

| variant | ann % | vol % | Sharpe | maxDD | Calmar | cost %/yr | net_lo | excess_lo |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| **ALL composite** | **18.7** | 25.7 | **0.73** | **28.7** | 0.65 | 8.8 | **+0.0096** | −0.0007 |
| @15% vol target | 11.7 | 16.4 | 0.71 | 25.9 | 0.45 | 6.9 | −0.0030 | −0.0078 |
| @25% vol target | 19.5 | 27.4 | 0.71 | 43.1 | 0.45 | 11.5 | −0.0049 | −0.0129 |
| @35% vol target | 27.0 | 38.3 | 0.70 | 60.8 | 0.44 | 16.0 | −0.0095 | −0.0213 |
| always long, equal weight | −18.5 | 77.0 | −0.24 | 182.6 | −0.10 | 0.0 | −0.8453 | — |

Year by year, against always-long on the same bars:

| year | ann % | vol % | Sharpe | maxDD % | always-long % |
|---|---:|---:|---:|---:|---:|
| 2021 | +30.6 | 36.8 | 0.83 | 23.8 | −35.5 |
| 2022 | +29.8 | 28.7 | 1.04 | 19.6 | −148.1 |
| 2023 | +16.7 | 23.8 | 0.70 | 28.7 | +84.2 |
| 2024 | +37.8 | 26.4 | 1.43 | 24.2 | +60.2 |
| 2025 | +7.7 | 19.1 | 0.40 | 20.3 | −45.3 |
| 2026 | **−18.0** | 16.9 | −1.07 | 22.2 | −43.3 |

Positive in 5 of 6 years. Over the span the book returned +18.7% a year while the market it
trades lost 18.5% a year with a 182.6% drawdown.

### Where it stands against the gates

`net_lo > 0`: **passes**, at +0.0096 bp/hour. Barely, but the net return is distinguishable from
zero on 2,000 day-clusters.

`excess_lo > 0` against always-long **scaled to the book's own volatility**: **fails at −0.0007**,
which is as close to exactly zero as a number gets. The point estimate of that excess is about
+25% a year and the t-statistic is roughly 1.9. Borderline, and borderline is not passed.

The vol match matters and is worth stating plainly: the benchmark runs at 77% volatility and a
183% drawdown, and comparing raw mean returns would ask a 26%-volatility book to beat a position
carrying three times its risk. Scaling the benchmark asks the question worth asking, and on that
question the book is sitting exactly on the line.

## 6. What is hindsight and what is not

Compositing was chosen after watching the selector fail. "Average rather than pick" is
parameter-free and was written down before it was run, so the ALL composite carries no
within-family discretion. The *subsets* do, and are reported as description only:

| subset | ann % | Sharpe | maxDD | net_lo | excess_lo |
|---|---:|---:|---:|---:|---:|
| slow only (336/504/720) | 18.5 | 0.74 | 29.0 | +0.0068 | −0.0017 |
| slow residual only | 21.9 | 0.94 | 42.7 | +0.0708 | **+0.0572** |
| slow cross-sectional only | 21.3 | 0.79 | 25.4 | +0.0233 | **+0.0098** |

Two of the three pass both gates. Dropping lookbacks shorter than the holding period is arguably
a principled restriction rather than a fitted one, but it was arrived at by looking at results,
and calling it principled afterwards is the move this project has already been burned by twice.
It stays in the description column until it is tested on data that did not suggest it.

## 7. What is measured and what is assumed

**Measured:** the price series, the turnover, the per-coin Kalshi one-way costs, the PCA
structure, every return and drawdown above.

**Assumed, and each one is a real risk:**

1. **Signal on Binance prices, execution on Kalshi perps.** The desk measured Kalshi tracking at
   β = +1.005, R² = 0.997 against an external composite at 1-minute horizon, but weekly basis
   drift is unmeasured. This is the single largest untested assumption in the book.
2. **Fills at the measured one-way cost.** Fine for a small order rebalancing weekly; it is not a
   capacity model.
3. **Six names at a time out of thirteen.** Concentrated by construction.
4. **No funding.** Kalshi funding read 0.0000 for 14 of 18 perps on one day of tape, which is why
   the carry model is deferred rather than dropped.
5. **2026 is the worst year and it is the current one.** −18% through August. A strategy whose
   drawdown is happening now is not the same object as one whose drawdown was in 2022.

## 8. Next, in order

1. **Kalshi tracking error over a week.** Measurable from the recorder tape. This converts the
   largest assumption into a number, and it gates everything downstream.
2. **Selection null on the composite** — how often does a sweep of this shape produce a Sharpe of
   0.73 when nothing is there.
3. **Funding accumulation.** `scripts/kalshi_funding_accumulate.py` appends a daily cross-sectional
   snapshot; the carry model waits on a few weeks of it.
4. **Risk engine.** Naive vol targeting measurably did not help, so the next candidates are
   drawdown scaling and position caps, each tested rather than assumed.
5. **Only then a fitted model.** Ridge over the feature set, and only if it beats this composite
   out of sample. The complexity rule stands.

## Reproduce

```
python3 scripts/portfolio_feasibility.py
python3 scripts/portfolio_baselines.py
python3 scripts/portfolio_select.py
python3 scripts/portfolio_v1.py
```
