# Kalshi desk: operating model and build plan

## What `tradeit up` means

`tradeit up` starts the local engine, opens the dashboard, and uses the authenticated Kalshi API
session configured on this Mac. Safari login is useful for viewing Kalshi, but API trading is
authenticated by the configured API key; browser cookies are not copied into the engine.

The engine refreshes watched markets, reconciles the account, computes model fairs, applies the
entry and risk gates, records research observations, manages exits and then evaluates unattended
orders. The dashboard refreshes independently and marks itself offline when it cannot reach the
engine.

AUTO is a permission switch, not a strategy. Turning it on permits only proposals that already
pass every active rule: model mode, model uncertainty, net edge after fees, fresh inputs, price and
size caps, daily stop, total/event/cluster exposure, cash and shard balance, settlement source,
T+2 horizon, position-aware admission and the daily unattended budget.

## The four lanes

1. **Research.** The estimate and supporting data are logged. No proposal or order is produced.
2. **Manual.** A proposal appears in Signals and waits for a click.
3. **Auto.** A proposal is sent when its strategy-specific auto rule passes and AUTO is armed.
4. **Instant.** A manual or auto proposal at or above `instant_edge` is sent in the cycle in which
   it appears, capped by `instant_max_cost`. It therefore normally never remains on the screen as
   a pending Signal. A send failure remains visible with its reason.

An edge shown as `0.07` means seven cents of expected value per $1 contract after the estimated
fee. It does not mean a 70% return or a guarantee. The instant lane is disabled at `0`; setting it
to `0.07` produces the behavior above for strategies allowed to trade. Research and off modes
never use it.

## Event management and additions

The first filled position changes the whole event from discovery to management. Other contracts
or the opposite side in that event leave ordinary Signals and point to the position/hedge tools.
The exact same ticker and side can reappear only as a qualified addition. Defaults on this desk:

- current net edge at least `0.08`;
- current edge at least `0.03` above the last recorded entry edge;
- at least 15 minutes since the last fill;
- no other working order in the event;
- manual approval, even when the instant lane is armed.

`auto_additions=true` can remove the last condition, but should remain false until additions have
their own out-of-sample record. A hedge is different: it is admitted only when the payoff table
shows a lower event worst case. A label such as “Notre Dame” or “Wisconsin” does not establish the
payoff relationship; the settlement states do.

## Time horizon

New risk must have an exchange `expected_expiration_time` (or its legacy expiration fallback) no
more than 48 hours away. Missing expiration guidance is blocked because the T+2 rule cannot be verified. This is an entry rule; an open
position remains visible and manageable if settlement is delayed. `expected_expiration_time` is
an estimate, so a dispute or operational delay can still push the actual cash payout later.

## Strategy state

- **15-minute crypto:** frozen for this workstream. Its model and execution code are unchanged.
- **Weather:** research-only until a point-in-time NBM/HRRR/station model beats the market mid on a
  held-out period and an executable replay remains positive after fees and fills.
- **NBA and other US sports:** research-only while possession-level data and order books accumulate.
  The current lead/time estimate is a baseline, not a tradeable proof.
- **Soccer:** the halftime state bug is guarded, but the model remains research-only until its
  phase transitions and calibration pass prospective tests.
- **Ladder/dominance arbitrage:** separate payoff-identity logic; all legs must be evaluated as a
  basket because partial fills create directional risk.
- **Manual fair:** accepts a user fair, then uses the same final risk and entry validation as every
  other entry path.

## NBA research sessions

### Session NBA-1: point-in-time dataset

Record play-by-play receipt time, score, regulation/OT period, clock, possession, bonus, team
timeouts and pregame strength. Align each state with Kalshi depth and trades using receipt time,
never the provider's earlier event time. Audit missing and revised plays.

### Session NBA-2: probability model

Fit a walk-forward model by season. Start with pregame strength, score differential, possession
and time remaining; add bonus/timeouts and team effects only when held-out log loss improves.
Represent a tie at 0:00 as entry into an overtime state with a conditional OT win probability.
The market should already price that branch, so there is no automatic “overtime jump” trade.

### Session NBA-3: microstructure replay

Measure spread, depth imbalance, book age, score-event latency, maker fill rate and post-fill
markout. Simulate actual order rules with queue uncertainty and report unfilled orders. Separate
forecast quality from execution quality.

### Session NBA-4: shadow deployment

Run research-only in real time. Freeze features and thresholds before the sample starts. Require
the model to beat the contemporaneous market probability and require positive fee-adjusted
execution results with confidence intervals that exclude zero.

### Session NBA-5: bounded activation

Move only the validated state buckets to manual, then auto with a small category budget. Keep OT,
feed gaps, clock corrections and states outside the trained support in research. Increase size only
after a new prospective sample passes the same gate.

## Weather research sessions

### Session WX-1: source and settlement map

For every city, bind the contract rule to its named station, product and reporting period. Do not
substitute a nearby station or a differently defined daily maximum.

### Session WX-2: point-in-time model archive

Archive NBM probabilistic guidance and every HRRR cycle with initialization, publication and local
receipt times. Record high-frequency station observations and the final settlement value. Preserve
model revisions rather than overwriting them.

### Session WX-3: distribution and calibration

Generate daily-maximum paths, apply city/lead-time bias corrections, and estimate residual tails.
Use rolling-origin folds. Compare the full bracket distribution with the Kalshi mid by Brier and
log loss; do not select a city or threshold on the final test period.

### Session WX-4: execution and activation

Replay maker/taker orders against recorded depth. Report fill rate, latency and post-fill markout.
Only city/lead-time cells with both forecast improvement and positive executable results may move
to manual. Auto remains a later, separately budgeted step.

## Microstructure data now collected

Each cycle prioritizes books for positions and pending orders, then fills the remaining request
budget with the most liquid watched Sports and Weather markets that have a model fair. A row is
stored on every book change and at least once per minute when unchanged. The heartbeat makes a
stable book distinguishable from a dead feed. This is the dataset needed for queue, fill and
adverse-selection tests; it is not itself an edge.

## Graduation rule

No model is activated because it made money on one day or displayed a 3–5 cent edge. A strategy
graduates only when a frozen, point-in-time, out-of-sample comparison beats the market probability
and the execution replay is profitable after fees, latency, missed fills and selection. The first
live stage uses manual approval and a small category budget. The scorecard continues comparing the
model with the market after activation and sends it back to research when the evidence deteriorates.
