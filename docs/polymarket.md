# Polymarket US: the second venue, the cross-venue fair, and the pair rule

Polymarket US (`api.polymarket.us` for trading, `gateway.polymarket.us` for public data) is the
CFTC-regulated exchange that lists the same binary game contracts Kalshi does, with its own fee
schedule, its own tick and its own cash. `PolyDesk` (`include/desk/poly.h`, `src/poly.cpp`) is
the part of the desk that reads it, trades it by hand behind rails, turns its prices into a fair
for the Kalshi markets it also lists (the `xvenue` strategy), and runs a two-leg spread rule.
The displayed spread is a conditional estimate after fees. Both fills and equivalent settlement
outcomes are required; it is not guaranteed profit or a validated statistical-arbitrage model.

## What overlaps

The desk fetched the whole open universe on 2026-09-08: 60,500 markets. There is no weather and
no short-dated crypto there, so the only contracts both venues list are sports game winners:
NFL (`aec-nfl-away-home-yyyy-mm-dd`, one market a game, the long side is the first team), college
football (`aec-cfb-...`), MLB (`aec-mlb-...`), and the NBA, NHL and WNBA in season, plus soccer
as one market per team (`atc-epl-bor-bre-date-bor`, whose draw has no Kalshi counterpart the desk
pairs). On the NFL snapshot 30 of 32 games matched by team code and prices sat within about a
cent of Kalshi's.

The matcher uses teams, dates and league metadata; that does not establish equivalent settlement
rules. Each contract's overtime, tie, postponement, cancellation and dispute provisions must be
checked independently. A suspended game or a venue voiding a market can leave one leg standing.
The current implementation does not certify rule equivalence automatically.

## Fees, ticks, contracts

| | Kalshi | Polymarket US |
|---|---|---|
| taker | 0.07·p(1−p) (quadratic, ceil to a centicent) | 0.06·C·p(1−p), C = 0.06 on game markets |
| maker | 0.0175·p(1−p) on `_with_maker_fees` series | **paid** 0.0125·p(1−p) |
| tick | 0.01 | 0.005 on game markets |
| short | buy NO | SELL YES; margin = $1 − price |

`poly_fee(p, taker)` is the schedule; a negative fee is the maker rebate. All arithmetic in the
desk is in Kalshi-YES terms for the Kalshi team: when that team is Polymarket's NO side the
Polymarket book is flipped (`1 − ask`, `1 − bid`) before anything is compared.

## Wire

Ed25519 over `timestamp_ms + METHOD + path` (path without the query), base64, in
`X-PM-Access-Key`, `X-PM-Timestamp`, `X-PM-Signature`. The secret shown once at key creation is
base64 of the 32-byte seed or of seed + public key (64 bytes); `PolyAuth` takes either and never
logs or stores it. The desk reads it from `params.polymarket.secret_path` (a file holding the
base64 string, `chmod 600`, `~` expands). A missing or unreadable file gives a public-only
client and a warning: the tab and the pairs still work, nothing can be sent. Rate limit 20 rps;
the client spaces calls to that.

Endpoints used: `GET /v2/leagues/{slug}/events` (universe), `GET /v1/markets?slug=a&slug=b`
(quotes, batched), `GET /v1/markets/{slug}/book`, `POST /v1/orders`, `GET /v1/orders/open`,
`POST /v1/order/{id}/cancel {marketSlug}`, `POST /v1/orders/open/cancel`,
`POST /v1/order/preview`, `GET /v1/portfolio/positions`, `GET /v1/portfolio/activities`,
`GET /v1/account/balances`.

## Rails (`params.polymarket`)

| key | default | meaning |
|---|---|---|
| `enabled` | on when `key_id` + `secret_path` are set, or explicitly | off = nothing fetched, nothing shown |
| `key_id`, `secret_path` | | the key id from polymarket.us/developer; the file holding the secret |
| `leagues` | nfl, cfb, mlb, nba, nhl, wnba, epl, lal, sea, bun, lg1, mls, ucl | league event lists to fetch |
| `max_order_dollars` | $25 | cost (or short margin) per order |
| `max_total_dollars` | $100 | positions plus resting orders |
| `price_band` | 0.10 | a limit further than this from the mid is a fat finger |
| `refresh_seconds` | 10 | quotes for matched and held markets |
| `universe_refresh_seconds` | 600 | league events |
| `account_refresh_seconds` | 30 | balance, positions, open orders, activity |

Plus the kill switch (every ticket refused, every resting Polymarket order pulled by the kill
sweep), a synced account under three minutes old, whole contracts, the market's tick, the market
open. `POST /api/poly/preview` runs the exchange's own preview before a hand ticket.

## Matching

`PolyDesk::match` pairs each Kalshi game event with one Polymarket market: same league, same
Eastern game date (the Kalshi ticker's date and, when the ticker carries one, its HHMM start,
within three hours of Polymarket's `gameStartTime`; daily leagues need the exact date because the
same two teams play on consecutive days), and each Kalshi team scored against each Polymarket
side (code match 3, name or alias 2, word overlap 1; both teams must score, total at least 4).
One Polymarket market belongs to one Kalshi event; when two claim it the closer start keeps it.
`poly_pairs` in the store holds the result; `pairs_by_ticker()` is the live map.

## The xvenue fair

`src/strategies/xvenue.cpp`: for a Kalshi game market with a fresh, two-sided Polymarket book
(under 90 s old), fair p = the Polymarket mid in Kalshi-YES terms, sigma = the half spread plus
half a cent (floor 0.01), `market_blend_weight` 0 (this is a market, not a model; it is not
shrunk toward the Kalshi mid). It sits after `manual_fair` in the registry, so a hand fair still
wins. Its rows show on the Trades board with the `polymarket` label and go through the same
screen and entry policy as any other estimate. Mode in `strategies.xvenue`, default `manual`.

## The lock arithmetic (`lock_math`)

For a pair with Kalshi book `kbid/kask` and Polymarket book `pbid/pask` (flipped when needed):

- Route A, buy the team on Kalshi and sell it on Polymarket: gross = pm price − kalshi price;
  capital = kalshi price + (1 − pm price), the second term being the margin a Polymarket short
  posts.
- Route B, sell the team on Kalshi (buy NO) and buy it on Polymarket: gross = 1 − NO price − pm
  price; capital = NO price + pm price.

Each route is priced four ways (Kalshi maker or taker, Polymarket maker or taker; a maker price
is one tick inside the local bid, or the bid itself when the spread is one tick). Profit is gross
less both fees. Eight rows in `routes`; `best` is the executable one: the Kalshi leg always goes
first and the Polymarket leg is the hedge, so only routes whose hedge crosses (Polymarket taker)
are locks. Taker/taker is `instant` (both legs cross now, the only free money); maker/taker rests
the Kalshi leg and hits Polymarket when it fills. A resting Polymarket leg would leave the Kalshi
fill naked, and two resting legs on a wide book are just the spread, so those rows are shown but
never `best`. `tight` = both spreads within the rule's `max_spread`; `qualifies` = tight, fresh,
and `best` clears `min_lock` and `min_return`. The Trades board lists only qualifying pairs, one
a game (the two team markets are the same trade from either side), ranked with everything else.

What the snapshot showed: NFL books sit within a cent of each other, so an instant lock is rare;
maker/taker locks of one to two cents appear on the smaller college and MLB games and on the
0.02/0.98 tails. Returns to settlement are single digits of a percent over days. This is a
capital-light, low-variance sleeve, not a money machine, and the board says so with its numbers.

## The pair rule (`PolyDesk::step`, the Polymarket tab)

Settings, all editable on the tab, persisted under `poly_algo:v1`:

| key | default | meaning |
|---|---|---|
| `min_lock` | 0.01 | executable lock per contract after both fees |
| `min_return` | 0.01 | lock over the capital both legs tie up |
| `max_pair_dollars` | 20 | capital per pair |
| `max_total_dollars` | 100 | capital across open pairs |
| `max_pairs` | 5 | open pairs at once |
| `contracts` | 5 | per pair, cut down by the caps and by displayed size for a taker leg |
| `kalshi_style` / `pm_style` | maker / taker | how each leg goes |
| `kalshi_ttl_seconds` | 240 | a resting Kalshi leg is pulled after this |
| `hedge_ttl_seconds` | 120 | the Polymarket leg's allowance before the pair is left unhedged |
| `daily_loss_stop` | 5 | realized on settled pairs today |
| `min_hours_to_start` | 0.5 | skip games starting sooner (in-play books move too fast to hedge) |
| `max_spread` | 0.06 | both books must be no wider than this |

States in `xvenue_trades`: `kalshi_working` (leg one resting, or filled at once) →
`hedging` (leg two sent at the live touch; an existing hedge order must be reconciled before a replacement; if the book moved more than
2 cents against the lock it waits inside the TTL, more than 5 cents it stops and marks the pair
`unhedged`) → `locked`. Paper mode simulates both payouts from the Kalshi result and labels the
result as simulated. Live pairs with a filled Polymarket leg remain open with an explicit
"awaiting independent Polymarket settlement" reason when only Kalshi is final. Automatic
verification of the second venue's settlement remains unfinished, so this projected payout is
not booked as realized P&L. A pulled or refused Kalshi leg puts the ticker on a five-minute backoff. One pair
per Kalshi event; the event is `busy` on the board while a pair is open. Paper mode fills a
maker leg when the touch trades through it and the hedge at the touch, with the same caveat as
the systematic trader's paper fills: no queue model. Live mode needs the Polymarket key and the
Kalshi executor, and the kill switch stops new pairs and new hedges.

Automatic Polymarket legs refuse to reduce opposite inventory already in the account and do not
cancel unrelated working orders. Their own cumulative order fills determine hedge quantity;
external purchases never count as fills of the Kalshi leg. Unknown hedge outcomes block retries.
Manual single-market orders remain manual. A manual pair submission explicitly delegates the
second leg of that pair. The pair strategy has its own budget: the Systematic tab's $40
allocation does not cap it or the separate perpetuals strategy.

A hand pair (`POST /api/poly/pair`, the modal on the board or the `pair` button on the tab) is the
same machinery with the route and styles chosen by hand: one row in `xvenue_trades`, managed by
the rule's step from then on.

## P&L

`PolyDesk::pnl()` is the perps shape: today = equity − the first snapshot of the local day − cash
moved in today; total = equity − net cash moved in; `poly_flows` records deposits and withdrawals
from the activity feed. The header sums it with the event contracts and the perps, and shows the
three apart.

## Endpoints

`GET /api/poly` · `GET /api/poly/pairs` · `GET /api/poly/book/{slug}` · `POST /api/poly/sync` ·
`POST /api/poly/preview` · `POST /api/poly/order {slug, action, price, quantity, post_only, tif, note}` ·
`POST /api/poly/orders/{id}/cancel {slug}` · `POST /api/poly/positions/{slug}/close` ·
`POST /api/poly/algo {running, paper, clear_halt, settings}` ·
`POST /api/poly/pair {kalshi_ticker, route, contracts, kalshi_style, pm_style}`

## Tests

`tests/test_poly.cpp` (121 checks) through a mock Polymarket that verifies the Ed25519 signature
on every authenticated call and a mock Kalshi: auth round trip, parsers, fees, league and date
parsing, team scoring, matching (four pairs, the EPL draw and an unlisted game excluded; a daily
league with a doubleheader and a game only one venue lists), quotes flipped for the NO side, the
lock arithmetic, account sync and rails, order body, cancel, close, cancel-all, preview, book, the
paper pair rule end to end (open, fill on trade-through, hedge, lock, settle both ways,
persistence) and the live pair rule (Kalshi maker leg posted, filled, Polymarket IOC hedge at
the bid, locked; a second leg pulled after its TTL).

## What this does not claim

No pair has been sent live from this code yet. The lock numbers are arithmetic on displayed
touches; the fill of a resting leg is a queue question the desk does not model, and the hedge
after a fill is a race the `hedge_ttl_seconds` window bounds but does not remove. The spread gate
keeps the rule off books where the "lock" is the spread, which is where most of the large
numbers were. Run it in paper first and read the open-pairs table and the log before going live.
