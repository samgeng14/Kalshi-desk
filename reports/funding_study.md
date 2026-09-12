# Funding study

141,318 ticker frames across 3 recorded hours.

## Can this tape answer the event question?

**No.** The next funding timestamp is `2026-09-11 20:00 UTC` and the tape ends at `2026-09-11 16:31 UTC`.

So the pre-funding basis, the post-funding return and the convergence afterwards are not measurable here. Only the ANNOUNCED rate is, and that is what is reported below. The event study needs recorder uptime spanning a funding timestamp; funding runs at 00:00, 08:00 and 16:00 ET, so a single overnight recording covers two of them.

## Announced rates

`rate` is per funding PERIOD. A period is eight hours, so three per day.

| market | rate bps/period | changed | round trip taker | round trip maker | periods to break even, taker | maker | 8h volatility | funding / vol |
|---|---:|---|---:|---:|---:|---:|---:|---:|
| KXZECPERP | -7.280 | yes | 38.6 | 8.8 | 5.3 | 1.2 | 789 | 0.009 |
| KXADAPERP | -3.653 | yes | 35.4 | 6.7 | 9.7 | 1.8 | 532 | 0.007 |
| KXWLDPERP | -3.554 | yes | 40.5 | 11.5 | 11.4 | 3.2 | 661 | 0.005 |
| KXNEARPERP | -1.204 | yes | 39.5 | 8.5 | 32.8 | 7.0 | 985 | 0.001 |
| KXAAVEPERP | +0.000 | yes | 38.6 | 14.0 | never | never | 575 | 0.000 |
| KXBCHPERP | +0.000 | no | 40.3 | 12.0 | never | never | 484 | 0.000 |
| KXBNBPERP | +0.000 | no | 33.9 | 12.4 | never | never | 300 | 0.000 |
| KXBTCPERP | +0.000 | yes | 24.6 | 10.4 | never | never | 274 | 0.000 |
| KXDOGEPERP | +0.000 | no | 33.6 | 9.8 | never | never | 460 | 0.000 |
| KXETHPERP | +0.000 | no | 25.0 | 11.0 | never | never | 458 | 0.000 |
| KXHYPEPERP | +0.000 | no | 31.9 | 8.6 | never | never | 416 | 0.000 |
| KXKSHIBPERP | +0.000 | no | 39.5 | 6.6 | never | never | 433 | 0.000 |
| KXLINKPERP | +0.000 | no | 42.6 | 5.3 | never | never | 520 | 0.000 |
| KXLTCPERP | +0.000 | no | 31.2 | 8.2 | never | never | 376 | 0.000 |
| KXSOLPERP | +0.000 | no | 28.2 | 9.7 | never | never | 369 | 0.000 |
| KXSUIPERP | +0.000 | no | 39.3 | 6.0 | never | never | 611 | 0.000 |
| KXVVVPERP | +0.000 | yes | 34.0 | 12.7 | never | never | 658 | 0.000 |
| KXXRPPERP | +0.000 | yes | 29.1 | 10.1 | never | never | 445 | 0.000 |

## Verdict

14 of 18 markets announced a funding rate of exactly zero over the whole recording. A zero rate is not a small opportunity, it is no opportunity.

The largest rate on the tape is `KXZECPERP` at **-7.28 bps per period**.

Against a 38.6 bps taker round trip that is **5 funding periods**, or about **2 days** of holding the position before the funding has paid for the trade that put it on.

Entering and exiting passively is cheaper. Priced at what the execution study measured a passive leg to actually cost -- half-spread earned, markout given back, fee paid -- the round trip is 8.8 bps, which the funding covers in **1.2 periods**, about 10 hours.

That is short enough to be worth taking seriously, so the objection has to be quantitative rather than a shrug at holding risk.

Over one funding period `KXZECPERP` moves with a standard deviation of **789 bps**. The funding collected over that period is 7.28 bps, or **0.92% of one standard deviation of the thing you are holding**.

To make the funding the dominant term rather than the noise, the position has to be held for roughly **11,741 periods** -- the ratio improves only as the square root of holding time, because funding accumulates linearly and risk accumulates as the square root. That is 3,914 days of unhedged exposure to collect a coupon worth a fraction of one day of its own volatility.

So this is not a funding trade. It is a directional position with a small coupon. Hedging it turns it into a real strategy -- and that hedge has its own two-sided cost, its own basis risk, and is not measured anywhere in this repository.

## What would make this worth revisiting

- A funding rate an order of magnitude larger. The break-even is linear in the rate, so a 10 bps per-period rate on BTC would pay for a taker round trip in about two periods.
- A cheaper round trip: see `reports/fee_ladder.md`.
- A tape that spans a funding timestamp, so the question stops being about the announced rate and becomes about what actually happens to the basis around the event.
- A hedge. Funding capture against a spot or external perp hedge is a different strategy with different arithmetic, and nothing here has measured it.

