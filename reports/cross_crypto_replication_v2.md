# Cross-crypto return predictability — replication, second pass

This supersedes `reports/cross_crypto_replication.md`, which is withdrawn at the top of its own
file. That version had a cost model that charged a full Kalshi round trip on every bar while the
position only changed on about half of them, tested on 80 days in which all fourteen assets rose,
and reported neither the bootstrap intervals nor a drift control. Every number here is recomputed.

Papers: Jia, Wu, Yan & Liu, *A seesaw effect in the cryptocurrency market*, J Empirical Finance 74
(2023); Guo, Sang, Tu & Wang, *Cross-cryptocurrency return predictability*, JEDC 163 (2024).
Abstracts and the pre-registered transfer problem: `research/literature/cross_crypto_replication.md`.

## What changed, and why each change mattered

| defect in v1 | fix | effect on the answer |
|---|---|---|
| Full round trip charged on every bar; position flips on ~50% | cost = `turnover × round_trip + round_trip/n` | hurdle was ~1.9× too harsh; specs clearing 1.0x went 6 → 18 of 210 |
| 80-day test slice, 2026-06-23..09-10 | 5.9 years, 2020-10-16..2026-08-31, hourly klines | the 80-day winners (SOL 3.31x, AAVE 3.53x) fall to 0.39x and 0.52x |
| One chronological split — one regime | 8-fold expanding-window walk-forward, pooled | out-of-sample now spans the 2022 drawdown, not only a rally |
| No drift control | `excess_vs_constant_bps` vs the better of always-long/always-short | in v1's window every asset rose, so long-biased rules looked good for free |
| Bootstrap intervals computed but never reported | `net_lo` is now a gate | in v1, zero of 252 specs passed it — the simplest reason to close, unreported |
| Null rolled the whole panel, then the target | rolls the target only | each draw had silently evaluated a different calendar window, widening the null |
| 1,200 bootstrap resamples on 80 clusters | 8,000 when clusters < 400, 2,500 above | at 1,200 a gate passed that fails at 8,000 — decided by Monte Carlo noise |
| "Inside the null" treated as absence | separate power measurement, `cross_crypto_power.py` | a specificity result was being read as a power result |

## Sample and construction

| | |
|---|---|
| Source | Binance USD-M monthly 1h klines, `data.binance.vision` |
| Assets | 13 perps: AAVE ADA AVAX BCH BNB BTC DOGE ETH LINK LTC NEAR SOL XRP |
| Excluded | SUI — listed 2023-05; keeping it would cut the common history from 5.9 years to 3.3 |
| Full sample | 2020-10-16 .. 2026-08-31 (2,140 days) |
| Bars | 60m (51,376), 240m (12,843), 720m (4,280) |
| Evaluation | 8 expanding folds; fold *i* trains on blocks 0..*i*−1, validates on *i*, tests on *i*+1; all test blocks pooled |
| Pooled out-of-sample | **2021-12 .. 2026-08**, 3,423 bars at 720m, **1,712 day-clusters** |
| Predictors | lagged returns of all 13 assets, 1 lag |
| Models | own momentum, BTC lagged, equal-weight basket, leader-only (BTC+ETH), seesaw sign, walk-forward LASSO |
| Cost | `turnover × (2 × measured Kalshi one-way for the coin traded) + amortised open/close` |
| Specifications | 13 × 6 × 3 = **234** (of which 39 are the exact negation of another 39 — see below) |

Three gates, fixed before looking: `economic_ratio > 1`, `net_lo > 0` (block-bootstrap 90% lower
bound on the cost-adjusted return), `excess_lo > 0` (beats the better constant position).

## Result

| bar | specs | best ratio | mean | median | frac positive | ratio > 1 | and net_lo > 0 |
|---|---:|---:|---:|---:|---:|---:|---:|
| 60m | 78 | 0.0701 | −0.0238 | −0.0248 | 0.269 | 0 | 0 |
| 240m | 78 | 0.2214 | −0.0720 | −0.0791 | 0.256 | 0 | 0 |
| 720m | 78 | **1.0441** | +0.0672 | +0.0099 | 0.526 | 1 | **0** |

**One specification of 234 clears the cost hurdle on the point estimate. None clears the
bootstrap gate.**

### The published mechanisms, over 5.9 years

| model class | specs | mean ratio | best | frac positive |
|---|---:|---:|---:|---:|
| own momentum | 39 | +0.0402 | **1.0441** | 0.308 |
| LASSO (walk-forward) | 39 | +0.0183 | 0.6372 | 0.487 |
| equal-weight basket | 39 | +0.0137 | 0.4630 | 0.333 |
| leader-only (BTC+ETH) | 39 | −0.0629 | 0.4577 | 0.154 |
| BTC lagged return | 39 | −0.0527 | 0.4207 | 0.154 |
| seesaw sign (minus basket) | 39 | −0.0137 | 0.3569 | 0.667 |

Every cross-asset rule tops out below 0.64x — the LASSO, which is both papers' method, reaches
0.637x, and the seesaw direction reaches 0.357x. **These are point estimates, not detection
failures: a ratio of 0.64 means the best cross-asset specification in 5.9 years returns 64% of
what it costs to trade.** The only thing that clears is own momentum, which contains no
cross-crypto information at all.

Two honest notes on that table. `seesaw sign (minus basket)` is exactly `−1 ×`
`equal-weight basket` — verified, max |sum| over the 39 pairs is 0.000000000000 — so its rows
carry no independent information, the effective specification count is 195 rather than 234, and
its 0.667 fraction-positive is the mirror of the basket's 0.333 rather than a finding. And the
LASSO's fraction-positive of 0.487 is a coin flip, which is what a fitted model with nothing to
find looks like.

## The one that clears, and what it is

**XRPUSDT own momentum at 720-minute bars.** 3,423 pooled out-of-sample bars, 1,712 day-clusters,
2021-12 to 2026-08.

| | |
|---|---:|
| IC | +0.0412 |
| gross | **+15.00 bp/bar**, 90% CI **[+6.91, +23.03]** |
| turnover | 0.494 |
| taker trade cost | 14.37 bp/bar (0.494 × 29.08 bp round trip) |
| net | **+0.63 bp/bar**, `net_lo` **−7.46** |
| economic ratio | 1.044 (0.516 under the every-bar cost) |
| always-long on the same slice | +1.46 bp/bar |
| excess over the better constant | **+13.54**, 90% CI **[+3.30, +24.17]** |

Read the last two rows before anything else. The gross return's interval excludes zero and the
excess over always-long excludes zero, on 1,712 day-clusters spanning a full cycle. **The signal
is real.** What fails is the arithmetic against the spread: 15.00 bp of gross against 14.37 bp of
cost leaves 0.63, and the interval on that straddles zero by a wide margin.

### It is stable gross and unstable net

| fold | window | bars | gross bp | net bp | turnover | hit rate |
|---|---|---:|---:|---:|---:|---:|
| 1 | 2021-12..2022-07 | 428 | +9.97 | −4.40 | 0.506 | 0.491 |
| 2 | 2022-07..2023-02 | 428 | +12.46 | −1.91 | 0.464 | 0.526 |
| 3 | 2023-02..2023-09 | 428 | +9.18 | −5.19 | 0.501 | 0.484 |
| 4 | 2023-09..2024-04 | 427 | +21.68 | **+7.31** | 0.491 | 0.511 |
| 5 | 2024-04..2024-11 | 428 | +28.71 | **+14.34** | 0.494 | 0.500 |
| 6 | 2024-11..2025-06 | 428 | +12.92 | −1.45 | 0.518 | 0.481 |
| 7 | 2025-06..2026-01 | 428 | +10.22 | −4.15 | 0.499 | 0.502 |
| 8 | 2026-01..2026-08 | 428 | +14.92 | +0.55 | 0.480 | 0.509 |

Gross is positive in **8 of 8 folds** and in **6 of 6 calendar years**, including 2022, when XRP
itself returned −12.49 bp/bar. Net of the taker round trip it is positive in **3 of 8**, and the
pooled +0.63 is carried almost entirely by 2024 (+14.37 bp/bar net against −3.20, −6.32 and −5.07
in 2022, 2023 and 2025).

Note the hit rates: 0.481 to 0.526, every fold within noise of a coin flip. **The edge is in
magnitude, not direction** — the rule is right about how far, not about which way. That is a
fragility worth stating: a magnitude edge depends on a handful of large bars behaving, and is
more exposed to a single tail event than a directional one.

## The selection null, and what it does and does not say

300 circular shifts of the target only; predictors, test window and bootstrap clusters identical
in every draw.

| statistic | observed | null p50 | p90 | p95 | p99 | max | p (right) |
|---|---:|---:|---:|---:|---:|---:|---:|
| max economic ratio | 1.0441 | 0.7903 | 1.1965 | 1.2974 | 1.5060 | 1.7376 | 0.217 |
| mean across specs | −0.0151 | 0.0046 | 0.0719 | 0.0868 | 0.1173 | 0.1565 | 0.627 |
| median across specs | −0.0334 | 0.0006 | 0.0193 | 0.0261 | 0.0350 | 0.0592 | 0.960 |
| count clearing 1.0x | 1 | 0 | 2 | 3 | 6 | 12 | 0.243 |
| fraction positive | 0.3231 | 0.5026 | 0.6364 | 0.6718 | 0.7129 | 0.7231 | 0.943 |
| count with net_lo > 0 | 0 | 0 | 0 | 0 | 0 | **0** | 1.000 |
| count passing all three gates | 0 | 0 | 0 | 0 | 0 | **0** | 1.000 |

The observed maximum of 1.0441 sits at the 78th percentile of the null — a misaligned sweep of
this size produces a best-of-195 above 1.0 about a quarter of the time. Nothing here is unusual.

**The last two rows are a specificity result, not a power result, and the distinction matters.**
In 300 draws — 58,500 specification-draws — the null never once produced a specification with a
bootstrap lower bound above its cost. That says the gate does not fire on noise. It says nothing
whatever about whether the gate would fire on a real effect. Reading it as "the gate has no
power" (which an earlier version of the script printed) or as "the mechanism is absent" are both
wrong. Power has to be measured separately.

## How large an edge would this test have caught?

`scripts/cross_crypto_power.py` injects an edge of known size on top of the real XRP return
series — the real noise, autocorrelation and tails all still present — and finds where the gates
start firing.

| injected edge | gross bp/bar | ratio | net_lo | gates |
|---|---:|---:|---:|---|
| none (observed) | 15.00 | 1.044 | −7.46 | — |
| 0.25 × cost | 18.58 | 1.293 | −3.89 | — |
| 0.50 × cost | 22.15 | 1.541 | −0.33 | — |
| **0.75 × cost** | 25.72 | **1.790** | **+3.24** | **all three** |
| 1.00 × cost | 29.29 | 2.038 | +6.81 | all three |

Closed form: `net_lo > 0` needs gross above `cost + half-width` = 14.37 + 8.06 = **22.43 bp/bar**,
an economic ratio of **1.56**. Observed is 1.044.

So the honest statement of what was and was not measured:

- **For the cross-asset mechanisms this is decisive.** Their best point estimates are 0.36x to
  0.64x of cost. They are not undetected; they lose money on the point estimate, over 5.9 years,
  walk-forward, in a window containing a full drawdown.
- **For the marginal case it is not.** The test can see an edge netting more than **8.06 bp/bar**
  and cannot see one below that. At 12-hour bars, 8.06 bp/bar is on the order of **59% a year**
  before compounding. An edge of a third that size would be worth having and would be invisible
  here. XRP own momentum nets +0.63 bp/bar, so it is not merely undetected — but its interval is
  wide enough that this sample cannot distinguish "slightly profitable" from "slightly
  unprofitable," and a longer sample would not fix that, because the gap is a cost gap, not a
  sample-size gap.

## Where this actually lands: an execution question, not a signal question

The cost charged above is the taker round trip, 29.08 bp for XRP, of which **24.00 bp is the
exchange fee** — this desk's own measurement puts XRP's non-fee cost of crossing at 2.54 bp one
way. The signal is not being beaten by the market. It is being beaten by the fee schedule.

Charging the desk's measured *passive* economics for XRP instead — maker fee 5.0 bp against a
measured `kept` of −0.41 bp, so 5.41 bp effective one way, 10.82 bp round trip:

| execution | cost bp/bar | net bp/bar | net_lo | ratio |
|---|---:|---:|---:|---:|
| taker / taker | 14.37 | +0.63 | −7.46 | 1.044 |
| maker / maker | 5.35 | **+9.66** | **+1.57** | 2.806 |

**Do not read the second row as a result.** It assumes every intended entry gets a passive fill,
and that assumption is exactly what this project already rejected once. The −0.41 `kept` is
measured adverse selection *conditional on having filled*; it says nothing about selection in
*which* entries fill at all, and a resting order fills when the market comes to you, which is
when your signal is going the wrong way. A 12-hour signal is patient enough to rest for minutes,
which the rejected construction was not — but "different enough to be worth measuring" is the
whole claim, not "it works."

That measurement is runnable with machinery this desk already has: `scripts/execution_sim.py` on
recorded KXXRPPERP tape, entries placed at the 12-hour signal times, fill rates and realised entry
prices measured rather than assumed. It needs days of tape rather than hours, and the recorder is
running.

## Verdict, stated narrowly

> **Cross-crypto lead–lag predictability does not produce an executable post-cost edge in this
> construction: 13 large-cap perps, one lag, 60/240/720-minute bars, 5.9 years, 8-fold
> walk-forward with the out-of-sample window spanning 2021-12 to 2026-08, priced at Kalshi's
> measured taker round trip. Every cross-asset rule, the LASSO included, tops out below 0.64x of
> its cost hurdle — a losing point estimate, not an undetected one.**

Unlike v1, this is a powered test rather than an underpowered one, and the power is quantified
rather than assumed: 1,712 day-clusters, an out-of-sample window containing a full drawdown, a
drift control the rules have to beat, and a stated minimum detectable effect of 8.06 bp/bar.

It is still not a refutation of either paper. What differs from what they published:

- **Lag structure.** One lag. Guo et al. use adaptive LASSO and PCA; the exact lag sets in each
  paper were not read, so this difference is assumed rather than established.
- **Portfolio form.** Both papers form long-short portfolios across the cross-section. This is a
  directional per-asset test, which is the form that maps onto a Kalshi position.
- **Universe.** The seesaw direction is large-predicts-small, and the smallest asset here is a
  top-50 coin because that is what Kalshi lists. The pre-registered transfer problem is still
  unresolved rather than resolved — a genuinely small-coin test is not runnable on this venue.
- **Costs.** Kalshi's fee schedule, not the papers' venues. Guo et al. use Binance data, the same
  price source used here, so the difference is the cost schedule applied, not the prices.

## What is open

**Own momentum at 12-hour bars on XRP is gross-positive in 8 of 8 folds and 6 of 6 years across a
full cycle, beats always-long with a lower bound above zero, and misses on execution cost alone.**
That is not a cross-crypto result and it does not belong to this lane. It belongs to the execution
lane, and the question it poses is narrow and answerable: what fraction of those entries can
actually be filled passively, and at what realised price.

## Reproduce

```
python3 scripts/binance_monthly_klines.py --assets AAVEUSDT ADAUSDT AVAXUSDT BCHUSDT BNBUSDT BTCUSDT DOGEUSDT ETHUSDT LINKUSDT LTCUSDT NEARUSDT SOLUSDT SUIUSDT XRPUSDT --start 2020-01 --end 2026-09
python3 scripts/cross_crypto.py --sub klines_1h --drop SUIUSDT --folds 8 --bar 720 --tag _wf
CC_SUB=klines_1h CC_DROP=SUIUSDT CC_FOLDS=8 python3 scripts/cross_crypto_close.py
python3 scripts/cross_crypto_power.py
```

Raw output: `reports/cross_crypto_close_walkforward.log`, `reports/cross_crypto_power.log`.
Per-specification detail: `reports/cross_crypto_{60,240,720}m_wf.csv`.
