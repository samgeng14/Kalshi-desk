# Feed semantics audit

**VERDICT: PASS**

Hours: 2026-09-11T14  
Rows: 6,078,688 over 0.96 h across 18 markets

## Warnings

- 18 of 36 snapshots arrived with no levels. That is a race at subscribe time, not the protocol -- the rest carry full depth. Only the empty ones need a warm-up
- median latency -33.1 ms is negative: the local clock is behind the exchange, which biases every measured lead

## Properties of this feed that the code must match

- no last_update_reason: trade/cancel classification MUST come from the trade channel, not the delta
- per-market sequence checking would report 3,233,929 gaps where there are 0: seq counts the SUBSCRIPTION, never the market
- a replay must start at an hour containing a connect: an hour with no snapshot in it reconstructs no book at all
- 2 connects: sequences restart at 1 and every book empties at each one

## Event mix

- `orderbook_delta`: 6,017,346
- `ticker`: 42,678
- `trade`: 18,620
- `orderbook_snapshot`: 36
- `subscribed`: 6
- `_connect`: 2

## Delta fields

`last_update_reason` present on **0 of 6,017,346** deltas (0.00%).

Keys seen in message bodies, with counts:

- `market_ticker`: 6,078,680
- `price`: 6,078,644
- `ts_ms`: 6,078,644
- `delta`: 6,017,346
- `side`: 6,017,346
- `bid`: 42,696
- `ask`: 42,696
- `bid_size_fp`: 42,678
- `ask_size_fp`: 42,678
- `last_trade_size_fp`: 42,678
- `volume`: 42,678
- `volume_notional_value_dollars`: 42,678
- `volume_24h`: 42,678
- `volume_24h_notional_value_dollars`: 42,678
- `open_interest`: 42,678
- `open_interest_notional_value_dollars`: 42,678
- `reference_price`: 42,678
- `settlement_mark_price`: 42,678
- `liquidation_mark_price`: 42,678
- `funding_rate`: 42,678
- `trade_id`: 18,620
- `count`: 18,620
- `taker_side`: 18,620
- `market_id`: 18

## taker_side orientation, re-derived from the tape

Exact magnitude matches between a trade print and a negative delta at the same price within 300 ms.

| taker_side | consumed the ask | consumed the bid |
|---|---:|---:|
| `bid` | 3,483 | 180 |
| `ask` | 161 | 3,117 |

**TAKER'S OWN SIDE (taker_side=bid is an aggressive BUYER)**

## Sequence continuity

| sid | frames | gaps | duplicates | late |
|---:|---:|---:|---:|---:|
| 1 | 6,017,382 | 0 | 0 | 0 |
| 3 | 18,620 | 0 | 0 | 0 |

Checked per MARKET instead, the same tape would show **3,233,929** gaps. That number is an artifact of interleaving, not loss.

## Snapshots

36 snapshots: **18 empty**, 18 carrying levels.

Level counts seen: 0 levels x18, 45 levels x2, 46 levels x2, 29 levels x1, 17 levels x1, 48 levels x1

## Book warm-up after a connect

Levels present per side as the streamed book fills in, measured from the FIRST connect on this tape. Where a snapshot arrived full this does not apply and no warm-up is charged.


`KXAAVEPERP`

| s since connect | bid levels | ask levels |
|---:|---:|---:|
| 0.28 | 0 | 0 |
| 0.58 | 2 | 3 |
| 1.31 | 2 | 3 |
| 2.69 | 8 | 6 |
| 5.39 | 11 | 10 |
| 11.09 | 11 | 6 |
| 22.24 | 10 | 12 |
| 44.49 | 6 | 5 |
| 89.24 | 11 | 14 |
| 178.7 | 11 | 12 |
| 357.71 | 11 | 12 |
| 715.42 | 6 | 8 |
| 1431.17 | 10 | 10 |

`KXADAPERP`

| s since connect | bid levels | ask levels |
|---:|---:|---:|
| 0.28 | 0 | 0 |
| 0.58 | 2 | 3 |
| 1.49 | 2 | 3 |
| 3.44 | 6 | 4 |
| 6.93 | 8 | 7 |
| 13.89 | 6 | 7 |
| 27.84 | 7 | 6 |
| 55.68 | 7 | 8 |
| 111.61 | 7 | 6 |
| 223.27 | 7 | 7 |
| 446.6 | 7 | 6 |
| 893.43 | 7 | 5 |
| 1786.87 | 7 | 6 |

`KXBCHPERP`

| s since connect | bid levels | ask levels |
|---:|---:|---:|
| 0.39 | 1 | 0 |
| 0.99 | 3 | 3 |
| 2.17 | 9 | 5 |
| 4.49 | 10 | 11 |
| 10.07 | 6 | 14 |
| 20.19 | 14 | 15 |
| 40.52 | 14 | 14 |
| 81.07 | 15 | 15 |
| 162.23 | 14 | 18 |
| 324.97 | 16 | 15 |
| 650.77 | 16 | 18 |
| 1301.9 | 23 | 25 |

## Clock

- median exchange-to-receive latency: **-33.1 ms** over 400,000 frames
- p05 -34.5 ms, p95 -28.9 ms
- a negative value is not negative latency; it is the local clock running behind

