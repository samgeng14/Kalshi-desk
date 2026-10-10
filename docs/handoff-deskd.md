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
- Week 1 stage 2 (2026-10-08): guarded RFC6455 transport and demo market-data worker; per-sid
  sequence validation and reconnect/resubscribe; complete paginated REST account/listing state;
  T/T+1 universe and watch pins; per-tab FULL/TOP book publication and account projections;
  bounded clock-offset estimates in hdr.lat and /ops/status. Demo read probe validated actual
  responses against contract 1.5.0: eight live books, $500 on shard S0, no orders sent.
  Manual submission remains disabled until stage 3 integration.
- Stage 3 prerequisite (2026-10-08): authenticated private user_orders/fill WebSocket,
  reconnect and heartbeat handling, strict private-event adapters, and an account-read fence.
  An intervening private event or reconnect invalidates the paginated REST read; KXP stays
  stale until a complete consistent read succeeds. Demo proof: KX and KXP up without gaps,
  eight books, contract 1.5.0 captures pass, zero orders sent. Manual still read-only; incremental
  gateway order/fill accounting and the write path remain to implement.
- Stage 3 order-state core: `gw/order_ledger` implements per-cid UNKNOWN, exchange-id mapping,
  monotone cumulative counts, deduplicated executions, pending cancels, and shard reservations.
  A cumulative fill without its execution retains collateral; a zero-reduction cancel requires
  a terminal order read/event. Pure state tests exercise 5,000 UNKNOWN orders and all 144
  permutations of full-fill and partial-fill/cancel races, checking records against contract
  1.5.0. This component is not yet wired into the runtime's gateway or HTTP order routes.
- Stage 3 send workers (2026-10-09): dedicated `KalshiSender` Manual/cancel roles, preopened
  one-write connections, revoked/deadline/KILL-generation permits, precise write outcomes and
  timings, and cancel-all independent of gateway and queue capacity. Fake TLS proofs cover
  accepted-but-lost replies, no retries, duplicate cids, soft refusals, result-queue loss, a
  frozen gateway, and KILL overtaking an in-flight Manual write. Runtime wiring remains open.
- Cash projection fix (2026-10-09): the header no longer subtracts collateral twice from
  Kalshi's available balance. It adds modeled resting collateral to reconstruct cash, shares
  reducing room across orders in price priority, and retains exact fractional quantities.
- Oracle VM `deskd-recorder` for the recorder: `ssh -i ~/.ssh/oracle_deskd ubuntu@64.181.211.165` (Ubuntu 24.04 aarch64, 1 OCPU, 6 GB, 145 GB). Nothing installed yet.

## Next, in order

1. **Stage 2 is implemented; carry its state into gw in stage 3.** The feed currently publishes
   to pub; gw's Manual owner must receive the same books and REST revisions before accepting
   orders. The strict /api/health schema stays unchanged; clock detail is in hdr.lat and
   /ops/status. Browser access was declined, so the real-page visual check is still open.
2. **Stage 3, the Manual order path** (plan section 3): wire `gw/order_ledger` into the gateway
   alongside the execution/position ledger; authoritative series fees; PlaceIntent validation,
   fat-finger guard, worst-case reservation per shard, preserved `M...` cid, journal, `send-k1`;
   Take = IOC at the supplied slippage cap; Post = post_only + GTC + expiration_time +
   cancel_order_on_pause with a backup client cancel; Flatten = IOC reduce_only; cancels on
   `send-cx`; exchange_index on every write; per-shard token buckets; bounded 50 ms web replies;
   latency timelines and `tools/latency_report.py`. UNKNOWN and execution deduplication are
   implemented in the standalone ledger, but their sender/private/REST integration remains open.
3. **Stage 4, prove it:** the real page against the fake (browser pane) and against demo; `tools/demo_checks` (duplicate client_order_id, shortest expiry, post_only cross refusal, reduce_only, group trigger/reset, self-trade shapes); 50+ demo orders; `docs/invariants.md` entries. Done when: click-to-ack p90 under 100 ms, Take fills at the shown price 90%+ in 1c books, nothing lives 2 s past expiry.
4. Then four reviews (orders, safety, latency/UI/contract, plan conformance), fixes, a final verification.
5. **Recorder** (plan section 7) on `desk/recorder`: asyncio, explicit subscription lists, hourly zstd Parquet with manifest and gaps, R2 uploader, deploy kit for the VM (setup.sh, systemd, deploy.sh). Soak an hour locally before deploying. The operator still has to create a production read-only Kalshi key, an R2 bucket with a write-only token, and a healthchecks.io check.
6. Week 2 (plan section 9): complete the risk book, Kill and order groups, Stop; private socket
   and UNKNOWN foundations have been brought forward for a correct Manual path.

### Stage 3 integration notes

- Contract 1.5 permits Manual TTLs of 300/1800 seconds and expiry through close; the plan's
  60-second strategy limit must not override Manual until-close orders.
- Preserve frontend `M<session>-n` client IDs (`M-` is also valid); do not require a literal `M-`.
- UNKNOWN is `ord.st=PEND`, `why=''`, with an EXCHANGE response carrying the pending order.
  A transport failure never proves an order was rejected.
- The page requires KX and KXP. Pull a real minimal private order/fill feed plus reconnect
  reconciliation into stage 3; do not advertise KXP up merely because REST succeeded.
- `rest_position_json` is a startup cost projection. REST realized/fees are lifetime totals;
  the Manual execution ledger must supply daily P&L and own order revisions once enabled.
- Series category fallback is conservative prefix classification when market metadata omits
  category. Fee fallback is QUAD x1; LIP/model values are not invented. Integrate authoritative
  series fees before relying on this presentation fallback for cash admission.
- Publisher inbox overflow latches its header read-only and requires a restart. The gateway
  must include `pub.healthy()` in its entry prerequisites when Manual writes are wired.
- `OrderLedger` belongs exclusively to gw. Admission supplies a resolved expiry and conservative
  all-in reservation; its arithmetic bounds are not risk limits. A caller books a position only
  on `FillResult::APPLIED`, in the same gateway turn; cumulative ack/order/cancel counts never
  book it again. `UNMAPPED` events must be buffered/reconciled, and unhealthy ledger state blocks
  admission. `History` requires complete orders AND fills with verified query coverage; do not
  set these flags from an open-orders-only read. Positions, persisted journal, fee calculation,
  recovery of existing foreign orders, cancel dispatch, and actual HTTP writes remain separate
  integration work. Header RO must remain true until all of that is connected and tested.

Read-only demo verification after launch (use the port deskd prints):

```sh
python3 tools/deskd_read_probe.py --base http://127.0.0.1:PORT --out build-deskd/demo-read-capture.json
# Stage 3 private-feed readiness as well:
python3 tools/deskd_read_probe.py --base http://127.0.0.1:PORT --require-private --out build-deskd/demo-private-read-capture.json
```

This changes only the probe's local book subscription and validates captured responses with
the frontend contract; it has no exchange write path. The captured token is never persisted.

## Edge research so far (`docs/edge_results.json`)

All after fees, on held-out data. Women's college basketball in-game (score model vs thin books, IOC, from November) is the only edge above the 2-4c auto-IOC bar: +16.5c a contract, about $1.60-2.60 a game, verified independently; depth and ESPN delay unknown. Weather: two resting-maker rules on daily highs, +1-2c, about $1-2 a day. Financial and sports: nothing above the bar; small structural plays only (last-seconds 97-99c sweep +1c, LIP about break-even, pregame 90-95c favourites +2-3c suggestive). Still to run: skeptic checks of the weather, financial and sports results, and the final ranking.
