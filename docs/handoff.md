# Handoff

One section per session. Each records what changed, what was run, what is still open, and what
the next session is. Coding sessions and evidence collection are separate: nothing here claims a
trading edge, and prospective validation takes weeks of calendar time regardless of how fast the
code lands.

Working agreement for every session in this programme:

- Preserve existing working features. The ladder and dominance arbitrage finders are the only
  measured edge on this desk and no session may regress them.
- Nothing graduates from research-only on a prediction score alone (session 10 sets the bar).
- Live orders can only be sent from Sam's Mac. The cloud sandbox is a datacenter IP and Kalshi's
  geolocation check rejects it, so autonomous execution has to run on the Mac. This constrains
  session 8 and is not negotiable by design choices made earlier.

## Session 21 - "post only cross" on /margin/orders (2026-09-12)

Sam reported `POST /margin/orders -> 400 invalid_order / "post only cross"`.

**What it is.** A post-only order priced from a book snapshot the market has since moved past. The
exchange refusing it is the exchange doing its job: post-only means "never take", and it declined
to turn the order into a taker fill nobody asked for.

**It is not breaking anything.** The perps entry loop catches it, logs `entry not sent`, and moves
to the next market; nothing is recorded and nothing halts. On the event-contract side the same
refusal is already classified `SoftMarketState` in `include/desk/reject.h`, which documents the
2026-09-11 02:32 incident where it halted the algorithm and requested the kill switch. Four
occurrences in the whole log, all on event contracts before that reclassification.

**The defect it reveals, which is the part worth fixing.** The maker entry gated the INDEX at 120
seconds and never gated the BOOK:

    if (now * 1000 - m->reference_ts_ms > 120000) continue;   // a stale index is not a basis

`PerpMarket` stamped `mark_ts_ms` and `reference_ts_ms` - the venue nests those with their own
clocks - but carried no timestamp for the bid/ask at all, because the venue sends none. So an
arbitrarily old book could both price an order the exchange then refused AND feed `mid()`, and
`mid()` is what `basis_bps()` is computed from. The refusal is benign; what it says is that the
basis the entry was decided on may no longer have existed.

**The fix.** `PerpMarket` gains `quote_received_ts_ms`, stamped in `parse_perp_market` with
`now_ms()` and named for what it can honestly be - when the snapshot reached us, not when the book
was true - which is the same distinction Sam insisted on for the commodity feed in Session 18. The
maker entry now refuses to price from a book older than `book_max_age_seconds` (default 2.0,
settable 0.2-60), warns once per market per run like the unsized warning rather than every cycle,
and the perps view exposes `book_age_ms` beside `reference_age_s`.

**A trap in adding the setting.** `configure_algo` throws on a settings key the stored object does
not already contain, and the range check reads 0.0 for an absent key - so a `range("book_max_age_
seconds", 0.2, 60)` without a matching default in the published settings object would have made
EVERY perps settings save fail validation. The default is in the settings literal with a comment
saying why it has to be.

No strategy logic changed: the side and price selection is untouched, and it was already correct
(join the touch, never cross). This is a freshness gate in front of it.

### What was run

- `perps.cpp`, `engine.cpp`, `test_perps.cpp` clean under `-Wall -Wextra`.
- `test_perps.cpp` extended: the stamp exists, is within seconds of now, and survives a market
  payload with no venue timestamps at all - it must not be read from the body.
- `ui-routes` 63, `ui-desk-ux` 94, both 0 failures.

### Open

- A rebuild is needed for any of this: `cmake --build build`, then restart.
- Whether 2 seconds is the right limit is a question for the live log: if `book snapshot is Ns old`
  starts appearing for a market every run, the snapshot cadence is the problem, not the gate.

## Session 20 - the desk UI answers its own questions (2026-09-12)

`docs/ui-redesign.md` has the full account. Built straight into `ui/index.html` at Sam's
instruction rather than as a prototype, and verified in headless Chromium against a stub API at
1440x900 and 1920x1080.

The board used to say `nothing actionable right now`. That sentence is true of a working router
almost all the time and is also what a broken one says, which is precisely the confusion that
prompted this: a category chip clicked once had persisted in localStorage and was filtering the
board to an empty category. The screen held the information to say so and did not.

**Four lanes** (FOR ME default) with counts describing what is available rather than what the
current tab admits. **A decision funnel** - markets, priced, edge, actionable, auto, manual - every
step clickable. **An empty board that itemises itself** from the server's own decision counts and a
new reason histogram: how many rows were weak, research-only or blocked, and the top reasons none
cleared. **Stale is distinguished from empty.** `σ` and `raw edge` left the default columns, since
the decomposition belongs in the ticket where that question is actually asked.

**Halted could read as running, and that was real.** `screen["algorithm"]` published only the
operator's switch, so a halted engine reported `algorithm LIVE`. It now carries
`requested_running`, `halted`, `halt_reason`, `halt_class` and `actually_running`; the board header
tests halted first; three pills that said `RUNNING · HALTED` say `HALTED`; and the reason gets its
own line that also states what the halt does NOT stop.

**The ticket's side logic.** The decision's side is primary and labelled as the one the desk priced
and will re-check. The other side opens a fresh hand ticket through the existing trade-strip path
rather than flipping the current ticket, because a flip in place would keep the `decision_id` and
send a decision-backed order for a side the desk never priced.

Backend additions are read-only: `funnel` and `reasons` on `decisions_view`, computed from the
UNFILTERED screen because the point is to account for the rows the board does not show. No model,
threshold, route, order or graduation logic was touched.

### What was run

- `ui-routes` 63 checks, `ui-desk-ux` 43 checks (new, registered in CMakeLists), both 0 failures.
- `engine.cpp` clean under `-Wall -Wextra`.
- Three scenarios in a real browser at both resolutions: 0 console errors, 0 page errors.

### Two bugs the browser found that the static tests could not

`routeCounts` was returned by `tradesModel` but not destructured by `renderTrades`, which threw and
blanked the entire panel. And the lane counts were computed after the lane switches had already
excluded the rows, so AUTO read 0 while two auto rows existed. Both were invisible to a test that
reads the page as text, and both would have shipped.

### Second tranche: the selected-row panel

The right column now leads with SELECTED, above the ladder: the market, the book, the model, the
settlement state, the edge waterfall, the route and its reason stack, and the buttons. It follows
the selection from both the mouse and `j`/`k`, survives a repaint (the selection is re-applied
after `innerHTML` destroys the row), and defaults to the top row so it is useful without a click.

**The waterfall checks itself.** It rebuilds the row's net edge from the row's own terms and prints
an `unexplained` line if they do not reconcile. Verified in both directions: an inconsistent
fixture produced `unexplained +0.1¢`; making the fixture add up removed the line.

**Buttons.** `tradable` is the route, not a guess. Disabled buttons state their reason per route.
The other-side control opens a fresh hand ticket rather than flipping the ticket, which would keep
the `decision_id` and send a decision-backed order for a side the desk never priced.

**Ownership.** The always-visible positions table leads with a ME / AUTO / MIXED badge, derived
from the manager the server reports and not from the algo/manual size split - a position can carry
algo size and still be hand-managed, and the split would mislabel exactly those.

`ui-desk-ux` is now 71 checks. Three scenarios at both resolutions, 0 console and 0 page errors.

### Manual sends queue instead of being refused

A send while one is awaiting confirmation used to be dropped with a toast. It now queues, capped at
five, visible and cancellable per item, and drains when the one in front is answered.

What keeps it safe: each item leaves through the ordinary send path, so the `/api/size` cap
re-check and the desk's re-check of the decision on a fresh book both still run on it;
`reviewed_ts` is stamped when the operator pressed send, not at departure, so the desk's
90-second unsent-review expiry still means what it says; a second send on an already-queued event
is refused as a double-press; and the drain HALTS on the first failure rather than pushing the
remainder into whatever broke, with explicit resume and discard controls.

The dialog now closes when the request goes in flight - it used to sit open over the board, which
made queuing impossible - and the submit lock is per dialog, so building the next ticket does not
arrive with disabled controls. The send button relabels to `queue …` in that state.

Verified in a browser against a slow stub endpoint: pressing one row then the same row three times
sent one order and queued one, cancel emptied the queue, and only the in-flight order reached the
server. 0 page errors.

Two bugs found there that no static test could see: `addEventListener('click', sendKalshiModal)`
passed the MouseEvent into the new optional first parameter, so every send returned early at
`st.kind !== 'kalshi'`; and the ladder read `L.book.yes_bids` unguarded while every neighbouring
field had a guard, so a partial response threw and blanked the panel.

`ui-desk-ux` is now 94 checks.

### Open

- The bottom strip as POSITIONS / ORDERS / FILLS / ACTIVITY tabs: the tables and the ownership
  column are in, but they are stacked rather than tabbed.
- The bottom strip as POSITIONS / ORDERS / FILLS / ACTIVITY with an ownership column.
- The consolidated `/api/desk-view` snapshot and timer consolidation.
- The per-row "why not?" inspector. The reason histogram is its aggregate form, which is the half
  that answers the question that started this.
- A rebuild is needed for any of it to appear: the UI is embedded into the binary at build time.

## Session 19 - the board was reporting a loss on an account that was up (2026-09-12)

Sam asked whether the P&L was right. It was not, by $44, and the three reasons were independent.

**What the board said, and what Kalshi said.** Settled P&L read -$50.03 all time and -$7.09 since
Sep 5. The account went $100.2273 -> $114.8953 over the same stretch. A number that says you lost
money on an account whose balance rose is not a rounding problem, so the settlement records were
reconciled against the balance row by row.

**Defect 1 - `revenue` is the NET payout.** Kalshi nets offsetting contracts when the closing
trade happens, not at expiry. A market traded in both directions therefore reports a revenue far
below what its counts imply, and exactly 0 when it ended flat. The old code read `revenue`
directly and subtracted both legs' cost, so every round trip was booked as a total loss of both
sides. 31 of 77 settled markets carried this shape. The gross payout is the winning side's own
count, because a contract on that side is worth $1 whether it was closed early or held.

**Defect 2 - fees were charged twice.** `settlements.fee` and the summed `fills.fee` for the same
ticker are the same money. They are equal to the cent on all 77 settled markets on this account.
The old expression subtracted both.

**Defect 3 - the `realized` ledger was preferred, and it is not a total.** Kalshi's
`realized_pnl` is the P&L of the CLOSED portion only, on a weighted-average cost basis; it omits
whatever settled at expiry and is 0 for any market never closed early, which was 53 of its 57
rows. Preferring it dropped the settlement leg of every market it covered. It is still the whole
story for markets that closed flat and never settled, and that loop is unchanged.

`settled_pnl()` in `src/eval.cpp` is now the single expression, used by both call sites so they
cannot drift: `payout(winning side count) - yes_cost - no_cost - fee`. Derivation and the two
hand-worked markets are in the comment above it.

**Verified.** Worked out by hand against two real markets and reproduced exactly:
ATLPHI-PHI (12 NO at .17, then 9 YES at .57 closing 9 NO at .43; Kalshi realized +2.34 on the
close, the 3 remaining settled +2.49 over a .51 basis, fees .2731) = +4.5569. BFCSAS-SAS (5.73
YES at a .2594 average, 1 closed at .19, 4.73 expired, fees .0812) = -1.3772, against -1.3774 by
hand, the 2e-4 being the tenth-of-a-cent rounding in the stored costs. The function text was
extracted verbatim from eval.cpp and executed against all six cases: 6 checks, 0 failures.

After the fix: settled P&L -$5.56 all time, +$4.21 since Sep 5.

**The test that encoded the bug.** `tests/test_main.cpp` asserted that for a settled market with a
ledger row "Kalshi's realized number is the single source; the settlement arithmetic is not added
on top". That assertion was the bug, written down and green. It now asserts the opposite and says
why, and four fixture cases pin the netted, partial, fee-once and one-sided shapes.

**What was checked and found NOT to be broken.** The settlements fetch was suspected of leaving a
hole across the 15-hour outage, and it does not: `get_settlements` pages properly (limit 1000,
cursor loop) and the 1-hour lookback covered the gap. No change made. The claim was withdrawn
rather than "fixed" speculatively.

**Still unexplained, and it is not a bug.** Equity moved +$36.46 across the outage that trading
does not account for, and there is a clean -$25.00 one-minute step on Sep 6. Both look like cash
movements rather than P&L. Nothing in these tables records deposits or withdrawals, so the
reconciliation cannot close without Sam saying whether he moved money. Backing out both,
balance-implied trading P&L since Sep 5 is about +$3.21 against the computed +$4.21.

**All-time P&L is now measured top-down, and it is +$29.76.** Sam asked for one number: the
all-time P&L on the account. The per-market sum cannot give it, because it can only see markets
that produced a settlement record, and a manual weather trade worth about $36 never did. So
`build_pnl` gained an `account` block taken from the two balances and the cashflow ledger, which
are complete by construction: the money is either in an account or it came through a cashflow.

    equity $164.7602  (events $114.8953 + perps $49.8649)
    less deposits     $110.00   ($10 on Jul 7, $100 on Sep 5, from `cashflows`)
    less credits       $25.00
    = all time        $+29.7602

The two things that looked like unexplained cash were neither a deposit nor a loss, and Sam named
both before the data did. The -$25.00 step on Sep 6 is a `to_margined` transfer, present in
`cashflows` with exactly that kind: internal, so it cancels in a total that counts both accounts.
The other is a promo credit for perps volume, and it reconciles to $25.0000 as the residual
`perps_equity + transfers - perps_fills_pnl`. It arrived before `perp_account` recording began on
Sep 8, so there is no step to detect and the residual is the only route to it. It gets its own
line and is subtracted, because free money is not performance; if that residual ever stops being
a clean credit, the line is where it will show, rather than quietly inflating the total.

The events account by itself: $110 deposited less $25 moved to perps is $85 staked, against
$114.8953 now, so +$29.90 there and -$0.14 on perps. That the two routes agree is the check.

Pinned in `tests/test_main.cpp` with these exact figures, plus the absent-perps-tables case (the
perps tables are created by the perps module, not the base schema, so a store that never ran
perps must read them as zero rather than throw) and a withdrawal case, since a withdrawal must
reduce deposits while a transfer must not.

**The Trades board.** `tradesCat` persists in localStorage, so a category picked once (weather)
filtered out every manual row on every later visit and the board looked empty when it was not.
Until a tab is clicked in the session, a remembered tab that would hide all tradable rows now
gives way to `all`; clicking any chip pins the choice and stops it. The predicate is the same one
the row renderer uses for `tradable`, so the two cannot disagree. `ui-routes` still 63 checks, 0
failures.

**Not runnable from here.** The full suite needs a build, and the device shell is a Linux VM over
the mounted folder rather than a macOS shell. eval.cpp, test_main.cpp and the extracted function
were checked with g++ `-Wall -Wextra` (clean) and the dashboard JS with `node --check` (clean).
`cmake --build build` and a desk restart are needed on the Mac; the UI is embedded at build time,
so the board change needs the same rebuild.

## Session 18 - observability fix, and the impulse lane answered (2026-09-12)

Sam's correction, which was right and which mattered more than naming: a deduplicated stream means
silence does not prove the source stopped publishing. It can equally mean the value did not change
and the venue sent nothing. Session 17 had written `fresh` and `gap_ms` as though the first
reading were established, and the volatility estimator was already acting on it.

### Part A: the fields say only what is observed

`Value` now carries four facts instead of two, because they are four different facts:

```
message_gap_ms        since the previous MESSAGE for this symbol
unchanged_age_ms      since the value last actually CHANGED
value_changed         did this message carry a different number
observed_value_fresh  can we vouch for the recency of what we hold
```

The trade gate uses `message_gap_ms`, because the desk should not price from a value whose recency
it cannot verify, and the reason string is now **"reference observation too old"** rather than
anything about the venue. `commodity_reference` carries all four; the old `gap_ms` and `fresh`
columns are left in place and stop being filled, because a column that silently changes meaning is
worse than one that stops.

The measurement that would settle the question is now counted: `unchanged_value_messages`. A
channel that suppresses unchanged values can never send one, so a single occurrence proves it does
not suppress them, and the interpretation can be strengthened.

**The volatility estimator was acting on the unproven reading.** Session 16 had it discard any
return spanning more than 120 s of silence, with a comment asserting an outage. If a silence means
"the price did not change", that rule throws away precisely the calm stretches and biases sigma up.
It is now a named setting with both positions documented (`VolSettings::GapPolicy::DropUncertain`
or `IncludeAll`), the conservative one is still the default, the estimate records which produced
it, and the research ladder races them. A test constructs the determinate case - a silence with the
same price on both sides - and shows the discard makes the estimate too high.

The test Sam asked for is `test_message_gap_and_unchanged_age_are_different_facts`: two feeds whose
value is the same age in wall-clock terms, one because messages kept arriving unchanged and one
because they stopped. They agree on the thing a single field would have collapsed them into and
disagree on everything else. Collapsing the two ages back into one breaks three of its assertions.

### Parts B and C

`research/commodity_channel_behavior.py` writes `reports/commodity_channel_behavior.md` from what
was recorded: messages/s, value changes/s, gap percentiles, and the decisive unchanged-value count.
It refuses to conclude below 240 minutes and 50,000 messages, and a long recording with no repeats
is reported as consistent-with-suppression and **not proof**. No data yet: the commodity feed has
never run live.

The commodity quote feed now subscribes to `orderbook_delta` as well as `ticker`, records book
events with their sequence numbers into `commodity_book_events`, counts sequence gaps, and takes
the venue's own subscription ids from its acknowledgements rather than assuming them. Reconstruction
is deliberately left offline where it can be tested. The two streams are documented as answering
two different questions: ticker says the quoted price moved, the book says a quantity could have
been executed.

### Parts D-M: the impulse lane, and its answer

**The answer is no, and the mechanism is the opposite of the thesis.** Full write-up in
`reports/impulse_first_pass.md`; the short version:

On 14.6 hours of the recorder's output (526 ETH events, 164 BTC, 3 bps cross-venue moves), entering
as a taker 788 ms after the signal produces a markout of **-0.79 bps at 250 ms, -1.17 at 500 ms,
-0.98 at 1 s**, with the 90% interval excluding zero, and a hit rate of 38%. Not "no edge":
negative edge. The response curve says why. At the first moment Kalshi's 1 Hz ticker can be
measured, two seconds after the move, **Kalshi has priced 92% of a move the external composite has
only completed 41% of**. On larger moves it reaches 122% by ten seconds. Kalshi leads our external
composite; it does not lag it. B5, the lag thesis written as a rule, loses.

What was built to get there, in the order the brief set:

- `research/impulse_target_audit.py` (Part D). Provenance tracking through arithmetic: every number
  carries the earliest observation behind it, operators propagate it, and a realized value drawing
  on anything before the entry is refused with the path that produced it. The self-test builds the
  adversarial case Sam specified - pre-entry spread predicts the score, the future is noise - and
  it reports **+30.1 bps** and is refused on all 400 samples, while the honest taker target on the
  same data reports -60.1 bps and passes. A pre-entry value laundered through three operations is
  still caught.
- `research/impulse_prepare.py`. One pass over the recorder's gzipped JSONL into an aligned
  dataset. Handles the file the recorder is still writing (that is the normal state of the current
  hour, not a failure). The delta stream is folded into 100 ms activity buckets rather than stored
  row by row, because without a snapshot it cannot give a book.
- `research/impulse_study.py` (Parts E-M). Taker entry from the real quote series, markout and
  executable P&L in separate tables that are never added together, six baselines, chronological
  splits by event, event-level bootstrap, and the response curve on three clocks.

Two things the data settled that the brief could not have known:

- **Kalshi's `ticker` channel is 1 Hz.** Measured at 0.48 messages/s per contract, median gap
  1,000 ms. A 50 ms response study is not possible from it, and the tables print "not measured"
  rather than interpolating.
- **No orderbook snapshot was ever captured**, so the dense delta stream cannot be anchored and no
  price is taken from it. The recorder was *not* restarted to fix this, because it has run
  continuously since 13:00 and continuity is worth more; the fix is one flag on its next restart.

And one place the method caught me. I first read the book-activity spike (2.0x within 50 ms) as
Kalshi reacting fast. The pre-event control says the book was already at 1.36x half a second
*before* the detector fired, which is exactly what an event stamped at the end of a one-second
window would do. The report now prints that reading itself rather than leaving it to whoever reads
the table. A number I nearly published as a finding is published as a refusal instead.

### What was run

- ctest 18 of 18 green: desk 896, systematic 350, pipeline 244, commodity 464, routing 40, perps
  144, poly 142, account-summary 36, manual-orders, pair-models, ui-routes 63, recorder,
  alpha-decay, commodity-replay, commodity-ladder, commodity-channel, impulse-audit,
  impulse-prepare.
- clang `-Wall -Wextra` clean apart from the pre-existing `poly.cpp` warning; ASan/UBSan clean.
- Every guard mutation-tested.

### Open

- **The composite may itself be lagging.** binance.com answers HTTP 451 from a US address, so the
  external set is Coinbase, Binance.us, Kraken and Hyperliquid. If the true price leader is outside
  it, what was measured is "Kalshi leads these four", which is weaker than the conclusion drawn.
  This is the one caveat that could overturn the result, and it needs a feed rather than a model.
- One quiet session with a seven-hour gap. Nothing here speaks to a volatile session or a release.
- **The commodity lane is blocked, not merely early.** Every commodity gate is at zero because
  nothing has ever recorded a commodity tick, and it cannot until two things happen on the Mac.
  The evidence, checked 2026-09-12T04:45Z: no `commodity_*` table exists in any database
  (`desk.sqlite`, `prod.sqlite`, `perps_research.sqlite`, `history.sqlite`); `build/kalshi-desk`
  is dated Sep 11 13:04 and contains none of the markers `pyth_value`, `commodity_reference_raw`,
  `commodity_calibration`, `commodity_quotes`, `KXCOPPER15M` - it predates the entire feed; and
  `data/prod.log` stops mid-cycle at 2026-09-11T13:46:24Z with no shutdown line, so the desk has
  been down about 15 hours. The only subscriber to `pyth_value` anywhere in the tree is the C++
  desk; there is no Python commodity recorder, and one must not be written, because a second
  implementation of the settlement feed would make replication ambiguous by construction.
  `config/desk.json` is already correct and complete: all five 15M series watched, five
  `commodity_terminal_*` at `manual`, `params.commodity.reference.enabled` true with
  `subscribe: ["all"]`, `replication_windows_required: 50`. Nothing in config needs changing.
  What is needed is `cmake --build build` and then starting the desk, both on the Mac - neither is
  doable from here, since the device shell is a Linux VM over the mounted folder rather than a
  macOS shell, and building there would overwrite the Mac's objects with Linux ones.
- Part N was already complete before this session: `reports/spread_matched_control.md` (Sep 12
  00:19) and the lane is archived in `research/conditional_maker_v1_rejected.md`, which cites it
  as the fifth of five independent routes to the same rejection. Nothing further is owed on it.
- The live Python perps recorder is healthy and untouched: heartbeat ok, 293 minutes, all seven
  crypto sources flowing. It records no commodities and is not supposed to.

## Session 17 - the commodity research ladder, and the capture it needs (2026-09-12)

Sam's brief: the four terminal models are infrastructure, not assumed alpha. Do not spend days
collecting 400 settlements to answer whether a zero-drift EWMA lognormal works; build the ladder
that can tell mispricing from a model arguing with a better-informed market, and gate every rung
on the one below it. Ten priorities, P0 to P10.

Three of the brief's premises turned out to be wrong, and the corrections improved the plan.

**There are seven 15-minute commodity series, not five.** Copper is real and trading
(`KXCOPPER15M`, `Commodities.Index.CU/USD`), and platinum and palladium exist as series with no
open windows - and settle on `Metal.XPT/USD` and `Metal.XPD/USD`, the plain metal feeds rather
than `.Index.` ones, which a pattern written for the other five would have missed. Copper is built
as `commodity_terminal_copper_v1`; the other two are recorded in the doc with their symbols and
are one line each to switch on.

**Kalshi's settlement channel does not carry confidence, exponent, publisher count,
feedUpdateTimestamp or marketSession.** Its `pyth_value` payload is exactly four fields:
`underlying_ticker`, `value_usd`, `source_ts_ms`, `received_at`. Those other fields are Pyth's own,
on Hermes, which is a different stream from the one these contracts settle against and now wants a
key. So V5 of the volatility race is not raced - explicitly, in the output, with the reason -
rather than quietly approximated from a feed the desk does not settle on.

**There is no session field to key regimes off.** All five feeds are scheduled 24/7 at fixed rates
(gold and silver 200 ms, WTI, natural gas and copper 50 ms). Sessions are real in the underlying
but have to be measured, so P8 reports Brier, volatility and stale-reference rate by hour rather
than reading a label.

Three things the checking turned up that the brief did not ask for:

- **The channel will name its own underlyings.** `update_subscription` has an `underlying_list`
  action returning everything streamed in the last two hours. The desk asks on every connect and
  records the answer, so the series-to-symbol mapping is the venue's statement rather than a
  pattern match. The patterns remain as fallback, exact published symbol first.
- **The contract states the candle convention verbatim**, and it is the half-open window already
  implemented: "the close price for the 1-minute candlestick at a given time is the price at the
  end of the immediately preceding one-minute interval."
- **Rounding is stated per commodity** - 2 dp gold and WTI, 3 dp silver, 5 dp natural gas and
  copper. The desk now reads the tolerance out of `rules_secondary` instead of keeping four
  guessed constants, which is the same discipline as everything else here: the rule is published,
  so do not retype it.

### What was built

**P0, the schema nobody has seen.** The first hundred frames of every run are kept byte for byte
in `commodity_reference_raw`, along with every frame that surprised the parser. The observed
schema - every field under `msg`, with its JSON type and a count - is reported in the desk's
status. A new field is logged once, kept, and priced through: an exchange adding an informational
field must not take the settlement feed down. A **known** field changing type is different in kind
(`value_usd` going from string to number silently changes what every price means), so it is
counted and kept. A frame with no usable price is rejected outright and kept for inspection.

**Carried-forward values are marked, not rejected.** The contract settles on the last published
value when the source publishes nothing, so carrying forward is the rule. But the stream is
deduplicated, which means a carried-forward price arrives as silence rather than as a repeated
message - so staleness here is a gap, judged against each symbol's own measured cadence (median
inter-arrival, eight times it, floor two seconds). `commodity_reference` now carries `gap_ms` and
`fresh` on every row, and the volatility estimator already refuses to difference across a gap.

**P1, the fifty-window gate.** `settlement_verified` now requires fifty *consecutive* windows whose
published strike this feed reproduced within the contract's stated tick. One mismatch resets the
run to zero, because the claim is "this feed reproduces the strike every time" and one failure
refutes it. That is about twelve hours of recording per underlying before any probability from it
is trusted, which is the price of the claim.

**P3, the calibration checkpoints - the irreversible part.** `commodity_calibration` gets one row
per window per checkpoint at T-14m, 10m, 5m, 2m, 1m, 30s and 10s: the probability, the
distribution behind it, the volatility and its inputs, the reference value with its age, gap and
freshness, the replication count, and the book at that instant. None of this can be reconstructed
afterwards - the probability at ten seconds depends on a reference value and a volatility estimate
as they stood at that instant, and nothing else on this desk keeps either. The ten-second row
exists even though the model refuses to *offer* a trade inside twenty seconds: the checkpoint is a
research record, not an offer, and that gate is about trading.

**P7's prerequisite, which the brief could not have known.** A 50 ms to 5 s response study cannot
be run on five-second REST polling; it would produce a table with the right column headings and no
information in it. The desk now subscribes to the exchange's own `ticker` channel for the windows
in play - the same socket machinery as the reference feed - and records every update to
`commodity_quotes` with the venue's millisecond clock and ours. The difference between those two
is the only honest way to say whether a lag we measure is theirs or our network's. The market set
rolls every fifteen minutes and is sent as a difference rather than by reconnecting.

**P10, the economic ladder.** `commodity_model_status` reports SETTLEMENT_UNVERIFIED,
DATA_ACCUMULATING, CALIBRATED_NO_ALPHA, MISPRICING_DETECTED or FAILED_COST from what is recorded,
with the interim milestones at 50 / 100 / 200 / 400. The two states that mean risking money are
not reachable by computation at all: PAPER_CANDIDATE and LIVE_CANDIDATE are read from a key a
person sets. A verdict written by the research script cannot promote a model, and the desk
refuses to report any verdict at all below the registered sample.

**P4, P5, P6, P8, P9: `research/commodity_ladder.py`.** Runs on the recorded checkpoints.
Calibration with slope and intercept (the brief's "70% settles 82%" is a slope under one), split
by commodity, time-to-close and measured volatility regime; a volatility horse race scored out of
sample on windows split chronologically **by window**, so no checkpoint from a window used to
choose is also used to score; Gaussian against Student-t and a two-state mixture, scaled to the
same variance so the tail test is a tail test; executable edge in buckets against the **takeable**
price net of the desk's own fee, with the monotonicity requirement that decides the whole thing;
regimes measured by hour; and drift last, fitted on where the reference goes rather than on the
settlement, and deleted unless it improves the out-of-sample Brier.

### What was run

- ctest 15 of 15 green: desk 896, systematic 350, pipeline 244, commodity 436, routing 40, perps
  144, poly 142, account-summary 36, manual-orders, pair-models, ui-routes 63, recorder,
  alpha-decay, commodity-replay, commodity-ladder.
- Browser: test_dashboard, test_ui_render, test_ui_book, test_ui_keys all pass.
- clang `-Wall -Wextra` clean apart from the pre-existing `poly.cpp` warning; ASan/UBSan clean on
  commodity and desk.
- The ladder was also run end to end on a synthetic database in the desk's own shape (120 settled
  windows, 840 checkpoints, an hour of ticks): every stage produced a table, and P9 correctly
  reported no drift improvement on a walk generated without drift.
- Each guard was mutation-tested: reverting it, confirming the test fails, restoring it.

### Open

- **Still nothing measured.** No window has settled against this model. The gate now needs fifty
  consecutive replicated windows per underlying before the models leave RESEARCH at all, and COM-1
  wants 400 settlements after that.
- **The live `pyth_value` shape has still never been seen**, because the API key is outside the
  folder this sandbox can reach. That is now much better handled than guessed: the first hundred
  frames are kept verbatim, the schema is observed and reported, unknown fields are tolerated and
  logged, and the channel is asked to name its own underlyings.
- P9 runs but is not yet meaningful: it needs P4 to have chosen a volatility model on real data.
- The desk shows `commodity_models` in its status; the Trades board does not yet display the
  ladder state per model. One small UI addition when there is something to show.

## Session 16 - the 15-minute commodity terminal-price model (2026-09-12)

Sam's brief: a probability engine for Kalshi's 15-minute gold, silver, WTI and natural-gas
contracts, and its one governing idea - **do not try to predict whether gold goes up. Estimate the
distribution of where gold can be when the contract settles, then price every strike from that
distribution.** No sports, no weather, no machine learning, no directional model, nothing
graduated. What this session had to produce is a clean baseline that can say whether Kalshi
misprices the terminal distribution at all. Built and tested in the cloud sandbox, written back to
the Mac, nothing committed.

**Settlement first, code second** (`docs/commodity-settlement.md`, new). Before any model: what do
these contracts actually settle on? Verified against the live API - one market per 15-minute event,
`strike_type` `greater_or_equal`, the strike is the settlement feed's own 1-minute candle close at
the window open, a tie pays YES, the fee is the ordinary quadratic taker fee, and `close_time` is
the settlement clock. That last one matters: `expected_expiration_time` on these series is about six
hours after the window and `expiration_time` is a week out. Neither is the settlement time. The doc
also states what is NOT verified - the exact `underlying_ticker` strings on the feed - and how the
desk resolves that at runtime instead of assuming it.

**The reference feed** (`include/desk/ws_client.h`, `src/ws_client.cpp`, `include/desk/reference_feed.h`,
`src/reference_feed.cpp`, both new). Kalshi publishes the values it settles on over its own
authenticated socket (channel `pyth_value`), so the desk now speaks WebSocket: RFC 6455 framing,
masked client frames, continuations, ping/pong, TLS with hostname verification, per-attempt
RSA-PSS auth headers, re-subscribe on every connect, stale-socket detection and capped backoff with
jitter. `ReferenceFeed` owns that socket, keeps 90 minutes of observations per symbol in memory for
the model, and appends them to `commodity_reference` - append-only, one row per symbol per second
plus, always, the last observation of each minute, because that one is the candle the contract
settles on. The socket runs on its own thread and never touches the store; the engine's cycle calls
`flush()`, which is the only writer, so nothing in the cycle ever waits on the network.

**The model** (`include/desk/models/terminal_price.h`, `src/models/terminal_price.cpp`,
`src/strategies/commodity_terminal.cpp`, all new). One distribution per underlying:
`log(S_T/S_t) ~ N(0, sigma^2 * tau)`, sigma per sqrt(second), tau in seconds, and `mu = 0` by
construction - v1 claims no drift and no direction. Every contract on that underlying is then
priced from it: `TerminalPayoff` parses the exchange's strike types into Above / Below / Range with
the tie rule explicit, and `TerminalDistribution::probability()` answers it. Volatility comes from
rv_1m/5m/15m/60m and an EWMA over sampled returns, combined conservatively (the slower estimate,
floored by the recent one, clamped to the hourly), with every input in the estimate's diagnostics.
Four models: `commodity_terminal_{gold,silver,wti,natgas}_v1`.

**It checks its own settlement.** The model does not assume it is reading the feed Kalshi settles
on: at each window open it recomputes the exchange's published strike from its own recording of the
feed, and until that matches within a tick, `settlement_verified` is false and the router routes it
RESEARCH. The check runs against the window being priced, newest first, skipping windows whose
close has passed - the store lists a just-closed window as open and hands markets over in ticker
order, so a check that took the first row it found would confirm a dead window and leave the live
one unchecked while the dashboard said "settlement confirmed".

**Routing.** All four models are MANUAL and nothing is graduated: they are offered to Sam and never
sent unattended, in the same cycle as an AUTO row from a graduated model. The route is the model's
own, not the category's - `commodities: auto` with these at `manual` is still manual. A stale or
missing reference blocks the row outright (the estimate fails its own input check); rules that name
some other settlement source, or a strike this feed cannot reproduce, make it research.

**One general change.** The desk's flat "too near close" floor is ten minutes, which on a
fifteen-minute contract is not a floor but an off switch. The floor a row is judged against is now
`min(min_minutes_to_close, min_life_fraction_to_close x the contract's own trading life)`, with the
new fraction at 0.2 - three minutes of a fifteen-minute window - and never below one minute
whatever the arithmetic says, because an order placed in the last seconds cannot be repriced or
pulled. Long-dated markets are untouched: a fifth of 48 hours is far above ten minutes, so the flat
floor is still the binding one. `min_minutes_to_close_for()` in `decision_router.h` is the single
place both the router and the resting-entry check read it from.

**The dashboard.** A commodity row on the Trades board carries a second line with what its
probability was actually computed from - the reference value and its age, the strike, the distance
in dollars and in sigma, and the time left - and the order dialog gets a block with the same, plus
the volatility used per minute, the model's p(YES) with its band, the settlement-feed verdict and
the futures cross-check basis. Both clocks are recomputed against the desk's own clock rather than
printed as the model stamped them: a row forty seconds old would otherwise say "4m21s left" when
there are three and a half minutes, and call a dead feed two seconds old.

**The research.** `research/commodity_terminal.py` (new) replays the model against what the desk
recorded, with no look-ahead: every probability uses only observations stamped at or before its own
quote, decisions start at the window open (the strike does not exist before it) and stay inside the
window the live model would price. It reports paired Brier model-vs-market with a bootstrap CI on
the difference **resampled by contract, not by decision** - one 15-minute window contributes dozens
of snapshots sharing one outcome, and treating them as independent makes the interval about five
times too narrow - plus log loss, calibration by decile, the distance-in-sigma buckets COM-2 is
about, and net return at the touch after the desk's own fee. `--self-test` builds its data in
memory and proves the look-ahead guard by moving the reference 5% from the middle of a window on:
no earlier probability may change, and the later ones must. It runs in ctest as `commodity-replay`.

**Hypotheses COM-1..COM-4** are registered in `src/hypotheses.cpp` and `docs/hypotheses.md`, before
the model has scored a single settled window, each with the sample it needs first (COM-1: 400
settled contracts across two underlyings). Nothing in them reports a result.

### What was run

- C++: desk 896, systematic 350, pipeline 244, routing 40, commodity 275, perps 144, poly 142,
  account-summary 36, manual-orders and pair-models PASS.
- Browser: `test_dashboard.cjs`, `test_ui_render`, `test_ui_book`, `test_ui_keys`,
  `test_dashboard_scroll`, `test_ui_routes` (63) all pass.
- `research/commodity_terminal.py --self-test` passes, on the Mac's copy as well as in the sandbox.
- `ctest`: 14 of 14 green, including `recorder`, `alpha-decay` and the new `commodity-replay`.
- clang `-Wall -Wextra`: clean apart from the pre-existing `poly.cpp` unused-variable warning.
- g++ ASan/UBSan on commodity, routing and pipeline: zero findings.
- Every fix in this session was mutation-tested: the guard was reverted, the test was confirmed to
  fail, and the fix was restored. The WebSocket backoff fix is the clearest case - without it, a
  server that accepts the handshake and hangs up drew 16,416 connection attempts in 2.5 seconds.

### An independent review found ten real defects, all fixed

Worth recording, because most of them were silent rather than loud:

1. The settlement self-check compared a wall clock against a window's open time, so once any
   window had been checked every *open* window was skipped - and the desk reported "settlement
   confirmed" on a strike it had never reproduced. Now keyed on the window itself.
2. `p_between` with a lower bound of zero (the bottom bracket of a ladder) returned half the true
   probability; `p_above(0)` returned 0 instead of 1. Non-positive bounds are now handled, and a
   threshold contract whose strike arrives as zero is not priced at all.
3. The volatility warm-up counted grid points rather than observations, so two ticks an hour apart
   passed as "warm" with 720 samples and the hour's move read as if it took five seconds. Returns
   are now taken between observations that really happened, over the time that really elapsed, and
   a gap longer than two minutes is excluded rather than attributed to the seconds it appears to
   have taken.
4. Tau was measured from the clock while the spot was an observation up to 20 s old, understating
   the standard deviation by up to 41% at the end of a window - always in the direction that makes
   the model look more certain. It now runs from the observation.
5. The replay priced quotes recorded before the window opened, against a strike that did not exist
   yet: look-ahead, in the file whose whole claim is that there is none.
6. The replay's bootstrap resampled decisions rather than contracts (interval ~5x too narrow).
7. `market_weight` defaults to 0.5, so these models' published probability would have been half
   model and half market mid - answering "does the model disagree with the market" partly with the
   market. A strategy now declares its own default; these declare 0.
8. A strategy with no entry in `strategy_modes` is "off", and nothing added these - the socket
   would have run, recorded the feed, and priced nothing. Backfilled in `config.cpp`.
9. The WebSocket reset its backoff on a completed handshake and reapplied it only on a failed
   connect, so a server that accepted and hung up was reconnected to as fast as the machine could
   open sockets. Pings and pongs also did not count as liveness, so a quiet feed was torn down
   every 45 seconds - and each teardown left the gap that (3) turned into an inflated sigma.
10. `decode_frame` trusted a 64-bit length (RFC 6455 requires the top bit be zero), which could
    wrap and desynchronise the stream; and a failed `flush()` dropped its rows silently, in the one
    table that is the entire replay sample.

### Open, and for the next session

- **Nothing has been measured yet.** No window has settled against this model. COM-1 needs 400
  settled contracts; at four underlyings and 96 windows a day each that is days of recording, not
  minutes. The feed has to run before any of this means anything.
- **The live `pyth_value` message shape is still unconfirmed.** The API key lives outside the
  folder this sandbox can reach, so the socket has never been opened against Kalshi. The parser is
  tolerant, the subscription is `["all"]`, symbols are matched by pattern, and the first message per
  underlying is logged - but the first real run is where the strings get confirmed, and the strike
  replication check is what says whether the whole thing is pointed at the right number.
- **`config/desk.json` now carries `params.commodity` and the four 15M series** in `watch_series`
  (added this session, secrets untouched). Without those the feature does not run at all.
- `expected_settlement_ts()` still returns the exchange's six-hour `expected_expiration_time` for
  these windows. It changes no decision (every horizon gate is 48 h) and the board no longer shows
  it for these rows, but it is wrong; parsing `settlement_timer_seconds` is the fix.
- The model's `sigma` prices volatility uncertainty only. It carries no allowance for the zero-drift
  assumption or for non-normality at a fifteen-minute horizon; COM-1's calibration curve is what
  will say whether those hold.
- `desk-tests` reports 896 or 897 checks depending on the run. It is `test_brackets`, whose
  `for (...) if (p.strategy=="target") CHECK(...)` runs zero or one time depending on proposal
  timing. Pre-existing, not a failure, and worth tidying when someone is next in that file.

## Session 15 - the canonical probability pipeline (2026-09-11)

Sam's brief: one path from data to order. `DATA -> PROBABILITY MODEL -> EDGE ENGINE -> DECISION
ROUTER -> MANUAL board | AUTO lane | RESEARCH record -> shared risk -> the shared executor`, with
the manual and automatic lanes running in the same cycle, and the rule behind all of it: **a model
never decides whether it trades automatically**. Phase 0 (session 14) was already in. Built and
tested in the cloud sandbox (the test-only curl shim; nothing of it ships), written back to the Mac,
nothing committed.

**The quote** (`include/desk/probability.h`, `src/probability.cpp`). `ProbabilityQuote`: ticker,
event, category, model name and version, `p_yes`, `sigma`, the model's own timestamp, its underlying
data's timestamp, when the desk received it, `inputs_valid`, `settlement_verified`, `auto_eligible`,
the model's research flag and reason, and diagnostics (the raw model number before the market blend,
the blend weight, the model's input report, source timestamps). It is a model output and nothing
else: no size, no style, no route, no price. Every model is adapted into one without touching its
math (`quote_from_fair`), and a strategy may override its version and its settlement verification
(`Strategy::model_version`, `Strategy::settlement_verified`). `settlement_verified_for` is the
mapping the desk actually has: `weather_nws` when the contract names the NWS and its series has a
read, mapped station product; `sports_inplay` for 90-minute soccer result markets; `sports_totals`,
`sports_live` and `manual_fair` yes; `crypto_lognormal` when the rules name the CF Benchmarks
real-time index; `xvenue` and anything else no.

**The edge engine** (`include/desk/edge.h`, `src/edge.cpp`). One `EdgeCandidate` for a quote against
a book, for YES and for NO: `raw_edge = fair - price`, `net_edge = raw - fee`, `executable_edge =
net - slippage - buffer x sigma`, EV per contract, Kelly, the touch and its displayed size. A taker
is priced at the side's actual ask, never the mid; a maker at the desk's maker price (a tick inside
a spread of two ticks or more, else joining the bid, never at the offer). `normalize_prices()` is
the one place the YES/NO identity lives (NO ask = 1 - YES bid, NO bid = 1 - YES ask, sizes
mirrored), with the entry paths that used to derive it themselves - the executor's post-only cross
check, the resting-entry re-validation, the capture plan's ranking, the hedge planner - now reading
it. The exits and the arbitrage legs keep their own reads, untouched this session.

**The router** (`include/desk/decision_router.h`, `src/decision_router.cpp`) owns eligibility.
`decide()` gives every priced side exactly one route - blocked, research, manual, auto - and the
reasons in words. Precedence: a data or risk gate, then research, then the thresholds ("weak"), then
the policies that hand a row to you, then AUTO. `docs/routing.md` is the rule in full. AUTO needs
all of: the effective route auto (the lowest of the category's route, the strategy's mode in
`config/desk.json` and the model's own route), the model graduated, the source auto-eligible,
settlement verified, sigma within `auto_max_sigma` (0.10), the edge over `min_edge` after the fee,
the slippage estimate and the buffer, a fresh two-sided book with displayed size, ownership clear,
and the master switch armed. Routes and graduation are per model **and version**
(`model_routes["weather_nws_v1"]`); a bare-name entry can only restrict every version of a model,
never grant.

**Defaults changed (this is the safety change).** Every category is `manual` and every model route
is `manual` or `research`; `graduated_models` is empty, so nothing trades unattended until it is
routed auto and graduated, which the Systematic tab now edits (Model routes) and asks about before
it applies. Locked baskets stay `auto`: they are locked, not forecasts. A one-time migration
(`kv systematic:route_migration = s15`) moved saved weather, sports and crypto routes from auto to
manual. Probability generation and book capture are unchanged on every route but `off`.

**One cycle, both lanes.** The cycle builds the board, runs the router once, publishes the decisions
and then lets the algorithm act on the AUTO ones, servicing the manual queue between every stage. No
threads. `SystematicTrader` no longer screens: it takes AUTO rows from that same screen and keeps
what is its own - entry style, the maker ladder (each step priced by the edge engine now), the TTL,
sizing and exits. A resting entry is pulled when the decision that sent it would no longer be AUTO,
including the model's route, its graduation, settlement, sigma and the strategy's mode.

**The manual lane.** `GET /api/decisions` is the Trades board's only source for Kalshi rows: this
cycle's MANUAL and AUTO decisions, ranked by Kelly, one row per event and route, RESEARCH behind
`?research=1`. The page recomputes nothing - not the route, not the edge, not ownership. AUTO rows
are shown and marked and cannot be clicked. The order dialog shows the desk's own breakdown for the
price and style in the box (`GET /api/size?decision_id=...`: p(win), sigma, price, raw edge, fee,
slippage, the uncertainty deduction, the executable edge, cost, max payout, max loss, the suggested
size and the route's reason), the request names its `decision_id`, and the desk refuses at the click
if the decision has gone, is not MANUAL, or is under the manual floor at your price. Just before the
order goes it re-reads the book from the exchange, re-runs the router and the edge engine and
refuses with what changed; a model estimate that is merely a few seconds past the entry limit makes
it wait for the next one instead of failing. The Systematic tab's Review opens the same dialog, and
only on a MANUAL row.

**The record.** `trade_decisions`: `offered` (sampled: first appearance, a route change, a 2c edge
move, or every 5 min), `research` (the candidates a research model would have taken, every 15 min),
`approved`, `revalidated` (naming the decision it was approved from), `auto` (the algorithm took it,
live or paper), `rejected`, `unknown` (sent, delivery not known) and `submitted`. Proposals carry
`decision_id` and orders inherit it, so `GET /api/decisions/trace/<order_id>` walks an order back to
the probability and the book that caused it. The sampled rows age out (offered 30 days, research
90); what happened to a decision is kept.

```
desk 897 (was 827), systematic 350 (was 324), pipeline 244 (new), routing 24, perps 144, poly 142,
account-summary 36: 0 failures; manual-order, pair-model PASS
node tests/test_ui_routes.cjs 63 - test_ui_keys 11 - test_ui_render 11 - test_ui_book 14 - scroll OK
node tests/test_dashboard.cjs PASS (it failed at line 169 before this session; that was the test
reading an upper-cased label case-sensitively)
clang++-18 -Wall -Wextra: one pre-existing warning (poly.cpp:1684)
g++ -fsanitize=address,undefined: pipeline, routing, systematic, desk and manual-order suites clean
```

New tests: `tests/test_pipeline.cpp` (quotes, normalization, the edge engine, the router, the
parallel lanes of one synthetic cycle, ownership, re-pricing a displayed decision, versioned
routes); in `tests/test_main.cpp` `test_one_cycle_both_lanes`, `test_manual_revalidation_and_trace`
(a moved market, a vanished edge, ownership that appears after the click - your own and the
algorithm's - the order's trail, and the kill switch still cancelling),
`test_manual_request_waits_for_fresh_estimate`, and the queue refusing a stray model entry; in
`tests/test_systematic.cpp` the AUTO lane's decision trail and `test_mode_cap_pulls_resting_entry`;
`tests/test_dashboard.cjs` rewritten around the decisions board.

An independent reviewer went through the diff and the spec. What it found and what was done:

- two data races it confirmed with ThreadSanitizer - the calibration cache and `strategy_modes`,
  both now read by HTTP threads through the order dialog and the Systematic tab. Both fixed (a
  mutex and a copy the writer updates under the engine lock);
- the uncalibrated-model cap had started binding hand tickets as well. It now applies only to an
  order placed on a model's decision - the case the old proposal lane capped - and the desk
  enforces it at the send rather than only suggesting a size (`risk.uncalibrated_max_order_frac`,
  0.05 in this desk.json, so $5 of a $100 bankroll);
- the Systematic tab's Review still opened an unrevalidated hand ticket on any row. It now opens
  the decision dialog, and only on a MANUAL row;
- two new checks were not actually covered by a test (the revalidation's ownership context, and the
  queue's refusal to send a model entry). Both have one now, each verified to fail with its fix
  reverted;
- a bare-name model route silently granted a route and a graduation to every version. Fixed;
- the research record was written on every price tick. Sampled now, and pruned;
- `rejected` was recorded for an order whose delivery was unknown (a 5xx, a timeout). That is
  `unknown` now, which is what the queue and the uncertain-order block already called it.

### Open

- **Nothing is graduated, so nothing trades unattended.** Weather is the only model the desk.json
  modes leave on the manual board (crypto, sports and in-play are in `research` mode there);
  `xvenue` (the Polymarket-priced rows) is RESEARCH everywhere because settlement parity with
  Polymarket is not verified in code. If those rows should be back on the manual board, the
  verification is the piece to write, not the route.
- The dashboard's model-routes table edits routes and graduation; the desk.json `strategies` modes
  still cap them and are edited on the Strategies tab (they are not persisted to desk.json).
- `allow_research_estimates` no longer does anything (a research-only estimate is RESEARCH) and is
  off the page; the stored value is ignored.
- The Trades board's pairs and perps rows are not router decisions and keep their own rules, including
  the page-side ownership filter they have always had.
- While the algorithm runs in paper, its AUTO rows are the paper lane's and are not offered to you.
  Stopping it hands them back as MANUAL, with that reason on the row.
- `record_books` still ranks captures with its own small edge formula (fee only, no buffer).
- The perps and Polymarket lanes are outside the pipeline: their orders have no decision snapshot.
- Exit logic was not touched, as the brief said. The exits still read their own side prices.
- A resting entry placed before this session by the old proposal lane (an approved model proposal)
  is not pulled just because model entries now come from the router; it is an ordinary manual order.
- Session 14's open items still stand, including `run_exits` checking the model fair at its own age
  and the orphaned `tests/test_dashboard_refresh.js`.

---

## Session 14 - Phase 0: routes, refusals, cancel state, one freshness limit (2026-09-11)

Sam's ask: fix the live bugs before the architecture brief starts (GPT's Phases 1-13 for Codex:
probability quotes, one edge calculator, a decision router, per-model routes). Four bugs from
the log: the book capture failing every cycle, a post-only cross halting the algorithm and
tripping the kill switch, the store refusing `submitted -> cancel_pending`, and the Book panel's
lane-2 counters stuck at zero. Then the brief's Phase 0: a route type, refusal classes, the
cancel transition and one market-data age. Built and tested in the cloud sandbox (a test-only
curl shim; nothing of it ships) and written back to the Mac. Nothing was committed.

**Routes are a type** (`include/desk/route.h`). `RouteMode {Off, Research, Manual, Auto}`,
`parse_route` (strict, for validation) and `route_setting` (the old bool shape still means what
it did: true auto, false manual; an unknown value is off). Capture follows `route != Off`,
unattended sending follows `route == Auto`, and nothing reads a route as a truthy value any more.
The capture bug: `record_books` read each category with `settings.value(cat, false)`, and since
routes became strings that threw `type must be boolean` on every cycle with a candidate over
`min_edge`, so the books step captured nothing. A second, older throw in the same step
(`performance()` reading NULL columns, `type must be number, but is null`) is fixed as well. New
route `research`: books and estimates are recorded, the screen shows the row as `research`, and
nobody is offered it. The route select reads auto / to me / research / off; the explainer and
the START LIVE confirmation list only the categories that are actually `auto` (`routeOf` in
`ui/index.html`; before, any non-empty string counted as on).

**Refusal classes** (`include/desk/reject.h`). `classify_rejection(status, body)` gives
`soft_market_state` (post-only cross, market closed or paused), `soft_collateral` (insufficient
balance), `retryable` (429, 5xx), `auth` (401/403), `hard_invalid_order` (other 400/422 and
"invalid") or `unknown`. Only the last three trip the kill switch or halt the algorithm. Phrases
are matched exactly, so an unrecognised body stays hard. On a post-only cross the systematic
trader marks the order not sent, reads that one book again (`Hooks::fresh_quote`), re-screens the
market at the new time and sends once more only if it is still `algo`; otherwise it logs "the book
moved; after a fresh read the row is weak" and gives the ticker a 30 s breather. Two soft refusals
end the step. `Executor::submit` now refuses a maker price at or through the offer it has just
read, before sending (`SoftRejection`). A refusal Kalshi itself answered (a 4xx with its error
body, not a 408) puts the previous write watermark back. `auto_submit` (the strategy lane) uses
the same classes: a soft refusal gives entries on that ticker a 30 s breather and never holds back
an exit. Every new halt stores `halt_class`. At startup, a halt recorded as "order rejected by the
exchange" whose class would not trip the kill today is cleared. The algorithm is then left
**stopped** until Start is pressed ("halt cleared at startup" in the log). The desk kill switch
is not touched: a kill tripped by the old rule still waits for a person, as every kill reason
except the disabled daily stop does.

**Cancel state** (`src/store.cpp`). `submitted -> cancel_pending` is allowed. It used to be
refused, so a failed cancel left the proposal `submitted` and nothing retried it. `pending ->
cancel_pending` is not allowed, since nothing was sent. A cancel that fails is sent again each
cycle while the order still rests locally, and stops once it has gone. Exits, brackets and the
auto hedge treat `cancel_pending` like `submitted`.

**One market-data age** (`Config::data_max_age`). `market_data_max_age_seconds` in `desk.json`
(0 means 3 x poll + 10 = 25 s) is the one limit for opening risk. It is used by:
- the strategy engine's quote TTL (already 25 s);
- the board's `quote_stale` / `fair_stale` (was 60 s);
- the systematic screen, which reads it from the board;
- the retry's re-screen.

The executor needs no limit of its own because it validates against a market it has just read
from the exchange. Managing what is already on keeps the older rule: 60 s through
`quote_stale_manage` / `fair_stale_manage`, with the 120 s backstop. That covers exits, stops and
a resting entry's re-validation. Refusing to manage a position because a slow cycle left its quote
26 s old would skip a stop, and pulling resting entries on it would bring back the cancel storms.
A captured book is merged into the research board while it is young enough to manage on, or when
it is at least as new as the market row. Only a capture past 60 s and older than the row is
dropped, and that row then reads like one with no capture. Before this fix, an old capture
overwrote the fresher quote and left the row stale indefinitely.

**Book panel.** Lane 2 (for you, blocked, research) is counted from the last full screen
(`systematic_view`) and shows `-` until a screen exists or once it is over a minute old. It used
to read `systematic_.view()`, which carries no screen, so it always counted zero.

```
desk-tests 827 (was 753), systematic-tests 324 (was 235 with 1 failure: the stale H fixture),
perps 144, poly 142, account-summary 36, routing 24: 0 failures; manual-order, pair-model PASS
node tests/test_ui_routes.cjs 42 · test_ui_keys 11 · test_ui_render 11 · test_ui_book 14 · scroll OK
```

New tests:
- systematic: `test_routes`, `test_screen_routes`, `test_rejection_classes`,
  `test_stale_halt_cleared_at_startup`, `test_post_only_cross_does_not_halt`,
  `test_post_only_retry_paths`, `test_management_keeps_its_own_age`;
- desk: `test_cancel_pending_transitions`, `test_failed_cancel_is_retried`,
  `test_route_capture_and_freshness`, `test_research_board_keeps_manageable_capture`,
  `test_refusal_watermark`, `test_auto_submit_soft_rejection`.

An independent review ran twice. Each new test fails with its fix reverted. `tests/test_dashboard.cjs`
still fails at line 169 (the modal's "expected at p(win)" line), exactly as it did before this
session.

### Open

- Weather, sports and crypto are still `auto`, in the defaults and in the saved settings, and
  none of those models has met session 10's bar. Moving them to `manual` or `research` is one
  click each on the Systematic tab, and it is Sam's call.
- The brief's Phases 1-13 are not started.
- `run_exits` checks the model fair's age at 3 x poll + 5 (20 s), separately from both limits.
- `book_capture_plan` does not rank `auto` rows ahead of manual or research ones, so with more
  than 20 candidates an `auto` row can miss a capture.
- The merge rule for a capture older than 60 s has no engine-level test; it would need a minute
  of wall clock or a hook.
- Seen in passing: the Polymarket key answers 401, and a stale `.git/index.lock` sits in the tree.

---

## Session 13 - why the algorithm lost, Polymarket, one board (2026-09-09)

Sam's list: the header (drop PROD, AUTO, instant-edge, daily stop, cycle, markets; P&L and balance
as event contracts + perps + Polymarket, shown apart), Polymarket US in the desk with a
systematic Kalshi/Polymarket pair strategy, the Signals and Research boards merged into one
ranked list of the best trades right now with event exclusivity and the perps labelled in it,
one way to send a hand order (double-click, a confirmation with the upside and downside), no
scroll bars, and why the algorithm was losing money with so many cancelled orders.

**Why it lost.** Two things, both in the entry and exit rules rather than the estimates. Entry:
a maker order was posted at the bid on thin four-cent weather books and re-posted at the same
price after every 180 s pull, so 20 of 23 posts never filled (the log's cancel storm). Exit: the
price stop fired on the first cycle after a fill against a bid that was a hole in the book
(filled 0.85, stopped 12 s later into a 0.63 bid the model valued at 0.92). Neither loss was a
model being wrong. Changes in `src/systematic.cpp`: an entry ladder (each unfilled re-post
climbs a tick while the edge net of fees and the sigma buffer still clears `min_edge`, up to
`ladder_ticks`, and becomes a taker when reaching the offer still clears; past the ladder the
market waits fifteen minutes); the stop now needs a two-sided book, is judged on the mid, has a
`stop_grace_seconds` after the fill and must persist `stop_persist_seconds`; a thesis stop
(model p(win) falls `stop_loss` under entry) exits as a taker only into a deep bid, else as a
maker; exits are maker orders one tick inside the offer that step down a tick per
`exit_ttl_seconds` (`exit_steps`) and become takers only when the ladder is exhausted or the mid
is twice the stop under entry. The paper trader uses the same plan. 168 checks.

**Polymarket US.** `PolyDesk` (`include/desk/poly.h`, `src/poly.cpp`, `docs/polymarket.md`):
Ed25519-signed client, universe by league, matching by league, Eastern game date (and the ticker
time for doubleheaders) and team, quotes flipped into Kalshi-YES terms, account sync, rails,
ticket with the exchange's preview, cancel, close, the kill sweep, and the pair rule in paper or
live (`xvenue_trades`). The `xvenue` strategy makes the Polymarket mid the fair for any Kalshi
game market it also lists. Header cells, a Polymarket tab, eleven endpoints. 121 checks. What
the venue has: sports game winners only, no weather, no short-dated crypto; NFL prices within a
cent of Kalshi's, so instant locks are rare and the realistic pairs are maker-on-Kalshi /
taker-on-Polymarket for one or two cents on the smaller games. The lock arithmetic went through
two corrections during the session that matter: the first cut of `best` took the maximum over
all eight routes, which on a 65-cent-wide book was a maker/maker "lock" of 67 cents that would
never fill; `best` is now only a route whose Polymarket hedge crosses, and both books must be
within `max_spread`. The first cut of the matcher paired by date ±36 h, which put Tuesday's
MLB game against Monday's Polymarket market (a 45-cent "lock"); it now needs the exact Eastern
game date, uses the ticker time, and gives each Polymarket market to one Kalshi event.

**One board.** `#tradespanel` replaces the Signals board and the Research board: every source
ranked by edge per dollar (Kelly for a Kalshi row, the locked return for a pair, the basis for a
perp), instant pairs first, one pair a game, top 15/20/40, chips by category, ALGO rows
switchable. Rows leave when they stop being actionable: the entry policy already blocks the
other sides of an event you hold or work, which is the Wisconsin/Notre Dame rule. Pending
proposals no longer count as working orders in `entry_check` (`src/risk.cpp`): a proposal nobody
has sent reserves nothing, and letting it block made every event with a strategy signal an
"event with a working order" on the screen (the 321 rows in Sam's screenshot) and refused hand
orders on those events. The proposal queue moved to the Strategies tab as the auto/instant
lane's input. Double-click or `trade` opens the modal: price, contracts, style, the caps from
`/api/size`, cost, payout if right, loss if wrong, expected value at p(win), breakeven, the
algorithm's take-profit and stop levels, edge at the chosen price; Enter sends, Esc closes. A
pair row opens the pair modal (route, legs, contracts, locked per contract and total, capital,
return). A perp row jumps to the Perps ticket. Scroll boxes are gone (`.scroll` no longer clips).

**Header.** API key · P&L today (K · perps · PM) · P&L total · unrealized · settling · balance
(K · perps · PM, with the basis and pair rule pills) · open/cap · freshness (with `pm`) · sync ·
KILL. The unattended-sends switch and the instant-edge lane live on the Strategies tab.

**A parallel session's work, merged.** While this session ran, another agent session edited the
same tree on the Mac (files dated 2026-09-09 02:00-02:20 UTC, built into `build-ci/` there, never
into `build/`): a read-only Polymarket adapter (`src/polymarket.cpp`), a read-only cross-venue
research service with its own thread and a "Cross-platform" tab (`src/crossvenue.cpp`), an
account-summary module (`src/account_summary.cpp`), an order dialog in its own copy of the UI, and
a set of execution fixes in `src/systematic.cpp`. What was kept: the execution fixes, ported into
this session's systematic trader (a resting entry is re-validated at its own limit every cycle,
`systematic_entry_cancel_reason`; a fill that races the cancel is counted from the exchange's
`reduced_by`; recorded fills price the entry and the exit when they cover the whole quantity;
take-profit is measured net of both fees; `execution_stats` with the fill rate and every pull
reason, shown on the Systematic tab), with its two tests (`test_resting_entry_revalidation`,
`test_partial_entry_cancel_race`); and the account-summary module with its 36 checks and
`GET /api/account-summary`. What was not built: `src/polymarket.cpp`, `src/crossvenue.cpp`,
`tests/test_crossvenue.cpp` and `tests/test_dashboard.cjs` stay in the tree but are not in
`CMakeLists.txt`; `PolyDesk` covers what the adapter read and also trades, and the research
service would have paged the whole 60,000-market universe on a second client next to it. Its UI
copy was superseded by this one (the order dialog's job is the board modal). Revive either from
the files if wanted; nothing was deleted.

```
cmake --build build-ci && ./build-ci/desk-tests && ./build-ci/systematic-tests && ./build-ci/perps-tests && ./build-ci/poly-tests && ./build-ci/account-summary-tests
# 716 + 187 + 144 + 121 + 36 checks, 0 failures
```

Verified headless (Chromium against a public-only instance with NFL, CFB and MLB watched): the
board fills, the Kalshi modal opens on double-click and re-prices on edits, the pair modal opens
from a pair row, the Polymarket tab renders 60 pairs, no console errors.

### Open

- Sam's Polymarket secret was pasted into chat in a screenshot: it has to be rotated at
  polymarket.us/developer and the new one put in `~/.polymarket/secret` (mode 600), with
  `params.polymarket.key_id` and `secret_path` in `desk.json`. The desk never stores the secret.
- The pair rule has run in paper only. Fill probability of the resting Kalshi leg is unmodelled;
  the first live pairs should be one or two contracts with the tab open.
- Settlement-rule parity is checked for NFL ties and read for MLB/college; soccer draws are not
  paired. Postponements and voids are the residual risk.
- The perps row on the board uses a 10 bps basis threshold; the sampled basis never reached it.
- `sports_live`'s scoreboard feed is unchanged; the xvenue fair does not use it.

---

## Session 12 - perps, and the screen's honesty (2026-09-08, evening)

Three asks. Sam has cash on the margined instance and wants the perps traded from the desk;
he asked what the "actionable" chip on the Systematic screen means, holding up a row that said
`manual` with a raw model of 94% against a market at 2%; and he wants the research board's
estimates tradable, with the research done for both.

**Perps.** `PerpsDesk` (`include/desk/perps.h`, `src/perps.cpp`, `docs/perps.md`): the
margined instance read, traded and funded from the desk. Header cell, a Perps tab, eight
endpoints, rails in `params.perps` (order $30, total $60, 2x, a 2% band around the mark, the
kill switch and its sweep), transfers both ways, and the basis-reversion rule in paper or live
with a TTL, a re-post backoff, a cooldown after any close, a past-the-stop guard and its own
loss stop. 144 checks in `tests/test_perps.cpp` through a mock margin exchange. Before the rule
was wired the desk sampled the BTC and ETH perps for 44 minutes
(`research/perps_basis_2026-09-08.csv`): basis within 3 bps of the index, spread under 1 bp,
never once past 10 bps. At the documented retail fees (5 + 12 bps) there is no fee-clearing edge
in fading the basis in normal conditions; the rule fires only in a dislocation and says so on
the tab. The honest uses of perps here are a hedge for crypto bracket exposure and hand-held
views behind the rails.

**The screen's mislabel.** That row was `KXHIGHNY-26SEP08-B80.5 NO`: raw model 0.943, market
0.02, a 0.93 disagreement. The engine's 0.30 rule had flagged it, but the strategy's own research
reason pre-empted the engine's text, and the screen read the wrong string. The screen now
computes the hard gap itself (`systematic_screen(..., hard_gap = risk.max_disagreement)`) and
blocks the row for everybody: "the market is the better estimate here; the model needs looking
at, not trading". Verdict priority is blocked > weak > manual > algo. The chip labels say what
they mean: ALGO + YOURS, ALGO, YOURS, weak, blocked. A maker entry pulled unfilled three times
in a row now waits fifteen minutes before it is posted again (the NY B84.5 loop in the log).

**The research board.** Crypto rows sat on "volatility warm-up" for half an hour after every
restart because the realized-vol window fills from live prices. `crypto_lognormal::prepare`
now backfills the last three hours of one-minute closes from Coinbase candles when the window is
short, so the board is live within a cycle of a restart. Weather rows had two reasons that were
answered facts, not input failures (see Session 11). What remains untradable on the board is
untradable for a reason the row prints: no model (econ, commodities, indices are `off`), the
disagreement gate, settlement outside T+2, a stale book.

```
cmake --build build-ci && ./build-ci/desk-tests && ./build-ci/systematic-tests && ./build-ci/perps-tests
# 715 + 137 + 144 checks, 0 failures
```

### Open

- Perps fees: `/margin/fee_tiers` is read hourly; until it answers, the rule and the ticket
  preview assume 5/12 bps.
- The basis rule has no queue model in paper mode (a resting entry fills when the touch trades
  through it), same caveat as the systematic trader's paper fills.
- Paper P&L for perps ignores funding (a position held across a funding time pays or receives
  it live; the rule's two-hour hold makes this small, not zero).

---

## Session 11 - the systematic trader (2026-09-08)

Sam asked for an algorithm he can start from a screen that then trades real money on its own for
the rest of the day, with its performance visible and its parameters editable, and for the
research board's estimates to be tradable with a balance struck between edge and the probability
of winning. Sources he named: weather, sports, crypto and the arbitrage baskets. Live from the
first click, small size.

**What was built.** `SystematicTrader` replaces the paper-only lab that a parallel session left
half-wired (its `tests/test_systematic.cpp` did not exist, so the tree did not configure). One
engine, live or paper, running inside the cycle after the exits and before the queue. Full
description in `docs/systematic.md`; the short form:

- A screen with four verdicts per side - algo, manual, weak, blocked - and the reasons in words.
  The algorithm takes edges inside `[min_edge, max_edge]` (0.03-0.12) with p(win) >= 0.55, fresh
  two-sided books, validated inputs, a settlement inside 48h; ranked by Kelly fraction so a small
  edge on a likely winner outranks a big edge on a coin flip. Everything above the cap is yours.
- Entries through `Executor::submit` (all caps apply), `auto` style: take when the edge survives
  the taker fee, otherwise post a bid and pull it after a TTL. Exits: take-profit, stop, time
  stop, hold-value-vs-sale, settlement; reduce-only IOC into the bid.
- Its own daily budget ($30) and loss stop ($6), 5 positions, $15 open, $3 a trade by default.
  Runs across midnight unless told to stop at day end; resumes after a restart.
- Locked baskets are sent by it, every leg, when arbitrage is on.

**Two things found on the way, both fixed.**

- `entry_context` refused every basket leg: the legs of one basket saw each other as "an event
  with a working order". No basket had ever been sent on this desk (0 `ladder_arb` proposals in
  the database), so nobody had hit it. Legs of the same basket no longer block each other, and a
  basket leg is exempt from the stack check that judges it against the independent-settlement
  model. Test: `test_basket_send`.
- `weather_nws` marked every current weather market's inputs invalid for two reasons that
  `docs/settlement.md` had already answered: "The Weather Company settles this; equivalence
  unverified" (verified, six for six) and the civil-midnight/standard-time window (a documented
  one-hour boundary effect). Both are warnings on the row now, not input failures. The engine's
  weather gates (WX-1b/1c, `weather_source_supported`) are unchanged.

`run_exits` skips the algorithm's positions (`owned_tickers()`); a hand bracket on one of them
still wins. `is_exit_strategy` knows `sys_exit`. Board rows carry `tick`. The Systematic tab is
rewritten; the old paper lab endpoints (`/api/systematic`, `/api/research/screen`) keep their
paths with the new shapes, plus `POST /api/systematic/close/{ticker}`.

```
cmake -S . -B build-ci -G Ninja && ninja -C build-ci
./build-ci/desk-tests          # 715 checks, 0 failures
./build-ci/systematic-tests    # 133 checks, 0 failures
```

### What the evidence says, for the record

Sam asked that his own settled trading data not be used as the reference for design decisions,
and it was not. The desk's model studies (WX-1b, WX-1c, the Brier table in `docs/platform.md`)
still say the market beats every model here, and the cap, floor, blend and size limits exist
because of that. The systematic screen prints the research reason on every such row.

### Open

- `board_view(0)` runs `entry_context` for every open market (two to three queries each,
  ~2,200 markets) and is now called by the engine each cycle and by the tab every 5 s. Fine on
  the 2026-09-08 numbers; worth a per-event cache if the cycle time grows.
- Paper fills are taker-only; a maker fill cannot be simulated honestly without queue data.
- The algorithm's own P&L is from its ledger (fills it sent, settlement by result). A position
  closed by hand or by a bracket is valued from the fills table since the entry.
- Deployment note: the first rebuild on the Mac failed to link (x86_64 objects against arm64
  OpenSSL). Cause: cmake 3.31 from the conda `(base)` shell, an Intel binary under Rosetta,
  reporting an x86_64 host; the working builds used cmake 4.4.3 (arm64). The failed link deleted
  `build/kalshi-desk`. Fix in the README: fresh `build/`, `-DCMAKE_OSX_ARCHITECTURES=arm64`.

---

## Session 9 - the dashboard (2026-09-07, started)

Deliberately small. The existing layout is the one Sam wants; this adds to it rather than
replacing anything, and removes nothing.

**Freshness, per source, independently.** One new header cell showing the age of each input:
`cyc` (engine cycle), `px` (crypto prices), `wx` (weather observations), `spt` (live match state),
`mdl` (newest model fair), `disc` (contract discovery). Each coloured against its own thresholds,
because they run at different rates - a match feed is legitimately quiet when nothing is playing
and a price feed is not.

The engine publishes raw timestamps and the page subtracts them from the `now` the server stamps
at request time. Publishing a pre-computed age would freeze it at whatever it was on the last
cycle, which is precisely the reading that must not be trusted.

**An unreachable desk no longer looks like a working one.** The status poll used to fail into a
small error string while every number stayed on screen reading as current. The page now marks
itself OFFLINE, dims the header and body, and says what time the values were last true. On
2026-09-07 the desk was dead for nine and a half hours behind a header that looked entirely
ordinary, which is how a ten-hour outage went unnoticed.

### Also done (same sitting)

**A position now says what it is worth if you get out now.** The risk book showed the bid and
`bid_value`, which is gross: selling costs a taker fee, so what actually returns is less. "Sell
now (net)" is that number with the fee taken off, next to what it leaves against cost. A page
answering "is this worth taking" with a figure that ignores the cost of taking it is answering a
different question. Where there is no bid it says so rather than inventing a value.

**`cancel_pending` is a real state now.** It was added to the transition table and nothing wrote
it. `cancel()` marks the proposal before making the call, so a cancel that hangs or dies mid-flight
leaves a proposal saying what is true - a cancel was asked for and the outcome is unknown - rather
than one still claiming to rest. Reconciliation resolves it from the exchange's own view.

**Per-candidate reasons were already there.** Checked rather than assumed: `research_only` and
`research_reason` are carried through `board_view` and rendered both as the gate label and in the
ticket panel. No change needed.

### Open

- Separate Weather and Soccer views: not done.
- Held positions still do not show a settlement-scenario table per event, or the next
  reassessment condition (that is session 6's exit policy, which does not exist yet).
- Updates are still polled on timers rather than backend-driven with reconnect recovery.
- No soak test across midnight, sleep/wake and network loss.

---

## Session 8 - order states (2026-09-07, started)

Taken next because EXEC-1 was a live instance of exactly what this session covers.

**An uncertain order could deadlock the desk.** When a submission fails ambiguously the desk
records the client order id and blocks new entries until it knows what happened - correct - but it
only ever cleared the block when the order was FOUND in the order history. An order that never
reached Kalshi leaves nothing to find, so the block was permanent with a dashboard button as the
only escape. It also never wrote `uncertain_since`: the key was cleared in two places and set in
none, so it always read zero. Absence now counts as evidence, but only where absence means
something - a complete history (not a full page, no continuation cursor) reaching back before the
submission, read at least two minutes after it.

**A resting exit could outlive its position, and I introduced that risk myself.** Removing the
exchange's `reduce_only` flag from resting exits in EXEC-1 was necessary - Kalshi refuses it - and
I said the guarantee would be enforced locally by sizing to the position at submit time. That was
not enough. `run_exits()` iterates positions, so a ticker whose position had gone was never looked
at again, and a still-working exit would have opened the opposite position if it filled. That is
the failure the plan names directly. A sweep now runs first, over resting orders rather than
positions, and pulls any exit larger than what is actually held. It logs the intent before
attempting the cancel, because a cancel that fails leaves the danger in place and that has to be
visible rather than swallowed.

```
ninja -C build-ci && ./build-ci/desk-tests    # 642 checks, 0 failures
```

### Finished (same sitting)

**Proposal states are a machine now, enforced on write.** Statuses were plain strings written from
a dozen places with nothing to stop an illegal move. `Store::update_proposal` refuses one and logs
it rather than writing a corrupted row that looks ordinary and fails somewhere else later. The
rule that matters: terminal is terminal. A filled or cancelled proposal returning to pending would
be picked up and sent a second time; a submitted one reset to pending would lose the order it
already owns. No existing flow violates it, which the suite confirms.

**A fill landing during a cancel is no longer lost.** `cancel()` read `fill_count` out of the
local orders table - whatever the desk last synced - and marked the proposal cancelled. Kalshi's
cancel response says `reduced_by`: how many contracts actually came off the book. Anything that
was resting and did not come back filled while the cancel was in flight. The desk now takes the
difference, records the partial fill, and re-reads the portfolio instead of carrying on from a
position it knows is wrong. The test drives three of five contracts filling mid-cancel.

**Reservation is serialized, not atomic against the exchange.** Every send path - `submit`,
`submit_exit`, `ticket`, `close_position`, `cancel`, `reconcile` - takes the same recursive
`order_mu_`, so the desk cannot race itself between checking funds and sending. It cannot be
atomic with respect to the exchange, where a resting order filling elsewhere can consume the
collateral between the check and the send; that is what the exchange's own rejection and the
on-demand shard top-up are for. Worth stating plainly rather than claiming a guarantee that does
not exist.

```
ninja -C build-ci && ./build-ci/desk-tests    # 680 checks, 0 failures
```

### Still open

- Kalshi's own order statuses, as opposed to the desk's proposal statuses, are still taken as
  given rather than validated against a machine of their own.
- The dashboard does not show the new proposal states; that is session 9.

---

## Session 4 - the weather model (2026-09-07)

**Ladder coherence was already met.** Checked against the live fair log before changing anything:
every weather ladder sums to between 1.000 and 1.004. The existing model reads all six brackets
off one normal on the day's maximum, so they cannot fail to be consistent.

**The defect is where that normal is centred.** It sits at the largest hourly point forecast, and
the maximum of the expected hourly values is not the expected maximum. When several hours sit near
the top of the day, whichever of them runs warm becomes the day's high, so the true centre is
above the highest forecast. Measured on a flat-topped afternoon (90/92/93/93/92/90):

| | lift over the peak forecast |
|---|---|
| hour-to-hour noise 0.5F | +0.15F |
| hour-to-hour noise 1.0F | **+0.34F** |
| hour-to-hour noise 2.0F | +0.91F |
| hours perfectly correlated (rho 0.95) | +0.13F |
| hours independent (rho 0) | +0.69F |

The lift comes from hours varying independently near the peak, which is why it grows with the
noise and shrinks as the hours move together. On a one-degree bracket +0.34F is several points of
probability, and it was missing in one direction every day.

`simulate_extreme()` draws the remaining trajectory - one day-level bias shared by every hour,
plus an AR(1) wiggle - takes the maximum of the path and the running observed maximum, and applies
the station's settlement shift as a mixture rather than an average. The ladder is then read off
one distribution, so it sums to one by construction, and the settlement uncertainty stays in the
answer. A daily minimum is the same computation with the sign reversed, and shows the mirror
image: -0.35F below the lowest hourly forecast.

### Changed

| file | change |
|---|---|
| `include/desk/wxdist.h`, `src/wxdist.cpp` | New. `MaxDistribution` over the settled whole-degree value, and `simulate_extreme()` which builds it from a simulated trajectory. Deterministic per seed. |
| `docs/experiments.md` | New. WX-EXP-1 (Austin) and WX-EXP-2 (Chicago) frozen in full: hypothesis, decision times, features, split, metric, sample floor and the graduation rule, all fixed before any data exists. |
| `tests/test_main.cpp` | `test_wx_distribution()`: the peak lift and its two physical relationships, ladder coherence, the running-maximum floor, the shift mixture, determinism, and the minimum case. |

```
ninja -C build-ci && ./build-ci/desk-tests    # 629 checks, 0 failures
```

### Open

- **Not yet wired into `weather_nws`.** The strategy still prices brackets from a normal centred
  on the peak forecast. Swapping it for `simulate_extreme()` is the next step and is the point of
  having built it.
- `sigma_hour` and `rho` are assumed (1.0F, 0.7) rather than fitted. They set the size of the
  lift, so they should be estimated per station from the recorded hourly series once there is one.
- NBM and HRRR are not ingested. The model still reads the NWS hourly grid forecast; the plan asks
  for calibrated blended guidance and a high-resolution trajectory, which is a separate fetch.
- The daily-minimum shift is still assumed symmetric with the maximum and unmeasured.
- **The experiments cannot report for about four months.** 0 of 120 station-days each.

---

## Session 7 - the risk book (2026-09-07)

Taken out of order because sessions 4 and 5 are blocked on data that starts accumulating only
now, and because this one is about money currently at risk.

**$25 of a $110 account is invisible to every risk number.** `/portfolio/balance` returns the sum
of exchange shards 0-3; the margined instance the money moved to on 2026-09-06 is not among them,
and the shard sum matches `account.balance` to the cent. Equity, the daily stop and the position
caps have all been computed on 77% of the account. Now reported as `untracked_transferred` and
logged.

**The desk could not explain its own cash.** Every fill was treated as a purchase, but a Kalshi
binary pair nets - buying YES while holding NO closes the position and returns $1 a contract at
once. Reconciling 2026-09-06 that way left $46.00 unexplained. With netting modelled, the day
reconciles to $0.00. See `docs/hypotheses.md` RISK-1.

**A correction I owe the trading log.** I told Sam his day was -$17.06 and his all-time -$26.78.
Both were wrong: I subtracted the $25 transfer from equity without noticing it had also left the
balance I was measuring. The day was **+$10.02**. The desk's own header was right.

### Changed

| file | change |
|---|---|
| `include/desk/risk.h`, `src/risk.cpp` | `reconcile_cash()` explains the day's cash movement with contract netting modelled, and reports a residual when it cannot. `untracked_transferred()` names money that has left the readable balance. `daily_equity` carries both. |
| `src/engine.cpp` | The daily stop logs when cash does not reconcile instead of computing on a number nobody can explain, and periodically states how much sits on an instance the desk cannot see. |
| `tests/test_main.cpp` | `test_cash_reconciliation()`: the netting case, settlements and transfers, and a deliberately broken balance that must be reported rather than absorbed. |

### Commands run

```
ninja -C build-ci && ./build-ci/desk-tests    # 564 checks, 0 failures
```

### Verified live (2026-09-07 03:15, after the rebuild)

The first build of all six sessions went in at 03:14 and the desk came up clean: no sources newer
than the binary, no errors in the event log, account syncing.

- `contract_universe` populated with 2,208 contracts. The twelve verified weather series carry
  their settlement product and zone; every one is classified `today` or `tomorrow` correctly for
  its own timezone.
- **2,136 of 2,208 contracts are `unknown`** - 97% of what the desk watches has no settlement
  mapping anybody has read. That is crypto, MLB, NCAAF and nineteen soccer leagues. It is the
  honest state of the universe rather than a defect in it, and it is a large piece of work.
- `observations` recording real books: ten levels a side with sizes.
- Two live defects found and fixed from that first run: the desk was re-fetching an empty book
  every cycle for a market that had already gone `determined` (Wisconsin-Notre Dame, closed
  03:09, fifteen identical empty rows), and it recorded every book whether or not it had changed.
  Books are now skipped once a market stops trading and recorded only on change, which keeps the
  timeline exactly reconstructible at a fraction of the volume.
- The three-zone rollover has not been exercised yet: at 03:15 UTC all of New York, Chicago and
  Los Angeles were still on 2026-09-06. New York crosses first at 04:00Z, and the universe should
  then hold `today` rows for two different dates at once.

### The margined instance cannot be read

`/portfolio/balance` accepts only `subaccount` and `exchange_index`, and Kalshi does not document
which index addresses the margined instance, so there is no way to value the $25 that was sent
there. `untracked_transferred()` reports the net amount SENT, which equals its worth only if
nothing has been traded on it. Recommend moving it back to the event-contract instance by hand;
the desk can transfer shard-to-shard but has no call for the return leg.

### Open - this is a fraction of session 7

- **Reading the margined instance is not implemented.** The desk knows money left and cannot see
  where it went. Until `/portfolio/balance` is queried for that instance too, equity is knowingly
  incomplete.
- **The other entry paths are not yet unified.** Manual tickets, strategy entries, working orders
  and hedges do not all pass the same limit checks; that audit has not been done.
- **Nested same-direction legs.** `stacks_event` exists but let PAR YES sit alongside OLM NO on
  2026-09-06, which is one bet twice, not a hedge. Not yet fixed or tested.
- Turnover and correlated-group limits do not exist. Fractional Kelly is still gated only by caps.
- The reconciliation was verified against the live database in Python; the C++ path has unit
  tests but has not been run against 2026-09-06 itself.

---

## Session 3 - recording and replay (2026-09-07)

**The headline is a subtraction.** The desk has never recorded an order book - `books` is created
at startup and nothing writes to it, zero rows across both databases. The quote history is 32
hours long. And `hist_markets` / `hist_trades`, which WX-1b/1c/1d read, no longer exist. So no
execution study has ever been possible here, none of the existing ones can be reproduced, and the
return columns in the weather results are not evidence. Their Brier comparisons are unaffected and
still carry the conclusion. See `docs/hypotheses.md` REPLAY-1.

### Changed

| file | change |
|---|---|
| `include/desk/record.h`, `src/record.cpp` | New. `Observation` keeps `valid_ts` / `init_ts` / `published_ts` / `received_ts` apart. `ReplayCursor` serves only what had arrived by the simulated time, counts what it withheld, and refuses to run backwards. `Arrival` and `StudyKind` make the basis and the claim explicit. |
| `src/store.cpp` | New `observations` table with indices on arrival and validity. |
| `src/engine.cpp`, `include/desk/engine.h` | `record_books()` snapshots the book under every pending proposal and open position, 20 per cycle, into `observations`. |
| `research/execution.py` | New. Study labelling, a fill model with a latency budget, an unfilled-share report, and a hard failure when a study's tables are gone. |
| `research/wx_evening.py` | Relabelled prediction-only. Fills held to a 60s budget instead of three hours; unfilled decisions counted and printed. Aborts if its tables are missing. |
| `research/wx_dayahead.py` | Relabelled prediction-only. |
| `tests/test_main.cpp` | `test_replay()`: the 12Z-cycle trap, withheld counting, unknown publication time, monotonic clock, determinism. |

### Commands run

```
ninja -C build-ci && ./build-ci/desk-tests    # 549 checks, 0 failures
```

### Open

- **Recording only starts now.** Nothing here recovers history. The weather shift, the in-play
  model and any execution claim all need calendar time before they have a sample.
- Forecast ingestion is not yet wired into `observations`; only books are. The weather and ESPN
  feeds still write their own tables with a single timestamp. Wiring them is mechanical and
  should happen before the next weather work, or session 4 will refit on the same thin data.
- `published_ts` is 0 for everything recorded so far. NOAA publishes product-specific
  availability schedules and those are what should populate it for forecast products; until then
  only the received-time basis is usable, which is the conservative one.
- No replay of a whole decision yet. `ReplayCursor` enforces the rule; nothing yet drives a
  strategy through it end to end and compares against what the desk actually did.

### Next

Session 4 (the weather model) is blocked on data that does not exist yet. The useful order is to
wire the weather and sports feeds into `observations` first, let it run, and meanwhile do session
7 (the risk book) or session 8 (execution states), neither of which needs a historical sample.

---

## Session 2 - settlement verification (2026-09-06/07)

**The headline.** The weather model is aimed at the right target. Kalshi moved settlement to The
Weather Company on 2026-08-27 and the rules stopped naming the NWS, which raised the possibility
that `weather_nws` had been fitting a station Kalshi does not settle on. It has not: TWC's settled
value equals the NWS single-station climate-report maximum at all six cities tested. See
`docs/settlement.md` and `docs/hypotheses.md` WX-SETTLE-1.

**The finding that matters more.** The correction the model applies between its own observations
and the settled value was invented, not measured. One global `+0/+1/+2 F` distribution for every
station, with a comment claiming 210 settled station-days of support that did not exist - it was
fitted from which brackets won, which never observes the settled value directly. Measured against
the climate report, the gap is a property of station reporting frequency: Central Park files ~38
observations a day and needs +0.87 F; the airports file ~250 and need +0.20 to +0.60. Austin's is
negative. One constant was wrong in both directions at once.

### Changed

| file | change |
|---|---|
| `docs/settlement.md` | New. The verified map: series, station, climate product, site, reporting frequency, observation window, rounding, close and publication times, and what is still unverified. |
| `include/desk/strategies.h` | `Station` gains `cli_product` and a per-station `settle_shift`. |
| `src/strategies/weather_nws.cpp` | Per-station shift replaces the global constant. New gate: a series with no verified `cli_product` is research-only whatever the model says. Station table now carries the confirmed climate product per series. |
| `research/wx_settle_basis.py` | New. Measures the shift directly against NWS climate reports, and breaks it down by station reporting frequency. |
| `tests/test_main.cpp` | The shift test now configures a known shift and checks the mixture arithmetic instead of a constant that moves on every refit. New test for the unverified-settlement gate. |

### Commands run

```
python3 research/wx_settle_basis.py --days 7 --products 40
ninja -C build-ci && ./build-ci/desk-tests     # 472 checks, 0 failures
```

### Session 2b - the universe (same sitting)

The second half, finished. `src/universe.cpp` is now the single settlement map; `weather_nws`
reads its station, zone and product from it instead of keeping a second copy that could drift.

- Four more series read from Kalshi's live rules: `KXLOWTCHI` settles on CLIMDW (the same station
  as the Chicago high), `KXLOWTMIN` on CLIMSP. `KXHIGHTNYC` and `KXHIGHTCHI` return 404 and have
  been deleted from the station table. `KXHIGHHOU` and `KXDENHIGH` exist but listed no market to
  read, so they stay research-only.
- `KXBRASILEIROGAME` mapped as the plan's starting league. Its rules settle on the result "after
  90 minutes plus stoppage time (does not include extra time or penalties)", which independently
  confirms session 1's decision that `MatchPhase::Stoppage` must not be tradable.
- `classify_contract()` places every contract in its own zone: future / tomorrow / today /
  day_done / awaiting_settlement / settled / unknown. Stored in `contract_universe`.
- `Engine::refresh_universe()` reruns discovery every five minutes **and immediately when any
  settlement zone's local date turns over**. That is the anti-stale trigger: New York rolls three
  hours before Los Angeles.
- Found while mapping soccer: those markets carry a close three days after kick-off and an
  expected expiration hours after it - expiration comes first, the reverse of weather. The
  classifier takes whichever is earlier, or a finished match would sit listed as tradable for days.
- Corrected in the docs: weather markets close at **01:00 local**, not local midnight.

Tests: 530 checks, 0 failures (was 472 at the end of session 2a, 417 before session 1). The
rollover test asserts one instant classifying differently in three zones.

### Open

- The daily-low shift is assumed symmetric with the daily high and has **not** been measured.
- **The sample is thin.** 17 station-days. `api.weather.gov` serves about a week of climate
  products, so it cannot be extended backwards from this source; the recorder in session 3 is what
  makes it grow.
- **Correction and revision handling is unknown.** CLI products are reissued as corrections and it
  is not established which issuance TWC takes. The research script takes the latest.
- **KXHIGHHOU, KXDENHIGH, KXHIGHTNYC, KXHIGHTCHI have no verified settlement product.** They are
  research-only until somebody reads their rules. Reading them is cheap and should happen next.
- **The second half of session 2 is not done.** Discovery refresh, the midnight rollover, and the
  today / tomorrow / ongoing / awaiting-settlement classification are not implemented. The
  settlement verification was the part that could invalidate everything downstream, so it went
  first. The `done when` for session 2 is only half met.
- Soccer: the league universe (Brasileirão as the plan's candidate) has not been established. The
  soccer half of session 2 is untouched.

### Next

Finish session 2: read the four unverified series' rules, then implement discovery refresh and the
event lifecycle classification with a midnight-rollover test. Then session 3, which everything
downstream depends on - without recorded point-in-time inputs, neither the weather shift nor any
execution claim can be checked.

---

## Session 1 - soccer match-state lifecycle (2026-09-06/07)

**Why.** `SportsInplay::prepare()` fitted scoring rates in its second section and ingested ESPN
state in its fourth. After any restart mid-match the state map was empty on the first cycle, an
empty map read as "before kick-off", and the fit calibrated a full-match model to a book that
already contained goals. See `docs/hypotheses.md` SPORT-STATE-1 for the full incident with the
numbers from the live database.

**Corrected a wrong diagnosis.** An earlier note in the trading log blamed ESPN reporting a
`"halftime"` state the code did not match. Fixtures captured live on 2026-09-06 disprove that:
`type.state` is only `pre`/`in`/`post`, and half time arrives as `state="in"`,
`type.name="STATUS_HALFTIME"`, `period=1`, `displayClock="45'+3'"`. The state field was fine.
The ordering was the defect.

### Changed

| file | change |
|---|---|
| `include/desk/strategies.h` | `MatchPhase` enum + `phase_name` / `phase_parse` / `phase_started` / `phase_in_play` / `phase_from_espn`. `MatchState` gains phase, phase_ts, data_ts, from_disk. `EventInfo` gains full fit provenance and `MODEL_VERSION`. New tunables: `fit_tolerance` (0.02), `inplay_min_progress` (5.0), `stale_seconds` (90). |
| `src/strategies/sports_inplay.cpp` | `prepare()` reordered: group → **ingest ESPN and settle phase** → fit → log. Fitting extracted to `fit_rates()`. `load_match_state()` / `save_match_state()` persist across restarts. Convergence residual computed against the fit's own snapshot. `fair()` gates on phase, not on the raw state string. Fixed `value("period", 0)` throwing on ESPN's `"period": null`. |
| `src/store.cpp` | New tables `match_state` and `rate_fits` (+ index). |
| `tests/test_main.cpp` | `test_match_phase()`, `test_inplay_restart()`. |
| `tests/fixtures/espn/status_shapes.json` | Real provider capture, 2026-09-06, one event per distinct (state, name, period). 4.6 KB. |
| `CMakeLists.txt` | `DESK_SOURCE_DIR` define so tests can find fixtures from any working directory. |

### Invariants now enforced

1. Missing or unrecognised provider data maps to `unknown`, never `prematch`.
2. A match never un-starts: a stale pre-match row for a fixture already seen in play keeps the
   phase it had, as does an ESPN id that changes mid-match.
3. No calibration without fresh first-hand state. A phase restored from disk proves the match
   started but cannot authorise a fit.
4. Only a genuine pre-match fit is persisted as this match's rates.
5. A fit that cannot reproduce its own calibration prices within `fit_tolerance` never trades.
6. An in-play fit reproduces its book by construction, so it is research-only until the score
   moves or `inplay_min_progress` minutes of clock have run. **Reproducing the snapshot is the
   success condition for the fit, not a signal.**
7. Only open play is quotable. Halftime, stoppage, finished, suspended and unknown return nothing.

### Commands run

```
cmake -S . -B build-ci -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build-ci
./build-ci/desk-tests          # 470 checks, 0 failures (was 417 before this session)
```

Fixtures captured with `curl` against `site.api.espn.com` soccer scoreboards for
fra.1 eng.1 ita.1 esp.1 ger.1 bra.1 usa.1 mex.1 por.1, then distilled to one event per distinct
status shape.

### Open

- **Not yet deployed.** Built and tested in the cloud sandbox against a byte-identical tree. The
  Mac copy has the source but has not been rebuilt, so the running desk is still the 17:17 binary
  with the defect.
- The Mac working copy is **not a git repository**. Version control exists only in the sandbox.
  For a ten-session refactor of a live trading system that should be fixed early.
- Duplicate-event handling is defensive (strict `>` on the name-match score, plus an id-change
  guard) but there is no fixture of an actual duplicate listing to test against. Capture one when
  it occurs.
- VAR reversals are covered only insofar as the score is re-read every cycle and a goal that is
  taken away moves the score back. There is no explicit correction event, because ESPN's
  scoreboard feed does not carry one; session 3's raw recording is what would let us detect it.
- The pre-2026-09-06 in-play sample contains fairs computed from contaminated rates wherever a
  restart crossed a live match. Filter by `rate_fits` provenance before drawing conclusions.
- `sports_live` (US team sports) has the same prepare-order shape and has not been audited.
- Three files exist in the Mac working copy that no build references: `tests/test_refresh.inc`,
  `tests/test_dashboard_refresh.js`, `docs/dashboard-refresh.md`. They are the weather-strip
  dashboard work from a parallel session, orphaned - not in `CMakeLists.txt`, not included by
  `tests/test_main.cpp`, not called by any script. They compile nothing and run nothing. Decide
  in a later session whether to wire the rollover test in or drop them.
- `.git` on the Mac has stuck lock files (`.git/HEAD.lock`, `.git/objects/maintenance.lock`, 87
  stray `tmp_obj_*`). The sandbox bridge cannot delete files, and git cannot commit while
  HEAD.lock exists. Remove them on the Mac (`rm -f .git/HEAD.lock .git/objects/maintenance.lock`)
  before using git there. The authoritative history lives in the sandbox clone meanwhile.
- `_to_delete/` holds the transfer patch and a rejected .gitignore hunk; safe to remove.

### Next

Session 2: market universe with verified settlement rules. Start with `KXHIGHAUS` and
`KXHIGHCHI`, plus one soccer league with reliable coverage. The first task is not code: it is
reading the actual Kalshi settlement rules for those weather series and establishing whether the
Weather Company source resolves to the NWS station the model currently uses. If it does not, the
existing weather model has been fitting the wrong target all along, and that finding outranks
everything else in the plan.

## Session 22 - the settlement gate decides the research program

`research/settlement_gate_census.py` -> `reports/settlement_gate_census.md`. Transcribes
`settlement_verified_for()` and every strategy's `covers()` out of the source and runs both against
the live universe, so "which model can pass" is measured rather than argued.

Verified transcription: **1,314 of 1,314** `trade_decisions` rows agree with the recorded
`quote_json.settlement_verified`, zero mismatches. Coverage attribution agrees on 99.6% of a
2-hour `model_fairs` window; the residual is pairs added to `poly_pairs` after the running
process's last `prepare()`.

The census, 4,583 active markets:

| model | covers | verified | live books |
|---|---|---|---|
| `crypto_lognormal` | 2,948 | 2,948 (100%) | 114 |
| `xvenue` | 778 | 0 (by design) | - |
| `sports_inplay` | 258 | 0 | - |
| `weather_nws` | 111 | 0 | - |
| `sports_live` | 103 | 103 (100%) | 91 |

Three mechanisms, each measured:

- **Weather is gone, not failing.** 111 of 111 active weather markets settle "according to The
  Weather Company"; zero name the NWS. `weather_source_supported()` rejects any TWC mention, so
  Kalshi's 2026-08-27 authority change took the whole lane out. `docs/settlement.md` has TWC
  matching the NWS CLI maximum 6/6 on one date - evidence, not the standing check the gate needs.
- **`sports_inplay` has no mapped series.** The gate wants `soccer_90`; the only mapped series
  (`KXBRASILEIROGAME`) lists no active market, and the 6 soccer series that ARE listed have no
  spec. 258 covered, 0 reachable. Unlock is reading rules, not changing a model.
- **Registry precedence costs 604 markets.** `xvenue` sits 2nd, ahead of every model that can
  pass, and takes 604 game markets that `sports_live` verifies unconditionally - 288 of them while
  producing no usable fair of its own (stale or one-sided Polymarket book).

Checked before recommending: all **707** structured markets in the `sports_live` leagues are
"<Team> wins" contracts settling on the official game result, so that model's unconditional
`settlement_verified = true` is sound on the live universe. Zero exceptions.

Error caught before it was filed: the first pass measured Polymarket freshness against wall clock
and reported **0 of 778** pairs fresh. The desk had stopped writing 13 minutes earlier; at its own
last cycle **396** pairs were fine. Every freshness test in the script now anchors to
`MAX(markets.updated_ts)`. Third time a missing observation nearly became a finding.

Program: `sports_live` first, `crypto_lognormal` second, nothing else. `sports_totals` passes the
gate unconditionally but `params.sports_totals.markets` is empty, so it covers nothing.

## Session 23 - weather lane, part 1: the settlement target

Weather only, per the WEATHER_V2 brief. Sections 3 and part of 10 are done; 4 and 7 are next.

`research/wx_settlement_watch.py` -> `data/wx_settlement.sqlite`, `reports/wx_settlement_watch.md`.
Standing collector, idempotent, reads prod.sqlite read-only and never writes to it.
`scripts/wx_watch_loop.sh` runs it hourly under the same nohup pattern as the perps recorder, and
is deliberately a separate process so the running recorder is never restarted for it.

**Gate A: 30 of 30 station-days agree.** The Weather Company's settled value falls inside the
Kalshi bracket that settled YES, on every completed station-day with both sides available, across
6 cities and 5 dates. The prior evidence was 6 agreements on one date. Still not enough to
graduate, but it is now a sample rather than an anecdote, and it accrues daily on its own.

**Finding that changes the architecture: the CLI is issued several times a day, not once.**
The NWS files a climate product in the morning, in the late afternoon, often again an hour later,
and a final one after local midnight. Each carries the daily maximum SO FAR from the station's
5-minute ASOS record. None is flagged CORRECTED - they are a running tally, not revisions, and the
first version of this collector reported "52 of 63 station-days had corrections" because it read
them as such.

Measured across 42 completed station-days:

| issuance (local) | already equals the final value | n |
|---|---|---|
| 07:00 | 0% | 6 |
| 16:00 | **86%** | 28 |
| 17:00 | 79% | 14 |
| 18:00 | 86% | 7 |
| 19:00 | 100% | 7 |
| after local midnight | 100% | 51 |

These markets trade until 01:00 local. So on roughly six days in seven the settling value is
publicly readable from about 16:00 local, eight or nine hours before the market closes, from the
same record Kalshi settles on. A running maximum cannot fall, so an afternoon issuance that is not
yet final is a hard floor and the only remaining question is how much further the day climbs.

This is strictly better than what `weather_nws` reads today. The model takes the maximum of the
HOURLY METAR series and then adds a guessed `settle_shift` to cover the peak that series missed
between reports. The CLI has no such gap. Reading it intraday replaces an estimated correction
(current support: n=6 at KNYC, n=1 everywhere else) with the settling series itself.

Two self-checks that caught errors before they were reported:
- `limit=1000` on the observations endpoint returns HTTP 400, so the first run recorded no METAR
  value at all for any station while still printing a full report. Capped at 500.
- The lock-time table joined issuances to `wx_settlement` on `(cli_loc, local_date)`, but
  KXHIGHCHI and KXLOWTCHI share MDW, so maxima were being compared against the daily MINIMUM row.
  It read 84% final for issuances after local midnight, which is impossible by construction. The
  final now comes from the issuance table itself, and that row reads 100% as it must.

Days still running are stored as provisional (`cli_complete=0`) and excluded from every agreement
and shift number, so a running maximum is never recorded as a settled value.

## Session 23, part 2: WX1 wired, and the baseline that says why it is needed

**Section 7 done.** `DayForecast` now keeps `hourly` (period start, temperature) and a
`remaining(now)` helper. `refresh_station` was building that path locally and discarding it after
taking a max. The path is assigned only on a successful parse, so a failed refresh leaves the last
good trajectory rather than blanking it. `remaining()` keeps the hour in progress (`t + 3600 >
now`), because on a hot afternoon that is usually the hour that matters.

**Sections 4 and 12 done, conservatively.** `WeatherNws::wx1_for()` simulates ONE distribution per
station-day - not per contract - and every bracket reads off it via `read_bracket()`. Rebuilt when
the forecast refreshes, an observation lands, or the hour turns. Recorded to `model_fairs` under
the strategy name `weather_wx1` and carried in `Fair::inputs` as `wx1_p` / `wx1_sigma` /
`wx1_mean_f` / `wx1_sd_f`. **The Fair the desk trades on is still WX0's.** Nothing about what is
offered or sent changes.

`st.bias_f` is deliberately not added in WX1. That constant stands in for the gap between a
sampled forecast's maximum and the continuous maximum it understates; the trajectory simulation
produces that gap directly, so adding the constant would apply one correction twice.

**Section 13 baseline, from 354 recorded observations over 31 settled event-days, 6 dates:**

| | Brier | vs market |
|---|---|---|
| raw `weather_nws` | 0.1707 | **+0.0202** |
| Kalshi mid | 0.1504 | - |
| desk fair (w=0.50) | 0.1492 | -0.0012 |

`raw - market` 95% CI clustered by event-day: [-0.0218, +0.0621]. **The raw model does not beat
the price.** The interval straddles zero, so it is not established as worse either; what is
established is that there is no evidence of an edge in the number itself.

By horizon the sign flips: T-24h +0.0361, T-12h +0.0247, T-6h +0.0321, then T-3h -0.0381,
T-1h -0.0054, T-30m -0.0478. The model is worse than the market day-ahead and better near close,
which is where its observations pin the answer. n at the short horizons is 15-27.

Section 17 sweep: w=0.00 +0.0202, w=0.25 +0.0067, w=0.50 -0.0012, w=0.75 -0.0034, w=1.00 by
definition 0. The shipped 0.50 is not the best weight on this sample, and every weight's advantage
over the market alone is inside the noise. In-sample over every day the desk has; not a split, and
not a tuned weight.

**Calibration says what is wrong, and it is the thing WX1 is for.** The raw model predicts 0.023
where 0.051 occurs, 0.074 where 0.129 occurs, 0.147 where 0.224 occurs. It is systematically too
low in the low-probability buckets, which is a distribution that is too narrow - precisely the
signature of putting a normal around max(E[hour]) instead of simulating E[max]. Pre-registered
prediction for the race: WX1 should improve the 0-20% buckets first.

Caveat that matters: 748 horizon-observations were skipped for a one-sided book against 354 kept.
If one-sided books are more common when the market is confident, the retained sample is not a
random subset. Worth measuring before any of the above is treated as settled.

Not yet done: sections 5, 6, 8 (beyond the running-max floor already in the simulator), 9, 10
(replace the guessed shift with the intraday CLI), 11, 14, 15, 16, 18-25.
