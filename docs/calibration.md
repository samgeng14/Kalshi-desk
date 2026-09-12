# What has beaten the price, and what has not

Run `python3 scripts/score_models.py` for the current table. This records the state on
**2026-09-11** and, more importantly, why the previous answer was wrong.

## The gate

A model earns a place in lane 1 by one test: on markets that have since settled, was its number
closer to the outcome than the market's own mid at the same moment. Both are scored on the same
events, so the comparison is paired and a Brier difference is meaningful.

Two things about how that is measured were wrong, and both flattered the models.

**One sample per market, not per fair.** `model_fairs` holds a row per cycle, so a contract the
desk quoted all day contributes a hundred rows that share a single outcome. They are not
independent. Averaging over rows inflates the sample count and silently weights whichever markets
happened to be quoted longest:

| strategy | fairs logged | settled markets | rows per market |
|---|---:|---:|---:|
| crypto_lognormal | 17,134 | 1,080 | 16 |
| weather_nws | 11,390 | **107** | **106** |
| sports_live | 1,692 | 33 | 51 |
| sports_inplay | 174 | 12 | 14 |

The sizing gate required 200 samples. Weather cleared it on 107 markets' worth of evidence while
reporting 11,390, and sports_live on 33.

**An interval, not a point.** The old test promoted a model whose Brier was "no worse than" the
market's. A point estimate that ties is a null result, and this gate decides what trades
unattended. Promotion now requires the whole 95% interval on the paired difference to sit below
zero.

## The table, 2026-09-11

Last fair before settlement, which is the most information each strategy ever had on that market
and therefore the reading most favourable to it.

| strategy | markets | model Brier | market Brier | difference | 95% CI | verdict |
|---|---:|---:|---:|---:|---|---|
| crypto_lognormal | 1,080 | 0.0540 | 0.0442 | +0.0098 | [+0.0070, +0.0127] | the price is better |
| weather_nws | 107 | 0.1445 | 0.0370 | +0.1074 | [+0.0659, +0.1569] | the price is better |
| sports_live | 33 | 0.0357 | 0.0491 | −0.0134 | [−0.0342, +0.0004] | too few markets |
| sports_inplay | 12 | 0.2674 | 0.1995 | +0.0678 | [+0.0040, +0.1462] | too few markets |
| xvenue | 0 | — | — | — | — | nothing settled yet |

Scored at a mid-life fair instead, crypto is indistinguishable rather than worse (+0.0010, CI
[−0.0009, +0.0030]) and weather is still clearly worse (+0.0521). So crypto's deficit is
concentrated in the last minutes before settlement, where the book is sharpest and the model has
least room; weather's is everywhere.

**Nothing has beaten the price.** The one interesting reading is sports_live, which is ahead by
0.0134 with an interval that stops just short of zero on 33 markets. That is a reason to keep
recording it, not a reason to trade it.

## What follows

- Every strategy is now sized flat at `uncalibrated_max_order_frac` rather than by Kelly, because
  none of them satisfies `demonstrated`. That is a real behaviour change and the correct one:
  Kelly is worth exactly as much as the probability fed into it.
- weather_nws is the largest recorded effort on this desk and is the furthest behind the price.
  Its Brier is four times the market's. Before more weather work, the question worth answering is
  whether the settlement source mapping is right at all, because a model this far off a liquid
  market usually has an input problem rather than a modelling one.
- sports_live needs about 100 settled markets to say anything. At 33, more sport is the cheapest
  experiment available.
- The 23 entries in `docs/hypotheses.md` are still untested. Most need data this desk does not
  record yet: a de-vigged closing line for SPORT-2, per-bracket curve snapshots for CRYPTO-2,
  consensus for ECON-1. Testing them is a data-collection project first and a modelling one
  second; the table above is the part that could be answered from what is already on disk.

---

# Market making, 2026-09-11

`python3 scripts/maker_edge.py`. Kalshi pays makers nothing on quadratic series, so a maker's
whole revenue is the half-spread he captured and his whole cost is what the mid then does against
him. Both are measurable from fills that already happened, which makes this the one idea here
that does not depend on a model being right.

Measured on 34 maker fills (taker fills excluded: a taker crossed because he wanted to, so the
drift afterwards is partly his own impact, and mixing the two made adverse selection look far
worse than a maker faces):

| horizon | n | half-spread captured | drift against | net per contract | 95% CI |
|---|---:|---:|---:|---:|---|
| 30s | 34 | 0.00779 | −0.00824 | −0.00044 | [−0.0132, +0.0112] |
| 60s | 34 | 0.00779 | −0.01015 | −0.00235 | [−0.0172, +0.0109] |
| 300s | 32 | 0.00734 | −0.00578 | +0.00156 | [−0.0309, +0.0292] |
| 900s | 27 | 0.00778 | −0.03593 | −0.02815 | [−0.0626, +0.0074] |

**Not distinguishable from zero at any horizon.** The half-spread and the adverse drift are the
same size to within noise. Making is not proven and not dead; on 34 fills it is a coin flip.

Two things worth keeping from this:

- The drift is worst around a minute and partly reverts by five, which is the shape of transient
  impact rather than pure information. If that holds up, holding through the first minute is
  better than quoting to flip immediately.
- Fills arrive at a half-spread of 0.0078 against a book average of 0.0081. Those are close, so
  there is **no** evidence of being picked off specifically at narrow spreads. An earlier draft of
  this claimed there was; the numbers do not support it.

Against the unconditional book, BTC's mean spread is 0.0305 and its mid moves 0.0110 a minute,
ETH 0.0471 against 0.0185 — a ratio near 0.37, which is why the idea looked promising before the
fills were consulted. The gap between that and the table above is the whole lesson: the spread on
the screen is not the spread you get filled at.

34 fills decides nothing. This is the cheapest experiment on the desk to run properly, because it
needs no new data source, only more maker fills.
