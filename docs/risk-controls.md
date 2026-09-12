# Risk controls and central risk book

The dashboard now has a **Central risk book** tab. It shows cash, marked position value,
committed purchase cost, exchange-shard balances, and each holding's normal YES/NO payoff.
The reduction calculator is a preview only: it never submits an order, moves funds, or sets a stop.
It assumes the entered price and fee; execution and available depth are not guaranteed.

## Entry path

Signals (manual/auto/bot) and manual tickets share the same pre-submission checks:

- Require an authenticated, successfully refreshed account snapshot.
- Wait for exchange user data to catch up to this desk's previous order write. A refusal that
  Kalshi itself answered (a 4xx with its error body, not a 408) wrote nothing, so the previous
  watermark is put back.
- Require a tradable market, valid side/style, finite price, and quantity in 0.01 increments.
- Enforce the configured price range, per-event, per-cluster, total-exposure and cash limits.
- Estimate fees using refreshed series metadata.
- Check collateral available on the specific market's exchange shard after resting reservations.
- Refuse a post-only price at or through the offer just read from the exchange, before sending:
  the book has moved, and the exchange would refuse it as a post-only cross.
- Never retry POST requests automatically.

Exchange refusals are classed (`include/desk/reject.h`). Only a malformed order
(`hard_invalid_order`), an auth failure (`auth`) or a refusal the desk does not recognise
(`unknown`) trips the kill switch. A post-only cross or a closed or paused market
(`soft_market_state`) and a short balance (`soft_collateral`) mark the order not sent and give
entries on that ticker a breather; 429 and 5xx (`retryable`) stop sending for the cycle. A
refused exit never trips the kill switch. The phrases are matched exactly on purpose, so an
unfamiliar refusal stays hard.

The sizing bankroll is the smaller of configured bankroll and cash plus marked positions.
The current production limits have not been increased. They remain configuration ceilings,
not recommended stake sizes or a guarantee against losses. Fractional manual orders are supported;
strategy proposals continue to choose whole contracts.

## Conservative payoff accounting (reviewed 2026-09-06)

The book evaluates YES and NO payouts separately for each market. It does not assume that all
markets in an event are mutually exclusive, or that a local list covers every outcome.
Different thresholds can resolve YES together. Until settlement relationships are verified,
cross-market hedges receive no credit. Filled opposite outcomes on the exact same ticker can
offset. Resting orders and pending proposals are optional fills: the calculation includes the
adverse fill subset, so an unfilled hedge never releases risk capacity.

Strategy proposals that add loss in an existing worst scenario are held back. Manual tickets
can bypass that strategy selection rule, but still obey the same event, cluster, total and
cash caps. Pending reservations are updated as proposals are generated within each cycle.

## Exposure and daily stop

Filled purchase cost and resting-order purchase cost (plus estimated resting fees) are shown
separately from the conservative payoff risk used for caps. Unsent proposals are displayed separately; the strategy scheduler reserves
space for its queue without labeling that queue as committed capital. All positions, orders,
and proposals use the same event/cluster mapping, including weather grouped by event date.
There is no assumed hedge credit between different markets.

Daily equity is reported cash plus marked position value. Reported cash already includes resting
reservations: the recorded September 5 cash movement from $100.2273 to $98.2732 matches exactly
$1.9541 of fills and fees while other orders remained unfilled. Adding resting cost to equity
would double-count it. Entry cash checks instead subtract those reservations, including on the
specific exchange shard. The baseline is the first positive combined
account snapshot of the UTC day. The migration deliberately ignores the previous positions-only
zero baseline. External deposits and withdrawals are not adjusted and therefore affect this
measure; it is not a cash-flow-adjusted performance return. The daily stop blocks new entries; it does not liquidate positions. It is polled and cannot
run while the computer or service is offline.

Account reads are applied together in a SQLite transaction. Resting orders are fetched before
positions so a fill during those reads can be conservatively counted twice rather than omitted.
The API does not provide an atomic portfolio snapshot; external concurrent trading can still
change the account between reads and submission. Exchange collateral checks are the final limit.

## Exchange shards

Market exchange_index is authoritative. Entries and closes target that shard explicitly.
Cancels carry market_ticker and exchange_index=-1 for automatic routing. Account cash on shard 0
does not fund a crypto order on shard 2. Allocate collateral separately at
https://kalshi.com/account/exchange-indexes. The desk never transfers funds automatically.

## Closing and fractional fills

Closing keeps two decimal places and sends a reduce-only immediate-or-cancel limit order at
the current touch. Partial execution remains possible. Remaining orders in that market must
be canceled first to avoid reopening the position. Closing stays available when entry risk
limits or the kill switch prohibit new entries. Check positions again after the close.

## Targets, stops and model exits

Targets, stops and model exits all use the same reduce-only submission path. They refresh the
held position, clamp the quantity to that position and cannot open or flip a position. Invalid
or stale proposals are rejected. An uncertain response stays blocked until the order is found;
a timer does not establish that the exchange rejected it.

AUTO off prevents automated model entries and exits. A position bracket that you explicitly
set to automatic runs independently of that switch. Alert mode sends no order. Turning a bracket
to alert mode or removing it cancels its owned target/stop orders before changing the setting.
Model exits require a current, eligible fair and cannot use a research-only signal.

A take-profit is a resting limit exit once accepted by the exchange. Stops and trailing stops
are local triggers: the desk must be running, connected and receive a usable quote. Triggered
IOC orders can partially fill or fail to execute. A stop price is not a guaranteed sale price.
No bracket or automatic trading mode is enabled by this code review.

## Weather validation status

The live weather contracts inspected on September 5, 2026 name The Weather Company as their
settlement source. The existing strategy uses NWS forecasts/observations and calibration from
NWS-style historical data. That is not a validated mapping to The Weather Company settlement.

Forecasts continue to be recorded for research. New weather-model proposals with an unsupported
or unknown settlement source are blocked and pending proposals are expired. This does not close
existing positions or authorize discretionary manual tickets. A source matching NWS is necessary
for this implementation but does not establish predictive accuracy or profitability.

Validation still requires final Weather Company outcomes, station/date mapping, chronological
out-of-sample forecasts, calibration/Brier comparisons against market prices, and returns after
fees and spreads. Until those data exist, no validated Weather Company trading model is claimed.

Official references:
- https://docs.kalshi.com/getting_started/exchange_sharding
- https://docs.kalshi.com/api-reference/orders/create-order-v2
- https://docs.kalshi.com/api-reference/orders/cancel-order-v2
- https://docs.kalshi.com/api-reference/exchange/get-user-data-timestamp
