# What the 15-minute commodity contracts settle on

*Verified against Kalshi's production API on 2026-09-11 (session 16). Every fact below was read
from `api.elections.kalshi.com`, not from documentation or memory. Where something is not verified,
it says so, and the model treats it as unverified: RESEARCH, never MANUAL.*

## The four series

| series | title | frequency | settlement source (Kalshi's own field) | Pyth symbol in the source URL |
|---|---|---|---|---|
| `KXGOLD15M` | Gold 15-minute | `fifteen_min` | Pyth - Gold | `Metal.Index.GOLD/USD` |
| `KXSILVER15M` | Silver 15-minute | `fifteen_min` | Pyth - Silver | `Metal.Index.SILVER/USD` |
| `KXWTI15M` | WTI 15-minute | `fifteen_min` | Pyth - WTI | `Commodities.Index.PYTHOIL/USD` |
| `KXNATGAS15M` | Natural Gas 15-minute | `fifteen_min` | Pyth - NATGAS | `Commodities.Index.NATGAS/USD` |

All four are `fee_type: quadratic`, `fee_multiplier: 1` (so a taker pays 0.07 p (1-p) a contract and
a maker pays nothing), and all four carry the same contract certification
(`COMMOD15M.pdf`). Kalshi also runs hourly (`KXGOLDH`, `KXSILVERH`, `KXWTIH`, `KXPALLADIUMH`),
daily, weekly, monthly and annual versions of the same underlyings, and 15-minute copper and
palladium. This session covers the four 15-minute series above; the model is written so another
series is configuration, not code.

## The contract

One market per event, and one event per 15-minute window. Example, read on 2026-09-11:

```
ticker                    KXGOLD15M-26SEP111900-00
event_ticker              KXGOLD15M-26SEP111900
status                    finalized
strike_type               greater_or_equal
floor_strike              4352.14
open_time                 2026-09-11T22:45:00Z
close_time                2026-09-11T23:00:00Z
expected_expiration_time  2026-09-12T04:45:00Z      <- NOT the settlement time of the window
expiration_time           2026-09-18T23:00:00Z      <- the outer limit, not the window
settlement_timer_seconds  1
result                    no
volume (24h)              117702.43
open_interest             44839.75
price_ranges              0.00-0.10 step 0.0010 | 0.10-0.90 step 0.0100 | 0.90-1.00 step 0.0010
```

`rules_primary`:

> If the close price of the 1-minute candlestick for Gold on Sep 11, 2026 at 7:00 PM EDT is at
> least the close price of the 1-minute Pyth GOLD candlestick at 6:45 PM EDT on September 11, 2026
> USD/|| Unit ||, then the market resolves to Yes.

`rules_secondary`:

> The settlement value is rounded to the nearest 2 decimal places. When confirming the settlement
> value, note the close price for the 1-minute candlestick at a given time is the price at the end
> of the immediately preceding one-minute interval. For example, the candlestick timestamped 4:59 PM
> reflects trading during 4:59:00 PM to 4:59:59 PM and closes at 5:00:00 PM. If no data is published
> by the specified source agency for the specified time, then the most recently available published
> data will be used to resolve the market.

WTI and natural gas carry the same sentence with "Pyth PYTHOIL" and "Pyth NATGAS" in place of
"Pyth GOLD".

### What that means, precisely

- **State variable.** `S_t` is the Pyth index value for that underlying. Nothing else settles these
  contracts: a futures price, a spot quote or another venue's index is a cross-check, never the
  state variable.
- **Terminal value.** `S_T` is the close of the 1-minute candle stamped at `close_time`, which by
  the rules is the last published value in the minute ending at `close_time`. In effect it is the
  reference value as of `close_time` exactly.
- **Strike.** `K = floor_strike`, and Kalshi sets it to the same construct at `open_time`: the close
  of the 1-minute candle at the start of the window. So the contract asks one question: is the
  reference higher at the end of the window than it was at the start. The strike is published, so
  the model never has to derive it; the desk recomputes it anyway from its own recording as a
  continuous check that it is reading the same feed Kalshi settles on
  (`strike_replication_error` in the quote's diagnostics).
- **Tie.** `strike_type` is `greater_or_equal`: `S_T >= K` resolves YES. This is not a rounding
  detail that can be waved away. The strike is a published price with the same granularity as the
  feed, so `S_T == K` happens, and it pays YES.
- **Rounding.** The gold rules say the settlement value is rounded to 2 decimals; the published
  strikes are 2 decimals for gold (4352.14) and WTI (96.37) and 5 for natural gas (2.94379). The
  payoff object therefore carries the tie rule and the comparison is made on the values as
  published, without re-rounding.
- **Missing data.** If the source publishes nothing for the minute, the most recent published value
  is used. A feed gap does not void the contract, so a gap in our own recording is a reason not to
  trade, not a reason to expect a void.
- **Timing.** `settlement_timer_seconds` is 1: the result follows the close by about a second.
  `expected_expiration_time` is about 5h45m after the close and is not the window's settlement time.
  **Anything that needs the clock for these markets must use `close_time`,** which is what the model
  and the time-to-expiry in every diagnostic use.

## The reference feed

Kalshi publishes the same Pyth values it settles on over its own authenticated WebSocket, channel
`pyth_value`:

```
wss://external-api-ws.kalshi.com/trade-api/ws/v2          (demo: external-api-ws.demo.kalshi.co)
signing path: "/trade-api/ws/v2", message = timestamp_ms + "GET" + path, RSA-PSS SHA-256, base64
subscribe:   {"id":1,"cmd":"subscribe","params":{"channels":["pyth_value"],"underlying_tickers":["all"]}}
message:     {"type":"pyth_value","sid":N,"seq":N,"msg":{"underlying_ticker":...,"value_usd":"...",
              "source_ts_ms":...,"received_at":...}}
```

`value_usd` is documented as a string to 8 decimal places, `source_ts_ms` as the Pyth source
timestamp in unix milliseconds and `received_at` as Kalshi's own receipt time. Duplicate and
out-of-order source timestamps are ignored per underlying. The channel requires authentication, and
the same key the desk already trades with is enough.

Two things about this are deliberately not assumed:

1. **The exact `underlying_ticker` strings.** The documented examples are `Metal.XAU/USD` and
   `Metal.XAG/USD`, while the settlement source URLs above name `Metal.Index.GOLD/USD`,
   `Metal.Index.SILVER/USD`, `Commodities.Index.PYTHOIL/USD` and `Commodities.Index.NATGAS/USD`.
   The desk therefore subscribes with `["all"]`, records every underlying it receives, and maps a
   series to an underlying by pattern (configurable in `config/desk.json` under
   `params.commodity.underlyings`). The first message for each underlying is logged, so the live
   strings are in the log the first time the feed runs.
2. **That the feed we read is the feed Kalshi settles on.** The desk checks it continuously rather
   than believing it: at each window open it compares Kalshi's published `floor_strike` with the
   1-minute candle close it computed from its own recording. A model whose replication error is
   larger than a tick, or which has not replicated a strike recently, reports
   `settlement_verified = false`, and the router then routes it RESEARCH. That check is the reason
   these models can be MANUAL at all.

   The check is run against the window being priced, newest first, and windows whose close has
   passed are skipped. This matters more than it sounds: the store keeps a 15-minute window listed
   as open until a reconcile catches up, and it hands markets over in ticker order, so the list
   normally *begins* with a window that has already settled. A check that ran on the first row it
   found and then recorded "done" would confirm a dead window and leave the live one unchecked,
   while the dashboard said "settlement confirmed". The replication record therefore carries the
   window it checked (`window_open`), not just the clock time of the check.

## What is recorded, and why the minute close is special

Every observation the socket delivers goes into `commodity_reference` (append-only:
`ts_received`, `ts_exchange`, `symbol`, `source`, `price`, `sequence`, `raw_json`), sampled to at
most one row per symbol per `record_min_interval_ms` (1 s by default) — **plus**, always, the last
observation of each minute. That last one is the candle close, which is the number the contract
settles on and the number the strike is drawn from, and which observation it is cannot be known
until the next minute begins. So it is held back and written the moment a tick from a later minute
proves it was the last. Sampling alone would keep whichever tick happened to land on the sampler's
clock, which is a different value, and a replay recomputing the settlement candle from the table
would then disagree with the live check above.

## The other three series, and what the exchange publishes about all of them

Read off each series' own `settlement_sources` on 2026-09-12, so these are the exchange's
spellings rather than a guess:

| series | Pyth underlying | settlement rounding | open windows on 2026-09-12 |
|---|---|---|---|
| KXGOLD15M | `Metal.Index.GOLD/USD` | 2 dp | yes |
| KXSILVER15M | `Metal.Index.SILVER/USD` | 3 dp | yes |
| KXWTI15M | `Commodities.Index.PYTHOIL/USD` | 2 dp | yes |
| KXNATGAS15M | `Commodities.Index.NATGAS/USD` | 5 dp | yes |
| KXCOPPER15M | `Commodities.Index.CU/USD` | 5 dp | yes |
| KXPLATINUM15M | `Metal.XPT/USD` | — | none |
| KXPALLADIUM15M | `Metal.XPD/USD` | — | none |

Two things to notice. Platinum and palladium settle on the **plain metal feeds**, not on `.Index.`
ones, so a pattern written for the other five would not have found them. And the rounding differs
by four orders of magnitude across the set, which is why the desk reads the tolerance out of each
contract instead of keeping a table of constants: `rules_secondary` says "The settlement value is
rounded to the nearest N decimal places", and one unit in that last place is what "within a tick"
means on that underlying.

The candle convention is stated in the same block, and it is exactly the half-open window this
desk implements:

> the close price for the 1-minute candlestick at a given time is the price at the end of the
> immediately preceding one-minute interval. For example, the candlestick timestamped 4:59 PM
> reflects trading during 4:59:00 PM to 4:59:59 PM and closes at 5:00:00 PM.

So does the rule for missing data:

> If no data is published by the specified source agency for the specified time, then the most
> recently available published data will be used to resolve the market.

A carried-forward value is therefore the settlement rule, not a fault. It is still recorded as a
distinct state (see below), because it is not a new observation and a volatility estimate that
counts it as one is reading silence as a market that stopped moving.

## What the feed carries, and what it does not

Kalshi's `pyth_value` payload is four fields, per the exchange's own AsyncAPI:

```
underlying_ticker   value_usd (string, 8 dp)   source_ts_ms   received_at
```

There is no confidence interval, no exponent, no publisher count, no feed-update timestamp and no
market-session field on this channel. Those exist on **Pyth's own** API (Hermes), which is a
different stream from the one these contracts settle against, and whose live price endpoint now
requires a key. Anything built on them would be a second source, and would need the same rule as
the futures cross-check: informative, never the settlement value.

The feeds are all scheduled 24/7 (`America/New_York;O,O,O,O,O,O,O`) at fixed publication rates -
gold and silver every 200 ms, WTI, natural gas and copper every 50 ms. There is consequently no
session field to key a regime off; sessions have to be measured from what the data does.

Because the stream is **deduplicated**, an unchanged price arrives as nothing at all. Staleness on
this feed is therefore a gap, not a repeated message, and the desk judges it against each
symbol's own measured cadence rather than a configured number: a gap longer than eight times the
median inter-arrival (floor two seconds) marks the observation that ends it as `fresh = 0`, and
`commodity_reference` carries both `gap_ms` and `fresh` on every row.

## Asking the channel what it carries

The subscription supports an `underlying_list` action, which returns every underlying streamed in
the last two hours. The desk sends it on every connect and records the answer, so the mapping from
series to feed symbol is the venue's own statement rather than a pattern match against a guess.
The pattern list is still there, with the exact published symbol first, as the fallback.

## Status of verification

| item | status |
|---|---|
| series, settlement source field, fee schedule | verified from the API |
| contract rules text, tie rule, rounding sentence | verified from the API |
| one market per 15-minute event, strike = open reference | verified from the API |
| close_time is the settlement clock, not expected_expiration_time | verified from the API |
| `pyth_value` channel, auth, subscribe shape, message fields | from Kalshi's AsyncAPI spec; the live message shape is recorded and logged on first run |
| exact `underlying_ticker` per series | published per series in `settlement_sources` (table above); also asked of the channel at runtime, and confirmed by strike replication |
| `pyth_value` carries only the four fields above | verified from the AsyncAPI schema |
| all five feeds are 24/7 at a fixed 50-200 ms rate | verified from Pyth's feed metadata |
| settlement rounding per commodity | verified from each contract's rules_secondary; the desk reads it rather than storing it |
| that our recorded feed reproduces Kalshi's strike | checked continuously at runtime, against the window being priced; until it passes, the model stays RESEARCH |

## One piece of exchange metadata that is wrong for these series

`expected_expiration_time` on a 15-minute window is roughly six hours after the window closes, and
`expiration_time` is a week later. Neither is when the contract settles: `settlement_timer_seconds`
is 1, so the result follows `close_time` by about a second. The model reads `close_time` and
nothing else, and the Trades board shows the model's own clock rather than the desk's generic
"settles in" column for these rows. The desk's shared `expected_settlement_ts()` still returns the
six-hour number, which is inside every horizon gate and so changes no decision today — but it is
wrong, and a later session that wants it right should parse `settlement_timer_seconds` from the
market payload and prefer `close_time + timer` when the exchange states one.
