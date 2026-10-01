# ESPN win probability against Kalshi: lead/lag and the discrepancy trade

Generated 2026-09-24 00:21 UTC by `research/espn_leadlag.py` from the desk's own recordings: 274 games, 27,970 ESPN samples with a fresh win probability, 414 game tickers settled. Leagues and dates as recorded (see the extract).

## A mapping bug, found by this study

In 8 games the desk matched ESPN to Kalshi with home and away swapped: ESPN's home win probability moved OPPOSITE to the Kalshi mid of the ticker the desk called home (within-game correlation -0.96, -0.96, -0.96, -0.91, -0.91, -0.86, -0.60, -0.38). ESPN's number was applied to the other team. Cause: sports_live paired team names in the game's order with ticker codes in sorted order, and when the codes did not match ESPN's abbreviations both orientations scored the same and the tie picked one. Fixed 2026-09-23 (each ticker is named from its own market; an ambiguous match is refused). Everything below is computed with those games corrected; section 2 also shows what the desk would have traded as recorded.

## What the data can resolve

ESPN was sampled every 30 s and the Kalshi book about once per desk cycle (15-25 s). A move is therefore placed only to within half a minute, and the Kalshi mid "at" a moment can be a cycle old, which makes Kalshi look LATER than it was. The 0/5/15 s horizons the plan asks for need the Kalshi WebSocket book on game markets, which the desk does not record for sports yet. What follows can say whether Kalshi was already most of the way there when ESPN's new number first appeared, and whether the trade paid at settlement.

## 1. Lead/lag around ESPN jumps

Each row: ESPN jumps of that size between consecutive samples (<= 45 s apart). "Kalshi at +h" is the Kalshi home mid at h seconds after the sample that first showed the jump, minus the mid at the sample before it, in the direction of the jump, as a share of the ESPN jump. 100% means Kalshi moved as far as ESPN did.

| ESPN jump | n | median jump | Kalshi at +0s | Kalshi at +30s | Kalshi at +60s | Kalshi at +120s | Kalshi at +300s | share with Kalshi >= half-way at +0s | Kalshi trend in the minute before |
|---|---|---|---|---|---|---|---|---|---|
| 0.05-0.10 | 468 | 0.07 | 0% (n=461) | 0% (n=460) | 9% (n=461) | 14% (n=444) | 20% (n=413) | 17% | 1.0c |
| 0.10-0.20 | 182 | 0.13 | -0% (n=181) | 4% (n=180) | 7% (n=178) | 9% (n=175) | 15% (n=165) | 18% | 3.0c |
| 0.20-1.00 | 45 | 0.27 | -0% (n=44) | 0% (n=44) | 3% (n=44) | 3% (n=43) | 7% (n=41) | 14% | 6.5c |

**Reading.** Over 613 ESPN jumps averaging 10.1c, Kalshi moved 3.2c in the minute before ESPN's previous sample and 2.5c in the five minutes after ESPN showed the jump (jumps of 10c or more: 5.7c before, 3.8c after). Two things follow. Kalshi never moves anything like as far as ESPN does: most of an ESPN jump is not followed at all. And for the large jumps, where the lane would find its biggest "edges", as much or more of Kalshi's move happened at least 30 s BEFORE ESPN published as after it. At this resolution that is Kalshi moving first and ESPN overshooting, which is the case the plan says makes the discrepancy trade wrong. The Kalshi book here can be a cycle stale, which hides Kalshi moves inside the last cycle; it cannot create the move measured before the previous ESPN sample.

## 2. The trade: ESPN above the Kalshi ask, held to settlement

Taker at the recorded ask (maker fills cannot be simulated from this data), $5 per trade, the taker fee rounded up per order, first trigger per game and side, P&L at settlement. The "edge >= ..." rows are the plan's raw trigger on ESPN's number; the "router" row approximates what the desk's router would let the lane send (the 50/50 blend with the mid, the 0.15 gap and 0.12 edge caps, the uncertainty buffer, min_edge, the cheap-side rule; see backtest()).

| mapping | trigger | cheap side | n | wins | mean ESPN p | mean ask | P&L | per trade | sd | t | 80% lower | fee/trade |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| corrected | router (approx.) | any | 34 | 6 | 0.32 | 0.19 | $-33.17 | $-0.98 | $12.40 | -0.46 | $-3.70 | $0.21 |
| corrected | edge >= 0.08 | <= 0.45 | 106 | 26 | 0.46 | 0.27 | $-130.74 | $-1.23 | $6.93 | -1.83 | $-2.10 | $0.21 |
| corrected | edge >= 0.05 | <= 0.45 | 134 | 36 | 0.42 | 0.28 | $-158.90 | $-1.19 | $6.54 | -2.10 | $-1.91 | $0.21 |
| corrected | edge >= 0.08 | any | 147 | 60 | 0.57 | 0.41 | $-94.88 | $-0.65 | $6.04 | -1.30 | $-1.28 | $0.17 |
| corrected | edge >= 0.05 | any | 187 | 82 | 0.56 | 0.44 | $-171.67 | $-0.92 | $5.04 | -2.49 | $-1.39 | $0.16 |
| corrected | edge >= 0.03 | any | 217 | 98 | 0.56 | 0.47 | $-224.25 | $-1.03 | $4.83 | -3.15 | $-1.45 | $0.15 |
| as recorded | router (approx.) | any | 34 | 7 | 0.33 | 0.20 | $-21.98 | $-0.65 | $12.44 | -0.30 | $-3.38 | $0.20 |
| as recorded | edge >= 0.08 | <= 0.45 | 109 | 29 | 0.47 | 0.28 | $-111.59 | $-1.02 | $6.95 | -1.54 | $-1.88 | $0.21 |
| as recorded | edge >= 0.05 | <= 0.45 | 136 | 38 | 0.44 | 0.28 | $-146.50 | $-1.08 | $6.55 | -1.92 | $-1.80 | $0.21 |
| as recorded | edge >= 0.08 | any | 148 | 61 | 0.57 | 0.40 | $-82.50 | $-0.56 | $6.07 | -1.12 | $-1.20 | $0.17 |
| as recorded | edge >= 0.05 | any | 186 | 81 | 0.56 | 0.43 | $-159.06 | $-0.86 | $5.12 | -2.28 | $-1.34 | $0.16 |
| as recorded | edge >= 0.03 | any | 216 | 97 | 0.56 | 0.46 | $-211.57 | $-0.98 | $4.90 | -2.94 | $-1.41 | $0.15 |

By league, the plan's trigger (edge >= 0.08, ask <= 0.45):

| league | n | wins | mean ESPN p | P&L | per trade |
|---|---|---|---|---|---|
| college-football | 61 | 15 | 0.46 | $-72.31 | $-1.19 |
| mlb | 32 | 8 | 0.46 | $-35.65 | $-1.11 |
| nfl | 13 | 3 | 0.41 | $-22.78 | $-1.75 |

**Reading.** On the plan's trigger (edge >= 0.08, cheap side): 106 trades, 26 won, against 48.4 that ESPN's probabilities expected and 29.0 that the Kalshi asks implied: $-1.23 a trade, t -1.83, 80% lower band $-2.10. The router approximation: 34 trades, $-0.98 a trade, t -0.46. 6 of 6 triggers lose on average. When ESPN and Kalshi disagree, the outcomes follow Kalshi's price more than ESPN's number: conditioning on a large disagreement selects ESPN's errors. By the plan's own gate ("a reading that is not negative") the lane does not qualify on this evidence; the live-sizing rule and the lane's loss stop bound what finding that out live costs, 30 settled trades at a time.

## What would settle the lead/lag question

Record the Kalshi WebSocket ticker for the game markets while ESPN is polled faster (the summary endpoint every 5-10 s for games in progress), then repeat section 1 at 0/5/15 s. The desk already runs a WebSocket quote feed for the commodity windows (MarketQuoteFeed); pointing a second one at live game markets is the smallest change that answers this properly.
