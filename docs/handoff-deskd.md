# deskd handoff (2026-10-07, updated 2026-10-21)

The desk is being rebuilt as `deskd`, an event-driven C++17 trading desk, per `docs/impl_plan.md` (the operator-approved plan; read sections 2-4, 8 and 9 first). This file says where everything is and what to do next.

## Paper and live (2026-10-12)

One desk with a **PAPER | LIVE** toggle in the header. PAPER is the Kalshi demo account (play
money), LIVE is production. Underneath each account is its own deskd (own account lock, order
ledger, venue connections, data dir; ports 8788 and 8789): `python3 tools/desk.py` starts PAPER,
and LIVE too once you type LIVE, in one terminal with labelled logs, and opens one tab; the toggle
flips that tab between them (the screen's tab kept), with a real-money confirm before LIVE.
`tools/desk.py` also has `setup`, `setup live`, `paper`, `live`, `sim`, `status`, `kill` (README).
Rules in code:

- `--live` takes a production credential (config `env: "prod"`) and only that; a demo key runs
  only without it. The venue's hosts must be that account's (production: api.elections.kalshi.com).
  Any mismatch never starts and dials nothing. The send guard still wants `--live` plus the
  production account lock for every production connection.
- LIVE starts only from a terminal, after typing LIVE; the live config `setup live` writes has
  Systematic off. Agents never start it: LIVE is the operator's.
- `tools/desk.py sim` (`tools/deskd_sim.py`) is a full sandbox: fake-kalshi with a scripted day
  (15-minute natgas windows with pyth for M1, today's KXHIGHNY bands for M2, a practice market),
  deskd `--sim` against it. Verified in Chromium 2026-10-12: Manual order, x, DEL ALL, KILL and
  re-arm; Systematic lanes M1 and M2 quoting, Start, Stop cancelling.

The work is on GitHub: `samgeng14/kalshi-desk`, branch `claude/pensive-meitner-8k2wae` (draft PR #1).

## 2026-10-21: a plan audit, order safety, the LIP probe, deskctl's direct KILL, the recorder

A section-by-section audit of `docs/impl_plan.md` against the code (471 items: 219 done, 106
partial, 141 missing) set this round's order: what protects money before LIVE first.

- **Order safety.** A post still working 1.5 s past its exchange expiry is cancelled by gw (plan
  section 3's backup; "nothing lives 2 s past expiry"). After KILL's cancel-all, send-cx reads the
  account's resting orders and batch-cancels anything still listed. Each account read now settles
  working orders and missed fills from the venue's rows (a private-feed drop no longer leaves an
  order WORK holding its cash), and an order with more filled than its fills for 5 s takes the
  account read's fill rows; REST and feed reports of one trade count once. A private fill that
  beats its order's 201 waits 5 s for it. An order group re-arms only once order_group_updates
  confirms the reset (the 200 alone after 5 s, counted in `gw.group_reset_fallbacks`). An
  adversarial review of these found three defects, fixed with tests (75ac677).
- **deskctl kill --direct --config FILE [--live]**: KILL at the exchange when deskd hangs
  (cancel-all, every order group triggered, a batch cancel of what still rests). Production only
  with --live, a prod config and a lock whose holder line says "deskd LIVE"; a free lock it takes.
- **LIP-15:** a 2-tick move of the touch pulls both sides; a deep 1-tick queue is never joined on
  the fill pace's relaxed edge; the lane row, `hdr.day.lip` and `/ops/status lip.lanes` show
  today's LIP estimate per lane (its own line, never in the stop). **Lane P1 LIP PROBE** (off
  until you turn it on): 30 a side in minutes 0-5 of M1's windows, 40 windows a day, paused at
  -$15 (plan 5(a), decision 4).
- **`tools/scorecard.py [paper|live]`**: the daily scorecard per lane and the plan's lane rules
  (the 200-fill rule for M1/P1, BAND's, X1/C1's) as advice.
- **Recorder** (`recorder/`, deployed by you): college and thin-sport games (WCBB from Nov 2, the
  thin men's decile as control; series tickers to confirm in `recorder.env.example`), their books
  by REST every 4 s (also the socket's fallback), ESPN and NCAA scores with first-seen times,
  hourly incentive programs, a daily /markets sweep, trade backfill, `market_lifecycle_v2`,
  `python3 -m recorder.report` (the 24 h / 1% gap check), and `deploy/base.sh` (chrony, swap,
  journald cap, security-only upgrades; plan mode by default).
- **The WCBB model** (plan 5(f); `docs/wcbb_model.md`): `tools/train_wcbb.py --panel FILE.csv`
  fits the logistic (pure Python; held-out Brier against the Kalshi mid, Stern's model and p0;
  never overwrites a model without `--force`) and writes `models/wcbb_logit_v1.json` with 10
  golden rows. deskd loads it at start (`deskd.wcbb_model`), refuses it unless its golden rows
  reproduce to 1e-6, and hands it to the game fair (`strat/game_fair.h`, one for WCBB, NBA and
  volleyball): each game's pregame price is the effective mid of its two markets while ESPN says
  "pre", before the start and at 15c or less (kept in `data_dir/game_p0.json` for 12 h), and the
  model's p for the home team's market (1 - p for the other) is `book.ref.fair` while the game is
  on (possession unknown: 0). No model ships: train it on the Mac from the WCBB panel;
  `deskd-wcbb` and `deskd-bball` check the C++ side against synthetic files the trainer wrote,
  `deskd-game_fair` the game fair, and `deskd-runtime_manual`'s wcbb_overlay the ladder end to end. Systematic use waits for the plan's gate (150+ settled games, held-out Brier at
  least the mid's, positive overlay markouts).
- **More safety and hygiene (later the same day):**
  - The equity floor's HALT (plan section 4, limit 5) lasts until a Start with the account's
    equity back over the floor, across days and restarts; the daily stop's still ends at ET
    midnight, and Start's refusal names the HALT it is.
  - LIP-15's 0.5 sigma pull works on every window it quotes (it needed a strike-checked window
    before); the 50-window replication counts are kept in `data_dir/replication.json`, so a
    restart no longer sends the anchor gate back to 0.
  - A foreign order blocks its market for Systematic too (tested: lanes go through the same
    admission as a click).
  - `ops.sqlite`: its size is in `/ops/status` (store.size_bytes) with a CRIT past 1 GB, and after
    each ET midnight the store writes `ops-research.sqlite` beside it (VACUUM INTO, then a rename):
    research reads that copy, never the live file.
  - The screen: a stream silent 3 s is reopened and resynced (was 5 s; it already greyed at 3 s),
    every tab redraws when Safari shows the page again, and the watchlist's ESPN number greys once
    its play is over 30 s old.
  - The recorder review's six findings are fixed with tests (restart backfill of markets that
    closed while it was down, the restart window from the socket's last frame, the control pick
    hour, a torn spool line in the gaps report, base.sh's package checks and chrony order).
- **Plan section 8 (tests before LIVE):**
  - `docs/invariants.md` maps every rule of plan sections 2-3 (85, plus the old lessons) to the
    test that asserts it: 50 tested, 17 partly, 8 missing (each with its reason), 10 n/a here.
    `ctest --test-dir build-deskd -L invariants` runs the 24 suites that hold them.
  - Races: a duplicate fill message counts once (Manual and Systematic); 5,000 orders through
    the UNKNOWN table settle in under 2 s, and gw now settles an account read in one indexed
    pass (it was quadratic in unknown orders). gw checks the ledger's bounds after every message
    in every runtime test and the soak (gw.invariant_violations must stay 0).
  - Sanitizers: `-DDESKD_ASAN=ON` (AddressSanitizer and UBSan). The first run passed 30 of 33
    deskd suites; the 3 failures were the tests' own (a dangling temporary twice, a wall-clock
    bound), now fixed. Configure it in build-deskd, then back (one build directory).
- **`tools/replication.py --data DIR`**: the anchor gate over the recorder's data (pyth_value
  against each 15-minute window's strike, deskd's own rules), and `--write-replication` to seed
  deskd's `replication.json` from it once the recorder has run (never over an existing file
  without `--force`).
- **`tools/lip_join.py --data DIR`** (the plan's join rule: a series joins when both sides hold
  the program's target in most minutes, sampled mid-minute) and **`tools/fillsim2.py --data DIR`**
  (queue position behind the touch: fill rate, 10/60 s markouts and LIP $ per contract by
  placement and age cap). Both read the recorder's books; an adversarial review's 4 findings in
  them are fixed with tests. Run them once the recorder has a few days of natgas.
- **Still open, deliberately:** Manual order groups (KILL's cancel-all and the batch sweep cover
  what they would add); exit 75 and the supervisor (the plan returns the LaunchAgent only once
  deskd is the production binary: your call); Stop by batch cancel (per-order cancels work); adopting the last
  run's own resting orders at start (short expiries; cash accounting risk); `cancel_epoch` (needs
  a browser-side sequence to be correct); the money ledger (settlements endpoint unverified); S1
  arbitrage (week 4; the plan's own evidence puts it at about $0); Polymarket (needs the account);
  the WCBB model's training (needs the panel, on the Mac); `config/lanes.json`; the 8-hour
  faulted soak.

## 2026-10-14: paper trading on the Mac

The operator's first paper session (2026-10-13): a ticket BUY "just said STALE", a KILL came from
nowhere, and Systematic sent nothing. Fixed:

- **The ticket sends an IOC by default** (a click takes the touch and fills). A result line under
  its buttons says why it cannot send now and what clears it (KILLED: RE-ARM in the header after
  60 s, then CONFIRM), or what became of the last order: sent, resting, filled at, cancelled, or
  refused with the backend's reason. The top BUY YES / SELL YES toggles are labelled SIDE: they
  send nothing.
- **Every stop says why.** A sender's KILL names the write and the venue's words ("venue refused a
  create (cid) as hard_invalid, HTTP 404: ..."), shown on the header's KILL button and the
  Systematic strip; local refusals carry a sentence; gw logs every create that did not land.
  `python3 tools/desk.py report` prints them from the desk's log file.
- **An unknown order no longer blocks its market for the run.** The STALE names it, and gw settles
  it from the account read: its own row at once, or its absence from a complete read begun after
  its deadline (an IOC's: 2 minutes). gw looks for a new account read every 250 ms.
- **Lane D1 PAPER** (demo only, on from the start): two liquid markets it picks itself, 1 contract
  at the touch both sides, an IOC buy after 5 quiet minutes, any position out within 90 s (by IOC
  at the touch). $20, 2 contracts a market. deskd adds it only without `--live`, gw refuses its
  orders on production, and `deskd.paper_lane: false` leaves it out. M1 has nothing to quote on
  demo (KXNATGAS15M's book is empty there) and M2's weather markets are not listed there.

## 2026-10-13

- **Fill probability** (operator: Manual should fill about 80% of the time; Systematic a fill
  every 5-10 minutes). `strat/fill_model` measures, from each market's own trade tape (the live
  trade channel plus a REST backfill of the last hour), the fraction of the last hour's windows in
  which enough taker volume came to fill an order behind the queue ahead of it. `GET /api/fill`
  (contract 1.6) gives the curve for a ticket: improve, touch, three behind, and P80, the price
  that fills 80% of the time. The ticket shows a FILL line and a P80 button. The makers lower
  their edge floor by 0.25c for each 10 minutes without a fill (never below 0.5c plus the maker
  fee), and a fill puts it back; `/ops/status` shows `est_fill_every_s` and `fills_30m` per lane.
- **Risk limits 2 and 5:** correlated groups (METALS, FX, ENERGY $25; any other market by its
  event, $15) and an equity floor (`deskd.equity_floor_dollars`, default $150) that halts
  Systematic.
- **The 4-hour TSan soak:** 3 h 52 m with no ThreadSanitizer report, until the container restarted
  (no end summary). It found two feed-wide STALE sources, both fixed:
  - **The account read** fetched every order and fill of the day each poll (13,500 orders by
    hour 4), so nearly every read overlapped a private event and was discarded; orders went
    STALE (585 refusals). It now reads the day whole once, then only what is new since the last
    good read (5-minute overlap, merged by id; an order that left the book is read by its id).
    A 40-minute soak after: 0 such refusals.
  - **Market data** reconnected at every watch-list change (each 15-minute roll, a pin), so every
    book went stale for a moment; and the private feed showed a gap on every event. Watch
    changes now happen on the live socket (added markets subscribe, removed ones are ignored
    until the next reconnect), and only an untrusted private event raises the gap.
- **Write budget:** gw keeps Kalshi's write buckets per shard (create 10 tokens, cancel 2);
  Systematic is refused RATE past 80% of a bucket, and Manual and cancels are never refused
  locally (`deskd.write_tier`: "basic", or "advanced" after the upgrade). `/ops/status` has
  `gw.write_budget`.
- **Order timelines:** each ended order's timeline (click, web, gw, sent, the wire's three
  segments, ack, the private feed's first word and fill, the cancel, done) and each fill go to
  `data/ops.sqlite` through the store thread (tables `orders`, `fills`). Read it with
  `tools/latency_report.py`.
- **Data releases:** `config/calendar.json` (ET weekly or dated releases, by series prefix; path
  `deskd.calendar`) holds the makers' resting quotes from 60 s before a release to 5 minutes
  after, as plan section 5 asks ("data release: NAME" in the lane's market status). It ships with
  EIA's Thursday 10:30 natural-gas storage report only. Add holiday moves from EIA's schedule, and
  CPI, payrolls and FOMC dates before gold or FX lanes trade. A missing file runs without it (and
  says so); a malformed one refuses the start.
- **LIP share (an estimate):** REST reads Kalshi's public `GET /incentive_programs` every 2 minutes
  for the LIP series' markets (filtering `incentive_type` itself: the venue ignores its type
  filter). Once a second strat estimates our share of each program's score from the book and
  Systematic's resting quotes:
  - a side counts from its best price out to the target size, each cent away discounted by the
    program's `discount_factor_bps`, and ours stand at the back of a level;
  - a second pays only when both sides reach the target;
  - its reward is split evenly between the two sides.

  `/ops/status` shows `lip.markets`, and each finished window goes to `ops.sqlite` `lip_windows`
  (seconds, seconds scored, mean share, est $). The venue publishes the terms, not its scoring
  code. Reconcile `lip_windows` against the payouts the account receives before trusting it.
- **Demo checks:** `tools/demo_checks` (the probe's `--order-check --group-check --venue-checks
  --feeds-check`). Venue checks: duplicate cid, shortest expiry, reduce_only on a resting order
  and on a flat IOC, post-only at the ask, self-trade both ways. Amend and
  `target_balance_allocation` are not run yet (unverified request shapes).
- **Still to do from the plan:** the demo proof on the Mac (`tools/demo_checks`, then 50+ demo
  orders); the money ledger in ops.sqlite (LIP payouts reconciled against `lip_windows`) and its
  nightly copy; engines for S1, C1 and X1.

## Where

| What | Where | Branch |
|---|---|---|
| deskd (C++ backend, this worktree) | `/Users/samgeng14/Code/kalshi-desk-deskd` | `desk/deskd` |
| Frontend `ui/desk/` (merged here at contract 1.5.0) | `/Users/samgeng14/Code/kalshi-desk` | `desk/frontend-v3` |
| Recorder (Python, for the VM) | `/Users/samgeng14/Code/kalshi-desk-recorder` | `desk/recorder` (nothing committed; an untracked, unfinished `recorder/` from a cut-off run is there: read it, keep what is sound) |
| Old desk (frozen, stopped for good) | `src/`, `include/desk/` | do not modify |

Nothing is pushed. `ui/desk/js/contract.js` is the API contract deskd must serve. New code goes under `src/deskd/`, `include/deskd/`, `tests/deskd/`, `tools/`; the fake Kalshi for tests is `tests/fake_exchange/`.

## Rules (hard)

- Never place an order on production. Agents never pass `--live` (the operator starts LIVE: `python3 tools/desk.py`, typing LIVE). deskd refuses production hosts without `--live` plus the production account lock; keep it that way.
- The only credential to use is the Kalshi DEMO key: `~/.kalshi/demo.json` (Ed25519), deskd config `~/.kalshi/deskd-demo.json`, data dir `~/Library/Application Support/kalshi-desk/demo/`. Demo REST `https://demo-api.kalshi.co/trade-api/v2`, WebSocket `wss://demo-api.kalshi.co/trade-api/ws/v2`. Demo balance is $500 play money (shard 0). Small sizes; cancel everything you leave resting.
- Never read the main repo's `data/`, `config/desk.json`, `.secrets/`, or `~/Downloads`. Never log, print or commit key material.
- Builds: `build-deskd/` only, Ninja, `nice -n 15`, at most `-j 4`, one build at a time (README has the arm64 recipe). Never a second build dir.
- Tests: `ctest --test-dir build-deskd -R "deskd-|ui-desk"` (deskd suites take about a minute; `ui-desk-v3` alone about 3 minutes).
- Commits: `deskd: what changed`, logical steps, no warnings, tests with the repo's CHECK style registered in CMakeLists.txt, one line a suite (`deskd_pure_test(x)`, `deskd_fake_test(x)` with the fake exchange, `fake_kalshi_test(x)` for the fake's own) and one line a config key in `config.cpp`'s known set.

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
- Venue fee metadata (2026-10-09): live/pinned/held series use the public single-series endpoint
  and a five-minute cache. The fee model/multiplier now enrich live markets and matching universe
  rows. Missing, stale, or unsupported metadata fails `rest::fee_ready()`; the display fallback
  must not be used for admission. Demo proof: 8/8 live markets fee-ready, both feeds healthy,
  five contract checks passed, zero orders sent. `/ops/status` reports metadata readiness.
- Manual admission (2026-10-09, `gw/manual_admission`): pure checks for cid, gates, tick grid
  per price range, two-tick band through the touch, POST expiry incl. until-close, reduce-only
  clip, collateral and a worst-case taker-fee allowance.
- 2026-10-10 (cloud session, Linux x86-64): the deskd suites build and pass on Linux (OpenSSL 3.0
  fallback, glibc thread names, SIGPIPE test). Fractional sizes: `fractional_trading_enabled` is
  parsed; whole-contract markets refuse non-whole counts (BAD_QTY) and clip reduce-only to whole
  contracts. `gw/manual_book`: admission context from the REST revision + book + ledger, shard
  free cash without double counting (see its header), conservative reducing room, foreign and
  duplicate orders, reservation and send job in one gw turn, sender replies applied to the
  ledger. `Snapshot::account_began_ms` added. Not yet wired into `Gateway` or `/api/order`.
  `docs/deploy-oracle.md` and `tools/vm/{inspect,update}.sh`: VM plan, read-only inventory and
  controlled updates (not yet run on the VM).
- Oracle VM `deskd-recorder` for the recorder: `ssh -i ~/.ssh/oracle_deskd ubuntu@64.181.211.165` (Ubuntu 24.04 aarch64, 1 OCPU, 6 GB, 145 GB). Nothing installed yet.

## Next, in order

1. **Stage 2 is implemented; carry its state into gw in stage 3.** The feed currently publishes
   to pub; gw's Manual owner must receive the same books and REST revisions before accepting
   orders. The strict /api/health schema stays unchanged; clock detail is in hdr.lat and
   /ops/status. Browser access was declined, so the real-page visual check is still open.
2. **Stage 3, the Manual order path** (plan section 3). Done 2026-10-10: gw runs admission,
   reservation, send-k1 replies, cancels and private order/fill events (`Gateway::attach_manual`);
   with a demo credential the runtime runs `KalshiSender`s on send-k1 (Manual) and send-cx
   (cancels, and KILL as a real cancel-all), each on its own pool, and gw reads a book board.
   `/api/order`, `/api/cancel`, `/api/cancel_all` post to gw; `/api/watch` reads the contract's
   `on`. Orders stay refused RO until `"deskd": {"manual_orders": true}` is set in the config;
   the header's cash is read-only until then. `/ops/status` shows `gw.manual_orders`,
   `books_live`, `rest.fee_ready_live`, `clock.post_trusted`. Tested in `test_runtime_manual`
   (the whole process against the fake over TLS: off by default, fill, rest and cancel, KILL).
   The screen also gets real data the simulator only fakes (2026-10-10): the trade channel's
   prints, last and volume on the ladders, today's fills (FILLS panel) with realized P&L by
   average cost, positions marked at the live mid with today's real and fees (flat rows kept),
   and the header's day.man and risk. Fair, ESPN, LIP and Systematic stay blank (no models or
   lanes yet; never a placeholder). A private event during a REST read now keeps the last
   consistent account instead of marking it not ready (clicks were refused after fills).
   **RO lock on demo (2026-10-10).** Every order after a KILL and re-arm was refused "RO an
   execution owner is not ready". A sender that failed (a hard venue refusal, an unconfirmed
   KILL cancel-all, a lost reply) stopped for good and nothing named it. Now: the refusal and
   `/ops/status` (`senders.k1/cx.why`, `gw.ledger_why`) name the owner and the reason; a sender
   comes back once a cancel-all sent after its failure is confirmed and KILL is re-armed; KILL's
   cancel-all is sent again on a 429 (a cancel-all costs a whole batch's write tokens). The
   ledger still needs a restart after a contradiction, and says so. The root cause on the Mac is
   not known yet: the next RO there names it. A failed REST poll now keeps the last good read
   (account up to 60 s, listing up to 5 minutes) instead of refusing clicks STALE.
   **Strategy foundation (2026-10-10).** Reference feeds: Kalshi's `pyth_value` (ref-k thread)
   and the 15-minute financial model fair on the ladder (`book.ref.fair/sd`, shown only when this
   window's strike was reproduced by the feed; the replication count toward the 50-window gate is
   in `/ops/status` "fair"); ESPN's game state and win probability on watched game markets
   (ref-espn thread, a data-only connection that cannot reach a venue). Systematic: lanes M1
   LIP-15 $55, M2 BAND 96-99 $25, S1 ARB $15, C1 COLLEGE TAKE $10, X1 ESPN EXP $5, all off; gw
   admits S orders with the risk book (inventory 20/40 contracts per market, lane budget, 70%
   ceiling, $30 daily stop -> HALTED), sends them on send-k2 in each lane's order group per shard;
   Stop cancels them and triggers the groups, KILL's send-cx triggers them too, and they are
   reset once stopped / at the re-arm. The LIP-15 maker engine (lane M1) quotes the open windows
   of `deskd.lip_series` (default KXNATGAS15M) per plan section 5. To run it on demo: set
   `"deskd": {"manual_orders": true, "systematic_orders": true}` (and `"lip_series"` to the
   15-minute series demo lists), start deskd, turn lane M1 on in the SYSTEMATIC tab, press Start
   once GROUPS passes. Later the same day: the header carries Systematic's day, stop, ceiling and
   cost (IOC lanes CAPPED past a $5 daily fee budget); the market list carries the model fair and
   the take edge after fees, and orders carry touch and edge; lane M2 BAND 96-99 joins 96-99c
   favourites post-only (5 a market, $15 a city-day, paused at -$20; `deskd.band_series`, default
   five weather cities; the plan's replay gate is the operator's call); `deskd-soak` runs LIP-15
   against the fake with a wandering street and checks the invariants every second (60 s under
   ctest; `DESKD_SOAK_SECONDS=14400` in a `-DDESKD_TSAN=ON` build for the plan's 4 hours). An
   18-minute soak here: 2 windows, 1355 quotes, 134 fills, 0 violations; it found series fee
   metadata expiring between polls (refused quotes for one poll every 5 minutes; now fetched again
   a minute early). Markouts: every fill's 1/10/60 s markout from md's mid history (`mo` events,
   per-lane means in `/ops/status`); the plan's markout rules stop BAND (-1c after 100 fills)
   and LIP-15 (-1.5c after 200). Systematic race tests (hit before the 201, fill during a cancel,
   Stop over an in-flight create). `order_group_updates` (its own subscribe command) tells gw of
   a venue-triggered group: the lane is BLOCKED until a Stop lets gw reset it. Not done yet: S1
   ARB (needs event exclusivity), C1 (needs the trained women's college basketball model), X1, LIP
   share estimates, the 4-hour TSan soak itself (run it on the Mac). An 8-minute soak after the
   fee fix: 745 quotes, 80 fills, 0 refusals, 0 violations.
   **Before turning lanes on, on the Mac (demo only):**
   `build-deskd/kalshi-demo-probe --handshakes 2 --feeds-check --group-check`. `--feeds-check`
   (reads only) lists the open markets of M1's and M2's series on demo (change them with
   `--lip-series` / `--band-series`, then `deskd.lip_series` / `band_series` to match), reads
   `pyth_value` for 15 s and says which 15-minute series' reference symbol demo relays, and checks
   the `order_group_updates` subscription. `--group-check` places one post-only $0.01 bid (15 s
   expiry) in a 1-contract group, triggers it (the bid must be cancelled, a second bid refused as
   soft_group), resets and deletes it, and wants `triggered` on the socket. Each prints NOT OK
   with the reason; a series with no open markets on demo or no `pyth_value` there means that
   lane has nothing to do on demo, not that deskd is broken.
   Next: the demo proof on the Mac (set manual_orders, small sizes), then latency timelines,
   per-shard token buckets, the journal, and positions from the execution ledger. Then: wire `gw/order_ledger` into the gateway
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
- Series metadata fills categories and fees for live/pinned/held markets; unvisited series keep
  prefix/category and QUAD x1 display fallbacks. Admission must require `rest::fee_ready(m,now)`
  before computing fees. The metadata worker uses at most 16 targeted requests per refresh:
  the unfiltered demo /series response measured 18.9 MB, above the normal HTTP body limit.
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
# Live-series fee readiness too:
python3 tools/deskd_read_probe.py --base http://127.0.0.1:PORT --require-private --require-fees --out build-deskd/demo-fees-read-capture.json
```

This changes only the probe's local book subscription and validates captured responses with
the frontend contract; it has no exchange write path. The captured token is never persisted.

## Edge research so far (`docs/edge_results.json`)

All after fees, on held-out data. Women's college basketball in-game (score model vs thin books, IOC, from November) is the only edge above the 2-4c auto-IOC bar: +16.5c a contract, about $1.60-2.60 a game, verified independently; depth and ESPN delay unknown. Weather: two resting-maker rules on daily highs, +1-2c, about $1-2 a day. Financial and sports: nothing above the bar; small structural plays only (last-seconds 97-99c sweep +1c, LIP about break-even, pregame 90-95c favourites +2-3c suggestive). Still to run: skeptic checks of the weather, financial and sports results, and the final ranking.
