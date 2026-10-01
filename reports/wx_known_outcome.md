# Weather known-outcome lag: brackets the running maximum has already decided

Generated 2026-09-24 00:21 UTC by `research/wx_known_outcome.py` from the desk's recordings. Rules: a bracket with upper edge `cap` is decided NO once the recorded running maximum is at least cap + 1 + 0.5F; an "above" bracket with floor `f` is decided YES once it is at least f + 1 + 0.5F. The running maximum is a lower bound on the settlement value, so these are the two things it can decide.

## 1. Safety

163 bracket-days decided NO by the rule; 0 of them settled YES.

## 2. The YES bid after the outcome was known

| minutes after the desk saw it | n with a quote | median YES bid | share with a bid >= 0.02 | share >= 0.05 | mean bid |
|---|---|---|---|---|---|
| 0 | 163 | 0.00 | 1% | 1% | 0.000 |
| 5 | 163 | 0.00 | 1% | 0% | 0.000 |
| 15 | 162 | 0.00 | 1% | 0% | 0.000 |
| 60 | 150 | 0.00 | 0% | 0% | 0.000 |

By series, the YES bid when the desk first saw the bracket decided:

| series | n | median bid | max bid |
|---|---|---|---|
| KXHIGHAUS | 20 | 0.00 | 0.00 |
| KXHIGHCHI | 15 | 0.00 | 0.05 |
| KXHIGHDEN | 8 | 0.00 | 0.00 |
| KXHIGHLAX | 36 | 0.00 | 0.00 |
| KXHIGHMIA | 27 | 0.00 | 0.00 |
| KXHIGHNY | 2 | 0.00 | 0.00 |
| KXHIGHPHIL | 21 | 0.00 | 0.01 |
| KXHIGHTATL | 2 | 0.00 | 0.00 |
| KXHIGHTBOS | 6 | 0.00 | 0.00 |
| KXHIGHTDAL | 5 | 0.00 | 0.00 |
| KXHIGHTDC | 4 | 0.00 | 0.00 |
| KXHIGHTHOU | 9 | 0.00 | 0.00 |
| KXHIGHTNOLA | 8 | 0.00 | 0.00 |

## 3. The "above" brackets the running max decides YES

2 bracket-days decided YES; 0 of them settled NO.

| minutes after the desk saw it | n with a quote | median YES ask | share with an ask <= 0.97 | mean ask |
|---|---|---|---|---|
| 0 | 2 | 1.00 | 0% | 1.000 |
| 5 | 2 | 1.00 | 0% | 1.000 |
| 15 | 2 | 1.00 | 0% | 1.000 |
| 60 | 2 | 1.00 | 0% | 1.000 |

Buying YES at the ask when the desk first saw it: 0 with an ask under $1, netting $0.00 in total after the taker fee on the ones that paid anything (0 of them).

**Reading (both directions).** 2 decided-NO brackets still showed a YES bid above zero when the desk first saw the running max cross; selling YES into those bids nets $0.04 in total per contract-set after the taker fee ($0.020 each). The rule was never wrong on this sample. A bid of one cent is the book's floor and pays nothing after the fee; the question is how often it is more than that, which the table answers. These are one contract each: depth is not recorded here.
