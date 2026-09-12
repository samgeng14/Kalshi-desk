# Cross-crypto return predictability — replication result

**Status: `FAILED_SELECTION`.** The one specification that cleared the cost hurdle is inside the
null distribution of the maximum over a sweep of the same size. The lane closes on multiple
testing, not on a measured negative effect.

Papers: Jia, Wu, Yan & Liu, *A seesaw effect in the cryptocurrency market*, J Empirical Finance
74 (2023); Guo, Sang, Tu & Wang, *Cross-cryptocurrency return predictability*, JEDC 163 (2024).
Both abstracts verified verbatim and recorded in `research/literature/cross_crypto_replication.md`,
along with the transfer problem stated before any code was written.

## Sample and construction

| | |
|---|---|
| Source | Binance USD-M futures 1m klines, `data.binance.vision` |
| Assets | 14 perps: AAVE ADA AVAX BCH BNB BTC DOGE ETH LINK LTC NEAR SOL SUI XRP |
| Full sample | 2025-08-07 12:00 .. 2026-09-10 12:00 UTC (400 days) |
| Bar lengths | 60m (9,599 bars), 240m (2,399), 720m (799) |
| Split | chronological 60% train / 20% validate / 20% test; **test 2026-06-23 .. 2026-09-10** |
| Predictors | lagged returns of all 14 assets (the papers' predictor set), 1 lag |
| Models | own momentum, BTC lagged, equal-weight basket, leader-only (BTC+ETH), seesaw sign (minus basket), LASSO |
| Cost hurdle | 2 × the measured Kalshi one-way cost **for the coin being traded**, set before any fitting |
| Specifications | 14 assets × 5 no-model rules × 3 bar lengths = **210** |

`economic_ratio = gross bps per trade / round-trip cost`, signed. Above 1.0 means the held-out
gross return covers the Kalshi round trip.

## The headline number, and what happened to it

The sweep's best specification was **SOLUSDT own momentum at 720m, 1.5424x** — +43.56 bp gross
per trade against a 28.24 bp round trip, the first thing in this project to clear a cost hurdle.
It is also the maximum of 210 correlated tries on a 160-bar test slice.

**The null.** Circularly shift the target returns relative to the predictors by a random offset,
the *same* offset for every asset. That preserves each asset's own autocorrelation and the entire
cross-sectional correlation structure — the two things that make a naive permutation test far too
easy — while destroying the alignment between predictor and target. Then run the identical sweep
and record its statistics. 300 shifts, offsets drawn from 80 to 700 bars.

| statistic | observed | null p50 | null p90 | null p95 | null p99 | null max | p |
|---|---:|---:|---:|---:|---:|---:|---:|
| **max** economic ratio | 1.5424 | 1.0854 | 1.9354 | 2.0430 | 2.8229 | 2.9346 | **0.167** |
| mean across specs | −0.0376 | −0.0026 | 0.1343 | 0.1717 | 0.2379 | 0.2961 | 0.630 |
| median across specs | −0.0528 | −0.0001 | 0.0353 | 0.0451 | 0.0742 | 0.0900 | 0.943 |
| count clearing 1.0x | 6 | 1 | 12 | 26 | 41 | 46 | 0.237 |
| fraction with positive gross | 0.3238 | 0.4976 | 0.6286 | 0.6762 | 0.7098 | 0.7714 | 0.923 |

A random misalignment of this panel produces a best-of-210 above 1.0x **more than half the time**.
1.5424x is the 83rd percentile of that distribution. It is not remarkable.

### Why the max test alone was not enough

A maximum-only test is weak against a *diffuse* effect: a real mechanism that lifts every
specification a little need not produce a standout maximum, and closing the lane on the max alone
would repeat, one level up, the error of closing it on a single specification. So the same null
was applied to four aggregate statistics.

Every one of them is at or below its null median. The most direct is the last row: **32.4% of the
210 specifications have a positive gross return on real data, against 49.8% under a random
misalignment.** Breaking the link between predictor and target *improves* the typical
specification. There is no diffuse effect hiding under the maximum; if anything these
one-lag rules are mildly counterproductive at these horizons.

The `count clearing 1.0x` row is worth reading carefully rather than as a near-miss. Observed 6
against a null median of 1 looks favourable until the tail is read: null p90 is 12 and the null
maximum is 46. When a shift happens to produce a spurious alignment, dozens of correlated
specifications clear at once. Six is ordinary.

## Sign stability of the winning specification

The gate required a stable sign across periods. Splitting the 160-bar held-out slice into
consecutive thirds, for SOL own momentum at 720m against its 28.24 bp round trip:

| third | bars | gross bp | net bp | hit rate |
|---|---:|---:|---:|---:|
| 1 | 53 | 39.87 | +11.63 | 0.434 |
| 2 | 54 | 6.61 | −21.63 | 0.481 |
| 3 | 53 | 84.89 | +56.65 | 0.660 |

The crude sign test passes — all three thirds are positive gross. **The decomposition fails.**
Two of the three thirds have a hit rate *below* 50%: the signal points the wrong way more often
than not, and their positive gross comes from a handful of large winners rather than from being
right. The middle third loses 21.63 bp net. Essentially the entire result is the final 53 bars,
where the hit rate jumps to 0.660.

That is the tail-concentration failure mode named in the quarter-hour write-up, now observed
directly. A 12-hour momentum signal that works in one 26-day window and is worse than a coin flip
in the other two is a period, not an edge.

## The published mechanisms cleared nothing, before the null was applied

Set the selection question aside and just read the leaderboard by model class. Six specifications
out of 210 clear 1.0x, all at 720m:

| rank | asset | model | gross bp | round trip | net bp | ratio |
|---|---|---|---:|---:|---:|---:|
| 1 | SOL | own momentum | 43.56 | 28.24 | +15.32 | 1.5424 |
| 2 | AAVE | own momentum | 58.24 | 38.56 | +19.68 | 1.5104 |
| 3 | XRP | own momentum | 34.14 | 29.08 | +5.06 | 1.1740 |
| 4 | ETH | equal-weight basket | 28.12 | 25.00 | +3.12 | 1.1250 |
| 5 | XRP | equal-weight basket | 32.06 | 29.08 | +2.98 | 1.1024 |
| 6 | AAVE | equal-weight basket | 40.42 | 38.56 | +1.86 | 1.0483 |

Three are **own momentum** — a single-asset rule with no cross-crypto content at all. Three are
the **equal-weight basket**, which is a market factor rather than a lead–lag effect.

Now the rules that encode what the papers actually claim:

| model class | specs | mean ratio | best | clear 1.0x |
|---|---:|---:|---:|---:|
| LASSO over all 14 assets' lags (both papers' method) | 42 | −0.1187 | 0.4915 | **0** |
| `seesaw sign (minus basket)` (Jia et al.'s stated direction) | 42 | −0.0840 | 0.7888 | **0** |
| all cross-asset rules together | 168 | — | 1.1250 | 3 |
| own momentum | 42 | — | 1.5424 | 3 |

**The LASSO is the worst model class in the sweep** — mean ratio −0.1187 against −0.0376 for the
field, only 21.4% of its specifications positive, and its single best result reaching 49% of the
required effect. The seesaw rule clears zero times out of 42 and its best result reaches 79%.

So the leaderboard never contained a cross-crypto replication. It contained a single-asset
momentum rule and a market-factor rule sitting above the cross-asset rules, and then the null
established that even those two are ordinary. The failure is not that the effect was too small
after Kalshi costs; it is that the mechanism did not appear.

## Verdict, stated narrowly

> **No evidence that cross-crypto lead–lag predictability yields an executable post-cost edge in
> this construction: 14 large-cap perps, one lag, 60/240/720-minute bars, a 400-day sample with a
> 160-bar held-out slice, priced at Kalshi's measured round trip.**

This is not "cross-crypto predictability does not exist," and it is not a refutation of either
paper. What was tested here differs from what they published in ways that matter:

- **Lag structure.** One lag. Guo et al. use adaptive LASSO and PCA over richer lag sets; the
  seesaw paper's LASSO is fit per focal coin with a longer predictor history.
- **Frequency.** Both papers' primary results are intraday-to-daily on their own venues. 60/240/720m
  bars overlap that range but match neither exactly.
- **Universe.** The seesaw effect says *large coins predict small coins*. The smallest asset here is
  still a top-50 coin, because that is the universe Kalshi lists. The coins where the published
  effect should be strongest are not tradable on the target venue at all — the transfer problem
  written down before the code, now unresolved rather than resolved.
- **Venue.** The signal is measured on Binance and charged Kalshi's costs. That is the correct
  hurdle for the transfer question, but it is not a Kalshi backtest and no Kalshi order was simulated.

A replication with a proper lag structure on daily bars over a multi-year sample is a different
experiment and remains open. It is not the next thing to run, because the transfer problem above
would still be waiting at the end of it.

## Reproduce

```
python3 scripts/cross_crypto.py
python3 scripts/cross_crypto_null.py
python3 scripts/cross_crypto_close.py
```

Raw output preserved in `reports/cross_crypto_null_raw.log` and
`reports/cross_crypto_close_raw.log`. Per-specification detail in `reports/cross_crypto_{60,240,720}m.csv`.

### Footnotes

- AVAXUSDT has no measured Kalshi one-way cost and uses the 17.0 bp default. It is not the winning
  specification at any bar length.
- The per-bar CSVs carry **252** rows: the 210 no-model specifications the null sweep covers, plus
  42 LASSO fits. The LASSO is excluded from the null because its penalty is selected on the
  validation slice, so shifting the target changes what is fitted and the null would no longer be
  the same sweep. Excluding it is conservative in the only direction that matters here — the LASSO
  clears the hurdle zero times, so including it would lower every observed aggregate statistic.
- `reports/cross_crypto_{60,240,720}m.{md,csv}` were regenerated after the last six assets finished
  downloading, so they now cover the same 14-asset panel as the null. The earlier 8-asset CSVs put
  the winner at 1.5394x on a slightly shorter sample; the reconciled figure is 1.5424x, and every
  number in this report comes from the 14-asset panel.
- The first null run (`cross_crypto_null.py`) swept 120 specifications over 8 assets and returned
  p = 0.135 for the same 1.5424x. Six more assets had finished downloading by the time of the
  closing run, which swept 210. The observed maximum is unchanged — none of the new assets
  produced a better specification — so the two runs are directly comparable, and both p-values are
  reported rather than only the later one.
