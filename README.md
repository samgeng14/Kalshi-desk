# kalshi-desk

Semi-systematic Kalshi desk in C++17. One engine, pluggable strategies, SQLite audit trail,
localhost trading dashboard. Every order goes through the same fee-aware, size-capped path
whether a model proposed it, you typed it, or a bot fired it.

```
 Kalshi REST ──► ingest (series, markets, quotes) ──► SQLite ◄── NWS / Coinbase / ...
                                                        │
                       strategies: fair(p, sigma) ──► engine: fees, thresholds, Kelly, caps
                                                        │
                       proposals ──► manual (approve on dashboard) ──► executor ──► Kalshi
                                └──► bot (auto within limits)          │
                       reconcile: balance, positions, orders, fills, settlements ──► eval / gate
```

## Build (macOS)

```sh
brew install cmake openssl@3            # curl and sqlite3 come with the Xcode CLT
git clone <this repo> ~/Code/kalshi-desk && cd ~/Code/kalshi-desk
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/desk-tests                      # 93 checks: fees, sizing, signing (verified with openssl), store, engine
```

Linux: `apt install cmake g++ libcurl4-openssl-dev libssl-dev libsqlite3-dev`, same commands.

## Keys (no passwords anywhere)

1. Kalshi → Settings → API → Create key. Save the `.pem`, note the Key ID.
2. Make a demo account at the demo exchange and repeat. Demo keys only work on demo.
3. `mkdir -p ~/.kalshi && chmod 700 ~/.kalshi`, put the pem files there.
4. `cp config/desk.example.json config/desk.json`, fill in `key_id` and `pem_path`. Keep `"env": "demo"`.
5. `./build/kalshi-desk sign-test` → `signature ok: yes`. `./build/kalshi-desk balance` → your demo balance.

`config/desk.json` and `data/` are git-ignored.

## Run

```sh
./build/kalshi-desk serve         # dashboard: http://127.0.0.1:8787
./build/kalshi-desk once          # one cycle, print status
./build/kalshi-desk report        # the $100 -> $1,000 gate as JSON
```

Without keys the desk still runs: market data, quotes history, model fairs and proposals all
work; only submit/cancel/close are disabled.

## The cycle (every `poll_seconds`)

1. Refresh series metadata for the watchlist (fee_type, fee_multiplier, category) hourly.
2. Pull open markets for each watched series; log top-of-book to `quotes` on every change
   (research data accumulates from day one).
3. Every `reconcile_every` cycles: balance, positions, resting/executed/canceled orders, fills,
   settlements → store. Fills placed on the Kalshi website show up too (labeled external).
4. Strategies: `prepare()` once (fetch spot, forecasts), then `fair(market)` → `(p, sigma)`.
   Raw estimates go to `model_fairs` (the Brier sample, traded or not). The traded number is
   shrunk toward the market mid: `p = (1-w)·model + w·mid`, `w = params.<strategy>.market_weight`
   (default 0.5; set 0 when a strategy has earned it).
5. Engine per market: candidates = {YES, NO} × {maker, taker}. Maker price = `floor(p − min_edge)`
   capped below the ask and only if it joins/improves the bid. Net edge = `p_win − price − fee`.
   Keep if `edge ≥ min_edge_(maker|taker)` and `edge ≥ sigma_mult · sigma`. Size = quarter Kelly,
   capped by per-event, per-cluster and total-open limits, and by displayed size for takers.
6. Research-only gates (the number is logged for the Brier sample, nothing is proposed, and any
   pending proposal is withdrawn): strategy mode `research`; a fair the strategy itself flags
   (`Fair::research_only`, e.g. same-day weather, crypto before realized vol exists); weather
   markets whose settlement source is not the validated one; and any non-manual model that
   disagrees with a two-sided market by more than `risk.max_disagreement` (0.30). The last one is
   the lesson of 2026-09-05: all five weather trades had |raw − mid| ≥ 0.34 and the market was
   right five times out of five.
7. Exits: for every position whose market has a fair from a running strategy, the engine keeps
   one reduce-only order working at our side's fair + `min_edge_maker` (or takes the bid when it
   is already richer than that). Exit proposals carry the strategy's mode: `auto`/`bot` send
   them alone, `manual` queues them with the entries; they are re-priced when the fair moves two
   ticks and withdrawn when the position is gone. Positions from strategies in `research` or
   `off` are left alone.
8. Proposals: `manual` waits in the queue; `auto` submits only proposals that pass the strategy's
   rule (`params.<strategy>.auto`: min_edge, max_cost, max_sigma, min_minutes_to_close, styles,
   min_spread for liquidity provision only where the book is wide) and
   queues the rest; `bot` submits everything (max `max_orders_per_cycle` per cycle).
   Pending proposals refresh in place while the edge persists and expire after `proposal_ttl_seconds`
   or when the edge is gone. Resting orders from our proposals are canceled when the fair moves
   through them. Existing positions are never added to or flipped by the engine.
7. Kill switch: cancels every resting order, expires proposals, blocks the bot. Trips itself on an
   order rejection or on a `daily_loss_stop_frac` drawdown in portfolio value. Re-arm on the dashboard.

## Fees

From the series object, not hardcoded: `fee_type` ∈ {quadratic, quadratic_with_maker_fees,
quadratic_with_combo_maker_fees}, `fee_multiplier` ∈ {0, 0.5, 1}. Taker `M·0.07·P(1−P)`,
maker `M·0.0175·P(1−P)` only on the `_with_maker_fees` series, rounded up to a centicent.
Weather, most sports and the crypto hourly series charge makers nothing; KXCPI, KXNBA, KXEPLGAME
and the other big series do.

## Order vocabulary (V2)

The API speaks in YES-book terms: `side: bid` buys YES (sells NO), `side: ask` sells YES (buys NO),
price is always the YES price, quantities are fixed-point strings. `OrderRequest` is in outcome
terms (`yes`/`no` at that outcome's price) and `KalshiClient::order_body` does the translation.
Closing a position buys the opposite outcome with `reduce_only`.

## Strategies

| name | category | data | status |
|---|---|---|---|
| manual_fair | any | p/sigma you enter (dashboard) | live |
| weather_nws | Climate and Weather | NWS hourly forecast + same-day station obs + ECMWF ensemble | day-ahead live; same-day research-only (WX-1b) unless `params.weather_nws.same_day_trading` |
| crypto_lognormal | Crypto | Coinbase spot, 3-hour realized vol (DVOL as a band) | research until CRYPTO-2 has n ≥ 100 hours; needs collateral on the crypto exchange shard |
| sports_inplay | Sports (20 leagues) | ESPN live state; Poisson goals from pre-match prices (Dixon-Coles) | research: collects ext_sports; the account cannot open Sports positions |
| sports_live | US team sports (NFL, CFB, NBA, WNBA, NCAA basketball, MLB, NHL) | ESPN live win probability; lead/time model (Stern) from the score, clock and the sportsbook line; the line alone pre-game | research: logs ext_wp every 30 s; maker-only auto rule with `min_spread` and exits ready for when the account can trade Sports |
| econ_nowcast | Economics | Cleveland Fed nowcast | stub |
| commodity_futures | Commodities | front-month futures | stub |
| index_close | Financials | real-time index + VIX | stub |
| scitech | Science and Technology | base rates | stub |

A strategy is ~80 lines: `covers()`, optional `prepare()`, `fair()`. Register it in
`src/strategies/registry.cpp` and add its mode to the config. See `docs/hypotheses.md` for what
each one is supposed to test and the data it needs.

Strategy modes: `off` (ignored), `research` (collectors and fair logging run, nothing is
proposed), `manual` (every proposal waits for you), `auto` (rule-passing proposals go alone),
`bot` (everything goes). `scripts/tradeit up|down|status|logs` starts and stops the desk on a
Mac with a single-instance lock and opens the dashboard.

## Auto-trading portion, in one place

- Strategy mode `auto` sends proposals that pass its rule; `bot` sends all of them. Both only
  send while the header **auto** switch is ON (`POST /api/auto {armed}`), and entries stop for
  the day once `risk.max_auto_dollars_per_day` of purchase cost has gone out unattended (the
  proposal then waits in Signals with an "auto budget" note). Exits, targets and stops always
  run: they only reduce risk.
- A 4xx rejection of an order we sent trips the kill switch (cancel everything, expire the
  queue, stop sending). Pre-flight refusals (caps, a stale account snapshot, an order already
  resting in that market) never trip it; they are logged on the proposal and retried next cycle.
- KILL is lock-free: it stops every send path immediately and sweeps orders in the running cycle.

## P&L and the trading day

- **P&L today** in the header is mark-to-market: equity now minus equity at the first sync of
  the local day (`risk.day_tz`, default America/Chicago); it resets at local midnight along
  with the daily stop baseline and the auto budget. The P&L tab breaks it into settled today and
  positions closed out before expiry.
- A position closed before settlement keeps its realized P&L: Kalshi's realized number is kept
  per market in a ledger (flat markets included) and frozen once the market settles, so closing
  a trade never makes it disappear from the totals.
- Once trading closes in a market (weather at 05:00–08:00Z the next morning), the position row
  says "trading closed · settles <time>" instead of offering a close; the payout arrives at
  settlement.
- Exchange shards: `risk.shard_topup` keeps a shard funded from shard 0 through Kalshi's
  intra-account transfer (asynchronous, inside your account); the risk book also has a
  "move $20 to shard 2" button and `POST /api/transfer`.

## Research bookkeeping that runs by itself

- Every strategy's raw fair is logged to `model_fairs` (traded or not). A settle sweep fetches
  results for closed markets that have a logged fair (10 every other cycle), so the Brier sample
  fills in for markets you never traded.
- The Strategies tab shows a scorecard per strategy: fairs logged, paired Brier vs the market
  mid on settled markets with a 90% CI (negative = the model is better), realized edge and P&L
  when traded. The rule for promotion is on the tab: `research` → `manual` when the CI is
  moving, `auto` only when it is below zero.

## Brackets (take-profit / stop / trailing stop)

Set on any position from the Positions panel: a profit target in dollars (converted to a price
of the side you hold from your average cost) or a price, a maximum loss in dollars or a stop
price, and an optional trail in cents. `alert` mode flags the row and logs an event when the bid
reaches either level; `auto` mode keeps one reduce-only order resting at the target (taking the
bid if it is already through) and dumps into the bid as a taker when the stop trades. A bracket
owns that position's exit (the model's exit logic steps aside), it is removed when the position
closes, and `POST /api/brackets/<ticker>` / `GET /api/brackets` drive it.

## Dashboard

Signals blotter (every live proposal, sorted by edge, with the mode badge and go/reject), price
ladder for the selected market (depth, your orders, fair marker; click a level to load the
ticket), risk panel (open exposure, per-event and per-cluster limit bars, daily stop), positions
with unrealized P&L, order/fill blotter with slippage vs the proposal price, alerts strip, and
tabs for strategies (modes and auto rules), P&L, the gate report, live sports states,
hypotheses, events and config. Reads never wait on a cycle: the engine publishes a snapshot.

Only one desk may run per database (`data/desk.sqlite.lock`); `report` and `balance` may run
alongside it.

## Research CLI (historical studies, public data only)

```sh
./build/kalshi-desk research backfill KXEPLGAME,KXLALIGAGAME 3     # settled markets + newest 3 pages of trades -> hist_*
./build/kalshi-desk research backfill KXHIGHNY,KXHIGHCHI 0          # markets only (results)
./build/kalshi-desk research tied80 KXEPLGAME,KXLALIGAGAME          # SPORT-1: score at 80' from ESPN goal minutes,
                                                                   #   tie price at 80' from the tape, Poisson model, NO-at-ask return
./build/kalshi-desk research longshot KXEPLGAME 135                 # SPORT-3/FIN-2: win rate by price N minutes before settlement
./build/kalshi-desk research wx-settle KXHIGHNY,KXHIGHCHI           # WX-1: settled bracket vs station daily max (IEM ASOS)
./build/kalshi-desk research wx-intraday KXHIGHNY,KXHIGHCHI 0.08    # WX-1b: same-day climatology model vs the tape, by hour
                                                                   #   (needs `research backfill <weather series> 3` first)
./build/kalshi-desk research wp-score 0.06                          # SPORT-4: ESPN wp / lead-time / market Brier by period,
                                                                   #   from ext_wp + settled home markets (`backfill <game series> 0`)
```

`research/wx_rise.py` is the Python prototype of `wx-intraday` (same definitions, same numbers);
the C++ command is the one the registry cites.

`report`, `balance` and `research` may run while `serve` is up; they share the database.

## Gate report (`/api/report`, dashboard tab)

Realized net edge per dollar staked over settled markets with a bootstrap 90% CI, by strategy;
calibration of traded model probabilities by bucket; paired Brier (model vs market mid) over
all forecasts; ops counts. Six criteria for moving from $100 to $1,000:
150 independent settled events, 28 days live, zero rejected orders, realized edge > 0 with CI
low > −5c, calibration within 10pp per bucket, model Brier ≤ market Brier on n ≥ 100.

## Layout

```
include/desk/   headers          src/            implementation
  types.h       API objects        client.cpp      REST + signing headers
  auth.h        RSA-PSS signer     store.cpp       SQLite schema + queries
  client.h      KalshiClient       engine.cpp      proposals, sizing, risk
  store.h       Store              executor.cpp    submit/cancel/close/reconcile
  fees.h        fee model, Kelly   eval.cpp        gate report
  strategy.h    Strategy interface server.cpp      HTTP API + dashboard
  engine.h      Engine             strategies/     manual_fair, weather_nws, crypto_lognormal, stubs
ui/index.html   dashboard (embedded into the binary at build time)
tests/          desk-tests         docs/hypotheses.md   research registry
```

## HTTP API

`GET /api/state` · `GET /api/markets?series=` · `GET /api/orderbook/{t}` · `GET /api/quotes/{t}` ·
`GET /api/proposals?status=` · `POST /api/proposals/{id}/approve|reject` · `POST /api/order` ·
`POST /api/orders/{id}/cancel` · `POST /api/positions/{t}/close` · `POST /api/fair` · `DELETE /api/fair/{t}` ·
`POST /api/kill` · `POST /api/strategy/{name}/mode` · `POST /api/watch` · `DELETE /api/watch/{s}` ·
`GET /api/series?q=` · `GET /api/report` · `GET /api/hypotheses` · `POST /api/hypotheses/{key}` ·
`GET /api/fills` · `POST /api/reconcile`

The server binds 127.0.0.1 only. There is no auth on it; do not expose it.
