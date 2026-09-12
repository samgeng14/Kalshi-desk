# The desk, defined

What happens when you start it, which lane a trade goes down, and what each sleeve is allowed to
touch. Written 2026-09-08.

## Starting it

`./scripts/tradeit up` starts one process that owns everything. The browser is a viewer: closing
Safari stops nothing, and the dashboard reaching the desk is not what makes the desk run.

Each cycle, in order: refresh series and markets, refresh the contract universe, snapshot the
order book under every live decision, reconcile against Kalshi, run the strategies, run brackets,
run the auto-hedger, run exits, then send whatever qualifies.

## The three lanes a trade can go down

Every proposal ends up in exactly one of these. This is the part worth memorising.

| lane | what sends it | you see it |
|---|---|---|
| **instant** | net edge >= `instant_edge` | after the fact, in the blotter |
| **auto rule** | strategy in `auto`/`bot` mode AND the rule passes | after the fact |
| **manual queue** | everything else | in Signals, waiting for your click |

`AUTO` armed is required for the first two. The kill switch, the daily stop, every cap, the
per-order and turnover limits and the calibration gate apply to all three without exception.

### instant_edge, and why 0.70 fires on nothing

`instant_edge` is **dollars of net edge per contract**, not a probability. Measured across 1,605
model proposals on 2026-09-06:

| threshold | proposals that would have fired |
|---|---|
| $0.10 | 63 |
| $0.15 | 9 |
| $0.20 | 4 |
| $0.30 | 2 |
| **$0.50 and above** | **0** |

The largest single edge the desk has ever produced is **$0.345**. A threshold of 0.70 means the
lane never opens. If the intent is "anything clearly good goes without me", the number is
somewhere near **$0.15**: about nine a day, and the $10 daily budget plus the $5 per-order instant
cap bound what that can cost.

## The sleeves

A sleeve is the answer to "may this strategy send without a click". It is a per-strategy mode,
not a separate pot of money - all sleeves draw on the same bankroll and the same caps.

| sleeve | mode | intended contents |
|---|---|---|
| **auto** | `auto` | weather, sports, crypto |
| **semi-systematic** | `manual` | soccer, NBA |
| **research** | `research` | everything unproven; logs fairs, never proposes |

Two constraints on that plan, both measured rather than assumed:

**Nothing currently qualifies for the auto sleeve.** Kelly sizing and, in practice, the case for
unattended trading both rest on a strategy having shown its probabilities beat the price it trades
against. On settled markets:

| strategy | settled samples | Brier, model | Brier, market |
|---|---|---|---|
| `crypto_lognormal` | 4,292 | 0.1058 | **0.1009** |
| `sports_live` | 626 | 0.0834 | **0.0723** |
| `weather_nws` | 344 | 0.2946 | **0.1591** |
| `sports_inplay` | 125 | 0.2587 | **0.2339** |

Every one is worse than the market. Putting weather in the auto sleeve today means unattended
trading on a model 85% worse than the price. The desk will do it if told to; it should be told
knowingly, at the flat 2%-of-bankroll size the calibration gate imposes.

**The arbitrage finders are the exception.** `ladder_arb` and dominance arbitrage do not rest on
an estimate at all - a locked basket is locked by construction - which is why they are exempt from
the calibration gate and are the only things on this desk with a measured edge.

## Entry scope

New risk must be expected to settle inside `max_entry_horizon_hours` (48 = T+2). The gate reads
`expected_expiration_time`, not `close_time`: those differ, and for soccer the close is days after
settlement, so using it would admit exactly what the rule is meant to exclude.

Holding anything in an event moves that event from discovery to management. After that only an
exact same-ticker same-side addition can return, and only after `add_cooldown_seconds` and at
`add_min_edge` with `add_edge_improvement` over what was already taken. `auto_additions` is off:
adding is a fresh risk decision, not a continuation of the last one.

## What the desk will not do, and why

- **Trade a series whose settlement rules nobody has read.** 2,136 of 2,208 contracts in the
  universe are `unknown` on that basis.
- **Quote into a one-sided book.** On a Sunday morning that is 95% of crypto strikes.
- **Act on a model that disagrees with a liquid market by more than `max_disagreement`** (0.30).
  A simple model that far from a two-sided price is the thing that is wrong.
- **Act on an edge smaller than the model's own sigma.**
- **Sell on a stale number.** Too old to enter on is too old to liquidate on.

## Where an edge is actually likely to come from

The honest reading of the evidence on this desk, and of the public literature:

1. **Arbitrage.** Already built, already the only measured edge. Ladder and dominance within an
   event. Cross-venue against Polymarket is the obvious extension and is not built.
2. **Microstructure.** Providing liquidity where the book is thin and the fee is zero for makers,
   rather than predicting outcomes. Kalshi's quadratic fee is 0 for makers on `quadratic` series,
   which is a structural advantage that does not require being right about anything.
3. **Speed on state.** In-play sports, where the desk sees a goal or a made basket before the book
   fully reprices. This is a latency question, not a modelling one, and the recorded books started
   on 2026-09-07 are what will show whether the desk is fast enough.
4. **Structural tails.** Events the market prices with a smooth model that reality does not
   follow - the overtime jump in basketball totals is a real candidate, because regulation-time
   scoring and an extra five minutes are different distributions stitched together.

Public outcome models are not on that list. Searching for one that documents beating a closing
line turns up hobbyist win-probability code, academic play-by-play work, and tout marketing.
That is consistent with the four models measured above: predicting the outcome better than a
liquid market is the hard way to make money here, and this desk has now tried it four times.
