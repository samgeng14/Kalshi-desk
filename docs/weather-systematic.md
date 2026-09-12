# The weather systematic book

One weather state per city-day, one distribution, several sleeves reading from it. This document
records what the evidence supports building, and what it says not to build yet. Section numbers
refer to the V3 brief.

## The architecture is not in dispute

```
FORECASTS + OBSERVATIONS
        |
        v
  WeatherEventState          one per city/day (section 2)
        |
        v
  P(settled extreme = x)     one distribution, every bracket an integral over it (section 4)
        |
        v
  WeatherOpportunityEngine
        |
        +---> sleeves, each with its own threshold, hold, exit and graduation (section 26)
```

This part of the brief is right and is what the WX1 work already builds toward: `wx1_for()`
simulates one distribution per station-day and every bracket reads off it via `read_bracket()`.
A 15-minute trade and a hold-to-settlement trade will never see contradictory forecasts, because
there is only one.

What the evidence changes is **which sleeves exist**.

## What the measurements say

Three independent measurements now exist, and they agree with each other.

**1. The raw model does not beat the price** (`reports/wx_score.md`). Brier +0.0202 against the
mid over 354 observations and 31 settled event-days; 95% CI clustered by event-day
[-0.0218, +0.0621]. Not established as worse; no evidence of edge in the number itself. It is
worse day-ahead (T-24h +0.0361) and better near close (T-30m -0.0478), which is where its
observations pin the answer.

**2. Bigger predicted edge produces worse realized settlement** (`reports/weather_horizons.md`).

| predicted edge | n | realized at settlement |
|---|---|---|
| 3-4c | 533 | +7.36c |
| 4-5c | 540 | +1.80c |
| 5-7c | 819 | +3.56c |
| 7-10c | 1044 | -1.12c |
| 10c+ | 2009 | **-5.27c** |

Section 16 of the brief sets monotonicity as the test of whether edge is an economic quantity.
This is the opposite of monotonic, and it is the same finding as (1) seen from another angle:
the model's largest disagreements with the market are its largest errors.

**3. Short-horizon convergence is dead by arithmetic, not by statistics.**

| | value |
|---|---|
| median weather spread | 1.0c |
| round-trip taker fee (20c / 50c / 80c) | 2.24c / 3.50c / 2.24c |
| cost to enter and exit | **3.2 - 4.5c** |
| median move in the mid over 15 minutes | **0.00c** |
| 15-minute windows with no movement at all | 50% |

Executable markouts on 17,746 candidate states over 46 event-days, entering at the ask and
exiting into the bid that actually existed:

| horizon | n | mean | win rate |
|---|---|---|---|
| 5m | 4554 | -3.61c | 1% |
| 10m | 4391 | -3.64c | 2% |
| 15m | 4169 | -3.58c | 3% |
| 1h | 3414 | -3.69c | 8% |
| 4h | 2537 | -2.96c | 19% |
| settlement | 4477 | -0.77c | 27% |

Every horizon lands on the same number because the number is the round trip. The market would
have to move six to ten times its typical fifteen-minute movement to break even. No amount of
extra data changes 0.00c into 3.5c.

**4. The 80c -> 90c trade has never occurred.** Across 17,746 states over 46 event-days there was
no contract with an executable entry near 80c where the desk fair said 90% or better. The closest
was `KXHIGHMIA-26SEP07-T93` NO at 90c with a desk fair of 94.4%. The trade cannot be made a
systematic rule because the desk has never seen one.

## Sleeve decisions

| sleeve | brief | decision | why |
|---|---|---|---|
| `WX_SCALP` | 5-20 min, taker convergence | **do not build** | costs 3.2-4.5c to round trip; market moves 0.00c median in 15 min |
| `WX_INTRADAY` | 20-90 min, taker convergence | **do not build** | same arithmetic; 1h markout -3.69c |
| `WX_SWING` | 1-4h convergence | **do not build yet** | 4h markout -2.96c; same cost problem, weaker |
| `WX_MAKER` | not in the brief | **the short-horizon lane worth having** | maker fill pays zero fee on plain `quadratic` and earns the 1.0c spread instead of paying it |
| `WX_SETTLEMENT` | hold to settlement | **build, stays RESEARCH** | only lane where the fee is paid once and no repricing is needed - but (1) and (2) must be fixed first |
| `WX_DAY_AHEAD` | overnight | **build, stays RESEARCH** | separate calibration is right; this is where the model is currently worst |
| `WX_MANUAL` | requested by the operator | **build, MANUAL** | see below |

`WX_MAKER` replaces two dead sleeves with the one short-horizon idea the data does not rule out.
It is a different object from what the brief describes: the question is not "will the market
reprice" (a market-reaction model) but "will my resting order get filled, and is the fill adverse"
(a fill-probability model). Two of the three Sep-11 trades were maker fills. Nothing measured so
far speaks to it either way, which is exactly why it is worth measuring next.

## WX_MANUAL

The operator's own trades are hold-to-settlement, and they are a real sleeve rather than an
absence of one. It behaves as follows.

- **Horizon: settlement.** Default exit is hold. No convergence target, no time stop. This matches
  how the operator actually trades and it is also the only horizon whose economics are not
  mechanically negative.
- **Route: MANUAL, always.** It never graduates to AUTO, because the thing that makes it manual is
  that a person decided. Graduation applies to the systematic sleeves.
- **Sizing: the desk proposes, the person disposes.** Size comes from the router probability and
  the risk caps, never from the raw model - this is the Sep-11 error written into the design
  (section 18 of V2). The suggested size is a ceiling, not an instruction.
- **Lot attribution: its own.** A manual lot is tagged `MANUAL` and is never reduced by a
  systematic sleeve's exit. If `WX_SETTLEMENT` ever graduates and wants to trim the same contract,
  it trims its own lots; the manual lot is untouched unless the person says so.
- **Scored separately and never pooled with model validation.** Manual trades are the operator's
  decisions, not the model's, and mixing them into a Brier or PnL series would credit or blame the
  model for something it did not choose. The Sep-11 three are marked `ILLUSTRATIVE, NOT
  VALIDATION` and stay that way.

## What actually gets built next, in order

1. **All-city discovery (section 1).** The desk already knows 10 weather series and watches 8; two
   more (`KXHIGHTLV`, `KXHIGHTSEA`) are listed and unwatched. Discovery is cheap, it is a
   prerequisite for every capacity question, and it does not depend on the model being good.
2. **Replace the guessed settlement shift with the intraday CLI (section 4 step 8).** The NWS
   publishes the settling value from ~16:00 local, 86% of the time already final, against a market
   that trades until 01:00. This is measured (`reports/wx_settlement_watch.md`), not assumed, and
   it is the single largest known improvement to the state estimate.
3. **Fix the model before adding sleeves.** Measurements (1) and (2) say the number is not yet
   worth trading at any horizon. WX1 is recorded and waiting for days; the pre-registered
   prediction is that it improves the 0-20% calibration buckets first.
4. **WX_MAKER fill study.** Does a resting order at the bid get filled, and is the fill adverse?
5. **Capacity (sections 12, 13, 30)** once there is an edge to have capacity in.

Sections 6 (scalp/intraday/swing), 9 (market reaction model), 10 and 11 (convergence targets) are
deliberately not being built. They are downstream of a premise the data rejects. If `WX_MAKER` or
a fixed model changes that, they come back.
