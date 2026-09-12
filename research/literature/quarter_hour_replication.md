# Quarter-hour effect — the paper, as specified

**Source.** Chan Kim and Peter Reinhard Hansen, *The Quarter-Hour Effect: Periodic Algorithmic
Trading and Return Predictability in Cryptocurrency Futures*, arXiv:2607.09426. Submitted
10 July 2026, revised 16 July 2026. Funding: a grant from Ripple's University Blockchain Research
Initiative; the paper states the funder had no role. The authors credit Wade Kimbrough with an
independent data validation and replication study.

**Abstract, verbatim:**

> Cryptocurrency markets exhibit periodic bursts in volatility and volume at one-minute,
> five-minute, and quarter-hour marks. Using trade data for six Binance perpetual contracts, we
> link these bursts to algorithmic participation: trade-size roundness declines sharply during
> them. The Autocorrelation Map, a clock-phase-resolved display, reveals serial dependence in
> order flow and returns at quarter-hour openings that conventional measures obscure. Opening
> returns are predictable out of sample, while opening order imbalance predicts returns over four
> to twelve hours, with much weaker effects at finer clock-time frequencies. Together, these
> findings characterize periodic algorithmic trading and its cross-frequency variation.

## Specification as extracted

| element | the paper |
|---|---|
| sample | 1 Jan 2021 – 31 Oct 2024, 1,400 calendar days |
| venue | Binance USDT-margined perpetual futures |
| assets | BTC, ETH, XRP, SOL, DOGE, ADA |
| data | aggregate trades: timestamp (ms), price, quantity, `isBuyerMaker` |
| boundaries | minute 0, 15, 30, 45 of each hour |
| window | the first **10 seconds** after the boundary, where the burst peaks |
| signed flow | `OF_t = Σ (V_k · D_k)`, `D_k = +1` buyer-initiated, `−1` seller-initiated |
| imbalance | `OI_t = OF_t / Σ V_k ∈ [−1, 1]` |
| returns | log returns, `r_t = log P_t − log P_{t−1}` |
| model | penalised linear regression (LASSO), rolling-window estimation |
| headline | opening OI predicts returns over **4–12 hours**; much weaker at finer clock frequencies |

## Two things the paper does not contain

**No transaction-cost analysis.** Checked directly. The paper discusses microstructure
implications and implications for scheduled algorithmic traders and liquidity providers, but
reports no fee, spread or feasibility adjustment anywhere. "Predicts returns" in that abstract
means predicts. It does not mean pays. Every number it reports is a Level 1 number in our
scheme, and the entire question of whether the effect survives a 24 bp Kalshi round trip is ours
to answer, not theirs.

**No reported effect size we could find for the 4–12 hour result.** The abstract asserts the
predictability and Section 6 treats it, but the fetched text does not surface an R², a
t-statistic or a coefficient for that specific horizon. So the replication gate here cannot be
"reproduce their number"; it has to be "reproduce their qualitative shape" — a monotone relation
between opening order imbalance and 4–12 hour returns, stronger than at 15–60 minutes.

## What we run, and why it is weaker than the paper on purpose

A **decile sort**, not a LASSO. If a published effect is present it is visible in a sorted table;
if it is not, a penalised regression will find something anyway. The sort is the test that can
fail cleanly. The paper's own model comes after, and only if the sort passes.

Sorting is **within asset**. The imbalance distribution differs across coins, so a pooled sort
would put the noisiest coin in the extreme buckets rather than the strongest signal.

Inference clusters on **calendar day, shared across assets**. Six assets at the same boundary
are one draw from the market, not six — a shock that moves BTC moves SOL in the same minute. Day
clustering also absorbs the overlap created by sampling 4-, 8- and 12-hour returns every fifteen
minutes, which is the dependence that would otherwise make ninety days look like nine thousand
observations.

**No lookahead, by construction.** The signal is measured over `[boundary, boundary+10s)`. The
return starts at the close of the one-minute bar containing the boundary, which is 60 seconds
after the boundary and 50 seconds after the signal window shuts.

## Our sample versus theirs

| | paper | here |
|---|---|---|
| days | 1,400 | 90 (aggregate trades), 400 (klines for returns) |
| period | 2021-01 to 2024-10 | trailing 90 days to 2026-09 |
| assets | 6 | 6, the same ones |
| boundaries | ~537,000 | ~52,000 |

The period does not overlap the paper's at all. That is a feature and a limitation: if the effect
replicates on a disjoint two-years-later sample it is stronger evidence than an in-sample rerun,
and if it does not, the honest reading is that it may have decayed rather than that the paper was
wrong.

## Raw data

Binance publishes immutable daily dumps at `data.binance.vision`. The REST API is geo-blocked
from this environment (HTTP 451); the archive is not. `scripts/binance_history.py` records a
manifest with the URL, byte count and SHA-256 of every file it fetched, so the derived boundary
dataset is reproducible byte-for-byte without anyone hosting 3.5 GB of trades. A checksum
mismatch on re-fetch is a loud failure, not a silent redownload.

## The gate

If the monotone 4–12 hour relationship is not present here, **this lane stops and is not tested
on Kalshi.** That is the pre-registered rule and it is the whole point of replicating first.
