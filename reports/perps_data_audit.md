# Perps data audit

Generated 2026-09-11 14:03 UTC from `/Users/samgeng14/Code/kalshi-desk/data/perps_research.sqlite`, 1-minute bars.

`--days 90` is a request, not a fact. Kalshi launched perps in late May 2026 with the
first crypto contracts listed in early June; gold and silver listed 2026-09-10. History
is per-contract and the grades below are the gate on which contracts a model may be
fitted across.

**This report contains no spreads.** The column is `close_quote_gap_bps` =
`ask_close - bid_close` over the mid. Nothing in the API guarantees that the last
observed bid and the last observed ask inside a minute share a timestamp, so this is a
**coarse liquidity proxy**, not an instantaneous spread, and it must not be used as an
execution cost. Differencing `bid_high` against `ask_low` would be worse still: two
different instants, producing a gap that never existed. The live L2 recorder is the
only source of a contemporaneous spread.

## Group A: launch / long history (13)

| ticker | first | last | days | bars | coverage | trades | trades/min | 0-vol min | median $/min | close quote gap bps (p25/med/p75/p95) | avg OI | funding obs |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `KXBTCPERP` | 2026-06-03 14:02 | 2026-09-11 13:58 | 100.0 | 142,193 / 143,997 | 99% | 0 | 0.00 | 1% | 66,688 | 0.3 / 0.5 / 1.3 / 2.5 | 883,114 | 300 |
| `KXETHPERP` | 2026-06-03 14:11 | 2026-09-11 13:58 | 100.0 | 142,160 / 143,988 | 99% | 0 | 0.00 | 2% | 45,580 | 0.6 / 1.1 / 1.7 / 2.6 | 1,048,346 | 300 |
| `KXSOLPERP` | 2026-06-03 14:11 | 2026-09-11 13:58 | 100.0 | 141,582 / 143,988 | 98% | 0 | 0.00 | 34% | 498 | 2.2 / 2.7 / 3.3 / 5.5 | 68,344 | 300 |
| `KXXRPPERP` | 2026-06-03 14:11 | 2026-09-11 13:58 | 100.0 | 141,186 / 143,988 | 98% | 0 | 0.00 | 33% | 411 | 2.8 / 3.6 / 4.0 / 5.9 | 655,931 | 300 |
| `KXHYPEPERP` | 2026-06-08 15:18 | 2026-09-11 13:58 | 94.9 | 134,973 / 136,721 | 99% | 0 | 0.00 | 12% | 2,206 | 1.3 / 3.0 / 4.5 / 7.1 | 126,291 | 285 |
| `KXLINKPERP` | 2026-06-08 20:13 | 2026-09-11 13:58 | 94.7 | 133,541 / 136,426 | 98% | 0 | 0.00 | 59% | 327 | 4.9 / 6.5 / 8.1 / 16.4 | 33,851 | 284 |
| `KXSUIPERP` | 2026-06-09 15:02 | 2026-09-11 13:58 | 94.0 | 133,144 / 135,297 | 98% | 0 | 0.00 | 45% | 2,511 | 2.9 / 3.8 / 5.1 / 12.4 | 26,031 | 282 |
| `KXBCHPERP` | 2026-06-09 15:03 | 2026-09-11 13:58 | 94.0 | 131,043 / 135,296 | 97% | 0 | 0.00 | 47% | 255 | 6.0 / 7.3 / 10.0 / 16.4 | 288,534 | 282 |
| `KXDOGEPERP` | 2026-06-09 15:03 | 2026-09-11 13:58 | 94.0 | 131,953 / 135,296 | 98% | 0 | 0.00 | 49% | 494 | 3.3 / 5.7 / 6.8 / 8.1 | 31,374 | 282 |
| `KXLTCPERP` | 2026-06-09 15:03 | 2026-09-11 13:58 | 94.0 | 130,819 / 135,296 | 97% | 0 | 0.00 | 48% | 796 | 3.3 / 4.3 / 5.1 / 6.6 | 47,976 | 282 |
| `KXKSHIBPERP` | 2026-06-09 15:43 | 2026-09-11 13:58 | 93.9 | 132,792 / 135,256 | 98% | 0 | 0.00 | 54% | 501 | 6.1 / 10.1 / 11.4 / 15.6 | 35,778 | 282 |
| `KXNEARPERP` | 2026-06-23 21:03 | 2026-09-11 13:58 | 79.7 | 111,495 / 114,776 | 97% | 0 | 0.00 | 32% | 899 | 6.1 / 8.2 / 11.3 / 17.1 | 153,470 | 239 |
| `KXZECPERP` | 2026-06-23 21:03 | 2026-09-11 13:58 | 79.7 | 113,318 / 114,776 | 99% | 0 | 0.00 | 32% | 1,208 | 4.6 / 5.5 / 8.0 / 15.1 | 82,316 | 239 |

## Group B: intermediate history (0)

_none_

## Group C: newly listed (5)

| ticker | first | last | days | bars | coverage | trades | trades/min | 0-vol min | median $/min | close quote gap bps (p25/med/p75/p95) | avg OI | funding obs |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `KXVVVPERP` | 2026-08-28 14:22 | 2026-09-11 13:58 | 14.0 | 19,668 / 20,137 | 98% | 0 | 0.00 | 59% | 500 | 8.7 / 10.4 / 12.4 / 18.0 | 35,697 | 42 |
| `KXAAVEPERP` | 2026-08-28 14:25 | 2026-09-11 13:58 | 14.0 | 19,580 / 20,134 | 97% | 0 | 0.00 | 80% | 34 | 13.4 / 15.3 / 17.1 / 20.3 | 31,342 | 42 |
| `KXWLDPERP` | 2026-08-28 14:25 | 2026-09-11 13:58 | 14.0 | 19,508 / 20,134 | 97% | 0 | 0.00 | 68% | 502 | 9.6 / 13.2 / 16.0 / 19.3 | 124,516 | 41 |
| `KXADAPERP` | 2026-08-31 15:04 | 2026-09-11 13:58 | 11.0 | 15,384 / 15,775 | 98% | 0 | 0.00 | 70% | 1,003 | 9.2 / 13.5 / 14.6 / 18.1 | 551,736 | 33 |
| `KXBNBPERP` | 2026-08-31 15:07 | 2026-09-11 13:58 | 11.0 | 14,805 / 15,772 | 94% | 0 | 0.00 | 64% | 46 | 9.0 / 10.8 / 16.8 / 22.1 | 136,725 | 33 |

## Group D: insufficient data -- do not fit (2)

| ticker | first | last | days | bars | coverage | trades | trades/min | 0-vol min | median $/min | close quote gap bps (p25/med/p75/p95) | avg OI | funding obs |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `KXGOLDPERP` | 2026-09-10 13:50 | 2026-09-11 13:58 | 1.0 | 1,445 / 1,449 | 100% | 0 | 0.00 | 1% | 2,334 | 0.2 / 1.1 / 2.3 / 3.2 | 378,455 | 0 |
| `KXSILVERPERP` | 2026-09-10 14:01 | 2026-09-11 13:58 | 1.0 | 1,437 / 1,438 | 100% | 0 | 0.00 | 13% | 347 | 0.3 / 2.3 / 3.4 / 4.2 | 54,927 | 0 |

## What this means for the research

- **13 contracts are fittable** (grade A or B). Everything else is a description of a few days.
- Longest history: `KXBTCPERP` at 100 days.
- **2 of them trade in under half of their minutes** (`KXLINKPERP`, `KXKSHIBPERP`). A quote change with no trade is not meaningless -- orders arriving and cancelling move the implied clearing price, and that can carry information. What it is not is the *same* data-generating process as a minute with flow, so results are reported by trade-count regime (0, 1-5, >5) rather than pooled across them.
- Any pooled fit must either restrict to one grade or carry a contract fixed effect. Pooling a 90-day contract with a 2-day one weights the result entirely toward the former while appearing to be a statement about the book.

## Coverage gaps

A coverage figure below 100% means minutes the exchange returned no bar for. That is usually a market with no quote rather than an outage, but a run of them is worth checking before treating the series as continuous.
