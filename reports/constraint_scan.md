# Constraint arbitrage: how much room is actually there?
Every leg required within 5s of the others, so no basket is assembled from quotes that never stood at the same moment.

Recorded quotes, 8.0 days back. 716 mutually-exclusive events examined, 713 of them with quotes in the window, 5,767,945 quote updates replayed.
Each event contributes its single best moment, not every tick, so nothing below is one wide spread counted thousands of times.

## Sell-side ladder

*buy NO on every bracket; needs only the exclusivity flag*

703 events presented the shape at least once.

| | best net edge / contract | gross (before fees) | fee paid |
|---|---:|---:|---:|
| median event | -0.0359 | -0.0100 | 0.0245 |
| 90th pct | -0.0068 | +0.0100 | 0.0455 |
| 99th pct | +0.0020 | +0.0400 | 0.0535 |
| **best single moment** | **+0.0163** | +0.0500 | - |

- events where the net edge was ever positive at all: **9 of 703**
- events that ever cleared the live `ladder_min_edge` of $0.02: **0**
- median fee the basket had to overcome: **$0.0245/contract**

Best events:

| event | net | gross | fee | legs |
|---|---:|---:|---:|---:|
| KXMLBGAME-26SEP081940AZKC | +0.0163 | +0.0300 | 0.0137 | 2 |
| KXBTC-26SEP1012 | +0.0153 | +0.0500 | 0.0347 | 3 |
| KXHIGHPHIL-26SEP07 | +0.0079 | +0.0100 | 0.0021 | 2 |
| KXHIGHNY-26SEP07 | +0.0065 | +0.0100 | 0.0035 | 2 |
| KXMLBGAME-26SEP081840HOUPHI | +0.0045 | +0.0100 | 0.0055 | 2 |
| KXMLBGAME-26SEP082140WSHSD | +0.0045 | +0.0100 | 0.0055 | 2 |
| KXMLBGAME-26SEP072205TORATH | +0.0040 | +0.0200 | 0.0160 | 2 |
| KXNCAAFGAME-26SEP06TXSOPV | +0.0020 | +0.0200 | 0.0180 | 2 |

## Dominance pair

*sell the harder contract, buy the easier one*

No event ever presented this shape in the window.

### Why the dominance finder found nothing

Of the 713 events examined, **0** contain even one pair where one contract's condition implies another's.

That is structural, not bad luck. `run_ladder_arb` groups markets by event and hands one event's contracts to the finder, but the brackets inside a mutually exclusive event are disjoint by construction - that is what makes the event exclusive. Containment can only appear BETWEEN events, or between series, which is precisely where the code never looks. `find_dominance_arb` has therefore never had a single candidate pair to price, and no threshold change would alter that. If dominance is wanted, the scan has to be re-wired to compare across events on the same underlying, and that needs a verified statement that the two events settle off the same source - the cross-event relation problem.

## What this means for constraint_arb_v2

The median basket paid **$0.0245/contract** in fees just to exist. Kalshi's fee is charged per leg and is largest at mid prices, so a solver that finds bigger baskets is buying a bigger fee bill. Any generalization has to earn its extra legs against that, and the fee is known before the edge is.

Nothing here is a forecast and nothing here traded. It is the headroom measurement that should decide whether the engine is worth building, and at what threshold.
