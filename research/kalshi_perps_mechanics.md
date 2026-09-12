# Kalshi perpetual futures: verified mechanics

Read from official documentation on 2026-09-11. Every claim below is either quoted from
docs.kalshi.com / help.kalshi.com, or marked **not disclosed**. Where the documentation is silent
the answer has to come from the API itself, and the endpoint that gives it is named.

## The headline: there IS historical data

The project brief assumed "Kalshi currently does not expose historical endpoints for margin/perps.
Therefore immediately construct a 24/7 recorder." That is not correct. Three historical endpoints
exist, and they change the critical path entirely:

| endpoint | gives |
|---|---|
| `GET /margin/markets/{ticker}/candlesticks` | 1m / 1h / 1d OHLC of **bid**, **ask** and **trade price**, plus volume, notional volume and **open interest**. See the synchrony caveat below. |
| `GET /margin/trades` | every public print with `taker_side`, cursor-paged, `min_ts` / `max_ts` |
| `GET /margin/funding_rates/historical` | every funding application with the mark at that time |

### What the candlesticks do and do not give you

An earlier version of this document said separate bid and ask OHLC means "a spread and a mid can be
reconstructed per minute historically". **That is wrong and the error matters.** `bid.high` and
`ask.low` inside a bar need not have occurred at the same instant, so differencing the extrema
produces a spread that may never have existed. Differencing `bid.low` and `ask.high` likewise
manufactures a spread wider than anything quoted.

What is legitimate:

| usable | why |
|---|---|
| `ask.close - bid.close`, called **`close_quote_gap_bps`** and never "spread" | nothing in the API guarantees the last observed bid and the last observed ask inside a minute share a timestamp, so this is a **coarse liquidity proxy**, not an instantaneous spread, and it must never be used as an execution cost |
| `(bid.close + ask.close) / 2` | bar-end midpoint, the correct target for a return label |
| minute mid-to-mid returns, realized vol | built from bar-end mids |
| `volume`, `volume_notional_value_dollars`, `open_interest` | exact, no synchrony problem |
| coarse liquidity statistics (median, quantiles, by hour) | from the close quote gap, treated as a proxy and not a path |

The live L2 recorder is the only source of a contemporaneous spread. What is **not** available
from 1-minute bars, at any level of cleverness: intraminute spread paths,
queue position, orderbook imbalance, cancel vs trade dynamics, adverse-selection measurement, or any
market-making fill simulation. Those need the tick recorder, and no amount of OHLC substitutes.

So both are required, for different questions. The backfill starts the 1-minute-and-up research
today; the recorder starts the 1-second-to-1-minute research from whenever it is switched on, which
is why it should be switched on now rather than after the backfill.
`scripts/backfill_perps_history.py`, `scripts/perps_recorder.py`.

## Contract structure

- Prices are **dollars per contract**. `contract_size` is the underlying per contract, *not* a
  notional multiplier: KXVVVPERP marks $2.4426 with `contract_size` 0.1, so one contract is 0.1 VVV
  and the token is $24.43. **Notional = count x price.** (Verified against live marks across all 20
  instruments; getting this wrong by a factor of `contract_size` is an easy and expensive mistake.)
- Tick size: 0.0001 dollars on every instrument observed. Not separately documented.
- Contracts trade in whole units unless the market's `fractional` flag is set; none of the 20
  currently listed are fractional.
- Minimum order size: **not disclosed**; the desk's own rail refuses under 0.01 contracts.

## Margin

- The perps account is **segregated** from the predictions balance: "Your perpetuals margin account
  is a separate account from your Kalshi predictions balance." Cash moves between them through
  `POST /margin/intra_account_transfer`.
- Margin held at the clearinghouse **earns interest**, "approximately 3.25% APY" as documented. This
  is a real and often-forgotten component of carry on idle collateral.
- Initial margin: per-market, from `GET /margin/risk_parameters` -> `initial_margin_multiplier`, a
  map of ticker to multiplier applied to maintenance margin. No fixed percentage is published.
- Maintenance margin: "typically around 90% of initial margin."
- Liquidation is a **two-tier** process, both thresholds from the same endpoint:
  `queue_entry_margin_ratio_threshold` (enters the liquidation queue) then
  `liquidation_margin_ratio_threshold` (liquidated). Treating liquidation as a single trigger
  mis-states where the risk actually starts.
- Maximum leverage varies by asset, published examples: BTC 5.8x, ETH 4.4x, LINK 3.4x, XRP 2.7x,
  SOL 2.6x, HYPE 2.1x. The live `/margin/markets` response carries a `leverage` field per market,
  which is the number to use.
- Isolated vs cross: both exist, with separate exit-trigger endpoints
  (`/margin/exit_triggers/isolated/*` and `/margin/exit_triggers/cross/*`).

## Funding

- Settles **every eight hours, at 00:00, 08:00 and 16:00 ET**.
- Two components: "a small, steady baseline set by the platform (the interest rate), and a variable
  amount that grows or shrinks based on the gap between the contract price and spot." The exact
  formula is **not disclosed**.
- **Capped at 2% of position per 8-hour window.**
- Live check, 2026-09-11: the funding rate is **zero on 18 of 20 instruments**, BTC the only
  materially nonzero one at 0.00017. There is no carry trade here at present.

## Fees

- `GET /margin/fee_tier_rates` returns `{fee_schedule, tier, maker_fee_rate, taker_fee_rate}`, rates
  as "decimal fraction of notional (e.g. 0.0005 = 0.05% = 5 bps)".
- Actual rates are **account-specific**; read them, never assume. The desk's fallback when the tier
  is unknown is 5 bps maker / 12 bps taker, which is what this account has been observed at.
- **A round trip is therefore ~17 bps of notional** (maker in, taker out). Every backtest number in
  this project is reported net of that.

## Market data: REST

Base `https://external-api.kalshi.com/trade-api/v2` (demo: `external-api.demo.kalshi.co`).
Auth is RSA-PSS (SHA-256, digest salt length) over `timestamp_ms + METHOD + path`, where `path`
**includes** the `/trade-api/v2` prefix, sent as `KALSHI-ACCESS-KEY` / `-TIMESTAMP` / `-SIGNATURE`.

`/margin/markets`, `/margin/markets/{t}`, `/margin/markets/{t}/orderbook`,
`/margin/markets/{t}/candlesticks`, `/margin/trades`, `/margin/balance`, `/margin/positions`,
`/margin/orders`, `/margin/fills`, `/margin/risk`, `/margin/risk_parameters`,
`/margin/notional_risk_limit`, `/margin/fee_tiers`, `/margin/fee_tier_rates`,
`/margin/funding_rates/{estimate,historical}`, `/margin/funding_history`,
`/margin/order_groups/*`, `/margin/exit_triggers/*`, `/margin/enabled`, `/margin/exchange_status`,
`/margin/account_api_limits`.

## Market data: WebSocket

**The documentation is wrong about the margin socket.** It gives
`wss://external-api-margin-ws.kalshi.com/` with `address: /`, and that combination answers
**HTTP 404** in production. Probed with `scripts/ws_probe.py`, what actually works:

| socket | URL | signed path |
|---|---|---|
| perps / margin | `wss://external-api-margin-ws.kalshi.com/trade-api/ws/v2/margin` | `/trade-api/ws/v2/margin` |
| CF Benchmarks index | `wss://external-api-ws.kalshi.com/trade-api/ws/v2` | `/trade-api/ws/v2` |

The host is the documented one; the path is not. `address: /` as documented answers **404**, and
the working path is `/trade-api/ws/v2/margin`. Each socket signs its own path.

**There is a trap here worth stating plainly.** Subscribing to perps on the main elections socket
*appears* to work: it acknowledges the subscribe and sends `orderbook_snapshot` frames. Those
frames contain no `bid`, no `ask`, and an empty `market_id`. A probe that checks only whether a
frame arrived will call that success and point a recorder at a socket that serves no perp book.
Check the payload, not the envelope.

Confirmed working payload from the real socket:

```json
{"type":"ticker","sid":1,"msg":{"market_ticker":"KXBTCPERP","price":"7.9041",
 "bid":"7.9042","ask":"7.9043","bid_size_fp":"316.00","ask_size_fp":"..."}}
```

So `ticker` carries top of book **with sizes**, which is L1 depth for free alongside
`orderbook_delta`.

Old documented values, kept because they are what the docs still say:
`wss://external-api-margin-ws.kalshi.com/`, demo `wss://external-api-margin-ws.demo.kalshi.co/`.

```json
{"id": 1, "cmd": "subscribe",
 "params": {"channels": ["orderbook_delta"], "market_tickers": ["KXBTCPERP"],
            "send_initial_snapshot": true}}
```

**The complete channel list is six:** `orderbook_delta`, `ticker`, `trade`, `fill`, `user_orders`,
`order_group_updates`.

**What is NOT a margin WebSocket channel**, contrary to the brief's Phase 2 list: funding rate,
reference/index price, settlement mark price, liquidation mark price. Those are REST only, or come
from the *event-contract* WebSocket (`cfbenchmarks-value`, `cfbenchmarks-value-5hz`, `pyth-value`),
which is a different socket. The recorder must poll `/margin/markets` for mark, reference and
liquidation mark, and subscribe to the event-contract socket separately for a faster index tick.
`cfbenchmarks_value_5hz` at 5 Hz is the fastest index source available and is what any lead-lag
study below one second depends on. Its subscribe parameter is **`index_ids`** and the values are
**CF Benchmarks index IDs** (`BRTI`, `ETHUSD_RTI`), not Kalshi tickers. The payload:

```json
{"type":"cfbenchmarks_value_5hz","sid":1,"seq":42,
 "msg":{"index_id":"BRTI","value_usd":"68000.12000000",
        "source_ts_ms":1710000000323,"received_at":1710000000341}}
```

Both timestamps are **milliseconds**. `received_at` is Kalshi's own receipt time for the tick, so
`received_at - source_ts_ms` is an upstream latency measured entirely on their clock -- the one
CF-to-Kalshi timing number that does not depend on ours being right, and worth recording
separately from our own receive latency.

**CF Benchmarks is the contract's reference anchor, not its fair value.** It is what the perps
settle and fund against, which makes `basis_cf = kalshi_mid - cf_index` the right quantity to
study -- but funding expectations, order flow, inventory pressure and latency all drive a real and
possibly persistent basis, so CF is not the price Kalshi "should" trade at. CF is also itself
downstream of somewhere. The chain worth measuring is three-layer:

```
external venues  ->  CF Benchmarks  ->  Kalshi
```

The interesting result would be Binance leading CF by ~300ms and CF leading Kalshi by ~1.5s, and
that is invisible if CF is treated as the terminal predictor. It is why the recorder carries
external feeds for BTC and ETH, where Kalshi's own basis is far too small to trade: they are there
to test who leads CF, not only who leads Kalshi.

**Clock quality is part of the measurement.** A monotonic clock only catches our own clock
stepping; it cannot tell us the wall clock is 37ms wrong relative to everyone else, which would
show up as a fake lead-lag. The recorder writes periodic NTP offset and round-trip estimates into
the stream. For cross-venue work, prefer differences of **receive** timestamps measured by one
machine over differences of two exchanges' own clocks -- never trust a lead that exists only
because two venues disagree about the time.

Orderbook messages:

```json
{"type":"orderbook_snapshot","sid":123,"seq":123,
 "msg":{"market_ticker":"...","bid":[["<price>","<qty>"]],"ask":[["<price>","<qty>"]]}}

{"type":"orderbook_delta","sid":123,"seq":123,
 "msg":{"market_ticker":"...","price":"...","delta":"...","side":"bid|ask",
        "last_update_reason":"Decrease|Amend|MarginCancel|SelfTradeCancel|ExpiryCancel|Trade|PostOnlyCrossCancel",
        "client_order_id":"...","subaccount":123,"ts_ms":123}}
```

Prices are dollar strings, quantities fixed-point contract-count strings, timestamps Unix ms.
`seq` is per-subscription and monotonic: a gap means a missed update and the book must be
resnapshotted, not patched. `last_update_reason` is unusually informative -- it separates a
`Trade` from a `Decrease` from a cancel, which is exactly the cancel-vs-trade distinction Phase 5
Strategy B needs and which most venues do not give away.

**All margin WebSocket timestamps are Unix epoch milliseconds with an `_ms` suffix.** Event-contract
sockets use seconds. Mixing them silently produces timestamps 1000x apart.

## Rate limits

Perps traffic is metered in **its own read and write buckets**: "Perps calls do not draw down your
event-contract budgets, and event-contract calls do not draw down your perps budgets." This matters
-- the recorder and backfill cannot starve the live event-contract desk.

Budgets are token buckets at 10 tokens per request. Basic tier: 200 tokens/s read, 100 write, so
~20 reads/s and ~10 writes/s, with some buckets holding up to two seconds of budget for bursting.
This account's tier: read from `GET /margin/account_api_limits`.

## Production access

Confirmed, not by the documented `GET /margin/enabled` but by something stronger: this account is
already reading `/margin/balance`, `/margin/positions`, `/margin/orders`, `/margin/fills`,
`/margin/risk` and `/margin/fee_tiers` in production and holding a real balance on the margined
instance. Production Perps API access is live.

## Trading hours and maintenance

Perps trade **24/7**; Kalshi's gold and silver perps were launched explicitly on "round-the-clock
trading". Scheduled maintenance windows are **not disclosed** in the documentation; the recorder
should treat any gap as an outage, log it, and resnapshot rather than assume continuity.

## Order types

`/margin/orders` supports limit orders with `post_only` and time-in-force
(`good_till_canceled`, `immediate_or_cancel`), plus `reduce_only`. Amend and decrease are separate
endpoints (`/margin/orders/{id}/amend`, `/decrease`). Stop-loss and take-profit are **not** order
types: they are **exit triggers**, set per position through `/margin/exit_triggers/isolated/*` or
`/margin/exit_triggers/cross/*`, and **order groups** (`/margin/order_groups/*`) provide
contingent/OCO behaviour. The desk currently implements neither and does its own exits in software,
which means its stops are only as reliable as its own uptime. Moving stops to exchange-side exit
triggers is a real robustness improvement and is the one piece of Phase 10 that can be done today.

There is also a **FIX API** for margin (`docs.kalshi.com/fix-margin/*`) with drop-copy and listener
sessions -- relevant only if latency ever becomes the binding constraint, which at $50 it is not.

## Price banding

`docs.kalshi.com/margin/price-banding` documents exchange-side rejection of orders too far from the
mark. The desk enforces its own 2% band before sending; the exchange's own band is the real one and
its parameters should be read rather than guessed.

## Probed against production, 2026-09-11

`scripts/perps_api_probe.py` against the live account. **14 of 16 endpoints returned 200.**

Confirmed working: `/margin/markets`, `/margin/markets/{t}`, `/margin/markets/{t}/orderbook`,
`/margin/markets/{t}/candlesticks`, `/margin/trades`, `/margin/funding_rates/historical`,
`/margin/funding_rates/estimate`, `/margin/balance`, `/margin/positions`, `/margin/risk`,
`/margin/risk_parameters`, `/margin/fee_tiers`, `/margin/fee_tier_rates`, `/margin/enabled`.

**Two documented endpoints do not exist: `/margin/exchange_status` and
`/margin/account_api_limits`, both 404.** So trading hours and this account's rate-limit tier
cannot be read from the API and have to be inferred from behaviour. This is exactly why the probe
exists.

`/margin/trades` pagination is sound: three pages, 300 ids, 300 unique, cursor advances and does
not loop. `taker_side` is present on every print, so signed trade imbalance is reconstructable
from history rather than needing the recorder.

No rate-limit headers are returned on any endpoint.

### History depth is the binding constraint

The first probe measured depth on `KXAAVEPERP` -- alphabetically first, and **not** one of the
launch contracts -- and found history running out between 8 and 16 days. That number is about
AAVE, not about the venue, and reporting it as the venue's depth would have been the same class of
error as the spread claim. The probe now binary-searches the boundary **per contract** (exact,
worst case 17 calls each), because Kalshi listed BTC, ETH, SOL, XRP, DOGE, LINK and DOT in early
June while gold and silver listed 2026-09-10.

Until that runs, assume the launch cohort has the most history and everything else has weeks or
days. `--days 90` remains a request, not a fact.

## What is still unknown, and how to find out

| unknown | how |
|---|---|
| this account's actual fee tier | `GET /margin/fee_tier_rates` -- confirmed working, 6 rows |
| initial-margin multiplier per market | `GET /margin/risk_parameters` |
| both liquidation thresholds | same endpoint |
| this account's rate-limit tier | **not available: that endpoint 404s.** Infer from 429 behaviour under load |
| exact funding formula | not published; fit it from `/margin/funding_rates/historical` against basis |
| how far candlestick history reaches per contract | empirical, and it differs a lot: Kalshi launched perps in late May 2026 with BTC, ETH, SOL, XRP, DOGE, LINK and DOT listed early June, so those have ~3 months by September. Gold and silver only listed 2026-09-10. `--days 90` does not mean 90 days exist; `reports/perps_data_audit.md` measures what actually arrived. |
| WebSocket subscription limits | not published; find by increasing subscriptions until refused |
| exchange price-band width | `docs.kalshi.com/margin/price-banding`, then confirm against a rejection |

Sources: [Perps API](https://docs.kalshi.com/margin), [margin WebSocket](https://docs.kalshi.com/margin-ws/websockets/websocket-connection.md), [orderbook updates](https://docs.kalshi.com/margin-ws/websockets/orderbook-updates.md), [candlesticks](https://docs.kalshi.com/margin-rest/market/get-market-candlesticks.md), [trades](https://docs.kalshi.com/margin-rest/market/get-trades.md), [historical funding](https://docs.kalshi.com/margin-rest/funding/get-historical-funding-rates.md), [fee tier rates](https://docs.kalshi.com/margin-rest/fees/get-fee-tier-rates.md), [risk parameters](https://docs.kalshi.com/margin-rest/risk/get-risk-parameters.md), [rate limits](https://docs.kalshi.com/getting_started/rate_limits.md), [Perps explained](https://kalshi.com/perpetuals/learn), [margin account](https://help.kalshi.com/en/articles/15357608-your-perpetuals-margin-account)
