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
