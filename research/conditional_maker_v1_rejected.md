# Conditional maker v1 — REJECTED_V1

**No evidence of executable edge for this one-sided conditional-maker construction under the
tested regime.**

That sentence is the whole claim and it is deliberately narrow. This does not say maker
strategies do not work. A two-sided market maker that fills on both sides captures a real spread
and is a different hypothesis that nothing here tested.

## What was tested

A one-sided passive entry, selected by a fitted score over 79 pre-fill features, exited under ten
policies. 48,663 passive entry fills from 18.8M recorded L2 events; the top 10% of the held-out
half taken as entries; 38,643 priced round trips.

## What rejected it

**The round trip.** 0 of 40 entry-filter / exit-policy combinations positive. Best −24.11 bps.
Measuring the exit rather than assuming it moved the best case by 1.4 bps, from −28.07
(`taker_now`) to −26.69 (`maker_touch_30s`). Passive exiting works mechanically — 76% passive
fills at best, exit fee down from 12.0 to 6.7 bps — and the 5.3 bps of fee saved costs 5.9 bps of
drift while holding. `reports/exit_engine.md`

**The markout.** Score-selected fills have *worse* post-fill markouts than a random selection at
every horizon, with separated bootstrap intervals: −5.51 vs −3.86 at 1s, −7.56 vs −5.09 at 60s.
`reports/kept_bias_check.md`

**The metric that produced the original positive result.** `kept = half_spread_at_POST +
markout_from_FILL` credits a spread against the mid at the moment the order was posted. You own
that spread only if you fill without the mid moving, and the reason you fill is that it moved.
The model selected 18.3 bps median spreads against 8.2 for a random pick, worth +4.04 bps of
post-time credit — which was the entirety of its apparent advantage. Wide spreads carry worse
adverse selection on this venue; the execution study measured that correlation at **−0.905**.
`kept` added a term the model could inflate to a term the model made worse.

**The one-feature null.** The fitted 79-feature ridge beat ranking by quoted spread alone by
+0.14 bps averaged across 24 cells. `reports/conditional_maker.md`

**The spread-matched control.** Holding instrument, 5-minute bucket, spread decile and volatility
quartile fixed, the within-stratum difference is flat at 3 of 4 horizons and marginal at the
fourth (+0.72 bps, [+0.11, +6.11], an interval 9× longer above the estimate than below).
1,712 of 1,946 selected fills matched across 251 strata. `reports/spread_matched_control.md`

Five routes, one answer.

## What is kept, not deleted

- `scripts/conditional_markout.py`, `scripts/exit_engine.py`, `scripts/kept_bias_check.py`,
  `scripts/spread_matched_control.py`
- `reports/maker_fills.csv` (48,663 fills, 101 columns), `reports/round_trips.csv`
- every report above
- `tests/test_exit_engine.py`, `tests/test_exit_engine_power.py`, `tests/test_kept_bias.py`

The exit engine and the queue machinery are reusable by anything that needs a fill model. The
bias check is now a permanent guard, not a one-off.

## What would reopen it

- A materially and robustly positive spread-matched difference on substantially more tape.
- A maker fee at or below roughly 2.4 bps — tier 3, $1M of 30-day volume. `reports/fee_ladder.md`
  also shows what it costs to get there, which is why this is not a plan.
- A genuinely two-sided construction, which is a different hypothesis and needs its own
  pre-registration rather than a re-run of this one.

## The rule this leaves behind

Any prediction target or P&L term that references a price from **before the fill** gets audited
before anything is optimised against it. A model will find the reference before it finds the
edge. That check is `kept_bias_check.py` for maker targets and `impulse_target_audit.py` for
directional ones, and it runs before the model, not after.
