# Execution simulator

Hours replayed: 2026-09-11T14, 2026-09-11T15, 2026-09-11T16  
Events: 18,808,420  Orders simulated: 211,940  
Order notional: $25.00  Sample interval: 5.0s  Passive timeout: 30.0s  Max size: 25% of touch

## The number that decides everything

Best net 60s EV on the pessimistic queue bound: **KXLINKPERP / improve = -1.63 bps** over 594 fills (20.2% of attempts).

That is entry cost -8.72 + fee 5.00 against a 60s markout of -5.34 bps.

It is not positive. Execution does not pay for itself on this sample, which means no signal built on top of it can be evaluated against a cheaper cost assumption than this one.

## Fill rate: the two bounds

The gap between the bounds is the error bar. A mode whose conclusion flips across it has no conclusion.

| market | mode | attempts | fill% pess | fill% opt | gap | median wait s |
|---|---|---:|---:|---:|---:|---:|
| KXAAVEPERP | taker | 2946 | 100.0 | 100.0 | 0.0 | 0.0 |
| KXADAPERP | taker | 2882 | 100.0 | 100.0 | 0.0 | 0.0 |
| KXBCHPERP | taker | 2858 | 100.0 | 100.0 | 0.0 | 0.0 |
| KXBNBPERP | taker | 2912 | 100.0 | 100.0 | 0.0 | 0.0 |
| KXBTCPERP | taker | 3082 | 100.0 | 100.0 | 0.0 | 0.0 |
| KXDOGEPERP | taker | 2960 | 100.0 | 100.0 | 0.0 | 0.0 |
| KXETHPERP | taker | 3110 | 100.0 | 100.0 | 0.0 | 0.0 |
| KXHYPEPERP | taker | 3030 | 100.0 | 100.0 | 0.0 | 0.0 |
| KXKSHIBPERP | taker | 2512 | 100.0 | 100.0 | 0.0 | 0.0 |
| KXLINKPERP | taker | 2946 | 100.0 | 100.0 | 0.0 | 0.0 |
| KXLTCPERP | taker | 2928 | 100.0 | 100.0 | 0.0 | 0.0 |
| KXNEARPERP | taker | 3030 | 100.0 | 100.0 | 0.0 | 0.0 |
| KXSOLPERP | taker | 2982 | 100.0 | 100.0 | 0.0 | 0.0 |
| KXSUIPERP | taker | 2930 | 100.0 | 100.0 | 0.0 | 0.0 |
| KXVVVPERP | taker | 3080 | 100.0 | 100.0 | 0.0 | 0.0 |
| KXWLDPERP | taker | 2866 | 100.0 | 100.0 | 0.0 | 0.0 |
| KXXRPPERP | taker | 3084 | 100.0 | 100.0 | 0.0 | 0.0 |
| KXZECPERP | taker | 3032 | 100.0 | 100.0 | 0.0 | 0.0 |
| KXBTCPERP | join_then_cross | 3082 | 100.0 | 100.0 | 0.0 | 0.9 |
| KXETHPERP | join_then_cross | 3110 | 99.9 | 99.9 | 0.0 | 1.0 |
| KXSOLPERP | join_then_cross | 2982 | 99.8 | 99.8 | 0.0 | 16.0 |
| KXDOGEPERP | join_then_cross | 2960 | 99.8 | 99.8 | 0.0 | 30.0 |
| KXADAPERP | join_then_cross | 2882 | 99.8 | 99.8 | 0.0 | 30.0 |
| KXXRPPERP | join_then_cross | 3084 | 99.7 | 99.7 | 0.0 | 15.3 |

## Adverse selection

Markout at 1s conditional on a passive fill. Negative means the fill was immediately wrong: the book moved against the side that was standing still. This is the cost that a fee schedule cannot show you.

| market | mode | fills | 1s | 5s | 10s | 60s | 300s |
|---|---|---:|---:|---:|---:|---:|---:|
| KXLINKPERP | join_then_cross | 2936 | -8.91 | -8.92 | -8.85 | -8.93 | -8.80 |
| KXWLDPERP | join_then_cross | 2857 | -8.07 | -8.20 | -8.39 | -8.54 | -8.35 |
| KXBCHPERP | join_then_cross | 2849 | -7.84 | -7.86 | -7.83 | -7.89 | -7.89 |
| KXWLDPERP | improve | 922 | -7.72 | -8.36 | -9.18 | -9.56 | -10.98 |
| KXWLDPERP | join | 762 | -7.63 | -8.16 | -9.11 | -9.61 | -11.32 |
| KXSUIPERP | join_then_cross | 2922 | -7.60 | -7.60 | -7.63 | -7.45 | -7.72 |
| KXSUIPERP | join | 695 | -7.58 | -7.80 | -7.81 | -7.73 | -14.74 |
| KXLINKPERP | join | 545 | -7.50 | -7.84 | -7.57 | -6.37 | -11.04 |
| KXKSHIBPERP | join_then_cross | 2504 | -7.49 | -7.55 | -7.49 | -7.42 | -7.49 |
| KXAAVEPERP | join_then_cross | 2938 | -7.18 | -7.21 | -7.18 | -7.14 | -7.19 |
| KXSUIPERP | improve | 729 | -6.88 | -7.12 | -7.13 | -7.21 | -13.72 |
| KXNEARPERP | join_then_cross | 3022 | -6.84 | -6.59 | -6.71 | -6.99 | -6.63 |
| KXAAVEPERP | join | 712 | -6.82 | -7.17 | -7.23 | -8.37 | -8.09 |
| KXBCHPERP | join | 522 | -6.74 | -6.88 | -7.03 | -8.07 | -14.66 |
| KXZECPERP | join_then_cross | 3023 | -6.58 | -7.00 | -7.15 | -6.71 | -6.89 |
| KXLINKPERP | improve | 594 | -6.34 | -6.77 | -6.48 | -5.34 | -10.55 |
| KXAAVEPERP | improve | 807 | -6.30 | -6.55 | -6.36 | -7.42 | -8.09 |
| KXKSHIBPERP | join | 504 | -6.27 | -6.50 | -6.28 | -6.81 | -8.67 |
| KXVVVPERP | join | 1912 | -6.14 | -6.37 | -5.93 | -5.71 | -2.47 |
| KXNEARPERP | join | 1616 | -5.91 | -5.69 | -6.11 | -6.58 | -9.77 |
| KXZECPERP | join | 1427 | -5.85 | -6.77 | -7.03 | -6.86 | -8.85 |
| KXVVVPERP | improve | 1956 | -5.74 | -6.16 | -5.76 | -5.33 | -2.79 |
| KXVVVPERP | join_then_cross | 3071 | -5.67 | -5.90 | -5.65 | -5.63 | -5.68 |
| KXZECPERP | improve | 1475 | -5.20 | -6.11 | -6.44 | -6.16 | -8.54 |
| KXBNBPERP | join | 615 | -5.19 | -5.15 | -5.22 | -6.17 | -7.69 |
| KXADAPERP | improve | 1746 | -5.05 | -5.20 | -5.07 | -5.30 | -5.29 |
| KXBNBPERP | join_then_cross | 2904 | -4.99 | -4.96 | -4.93 | -5.12 | -4.91 |
| KXADAPERP | join_then_cross | 2875 | -4.95 | -4.91 | -4.92 | -5.17 | -4.90 |
| KXNEARPERP | improve | 1703 | -4.95 | -4.92 | -5.12 | -5.88 | -9.08 |
| KXKSHIBPERP | improve | 577 | -4.89 | -5.07 | -4.84 | -4.89 | -6.79 |
| KXDOGEPERP | join | 1055 | -4.67 | -4.77 | -4.51 | -4.22 | -4.80 |
| KXDOGEPERP | join_then_cross | 2953 | -4.56 | -4.58 | -4.48 | -4.61 | -4.38 |
| KXBCHPERP | improve | 674 | -4.50 | -4.79 | -4.87 | -5.35 | -14.28 |
| KXDOGEPERP | improve | 1089 | -4.43 | -4.57 | -4.27 | -3.88 | -4.71 |
| KXBNBPERP | improve | 866 | -4.20 | -4.13 | -4.20 | -4.89 | -5.57 |
| KXLTCPERP | join | 1265 | -3.94 | -3.76 | -3.73 | -3.55 | -4.64 |
| KXLTCPERP | improve | 1290 | -3.90 | -3.77 | -3.75 | -3.53 | -4.77 |
| KXLTCPERP | join_then_cross | 2920 | -3.66 | -3.60 | -3.61 | -3.64 | -3.39 |
| KXHYPEPERP | join_then_cross | 3021 | -3.65 | -3.57 | -3.51 | -3.77 | -3.51 |
| KXADAPERP | join | 1310 | -3.63 | -3.87 | -3.90 | -3.36 | -4.47 |
| KXHYPEPERP | join | 1542 | -3.42 | -3.42 | -3.31 | -3.48 | -4.15 |
| KXHYPEPERP | improve | 1583 | -3.23 | -3.23 | -3.06 | -3.34 | -3.82 |
| KXXRPPERP | join | 1942 | -2.60 | -2.78 | -3.02 | -2.76 | -2.86 |
| KXXRPPERP | join_then_cross | 3076 | -2.58 | -2.66 | -2.80 | -3.10 | -2.70 |
| KXXRPPERP | improve | 2118 | -2.44 | -2.64 | -2.77 | -2.64 | -2.36 |
| KXSOLPERP | join | 1846 | -2.22 | -2.42 | -2.53 | -2.34 | -2.53 |
| KXSOLPERP | join_then_cross | 2977 | -2.17 | -2.30 | -2.36 | -2.55 | -2.36 |
| KXSOLPERP | improve | 1905 | -2.15 | -2.28 | -2.34 | -2.16 | -2.47 |
| KXETHPERP | join | 2800 | -0.58 | -0.69 | -0.69 | -0.78 | -0.68 |
| KXETHPERP | join_then_cross | 3107 | -0.54 | -0.63 | -0.64 | -0.73 | -0.62 |
| KXETHPERP | improve | 2062 | -0.47 | -0.61 | -0.51 | -0.52 | -0.60 |
| KXBTCPERP | join | 2780 | -0.34 | -0.33 | -0.43 | -0.54 | -0.50 |
| KXBTCPERP | join_then_cross | 3081 | -0.34 | -0.33 | -0.41 | -0.51 | -0.39 |
| KXBTCPERP | improve | 2717 | -0.27 | -0.28 | -0.37 | -0.39 | -0.28 |

## Where the spread goes

For a passive fill the spread is the whole revenue: you buy at the bid and the mid is half a spread above you. What you KEEP is that half-spread plus the markout, and the markout is what the market takes back for having traded with you.

| market | spread | half-spread | 60s markout | kept | maker fee | net |
|---|---:|---:|---:|---:|---:|---:|
| KXLINKPERP | 17.9 | 8.9 | -6.37 | **+2.57** | 5.0 | -2.51 |
| KXWLDPERP | 16.6 | 8.3 | -9.61 | **-1.33** | 5.0 | -6.37 |
| KXBCHPERP | 15.8 | 7.9 | -8.07 | **-0.15** | 5.0 | -5.32 |
| KXKSHIBPERP | 15.4 | 7.7 | -6.81 | **+0.90** | 5.0 | -4.20 |
| KXNEARPERP | 15.2 | 7.6 | -6.58 | **+1.04** | 5.0 | -4.07 |
| KXSUIPERP | 15.1 | 7.6 | -7.73 | **-0.16** | 5.0 | -5.21 |
| KXZECPERP | 14.4 | 7.2 | -6.86 | **+0.33** | 5.0 | -4.63 |
| KXAAVEPERP | 14.1 | 7.0 | -8.37 | **-1.34** | 5.0 | -6.39 |
| KXVVVPERP | 9.9 | 4.9 | -5.71 | **-0.77** | 5.0 | -5.77 |
| KXADAPERP | 9.6 | 4.8 | -3.36 | **+1.43** | 5.0 | -2.80 |
| KXBNBPERP | 9.6 | 4.8 | -6.17 | **-1.39** | 5.0 | -6.38 |
| KXDOGEPERP | 8.1 | 4.1 | -4.22 | **-0.17** | 5.0 | -4.52 |
| KXHYPEPERP | 7.3 | 3.7 | -3.48 | **+0.18** | 5.0 | -4.55 |
| KXLTCPERP | 6.0 | 3.0 | -3.55 | **-0.57** | 5.0 | -5.16 |
| KXXRPPERP | 4.4 | 2.2 | -2.76 | **-0.57** | 5.0 | -5.23 |
| KXSOLPERP | 3.4 | 1.7 | -2.34 | **-0.64** | 5.0 | -5.26 |
| KXETHPERP | 0.8 | 0.4 | -0.78 | **-0.39** | 5.0 | -5.29 |
| KXBTCPERP | 0.5 | 0.3 | -0.54 | **-0.28** | 5.0 | -5.21 |

**Correlation between the quoted spread and the 60-second markout: -0.930** across 18 markets.

A wide spread is not a wider margin. It is compensation for the information in the flow that crosses it, and the compensation is close to exact: the markout scales with the spread almost one for one. What survives -- the `kept` column -- runs from -1.39 to +2.57 bps, mean -0.07.

The tightest market on the board (`KXBTCPERP`, 0.5 bps) shows the same thing from the other end: its markout is only -0.54 bps, so almost nothing is lost to selection -- and there is almost nothing to win either.

Against a maker fee of 5 bps, the fee is roughly 68x what the market leaves on the table. That ratio, not the choice of feature or horizon, is what decides whether quoting these contracts can work.

## Cost of crossing

| market | fills | entry cost bps | + fee | total to cross |
|---|---:|---:|---:|---:|
| KXAAVEPERP | 2946 | 7.28 | 12.00 | 19.28 |
| KXADAPERP | 2882 | 5.70 | 12.00 | 17.70 |
| KXBCHPERP | 2858 | 8.09 | 12.00 | 20.09 |
| KXBNBPERP | 2912 | 4.94 | 12.00 | 16.94 |
| KXBTCPERP | 3082 | 0.33 | 12.00 | 12.33 |
| KXDOGEPERP | 2960 | 4.77 | 12.00 | 16.77 |
| KXETHPERP | 3110 | 0.51 | 12.00 | 12.51 |
| KXHYPEPERP | 3030 | 3.99 | 12.00 | 15.99 |
| KXKSHIBPERP | 2512 | 7.81 | 12.00 | 19.81 |
| KXLINKPERP | 2946 | 9.25 | 12.00 | 21.25 |
| KXLTCPERP | 2928 | 3.50 | 12.00 | 15.50 |
| KXNEARPERP | 3030 | 7.81 | 12.00 | 19.81 |
| KXSOLPERP | 2982 | 2.15 | 12.00 | 14.15 |
| KXSUIPERP | 2930 | 7.63 | 12.00 | 19.63 |
| KXVVVPERP | 3080 | 5.02 | 12.00 | 17.02 |
| KXWLDPERP | 2866 | 8.36 | 12.00 | 20.36 |
| KXXRPPERP | 3084 | 2.55 | 12.00 | 14.55 |
| KXZECPERP | 3032 | 7.37 | 12.00 | 19.37 |

## Orders not simulated

- `no_room_to_improve`: 1,144
- `size_exceeds_touch`: 72

1% of sampled decisions were dropped before an order was placed.

## What would change these numbers

- Our order is absent from the replayed book. Fill rates are therefore optimistic for `improve` (a real order inside the spread invites others to step in front) and roughly neutral for `join` at small size.
- A resting order here never repositions. Real quoting cancels and replaces, which raises fill rate and worsens the 1s markout at the same time.
- Markouts use the mid, not what we could have exited at. The exit is a second spread. A round trip costs the entry above plus an exit of comparable size.
- Sample: one session of recorded L2. Intraday, day-of-week and regime variation are not in it.

