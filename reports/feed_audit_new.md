# Feed semantics audit

**VERDICT: PASS**

Hours: 2026-09-11T23, 2026-09-12T00  
Rows: 1,289,777 over 0.61 h across 18 markets

## Warnings

- truncated: hour=2026-09-12T00 (Compressed file ended before the end-of-stream marker was reached)
- median latency -1.9 ms is negative: the local clock is behind the exchange, which biases every measured lead

## Properties of this feed that the code must match

- no last_update_reason: trade/cancel classification MUST come from the trade channel, not the delta
- per-market sequence checking would report 372,480 gaps where there are 0: seq counts the SUBSCRIPTION, never the market
- a replay must start at an hour containing a connect: an hour with no snapshot in it reconstructs no book at all

## Event mix

- `orderbook_delta`: 1,248,586
- `ticker`: 38,142
- `trade`: 3,027
- `orderbook_snapshot`: 18
- `subscribed`: 3
- `_connect`: 1

## Delta fields

`last_update_reason` present on **0 of 1,248,586** deltas (0.00%).

Keys seen in message bodies, with counts:

- `market_ticker`: 1,289,773
- `price`: 1,289,755
- `ts_ms`: 1,289,755
- `delta`: 1,248,586
- `side`: 1,248,586
- `bid`: 38,160
- `ask`: 38,160
- `bid_size_fp`: 38,142
- `ask_size_fp`: 38,142
- `last_trade_size_fp`: 38,142
- `volume`: 38,142
- `volume_notional_value_dollars`: 38,142
- `volume_24h`: 38,142
- `volume_24h_notional_value_dollars`: 38,142
- `open_interest`: 38,142
- `open_interest_notional_value_dollars`: 38,142
- `reference_price`: 38,142
- `settlement_mark_price`: 38,142
- `liquidation_mark_price`: 38,142
- `funding_rate`: 38,142
- `trade_id`: 3,027
- `count`: 3,027
- `taker_side`: 3,027
- `channel`: 3

## taker_side orientation, re-derived from the tape

Exact magnitude matches between a trade print and a negative delta at the same price within 300 ms.

| taker_side | consumed the ask | consumed the bid |
|---|---:|---:|
| `bid` | 1,072 | 7 |
| `ask` | 2 | 968 |

**TAKER'S OWN SIDE (taker_side=bid is an aggressive BUYER)**

## Sequence continuity

| sid | frames | gaps | duplicates | late |
|---:|---:|---:|---:|---:|
| 1 | 1,248,604 | 0 | 0 | 0 |
| 3 | 3,027 | 0 | 0 | 0 |

Checked per MARKET instead, the same tape would show **372,480** gaps. That number is an artifact of interleaving, not loss.

## Snapshots

18 snapshots: **0 empty**, 18 carrying levels.

Level counts seen: 30 levels x1, 17 levels x1, 57 levels x1, 25 levels x1, 343 levels x1, 58 levels x1

## Book warm-up after a connect

Levels present per side as the streamed book fills in, measured from the FIRST connect on this tape. Where a snapshot arrived full this does not apply and no warm-up is charged.


`KXAAVEPERP`

| s since connect | bid levels | ask levels |
|---:|---:|---:|
| 3.77 | 0 | 0 |
| 9.95 | 8 | 7 |
| 20.69 | 8 | 7 |
| 41.41 | 9 | 9 |
| 90.83 | 8 | 10 |
| 183.43 | 10 | 9 |
| 374.55 | 11 | 10 |
| 750.12 | 11 | 9 |
| 1500.32 | 13 | 10 |

`KXADAPERP`

| s since connect | bid levels | ask levels |
|---:|---:|---:|
| 3.78 | 0 | 0 |
| 9.91 | 4 | 6 |
| 22.69 | 4 | 7 |
| 52.69 | 4 | 7 |
| 106.45 | 6 | 6 |
| 215.19 | 7 | 7 |
| 435.22 | 7 | 8 |
| 896.29 | 6 | 6 |
| 1796.69 | 7 | 7 |

`KXBCHPERP`

| s since connect | bid levels | ask levels |
|---:|---:|---:|
| 0.21 | 0 | 0 |
| 1.23 | 3 | 3 |
| 2.74 | 4 | 4 |
| 5.67 | 9 | 12 |
| 11.52 | 9 | 11 |
| 23.26 | 9 | 13 |
| 47.46 | 13 | 15 |
| 95.23 | 12 | 18 |
| 190.69 | 18 | 18 |
| 382.43 | 20 | 19 |
| 764.92 | 16 | 17 |
| 1530.29 | 23 | 26 |

## Clock

- median exchange-to-receive latency: **-1.9 ms** over 400,000 frames
- p05 -49.2 ms, p95 +76.6 ms
- a negative value is not negative latency; it is the local clock running behind

