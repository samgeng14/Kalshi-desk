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

## The desk (deskd): one desk, a PAPER | LIVE toggle

```sh
python3 tools/desk.py setup            # once: PAPER (the Kalshi demo account) from ~/.kalshi/demo.json
python3 tools/desk.py setup live       # once: LIVE (real Kalshi) from ~/.kalshi/prod.json (or --key FILE)
python3 tools/desk.py                  # the desk: PAPER, and LIVE too after you type LIVE; one browser tab
python3 tools/desk.py sim              # the fake exchange: no network, no key, http://127.0.0.1:8790/
python3 tools/desk.py status           # both accounts
python3 tools/desk.py kill paper|live  # KILL, as the screen's button
```

The header's PAPER | LIVE toggle flips the tab between the two accounts (asking first before
LIVE), keeping the screen's tab. Underneath each account is its own deskd (its own lock, order
ledger and venue connections; ports 8788 and 8789), so a fault on one never touches the other's
orders; one command starts both and Ctrl-C stops both. The live config starts with Systematic
off; set `"systematic_orders": true` in `~/.kalshi/deskd-prod.json` yourself when you want lanes
there. `python3 tools/desk.py paper` or `live` runs one alone.

If a desk hangs, `build-deskd/deskctl kill --direct --config ~/.kalshi/deskd-demo.json` cancels
everything at the exchange with the desk's own key (for LIVE: the prod config and `--live`, and
only while the lock says "deskd LIVE"). `python3 tools/scorecard.py` (or `scorecard.py live`) prints
each lane's day and the plan's lane rules.

Women's college basketball (KXNCAAWBGAME): `python3 tools/train_wcbb.py --panel FILE.csv` fits
the in-game model and writes `models/wcbb_logit_v1.json` (the panel's columns and the pandas
one-liner that makes it are in the script's docstring; `docs/wcbb_model.md` is the file format).
With that file present, deskd shows the model's fair on those ladders while a game is on, from
the pregame Kalshi price it saw before tip. Without it there is no fair, and the log says so.

Before LIVE: `ctest --test-dir build-deskd -L invariants --output-on-failure` runs every test that
`docs/invariants.md` names for the plan's rules. `python3 tools/replication.py --data DIR` (the
recorder's data) counts the 15-minute windows whose strike pyth_value reproduced, LIP-15's anchor gate;
`tools/lip_join.py` and `tools/fillsim2.py` (the same `--data DIR`) give the join rule per series
and what resting behind the touch would have earned.

Research reads `ops-research.sqlite`, the copy the desk writes beside `ops.sqlite` after each ET
midnight, never the live file. `/ops/status` shows the live file's size; past 1 GB the desk alerts.

Each desk's log also goes to `deskd.log` in its data directory. After anything odd, run
`python3 tools/desk.py report` (or `report live`): the run state and why, any stopped sender, and
every KILL, refusal and unsent order from the log. On PAPER, Systematic has lane D1 PAPER, on from
the start: it quotes 1 contract at the touch in two liquid demo markets, buys 1 at the ask after
5 quiet minutes and exits any position within 90 s, so START shows orders going in and out. It
never runs on LIVE; `"paper_lane": false` in the paper config leaves it out.

```sh
tools/demo_checks                          # the plan's demo checks on PAPER (play money): order, group,
                                           # venue shapes (duplicate cid, expiry, reduce_only, post-only
                                           # cross, self-trade) and the lanes' feeds; replies kept in demo-checks/
python3 tools/latency_report.py            # where each order's time went (data/ops.sqlite): p50/p90/p99
                                           # per segment, click to ack; --hours, --own SYS, --json
```

Each desk writes every ended order's timeline and every fill to `data/ops.sqlite` (read it, never
write it: deskd's store thread is its only writer). Systematic uses at most 80% of each shard's
write budget (Kalshi's Basic tier, 100 tokens a second); after the free Advanced upgrade set
`"write_tier": "advanced"` in the desk's config.

## Build (macOS)

```sh
brew install cmake ccache ninja openssl@3   # curl and sqlite3 come with the Xcode CLT
git clone <this repo> ~/Code/kalshi-desk && cd ~/Code/kalshi-desk
/opt/homebrew/bin/cmake -S . -B build-deskd -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3
nice -n 15 ninja -C build-deskd -j 4 deskd-all            # deskd, deskctl, fake-kalshi, their tests
cd build-deskd && ctest -R deskd --output-on-failure
./kalshi-demo-probe [--order-check]        # signed reads (and one tiny order) on Kalshi's DEMO through
                                           # deskd's net/ and signer; reads ~/.kalshi/demo.json, prints
                                           # handshake and request latencies; never touches production
```

Never more than `-j 4`, always `nice -n 15`, one build at a time, and never in `build/` while a
desk runs: an unbounded build (`cmake --build build -j`, ncpu + 2 jobs) stalled the live desk for
18 minutes on 2026-09-24. Under Ninja the CMake job pools hold compiles to 4 and links to 2
whatever `-j` says. On Apple Silicon every compile and link goes through `arch -arm64`, so an
Intel ninja or cmake (`/usr/local/bin`) no longer runs clang under Rosetta (server.cpp 10.2 s
there against 3.6 s native); the whole deskd build takes about 16 s at `-j 4` without ccache, and
a strategy edit relinks deskd in under a second. The old desk's targets (`kalshi-desk`,
`desk-tests` ...) build the same way, by name.

Apple Silicon: pass `-DCMAKE_OSX_ARCHITECTURES=arm64` (or `conda deactivate` first). A conda
`(base)` shell puts an Intel `cmake` first on PATH; under Rosetta it reports an x86_64 host, the
objects come out x86_64, and the link fails against Homebrew's arm64 OpenSSL. A failed link also
deletes `build/kalshi-desk`, so `tradeit up` then says the build is missing. Configure into a
fresh `build/` after that (`rm -rf build`): the cache keeps the wrong architecture.

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

For unattended running use `scripts/tradeit up` / `tradeit down` / `tradeit status`. `up` starts
`scripts/desk_supervise.sh`, which restarts the desk if it exits or stops cycling (three stale
readings of `last_cycle_ts`, 180 s) and logs to `data/supervisor.log`.

A reboot is the one thing the supervisor cannot survive. To start it at login, install the agent
(`scripts/desk_autostart.sh` stays down after `tradeit down`, which leaves `data/desk.stop`):

```bash
cp scripts/com.kalshi-desk.supervisor.plist ~/Library/LaunchAgents/
```

It loads at the next login. Remove it with `launchctl bootout gui/$(id -u)/com.kalshi-desk.supervisor`
and delete the plist.

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

The full operating model, entry lanes, T+2 rule, position-aware additions, and NBA/weather
research sessions are in [docs/platform-overview.md](docs/platform-overview.md).

## The systematic trader

The **Systematic trading** tab runs one algorithm on your say-so: it screens every watched
contract, takes the sides whose net edge sits inside its band with a win probability above the
floor, sizes by a capped Kelly fraction, enters through the same executor as everything else and
manages its own exits (take-profit, stop, time stop, hold-value-vs-sale, settlement). Its own
daily budget and loss stop sit on top of the desk's caps. Everything it will not take - edges
above its cap, wide books, big model/market gaps - stays on the board for you. Locked baskets go
through it too. Paper mode runs the same rules on simulated fills. `docs/systematic.md`.

## Perps (the margined instance)

The **Perps** tab is the only part of the desk that sees Kalshi's second exchange instance:
its own cash (moved from the tab), equity, margin, positions with the exchange's liquidation
price, every active perp with basis and funding, a ticket behind its own rails
(`params.perps`: order notional, total notional, leverage, a price band around the mark, the
kill switch), and one rule, the basis reversion, paper or live. Contracts are slices of the coin
priced in dollars; fees are basis points of notional; nothing from the binary-contract
arithmetic applies. `docs/perps.md` has the rails, the rule and what the sampling found (the
BTC perp sits within 3 bps of its index in normal conditions, so the rule fires rarely by design).

## Polymarket US and the pair rule

The **Polymarket** tab reads the second venue (public prices without a key; orders with one):
its cash and positions, every game market it shares with Kalshi paired by league, Eastern game
date and team, both books side by side with the lock arithmetic for every route, a ticket
behind its own rails (`params.polymarket`: order dollars, total dollars, a price band, the kill
switch, the exchange's own preview), and the pair rule, paper or live: post the Kalshi leg,
hedge on Polymarket when it fills, hold to settlement. The `xvenue` strategy makes the
Polymarket mid the fair for any Kalshi game market both venues list, so those rows sit on the
Trades board like any other estimate. The secret key lives in a file named by `secret_path`,
never in the config. `docs/polymarket.md` has the wire format, the matching, the arithmetic and
what the venue actually offers (sports game winners only; NFL within a cent of Kalshi).

## The Trades board

The front page is one ranked list from every source: the systematic screen's Kalshi rows
(weather, crypto, sports, Polymarket-priced), the qualifying Kalshi/Polymarket pairs, and any
perp with a basis worth a look, ordered by edge per dollar. A row is there only while it is
actionable: hold or work one side of an event and its other sides leave. Double-click a row (or
`trade`) for the confirmation: price, contracts, style, the caps, cost, what it pays if right,
what it loses if wrong, the expected value at p(win), breakeven, and the algorithm's take-profit
and stop levels; Enter sends. Pairs open the pair modal; a perp row jumps to the Perps ticket.
The strategies' own proposal queue (the auto/instant lane) is on the Strategies tab.

## Auto-trading portion, in one place

- Strategy mode `auto` sends proposals that pass its rule; `bot` sends all of them. Both only
  send while the unattended-sends switch on the Strategies tab is ON (`POST /api/auto {armed}`),
  and entries stop for the day once `risk.max_auto_dollars_per_day` of purchase cost has gone
  out unattended (the proposal then waits in the Strategies tab's queue with an "auto budget"
  note). Exits, targets and stops always
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

The Trades board (every source ranked, double-click to trade), price ladder for the selected
market (depth, your orders, fair marker; click a level to load the ticket), risk panel (open
exposure, per-event and per-cluster limit bars, daily stop), positions with unrealized P&L,
order/fill blotter with slippage vs the proposal price and the recent proposals, alerts strip,
and tabs for strategies (modes, auto rules, the proposal queue), the systematic trader, perps,
Polymarket, the central risk book, P&L, the gate report, live sports states, hypotheses, events
and config. Reads never wait on a cycle: the engine publishes a snapshot.

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
  perps.h       PerpsDesk          perps.cpp       the margined instance: rails, ticket, basis rule
  poly.h        PolyDesk           poly.cpp        Polymarket US: auth, matching, rails, ticket, pair rule
                                   strategies/xvenue.cpp   the Polymarket mid as a Kalshi fair
  account_summary.h/.cpp             read-only cash/equity/P&L totals across event contracts and perps (GET /api/account-summary)
tests/          desk-tests, systematic-tests, perps-tests, poly-tests, account-summary-tests   docs/hypotheses.md   research registry
```

## HTTP API

`GET /api/state` · `GET /api/markets?series=` · `GET /api/orderbook/{t}` · `GET /api/quotes/{t}` ·
`GET /api/proposals?status=` · `POST /api/proposals/{id}/approve|reject` · `POST /api/order` ·
`POST /api/orders/{id}/cancel` · `POST /api/positions/{t}/close` · `POST /api/fair` · `DELETE /api/fair/{t}` ·
`POST /api/kill` · `POST /api/strategy/{name}/mode` · `POST /api/watch` · `DELETE /api/watch/{s}` ·
`GET /api/series?q=` · `GET /api/report` · `GET /api/hypotheses` · `POST /api/hypotheses/{key}` ·
`GET /api/fills` · `POST /api/reconcile` · `GET|POST /api/systematic` · `POST /api/systematic/close/{t}` ·
`GET /api/perps` · `GET /api/perps/book/{t}` · `POST /api/perps/sync` · `POST /api/perps/order` ·
`POST /api/perps/orders/{id}/cancel` · `POST /api/perps/positions/{t}/close` · `POST /api/perps/transfer` ·
`POST /api/perps/algo` · `GET /api/poly` · `GET /api/poly/pairs` · `GET /api/poly/book/{slug}` ·
`POST /api/poly/sync` · `POST /api/poly/preview` · `POST /api/poly/order` · `POST /api/poly/orders/{id}/cancel` ·
`POST /api/poly/positions/{slug}/close` · `POST /api/poly/algo` · `POST /api/poly/pair` · `GET /api/account-summary`

The server binds 127.0.0.1 only. There is no auth on it; do not expose it.
