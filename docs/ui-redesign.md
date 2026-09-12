# Desk UI: what changed, and why each change was needed

The goal, in one line: **within five seconds you should know what is tradable, what is automatic,
what needs you, and what is blocking everything else.** The backend already answered all four. The
screen did not ask it.

This was done directly in `ui/index.html` rather than as a throwaway prototype, and verified in a
real headless browser against a stub API at 1440x900 and 1920x1080.

## The problem, stated precisely

The board said `nothing actionable right now`. That sentence is true of a working router almost all
the time, and it is also what a broken one says. There was no way to tell them apart, which is
exactly the question that came up in practice: *why are there no manual trades?* The answer that
day was that a category chip had been clicked once and persisted in localStorage, so the board was
filtering to a category that had nothing in it. The screen had the information to say so and did
not.

## What the screen now does

**Four lanes, FOR ME first.** `FOR ME / AUTO / ALL / RESEARCH`, with live counts on each chip. FOR
ME is the default, because the first question on opening the desk is what you can act on. The
counts describe what is AVAILABLE, not what the current tab admits - an early version derived the
lane switches from the active tab, which made AUTO read 0 while two auto rows existed.

**The decision funnel.** `MARKETS 812 -> PRICED 146 -> EDGE 11 -> ACTIONABLE 5 -> AUTO 2 MANUAL 3`.
Every step is clickable and lands on the lane that explains it. This is what makes an idle board
visibly the output of a working system rather than an absence of one.

**An empty board that accounts for itself.** Instead of one sentence:

    NO MANUAL TRADES RIGHT NOW
    11 decisions evaluated this cycle
      3  edge below the floor
      4  research only
      4  blocked by a data or risk gate
    Top reasons nothing cleared:
      3  net edge below the manual floor of 4c
      4  model is research-only until it graduates
      2  sports source is stale
      1  no executable offer on this side
      1  settlement feed not verified against a published strike
    [ SHOW ALL DECISIONS ]  [ SHOW RESEARCH ]

Every number comes from the server. Nothing is recomputed in the browser, because a second opinion
computed in JS is how a screen starts disagreeing with the engine it is meant to be reporting on.

**Stale is not the same as empty.** If the decision snapshot is older than the cycle window the
board says `DECISIONS ARE STALE` and gives the last good update, rather than showing an empty list
that looks like a considered answer.

**Halted can no longer read as running.** `screen["algorithm"]` carried only the operator's switch,
so a halted engine reported `algorithm LIVE`. It now publishes `requested_running`, `halted`,
`halt_reason`, `halt_class` and `actually_running`, the board header tests halted first, and three
pills that said `RUNNING · HALTED` now say `HALTED`. The reason gets its own line: *"The algorithm
is HALTED and is sending nothing: post-only order rejected by the exchange. Clear the halt on the
Systematic tab. Manual orders are unaffected."* - including what the halt does NOT stop.

**Data health in words.** `spt 49m` is a value the operator has to decode before acting on it.
`SPORTS STALE 49m` is the finding. The word STALE appears only past the bad threshold, so a healthy
row stays short and a broken one is unmissable.

**The board shows the operator's number only.** `σ` and `raw edge` are gone from the default
columns. They are decomposition, and the place that question gets asked - why did an 8.8c
discrepancy become a 6.2c trade - is the ticket, which still itemises fair, entry, fee, slippage,
uncertainty and executable edge in one table.

**Changing lane clears the category narrowing.** A category picked while looking at AUTO is not a
statement about what you want under FOR ME, and carrying it over is how the board ends up empty for
a reason the operator did not choose.

**The ticket's side logic.** The decision has a side; that side is the primary action and is
labelled as the one the desk priced and will re-check. The other side is offered as a plainly
secondary control that opens a **fresh hand ticket** through the existing trade-strip path. It is
deliberately not a flip of the current ticket: flipping in place would keep the `decision_id` and
send a decision-backed order for a side the desk never priced.

**Two standing explanations folded away.** They did not change minute to minute and were costing a
third of the board's height at 900px. They are one click away behind `? what the routes mean`.

## Backend changes, all read-only

`decisions_view` gained `funnel` and `reasons`, both computed from the UNFILTERED screen - the whole
point is to account for the rows the board does not show. `screen["algorithm"]` gained the halt
fields above. No model, threshold, route, order or graduation logic was touched.

## Verified

- `ui-routes` 63 checks, `ui-desk-ux` 43 checks (new), both 0 failures.
- `engine.cpp` clean under `-Wall -Wextra`.
- Headless Chromium against a stub API, three scenarios (3 manual + 2 auto, empty FOR ME, halted
  engine), both resolutions, **0 console errors and 0 page errors** in all three.

Two bugs were found by the browser pass that the static tests could not see: `routeCounts` was
returned by the model but not destructured by the renderer, which blanked the whole panel, and the
lane counts were being computed after the rows had already been excluded. Both are why this was
built and run rather than reasoned about.

## The selected-row panel (second tranche)

The right-hand column now leads with **SELECTED**, above the ladder. One panel answers the five
questions a row raises, for whatever is selected - by mouse or by `j`/`k`, and the selection
survives a repaint rather than being destroyed by `innerHTML`. With rows on the board it defaults
to the top one, so the panel is useful on arrival without a click.

    SELECTED                          KXHIGHCHI-26SEP12-B79.5
    Chicago high temperature · 79° to 80°
    weather · settles 3.2h · quote 3s · model 4s
    book        0.40 / 0.42 (120 × 90) spread 2.0¢
    model       weather_nws_v2
    settlement  verified

    model fair                                   51.0¢
    entry taker at                              −42.0¢
    raw discrepancy                              +8.9¢
    fee                                          −1.1¢
    slippage estimate                            −0.5¢
    uncertainty (buffer × σ 0.020)               −1.1¢
    NET EDGE                                     +6.2¢

    MANUAL  yours to review. The desk re-checks the decision on a fresh book
            before anything is sent.
    Why it is on this route:
      · <the row's own reason stack>

    [ BUY YES ]   take NO instead     Enter opens the ticket

**The waterfall checks itself.** It reconstructs the row's net edge from the row's own terms, and
if `raw − fee − slippage − uncertainty` does not equal the published `executable_edge` it prints an
`unexplained` line rather than a tidy column that happens to be wrong. This was verified in both
directions: an inconsistent fixture produced `unexplained +0.1¢`, and making the fixture add up made
the line disappear. A screen that silently rounds away a disagreement teaches you to trust a number
it never actually checked.

**Button logic.** `tradable` is the route, never a guess: `r.kind !== 'kalshi' || route === 'manual'`.
The primary button is disabled on anything not yours, and a disabled button says why, per route -
AUTO says the algorithm sends it, RESEARCH says it is offered to nobody, BLOCKED names the gate.
The secondary control opens a **fresh hand ticket** on the other side through the existing
trade-strip path, for the same reason as in the modal: a flip in place would keep the `decision_id`.

**The why-not inspector, per row.** The reason stack that the empty board shows in aggregate is now
on every selected row, from the row's own `reasons` and `warnings`.

**Ownership is scannable.** The always-visible positions table leads with an owner badge -
`ME` / `AUTO` / `MIXED` - derived from the manager the server reports and deliberately NOT from the
algo/manual size split, because a position can carry algo size and still be managed by hand, and
guessing from the split would mislabel exactly those.

## Manual sends queue instead of being refused

Submitting while an order is awaiting confirmation used to toast *"an order request is still
pending"* and drop the send. It now joins a queue and goes when the one in front of it has been
answered. This is a change to the order path, not presentation, so the properties that keep it
safe are worth stating.

**Each queued item goes out through the ordinary send path.** Nothing about being queued bypasses
a check an interactive send passes: the `/api/size` cap re-check still runs at departure, and the
desk still re-checks the decision against a fresh book and refuses it if anything moved.

**The review timestamp is yours, not the departure time.** `reviewed_ts` is stamped when you
pressed send. The desk expires an unsent review after 90 seconds, and stamping at departure would
have hidden the wait from the one rule that exists to catch exactly this.

**One entry decision per event.** A second send on an event already queued is refused as a
double-press, which is what it almost always is. Verified: pressing the same row three times
queues it once.

**The drain stops on the first failure.** A kill switch or a rejected review would reject every
remaining item in turn, so the rest stay queued and unsent with the reason shown, plus explicit
`resume queue` and `discard queue` controls. Only a clean result advances the queue.

**Depth is capped at five**, and the queue is visible and cancellable per item - a queue the
operator cannot see is a queue they cannot cancel. Cancel applies only to items not yet sent;
anything already awaiting confirmation belongs to the desk and closing a dialog never recalled it.

**The dialog now closes when the request goes in flight.** It used to stay open showing
"Submitting…", which left a modal overlay between the operator and the board - so the next row
could not be reached and nothing could be queued behind anything. Everything the open dialog was
still showing now lives in the banner.

**The submit lock is per dialog.** `submittingThis()` compares the open dialog against the one
actually in flight, so building the next ticket while the first is pending no longer arrives with
its own controls disabled. The send button relabels itself `queue …` in that state, and says so.

Two bugs found while verifying this in the browser, neither visible to a static test:
`addEventListener('click', sendKalshiModal)` handed the new optional first parameter a MouseEvent,
so every send silently returned at `st.kind !== 'kalshi'`; and the ladder read `L.book.yes_bids`
without a guard while every neighbouring field had one, so a partial ladder response threw and
blanked the panel.

## Not done in this session

Still open, and none of it is load-bearing for the operator questions this set out to answer:
the bottom strip as POSITIONS / ORDERS / FILLS / ACTIVITY *tabs* (the tables are all present and
the ownership column is in, but they are stacked rather than tabbed), the consolidated
`/api/desk-view` snapshot with one timer instead of several, and the model-lab verdict screen that
keeps rejected strategies visible so they are not accidentally rebuilt.

Density (compact / normal / roomy, remembered locally) and the keyboard map already existed and
were left alone.
