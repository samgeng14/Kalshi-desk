# The systematic trader

The part of the desk that trades on its own once you press Start. Written 2026-09-08; the
entry side rewritten for the decision router in session 15 (2026-09-11).

Screen: the **Systematic trading** tab. Code: `include/desk/systematic.h`, `src/systematic.cpp`.
Tests: `tests/test_systematic.cpp` (`./build-ci/systematic-tests`).

## What it is

One algorithm, one switch. While it runs it does three things every engine cycle (5 s):

1. **Takes the desk's AUTO decisions.** Since session 15 it does not screen on its own: the
   decision router (`include/desk/decision_router.h`, see `docs/routing.md`) routes every priced
   side once a cycle, and the algorithm takes the rows routed `auto`, in the router's rank order:
   Kelly fraction, `(p(win) - cost - buffer * sigma) / (1 - cost)` with cost including the fee
   and the slippage estimate. That is edge per dollar of risk, and it is where "edge" and
   "probability of winning" meet. A 5c edge on a 90c contract ranks far above a 5c edge on a 30c
   contract, and a 25c edge on a coin flip does not qualify at all because the cap says so. The
   manual board shows the same rows, with the same numbers.
2. **Enters**, within its own budgets, through the same `Executor::submit` every other order on
   this desk goes through. The order cap ($10), the turnover cap ($100/day), the event, cluster
   and total exposure caps, the daily equity stop, the T+2 horizon and the kill switch all apply
   exactly as they do to a hand ticket. Nothing here bypasses them.
3. **Manages its exits**: take-profit, stop, time stop, hold-value-vs-sale, settlement. The
   engine's own exit logic (`run_exits`) leaves the algorithm's positions alone; a bracket you set
   by hand on one of them takes precedence over the algorithm.

It runs until you stop it, the kill switch trips, the exchange refuses one of its orders in a way
that says the desk is wrong, or its own daily loss stop halts new entries. Refusals are classed
(`include/desk/reject.h`, session 14): a malformed order, an auth failure or a refusal nobody
recognises halts it and trips the kill switch, the desk's standing rule. A refusal that is the
market moving does not: a post-only price the book moved through ("post only cross"), a closed or
paused market, or a short balance marks that order not sent. A post-only cross gets one fresh
book read and one re-send if the row still screens `algo` at the new time; otherwise, and for the
other soft refusals, the ticker gets a breather. It does not stop at local midnight unless
`stop_at_day_end` is on; the budget and the loss stop reset there. A restart of the desk resumes
it in the state it was in, except that a halt from a refusal that would not trip the kill today
(the post-only halts from before session 14) is cleared at startup and the algorithm is left
stopped until you press Start.

## The line between the algorithm and you

Every candidate side gets one route from the decision router, and the screen shows the reasons in
words (the Systematic tab labels them AUTO, MANUAL, weak, research, blocked).

| route | meaning | who acts |
|---|---|---|
| **auto** | passes every rule below | the algorithm, while running |
| **manual** | tradable, held back by a named reason | you, from the Trades board |
| **weak** (blocked) | data is fine, thresholds not met (probability or edge too low) | nobody, unless you disagree with the thresholds |
| **research** | a valid probability nobody should act on yet: the model or category is routed to research, the estimate is research-only, or settlement is not verified for this model | nobody; recorded |
| **blocked** | data or risk gate: stale quote or model, inputs failed their own checks, entry policy, event owned, disagreement gate, no two-sided book, kill switch, beyond 48 h | nobody |

The algorithm takes a side when, all at once:

- the **effective route** is `auto`: the lowest of the category's route (`auto`, `manual`,
  `research` or `off`, per category and for `arbitrage`), the strategy's mode in
  `config/desk.json` (a strategy in `research` or `manual` mode there is capped to it), and the
  model's own route in `model_routes`; and the model is in `graduated_models`. Defaults since
  session 15: every category `manual`, locked baskets `auto`, every model `manual` or `research`,
  nothing graduated, so no model trades unattended until you route and graduate it (the
  Systematic tab's Model routes table; graduating asks first). The old true/false shape of a
  category still reads as auto/manual (`include/desk/route.h`);
- settlement is verified for the model on this contract, the estimate is not flagged
  research-only, its source is eligible for unattended execution, and sigma is at most
  `auto_max_sigma` (0.10);
- the algorithm is armed: running, not halted, not killed, and (live) an API key is loaded.
  While it is not, its rows are offered to you as manual, with that reason;
- p(win) for that side, after the market blend, is at or above `min_win_probability` (0.55);
- the executable edge, after the entry fee, the taker's `slippage_estimate` and
  `uncertainty_buffer` x sigma, is at least `min_edge` (0.03), and the net edge is at most
  `max_edge` (0.12);
- the raw model is within `max_model_gap` (0.15) of the market mid;
- the book is fresh (inside `market_data_max_age_seconds`, the desk's one limit for opening
  risk: 3 x poll + 10 = 25 s unless set), two-sided, no wider than `max_spread` (0.06), with
  displayed size. Managing what is already on (exits, stops, a resting entry's re-validation)
  keeps the looser 60 s rule, because refusing to manage a position is a risk of its own;
- expected settlement is inside `max_horizon_hours` (48, the desk-wide T+2 limit) and the market
  does not close within `min_minutes_to_close` (10);
- the strategy's own input checks passed (forecast and observation ages, volatility warm-up);
- the entry policy allows a new entry in the event (no position, no working order, no queued
  manual request, no proposal waiting to be sent, no cooldown). The engine's model/market
  disagreement gate (0.30) is never overridden.

**Positions you opened are yours, and the algorithm never opens anything new in their event.**
That covers a ticket sent from the dashboard and an order placed in the Kalshi app: neither leaves
a `systematic_trades` row, so the account position itself is what marks the event as taken. It
matters in both directions. Adding to your side would silently raise your size on a thesis that is
not yours; buying the other side would be worse, because binary-pair netting flattens contracts you
paid for and hands back the collateral as though you had closed. `SystematicTrader::blocked_events`
builds that set once, from the algorithm's own open trades plus every event the account holds a
position in through any route, and the screen you read, the paper ledger and the live sender all
take it from there. They used to build it from three different queries, and only the live sender
included your positions, so the screen could show a green `algo` row on an event the sender would
skip. Your exits are separate and unchanged: `run_exits` touches a position only when
`model_exit:<ticker>` is explicitly set, brackets act on `manual_position_quantity` (the position
minus the algorithm's own lots), and the algorithm's exits are capped at its own lots.

**Everything else is yours.** In particular the edges above the cap: the biggest numbers on the
board are the ones most likely to be a stale input or a model mismatch rather than money, which is
why the cap exists and why they go to review instead of to the exchange.

**Locked baskets** (ladder / dominance arbitrage) are sent by the algorithm as well when the
`arbitrage` category and the `ladder_arb` model route are both `auto` (the default), every leg, out
of the same daily budget, without waiting for the AUTO switch. A basket is locked, not a
probability, so it needs no graduation.
The entry policy was changed so that the legs of one basket do not block each other; they used to,
which is one reason no basket had ever been sent.

## How it trades

- **Entry style `auto`**: lift the offer (IOC) when the edge survives the taker fee; otherwise
  post a bid one tick inside the spread (or join it), post-only, and pull it after
  `entry_ttl_seconds` if unfilled. Makers pay nothing on `quadratic` series (weather, BTC/ETH,
  MLS, WTI, NASDAQ), which is the one structural edge on this exchange that needs no forecast.
  `maker` and `taker` force one style.
- **The entry ladder** (session 13; the cure for 20 of 23 posts never filling). A post pulled
  unfilled is re-posted a tick higher, then another, up to `ladder_ticks` (3), as long as the
  executable edge at that price, from the edge engine (fees, the taker's slippage when a step
  reaches the offer, `uncertainty_buffer` x sigma), still clears `min_edge`; when the ladder
  reaches the offer and a taker still clears, it lifts. Past the top of the ladder the
  market is left alone for fifteen minutes. A bid nobody hits on a thin book is not liquidity
  provision, it is a queue the market walks past.
- **A resting entry is re-validated every cycle** at its own limit price
  (`systematic_entry_cancel_reason`): a stale quote or model, a changed model source, inputs
  that failed their own checks, a model no longer routed auto or graduated, settlement no longer
  verified, a research flag, sigma over the automatic cap, the strategy's mode capping it, a wider
  spread, a model/market gap over the limit, p(win) under the floor, or an executable edge (from
  the edge engine) that vanished at that price pulls it with the reason on the row; a partial
  fill pulls the tail and manages the position. A fill that races the cancel is counted from the
  exchange's `reduced_by`, and the recorded fills price the entry once they cover it.
- **Size**: `min(per_trade_dollars, kelly_fraction x Kelly x bankroll)`, never over the remaining
  daily budget, `max_open_dollars`, or the displayed size for a taker. Defaults: $3 a trade,
  quarter Kelly, $15 open, 5 positions, 1 entry per cycle, $30 a day.
- **Exits** (`plan_exit`), evaluated every cycle on a two-sided book for the side held:
  - take profit: the bid, net of the exit fee and of the entry cost including its fee, is
    `take_profit` over entry -> taker into the bid;
  - hold value: if the bid net of fee pays more than p(win), the market is paying more than the
    position is worth -> taker. The entry price plays no part in this rule;
  - thesis stop: the model's own p(win) has fallen `stop_loss` under entry -> taker when the bid
    is deep (displayed size covers the position) and not a hole, else a resting sell;
  - market stop: measured on the **mid**, never inside `stop_grace_seconds` (90) after the fill
    (our own fill is what emptied the bid), and only once it has persisted `stop_persist_seconds`
    (30). It goes into the bid when the bid is real and within 3c of the stop, as a resting sell
    otherwise; twice the stop on the mid goes into whatever bid there is;
  - time stop (`max_hold_minutes`; 0 = hold to settlement) at the best available price.
  A resting exit sits one tick inside the offer (or joins a one-tick offer), is pulled after
  `exit_ttl_seconds` (240) and re-posted a tick lower (`exit_steps`) down to a tick above the bid,
  after which the ladder ends in the bid. Taker exits are reduce-only IOC into the bid, sized to
  the displayed depth; partial fills are recorded and the rest re-planned next cycle. The
  recorded fills price the exit once they cover it.
- **Cooldown**: after an exit the event is left alone for `cooldown_minutes`.
- **Execution stats** (Systematic tab): fill rate, filled / pulled / resting counts and every
  pull reason, by mode.
- **Settings from the config file**: an optional `"systematic": {...}` block in
  `config/desk.json` is applied over the saved settings at every start, through the same
  validation the screen uses, and the change is named in the log. Keys it does not mention keep
  whatever was saved; a block that fails validation is logged and ignored rather than reverting
  anything. It exists so a preference you mean to keep does not live only in a database blob that
  the next dashboard save overwrites. As of 2026-09-10 it carries
  `daily_loss_stop_enabled: false`, `stop_loss_mode: "fraction"` and `stop_loss_fraction: 0.70`.
- **Daily loss stop**: realized on trades closed today plus the mark on open trades. At
  `-daily_loss_stop_dollars` no new entries go until the day rolls or you clear the halt; clearing
  it re-arms the stop from today's P&L as it stands, so the same loss does not trip it again.

## Paper mode

Same screen, same exit rules, simulated fills: taker at the ask plus `paper_slippage`, exits at
the bid minus slippage, fees on both, no fill without displayed depth. Nothing is sent. The
performance strip shows the mode that is active; both ledgers are kept in `systematic_trades`.

## Bookkeeping

Table `systematic_trades`: one row per trade with state
`entering | open | exiting | settling | closed | canceled`, entry and exit prices, fees, realized
and unrealized P&L, the p(win), edge and Kelly it was taken on, and the settings version. Entries
are ordinary proposals (`strategy = systematic`) carrying the router's `decision_id`, exits are
`sys_exit`, so the blotter, the P&L-by-strategy table and the gate report see them like everything
else. Each entry the lane takes, live or paper, is recorded in `trade_decisions` as `auto` against
its decision, a refusal as `rejected`, and the executor adds `submitted` when the order goes.
Settings and run state live in `kv` under `systematic:v2`.

## What this does not claim

The estimates behind the candidates are the desk's own models. Their standing against the market
on settled markets is on the Strategies tab and in `docs/platform.md`, and it is not flattering:
this desk's studies found the market beat every one of them. The cap, the probability floor, the
market blend, the gap limit and the size limits are there because of that. They bound what a
wrong estimate can cost; they do not make it right. Treat the first days as the sample they are.
