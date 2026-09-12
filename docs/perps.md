# The margined instance: perpetual futures

Kalshi runs the perps (KXBTCPERP, KXETHPERP and sixteen more, all crypto) on a second exchange
instance with its own cash, its own REST surface (`/margin/*` under the same base) and the same
API key. The event-contract desk cannot see it: `/portfolio/balance` reports the event-contract
cash only, which is why the header said $82 while Kalshi said $107 on 2026-09-08. `PerpsDesk`
(`include/desk/perps.h`, `src/perps.cpp`) is the part of the desk that reads it, trades it, moves
cash to and from it, and runs one rule on it.

## What a contract is

A contract is a fixed slice of the coin, priced in dollars: KXBTCPERP is 0.0001 BTC (about
$7.85 at $78,500), KXETHPERP 0.001 ETH, KXDOGEPERP 100 DOGE. Tick 0.0001 everywhere, whole
contracts only (`fractional_trading_enabled` is false on every market today). The exchange marks
positions to `settlement_mark_price`; `reference_price` is the underlying index scaled per
contract. P&L is continuous and is realized on every fill against the position's entry price.

Fees are basis points of notional on every fill, maker and taker; the account's rates come from
`/margin/fee_tiers` (the doc's example tier: maker 5, taker 12). Funding is exchanged every
eight hours between longs and shorts at the estimated rate (`/margin/funding_rates/estimate`;
BTC was +0.0146% per period on 2026-09-08, longs paying). The exchange can liquidate a position
whose margin is used up; `/margin/risk` publishes its estimated liquidation price per position,
and the tab shows it next to the position.

None of the binary-contract arithmetic applies: no quadratic fee, no settlement at $0/$1, no
T+2 horizon, no Kelly on p(win). Perps sit outside the event-contract risk caps for the same
reason; they have their own.

## Rails (`params.perps`)

Every hand ticket and every order the rule sends passes `PerpsDesk::rails` before the exchange's
own margin check:

| key | default | meaning |
|---|---|---|
| `enabled` | true | off = the tab shows nothing and nothing is sent |
| `max_order_notional` | $30 | price x count per order |
| `max_total_notional` | $60 | positions at the mark plus resting orders, all perps |
| `max_leverage` | 2.0 | total notional / perps equity |
| `price_band_pct` | 2.0 | a limit further than this from the mark is a fat finger |
| `refresh_seconds` | 10 | public data (every active perp, one request) |
| `account_refresh_seconds` | 30 | balance, positions, resting orders, fills, risk |
| `watch` | [] | [] = every active perp |

Plus: the kill switch (`kv kill`) refuses every ticket and the kill sweep pulls every resting
perp order; the account snapshot must be under three minutes old; the market must be open;
whole contracts where the market demands them. A reduce-only order skips the three caps (taking
risk off is never capped) but not the band or the kill switch.

Cash moves with `POST /api/perps/transfer {source, destination, dollars}` between
`event_contract` and `margined`, either direction, in centicents on the wire; asynchronous, both
balances update on the next sync. The desk's daily P&L already nets these flows out
(`get_cashflows` sees `to_margined`/`from_margined`).

## What the tab shows

Equity, settled cash, positions at the mark, unrealized, available, maintenance and resting
margin, notional and leverage against their caps; positions with entry, mark, unrealized,
liquidation price and margin, a close button (reduce-only IOC into the touch); resting orders
with cancel; a ticket with a live notional / fee / margin preview and the rail warnings before
you click; every active perp with bid/ask, spread in bps, mark, index, basis in bps, the
8-hour funding estimate, leverage, volume and open interest; fills; the rule (below). The
header carries a `perps` cell: equity, unrealized, and the rule's state.

`GET /api/perps` (everything) · `GET /api/perps/book/{t}` · `POST /api/perps/sync` ·
`POST /api/perps/order {ticker, side: bid|ask, price, count, post_only, tif, reduce_only, note}` ·
`POST /api/perps/orders/{id}/cancel` · `POST /api/perps/positions/{t}/close` ·
`POST /api/perps/transfer` · `POST /api/perps/algo {running, paper, clear_halt, settings{}}`.

Tables: `perp_markets`, `perp_quotes` (bid/ask/mark/index every 5 s on change, every 60 s
regardless: the basis history), `perp_positions`, `perp_orders` (with `origin` ticket|algo and
the note), `perp_fills`, `perp_account`, `perp_algo_trades`. Rule state in kv `perps_algo:v1`.

## The basis rule

The one systematic thing the desk runs on perps. The perp's mid drifts from its index; the rule
posts against the drift and takes the position off when it closes:

- entry: `|mid - index| / index >= max(entry_bps, maker + taker fee + 5 bps)` and under
  `stop_bps`; a stale index (over two minutes) is not a basis. Rich perp: post-only sell at the
  offer. Cheap perp: post-only buy at the bid. One position per market, `contracts` each; a
  market held by hand is left alone.
- a resting entry is pulled after `entry_ttl_seconds` unfilled and not re-posted for another TTL;
  after any close the market sits out `cooldown_minutes` (default 30). A basis already past the
  stop is never entered: that is the index and the perp parting for a reason.
- exit at the touch, reduce-only IOC: basis back inside `exit_bps`; basis through `stop_bps`
  against the position; `max_hold_minutes`; the exchange no longer shows the position
  (liquidated or closed by hand: valued from the fills, nothing sent).
- `daily_loss_stop` on the rule's own realized plus unrealized for the local day: halts new
  entries until cleared or the day turns.
- paper mode simulates the same thing: a maker entry fills when the touch trades through it,
  exits at the touch, fees at the account's rates (defaults 5/12 bps when unknown).

Settings live on the tab (`entry_bps` 25, `exit_bps` 5, `stop_bps` 60, `max_hold_minutes` 120,
`contracts` 1, `daily_loss_stop` $3, `entry_ttl_seconds` 90, `cooldown_minutes` 30, `markets`
BTC and ETH); `params.perps.algo` seeds them.

### What the sampling says

Before wiring the rule the desk sampled KXBTCPERP and KXETHPERP every ten seconds against the
exchange's own index and Coinbase spot (`research/perps_basis_2026-09-08.csv`, 44 minutes,
2026-09-08 20:20-21:04Z, normal conditions):

| | basis vs index, bps | spread, bps | share with `abs(basis) >= 10` |
|---|---|---|---|
| KXBTCPERP | mean +1.7, sd 0.4, min -0.8, max +2.7 | 0.4 | 0 of 240 |
| KXETHPERP | mean -0.7, sd 0.9, min -4.0, max +2.2 | 0.5 | 0 of 240 |

So: the perp tracks its index to within a few basis points, and a round trip costs about 17 bps
in fees at the documented retail tier. There is no fee-clearing edge in fading the basis in
normal conditions, and the rule is built so it does not try: at 25 bps it fires only in a
dislocation (a fast move where the perp lags or overshoots the index, a stale index, a squeeze),
which is when a reversion is worth something and when the stop matters. Expect it to sit idle
for hours or days. Lowering `entry_bps` below the fee floor is refused by the floor itself.

What perps are for on this desk, in order: a hedge for crypto bracket exposure (a KXBTC
bracket's delta can be covered with a fraction of a contract's worth of notional, sized by hand
from the ticket); a directional view you hold yourself, behind the rails; the basis rule as a
small, rare, bounded thing. Funding capture (long perp against spot) is not possible here, there
is no spot leg on Kalshi.

## Tests

`tests/test_perps.cpp` (144 checks): parsers against the documented shapes, the rails one by
one, the order/cancel/close/transfer bodies on the wire, fill reconciliation into order rows
without double counting, the rule in paper (entry, fill, reversion exit with the fee accounting,
stop, past-the-stop guard, loss stop and clear, TTL and the re-post backoff, stale index,
cooldown, persistence) and live (post-only entry, fill through the sync, reduce-only IOC exit,
hand position respected, TTL pull, kill switch pull, position gone, immediate maker fill,
rejection logged).
