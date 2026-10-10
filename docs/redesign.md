# Desk redesign v2: Systematic and Manual on Kalshi and Polymarket US

A plan only, built from the 2026-10-05 reports and the v1 review; nothing was traded.

## The short version

- **Two modes, one gateway.** Systematic: press Start and it trades today's plan. Manual: click a live order book and the order goes out at once. Both share one risk check and one fill ledger. Perps and research-only modes are cut.
- **Execution comes first.** A click takes ~15 s to reach Kalshi because it waits for the engine's 8-9 s loop. A gateway outside the loop should take under 150 ms, and every order gets an exchange-enforced deadline.
- **Start trades something within about three weeks:** arbitrage (rare) and, as an experiment, 15-minute commodities at minimum size with resting orders.
- **"The slightest edge plus volume" fails by itself.** Volume multiplies (edge minus cost), and a Kalshi taker at 50c pays 2.25c a contract, 4.5% of the stake. So the desk rests orders by default.
- **Recording restarts this week.** The desk has been down since 09-28, with about 4 days of disk headroom.
- **Sports:** ESPN vs Kalshi lost money in the desk's own test. Your friend's approach starts in November as a fair-value line on college basketball that you trade by hand.
- **Polymarket** is blocked by the API key, not the code.
- **The stops must fit the plan.** At zero edge, v1's $12 daily stop would fire on about 45% of days.

## Phase 0: this week, your actions, no code

The disk has 24 GiB free against a 10 GB floor, and the book stream writes about 3 GB a day.

1. **Free disk.** Move data/raw (11 GB, mostly old perps data) to an external drive. Keep data/archive (0.9 GB of commodity book data for the S4 replay).
2. **Archive and extract first.** Both need the desk stopped (it holds the database lock). Archive the commodity book data with scripts/archive_quotes.py and take the 15-minute extract for S4.
3. **Restart record-only.** Kill switch on, Start not pressed, the legacy AUTO switch off. Any build: one at a time, -j 4 at most, and never into build/ while the desk runs. Set commodity_book_retain_days to about 3, so prod.sqlite reuses space instead of growing. Check the log: rows written, no orders.
4. **Perps.** Close the contract and move the $29.19 back (decision 7).
5. **Polymarket.** Regenerate the key (decision 4). "API key not found" means no key has that ID: a key worked mid-month (the desk read $98.70 on 09-17), then was probably revoked in the secret rotation.
6. **Kalshi:** get a demo key, to test that a repeated client_order_id is rejected and how short an expiry is accepted, and run the free Advanced-tier upgrade.
7. **Clock sync** on: Kalshi expiries are absolute Unix seconds.

## What we're building

### Two modes

In Systematic the engine picks market, side, size and price from the lanes you switched on; in Manual you pick. **The line is who decided to take the risk.** Kalshi nets one position per market. So a manual order in a market where Systematic holds inventory or resting orders is blocked unless you confirm it; on confirm, Systematic's orders there are cancelled and the position becomes yours.

- **Start** checks keys, cash per venue and per Kalshi shard (Kalshi's separate matching engines, each holding its own cash), clock, disk, and today's plan with its chance of hitting the daily stop, then trades.
- **Stop** cancels resting systematic orders; exits keep running.
- **Kill** cancels everything on both venues, with no re-arm for 60 s.

### Orders and the gateway

| Button | Kalshi order | If unfilled |
|---|---|---|
| Take | Immediate-or-cancel (IOC: fills what it can now, cancels the rest) at the touch (best opposing price) plus a slippage cap | The exchange cancels the rest at once |
| Post 15 s | Post-only (it can only rest), expiring at now + 15 s | The exchange expires it |

A limit far from the touch is blocked: most unfilled manual takers were mispriced (0.57 against an ask of 0.77), not just late.

The gateway is a thread outside the engine loop. It checks risk in memory, signs, sends, and returns Kalshi's fill result in one round trip. Click-to-ack, from your click to the exchange's answer, should be well under 150 ms.
- **Kalshi** keeps the connection open only after the demo shows duplicate orders are rejected.
- **Polymarket** writes always use a fresh connection. A reused one can silently re-send a POST, and Polymarket orders carry no client id to catch it.
- **The existing safety guards are ported, not rewritten.**

### The order-book screen
- **One market, full width,** pushed to the browser with server-sent events (SSE) instead of a 2.5 s poll.
- **"Stale" means the feed is broken, not quiet:** the socket dropped, a sequence number was skipped or the heartbeat stopped. Thin college books go 57-90 s between trades.
- **Overlays:** the break-even band, our orders with countdown and queue position, and fair value (ESPN labelled "~40 s behind, measured on NBA/MLB"; the S5 model from November).
- **Shard cash on the ticket.** If a shard is short, the gateway first moves cash from the manual reserve; Kalshi stopped doing this for API orders on Oct 2.

### The central risk book
One row per holding, grouped by real-world outcome, with cash per venue and shard and its own fill ledger (direction, owner, transfers). Kalshi's private fill, order and position channels update it by push; a REST reconcile (re-reading everything from the exchange) is the backstop.
- **Reserve before sending:** each order's worst case is reserved atomically when sent and released on fill, cancel or expiry, so two quick clicks can't both slip under a cap.
- **Fail closed:** after a socket drop, sequence gap or missed heartbeat, no new orders until a REST reconcile. Cancels and Kill still work.
- **Caps on worst-case loss, before the trade:** ~10% of venue cash per event (~$11-14 on Kalshi) and $15 net per outcome across venues. That is the "minor hedging"; an IOC flatten (one taker leg, ~2.1c) only mops up overshoot from simultaneous fills.
- **Polymarket** is read-only, its rows "unknown", until the key works. A cross-venue pair counts as hedged only if its settlement rules match.

## Roadmap

| Phase | Delivers | Done when |
|---|---|---|
| 0. This week (you) | Disk, extract, record-only desk, perps closed, keys, clock | Rows flowing; Kalshi cash ≈ $142 |
| 1a. ~1 week | Kalshi gateway; Take and Post; one-market screen with SSE; click-to-ack logging; shard cash; dead code deleted | Click-to-ack p50 under 150 ms over 50+ minimum-size orders; no order alive 5 s past its deadline |
| 1b. ~2 weeks | Private websockets; risk book; Start/Stop/Kill; lanes S1 and S4; markouts; daily recorder files; the cuts | Start trades S4 by itself; a dropped socket blocks orders (tested); Stop clears in under 2 s |
| 2. ~2 weeks, late Oct | S2 backtest, then live if it passes; the ESPN check; S5 overlay; capital plan; scorecard | Scorecard fills daily; overlay ready for college basketball |
| 3. Once the key works | Polymarket orders, websockets, positions, fee fix; S3; two-venue weather | Polymarket fills by push |

**Recording from 1b.** Each day goes to its own file (data/rec/YYYY-MM-DD.sqlite), compressed when closed. prod.sqlite keeps one writer, and backtests read closed files without a lock. Within a GB-per-day budget, it records:
- best bid/ask per watched series, never the all-markets firehose;
- the full book for the S4 metals and a sample of games;
- game state from Kalshi and ESPN (polled every 3-5 s), with first-seen times.

## Lanes

| Lane | Evidence | Orders | Volume | Starts |
|---|---|---|---|---|
| S1 Arbitrage | Pays when it fills; 9 of 703 events ever net-positive | IOC legs | Low | 1b |
| S4 15-minute commodities | None positive | Resting only | Medium, capped | 1b |
| S2 Late favorites | Weak | Taker at the ask | Medium on game days | 2, if it passes |
| S5 College basketball model | Weak to negative | Overlay; you click | Yours | November |
| S3 Polymarket pre-game quoter | Rebate is real; fills unmeasured | Post-only with expiry | Low or unknown | 3 |

**S1** has no standing budget; it draws free cash when a lock (a basket that pays more than it costs in every outcome) appears. Two-venue weather baskets (Phase 3) are capped at a few dollars: the venues use different weather sources (they agreed on 30 of 30 days), and one 1°F miss breaks a lock.

**S4** trades gold, WTI and silver, never crypto. A logistic model adds one drift term, of either sign, from 2-4 features fixed in advance; so far consecutive windows lean slightly toward reversal.
- It enters in the first 3 minutes only, with $2-4 clips, $6 per window across the correlated metals and a $15 lane loss cap.
- A backtest can't settle it (a 52.5% hit rate needs ~3,100 windows per series; we have ~1,414 across five). Its real output is the markout of resting fills on a fee-free series: how far the mid moves our way 10 and 60 s after each fill.

**S2** buys favorites at 85-97c late in games. Backtest: Kalshi's public trades, filled at the ask plus fee (0.83c at 95c).
- **Only live-knowable features:** period and clock, or inning and outs. Never minutes to the actual close, since game markets close once the result is known. Results are split by YES vs NO side and by league, with a sample-size target per league.
- **The outside evidence is weak.** Becker's "1c YES −41%, 1c NO +23%" is pooled and mostly a YES-vs-NO effect: it implies 99c YES lost money. Bürgi's positive returns above 70c come from markets open 24+ hours.
- **A resting-bid version is a later lane:** a bid on a late favorite fills mainly on comeback news.

**S5** is a logistic model per league on Kalshi's pregame price plus live score and clock. XGBoost is added only if it beats the logistic on held-out games.
- **Fees:** women's college basketball charges makers nothing, men's 0.44c at 50c. FCS football is dropped: it trades inside KXNCAAFGAME, which charges makers.
- **Expect a tie at best.** In 1,438 Kalshi NBA games a simple model matched the live price, and taking the gap lost money. Any edge is you watching a thin game live, ahead of bots on a 40 s feed.

**S3** earns a 0.31c rebate at 50c, rounded per fill, so $0 on single contracts. Behind 100k+ contracts on liquid games, its quotes fill mostly when price runs through them. It quotes only where Polymarket's spread exceeds one tick, under the $15 pre-trade cap, and retires if, after 200 fills, its 60 s markout loses more than the rebate earns.

**Taking liquidity** is allowed only for S1 legs, and for S2 once it passes. S4's noise "edges" will often top 2.25c.

## The honest numbers

### Fees, and "the slightest edge plus volume"
Volume multiplies (edge minus cost); it never makes an edge below cost pay. Cost at zero edge, with $240 deployed once a day and held to settlement:

| How | Cost per contract | % of stake | Per day | Account halves in |
|---|---|---|---|---|
| Kalshi taker, 50c | 2.25c | 4.5% | −$10.80 | ~15 trading days |
| Kalshi taker, 95c | 0.83c | 0.9% | −$2.10 | ~79 |
| Kalshi round trip, 50c | 4.50c | 9% | −$21.60 | ~7 |
| Polymarket taker, 50c | 1.86c | 3.7% | −$8.90 | ~18 |
| Resting order, fee-free series | adverse selection − half spread | unmeasured | ? | ? |

As a taker at 50c, with 100 independent bets a day:

| Edge per contract | Mean per day | Chance of a positive month |
|---|---|---|
| 0c | −$10.80 | 2% |
| 1c | −$6.00 | 13% |
| 2c | −$1.20 | 41% |
| 3c | +$3.60 | 75% |
| 5c | +$13.20 | 99% |

More bets shrink the noise, not the mean, and confirming a 1c edge takes about 10,000 outcomes. So the plan drives cost toward zero.

### ESPN vs Kalshi (corrected)
- **The desk's test lost $1.23 a trade** (n=106, t −1.83: suggestive, not significant at 5%).
- **At gaps of 30+ points (40 games), outcomes sided with Kalshi both ways.** Where ESPN was higher: ESPN 72%, Kalshi 33%, the team won 22%. Where Kalshi was higher: Kalshi 84%, ESPN 44%, the team won 93%. Kalshi was right but under-confident.
- **ESPN ran a median 41.5 s late**, measured on 3 games (NBA preseason and MLB, no football).
- **Use:** on screen in Manual. One cheap check on the existing ~203-game extract: take Kalshi's side against stale ESPN at 20-30+ point gaps. Weak evidence (~40 games).

### Your friend's resume
- 27,000 contracts × 3-6c is $340-1,320 after fees, so 38.9% implies $0.9k-3.4k of capital. Read per contract instead, it needs ~12c of edge, 2-4× his stated 3-6 points.
- About 1,000-2,000 bets leave it 1-3 standard deviations from zero, with no period or drawdown. "15-20% better calibration" contradicts the NBA result, where a good model only ties the market.
- **Copy his process, not his number:** thin college books, resting orders, a model pulled toward the price, small size. Ask him for the period, capital and drawdown.

### The 18-ETF momentum fund
It can't be verified, and one year proves little. Momentum works over months, earns on the size of moves (binaries pay on direction) and costs basis points, not 4.5% of the stake. What transfers: a few fixed rules, equal-risk sizing over many small bets, the market price as benchmark.

## Capital, rails and measuring

Kalshi $113.20, perps $29.19 and Polymarket $98.70 (last read 09-17) make $241.09. All-time Kalshi P&L is +$8.20, almost all from your own tickets.

- **A daily cost budget** of about $4.80 (fees plus spread paid) stops taking when spent.
- **Every fill** gets a 10 s and 60 s markout; negative means we were picked off.
- **A daily scorecard per lane** shows fill rate, markout, edge against break-even, costs and P&L. Lanes **scale or retire on markouts, in small steps.** v1's 300-outcome rule is too weak: a zero-edge lane passes it 22% of the time, and a real 3c lane fails it 40%.

## Cuts, and what stays

**Cut:** perps, research and paper modes, the research tabs and strategies, the legacy auto-sender, the manual queue, ten overlapping caps and dead code: about 5,000 lines of src plus 27,000 in research/ and scripts/.

**Kept in tools/:** the Kalshi history backfill, the lock-checked extract helper, the commodity replay and the weather settlement watch. **Order:** dead code first, perps once the cash shows on Kalshi, research/ and scripts/ last.

## Decisions only you can make

**1. Loss stops.** On a stop, Systematic halts and Manual can only reduce risk until the next day. Chance per day of hitting a daily stop at zero edge, before costs, for the Phase 2 plan (about 30 S4 fills of $3 plus a few favorites; daily standard deviation ~$16). In brackets: with $60 of Polymarket quote inventory added in Phase 3 (~$34).
- $12 (v1): ~45% [~72%]
- $20: ~21% [~56%]
- $25: ~12% [~46%]
- $40: ~1% [~24%]
- **Longer stops:** a $30 weekly stop fires in ~40% of weeks. Over a month of daily trading, a $170 floor is touched ~33% of the time, $150 ~22%.
- **Recommendation:** $25 a day, no weekly stop, and a $150 floor that needs your restart. Show the live chance next to Start, and shrink S3 before Phase 3.

**2. What "deploy most of the capital" means.** (a) Everything into a few bets. (b) 80% of cash as turnover in $3-5 clips over 30+ outcomes, under the cost budget. (c) A fixed $40 a day.
- **Recommendation:** (b). Five full-size bets a day swing the account ~45% a day.

**3. How lanes start, including S4 (no evidence of edge).** (a) Live at minimum size with a lane loss cap, resting by default (taking only for arbitrage, and S2 after its backtest), scaled on markouts; S4 first, labelled an experiment. (b) The same, without S4. (c) A backtest pass before any live trade. (d) Take freely for volume.
- **Recommendation:** (a). S4 pays no fee and measures what every volume lane depends on. (c) leaves Start idle for weeks; (d) halves the account in ~15 trading days.

**4. The Polymarket key.** (a) Regenerate it now (same sign-in method as the app), test once with the official SDK outside the desk, then send the first live orders at minimum size while you watch. (b) Defer Polymarket until after Phase 2. (c) Drop it and withdraw the ~$99.
- **Recommendation:** (a). There is no retail sandbox, so the first orders are the test.

**5. Separating Manual from Systematic on Kalshi.** (a) One account, with ownership tags, the gateway's block-or-confirm rule, and self-trade prevention set so a crossing order cancels itself, not a resting one. (b) Subaccounts: the exchange enforces the split, but ~$142 becomes six pools of ~$20.
- **Recommendation:** (a), until capital grows.

**6. The sports model.** (a) A fair-value overlay for college basketball from November, made systematic later at minimum size if you want. (b) v1's plan: a systematic lane after Phase 3, behind an accuracy gate. (c) Skip it.
- **Recommendation:** (a): ready for the season, and you watching is the likeliest edge.

**7. Closing perps.** (a) You close it and move the margin back in the web app; the desk's close route stays until the cash shows. (b) The desk closes it. (c) Leave it and cut the code.
- **Recommendation:** (a). (The extra ~$25 on the perps side is Kalshi's promo credit for perps volume, reconciled in Session 19; it is not performance.)
