# Frozen evaluation specifications

Written 2026-09-07, before any results exist. The point of freezing a specification is that the
decision rule is fixed while the answer is still unknown; a rule chosen after seeing the numbers
is not a test. Nothing below may be changed once data starts arriving. If a specification turns
out to be wrong, it is abandoned and replaced by a new one with a new name, and the abandoned one
stays in this file with the reason.

Both experiments are **prediction-only** (`research/execution.py`). Neither may conclude anything
about profit. An execution claim needs recorded books at the decision time, which began
accumulating on 2026-09-07 and did not exist before.

## Shared ground rules

**Target.** The value Kalshi settles on: the NWS daily climate maximum for the station named in
the contract rules, as whole degrees Fahrenheit. Verified in `docs/settlement.md`.

**Data.** Only rows from the `observations` table, read through `ReplayCursor` on the
`received_ts` clock. A decision at time T sees exactly what the desk had at T. Forecast rows carry
their model cycle in `init_ts` and that field is never used to decide availability.

**Split.** Chronological, by complete station-day, never by contract. The brackets of one
station-day are one observation, not six: they share a target and their errors are perfectly
dependent. Days are assigned train / validation / test in blocks, not interleaved, because
weather is autocorrelated across adjacent days.

**Metric.** Brier score against the settled outcome, computed per station-day on the whole ladder
(the ranked probability score, so that being wrong by one bracket costs less than being wrong by
four). Reported with a bootstrap interval over station-days.

**Comparison.** Three predictions on identical station-days:

| | |
|---|---|
| market-only | the contemporaneous mid, normalised across the ladder |
| weather-only | the model, no price input |
| combined | a fixed 50/50 blend, chosen now rather than fitted |

**Sample floor.** No conclusion before **120 station-days** in the test block for that station.
At one day per station per day, Austin and Chicago together reach that in roughly four months.
This is the number that makes the programme slow, and it is not negotiable downwards.

**Decision rule, fixed now.** A model graduates from research-only if, on the untouched test
block: the weather-only ranked probability score is lower than market-only, AND the bootstrap
interval on the difference excludes zero, AND the combined score is no worse than market-only.
Anything else leaves the model research-only. A better score is not permission to trade; it is
permission to run session 10's execution work.

---

## WX-EXP-1 (Austin): does the morning tell you about the afternoon?

**Hypothesis.** By late morning, the difference between what has been observed and what was
forecast for those same hours carries information about the rest of the day that the market has
not fully absorbed. Austin because its summer days are hot, dry and convection-poor, so the
temperature curve is smooth and a morning error tends to persist.

**Decision times.** 09:00, 11:00 and 13:00 local, scored separately. Later hours are excluded
deliberately: by mid-afternoon the running maximum dominates and the question stops being about
forecasting.

**Features, fixed now.**

1. Observed minus forecast for each elapsed hour today, summarised as the mean residual over the
   last three hours.
2. The running maximum so far, and the one-hour and three-hour trend.
3. Hours of daylight left, and the hour of the day.
4. Station bias by lead time, fitted on the training block only.

The residual enters as a partial carry-forward: the remaining hourly forecasts are shifted by
`k` times the recent residual, and `k` is fitted on training days only, once, and then frozen.

**What would falsify it.** `k` fitted on the training block coming out near zero, or the
validation score failing to improve over market-only. Either ends the experiment.

---

## WX-EXP-2 (Chicago): do cloud, wind and frontal changes beat the tape?

**Hypothesis.** Chicago's daily maximum is set by synoptic timing - when a front arrives, whether
the lake breeze turns, how quickly cloud clears - and forecast updates carrying that information
reach the desk before the market fully reprices.

**Decision times.** Every forecast update received between 06:00 and 15:00 local, scored at the
moment of receipt. This experiment is about update timing, so it is keyed to arrivals rather than
to the clock.

**Features, fixed now.**

1. Change in the forecast maximum since the previous received cycle.
2. Cloud cover and wind direction/speed forecast for the peak hours, and their change.
3. Time since the last update actually received, so a stale feed is visible as a feature.
4. The same residual and running-maximum terms as WX-EXP-1, so the two are comparable.

**What would falsify it.** Forecast-change carrying no information after the residual terms are
already in, or the market moving on the same update within the desk's own latency - which the
recorded books can now measure, and which would say the information exists but is not reachable.

---

## Los Angeles

Deferred, as a separate coastal experiment. The marine layer makes the daily maximum depend on
burn-off timing, which is a different problem from either of the above and should not be pooled
with them.

## Status

| | |
|---|---|
| WX-EXP-1 (Austin) | specification frozen; 0 of 120 station-days recorded |
| WX-EXP-2 (Chicago) | specification frozen; 0 of 120 station-days recorded |
| earliest possible conclusion | roughly four months from 2026-09-07 |
