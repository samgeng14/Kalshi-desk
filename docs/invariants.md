## Invariants (plan section 8)

Plan section 8: every kept or new rule of sections 2 and 3, plus three old lessons, has one test,
and every one is green before any live order. One row per rule, in the plan's words. `enforced in`
names the code (paths under `src/deskd/` and `include/deskd/` unless written in full); `test` names
the test function and, in brackets, its ctest name. Status is one of:

- `tested`: the test asserts the rule itself
- `partly`: some of it is asserted; the cell says what is not
- `MISSING`: no code, or no test
- `n/a here`: Polymarket-only or operator-only (the cell says why)

Run them with `ctest --test-dir build-deskd -L invariants --output-on-failure`; the label goes on
the ctest names listed at the end. Without node, `deskd-web` reports itself skipped (exit 77),
`deskd-order_ledger` fails, and the `ui-desk-v3*` entries are not registered at all, so run them
where node is installed. Section 4's limits and fail-closed rules are outside this table, as
section 8 asks only for sections 2 and 3.

Checked against commit 62ee42b on 2026-10-21 by reading the code and the test bodies; nothing was
built or run for this table. 85 rules: 45 tested, 20 partly, 10 MISSING, 10 n/a here. The same day
tests were added for S2-05, S2-10, S2-25, S2-31, S3-11, S3-33 and S3-36: now 50 tested, 17 partly,
8 MISSING, 10 n/a here.

Still MISSING, and why (the operator decides what must close before LIVE):
- S2-06 `cancel_epoch`, S2-09 the intents journal, S3-28 Manual order groups: not built. gw drains
  its inbox in order, KILL's cancel-all and leftover sweep cover what Manual groups would add, and
  an uncertain create is settled from the account read instead of a journal.
- S2-29 exit 75: the plan returns the supervisor only once deskd is the production binary.
- S3-42 `target_balance_allocation`: needs its demo behaviour first (S3-43).
- S3-29 a derived `contracts_limit`: fixed per lane (50/50/75) until lanes come from a file.
- S2-02 mutexes outside queues: kept where they guard short copies (no I/O under them); TSan is
  the check (the 4-hour soak).
- S2-36 cadence: rest reads every 12 s as planned; ESPN is read every 20-30 s, not 3-5 s (the
  recorder polls scores at 3-5 s for research; deskd shows them, it does not trade on them).

### Section 2: architecture

| # | rule | enforced in | test | status |
|---|---|---|---|---|
| S2-01 | Every piece of state has one owner thread; others get copies by message or read an immutable snapshot. | `gw/gateway.h` (gw-only state, published by `std::atomic_store`); `bus/queue.h` | `soak.cpp:main` (deskd-soak), in a `-DDESKD_TSAN=ON` build | partly: only ThreadSanitizer can catch a second writer, and only in a TSan build; the default 60 s deskd-soak run asserts nothing about ownership |
| S2-02 | Mutexes live only inside queues, never across I/O. | `bus/queue.h:BoundedQueue` | none | MISSING: no test. Mutexes also live outside queues (`gw/kill.cpp`, `net/kalshi_sender.h:GroupRegistry`, `net/pool.cpp`, `rest/state.cpp`, `web/streams.cpp`, `strat/*`), so nothing shows none is held across a network call |
| S2-03 | The gateway never blocks: every hand-off is a bounded push that fails at once when full. | `bus/queue.h:BoundedQueue::push`; `gw/gateway.cpp:Gateway::place_manual` | `test_bus.cpp:test_queue` (deskd-bus); `test_manual_gw.cpp:refusals` (full send-k1 inbox: RO, REJ, reservation freed) (deskd-manual_gw) | tested |
| S2-04 | Two inbox lanes: Kill, Stop, cancels and private events go before new orders. | `bus/msg.cpp:lane_of`; `bus/queue.h:BoundedQueue::pop_for`; KILL is no message (`gw/kill.h`) | `test_bus.cpp:test_lanes`, `test_queue` (deskd-bus) | tested |
| S2-05 | Draining a cancel, Stop or Kill also deletes matching new orders still queued. | `gw/gateway.cpp:Gateway::handle` (CancelIntent, Stop), `Gateway::sync_kill` | `test_bus.cpp:test_gateway_rules` (KILL deletes 100 queued orders, a cancel deletes the click, Stop deletes a queued Start) (deskd-bus); `test_web.cpp:test_control_waits` (deskd-web); `test_bus.cpp:test_gateway_rules` (a Stop deletes the SYS order queued before it, not the Manual one) (deskd-bus) | tested |
| S2-06 | Each PlaceIntent carries its market's and lane's cancel count, so gw drops stale ones. | `bus/msg.h:PlaceIntent::cancel_epoch` (declared only) | none | MISSING: nothing sets or reads `cancel_epoch`; an intent made before a cancel but queued after gw drained it is not dropped |
| S2-07 | A click followed at once by Esc never rests. | `gw/gateway.cpp:Gateway::handle` (erase_if), `Gateway::cancel_manual`; `gw/manual_book.cpp:ManualBook::revoke`; `net/kalshi_sender.cpp:KalshiSender::admission` (permit checked before any byte) | `test_bus.cpp:test_gateway_rules` (deskd-bus); `test_sender.cpp:permits_and_kill` (revoked permit: USER, zero requests) (deskd-sender) | tested |
| S2-08 | Outgoing queues are fixed-size: a full one drops events and marks a gap; the page refetches its snapshot. | `bus/queue.h:BoundedQueue::push`; `web/sse.cpp:StreamQueue::offer`; `web/pub.cpp:Pub::post`; the page's store (`ui/desk`) | `test_bus.cpp:test_queue` (deskd-bus); `test_web.cpp:test_stream_queue_gap` (deskd-web); `test_pub_data.cpp:upstream_overflow_is_visible` (deskd-pub_data); `test_ui_desk_v3.cjs` part 5, "a skipped seq is a gap" (ui-desk-v3) | tested |
| S2-09 | jrnl: the intents journal (plain `write()`, no fsync); gw journals each order before its job goes to the sender. | none | none | MISSING: there is no jrnl worker or journal in src/deskd |
| S2-10 | If store falls behind, records wait in the journal and an alert fires. | `gw/gateway.cpp:Gateway::record` (counts `records_lost`, alerts WARN STORE at most once a minute) | `test_manual_gw.cpp:store_behind` (deskd-manual_gw) | partly: the alert fires and the loss is counted; the rows are not journalled (no jrnl, S2-09) |
| S2-11 | web waits at most 50 ms for gw, then answers "queued". | `web/server.cpp` (`kGwWaitMs` = 50 on Start, Stop, lane, release) | `test_web.cpp:test_control_waits` (gw frozen: Stop and Start answer "queued") (deskd-web) | partly: the bound asserted is 500 ms, not 50; POST /api/order waits up to 3 s (`kOrderWaitMs`) for the venue, which the rule does not allow for |
| S2-12 | Kill has its own path: the route sets an atomic flag and wakes send-cx, which triggers every group and sends cancel-all without gw. | `gw/kill.cpp:KillSwitch::trip`; `net/kalshi_sender.cpp:KalshiSender::take_kill` | `test_web.cpp:test_kill_with_gw_frozen`, `test_kill_with_cx_full` (deskd-web); `test_bus.cpp:test_cancel_sender_kill` (deskd-bus); `test_sender.cpp:permits_and_kill`, `systematic_role` (deskd-sender) | tested |
| S2-13 | send-cx has its own connections and its own copy of every order-group id. | `net/kalshi_sender.h:GroupRegistry` (gw adds each group it creates); `app/venue.cpp` (send-cx's own pool) | `test_sender.cpp:systematic_role` (KILL triggers the registered group) (deskd-sender); `test_runtime_manual.cpp:systematic` (KILL triggers M2's groups) (deskd-runtime_manual) | tested |
| S2-14 | ops writes the heartbeat only if gw, md-k and priv-k each advanced a progress counter in the last 5 s. | `app/ops.cpp:HeartbeatRule::withheld`, `Ops::run` | `test_ops.cpp:test_heartbeat_rule`, `test_ops_thread` (deskd-ops) | tested |
| S2-15 | `deskctl kill` does the same if deskd hangs. | `app/deskctl.cpp:kill_direct`, `kill` | `test_sender.cpp:deskctl_direct` (deskd-sender); `test_process.cpp:main` (deskd-process) | tested |
| S2-16 | Prices are integers in 1/10,000 of a dollar; counts in 1/100 of a contract. | `bus/units.cpp:parse_price`, `qty_from_number`; `gw/kalshi_wire.cpp` | `test_bus.cpp:test_units` (deskd-bus); `test_wire.cpp:test_values` (deskd-wire) | tested |
| S2-17 | gw validates, guards, reserves and registers the cid in one turn, before the job goes to the sender. | `gw/manual_book.cpp:ManualBook::place`; `gw/order_ledger.cpp:OrderLedger::insert` | `test_manual_book.cpp:places_and_reserves` (deskd-manual_book) | tested |
| S2-18 | Click to acknowledgement: pass at p90 under 100 ms. | `gw/gateway.cpp:Gateway::note_latency`; `tools/latency_report.py` | `test_runtime_manual.cpp:orders_cancel_kill` checks only that the figures exist | n/a here: a demo and live measurement (plan section 9, week 1); loopback figures prove nothing about it |
| S2-19 | send-k1 is Manual only; Systematic never uses send-k1 or send-cx. | `net/kalshi_sender.cpp:KalshiSender::admission` (role against cid prefix); `gw/gateway.cpp:Gateway::send_cancel` (Systematic cancels to send-k2) | `test_sender.cpp:permits_and_kill` (an S order on send-k1: BAD_REQUEST, no request), `systematic_role` (deskd-sender) | partly: the send-cx half rests on `Gateway::send_cancel`'s routing alone (send-cx takes any cid); no test checks a Systematic cancel goes out on send-k2 |
| S2-20 | One pre-opened TLS connection per sender. | `net/pool.cpp:SparePool::take`; `app/venue.cpp` (a pool per sender) | `test_net.cpp:test_pool` (deskd-net) | tested |
| S2-21 | Every write carries a client_order_id, prefixed `M` or `S<lane>-`, so ownership survives a crash. | `gw/kalshi_wire.cpp:check_create`; `gw/manual_admission.cpp:admit_manual` (cid_ok); `net/kalshi_sender.cpp:KalshiSender::admission` | `test_wire.cpp:test_builders` (deskd-wire); `test_manual_admission.cpp:fees_and_bad_inputs` (deskd-manual_admission); `test_runtime_manual.cpp:systematic`, `systematic_book` (a restart keeps the lane's positions) (deskd-runtime_manual) | tested |
| S2-22 | An unprefixed open order is foreign: shown, its market blocked until you cancel it. | `app/projection.cpp:rest_order_json` (own EXT); `gw/manual_book.cpp:ManualBook::context`; `gw/manual_admission.cpp:admit_manual` (RO); `gw/gateway.cpp:Gateway::cancel_foreign` | `test_rest.cpp:discovery` (deskd-rest); `test_manual_book.cpp:reducing_room` (deskd-manual_book); `test_runtime_manual.cpp:foreign_cancels`, `systematic_foreign` (deskd-runtime_manual); `test_ui_desk_v3_manual.cjs` "a foreign working order is flagged EXT" (ui-desk-v3-manual) | tested (stricter than the plan: any resting order this run did not place blocks its market, prefixed or not) |
| S2-23 | 5xx, timeout or no response is UNKNOWN, held in a table by cid (a second never overwrites the first). | `gw/kalshi_wire.cpp:classify_write`; `gw/order_ledger.cpp:OrderLedger::unknown` | `test_wire.cpp:test_classify`, `round_trip` (deskd-wire); `test_sender.cpp:writes_and_faults` (deskd-sender); `test_order_ledger.cpp:unknown_table_and_proof` (deskd-order_ledger) | tested |
| S2-24 | A fill during a cancel isn't lost: it raises cumulative filled, not the position. | `gw/order_ledger.cpp:OrderLedger::cancelled` (reduced_by) | `test_order_ledger.cpp:races`, `fractional_and_permutations` (deskd-order_ledger); `test_runtime_manual.cpp:systematic_races` (deskd-runtime_manual) | tested |
| S2-25 | Post-only cross and collateral refusals are soft. | `gw/reject.cpp:classify_refusal`; `gw/reject.h:reject_trips_kill` | `test_sender.cpp:writes_and_faults` (cross: REFUSED, KILL clear) (deskd-sender); `test_manual_gw.cpp:fills_and_rests` (CXL POST_CROSS) (deskd-manual_gw); `test_wire.cpp:test_classify`, `round_trip` (deskd-wire); `test_manual_book.cpp:replies` (deskd-manual_book); `test_wire.cpp:test_soft_refusals` (cash, self-trade, cross, closed, triggered group: soft and KILL clear) (deskd-wire) | tested |
| S2-26 | One write per connection, never silently retried. | `net/pool.cpp:SparePool::take`; `net/kalshi_sender.cpp:KalshiSender::execute` | `test_net.cpp:test_one_request_per_connection`, `test_pool` (deskd-net); `test_sender.cpp:writes_and_faults` (one request per faulted create) (deskd-sender); `test_wire.cpp:round_trip` (deskd-wire) | tested |
| S2-27 | Kept: reference_feed's Pyth gap rule (8x the median update interval, 2 s floor). | `ref/pyth.cpp:PythValues::gap_limit_ms`, `fresh` | `test_pyth.cpp:reducer` (deskd-pyth) | tested |
| S2-28 | Kept: the replication gate (50 consecutive windows whose published strike the feed reproduces). | `strat/fair.cpp:FinancialFair::step` | `test_fair.cpp:replication_gate` (deskd-fair) | partly: the gate is only seen shut (0 consecutive); no test reaches 50 and sees it open |
| S2-29 | Kept: the heartbeat with exit 75. | heartbeat: S2-14; exit 75: nowhere in src/deskd (the old desk's `src/main.cpp`) | none | MISSING: deskd never exits 75 (the old desk's "storage failing, restart me") |
| S2-30 | `desk_supervise.sh` and the LaunchAgent return, pointed at deskd, only once it is the production binary. | `scripts/desk_supervise.sh` | none | n/a here: the operator's switch-over |
| S2-31 | deskd leaves out perps, research modes, lane stages, the manual queue and the old strategies. | `CMakeLists.txt` (deskd_core shares nothing with the desk library) | `tests/deskd/no_legacy_includes.cmake` (deskd-no-legacy-includes) | tested |
| S2-32 | The frozen code gets no other work and goes on Nov 22; dead files and `config/desk.json.bak-20260906` (a key ID) leave git. | the repository | none | n/a here: schedule and repo hygiene; at 62ee42b none of `crossvenue.*`, `polymarket.*`, `ui/index.html.bak-033631`, `config/desk.json.bak-20260906` is tracked |
| S2-33 | net/: a TLS connection is replaced after use or 20 s idle. | `net/pool.cpp` (`PoolConfig::idle_replace_ms` = 20000, `lead_ms`) | `test_net.cpp:test_pool` (deskd-net) | partly: retirement is exercised at 300 ms; the 20 s default is not asserted |
| S2-34 | A body cut off mid-way is UNKNOWN, never "failed" (responses go through a tested parser). | `net/http.cpp:ResponseParser` (deskd's own strict parser, not llhttp); `gw/kalshi_wire.cpp:classify_write` | `test_http.cpp:test_cuts`, `every_split` (deskd-http); `test_net.cpp:test_faults_over_tls` (deskd-net); `test_sender.cpp:writes_and_faults` (deskd-sender) | tested |
| S2-35 | Builds use `nice -n 15`, `-j 4` at most, never `build/`. | operator practice | none | n/a here: operator |
| S2-36 | rest reconciles and reads balances every 10-15 s; poll reads scores every 3-5 s with first-seen times. | `rest/state.h` (`poll_ms` = 12000); `ref/espn.h` (`scoreboard_ms` = 20000, `summary_ms` = 30000) | `test_espn.cpp:feed` asserts the 20-30 s pacing (deskd-espn) | MISSING: no test pins the REST cadence, and deskd polls scores every 20-30 s, not 3-5 s (the recorder's 3-5 s poll is section 7's) |

### Section 3: order handling

| # | rule | enforced in | test | status |
|---|---|---|---|---|
| S3-01 | Take: IOC at the touch plus a slippage cap (1 tick default, "fills up to X"); FOK for all-or-none. | the page's ticket (the price sent is the cap); `gw/kalshi_wire.cpp:from_intent` | `test_ui_desk_v3_manual.cjs` "BUY at the ask: IOC up to one tick more (the slippage cap)" (ui-desk-v3-manual); `test_manual_admission.cpp:basics` (deskd-manual_admission); `test_order_ledger.cpp:cumulative_is_not_execution` (FOK all or none) (deskd-order_ledger) | tested (gw takes the page's price as the cap; `PlaceIntent::slip_ticks` is unused) |
| S3-02 | Post: `post_only` + `good_till_canceled` + `expiration_time` + `cancel_order_on_pause`; no GTC without an expiry. | `gw/kalshi_wire.cpp:from_intent`, `check_create` | `test_manual_admission.cpp:expiry` (deskd-manual_admission); `test_wire.cpp:test_builders` (deskd-wire) | tested |
| S3-03 | A client cancel at TTL + 1.5 s backs up the exchange's expiry. | `gw/gateway.cpp:Gateway::expiry_sweep` (`kExpiryBackupMs`) | `test_manual_gw.cpp:expiry_backup` (ends between expiry + 1.5 s and + 2 s, once; a refused backup is asked again) (deskd-manual_gw) | tested (on a Manual post; Systematic posts go through the same sweep) |
| S3-04 | Self-trade: Manual `maker` (your click cancels our quote); Systematic `taker_at_cross`. | `gw/kalshi_wire.cpp:from_intent` | `test_wire.cpp:test_builders` (deskd-wire); `test_manual_admission.cpp:basics` (deskd-manual_admission) | tested |
| S3-05 | Flatten: IOC with `reduce_only`, which Kalshi allows only on IOC; never FOK. | `gw/manual_admission.cpp:admit_manual`; `gw/kalshi_wire.cpp:check_create`; `strat/maker.cpp:maker_quote` (T-60 s) | `test_manual_admission.cpp:cash_and_reducing` (reduce_only FOK refused) (deskd-manual_admission); `test_wire.cpp:test_builders` (deskd-wire); `test_maker.cpp:engine_own_quotes` (the flatten is IOC reduce_only) (deskd-maker) | tested |
| S3-06 | Polymarket: Take LIMIT + IOC or FOK; Post LIMIT + GTD + `participateDontInitiate`; Flatten IOC. | `web/server.cpp` order route refuses venue P | none | n/a here: Polymarket is not wired |
| S3-07 | Expiry is in whole Unix seconds (an order dies N-1 to N s after sending). | `gw/manual_admission.cpp:admit_manual`; `gw/kalshi_wire.cpp:from_intent` | `test_manual_admission.cpp:expiry` (deskd-manual_admission); `test_wire.cpp:test_builders` (deskd-wire) | tested |
| S3-08 | The minimum lead time is undocumented: test on demo, start at 2 s. | `gw/manual_admission.cpp:admit_manual` (TTL and absolute expiry at least 5 s) | `test_manual_admission.cpp:expiry` (2 s refused, 5 s accepted) (deskd-manual_admission) | partly: the floor is 5 s, not the plan's 2 s; the demo measurement (`kalshi-demo-probe --venue-checks`, "shortest expiry") is not a ctest |
| S3-09 | NTP is required: an untrusted clock blocks Post. | `rest/state.cpp:ClockCheck::estimate` (offset interval inside +/-500 ms); `gw/manual_admission.cpp:admit_manual`; `strat/maker.cpp:MakerEngine::step` | `test_rest.cpp:clock_checks` (deskd-rest); `test_manual_admission.cpp:expiry` (deskd-manual_admission); `test_manual_book.cpp:staleness` (deskd-manual_book); `test_maker.cpp:engine_limits` (deskd-maker) | tested (keeping NTP on is the operator's) |
| S3-10 | After expiry the quoter re-prices from the live touch. | `strat/maker.cpp:MakerEngine::step` | `test_maker.cpp:engine_own_quotes` (deskd-maker); `test_runtime_manual.cpp:systematic_maker` (deskd-runtime_manual) | partly: no test lets a quote expire and checks the next one follows a moved touch (re-quoting is shown after a fill and after a touch move) |
| S3-11 | Every order, LIP included, lives at most 60 s. | `gw/manual_admission.cpp:admit_manual` (Systematic TTL 5-60 s, expiry at most now + 61 s); `strat/maker.h` (`MakerParams::ttl_s` = 60) | `test_maker.cpp:engine_limits` (the maker posts ttl_s 60) (deskd-maker); `test_manual_admission.cpp:expiry` (SYS 60 s ok, 61 s and until-close refused, relative and absolute) (deskd-manual_admission) | tested (Systematic; Manual's 5 min, 30 min and until-close are the operator's) |
| S3-12 | No amends in the first live weeks: a re-price is a cancel plus a new post_only order. | `web/server.cpp` (`/api/amend` answers NOT_FOUND for every cid); `gw/gateway.cpp:Gateway::handle` ignores AmendIntent; `strat/maker.cpp` | `test_web.cpp:test_guards_and_page` (`/api/amend` 404) (deskd-web); `test_maker.cpp:engine_own_quotes` (a re-price is a CancelIntent) (deskd-maker) | partly: the 404 is asserted for an unknown cid only, never for a working order |
| S3-13 | An expiry-only amend resends the current price and total quantity, built from gw's latest order state, never strat's copy. | `bus/msg.h:AmendIntent` (comment only) | none | n/a here: amends are off until demo proves them (weeks 7-8); the test comes with them |
| S3-14 | Improve one tick when the spread is 2+ ticks; at exactly 2, only on the side fair or inventory favours. | `strat/maker.cpp:maker_quote` | `test_maker.cpp:quotes` (a 6-tick book: both sides one tick inside) (deskd-maker) | partly: no exactly-2-tick case |
| S3-15 | Never join the back of a 1-tick queue deeper than 3x our size unless fair clears the price by the edge floor (or simulation shows the series pays). | `strat/maker.cpp:maker_quote` (deep_join; `MakerParams::deep_join_x` = 3, 0 for a series simulation clears) | `test_maker.cpp:deep_join` (deskd-maker) | tested |
| S3-16 | Positions move only on fill messages, de-duplicated by `trade_id`. | `gw/order_ledger.cpp:OrderLedger::fill`; `gw/sys_book.cpp:SysBook::fill`; `gw/gateway.cpp:Gateway::fill_applied` | `test_order_ledger.cpp:rest_fill_duplicates`, `races` (deskd-order_ledger); `test_sys_book.cpp:settle_and_restore` (deskd-sys_book); `test_runtime_manual.cpp:systematic_book` (deskd-runtime_manual); `test_manual_gw.cpp:missed_fill_from_account_read` (deskd-manual_gw) | tested (Manual's shown position is the venue's REST position) |
| S3-17 | The 201's `fill_count`, `user_orders` and `reduced_by` only raise the order's cumulative filled (the largest any source reports). | `gw/order_ledger.cpp:OrderLedger::acknowledged`, `observed`, `cancelled` | `test_order_ledger.cpp:cumulative_is_not_execution`, `races`, `zero_cancel_needs_read` (deskd-order_ledger); `test_manual_gw.cpp:fills_and_rests` (deskd-manual_gw) | tested |
| S3-18 | If cumulative filled exceeds summed fills for 5 s, a REST read of that ticker's fills runs. | `gw/order_ledger.cpp:OrderLedger::fills_due`; `gw/gateway.cpp:Gateway::reconcile_working` | `test_order_ledger.cpp:races` (due at 5,006 ms, not 5,005) (deskd-order_ledger); `test_manual_gw.cpp:missed_fill_from_account_read` (deskd-manual_gw) | partly: that gw asks rest for a read is not asserted (the test publishes reads by hand), and the read is the whole account, not that ticker's fills |
| S3-19 | A fill turns its reservation into worst case in the same step. | `gw/gateway.cpp:Gateway::fill_applied` (ledger and lane book in one gw turn); `gw/order_ledger.cpp:Order::reservation` | `test_order_ledger.cpp:races` (deskd-order_ledger); `test_runtime_manual.cpp:systematic_maker` (deskd-runtime_manual) | partly: no test checks Systematic's used worst case never dips between releasing the reservation and booking the position |
| S3-20 | `order_id` maps to cid from the 201 or `user_orders`. | `gw/order_ledger.cpp:OrderLedger::bind` | `test_order_ledger.cpp:fractional_and_permutations`, `bad_evidence` (deskd-order_ledger); `test_manual_gw.cpp:unknown_settled_by_account_read` (deskd-manual_gw) | tested |
| S3-21 | Private events that beat the 201 wait up to 5 s. | `gw/gateway.cpp:Gateway::manual_private`, `retry_orphans` (`kOrphanWaitMs` = 5000) | `test_manual_gw.cpp:fill_before_201` (deskd-manual_gw); `test_runtime_manual.cpp:systematic_races` (deskd-runtime_manual) | partly: the 5 s cap (an orphan given up after 5 s) is not asserted |
| S3-22 | A cancel for an order still in flight goes out once its order_id is known; the exchange expiry is the backstop. | `gw/gateway.cpp:Gateway::flush_cancel` | `test_manual_gw.cpp:unknown_settled_by_account_read` (deskd-manual_gw); `test_runtime_manual.cpp:systematic_races` (Stop overtakes a create on the wire) (deskd-runtime_manual) | tested |
| S3-23 | UNKNOWN orders sit in a table by cid, no limit. | `gw/order_ledger.cpp` (orders by cid) | `test_order_ledger.cpp:unknown_table_and_proof` (5,000 of them) (deskd-order_ledger) | tested |
| S3-24 | An UNKNOWN clears when `user_orders` or a fill reports it. | `gw/order_ledger.cpp:OrderLedger::observed`, `fill`; `gw/gateway.cpp:Gateway::settle_uncertain` | `test_manual_gw.cpp:unknown_settled_by_account_read` (deskd-manual_gw); `test_order_ledger.cpp:zero_cancel_needs_read` (deskd-order_ledger) | partly: no test sends a fill for an order already UNKNOWN (`races` has only a fill that beat the timeout) |
| S3-25 | An order with an expiry clears at `expiration_time` + 5 s, after a paginated read of that ticker's orders and fills since 5 s before the send. | `gw/order_ledger.cpp:OrderLedger::resolve_absent`; `gw/gateway.cpp:Gateway::settle_uncertain` | `test_order_ledger.cpp:unknown_table_and_proof` (deskd-order_ledger) | tested |
| S3-26 | The 2-minute complete-history rule stays only for orders without an expiry. | `gw/order_ledger.cpp:OrderLedger::resolve_absent` | `test_order_ledger.cpp:unknown_table_and_proof` (deskd-order_ledger); `test_manual_gw.cpp:unknown_settled_by_account_read` (an IOC) (deskd-manual_gw) | tested |
| S3-27 | Each lane has an order group per shard (groups don't span shards). | `gw/sys_book.cpp:SysBook::group`, `groups_ready`; `gw/gateway.cpp:Gateway::sys_tick`, `place_sys` (RO without an active group) | `test_sys_book.cpp:lanes_and_groups` (deskd-sys_book); `test_runtime_manual.cpp:systematic`, `systematic_group_trigger` (deskd-runtime_manual) | tested |
| S3-28 | Manual has one order group per shard, at the maximum. | none | none | MISSING: Manual creates carry no `order_group_id` (`gw/manual_book.cpp:ManualBook::place`), and send-k1 takes no group job |
| S3-29 | A lane's `contracts_limit` is about 2.5 x per-side size x markets in the group. | `gw/sys_book.cpp:default_lanes` (fixed: 50 contracts for M1 and M2, 75 for P1) | none | MISSING: fixed per lane, not derived from size and markets; no test |
| S3-30 | Kill: flag set, gw refuses new orders, send-cx triggers every group, cancel-all, batch cancel of anything still listed, Polymarket open orders cancelled. | `gw/kill.cpp:KillSwitch::trip`; `gw/gateway.cpp:Gateway::sync_kill`; `net/kalshi_sender.cpp:KalshiSender::take_kill`, `sweep_resting` | `test_bus.cpp:test_gateway_rules` (deskd-bus); `test_manual_gw.cpp:refusals` (deskd-manual_gw); `test_sender.cpp:permits_and_kill`, `systematic_role`, `kill_sweeps_leftovers` (deskd-sender); `test_runtime_manual.cpp:orders_cancel_kill` (deskd-runtime_manual) | partly: send-cx sends the cancel-all first and triggers the groups after it (the plan has the groups first), and no test pins the order; the Polymarket step is not wired |
| S3-31 | Cancel-all also cancels orders placed in the next minute, which fits the 60 s lockout. | `gw/kill.cpp:KillSwitch::cancel_all_done`, `rearm` | `test_bus.cpp:test_kill_switch` (deskd-bus); `test_sender.cpp:permits_and_kill` (deskd-sender); `test_web.cpp:test_kill_with_gw_frozen` (deskd-web) | tested |
| S3-32 | Stop: gw clears the running flag, so late Systematic intents are refused. | `gw/gateway.cpp:Gateway::handle` (Stop), `place_sys` (BAD_STATE) | `test_runtime_manual.cpp:systematic` (deskd-runtime_manual); `test_bus.cpp:test_gateway_rules` (deskd-bus) | tested |
| S3-33 | Stop triggers Systematic's groups and batch-cancels Systematic's order ids only; no cancel-all, which could cancel your next Manual orders. | `gw/gateway.cpp:Gateway::handle` (Stop: `cancel_sys`, `trigger_groups`) | `test_runtime_manual.cpp:systematic` (n = 1, nothing rests), `systematic_maker` (deskd-runtime_manual); `soak.cpp:main` (deskd-soak); `test_runtime_manual.cpp:stop_keeps_manual` (a Manual order rests on, the groups are triggered, no cancel-all) (deskd-runtime_manual) | tested |
| S3-34 | Re-arm and Start reset the groups and wait for `order_group_updates` to confirm. | `gw/gateway.cpp:Gateway::sync_kill` (reset_groups), `group_event`, `sys_tick`; the GROUPS preflight row | `test_runtime_manual.cpp:systematic`, `systematic_group_trigger` (`group_reset_fallbacks` == 0) (deskd-runtime_manual); `test_bus.cpp:test_gateway_rules` (PREFLIGHT) (deskd-bus) | partly: gw takes a reset's 200 as confirmation after 5 s without the socket's word (`kGroupResetWaitMs`), which the plan does not allow and no test exercises |
| S3-35 | Soft, never Kill: "order group triggered" refusals. | `gw/reject.cpp:classify_refusal` (SOFT_GROUP); `gw/gateway.cpp:Gateway::manual_reply` | `test_wire.cpp:test_classify`, `round_trip` (deskd-wire); `test_sender.cpp:systematic_role` (SOFT_GROUP, KILL clear, sender healthy) (deskd-sender) | tested |
| S3-36 | Soft, never Kill: a self-trade cancel or refusal on a Systematic order (`reject.h` no longer treats self-trade as hard). | `gw/reject.cpp:classify_refusal` (SOFT_SELF_TRADE); `gw/manual_book.cpp:ManualBook::refusal_code` (STP) | `test_wire.cpp:test_classify` (deskd-wire); `test_manual_book.cpp:replies` (deskd-manual_book); `test_wire.cpp:test_soft_refusals` (deskd-wire) | partly: classification tested end to end in deskd-wire; no self-trade against the fake through gw |
| S3-37 | Write budget: one bucket per shard; Systematic may use 80%. | `gw/write_budget.cpp:WriteBudget::sys_may`; `gw/gateway.cpp:Gateway::place_sys` (RATE) | `test_write_budget.cpp:basic`, `advanced` (deskd-write_budget); `test_runtime_manual.cpp:systematic` (cap 100, share 0.8) (deskd-runtime_manual) | tested |
| S3-38 | Basic gives 100 tokens a second (create 10, cancel 2). | `gw/write_budget.h` | `test_write_budget.cpp:basic` (deskd-write_budget) | tested |
| S3-39 | Run the free Advanced upgrade (300) before going wider. | `app/config.cpp` (`write_tier`) | `test_write_budget.cpp:advanced` covers the arithmetic | n/a here: operator |
| S3-40 | Polymarket: asynchronous submission, GTD, post-only via `participateDontInitiate`, `manualOrderIndicator` MANUAL only for your clicks; with no client id, list a market's open orders and trades after any uncertain write. | none | none | n/a here: Polymarket-only, not wired |
| S3-41 | Polymarket: maker fills need 2-5 contracts (fees round per fill); 25 requests a second per IP; skip Thursday 02:00-06:00 ET maintenance. | none | none | n/a here: Polymarket-only, not wired |
| S3-42 | Read `target_balance_allocation` at startup and set Systematic's targets at Start, so Kalshi rebalances instead of a sweeper of ours. | none | none | MISSING: nothing in src/deskd reads or sets `target_balance_allocation`, and the demo probe's venue checks do not cover it |
| S3-43 | If demo shows it misbehaving: require it empty and fall back to transfers at most every 5 minutes. | `gw/gateway.cpp:Gateway::transfer` (the operator's MOVE only) | none | n/a here: depends on S3-42's demo result; no automatic transfer exists, and the MOVE has no 5-minute spacing |
| S3-44 | A market's shard comes from its `exchange_index`, never a table. | `gw/manual_admission.cpp:admit_manual`; `gw/gateway.cpp:Gateway::place_sys`; `gw/kalshi_wire.cpp:check_create` | `test_manual_admission.cpp:basics`, `gates` (deskd-manual_admission); `test_wire.cpp:test_builders` (deskd-wire) | tested |
| S3-45 | A short shard turns the ticket amber with a one-click "move $X from shard N". | the page's ticket; `web/server.cpp` (`/api/transfer`); `gw/gateway.cpp:Gateway::transfer` | `test_ui_desk_v3_manual.cjs` "a short shard offers MOVE from the richest shard" (ui-desk-v3-manual); `test_runtime_manual.cpp:shard_preflight` (deskd-runtime_manual) | tested (the amber colour itself is not asserted) |

### Old lessons

| # | rule | enforced in | test | status |
|---|---|---|---|---|
| OL-01 | An empty subscription list is refused. | `md/subscribe.cpp:kalshi_subscribe`; `md/kalshi_md.cpp:KalshiMd::set_watch`; `recorder/kalshi.py:subscribe_cmd` | `test_ops.cpp:test_subscribe_guard` (deskd-ops); `test_md.cpp:socket_integration` (deskd-md); `recorder/tests/test_kalshi.py:CommandTest.test_explicit_lists`, `recorder/tests/test_rec.py:RecorderListsTest.test_sync_explicit_lists` (recorder-unit) | tested |
| OL-02 | SIGPIPE is ignored (it killed the process on a TLS reconnect). | `app/runtime.cpp:Deskd::start`; `app/deskd.cpp:main`; `net/conn.cpp:ConnFactory::open` (SO_NOSIGPIPE where the OS has it) | `test_web.cpp:test_client_reset_in_process` (deskd-web); `test_process.cpp:main` (a stream client vanishes, the real binary lives on) (deskd-process); `test_net.cpp:test_no_sigpipe` (deskd-net) | tested (on Linux `test_no_sigpipe` runs its TLS half with SIG_IGN set; the SIG_DFL case runs on macOS only) |
| OL-03 | Frames past 8,192 bytes arrive whole (the cut-off happened). | `net/ws.cpp:WsParser::feed` (accumulates across reads), `WsClient::read` | `test_md.cpp:framing` (8,193, 65,535, 65,536 and 200,000 bytes fed in 1-, 4,096-, 8,192- and 300,000-byte pieces) (deskd-md) | tested |
| OL-04 | Frames past 4,096 bytes arrive whole (the cut-off happened). | `net/ws.cpp:WsParser::feed`, `WsClient::read` | `test_md.cpp:framing` (4,097 bytes, same pieces) (deskd-md) | tested |

### ctest names to label `invariants`

Each holds at least one `tested` row above (the names are CMake's: `deskd-` plus the test file's
own spelling, so `deskd-manual_gw`, not `deskd-manual-gw`):

deskd-bus, deskd-http, deskd-manual_admission, deskd-manual_book, deskd-manual_gw, deskd-maker,
deskd-md, deskd-net, deskd-ops, deskd-order_ledger, deskd-process, deskd-pub_data, deskd-pyth,
deskd-rest, deskd-runtime_manual, deskd-sender, deskd-sys_book, deskd-web, deskd-wire,
deskd-write_budget, recorder-unit, ui-desk-v3, ui-desk-v3-manual, deskd-no-legacy-includes.

Not labelled, as they hold only `partly` or `MISSING` rows: deskd-fair (S2-28), deskd-soak (S2-01,
S3-33), deskd-espn (S2-36).

# deskd verification ledger

## Stage 2 — 2026-10-08

- Every public WS connection and reconnect uses the existing guarded ConnFactory. The market
  data test proves a production-host attempt without live permission opens zero sockets.
- Explicit subscriptions only. Empty lists are refused; clearing the watchlist closes the old
  subscriptions rather than issuing an exchange-wide subscribe.
- Sequence continuity is per subscription ID across all tickers and numbered control replies.
  A gap invalidates all books on that subscription; new snapshots are required before KX clears
  its gap. Quiet books stay healthy through verified ping/pong rather than price-age guesses.
- RFC6455 framing tests cover fragmented messages, interleaved control frames, masked client
  messages, oversized and invalid frames, UTF-8, and messages above 64 KiB.
- An account refresh is published only after complete cursor pagination. Failure retains the
  previous values but marks the account unready. Repeated cursors and page caps are failures.
- Clock checks retain HTTP Date's whole-second quantization and network uncertainty. One
  coarse sample cannot certify Post; the full offset interval must fit inside +/-500 ms.
- Publisher snapshots and stream counters share one owner. FULL/TOP depth is per tab, books
  replace both sides atomically, fills deduplicate, and order revisions cannot regress.
- A publisher inbox overflow is distinguishable from a slow tab: the header becomes read-only
  and streams reconnect to show the restart-required alert. A slow stream alone keeps the
  existing sequence-gap/resnapshot mechanism.
- Missing exchange_index stays null. Missing quote sides stay empty/null. Unknown model values
  stay null, and no listing is labelled as paying LIP from its ticker alone.
- Daily weather uses the named observation date, so a next-day trading close does not move
  today's weather to tomorrow. REST lifetime realized P&L is not labelled as today's P&L.

Demo read verification used only the demo credential and sent zero exchange orders. Actual
snapshot, book subscription and T/T+1 universe responses passed contract 1.5.0 validation.
Observed cash was $500 on S0, with zero on S1–S3; eight subscribed books were healthy. The
private feed remained down as expected for this stage. Manual, latency and expiry proof gates
are still pending. Browser visual verification remains pending because access was declined.

Validation: `nice -n 15 ninja -C build-deskd -j 4 deskd-all` completed without warnings;
`ctest --test-dir build-deskd -R 'deskd-|ui-desk' --output-on-failure --timeout 240`
passed all 16 suites in 253 seconds. The process/socket suites were run outside the sandbox
after the sandboxed ops run stalled; the bounded rerun passed.

## Stage 3 private-feed prerequisite — 2026-10-08

- Private account channels subscribe without market filters and use the guarded signed socket.
  Subscription acknowledgements and matching ping/pong are required; reconnects invalidate
  account readiness. These private channels have no sequence field, unlike public books.
- A generation captured before the first account REST request must still match after every
  account page succeeds. Every private order/fill and every reconnect changes that generation.
  Failed fencing requests another read (throttled to at most one attempt per second).
- Private fills retain exact centi-contract quantities and micro-dollar fees. trade_id is the
  stable execution identity; malformed units, missing identity and invalid sides are refused.
- Until gateway event accounting is connected, each private event requires REST reconciliation.
  The header stays read-only even when both feeds are healthy.

The seven focused private/REST/MD/publisher/web/process/ops suites pass. A demo run using
`deskd_read_probe.py --require-private` passed five contract checks with eight live books,
KX and KXP up without gaps, and no exchange orders sent. The observed clock offset was -44 ms;
this is read-path evidence, not a click-to-ack measurement. Manual submission remains pending.

## Stage 3 order-state core — 2026-10-08

- Each cid retains its own state; a second timeout cannot overwrite the first UNKNOWN order.
  UNKNOWN is PEND with an empty reason, retains its reservation, and blocks its market/shard.
- Cids are never reused. Exchange ids cannot be rebound to another cid. Contradictory identity,
  quantity or execution evidence latches the ledger unhealthy and blocks subsequent inserts.
- Only a new trade_id returns APPLIED for execution accounting. REST and WS copies may have
  different fill ids and timestamp precision; conflicting economic details fail closed.
  Ack/order/cancel cumulative fill counts take the maximum and never duplicate executions.
- Terminal orders cannot be resurrected by old REST rows or a late create response. A full
  execution still wins a cancel race. A cancel requested before the order id arrives persists.
- Reservations remain on reported but undelivered fills, including terminal orders. After five
  seconds these orders require a fills read. Zero-reduction cancel responses do not prove zero
  fills; they keep the order uncertain until an authoritative order state arrives.
- Absence proof requires both complete histories, the right market/shard, coverage starting
  at least five seconds before send, and a read begun after expiry + 5 seconds (or send + two
  minutes without expiry). A read that merely finishes after the deadline does not qualify.
- Integer fractional quantities and upward-rounded proportional reservations preserve each
  shard's collateral. Tests cover all 24 full-fill and 120 partial-fill/cancel event permutations,
  duplicate and contradictory fills, stale snapshots, malformed counts, and 5,000 UNKNOWN ids.

This is a standalone gateway-owned state component. It is not wired to runtime submission;
positions, fee admission, persistence, senders and web order responses remain to integrate.

Validation: all 18 deskd/frontend suites passed (242 seconds); after the final zero-reduction
cancel/late-ack correction, `deskd-all` rebuilt without warnings and the ledger suite passed
again. The final targeted rerun covers the new partial and complete late-ack cases as well.

## Stage 3 send workers — 2026-10-09

- Manual and cancels use separate queues, threads and preopened connection pools. Every write
  consumes its connection. There are no automatic create/cancel retries; duplicate cid
  refusals become UNKNOWN for reconciliation of the original order.
- A queued Manual job must carry an unrevoked permit, a future monotonic deadline, its current
  KILL generation, and a Manual cid. These are checked again after connection acquisition and
  signing. Cancel jobs have no entry-health dependency.
- KILL is observed atomically outside the cancel inbox and calls cancel-all without gateway
  cooperation. Only a confirmed cancel-all extends the venue lockout from its acknowledgement.
  An unconfirmed attempt leaves the sender unhealthy; another explicit KILL can try again.
- Result delivery failure latches the sender unhealthy and trips KILL. Future runtime admission
  must check BOTH sender health latches, publisher health and ledger health; re-arming KILL
  alone must not bypass any of them.
- Fake TLS tests prove one accepted order per lost/500/cut/timed-out create response, no hidden
  resend, the duplicate-cid rule, soft post-cross/rate refusals, and permit/expiry checks. KILL
  works with gw frozen and a full cancel queue, and completes while Manual awaits a stalled
  create response. All four sender/order-ledger/wire/net suites passed (15.4 seconds).

These workers are not yet installed in runtime; no demo or production write was made by them.

## Available cash projection — 2026-10-09

Kalshi's [balance endpoint](https://docs.kalshi.com/api-reference/portfolio/get-balance)
returns available balance. The prior header subtracted resting collateral from that number a
second time. The header now preserves it as free cash and reconstructs cash by adding modeled
resting collateral. Reducing orders share existing inventory in price priority; two orders
cannot both claim the same reducing contracts. Fractional collateral rounds upward, and
missing/conflicting shard metadata keeps the view read-only. A fake-exchange resting $4 order
now displays cash $500, free $496 on its shard, rather than cash $496, free $492.

## Authoritative live-series fees — 2026-10-09

- Fees come from [GET /series/{ticker}](https://docs.kalshi.com/api-reference/market/get-series),
  independently of model edges. Missing/non-numeric/negative multipliers are rejected. Unknown
  fee types stay explicit and fail `fee_ready`, rather than being treated as ordinary QUAD.
- Only live, pinned or held series are fetched, at most 16 per refresh, cached for five minutes.
  Stale metadata and backward clock steps fail readiness. Cached data enriches matching universe
  rows too. The legacy display fallback remains presentation-only for unvisited/unknown series.
- Metadata failure is visible separately in ops status and does not fabricate an account failure.
  No discovery request or fee fetch occurs on an order's click path.

Seven impacted integration suites passed (38.7 seconds). The actual demo probe with
`--require-private --require-fees` passed all five contract checks, with 8/8 live markets having
fresh supported fees, eight books, KX/KXP up without gaps, and zero exchange orders sent.
