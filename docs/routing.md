# Which trades the desk sends, and which come to you

*This is the rule as implemented (session 15, the canonical probability pipeline).
`docs/strategy-routing.md` is the September 9 research note that proposed an earlier version, and is
a plan, not a description of the code.*

One path. Every contract the desk prices goes through the same four steps, and the manual board
and the automatic lane read the same result:

```
DATA -> PROBABILITY MODEL -> EDGE ENGINE -> DECISION ROUTER -> MANUAL board | AUTO lane | RESEARCH record
                                                            -> shared risk checks -> Executor::submit
```

- **The model** (a strategy) produces its estimate. The engine adapts it, unchanged, into a
  `ProbabilityQuote` (`include/desk/probability.h`): `p_yes`, `sigma`, model name and version,
  the model's own and its source data's timestamps, whether its inputs passed their checks,
  whether settlement is verified for it, whether its source may be executed unattended, and its
  research flag. A quote is a model output only: no size, no style, no route, no price.
- **The edge engine** (`include/desk/edge.h`) prices the quote against the book, for YES and for
  NO. `raw edge = fair - price`; `executable edge = fair - price - fee - slippage - buffer x sigma`.
  A taker is priced at the side's actual ask, never the mid; a maker at the desk's maker price
  (one tick inside a spread of two ticks or more, else joining the bid, never at the offer). All
  YES/NO price arithmetic goes through `normalize_prices()`: NO ask = 1 - YES bid, NO bid =
  1 - YES ask, and the sizes follow.
- **The decision router** (`include/desk/decision_router.h`) owns eligibility. A model never
  decides whether it trades automatically.

## Four routes, one each

| route | meaning | who acts |
|---|---|---|
| `blocked` | a data or risk gate failed, or the thresholds are not met (shown as `weak`) | nobody |
| `research` | a valid probability nobody should act on yet | nobody; it is recorded |
| `manual` | tradable and worth a look, held back from unattended execution by a named reason | you, from the Trades board |
| `auto` | everything passes, the model is routed auto **and** graduated, the algorithm is armed | the algorithm |

Precedence is literal: blocked, then research, then weak, then manual, then auto. Every candidate
gets exactly one, and the reasons that shaped it are on the row in words.

**Blocked**: market not open or its close has passed; quote or model estimate older than
`market_data_max_age_seconds` (3 x poll + 10 = 25 s unless set); probability or sigma missing or
invalid; no two-sided book; fee schedule unknown; settlement time unknown, passed, or beyond the
desk's 48 h entry horizon; closing inside the near-close floor (below); the model's inputs failed their
own checks; the entry policy refuses; the event is owned (a position, a working order, a queued
manual request, a proposal waiting to be sent, or one of the algorithm's trades) or cooling off;
model/market disagreement past the desk's hard gate (0.30, also when a model flags it itself); the
kill switch; a taker with no displayed size. **Weak** (blocked, valid data): p(win) under
`min_win_probability`, executable edge under `min_edge`, a manual row under `manual_min_edge`, an
addition under its required edge.

**Research**: the effective route is `research`; the model flagged the estimate research-only; or
settlement is not verified for this model on this contract. (`allow_research_estimates` no longer
exists as a switch: a research-only estimate is never offered.)

**Manual**, any of: the effective route is `manual`; the model is routed auto but not graduated;
its source is approved for manual research only; sigma above `auto_max_sigma` (0.10); the raw
model further than `max_model_gap` from the mid; net edge above `max_edge` (an estimate that far
from a live market is usually the estimate that is wrong); a spread wider than `max_spread`;
settlement beyond the algorithm's `max_horizon_hours` but inside the desk's 48 h; the entry policy
asks for review; a qualified same-side addition to a position you hold yourself; the algorithm not
armed (stopped, halted, or live without an API key). A manual row must still clear
`manual_min_edge`, or it is weak and goes to nobody.

**Auto**: none of the above.

## Who sets the route

```
effective route = the lowest of   category route      (Systematic tab: weather, sports, crypto, ...)
                                   strategy mode       (config/desk.json or the Strategies tab: off < research < manual)
                                   model route         (model_routes, by name_version, else name)
order: off < research < manual < auto
```

A category on `auto` is the outer permission and never promotes a model that is not: `crypto:
auto` with `crypto_lognormal_v1: manual` is manual. A model with no route of its own is research
until it is given one. AUTO also needs the model in `graduated_models`, and nothing is graduated by
default; graduating one (the Systematic tab, Model routes) asks first.

Defaults: every category `manual` (weather, sports and crypto included), locked baskets
(`arbitrage`) `auto`. Model routes: `weather_nws_v1`, `crypto_lognormal_v1`,
`sports_live_v1`, `xvenue_v1`, `manual_fair_v1` and the four `commodity_terminal_*_v1` manual;
`sports_inplay_v1` and `sports_totals_v1` research; `ladder_arb` auto (a locked basket is sent when
both its category and this route are auto; it needs no graduation, it is not a probability). A
one-time migration (`kv systematic:route_migration = s15`) moved saved weather, sports and crypto
routes from auto to manual; probability generation and book capture continue on every route but
`off`.

Settlement verified (`settlement_verified_for`): `weather_nws` when the contract names the NWS and
its series has a mapped station product; `sports_inplay` for 90-minute soccer result markets;
`sports_totals`, `sports_live` and `manual_fair` yes; `crypto_lognormal` when the rules name the CF
Benchmarks real-time index; `xvenue` and anything else no (research). The four
`commodity_terminal_*` models answer for themselves (`Strategy::settlement_verified`) and answer
empirically: the contract's rules must name their feed **and** their own recording of that feed
must have reproduced the exchange's published strike within the last two hours. Until it does they
are research, because a probability computed on a feed that is not the settlement feed is a
confident statement about the wrong number. See `docs/commodity-settlement.md`.

## The near-close floor

`min_minutes_to_close` (10) is the flat rule: do not open something that stops trading in a
moment. On a fifteen-minute contract that rule is not a floor but an off switch — the whole
contract is gone in fifteen minutes, so a ten-minute floor blocks two thirds of its life and the
part anyone would want to trade. So the floor a row is actually judged against is

```
min(min_minutes_to_close, min_life_fraction_to_close x the contract's own trading life)
```

with `min_life_fraction_to_close` at 0.2: three minutes of a fifteen-minute window, and still the
flat ten minutes for anything whose life is measured in hours (a fifth of 48 h is far above 10 min,
so the smaller of the two is the flat floor and nothing about long-dated markets changed). The
contract's life is `close_ts - open_ts` from its own market row; a row with no open time gets the
flat floor. The same number is used by the router and by the resting-entry check, through
`min_minutes_to_close_for` in `decision_router.h`.

## Two floors, one number

- `min_edge` (0.03): the algorithm's.
- `manual_min_edge` (0.05): yours. It may not be set below `min_edge`.

Both are measured on the executable edge: after the entry fee, the slippage estimate
(`slippage_estimate`, a taker's, default 0) and `uncertainty_buffer` sigmas.

## The manual lane

The Trades board reads `GET /api/decisions`: the cycle's MANUAL and AUTO rows ranked by Kelly (edge
per dollar of risk), one row per event and route (`event_rows` says how many the event had),
RESEARCH behind `?research=1` (only those with a positive executable edge). The page never
recomputes a route, an edge or ownership. AUTO rows are shown and marked and are not offered;
double-clicking a MANUAL row opens the order dialog, whose numbers come from the desk
(`GET /api/size?decision_id=...`: the decision priced at the dialog's price and style by the same
edge engine). The order names its `decision_id`. At the click the desk refuses a decision that is
no longer on the board, not MANUAL, or under the manual floor at your price. Just before it sends,
it reads the book from the exchange again, re-runs the router and the edge engine, and refuses with
what changed ("nothing sent") if the decision is no longer MANUAL, the model changed, or the edge at
your price is under the floor. An order on a model that has not yet beaten the price on settled
markets is also capped at `risk.uncalibrated_max_order_frac` of the bankroll (the cap the old
proposal lane put on every model signal it sized); a hand ticket, which is your own judgement and
not a model's, is not. Risk and the executor's own checks follow as for every order.

## The automatic lane

`SystematicTrader` takes AUTO rows from the same screen, in rank order, and does not rebuild fair,
edge or route. It keeps its entry style, the maker ladder (each step priced by the edge engine,
including the taker's slippage when a step reaches the offer), the entry TTL, sizing and exits. A
resting entry is re-validated every cycle and pulled when the decision that sent it would no longer
be AUTO: model route, graduation, settlement, research flag, sigma, the strategy's mode.

## The record

`trade_decisions`, one row per thing that happened to a decision: `offered` (routed MANUAL or AUTO
this cycle; re-recorded when the route, price or edge moves, else every 5 min), `research` (every
15 min), `approved` (you clicked it), `revalidated` (re-priced just before sending), `auto` (the
algorithm took it, live or paper), `rejected` (a gate, the risk checks or the exchange refused it),
`submitted` (an order went). Each row carries the quote, the book and the edge breakdown. Proposals
carry the decision id and orders inherit it: `GET /api/decisions/trace/<order_id>`.

## Positions you opened yourself

Untouched, by every route. The algorithm manages only the entries it made: a position or working
order held through any other path takes its event off the board, and your own take-profit and
stop-loss brackets are what exit it. See `SystematicTrader::blocked_events()`.

## The three lanes

The Book panel's switches are independent, and each is its own `POST /api/book {running, lane}`:

| lane | what it trades |
|---|---|
| `systematic` | the event contracts: weather, crypto, sports, locked baskets |
| `pairs` | the same game on Kalshi and Polymarket |
| `perps` | the margined instance: basis against the index |

Turning the master switch on starts all three; a refusal from one (no Polymarket key, say) never
stops the others. Lane 2 ("yours") counts this cycle's MANUAL decisions.
