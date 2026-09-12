# Perps research log

## 2026-09-11 — BTC/ETH are tradeable, but not at the horizons we planned

I overstated a claim and it is corrected in `research/maker_viability.md`: BTC and ETH are ruled
out for **spread capture**, which I wrote as "ruled out permanently". They remain first-class
markets under a directional thesis. Screen in `research/impulse_viability.md`.

**The moves exist.** BTC has 10.6 opportunities a day above 25 bps at 2 minutes, ETH 21.6, SOL
32.8, XRP 33.9. Opportunity count is not the constraint.

**The short horizons are arithmetically dead.** A perfect oracle that identifies the top decile of
1-minute BTC moves and is never wrong about direction still **loses 3.2 bps** after a 17 bps round
trip, because the move is smaller than the fee. Sub-minute horizons are smaller still. **Dropping
10s, 30s and 1m from the BTC/ETH horizon list; testing 2m, 5m and 10m, weighted to 10m.**

**The bar, set before any model is fitted.** Trading the top 5% of moves, breakeven directional
accuracy is `(fees/E|r| + 1)/2`:

| | @17 maker/taker | @10 maker/maker |
|---|---|---|
| BTC 10m | 65.4% | 59.0% |
| ETH 10m | 61.8% | 57.0% |
| XRP 10m | 59.7% | 55.7% |

Published models reach 52-56%. **Passive execution on both legs is the strategy, not a
refinement** -- it moves the bar 4 to 6 points, which is the entire margin between "no documented
model does this" and "the edge of the published range".

**The most tradable major is XRP, not BTC.** Larger moves against the same fixed fee. BTC has the
best data and the worst economics; rank by required accuracy, not familiarity.

**Data-quality flag:** the same calculation on WLD returns +5,422 bps, which is a 14-day book with
68% empty minutes producing quote jumps rather than trades. Grade-C contracts need their moves
verified against prints before any midpoint return distribution from them is believed.


## 2026-09-11 — is there enough spread to make a market? Six contracts, and not the ones you'd pick

Run before building FAIR-MAKER, because the strategy rests on spread capture and the question is
answerable from data already on disk. Full screen in `research/maker_viability.md`.

**Six of twenty contracts have a median quoted gap above the 10 bps maker/maker round trip**:
AAVE 15.3, ADA 13.5, WLD 13.2, BNB 10.8, VVV 10.4, KSHIB 10.1. **BTC and ETH are ruled out
permanently by arithmetic** at 0.5 and 1.1 bps against a 10 bps fee; no model rescues that.

**The correlation between log(notional/min) and the quoted gap is −0.64.** Spread is wide exactly
where flow is thin, which is the market working rather than an inefficiency: the six wide
contracts average $501/min of notional and 68% of minutes with no trade, against $2,334/min and
12% for the six tightest. No contract here has both width and flow, so the strategy picks one.

ADA and WLD are the only two with meaningful amounts of both. If FAIR-MAKER works anywhere it
works there first. Both are grade C, 11 and 14 days, so the backfill cannot answer questions about
them and the recorder has hours. That is a reason to wait, not to widen the universe.

The upper bound, assuming away every hard part: capturing 100% of flow at the median gap across
the qualifying contracts is $804/day. **This is not a forecast and must not be quoted as one** --
it ignores queue position, adverse selection, the 68% of empty minutes, and our own size being a
meaningful share of a $34/min book. Its only use is ruling in that the venue is not arithmetically
too thin, which is what the screen was run to decide.

**The constraint to model is fill rate and adverse selection, not spread.** The spread is present
and measured. Whether a passive quote gets filled at it without being run over is the whole
question, and it is what the conditional markout and fill models are for.

## 2026-09-11 — minute-bar directional alpha: closed as a negative result

**1,736 tests. 0 clear a 17 bps round trip. The largest gross edge anywhere in the study is 16.9
bps.** Full write-up in `research/minute_bar_alpha_results.md`; every row classified in
`reports/strategy_scoreboard.csv`.

| hurdle | positive |
|---|---|
| gross | 177 of the 400 strongest (44%, what coin flips give) |
| maker/maker 10 bps | 3 |
| maker/taker 17 bps | **0** |
| taker/taker 24 bps | **0** |

**Multiple testing is the headline, not a footnote.** At an uncorrected 5% over 1,736 hypotheses,
~87 results are expected by chance; 138 passed the three gates, which is not meaningfully more.
Benjamini-Hochberg at FDR 5% gives 70. Bonferroni at family-wise 5% gives **33**, some of them
overwhelming: HAC t of 30.7, 29.4, 27.6 on samples above 60,000.

**All 33 lose 14.2 to 19.8 bps after costs.** The single most significant result in the study is
`KXBTCPERP close_quote_gap_bps high_gt5 10m`, n=141,195, t=-30.7, with a decile spread of **+0.5
bps**. Statistical and economic significance have come fully apart, which is what a large sample
does to a small effect.

Classifications:

- **REJECTED_NONROBUST** (3): the only combinations clearing 10 bps on their point estimate. All
  three have bootstrap CIs spanning zero, HAC t below 2 (1.5, 1.9, 1.2), n~700 out of 142,000, and
  sit in no-trade or low-trade regimes where a wide close quote gap describes a stale book rather
  than a tradable price.
- **REJECTED_COST** (22): 1-minute mean reversion in high-flow minutes and the
  `close_quote_gap_bps` family. Real by Bonferroni, worth 0.2 to 2.8 bps against a 17 bps round
  trip. We would need roughly six times the effect, or six times cheaper execution.

**No further ML on the 1-minute historical feature set.** A booster on features whose univariate
ceiling is 2.8 bps cannot produce 17.

The one robust effect lives in **high-flow minutes**, which is exactly where a 1-minute bar sees
nothing and where the recorder's L2 data does. Next phase is the microstructure feature engine and
eight pre-registered hypotheses, in `research/preregistration.md`. Two things make it a different
bet rather than a retry: the maker/maker hurdle is 10 bps rather than 17, and a passive strategy
earns the spread as well as the alpha.


## 2026-09-11 — the perps socket, found properly this time

`wss://external-api-margin-ws.kalshi.com/trade-api/ws/v2/margin`, signed `/trade-api/ws/v2/margin`.
The host was the documented one all along; the path was not, and `address: /` as documented
answers 404.

**I pointed the recorder at the wrong socket first, and my own probe is why.** Subscribing to
perps on the main elections socket acknowledges and sends `orderbook_snapshot` frames, so the
probe -- which treated "a frame arrived" as success -- reported it working. The frames carry no
`bid`, no `ask` and an empty `market_id`. That is the same error as calling `ask.close-bid.close`
a spread: checking the shape and not the content. The probe now reads eight seconds past the
subscribe ack and requires a payload with an actual book in it, reporting `EMPTY frames but no
book` otherwise.

The real socket's `ticker` channel carries `bid`, `ask`, `bid_size_fp` and `ask_size_fp`, so L1
depth comes free alongside `orderbook_delta`.

**The clock warning fired on the first run, and it was right:** the laptop is **+0.107s** off
time.apple.com. That is 107 milliseconds, against lead-lag horizons that start at one second. Any
cross-venue lead computed from source timestamps would be measuring the clock. Use receive-time
differences measured by this machine, and fix the NTP sync before trusting anything finer.

**`--rest-book-hz` was lying.** It meant requests per second across the whole book, so 18 markets
at "1 Hz" were each polled every 18 seconds while the log printed "every 1000ms". It now means
per-market Hz, computes the resulting request rate, and warns when that approaches the ~20/s perps
read budget. It is off by default now that the socket works, and kept as the fallback that needs
no WebSocket at all.


## 2026-09-11 — the WebSocket documentation is wrong, and I wired the index feed wrong

`scripts/ws_probe.py`, two hosts x five URL paths x four signed-path variants per socket:

| socket | URL | signed path |
|---|---|---|
| perps / margin | `wss://api.elections.kalshi.com/trade-api/ws/v2` | `/trade-api/ws/v2` |
| CF Benchmarks | `wss://external-api-ws.kalshi.com/trade-api/ws/v2` | `/trade-api/ws/v2` |

The documented `wss://external-api-margin-ws.kalshi.com/` with `address: /` answers **404**. Perps
subscribe from the **main** socket, and both sign `/trade-api/ws/v2`, not `/`. This is the second
time production has contradicted the documentation, after the two `/margin/*` endpoints that 404,
and it is the argument for the probe existing.

**Three errors of mine in the index feed**, found by reading the payload spec rather than by
running it: the subscribe parameter is `index_ids` not `index_tickers`; the values are CF
Benchmarks index IDs (`BRTI`, `ETHUSD_RTI`) not Kalshi tickers; and the timestamp is
`source_ts_ms` in **milliseconds**, where I had been reading a non-existent `ts` field as
**seconds**. The first two subscribe to nothing. The third is the exact 1000x error the whole
`to_us()` discipline exists to prevent, introduced in the feed wiring where no test was looking.
`index_handler` is now extracted and tested like the margin handler.

**One thing worth having found:** the payload carries `received_at`, Kalshi's own receipt time for
the tick. `received_at - source_ts_ms` is an upstream latency measured entirely on their clock, so
it is the one CF-to-Kalshi timing number that does not depend on ours. Recorded as
`venue_latency_us`, separate from our own `latency_us`.


## 2026-09-11 — history depth answered, and it is per contract

The backfill settles the question the probe got wrong. 932,659 bars in, from an interrupted run:

| contract | bars | days | first |
|---|---|---|---|
| KXBTCPERP | 128,034 | 90.0 | 2026-06-13 |
| KXETHPERP | 128,016 | 90.0 | 2026-06-13 |
| KXHYPEPERP | 127,972 | 90.0 | 2026-06-13 |
| KXKSHIBPERP | 127,273 | 90.0 | 2026-06-13 |
| KXDOGEPERP | 126,385 | 90.0 | 2026-06-13 |
| KXBCHPERP | 125,470 | 90.0 | 2026-06-13 |
| KXLINKPERP | 118,343 | 84.0 | 2026-06-13 |
| KXAAVEPERP | 19,568 | 14.0 | 2026-08-28 |
| KXADAPERP | 15,372 | 10.9 | 2026-08-31 |
| KXBNBPERP | 14,793 | 10.9 | 2026-08-31 |
| KXGOLDPERP | 1,433 | 1.0 | 2026-09-10 |

Seven contracts have a full quarter of minute bars; four have days to a fortnight. The probe's
earlier "8 to 16 days" was measured on KXAAVEPERP because it sorts first, and it was an AAVE fact.

**Those seven stop exactly on the `--days 90` boundary, which means the request was binding and
not the exchange.** Kalshi launched perps in late May, so there is more to be had; the backfill now
says so when it detects a contract reaching its own start boundary.

Three backfill fixes came out of this run. Funding history was fetched **last**, so the interrupt
lost it (0 rows) -- it is one call for the whole book and now runs first. Ctrl-C printed a
traceback instead of saying the work is committed and resumable. And resume keyed on
`"period:start"`, which silently assumed a fixed window: measuring the window limit and using a
wider one would have missed every key and re-fetched all 932,659 bars. Resume is now by time
coverage, with merged intervals, and both key formats are understood.


## 2026-09-11 — first live probe, and three bugs

**14 of 16 endpoints returned 200.** `/margin/exchange_status` and `/margin/account_api_limits`
are both 404, so trading hours and this account's rate-limit tier are not readable and must be
inferred from behaviour. `/margin/trades` paginates correctly (3 pages, 300 unique, no loop) and
carries `taker_side` on every print, which means **signed trade imbalance is reconstructable from
history** and does not have to wait for the recorder.

**The history-depth measurement was wrong in the same way the spread claim was wrong.** The probe
tested `KXAAVEPERP` because it sorts first, found 8-16 days, and that is a fact about AAVE rather
than about the venue -- AAVE is not one of the June launch contracts. Generalising it would have
set the whole research plan against the shallowest contract in the book. The probe now
binary-searches the boundary per contract; the search is verified exact across 16 cases including
the cap-overshoot case that made it report 400 days for a contract with 365.

**Recorder bugs, found by running it.** (1) `websockets` renamed `extra_headers` to
`additional_headers` at v14, and the TypeError surfaces from `create_connection()` when the
connection is awaited rather than when `connect()` is called, so a try/except around the call does
nothing -- both Kalshi feeds were reconnecting forever while the external ones ran fine. Now
detected from the signature and flipped-and-retried if that guesses wrong. (2) binance.com returns
HTTP 451 from a US address; switched to binance.us, which is a different and thinner book and
should be treated as its own venue in any lead-lag result. (3) A feed failing identically forever
logged on every backoff; now once per distinct error.


## 2026-09-11 — methodology corrections (external review)

Five, all accepted, four of them narrowing a claim I had made too strongly:

1. **`ask.close - bid.close` is not a spread.** I had already fixed the OHLC-extrema error but
   still called the bar-end pair "near-simultaneous". Nothing in the API guarantees the last
   observed bid and the last observed ask inside a minute share a timestamp. Renamed to
   `close_quote_gap_bps` everywhere and demoted to a coarse liquidity proxy. The live L2 recorder
   is the only source of a contemporaneous spread.
2. **CF Benchmarks is the reference anchor, not fair value.** I wrote "fair value by construction".
   It is what the contract settles and funds against, which is not the same claim. The chain to
   measure is external venues -> CF -> Kalshi, and an external venue may lead CF itself.
3. **Zero-trade minutes are a regime, not noise.** I wrote that a quote change without a trade "is
   not a price change". Too strong: orders arriving and cancelling move the implied clearing price
   and can carry information. What is wrong is pooling zero-, low- and high-flow minutes as one
   process, so everything is now reported split at 0 / 1-5 / >5 trades.
4. **Univariate alpha audit before any ML.** Right, and it is now the gate: IC, Spearman IC,
   Newey-West t, block-bootstrap CI and decile monotonicity per feature, per horizon, per regime.
   A booster fed thirty features reports an edge whether or not one exists.
5. **Clock quality.** Monotonic only catches our own clock stepping. The recorder now writes NTP
   offset and round-trip into the stream, and the standing rule is to prefer receive-time
   differences measured by one machine over two exchanges' source clocks.


Newest first. One entry per finding, including the ones that killed a hypothesis -- especially
those. A rejected strategy that is written down is worth more than an accepted one that is not,
because the next person does not pay for it twice.

## 2026-09-11 — Phase 1, and the premise correction

**The brief's central assumption was wrong, in our favour.** Kalshi exposes three historical
endpoints for perps: candlesticks (1m/1h/1d OHLC of *bid, ask and trade price*, plus volume and
open interest), trades (with `taker_side`, cursor-paged), and historical funding rates. The plan of
"record forward for weeks, then research" is replaced by "backfill months now, and record forward
in parallel for the sub-minute questions only".

Separate bid and ask OHLC per minute is the specific thing that matters: spread and mid are
reconstructible historically, so Phases 4 through 8 have an input today.

**Second correction, to the brief's Phase 2 channel list.** The margin WebSocket has exactly six
channels: `orderbook_delta`, `ticker`, `trade`, `fill`, `user_orders`, `order_group_updates`. There
is no funding, reference-price, mark-price or liquidation-mark channel. Mark and reference come
from REST `/margin/markets`; the fast index tick comes from the *event-contract* socket's
`cfbenchmarks-value-5hz` (5 Hz), which is a different connection. Any lead-lag study under one
second depends on that socket, not the margin one.

**Useful detail the exchange gives away:** `orderbook_delta` carries `last_update_reason`, which
separates `Trade` from `Decrease` from the several cancel types. That is the cancel-vs-trade
distinction Strategy B needs, and most venues do not publish it.

**Trap found and documented:** margin WebSocket timestamps are epoch **milliseconds** with an `_ms`
suffix; event-contract sockets use seconds. Mixing them puts two feeds 1000x apart.

**Also:** perps traffic has its own rate-limit buckets, so a recorder cannot starve the live
event-contract desk. Collateral earns ~3.25% APY at the clearinghouse. Liquidation is two-tier
(queue entry, then liquidation), not one trigger. Stops are exchange-side *exit triggers*, not
order types -- the desk currently does stops in its own software, which means they are only as
reliable as its uptime.

## 2026-09-11 — five model families rejected

341 signal/horizon/instrument combinations across basis fade, basis follow, index lead-lag, perp
momentum/reversal, BTC cross-lead, and touch basis net of spread. 17 expected to survive by chance
at 5%; 7 survived, all marginal. At or below chance.

Two methodology bugs found in my own first pass, both of which manufactured signal:

1. **Overlapping windows.** ~17s quotes sampled at every point for a 30-minute forward return gives
   ~106 overlapping observations per independent one, inflating every t by ~10x. Fixed by stepping
   a full horizon between samples.
2. **Bid-ask bounce.** Signal and target both built from the mid, so any signal containing `mid(t)`
   predicts the reversal of its own noise. Fixed by entering one observation after the signal.

Before the fixes: a "surviving signal" on all 16 liquid instruments, every one negative. That
uniformity was the tell.

**Structural reason the basis cannot work on this book:** the instruments with a clean basis (BTC,
ETH, GOLD, SILVER quote inside 0.5 bps) never dislocate far enough to clear a 17 bps round trip;
the instruments that dislocate (VVV, ZEC) have 11-15 bps spreads, so most of a "22 bps excursion"
there is the spread itself. No instrument has both. See `docs/perps-models.md`.

## Open questions, ranked

1. **Does a post-only entry at the touch fill, and what does price do in the 60s after?** Paper
   cannot answer it (38 paper entries, 0 fills, because the paper model requires trading *through*
   the posted price). Live at $5 an entry is measuring this now. `scripts/perp_fills.py`.
2. **Orderbook imbalance** — the most promising untested family, and the only one that needs the
   tick recorder rather than the backfill.
3. **How far back do candlesticks go?** Empirical. Determines whether Phase 7's regime splits are
   possible at all.
