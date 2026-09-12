# Alpha 4 — "medium-horizon order flow" — source verification

Status: **VERIFIED_ABSTRACT_ONLY / NOT_REPLICABLE_HERE**

The ranked plan listed this lane as *medium-horizon order flow (Anastasopoulos et al., J
Financial Markets 2026)*, sitting below cross-crypto and above funding/basis. Before writing any
code the paper itself was checked, and it is not the paper the plan assumes it is.

## What the paper actually is

Alexia Anastasopoulos, Nikola Gradojevic, Fred Liu, Alex Maynard, Ilias Tsiakas,
*Order Flow and Cryptocurrency Returns*, **Journal of Financial Markets** (article
S1386418126000029); SSRN 5020002, posted 13 December 2024.

Abstract, as published on SSRN:

> We assess the information content of order flow for the cross-section of cryptocurrency
> returns. Our analysis is based on a set of international order flows denominated in 11 major
> currencies that reflect world order flow. We find that world order flow has strong explanatory
> and predictive power for cryptocurrency returns. Order flow tends to dominate economic
> fundamentals for out-of-sample prediction, especially in the context of non-linear machine
> learning models, and its performance cannot be explained by limits to arbitrage. Overall, our
> findings indicate that order flow has a permanent effect for cryptocurrency returns.

Keywords: Machine Learning, Forecasting, Portfolio Sorts, Cryptocurrencies, Order Flow.
JEL: C58, G11, G15.

## Why this closes the lane before any code

**"Order flow" here means foreign-exchange customer order flow, not exchange limit-order-book
flow.** The predictor is a set of international order flows denominated in 11 major currencies —
the Evans–Lyons FX microstructure construct — aggregated into a "world order flow" factor. It is
not signed trade imbalance on a crypto venue, which is the thing this desk can measure and the
thing the plan had in mind.

Three consequences, in order of how much they matter:

1. **The input is not obtainable.** FX customer order flow is proprietary dealer data. There is
   no public archive of it, and nothing in the Binance or Kalshi feeds is a substitute. A
   replication cannot be attempted at any horizon.
2. **The horizon is wrong for this desk.** "Portfolio sorts" and "economic fundamentals" place
   this at the daily-or-lower frequency of the asset-pricing literature, not at the minutes-to-
   hours horizon the plan called medium. The cost hurdle work in this project applies to the
   latter.
3. **The abstract makes no post-cost claim.** Unlike the two cross-crypto papers, which both
   assert profitability net of transaction costs, this one claims explanatory and predictive
   power only. Even with the data it would enter the same discovery-threshold screen that
   `quarter_hour` failed.

## What was and was not verified

Verified: title, full author list, journal, SSRN identifier, posting date, and the abstract
verbatim.

**Not** verified: the data vendor, exact frequency, sample period, out-of-sample R-squared, and
whether any trading strategy in the paper deducts transaction costs. The publisher page returned
a 400, and the two open-access copies (the EFMA 2025 conference PDF and the author's University
of Guelph thesis, *Three Essays on Order Flow and Cryptocurrency Returns*) are both disallowed to
this fetcher. Those claims are therefore stated as inference from the abstract and the keywords,
not as read from the method section. If the lane is ever reopened, the thesis is open access and
a human can read it directly.

## Recommendation

Mark Alpha 4 `NOT_REPLICABLE_DATA` rather than `NOT_REPLICATED`. The distinction matters: this
is not an untested hypothesis waiting for effort, it is a hypothesis whose input this desk cannot
obtain. Nothing about it should be read as evidence for or against exchange-level order-flow
imbalance as a signal — that is a different construct which this paper does not test and which
this note does not close.

## Sources

- https://papers.ssrn.com/sol3/papers.cfm?abstract_id=5020002
- https://www.sciencedirect.com/science/article/pii/S1386418126000029
- https://www.efmaefm.org/0EFMAMEETINGS/EFMA%20ANNUAL%20MEETINGS/2025-Greece/papers/OrderFlowpaper.pdf
- https://atrium.lib.uoguelph.ca/items/803d3c13-a2cd-4600-8b3a-a48648a6746a
