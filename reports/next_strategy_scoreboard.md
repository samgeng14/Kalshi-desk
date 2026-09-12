# Strategy scoreboard

Net is per trade, at **tier 0** -- this account's actual fees. A strategy is not profitable because its gross edge is positive, its IC is significant, its accuracy beats 50%, or a lower fee tier would rescue it.

83 strategy/market rows. **0** are not rejected.

| strategy | market | h | trades/day | gross | fee | slippage | net | break-even maker | tier | status |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| `impulse_minute_bar` | all | 1m-1h |  | +16.90 | 5.0 | 0.0 | **-0.10** |  |  | REJECTED_NONROBUST |
| `unconditional_maker_improve` | KXLINKPERP | 60s | 5702.4 | +8.88 | 5.0 | 0.0 | **-1.47** | 3.53 | 2 | REJECTED_COST |
| `funding_capture_unhedged` | KXZECPERP | 8h | 0.125 | +7.28 |  | 8.83 | **-1.55** |  |  | REJECTED_RISK |
| `unconditional_maker_improve` | KXKSHIBPERP | 60s | 5539.2 | +7.67 | 5.0 | 0.0 | **-2.23** | 2.77 | 3 | REJECTED_COST |
| `unconditional_maker_join` | KXLINKPERP | 60s | 5232.0 | +8.94 | 5.0 | 0.0 | **-2.43** | 2.57 | 3 | REJECTED_COST |
| `unconditional_maker_improve` | KXBCHPERP | 60s | 6470.4 | +7.92 | 5.0 | 0.0 | **-2.44** | 2.56 | 3 | REJECTED_COST |
| `funding_capture_unhedged` | KXADAPERP | 8h | 0.125 | +3.65 |  | 6.74 | **-3.09** |  |  | REJECTED_RISK |
| `unconditional_maker_improve` | KXNEARPERP | 60s | 16348.8 | +7.62 | 5.0 | 0.0 | **-3.26** | 1.74 | 5 | REJECTED_COST |
| `unconditional_maker_join` | KXADAPERP | 60s | 12576.0 | +4.79 | 5.0 | 0.0 | **-3.57** | 1.43 | 6 | REJECTED_COST |
| `unconditional_maker_join` | KXNEARPERP | 60s | 15513.6 | +7.62 | 5.0 | 0.0 | **-3.96** | 1.04 | 8 | REJECTED_COST |
| `unconditional_maker_improve` | KXZECPERP | 60s | 14160.0 | +7.16 | 5.0 | 0.0 | **-4.00** | 1.0 | 9 | REJECTED_COST |
| `unconditional_maker_join` | KXKSHIBPERP | 60s | 4838.4 | +7.71 | 5.0 | 0.0 | **-4.10** | 0.9 | 9 | REJECTED_COST |
| `unconditional_maker_improve` | KXSUIPERP | 60s | 6998.4 | +7.56 | 5.0 | 0.0 | **-4.65** | 0.35 |  | REJECTED_COST |
| `unconditional_maker_join` | KXZECPERP | 60s | 13699.2 | +7.19 | 5.0 | 0.0 | **-4.67** | 0.33 |  | REJECTED_COST |
| `unconditional_maker_improve` | KXHYPEPERP | 60s | 15196.8 | +3.65 | 5.0 | 0.0 | **-4.69** | 0.31 |  | REJECTED_COST |
| `unconditional_maker_join` | KXHYPEPERP | 60s | 14803.2 | +3.66 | 5.0 | 0.0 | **-4.82** | 0.18 |  | REJECTED_COST |
| `unconditional_maker_improve` | KXDOGEPERP | 60s | 10454.4 | +4.05 | 5.0 | 0.0 | **-4.83** | 0.17 |  | REJECTED_COST |
| `unconditional_maker_improve` | KXBNBPERP | 60s | 8313.6 | +4.78 | 5.0 | 0.0 | **-5.11** |  |  | REJECTED_COST |
| `unconditional_maker_improve` | KXETHPERP | 60s | 19795.2 | +0.39 | 5.0 | 0.0 | **-5.13** |  |  | REJECTED_COST |
| `unconditional_maker_improve` | KXBTCPERP | 60s | 26083.2 | +0.25 | 5.0 | 0.0 | **-5.14** |  |  | REJECTED_COST |
| `unconditional_maker_join` | KXBCHPERP | 60s | 5011.2 | +7.92 | 5.0 | 0.0 | **-5.15** |  |  | REJECTED_COST |
| `unconditional_maker_join` | KXSUIPERP | 60s | 6672.0 | +7.57 | 5.0 | 0.0 | **-5.16** |  |  | REJECTED_COST |
| `unconditional_maker_join` | KXDOGEPERP | 60s | 10128.0 | +4.05 | 5.0 | 0.0 | **-5.17** |  |  | REJECTED_COST |
| `unconditional_maker_join` | KXBTCPERP | 60s | 26688.0 | +0.25 | 5.0 | 0.0 | **-5.28** |  |  | REJECTED_COST |
| `unconditional_maker_improve` | KXVVVPERP | 60s | 18777.6 | +4.96 | 5.0 | 0.0 | **-5.37** |  |  | REJECTED_COST |
| `unconditional_maker_join` | KXETHPERP | 60s | 26880.0 | +0.38 | 5.0 | 0.0 | **-5.39** |  |  | REJECTED_COST |
| `unconditional_maker_improve` | KXAAVEPERP | 60s | 7747.2 | +7.02 | 5.0 | 0.0 | **-5.41** |  |  | REJECTED_COST |
| `unconditional_maker_improve` | KXXRPPERP | 60s | 20332.8 | +2.19 | 5.0 | 0.0 | **-5.45** |  |  | REJECTED_COST |
| `unconditional_maker_improve` | KXSOLPERP | 60s | 18288.0 | +1.70 | 5.0 | 0.0 | **-5.47** |  |  | REJECTED_COST |
| `unconditional_maker_improve` | KXADAPERP | 60s | 16761.6 | +4.79 | 5.0 | 0.0 | **-5.51** |  |  | REJECTED_COST |
| `unconditional_maker_improve` | KXLTCPERP | 60s | 12384.0 | +2.98 | 5.0 | 0.0 | **-5.55** |  |  | REJECTED_COST |
| `unconditional_maker_join` | KXLTCPERP | 60s | 12144.0 | +2.98 | 5.0 | 0.0 | **-5.57** |  |  | REJECTED_COST |
| `unconditional_maker_join` | KXXRPPERP | 60s | 18643.2 | +2.19 | 5.0 | 0.0 | **-5.57** |  |  | REJECTED_COST |
| `unconditional_maker_join` | KXSOLPERP | 60s | 17721.6 | +1.70 | 5.0 | 0.0 | **-5.64** |  |  | REJECTED_COST |
| `unconditional_maker_join` | KXVVVPERP | 60s | 18355.2 | +4.94 | 5.0 | 0.0 | **-5.77** |  |  | REJECTED_COST |
| `unconditional_maker_improve` | KXWLDPERP | 60s | 8851.2 | +8.28 | 5.0 | 0.0 | **-6.28** |  |  | REJECTED_COST |
| `unconditional_maker_join` | KXWLDPERP | 60s | 7315.2 | +8.28 | 5.0 | 0.0 | **-6.33** |  |  | REJECTED_COST |
| `unconditional_maker_join` | KXAAVEPERP | 60s | 6835.2 | +7.03 | 5.0 | 0.0 | **-6.34** |  |  | REJECTED_COST |
| `unconditional_maker_join` | KXBNBPERP | 60s | 5904.0 | +4.78 | 5.0 | 0.0 | **-6.39** |  |  | REJECTED_COST |
| `funding_capture_unhedged` | KXNEARPERP | 8h | 0.125 | +1.20 |  | 8.47 | **-7.27** |  |  | REJECTED_RISK |

## Status meanings

- `LIVE_CANDIDATE` -- positive net AND a bootstrap lower bound above zero
- `PROMISING` -- positive net, interval includes zero
- `REJECTED_COST` -- real effect, smaller than the cost of trading it
- `REJECTED_NONROBUST` -- does not survive multiple-testing correction
- `REJECTED_RISK` -- the edge is real but trivial against the risk carried to collect it
- `REJECTED_V1` -- this construction is frozen; see the linked record for what would reopen it. The claim is narrow: no executable edge for THIS one-sided construction under the tested regime, which is not a claim about maker strategies in general

