# Cross-crypto return predictability — the papers, as specified

Two papers, both verified directly rather than taken on the citation.

## Paper 1 — the seesaw effect

Yuecheng Jia, Yangru Wu, Shu Yan, Yuzheng Liu, *A seesaw effect in the cryptocurrency market:
Understanding the return cross predictability of cryptocurrencies*, **Journal of Empirical
Finance, Volume 74, 2023**.

> This paper investigates the intraday return cross-predictability of cryptocurrencies. In
> contrast to the positive lead–lag effect for stocks, we document a negative lead–lag effect in
> the cryptocurrency market. Specifically, the large coins negatively predict the other coins but
> the small coins rarely predict the large coins. A trading strategy that exploits the
> cross-predictability via the Least Absolute Shrinkage and Selection Operator (LASSO) yields
> highly significant profits across major cryptocurrency exchanges even in the presence of
> realistic transaction costs.

## Paper 2 — cross-cryptocurrency return predictability

Li Guo, Bo Sang, Jun Tu, Yu Wang, *Cross-cryptocurrency return predictability*, **Journal of
Economic Dynamics and Control, Volume 163, 2024**.

> Using data from Binance, we find strong evidence of cross-cryptocurrency return predictability.
> The lagged returns of other cryptocurrencies serve as significant predictors of focal
> cryptocurrencies. The results are robust across various methods, including the adaptive LASSO
> and principal component analysis. Furthermore, a long-short portfolio formed on the past
> returns of cryptocurrencies can generate a sizable return out-of-sample after accounting for
> transaction costs. Overall, our findings corroborate cross-cryptocurrency return predictability
> and are consistent with the spillover effect mechanism, where common shocks among
> cryptocurrencies coupled with the limited attention of investors lead to slow information
> diffusion across coins.

## Why these are better grounded than the quarter-hour paper

Both state post-cost profitability in the abstract. The quarter-hour paper contains no
transaction-cost analysis at all, which is why its statistically real effect turned out to be
0.05× of what a Kalshi round trip needs. These two at least claim to have cleared a cost bar,
on their own venues and at their own sizes.

## The transfer problem, stated before any code is written

The seesaw result has a direction: **large coins predict small coins, and small coins rarely
predict large coins.** So a strategy built on it does not predict BTC and ETH. It predicts the
small coins, using BTC and ETH as inputs.

That matters here more than it would anywhere else, because of what this project already
measured about Kalshi execution:

| Kalshi perp | median quoted spread | one-way cost to cross |
|---|---:|---:|
| BTC | 0.5 bp | 12.32 bp |
| ETH | 0.8 bp | 12.50 bp |
| ADA | 9.6 bp | 17.69 bp |
| NEAR | 14.9 bp | 19.74 bp |
| LINK | 18.1 bp | 21.30 bp |

**The signal is strongest exactly where the execution is worst.** The coins the seesaw effect
says are predictable are the ones that cost 18–21 bp to cross one way on Kalshi, against 12.3 for
BTC. A replication that succeeds on Binance and then fails on transfer is the expected outcome,
and the scoreboard should be ready to record that as `FAILED_TRANSFER` rather than as a surprise.

This is not a reason to skip the replication. It is a reason to run the economic screen on the
*small-coin* hurdle from the first line, rather than on BTC's.

## What the replication must produce, in order

1. Does the published statistical relation appear on our sample? (IC, rank IC, block-bootstrap CI)
2. Is it economically large? (top-minus-bottom spread, turnover, gross bps per trade)
3. `economic_ratio = observed effect / required effect` at the hurdle for the coin actually traded
4. Only then: does it survive Kalshi's bid/ask and fees?

The economic hurdle is computed **before** any model tuning, and the signal is frozen before it
is tested against the hurdle. Optimising against a 24 bp threshold is how a threshold becomes a
target.

---

# Outcome — `FAILED_SELECTION`

Everything above this line was written before the code. What follows is what happened.

The four required outputs, in the order they were specified:

1. **Does the published statistical relation appear?** No. The two mechanisms the papers
   actually propose — LASSO over the cross-section of lagged returns, and the seesaw direction
   in which large coins negatively predict the rest — clear the cost hurdle **zero times out of
   84 specifications**. The LASSO is the worst model class in the sweep (mean economic ratio
   −0.1187, best 0.4915, 21.4% of its fits positive). The six specifications that did clear were
   three own-momentum rules, which have no cross-crypto content, and three equal-weight-basket
   rules, which are a market factor rather than a lead–lag effect.
2. **Is it economically large?** The best specification, SOL own momentum at 720m, returned
   +43.47 bp gross per trade against a 28.24 bp Kalshi round trip.
3. **`economic_ratio` at the traded coin's hurdle:** 1.5424x. The first number in this project to
   clear 1.0.
4. **Does it survive Kalshi's bid/ask and fees?** Never tested, because it did not survive the
   step before it.

**It did not survive multiple testing.** 1.5424x is the maximum of 210 correlated specifications
on a 160-bar test slice. Under a circular-shift null that preserves each asset's autocorrelation
and the full cross-sectional correlation structure, the median best-of-210 is 1.0854x and
p = 0.167 over 300 shifts. A random misalignment of this panel clears the cost hurdle somewhere
more than half the time.

The diffuse check is worse than the max check. Only **32.4%** of the 210 specifications have a
positive gross return on real data, against **49.8%** under the null — breaking the link between
predictor and target improves the typical specification. And the winner's held-out slice, split
into consecutive thirds, has hit rates of 0.434 / 0.481 / 0.660: wrong more often than right in
two thirds of the test window, with the entire result carried by the last 53 bars.

Full numbers, method and the narrow statement of what was and was not tested:
`reports/cross_crypto_replication.md`.

## What the transfer problem did

Nothing, in the end — the replication failed on Binance, before Kalshi execution was ever priced
against it. The prediction written above still stands untested: the coins where the seesaw effect
should be strongest are small coins, and the smallest asset in this panel is a top-50 perp,
because that is the universe Kalshi lists. A future replication with a proper lag structure on
daily bars would still arrive at that wall.
