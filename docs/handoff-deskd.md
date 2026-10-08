# deskd handoff (2026-10-07)

The desk is being rebuilt as `deskd`, an event-driven C++17 trading desk, per `docs/impl_plan.md` (the operator-approved plan; read sections 2-4, 8 and 9 first). This file says where everything is and what to do next.

## Where

| What | Where | Branch |
|---|---|---|
| deskd (C++ backend, this worktree) | `/Users/samgeng14/Code/kalshi-desk-deskd` | `desk/deskd` |
| Frontend `ui/desk/` (merged here at contract 1.5.0) | `/Users/samgeng14/Code/kalshi-desk` | `desk/frontend-v3` |
| Recorder (Python, for the VM) | `/Users/samgeng14/Code/kalshi-desk-recorder` | `desk/recorder` (nothing committed; an untracked, unfinished `recorder/` from a cut-off run is there: read it, keep what is sound) |
| Old desk (frozen, stopped for good) | `src/`, `include/desk/` | do not modify |

Nothing is pushed. `ui/desk/js/contract.js` is the API contract deskd must serve. New code goes under `src/deskd/`, `include/deskd/`, `tests/deskd/`, `tools/`; the fake Kalshi for tests is `tests/fake_exchange/`.

## Rules (hard)

- Never place an order on production. Never pass `--live`. deskd refuses production hosts without `--live` plus the production account lock; keep it that way.
- The only credential to use is the Kalshi DEMO key: `~/.kalshi/demo.json` (Ed25519), deskd config `~/.kalshi/deskd-demo.json`, data dir `~/Library/Application Support/kalshi-desk/demo/`. Demo REST `https://demo-api.kalshi.co/trade-api/v2`, WebSocket `wss://demo-api.kalshi.co/trade-api/ws/v2`. Demo balance is $500 play money (shard 0). Small sizes; cancel everything you leave resting.
- Never read the main repo's `data/`, `config/desk.json`, `.secrets/`, or `~/Downloads`. Never log, print or commit key material.
- Builds: `build-deskd/` only, Ninja, `nice -n 15`, at most `-j 4`, one build at a time (README has the arm64 recipe). Never a second build dir.
- Tests: `ctest --test-dir build-deskd -R "deskd-|ui-desk"` (deskd suites take about a minute; `ui-desk-v3` alone about 3 minutes).
- Commits: `deskd: what changed`, logical steps, no warnings, tests with the repo's CHECK style registered in CMakeLists.txt.

## Done

- Week 0: skeleton (bus, owner threads, gw run state, KILL path, ops heartbeat, per-account lock, web/SSE on 127.0.0.1:8788 with Host/Origin/X-Desk guards), fake Kalshi (REST, WebSocket, matching engine, tape, faults, TLS), 10 test suites.
- Week 1 stage 1 (`net/`, `gw/kalshi_sign`, `gw/kalshi_wire`, `tools/kalshi_demo_probe`): TLS factory with verification and the guard on every connect, one request per connection, spare pool, strict HTTP/1.1 parser, RSA-PSS and Ed25519 signer, create order V2 and every read route, refusal classes. Proven against demo: signed reads, one post-only order placed and cancelled.
- Frontend round 2 merged (contract 1.5.0): darker theme, instant controls, calm flashes, the T/T+1 universe with search, frequent Systematic demo, ACTIVITY tapes and alerts, until-close orders.
- Oracle VM `deskd-recorder` for the recorder: `ssh -i ~/.ssh/oracle_deskd ubuntu@64.181.211.165` (Ubuntu 24.04 aarch64, 1 OCPU, 6 GB, 145 GB). Nothing installed yet.

## Next, in order

1. **Week 1 stage 2, market data and REST state:** `md-k` on the demo WebSocket (per-sid sequence numbers, STALE on a gap with resubscribe, never an empty subscription list, reconnect with backoff; lessons in `src/reference_feed.cpp` and `src/ws_client.cpp`), books as integer levels published to pub and gw; `rest`/`poll` for balance per shard, positions and open orders (cursor paged) every 10-15 s, exchange status, a startup reconcile before any order; the universe for `GET /api/universe` (markets settling today and tomorrow, category, close time, exchange_index, tick) and `POST /api/watch` pins; clock offset in health, over 0.5 s blocks Post.
2. **Stage 3, the Manual order path** (plan section 3): PlaceIntent validation, fat-finger guard, worst-case reservation per shard, cid prefix `M-`, journal, `send-k1`; Take = IOC at the touch plus slippage cap; Post = post_only + GTC + expiration_time + cancel_order_on_pause with a backup client cancel; Flatten = IOC reduce_only; cancels on `send-cx`; `exchange_index` on every write; UNKNOWN on 5xx/timeout/cut body, which blocks the market until a REST read clears it; fills de-duplicated by trade_id; token bucket per shard; web routes wait at most 50 ms for gw then answer queued; the latency timeline per order and `tools/latency_report.py`.
3. **Stage 4, prove it:** the real page against the fake (browser pane) and against demo; `tools/demo_checks` (duplicate client_order_id, shortest expiry, post_only cross refusal, reduce_only, group trigger/reset, self-trade shapes); 50+ demo orders; `docs/invariants.md` entries. Done when: click-to-ack p90 under 100 ms, Take fills at the shown price 90%+ in 1c books, nothing lives 2 s past expiry.
4. Then four reviews (orders, safety, latency/UI/contract, plan conformance), fixes, a final verification.
5. **Recorder** (plan section 7) on `desk/recorder`: asyncio, explicit subscription lists, hourly zstd Parquet with manifest and gaps, R2 uploader, deploy kit for the VM (setup.sh, systemd, deploy.sh). Soak an hour locally before deploying. The operator still has to create a production read-only Kalshi key, an R2 bucket with a write-only token, and a healthchecks.io check.
6. Week 2 (plan section 9): private socket, UNKNOWN table, risk book, Kill and order groups, Stop.

## Edge research so far (`docs/edge_results.json`)

All after fees, on held-out data. Women's college basketball in-game (score model vs thin books, IOC, from November) is the only edge above the 2-4c auto-IOC bar: +16.5c a contract, about $1.60-2.60 a game, verified independently; depth and ESPN delay unknown. Weather: two resting-maker rules on daily highs, +1-2c, about $1-2 a day. Financial and sports: nothing above the bar; small structural plays only (last-seconds 97-99c sweep +1c, LIP about break-even, pregame 90-95c favourites +2-3c suggestive). Still to run: skeptic checks of the weather, financial and sports results, and the final ranking.
