# Minute-bar directional alpha on Kalshi perpetuals: a negative result

**Status: closed. Do not run further models on the 1-minute historical feature set.**

Measured 2026-09-11 on 1,812,026 one-minute bars covering 13 Kalshi perpetual contracts with 80
to 100 days of history each, from the backfill described in `docs/perps-models.md`. The question
was whether any primitive feature computed from a 1-minute bar predicts the next midpoint move by
enough to cover execution costs. It does not, and the margin is not close.

## Result

| hurdle | combinations positive |
|---|---|
| gross, no costs | 177 of the 400 strongest-IC results (44%) |
| maker/maker, 10 bps | 3 |
| maker/taker, 17 bps | **0** |
| taker/taker, 24 bps | **0** |

1,736 feature/horizon/regime/contract combinations were tested. The **largest gross edge found
anywhere in the study is 16.9 basis points**, against a maker/taker round trip of 17. The median
gross edge is 0.4 basis points.

Features tested: `mom_1m`, `mom_5m`, `mom_15m`, `revert_1m`, `close_quote_gap_bps`, `volume`,
`volume_shock`, `notional`, `oi_change_5m`, `minutes_since_trade`. Horizons 1, 2, 5 and 10
minutes. Regimes: all minutes, zero-trade minutes, 1 to 5 trades, more than 5 trades. Target is
always the **future midpoint return**, never trade-price to trade-price, because a trade-price
label carries bid-ask bounce and makes any feature built from a recent price appear to predict the
reversal of its own noise.

## Multiple testing

This is the part that decides how to read everything above. With 1,736 hypotheses at an
uncorrected 5% level, **roughly 87 results are expected to pass by chance alone**. 138 passed the
study's own three gates, which is not meaningfully more than chance produces.

Correcting properly:

| correction | threshold | discoveries |
|---|---|---|
| none, 5% | p < 0.05 | 138 passed the three gates |
| expected by chance | | ~87 |
| Benjamini-Hochberg, FDR 5% | | 70 |
| Bonferroni, family-wise 5% | p < 2.9e-5, \|t\| > 4.18 | **33** |

So 33 results are statistically real by the strictest available standard, some of them
overwhelmingly so: HAC t-statistics of 30.7, 29.4 and 27.6 on samples above 60,000.

**The choice of correction turns out not to matter, which is a stronger statement than picking
one.** Sweeping Benjamini-Hochberg across false-discovery rates:

| FDR | discoveries | best gross | best net @10 | best net @17 |
|---|---|---|---|---|
| 1% | 49 | +2.8 | −7.2 | −14.2 |
| 5% | 70 | +2.8 | −7.2 | −14.2 |
| 10% | 76 | +2.8 | −7.2 | −14.2 |
| 20% | 110 | +4.5 | −5.5 | −12.5 |
| 50% | 251 | +12.9 | +2.9 | −4.1 |

The best gross edge is **stuck at 2.8 bps from FDR 1% through 10%**. Loosening the gate from 49
discoveries to 76 adds twenty-seven results and not one basis point of edge, because the additional
discoveries are more significant and less profitable. Only at FDR 50%, where half the discoveries
are false by construction, does anything exceed 10 bps, and even there the net at 17 is −4.1.

**To clear the 17 bps maker/taker round trip the best discovery would need to be 6.1 times larger.
To clear the 10 bps maker/maker hurdle, 3.6 times.** That ratio, not the p-value, is the finding.

**Every one of those 33 loses between 14.2 and 19.8 basis points after a maker/taker round trip.**

The best of them:

```
KXKSHIBPERP  revert_1m  high_gt5  2m  n=50,849  t=+4.6  gross +2.8 bps  net@17 -14.2
KXZECPERP    revert_1m  high_gt5  1m  n=66,620  t=+5.5  gross +2.2 bps  net@17 -14.8
KXHYPEPERP   revert_1m  no_trade  1m  n=16,656  t=+4.4  gross +2.1 bps  net@17 -14.9
```

And the most significant result in the entire study:

```
KXBTCPERP  close_quote_gap_bps  high_gt5  10m  n=141,195  t=-30.7  gross +0.5 bps  net@17 -16.5
```

A t-statistic of 30.7 attached to a decile spread of half a basis point. That single line is the
finding: **these effects are not marginal, they are real and tiny.** Statistical significance and
economic significance have come apart completely, which is what a large sample does to a small
effect.

Note that `mom_1m` and `revert_1m` are the same feature negated, so they appear as mirrored pairs
(t of −5.5 and +5.5, gross of −2.2 and +2.2). They are one result, not two.

## Classification

### REJECTED_NONROBUST

Three combinations cleared the 10 bps maker/maker hurdle on their point estimate:

| asset | feature | regime | h | n | IC | HAC t | bootstrap CI | gross | @10 |
|---|---|---|---|---|---|---|---|---|---|
| KXBTCPERP | `close_quote_gap_bps` | no_trade | 10m | 734 | +0.148 | +1.5 | [-0.085, +0.237] | +16.9 | +6.9 |
| KXETHPERP | `mom_15m` | low_1_5 | 10m | 635 | +0.103 | +1.9 | [-0.058, +0.213] | +12.9 | +2.9 |
| KXBTCPERP | `close_quote_gap_bps` | no_trade | 5m | 735 | +0.137 | +1.2 | [-0.090, +0.272] | +11.0 | +1.0 |

Rejected as non-robust, for four separate reasons, any one of which would be sufficient:

1. **Every bootstrap confidence interval spans zero.** The point estimates are not distinguishable
   from no effect.
2. **Every HAC t-statistic is below 2**, at 1.5, 1.9 and 1.2. These would not pass an uncorrected
   single test, let alone a correction for 1,736 of them.
3. **Sample sizes are approximately 700**, against 142,000 available rows. They are the thinnest
   slice in the dataset: BTC trades in 99% of minutes, so the `no_trade` regime is 734 rows.
4. **Executability is doubtful on its face.** A wide close quote gap in a minute with no trade
   describes a stale book. The forward "return" is the midpoint catching up to a level at which
   nothing transacted, so it is not a price anyone could have traded against.

### REJECTED_COST

The 1-minute mean-reversion effect in high-flow minutes, plus the `close_quote_gap_bps` results,
are **real** by the strictest correction and **rejected on economics**. Gross edges of 0.2 to 2.8
basis points against a 17 basis point round trip. Nothing here is broken or mismeasured; the
effect simply is not large enough to pay for the trade.

This is a more useful rejection than the other kind, because it names the size of the gap. To
trade 1-minute reversion on this venue we would need roughly **six times** the current effect, or
a cost structure roughly six times cheaper.

## What this rules out, and what it does not

Ruled out: predicting the direction of the next 1 to 10 minute midpoint move on Kalshi perpetuals
from features computable off a 1-minute bar. Seven model families across this study and the
earlier one in `docs/perps-models.md`, 1,736 tests here, on three months of history.

Not ruled out, and specifically pointed to by this result: the one robust effect lives in
**high-flow minutes**, where book dynamics live and where a 1-minute bar can see nothing. Order
book imbalance, cancel-versus-trade classification, microprice, aggressive signed flow and
external-venue lead-lag are all invisible at this resolution. They are the subject of the
pre-registered work in `research/preregistration.md`.

Two things make that a different bet rather than a retry of this one:

- **The hurdle drops from 17 bps to 10.** A maker-in, maker-out round trip is 5 plus 5. Three
  results here already clear 10, and the open question is whether a maker fill is achievable at
  all, which paper trading has never answered.
- **Spread capture is a second source of return.** This study measured directional alpha only. A
  passive strategy earns the spread as well, and the cost model changes from a fee subtracted to a
  spread earned minus adverse selection. `research/maker_viability.md` screens every contract for
  whether the spread is actually there: six of twenty have a median quoted gap above the 10 bps
  maker/maker round trip, and BTC and ETH are ruled out permanently at 0.5 and 1.1 bps.

## Method notes

Two bugs were found and fixed during this study, both of which had manufactured apparent edges.
Recorded because the next person will otherwise rediscover them:

**Overlapping windows.** Sampling every bar for a 10-minute forward return gives ten overlapping
observations per independent one and inflates every t-statistic. Corrected with Newey-West HAC
errors and a moving-block bootstrap, both reported for every result.

**Tied features sorted by the label.** `deciles()` used `sorted(zip(x, y))`, which is
lexicographic on tuples: when x ties, it falls through to y, the forward return. On a feature that
is 68% one value, that orders the tied block by the thing being predicted and produces a clean
monotonic decile ramp from pure noise. Measured on noise with 68% ties it produced a +12.93 bps
spread and a monotonicity of +0.66. It reported a "+49.2 bps net" edge on KXNEARPERP before it was
caught. The fix sorts on x alone, refuses a decile table for any feature that is more than half one
value or has fewer than 20 distinct values, and adds a gate requiring the IC and the decile
monotonicity to agree on sign.

A third data fault, the INT64_MAX no-order sentinel arriving as 922337203685477.6, reached the
research path from the exchange and produced a reported edge of 3.19e14 basis points. 1,920 bars
carried it. `scripts/clean_sentinels.py` removes it and the backfill now rejects it at ingest.

Reproduce with:

```
python3 tests/test_alpha_decay.py
python3 scripts/alpha_decay.py --tickers KXBTCPERP KXETHPERP KXSOLPERP KXXRPPERP KXHYPEPERP \
  KXLINKPERP KXSUIPERP KXBCHPERP KXDOGEPERP KXLTCPERP KXKSHIBPERP KXNEARPERP KXZECPERP
```
