# Strategy routing and the next models

*Research and proposals. For the routing rule as it is actually implemented -- the four
verdicts, the per-category routes and the two edge floors -- see `docs/routing.md`.*

Public-source research: **2026-09-09, approximately 11:53–11:57 UTC**; implementation notes refreshed against the later September 9 changes. This document separates observed capabilities from proposed work. It does not claim that a displayed model edge, a recent win, or a strategy described below is profitable. The public market observations below are a dated sample, not current trade recommendations.

## What should belong to each workflow

The manual board and the systematic trader should share market data, model explanations, the order preview, and account risk checks. They should have different ownership: a manual click creates a manual position; a systematic entry creates a position whose named algorithm owns its exit policy. Starting an algorithm must not adopt an unrelated manual position. A manual take-profit/stop bracket should require an explicit choice in the ticket. Closing or reducing an existing position is a separate action from opening the opposite team as new risk.

Use a configured category/strategy allowlist for automation. A row is eligible only if its model is implemented, its settlement target is identified, its input and book timestamps are fresh, its model diagnostics pass, and its fee-adjusted executable price satisfies the configured thresholds. Being visible on the board, having a high win probability, or having a large raw model gap is insufficient. Manual review may accept an explicitly labeled research estimate; that is different from saying the estimate has been calibrated for unattended trading.

One event belongs to one entry decision while it has exposure or working orders. A Wisconsin entry should suppress both an ordinary Notre Dame entry and another Wisconsin suggestion. A same-side addition needs the configured higher edge threshold and cooldown. A deliberate hedge belongs in a separate preview showing the payoff of the whole event, since buying the other team does not invariably improve the position after its cost and fees.

For the board's today/tomorrow scope, use the configured local calendar dates and the actual expected outcome/expiration fields. Also display market close, expected expiration, and latest expiration separately. A rolling 48-hour filter and a today/tomorrow calendar filter are different. No known expected outcome time means unknown eligibility, not an automatic pass.

## Actual 15-minute commodity markets exist

The public [Kalshi commodity series API](https://external-api.kalshi.com/trade-api/v2/series?category=Commodities) returned 157 commodity series. Seven had `frequency=fifteen_min`:

- `KXGOLD15M`: Gold, sourced from Pyth Gold.
- `KXSILVER15M`: Silver, sourced from Pyth Silver.
- `KXCOPPER15M`: Copper, sourced from Pyth Copper.
- `KXNATGAS15M`: Natural Gas, sourced from Pyth NATGAS.
- `KXWTI15M`: WTI, sourced from Pyth WTI/PYTHOIL.
- `KXPLATINUM15M`: Platinum, sourced from Pyth Platinum.
- `KXPALLADIUM15M`: Palladium, sourced from Pyth Palladium.

These are discovered series, not guessed ticker names. A series listing does not by itself establish that a particular instance has an executable book. The [series API documentation](https://docs.kalshi.com/api-reference/market/get-series-list) and [market API documentation](https://docs.kalshi.com/api-reference/market/get-markets) describe discovery and market status queries.

Public open-market samples for `KXGOLD15M-26SEP090800-00`, `KXSILVER15M-26SEP090800-00`, and `KXNATGAS15M-26SEP090800-00` opened at **11:45 UTC** and closed at **12:00 UTC**. Each reported **17:45 UTC expected expiration**, several hours after its 15-minute trading window. The NATGAS instance reported a latest expiration of September 16 at 12:00 UTC. Its YES criterion compared the relevant one-minute Pyth candle closes at the beginning and end of the interval, including equality. These observations came from the public [NATGAS market endpoint](https://external-api.kalshi.com/trade-api/v2/markets/KXNATGAS15M-26SEP090800-00) and equivalent gold/silver market endpoints. Other quote requests returned HTTP 429, so this research did not establish current book quality for every series.

**A 15-minute holding target is not a 15-minute settlement promise.** Selling before close requires a counterparty. Waiting for the binary outcome can tie up cash until the exchange pays out. The [commodity contract terms](https://assets.kalshi.com/contract_terms/COMMODITIES.pdf) distinguish the observation period, expiration, and settlement and include delayed-publication contingencies. The particular market's source, comparison operator, units, and timing must control the model.

### Commodity model proposal

`CommodityFutures` currently returns `std::nullopt` in `include/desk/strategies.h`; the registered strategy name is a placeholder, not a working commodity probability model. Do not relabel a generic front-month futures model as a model of a specific Pyth candle close.

Implement a separate `commodity_interval` model, leaving the existing 15-minute crypto implementation unchanged:

1. Resolve each market's actual Pyth underlying, target candle, threshold, equality rule, and tick schedule. Source identifiers such as a Pyth index and a spot-metal symbol are not interchangeable aliases without verification.
2. Record timestamped underlying prices and the Kalshi bid/ask plus depth. Kalshi documents an authenticated [Pyth WebSocket feed](https://docs.kalshi.com/websockets/pyth-value) carrying source and receive timestamps; its underlying-list message is useful for checking available symbols. Public REST market discovery does not grant access to this authenticated feed. Direct Pyth integration is another option; current [Pyth documentation](https://docs.pyth.network/price-feeds/core/use-real-time-data) notes an API-key requirement for Hermes following its August 26, 2026 upgrade.
3. Estimate the distribution of the remaining interval return using only prices available at the decision time. Fit intraday volatility, account for the market session and scheduled releases, and compare a simple empirical conditional-return distribution against a normal baseline. A model of a candle close needs the candle's exact construction rather than a last-price shortcut.
4. Convert that distribution into YES/NO probabilities, then price entry using the actual offer, current series fees, the uncertainty allowance, and a conservative exit/slippage assumption. A large gap must trigger a source/time/threshold diagnostic before eligibility.
5. Replay held-out days with measured data delay and executable order-book prices. Measure fills, adverse movement after fills, fees, cash tie-up, calibration, and net results by commodity and time of day. A paper fill at the mid is not evidence that a live order would fill there.

Start as research/manual review; allow unattended execution only for a specific version and source mapping that passes the chosen validation criteria. High C++ throughput does not replace input quality or liquidity.

## Sports: model assistance now, separate swing research

`SportsLive::default_leagues()` currently maps NFL, college football, NBA, WNBA, men's and women's college basketball, MLB, and NHL. Soccer uses the separate `SportsInplay` implementation. A sport or league absent from these maps has no automatic coverage merely because ESPN lists it.

The current `sports_live` source code chooses, in order:

1. An ESPN win-probability chart value during a game that passes the implemented scoreboard/summary state and play-ID checks.
2. A Gaussian score/remaining-time model for the configured football and basketball leagues when ESPN probability is unavailable.
3. A pregame spread converted to a probability when a spread exists.

MLB and NHL deliberately lack the score/time fallback. The code stops producing a fair when the scoreboard is over 120 seconds old, does not price completed games, and requests the scoreboard and probability summary on different schedules. A Kalshi-derived pregame prior is not independent evidence of edge against Kalshi.

The public [ESPN MLB scoreboard for September 8](https://site.api.espn.com/apis/site/v2/sports/baseball/mlb/scoreboard?dates=20260908&limit=30) returned 15 games. A sampled [Cleveland–Baltimore summary](https://site.api.espn.com/apis/site/v2/sports/baseball/mlb/summary?event=401816854) exposed a `winprobability` array with `playId` values and separate plays carrying clock/state information. This confirms an available source shape, not a guaranteed service level or proof that the latest chart point agrees with the current scoreboard.

The September 9 correction adds `SportsLive::accept_win_probability`. It requires an in-play scoreboard updated within 120 seconds, a matching ESPN event ID, and exact agreement between scoreboard and summary on game state, period, displayed clock, and both scores. The last chart point must identify a play: it must match the scoreboard's `situation.lastPlay.id` when supplied and the final summary play's ID when a plays array is supplied. Missing linkage or disagreement fails the gate. The accepted probability is tied to a signature including state, period, scores, clock, and scoreboard last-play ID; `fair()` stops using it when that signature changes. A rejected summary clears the cached probability. These checks prevent a successful download alone from presenting a chart point as current.

The metadata now distinguishes `source`, `source_ts`, `state_signature`, and `auto_eligible`. An aligned ESPN probability is marked `auto_eligible=true`; the score/time and pregame-spread approximations are explicitly `research_only=true` and `auto_eligible=false`. The systematic selection still must pass its other configured data, model, price, and risk checks. This metadata is not evidence of profitability. Exact matching can exclude useful but delayed chart points, including points before a non-scoring end-of-period marker. The gate uses the summary array's final play; validating that ordering across sports and recording mismatches remains part of feed QA. Source timestamps represent validated fetch time, not a new guarantee of ESPN publication latency.

The Gaussian fallback is not a model of possession, red cards, fouling, timeouts, or overtime transitions. Overtime is currently represented as a short remaining fraction and a tie is simplified. An overtime jump is a research hypothesis, not automatically an underpriced tail. A better basketball model should condition on possession, score margin, timeouts, foul state, remaining clock, team strength, and an explicit overtime state; replay late-game and overtime segments separately.

### The underdog 9-cent to 18-cent idea

Buying at 9 cents and later selling at an executable 18-cent bid doubles the contract price before fees. It does not require that team to win. It also does not show that buying 9-cent teams repeatedly is profitable: some never reach 18 cents, the next tradable bid can fall sharply, and spread/fees reduce the return.

Treat this as a **short-horizon price-move strategy**, separate from the model of who wins the game. Record every qualifying entry opportunity, including losses and unfilled orders. Test the probability of hitting the profit bid before the stop or time limit, not just the settlement win rate. Require a valid game state, executable depth, and a prespecified exit horizon. Use exact game replay to compare the 18-cent target with smaller targets and time-based exits without selecting the winning target after observing the same games.

A high minimum settlement win-probability filter will intentionally reject a 9-cent underdog. It should not be silently bypassed. A manual sports-swing ticket should identify its separate objective and display both estimated final win probability and the unvalidated price-move hypothesis. Estimated contract P&L at a target is quantity × (target bid − entry cost per contract − exit fee); a target order does not guarantee execution.

## Weather automation

The repository already has an implemented NWS weather model, station observations, forecasts, provisional station-to-settlement adjustments, and structured data-quality metadata. The [NWS API](https://www.weather.gov/documentation/services-web-api) provides forecasts and observations; obtaining that data is the input stage, not evidence that the model beats exchange prices.

Keep automatic weather entries limited to verified station/date/product mappings with fresh observations, a complete local-day window where required, current forecasts, a sane ensemble comparison, and healthy quotes. Preserve the source warning for contracts settled by The Weather Company when the model uses NWS: the repository's one-date agreement check supports further testing but cannot establish equivalence on all days. `same_day_trading` or `allow_research_estimates` must not turn failed input/settlement checks into a pass.

Balance edge and winning probability explicitly: evaluate the probability of the selected side, subtract fees from its executable purchase price, apply uncertainty, then cap exposure to correlated city/date outcomes. A cheap YES with a tiny chance of winning can have positive estimated value and still violate the user's preference. Conversely, a 90% chance is a losing proposition if the all-in price exceeds its value.

Compare hold-to-settlement, model-value exit, profit-target, and short time-limit policies on the same held-out station-days. Weather observations can revise the distribution abruptly; a fixed 15-minute exit is not automatically suitable. Current systematic code already has grace/persistence for market stops, separate model-thesis exits, and maker/taker exit logic. Validate each against real depth instead of broadly widening stops to make the historical loss count look smaller.

## Economics, financials, technology, and science

`EconNowcast`, `IndexClose`, and `SciTech` remain placeholder classes returning no fair. They may be listed for manual browsing with rules and live quotes, but a missing model must display as missing rather than a synthetic 50% estimate with an apparent edge.

For economics, begin with a release calendar and specific series models. As of this research, the [BLS September 2026 calendar](https://www.bls.gov/schedule/2026/09_sched.htm) lists Employer Costs for Employee Compensation on September 9 at 10:00 ET, PPI on September 10 at 08:30 ET, and CPI on September 11 at 08:30 ET. These are release times, not proof that matching Kalshi contracts exist or will settle that day. Re-check the release calendar and each market's exact statistic, reference month, rounding, and revision rule. Use [BEA's release calendar](https://www.bea.gov/news/schedule) for its statistics. A CPI nowcast is not a PPI model.

For financial closes, use the exact specified index/reference feed and closing convention with an intraday distribution; a correlated ETF or futures quote creates basis risk. For technology/science events, use the named primary source and event-specific rules. A reliable event-date filter is useful even when no defensible probability model exists. Do not force these categories to populate a fixed quota of 20 apparent opportunities.

## What a $40 algorithm allocation and a 70% loss stop mean

The implemented `capital_budget_dollars=40` allocation applies to **SystematicTrader only**. It does not cap the separate perpetuals algorithm, Kalshi/Polymarket pair strategy, or manual trades; those retain their own ownership and budgets as well as applicable account-wide checks. It is not a $40 global dashboard spending limit and does not transfer or segregate exchange cash.

`systematic_capital()` includes that algorithm's working entries and filled positions in committed capital, accounts for realized losses and capital released to manual ownership, and does not expand the cap when it makes a profit. Daily purchase turnover remains a separate budget because repeatedly recycling positions can consume the allocation multiple times over a day. Display allocation, committed capital, available allocation, realized P&L, and marked open P&L separately; aggregate risk still needs to cover simultaneous activity in the other algorithms.

A 70% drawdown of $40 is a **$28 loss**, leaving $12; returning to $40 would then require approximately **233% growth**. That arithmetic is not a recommendation to use such a limit. Treat the threshold as a final capital stop, distinct from a per-trade exit or the normal daily loss budget. A low loss frequency achieved by letting losses become much larger is not an improvement in expected returns.

The implementation now distinguishes two per-position modes. In `stop_loss_mode="price"`, `stop_loss=0.10` is a **10-cent contract-price distance**; setting that field to `0.70` would still mean 70 cents. In `stop_loss_mode="fraction"`, `stop_loss_fraction=0.70` sets a **70% loss threshold relative to the position's entry cost per contract**, including entry costs, and the market comparison accounts for an estimated exit fee. The 50-cent-to-15-cent example is the approximation before fees. This percentage mode is a per-position exit rule, not a 70% drawdown stop on the whole $40 allocation. The algorithm's separate dollar-denominated daily loss stop remains distinct. Grace/persistence and available liquidity affect the actual exit; no stop setting guarantees its execution price.

## Current implementation and remaining sequence

The September 10 build includes separate manual and algorithm quantities, manual-only bracket
sizing even in mixed positions, event-wide suppression on the main board, today/tomorrow category
filters, the $40 allocation control, and a previewable 70% per-position stop preset. Normal
dashboard and Ticket entries share one guarded review dialog. Applying the preset and starting
an algorithm are separate actions; these code changes do not change the running service's saved
settings. Perps and Polymarket pairs retain their separate budgets. Unknown model values remain
unknown, and fallback sports estimates cannot become automatic entries just by allowing research.

The production binary and mock tests do not validate profitability. Follow-up work is:

1. Verify the new ownership and routing behavior during an observed restart, including existing
   saved orders. Manual brackets defer when unrelated orders are resting; they do not cancel those
   orders to make room. A released algorithm position becomes manual rather than an invented
   closed-trade win/loss.
2. Verify the implemented ESPN chart/state/play gate on recorded examples from each supported sport and surface mismatch diagnostics. Preserve the manual-only status of fallback approximations while improving the sports preview.
3. Add the separate Pyth commodity-interval recorder and model; test source mapping, candle rules, rate limiting, and expiration timing. Preserve 15-minute crypto behavior.
4. Validate weather versions per station and local hour with actual settlement outcomes, then tune execution and exit rules on held-out data. Save parameter changes with a version and evaluate subsequent performance separately.
5. Add individual economics/financial models as their inputs and settlement conventions are verified. Keep technology/science manual until an event-specific model exists.
6. Promote each version on evidence: probability calibration, executable net results including fees, fill rate, adverse movement after fills, maximum drawdown, capital tied up, and failures during feed outages. Split validation by event/day to avoid treating repeated quotes from one outcome as independent wins.

Cross-venue work remains a separate strategy: contracts must have equivalent payoff and settlement rules before complementary YES/NO purchases are treated as an arbitrage candidate. Related but nonidentical events are statistical trades with basis risk, and two venues cannot promise atomic fills. Shared infrastructure does not make those risks disappear.
