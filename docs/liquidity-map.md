# Where the liquidity actually is

Measured from Kalshi's public market API on **2026-09-10, 01:07-01:15 UTC**. This is one
snapshot on a Wednesday night; it is a starting map, not a claim about any other hour.

## Read this first: an earlier measurement here was wrong

On 2026-09-06 this desk's notes said the sports totals series had "zero two-sided quotes, zero
volume, zero open interest", and a commodity check reported the same. That was a bug in the
measuring script, not a fact about the exchange. **Kalshi renamed the market fields**: `yes_bid`
became `yes_bid_dollars` (dollars, not cents), `volume` became `volume_fp`, `open_interest`
became `open_interest_fp`, `yes_bid_size` became `yes_bid_size_fp`. A reader still asking for the
old names gets `null` for every one of them and, if it treats null as zero, concludes the market
is dead. Every number below was re-read with the current names.

The lesson generalizes past this table: a measurement that reports exactly zero across an entire
category is more likely to be a broken reader than an empty exchange, and should be checked
against a market known to trade before anything is concluded from it.

## Sports

| series | markets | two-sided | mean spread | volume | open interest | bid size |
|---|---:|---:|---:|---:|---:|---:|
| KXNFLGAME | 64 | 64 | **1.3c** | 27,021,163 | 20,003,266 | 2,793,437 |
| KXMLBGAME | 88 | 88 | 2.9c | 14,544,022 | 7,899,076 | 1,324,499 |
| KXNCAAFGAME | 478 | 450 | 13.3c | 7,068,938 | 6,543,388 | 1,997,779 |
| KXMLSGAME | 135 | 133 | 22.2c | 4,701,360 | 3,218,772 | 479,537 |
| KXEPLGAME | 60 | 60 | 1.9c | 552,352 | 476,945 | 162,378 |
| KXNFLTOTAL | 306 | 303 | 2.1c | 3,729,305 | 2,855,386 | 1,946,959 |
| KXMLBTOTAL | 130 | 125 | 2.3c | 3,218,569 | 2,359,547 | 555,343 |
| KXNCAAFTOTAL | 600 | 600 | **58.9c** | 6,624 | 6,411 | 98,730 |
| KXNBATOTAL | 0 | - | - | - | - | off-season |

Totals markets are real and heavily traded. The earlier note claiming otherwise was wrong.

Two structural facts decide what is buildable in each row:

1. **The fee is the hurdle, not the spread.** A taker pays `7% x p(1-p)` a contract, which is
   1.75c at p=0.5. NFL game markets are 1.3c wide. Taking there requires being right by more
   than the whole spread plus the fee, against the most-watched line on the exchange.
2. **Sports series charge makers.** Every series in the table above is `quadratic_with_maker_fees`.
   Posting does not escape the fee here, unlike the commodity series below. This removes the one
   structural edge on this exchange that needs no forecast.

Where a model has room is the wide rows: **KXNCAAFGAME at 13.3c on 7.1M volume**, and
**KXMLSGAME at 22.2c on 4.7M volume**. A wide two-sided market with real size behind it is the
market saying it does not know. KXNCAAFTOTAL is wider still at 58.9c but carries almost no
volume (6,624), which is a different thing: nobody is trading it, so a fill is a favor from
someone who wants the other side, not a price.

## The 15-minute commodity series

All five exist, all five trade, and all five are `quadratic`: **makers pay nothing**.

| series | volume | open interest | spread | underlying |
|---|---:|---:|---:|---|
| KXGOLD15M | 155,728 | 102,896 | 0.1c - 0.6c | Pyth `Metal.Index.GOLD/USD` |
| KXSILVER15M | 43,820 | 28,598 | 0.2c | Pyth Silver |
| KXWTI15M | 41,482 | 21,727 | 0.2c | Pyth WTI |
| KXNATGAS15M | 15,790 | 5,429 | 4.0c | Pyth NATGAS |
| KXCOPPER15M | 7,457 | 3,911 | 1.0c | Pyth Copper |

Those are single markets: one 15-minute window each. Gold turning over 155,728 contracts in one
window makes it, per window, among the busiest instruments on the exchange.

**The contract.** From the live rules of `KXGOLD15M-26SEP092130-30`: *"If the close price of the
1-minute candlestick for Gold at 9:30 PM EDT is at least the close price of the 1-minute Pyth
GOLD candlestick at 9:15 PM EDT, then the market resolves to Yes."* So it is direction over the
window, referenced to two 1-minute candle closes, and **a tie resolves YES**. That last clause is
not decoration: on a quiet minute in copper, equality is a real fraction of outcomes.

**Two constraints that shape any strategy here.**

- *The fee wall.* Gold's spread is a tenth of a cent while the taker fee at p=0.5 is 1.75c. Taking
  near the middle of the window is hopeless. Fees fall as p leaves 0.5 (0.89c at p=0.85), so any
  taker trade lives late in the window when direction is already largely resolved, which is also
  when the market knows it. Posting costs nothing, which is why the maker side is the interesting
  one, but a 0.1c spread has nothing in it to capture. **Copper at 1.0c and natural gas at 4.0c
  are the rows with room**, and they are also the thinnest.
- *Settlement is hours after the close.* `KXGOLD15M-26SEP092130-30` opened 01:15 UTC, closed
  01:30 UTC, and reports expected expiration at **07:15 UTC**, roughly six hours later. Holding to
  settlement parks the cash for those six hours. On a $40 sleeve that is about four positions a
  day, not ninety-six. Exiting inside the trading window is not an optimization here, it is the
  difference between a strategy and a savings account.

**The blocker is the price feed, and it is a real one.** A 15-minute contract needs the underlying
now.

- Yahoo Finance futures (`GC=F`, `SI=F`, `HG=F`, `CL=F`, `NG=F`) were measured at **607 seconds of
  delay** on 2026-09-10. Ten minutes late on a fifteen-minute contract is not a usable feed.
- Pyth's own Hermes price endpoints return **HTTP 401** without an API key, following its
  August 2026 change. Its metadata endpoints are still open, which is how the GOLD feed id
  `fa0f57...6b01`, "PYTH PRICE IN USD FOR GOLD 24/7", was confirmed.
- Pyth Benchmarks' TradingView shim endpoints return 404.
- **Kalshi publishes the settlement feed itself**, over an authenticated WebSocket at
  `wss://external-api-ws.kalshi.com/pyth_value`, channel `pyth_value`, subscribing by
  `underlying_tickers` such as `Metal.XAU/USD`. Each message carries `value_usd`, `source_ts_ms`
  and `received_at`, which is the same source/receipt split this desk already records elsewhere.

That last one is the right source, because it is the source that settles the contract, and this
desk has the credentials for it. The cost is that **the desk has no WebSocket client at all**:
every external feed today is HTTP polling through `Http`, and `third_party/httplib.h` does not
speak WebSocket. Adding one is the prerequisite for any 15-minute commodity work, and it is the
honest price of this idea.

## Economics

The category has 818 series. The liquid ones are all long-dated, and the median hours to expected
expiration is the reason this matters:

| series | markets | spread | median hours out | volume |
|---|---:|---:|---:|---:|
| KXFEDDECISION | 60 | 10.2c | 6,545 | 43,468,997 |
| KXRATECUTCOUNT | 21 | 0.3c | 2,702 | 8,810,464 |
| KXCPIYOY | 68 | 35.3c | 829 | 769,126 |
| KXCPI | 40 | 37.4c | 829 | 560,600 |
| KXGDPYEAR | 154 | 4.5c | 47,966 | 1,109,776 |

829 hours is 34 days. **Nothing liquid in economics settles today or tomorrow.** The
`near_day_categories_only` rule (on by default, covering economics, finance and tech_science)
is therefore doing exactly what it should: combined with `economics` defaulting off, the
algorithm sees none of these, and that is the correct outcome rather than a gap to fill.

## Financials, technology and science

The same shape as economics, and the same conclusion. Financials has 1,066 series; the liquid ones
are 492 to 11,468 hours out (KXLLM1 493h, KXINXY 11,468h, KXNASDAQ100Y 2,708h). Science and
technology has 389; its liquid rows sit at 493 to 29,030 hours (KXALIENS 2,726h and 29.7M volume,
KXCODINGMODEL 2,702h, KXCLAUDE 2,726h).

The near-day financial markets that would qualify do exist and are close to untraded: KXINX
carried 1,425 contracts of volume and KXNASDAQ100 5,999, against 10.9c and 23.2c spreads. The
daily variants KXINXD and KXNASDAQ100D had no open markets at all.

So for all three categories the today-or-tomorrow rule and the liquidity are in direct conflict:
what settles soon does not trade, and what trades does not settle soon. Leaving `economics`,
`finance` and `tech_science` switched off is the right state, and `near_day_categories_only`
protects against turning one on and finding a stale quote that looks like an edge.

## Commodities beyond the 15-minute series

| series | markets | spread | median hours out | volume |
|---|---:|---:|---:|---:|
| KXWTIMAX | 45 | 59.0c | 521 | 7,934,190 |
| KXAAAGASMAX | 13 | 2.2c | 2,718 | 1,433,514 |
| KXWTI | 135 | 19.1c | 1,310 | 784,804 |
| KXAAAGASD | 27 | 6.7c | **12.8** | 244,354 |
| KXWTIW | 27 | 2.1c | **41.3** | 217,886 |

Two rows clear the T+2 horizon on their own: **KXAAAGASD** (the daily AAA gas price, 12.8 hours
out, 6.7c wide, 244k volume) and **KXWTIW** (weekly WTI, 41.3 hours out, but only 2.1c wide).
KXAAAGASD is the more interesting of the two: a published daily average with a real spread and
real volume, and a settlement source that is a survey rather than a market, which is the kind of
thing a model can be better at than the tape.

## What this changes

- Sports totals are tradable, and the earlier claim that they were not should not be relied on
  anywhere it was repeated.
- The 15-minute commodity markets are the best structural setup on the exchange (liquid, zero
  maker fee, fast cycle) and are gated on one piece of missing infrastructure rather than on a
  missing model.
- Economics under a today/tomorrow rule is empty, and should stay switched off rather than be
  made to populate.
