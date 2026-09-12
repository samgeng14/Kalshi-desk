# Can BTC and ETH be traded directionally on Kalshi?

Run before building BTC/ETH IMPULSE, the same way `maker_viability.md` was run before FAIR-MAKER.
The strategy earns from large directional moves rather than spread, so the questions are whether
moves that large exist, how often, and what a model would have to achieve to profit from them.

**Answer: the moves exist in quantity, the short horizons are arithmetically dead, and the whole
thing turns on getting both legs passive.**

## Correction to an earlier claim

`maker_viability.md` said BTC and ETH were "ruled out permanently." That was about **spread
capture** and it was written as though it were about trading them at all. It is not. A 0.5 bps
quoted gap cannot fund a market-making strategy, and that says nothing about whether a directional
strategy can clear a 17 bps round trip. BTC and ETH remain first-class trading markets under a
different thesis. The claim has been narrowed in that document.

## The moves exist

Kalshi mid-to-mid, 100 days, non-overlapping opportunities per day:

| contract | ≥20 bps in 2m | ≥25 bps in 2m | ≥25 bps in 5m | ≥25 bps in 10m |
|---|---|---|---|---|
| KXBTCPERP | 19.8/day | 10.6 | 13.8 | 14.4 |
| KXETHPERP | 36.4/day | 21.6 | 23.6 | 22.4 |
| KXSOLPERP | 54.3/day | 32.8 | 34.6 | 30.8 |
| KXXRPPERP | 54.2/day | 33.9 | 33.1 | 28.6 |

Roughly 10 to 15 opportunities a day on BTC and 20 to 24 on ETH at a 25 bps threshold. That is
close to the 5-30 and 10-40 a day guessed in advance, so the opportunity count is not the problem.

## Perfect foresight, as an upper bound

What one trade earns after a 17 bps round trip, for a model that identifies the largest moves and
is **never wrong about direction**. Nothing real can beat these.

| contract | 1m top 10% | 2m top 10% | 5m top 10% | 10m top 10% |
|---|---|---|---|---|
| KXBTCPERP | **−3.2** | +2.3 | +13.2 | +25.3 |
| KXETHPERP | +0.4 | +7.6 | +21.8 | +37.8 |
| KXSOLPERP | +3.8 | +12.2 | +29.1 | +48.1 |
| KXXRPPERP | +4.7 | +13.3 | +30.2 | +49.5 |

**A perfect oracle on the top decile of 1-minute BTC moves loses money.** The move is smaller than
the fee. That single cell disposes of the shortest horizons before any model is fitted.

## The number that matters: required directional accuracy

Trading the top 5% of moves, a model right a fraction p of the time earns `(2p−1)·E|r| − fees`.
Breakeven is `p = (fees/E|r| + 1)/2`. This is the bar, and it is set here in advance so it cannot
be moved later.

| contract | h | E\|r\| top 5% | breakeven p @17 | breakeven p @10 |
|---|---|---|---|---|
| KXBTCPERP | 1m | 18.2 | **96.8%** | 77.5% |
| KXBTCPERP | 2m | 25.2 | 83.7% | 69.8% |
| KXBTCPERP | 5m | 39.6 | 71.4% | 62.6% |
| KXBTCPERP | 10m | 55.3 | 65.4% | **59.0%** |
| KXETHPERP | 1m | 22.9 | 87.1% | 71.8% |
| KXETHPERP | 2m | 32.3 | 76.3% | 65.5% |
| KXETHPERP | 5m | 50.9 | 66.7% | 59.8% |
| KXETHPERP | 10m | 71.9 | 61.8% | **57.0%** |
| KXSOLPERP | 10m | 84.1 | 60.1% | 55.9% |
| KXXRPPERP | 10m | 87.9 | 59.7% | **55.7%** |

**A coin flip is 50%. Published short-horizon crypto microstructure models reach roughly 52-56%.**
Read the table against that, not against 100%.

## Three conclusions, committed before any model is fitted

**1. Drop 10s, 30s and 1m for BTC and ETH.** The pre-registered horizon list had six entries; the
three shortest are arithmetically dead. One-minute BTC needs 96.8% directional accuracy at
maker/taker, and a perfect oracle on the top decile still loses 3.2 bps. Sub-minute horizons are
smaller still and cannot be better. **The horizons to test are 2m, 5m and 10m, weighted toward
10m.**

**2. Passive execution on both legs is the strategy, not an optimization.** Going from maker/taker
to maker/maker drops the bar on BTC 10m from 65.4% to 59.0%, and on XRP 10m from 59.7% to 55.7%.
That is the difference between a level no documented model reaches and one at the edge of the
published range. The fill model is therefore not a refinement to add later; it decides whether the
strategy exists.

**3. The most tradable major is not BTC.** XRP at 10m needs 55.7% and ETH needs 57.0%, against
BTC's 59.0%, because their moves are larger relative to the same fixed fee. BTC has the best data
and the worst economics. Rank by required accuracy, not by familiarity.

## A data-quality warning

The same calculation on KXWLDPERP returns +5,422 bps at the 10-minute top percentile. That is not
an opportunity, it is a 14-day contract with 68% empty minutes and a 13 bps quoted gap, where the
"moves" are quote jumps in a book nobody is trading. Any contract from the grade-C group must have
its moves verified against actual prints before a return distribution computed from its midpoint
is taken seriously. This is the same failure as the stale-book `close_quote_gap` result in
`minute_bar_alpha_results.md`, and it is why that group is excluded from this analysis.

## What this does not answer

Whether any model reaches 57-59% on the direction of large moves. This document only establishes
what the target is and rules out the horizons where no target is reachable. The pre-registered
tests H5 and H6 in `research/preregistration.md` are the ones that bear on it, since the thesis is
that external venues move first and Kalshi has not finished responding.

Everything above is computed from 1-minute bars, so it cannot speak to 10-second or 30-second
behaviour directly. It bounds them: moves over a shorter window are smaller, and the 1-minute
figures already fail.
