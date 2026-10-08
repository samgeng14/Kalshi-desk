# Desk rebuild: implementation plan

This is a plan only. I edited nothing in the repo, placed no orders and used no keys. "Est." marks an estimate; everything else comes from the reports or the code.

## 1. In one screen

**What we build.** A new C++ program, `deskd`, replacing today's `kalshi-desk`, which is stopped for good in week 0 so the two never trade one account. Workers pass messages and never hold a lock across a network call. On SYSTEMATIC you press Start and it quotes; on MANUAL a click is acknowledged in about 50 ms (est.). A Python recorder on a free Oracle VM records to Cloudflare R2 around the clock. Orders leave only from the Mac.

**How it can make money (honest version).**

| Source | Evidence | Expect |
|---|---|---|
| Kalshi's Liquidity Incentive Program (LIP: $20 per 15-minute window shared among resting quotes near the best price, filled or not) on seven fee-free non-crypto series | Our share comes from 3 overnight snapshots | **The main volume lever, unmeasured.** At 30-40 a side and that share, $1.2-2 a window per series: $38-64 for an 8-hour supervised day (est.). $0 below a 5% share; 10-lots mostly earn $0 |
| Spread capture as a maker | Tape maker +0.28c, mostly pros; our back-of-queue replay −2.15c | −1c to +0.5c a contract, −$20 to +$15 a day |
| 96-99c maker band | Average maker positive in every group, +0.55c to +1.54c | Small, if our own replay agrees; a rare ~97c loss |
| Thin-college stale quotes | Real but sparse | At most $3-6.5 a game |
| Game pairs across venues | Locks in 0.02% of minutes | About $0; not built |
| Weather pairs across venues | Locks in 17-19% of minute data | A hypothesis; at best $0.5-1 a day |

**Maker rules from the fill simulation** (45,550 virtual orders, September's 15-minute books): joining the back of a 1-tick queue lost 2.15c a fill, worse than taking; improving one tick in 2c+ books lost 0.53c, 78% filled in 15 s. Late fills are toxic (−1.5c after 15-60 s, −3.1c after 60-120 s), so orders rest 5-15 s, never over 60 s. A shown price survives 92-94% of the time at 50-100 ms, 56% at today's 15 s.

**Order of work.** (1) Stop the old desk for good, move the data, fast builds, the `deskd` skeleton; the recorder runs as its own Python track. (2) The gateway and Manual click path, proven on Kalshi's demo account first. (3) Private socket, risk book, Kill, order groups. (4) The maker engine on natural gas at 10 a side: **Start trades by the end of week 3 (Nov 1)**, after a 4-hour soak test. (5) A LIP probe at 30 a side; then the band, arbitrage, Polymarket. (6) College: record now, overlay in November, an IOC lane only if the test passes.

**Words used below.** *Touch*: best bid or ask. *Fee-free*: makers pay no fee; takers still pay about 1.75c a contract at 50c. *IOC*: fill now, cancel the rest. *Markout*: how far the mid moved our way N seconds after a fill. *Shard*: one of Kalshi's matching engines, each with its own cash. *Order group*: an exchange-side bundle with a fill limit and a one-call kill. *UNKNOWN*: an order we can't prove was or wasn't placed. *LIP-15*: our LIP lane (formerly "M1").

## 2. Architecture

### Workers and what each owns

Every piece of state has one owner thread; others get copies by message or read an immutable snapshot. Mutexes live only inside queues, never across I/O, which removes the bug class behind the 09-23 deadlock and the `kill_reason_` race.

| Thread | Owns and does |
|---|---|
| `md-k`, `md-pm` | Books, sequence numbers, Pyth values |
| `priv-k`, `priv-pm` | Parse fills, order updates, positions, group updates for the gateway |
| `gw` (gateway) | Risk book, reservations, cid registry, order states, UNKNOWN table, write buckets, the Systematic-running flag |
| `send-k1` (Manual only), `send-k2..3`, `send-pm1..2` | One pre-opened TLS connection each |
| `send-cx` | Cancels and Kill on its own connections, with its own copy of every order-group id |
| `strat` | Quotes, lanes, models, LIP share estimate |
| `rest`, `poll` | Reconcile, balance every 10-15 s; scores every 3-5 s with first-seen times |
| `store`, `jrnl` | The ops database; the intents journal (plain `write()`, no fsync) |
| `pub`, `web` (16), `ops` | Bounded per-stream SSE queues; HTTP; heartbeat, disk, clock |

**The gateway never blocks.**
- Two inbox lanes: Kill, Stop, cancels and private events go before new orders. Draining a cancel, Stop or Kill also deletes matching new orders still queued, and each `PlaceIntent` carries its market's and lane's cancel count so gw drops stale ones. A click followed at once by Esc never rests.
- Outgoing queues are fixed-size. A full one drops events and marks a gap; the page refetches its snapshot. If `store` falls behind, records wait in the journal and an alert fires.
- `web` waits at most 50 ms for gw, then answers "queued".

**Kill has its own path.** The route sets an atomic flag and wakes `send-cx`, which triggers every group and sends cancel-all without gw. `ops` writes the heartbeat only if gw, `md-k` and `priv-k` each advanced a progress counter in the last 5 s, so the supervisor restarts a hung desk. `deskctl kill`, a command-line tool, does the same if `deskd` hangs.

**Messages** (`bus/msg.h`): `PlaceIntent{cid, owner, venue, ticker, side, price, count, TAKE|POST, ttl_s, slip_ticks, override, cancel_epoch, t_click_ms}`, `AmendIntent{cid, expiry}`, `CancelIntent{cid|market|lane|ALL}`, Kill/Start/Stop, `SendResult`, `OrderEvent{cid, order_id, state, filled, left, avg, reason}`. Prices are integers in 1/10,000 of a dollar; counts in 1/100 of a contract, since Kalshi trades 0.01-contract steps.

### One click, end to end

| Step | Where | Budget |
|---|---|---|
| Paint a grey PEND row | Browser | one frame, 8-17 ms |
| `POST /api/order`, kept-open loopback | Browser → `web` | 0.1 ms (measured) |
| Validate, guard, reserve, register cid, journal, job to `send-k1` | `gw` | under 0.5 ms (est.) |
| Sign RSA-PSS | sender | 0.34 ms (measured) |
| Write; Kalshi answers 201 with fill_count | network | 25-35 ms p50, 60-100 ms p99 |
| Push `ord`; paint WORK, FILL or "IOC 0/10 killed" | `gw` → browser | one frame |
| **Click to acknowledgement** | | **~45-70 ms p50 (est.); pass: p90 under 100 ms** |

Systematic never uses `send-k1` or `send-cx`, so a quoting burst can't make your click pay a fresh TLS handshake (35-95 ms).

### What stays from today's code

| Invariant | Today | In deskd |
|---|---|---|
| Every write carries a `client_order_id` | `executor.cpp:184`, a `uuid4()` | Prefixed `M-` or `S<lane>-`, so ownership survives a crash. An unprefixed open order is foreign: shown, its market blocked until you cancel it |
| 5xx, timeout or no response = UNKNOWN | `executor.cpp:225-251, 676-707`: one global slot (a second overwrites the first), all entries blocked | A table by cid (section 3) |
| A fill during a cancel isn't lost | `reduced_by`, `executor.cpp:351-405` | Raises cumulative filled, not the position |
| Post-only cross and collateral refusals are soft | `include/desk/reject.h` | Plus section 3's soft cases |
| One write per connection, never silently retried | `http.cpp:98-105` | Kept by the spare pool |

Also kept: `auth.cpp`, `fees.cpp`, `ws_client.cpp`, `poly.cpp` (auth, parsers), `reference_feed.cpp` (Pyth, gap rule, replication gate), `models/terminal_price.cpp`, `risk.cpp` `event_payoffs` and `stacks_event` (as pure functions), `ladder.cpp`, `Executor::topup_shards`, the heartbeat with exit 75. `desk_supervise.sh` and the LaunchAgent return, pointed at `deskd`, only once it is the production binary.

### What is never ported

`deskd` leaves out perps, research modes (`research.cpp`, `hypotheses.cpp`, `research/` and its 6 ctest entries), lane stages, the manual queue and the old strategies. All of it goes, with `engine.cpp`, `systematic.cpp` and `server.cpp`, on **one date: Nov 22**. The frozen code gets no other work; week 0 only deletes dead files outside CMake (`crossvenue.*`, `polymarket.*`, `ui/index.html.bak-033631`) and untracks `config/desk.json.bak-20260906`, which holds a key ID.

**New modules:** `bus/`, `md/`, `priv/`, `gw/` (`gateway`, `risk_book`, `unknown`, `kalshi_wire`, `poly_wire`), `net/` (one HTTP/1.1 write per TLS connection, replaced after use or 20 s idle; responses parsed with llhttp, a tested parser; a body cut off mid-way is UNKNOWN, never "failed"), `strat/` (`quoter`, `fair`, `lip`, `lanes/`, `models/`), `store/`, `web/`, `app/deskd.cpp`, `app/deskctl.cpp`, `tools/*.py`, `recorder/`.

**Fast rebuilds.** Touching `store.h` rebuilds 47 files in 4 min 12 s today. We don't split the frozen code's headers; `deskd` starts with small ones. Ninja, ccache and precompiled `json.hpp`/`httplib.h`; set `CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime` and `-Xclang -fno-pch-timestamp`, or ccache misses every time. Target: a strategy edit under 20 s (est.). `--ui-dir` serves `ui/` from disk. Builds use `nice -n 15`, `-j 4` at most, never `build/`.

## 3. Order handling

| Use | Kalshi | Polymarket US |
|---|---|---|
| **Take** | IOC at the touch plus a slippage cap (1 tick default, shown as "fills up to X"); FOK for all-or-none | LIMIT + IOC or FOK |
| **Post** | `post_only` + `good_till_canceled` + `expiration_time` + `cancel_order_on_pause`; a client cancel at TTL + 1.5 s as backup | LIMIT + GTD + `participateDontInitiate` |
| Self-trade | Manual `maker` (your click cancels our quote); Systematic `taker_at_cross` | n/a |
| Flatten | IOC with `reduce_only`, which Kalshi allows only on IOC; never FOK | IOC |

**Expiry** is in whole Unix seconds, so an order dies N−1 to N s after sending. The minimum lead time is undocumented: test on demo, start at 2 s. NTP is required (the Mac ran 30-60 ms behind Kalshi). After expiry the quoter re-prices from the live touch. Every order, LIP included, lives at most 60 s.

**Amends.** None in the first live weeks. An amended price that crosses the book fills at once (Kalshi's changelog), and nothing says `post_only` survives, so until demo proves otherwise a re-price is cancel plus a new post_only order. An expiry-only amend keeps queue place (FIFO since 10-01) but must resend "the current price and total quantity", so gw builds it from its latest order state, never strat's copy; demo-check it after a partial fill.

**Placement.** Improve one tick when the spread is 2+ ticks (at exactly 2, only on the side fair or inventory favours). Never join the back of a 1-tick queue deeper than 3× our size unless fair clears the price by the edge floor, or simulation shows markout plus LIP dollars positive for that series (5a).

**Fills, cancels and unknown orders: one rule.**
- Positions move only on fill messages, de-duplicated by `trade_id`. The 201's `fill_count`, `user_orders` and `reduced_by` only raise the order's cumulative filled (the largest any source reports); if that exceeds summed fills for 5 s, a REST read of that ticker's fills runs. A fill turns its reservation into worst case in the same step.
- `order_id` maps to cid from the 201 or `user_orders`; private events that beat the 201 wait up to 5 s. A cancel for an order still in flight goes out once its order_id is known; the exchange expiry is the backstop.
- UNKNOWN orders sit in a table by cid, no limit. One clears when `user_orders` or a fill reports it. An order with an expiry clears at `expiration_time` + 5 s, after a paginated read of that ticker's orders and fills since 5 s before the send. The 2-minute complete-history rule stays only for orders without an expiry; today's single 1,000-order read could never be complete at the 5,000-10,000 quotes a day (est.) that LIP-15 sends.

**Kill, Stop and soft refusals.**
- Each lane has an order group per shard (groups don't span shards); Manual has one per shard at the maximum. A lane's `contracts_limit` is about 2.5 × per-side size × markets in the group, so normal round trips never trip it.
- **Kill:** flag set, gw refuses new orders → `send-cx` triggers every group → cancel-all → batch cancel of anything still listed → Polymarket open orders cancelled. Cancel-all also cancels orders placed in the next minute, which fits the 60 s lockout.
- **Stop:** gw clears the running flag (so late Systematic intents are refused), triggers Systematic's groups and batch-cancels Systematic's order ids only; no cancel-all, which could cancel your next Manual orders. **Re-arm and Start** reset the groups and wait for `order_group_updates` to confirm.
- **Soft, never Kill:** a self-trade cancel or refusal on a Systematic order, and "order group triggered" refusals. `reject.h` treats self-trade as hard today; that changes. Demo-test each shape.

**Write budget.** One bucket per shard; Systematic may use 80%. Basic gives 100 tokens a second (create 10, cancel 2), about 8-10 quoted markets; run the free Advanced upgrade (300) before going wider.

**Polymarket.** `gw/poly_wire.cpp` replaces today's synchronous body with asynchronous submission, GTD, post-only via `participateDontInitiate`, and `manualOrderIndicator` from the owner (MANUAL only for your clicks). With no client id, list a market's open orders and trades after any uncertain write. Fees round per fill to the cent, so maker fills need 2-5 contracts. 25 requests a second per IP; skip Thursday 02:00-06:00 ET maintenance.

**Shard cash.** API orders never move cash between shards. Read `target_balance_allocation` at startup and set Systematic's targets at Start, so Kalshi rebalances instead of a sweeper of ours; if demo shows it misbehaving, require it empty and fall back to transfers at most every 5 minutes. A market's shard comes from its `exchange_index`, never a table. A short shard turns the ticket amber with a one-click "move $X from shard N".

## 4. Simple risk book

**It tracks:** cash per venue and per shard, from the exchange; positions per market (owner, average cost, mark at mid, worst case); working orders, each holding a worst-case reservation made before sending and released on fill, cancel or expiry (a reducing order reserves $0); P&L per lane and day, with fees and LIP on separate lines.

**Five limits, Systematic only:**

| # | Limit | Value |
|---|---|---|
| 1 | Worst case plus reservations ≤ 70% of each venue's equity (cash plus positions at cost) at Start, within the free cash Manual leaves; reducing orders never refused | ~$99 Kalshi (of ~$142 with perps cash back), ~$69 Polymarket |
| 2 | Worst case per correlated group (metals, FX, one city-day, one game across venues) | $15 ($25 for an LIP group: 40 at 60c is $24) |
| 3 | Net inventory per market | 20 contracts (40 on LIP series) |
| 4 | Daily loss stop | $30 (decision 1) |
| 5 | Floor: combined equity of both venues, marked at the bid; halts Systematic until you restart it | $150 (of ~$240) |

**Lane budgets** sum below the ceiling: LIP-15 $55, band $25, arbitrage $15; a second LIP series takes from the band.

**Fail closed:** after a private-socket drop, sequence gap or missed heartbeat, Systematic sends nothing new until a REST reconcile matches. An UNKNOWN blocks its market for both modes. A stale book (socket down, sequence gap, missed heartbeat) blocks orders priced off it; quiet isn't stale, since thin college books go 57-90 s between trades. A clock offset over 0.5 s blocks Post. Nothing goes out before the startup reconcile. Cancel and Kill always go.

**Manual is uncapped** and may use all free cash. The one gate is the fat-finger guard: a crossing order more than 2 ticks through the touch needs Shift+click or OVERRIDE. The daily stop only warns Manual. Kalshi nets one position per market, so a manual order where Systematic quotes cancels Systematic's orders, marks the market MAN until you release it, and hands Systematic's inventory to you at the mid with a logged line; you own any flatten (a subaccount is decision 8).

## 5. Strategies

**One maker engine runs every lane.** Fair = microprice, (bid × ask size + ask × bid size) / (bid size + ask size), or the mid when the spread is over 3 ticks. Only a validated anchor is averaged in; quotes are pulled if the two differ by over 3c. Edge floor e = 0.5c plus any maker fee (1c with no anchor); inventory skew 0.1c per contract. A new market type is a row in `config/lanes.json`.

### (a) LIP-15 on fee-free 15-minute metals and FX (the core lane)

- **Markets:** KXNATGAS15M first, one series, one shard; platinum next (best 60 s tape markout of the four measured, +0.36c). A series joins only when recorded books show 300 contracts a side in most snapshots, since LIP pays nothing for a snapshot where the whole book lacks its target size. FX also waits until its settlement feed is confirmed on `pyth_value` and replicated: its books are 5-8c wide with 1-contract best quotes, and EURUSD has the worst tape markout (−1.37c).
- **Fair:** microprice alone with a 1c floor until the anchor passes the desk's own gate (`reference_feed.h`): 50 consecutive windows whose published strike this feed reproduces. None had settled at the last record, so the VM records `pyth_value` and strikes from week 0 for `tools/replication.py`. Then 50/50 microprice and Φ(ln(S/S0)/(σ√τ)) from `terminal_price.cpp` (S the feed price, S0 the window's opening reference, σ trailing volatility, τ time left).
- **Quotes:** post-only both sides; YES bid at the best bid or one tick better, never above fair − e − 0.1c·q (q = net inventory); NO mirrors it. Two-sided in minutes 0-10, only the reducing side after, an IOC flatten at T−60 s if |q| > 10 (it pays the taker fee).
- **Behind the touch is untested:** an order there fills only once the touch is used up, as price moves through it. Before week 3, `fillsim2.py` tests "rest 1-2 ticks behind" and age caps on natural gas; rest behind or join only where simulated markout plus expected LIP dollars per contract is positive.
- **Pull both sides** on our fill, a 2-tick touch move, a 0.5σ underlying move, a Pyth gap by the existing rule (8× the median update interval, 2 s floor; the feed is de-duplicated, so silence can mean "no change"), a dropped private socket, and data releases from −60 s to +5 min (`config/calendar.json`).
- **Start in two parts:** a markout leg at 10 a side in all minutes, scaled only by the rule below; and a **LIP probe** at 30 a side for about 40 windows, minutes 0-5 only (the tape's best stretch, +1.77c), with its own $15 loss cap, to measure share and payout.
- **Scoring:** 10 s and 60 s markouts; our share each second; the LIP payout per window from the balance ledger (or the daily reconcile residual if it isn't itemized); a nightly comparison with the tape's average maker. Full-size volume (est.): 60-250 fills, 1,000-4,000 contracts a day.
- **Rule after 200 fills** (60 s markout plus LIP dollars per contract): scale up one step if above 0 with an 80% lower bound above −0.3c; halve between −0.3c and 0; retire below −1.5c, or if our share is under 5% in over half the windows.

### (b) 96-99c maker band on T+1 markets

- **Evidence gap and gate:** weather's +0.98c is the average maker, mostly front-of-queue. Its 96-99c books are usually 1 tick wide with deep queues, which our rule forbids joining (the −2.15c case), and none has been recorded. The VM records KXHIGH books from week 0; start only if the back-of-queue replay's 60 s markout is positive.
- **Markets and rule:** KXHIGH in 5 cities, then the 96-99c side of 15-minute series LIP-15 isn't quoting (the band figure covers whole windows; the last 3 minutes averaged −0.08c for all makers); fee-free games later, pre-game only. Bid the 96-99c side, post-only. Weather: pull around the hourly observation (−2 to +5 min); once the high is effectively known, quote only the favourite.
- **Size and kill:** 5 a market for 100 fills, then 10; $15 per city-day or game, $25 for the lane (a 10-lot loss costs about $9.70). Score on 60 s markout, settlement P&L and the tape's 96-99c makers; pause below −1c after 100 fills, or at −$20.

### (c) Arbitrage

- **Kalshi ladders and dominance:** the scanner (`ladder.cpp`, `event_payoffs`) trades only baskets paying more than cost plus fees in every outcome, settling today or tomorrow, within its $15 budget. IOC legs scarcest first; a missed leg is completed within 1 tick or unwound. Allow 1c legs (`min_price` blocked 209). Evidence: 9 of 703 events ever positive, best +1.6c. Pause if legging losses pass $5.
- **Weather two-venue NO basket**, a capped hypothesis: log the merged ladder 2 weeks; go live only if 1c+ locks after fees appear at executable size, then $10 outstanding. The venues settle on different sources.
- **No game-pair lane.** Manual Take routes to the cheaper venue when rules match (about 0.2c a contract).

### (d) Thin-college late-game stale quotes

Record now volleyball (Thu-Sun, ~210 a week), college hockey (Fri-Sat), and table tennis, AHL, Liiga, SHL and CS2 daily; women's basketball from Nov 2, with the thin men's decile as a control. Measure what was knowable then: how often the best price sits 20c+ from a live fair, for how long, at what size, and 60 s and 300 s markouts net of the fee. **Gate:** 50+ cases with a positive net markout (at ~25c edge, about t = 4.4, est.). Then $2-5 IOC clips, $10 a game, ~$40 open, a score feed at most 5 s old; fair from a Markov point model (volleyball) or model (f) (basketball). Kill after 100 trades if the net markout isn't positive or the lane is down $20.

### (e) ESPN experiment (X1)

Backtest both directions on the ~203 recorded games in `data/research/espn_leadlag.sqlite`, aligning ESPN by first-seen time or +41.5 s (its play timestamps look ahead). The prior favours trading against stale ESPN: at 30+ point gaps, Kalshi was right but under-confident. Trade the better direction as **$1-2 IOC takes, as backtested**; a resting order would need cancelling within ~5 s of a score while ESPN runs ~41.5 s late (the S2 mistake was testing one order type and trading another). Labelled EXPERIMENT, scored on 60 s markouts, retired after 100 trades or $20 lost.

### (f) Friend-style model, women's college basketball

- **Training** (`tools/train_wcbb.py`): logistic on margin, time left, possession and the pregame Kalshi price as a logit, checked against Stern's model; Elo ratings as the prior when the pregame book is thin; XGBoost only if it beats the logistic on held-out Brier score (mean squared error of the probability). `models/wcbb_logit_vN.json` carries coefficients, scaling and 10 golden rows; XGBoost loads through `XGBoosterLoadModel` behind `DESK_XGBOOST`; C++ refuses a model whose golden rows differ by over 1e-6.
- **Manual overlay from the first listed game** (season opens Nov 2; last season's listings began Nov 20): F marker with ±σ, break-even band, edge line. **Systematic later**, at 10 a side, after 150+ settled games, a held-out Brier at least the Kalshi mid's, and positive markouts on your overlay clicks.

## 6. UI

**Kept from `ui/index.html`:** the render core (`keyedRows`, `setText`, `setHTML`, lines 874-974), the keyboard handler and typing guard (1074-1130), storage and timeout wrappers, the ladder renderer with its NO-to-YES conversion (3311-3354), the blotter. **Dropped:** prose, 93 toasts, 31 confirm() calls, 48 ad-hoc colours, 13 pollers, 10 of 12 tabs. Rewrite est. 1,200-1,600 lines.

**Layout.** One non-scrolling screen (about 1512×860 in Safari, est.); 18 px rows; no cards, toasts or dialogs. **Header (28 px):** the [SYSTEMATIC RUN | MANUAL 3] switch (Systematic's state; working manual orders); cash and free cash per venue; day P&L split S/M; stop left; Systematic used against its ceiling; health cells (feed lag, click-to-ack p50, heartbeat, clock); KILL (one click, no dialog, 60 s lockout, a two-click re-arm that waits for the groups to reset).

**SYSTEMATIC tab:** Start/Stop with preflight tags (KEY, CASH, SHARD, CLK, FEED, PRIV, GROUPS); Start stays disabled, with the reason, while any fails. A lanes grid (budget, used, fills, MO10, MO60, fees, P&L, and a separate LIP $ column so a reward can't hide losing fills), working orders, fills, risk by outcome.

**MANUAL tab:**
- **Watchlist:** bid, ask, model fair, ESPN with its age, position, clock.
- **Two ladders on a fixed axis.** Click the bid side to buy, the ask side to sell: away from the touch it posts with the chosen expiry; at or through it, IOC under the guard. Right-click cancels a level. Our orders show size and seconds left; each ladder shows "last change Ns ago".
- **Right column:** the ticket (IOC, FOK, POST 5 s/15 s/60 s/5 min/30 min/until start, all with exchange-side expiry, 15 s default, no GTC; cost, fee and edge lines), working orders (foreign ones flagged), fills, positions with FLAT.

**Hotkeys** (off at load; H toggles): B/S IOC at the touch; Shift+B/S join; Alt+B/S improve one tick; Esc cancel this market; Shift+Esc cancel all manual; 1-6 size presets. KILL is click-only.

**Colours** carry meaning only: bid #4DA3FF, ask #FF5C5C, up #3FB950, down #F85149, amber #E3A008, critical #9B1C1C, model fair #C792EA; grey otherwise; a colour-blind mode.

**Stream and guards.** One `GET /api/stream` (SSE) per tab carries book (≤20/s per ticker), order, fill, position, lane, risk and alert events, plus a heartbeat each second. On a sequence gap (including one marked by a full server queue) or 3 s without a heartbeat, the page dims and refetches `/api/snapshot`. Requests whose Host isn't `127.0.0.1:<port>` or `localhost:<port>` are refused, which stops DNS-rebinding pages that could otherwise add `X-Desk`. Every POST, Kill included, carries `X-Desk` plus a random per-start token written into the page. `deskd` listens on 8788, not the old 8787, with 16 threads, 2 s keep-alive and at most 4 streams (fixing the measured 4.8 s stall when 9 idle connections held all 9 threads).

**Safari:** read `event.code` (Option+B types "∫"); never bind Cmd; block the context menu only on the ladder; redraw on `visibilitychange`, since Safari slows hidden tabs. **Wide mode:** a third ladder at 1850 px; at 2400 px (external monitor), both tabs side by side.

## 7. Data and cloud

**The Mac.** `data/ops.sqlite` (under 1 GB), written only by `store`: orders with timelines, fills, markouts, the ledger (fills, settlements, transfers, LIP), LIP windows (share against payout), a 3-day top-of-book cache. A nightly `VACUUM INTO` copy is what research reads; the journal buffers meanwhile.

**The Oracle VM recorder**, separate from the C++ work: `recorder/rec.py`, Python asyncio, reusing `perps_recorder.py`'s pattern and `kalshi_client.py`'s signing. On a read-only key it records `orderbook_delta` for the LIP series, KXHIGH weather and sampled games; `ticker`, `trade` and `market_lifecycle_v2` for all non-crypto markets; `pyth_value` and each window's strike. By public REST: thin-college books every 3-5 s, trade backfill, daily `/markets`, hourly `incentive_programs`. Also Polymarket books for paired games and weather every 2-5 s with the `Age` header, and ESPN and NCAA scores every 3-5 s with first-seen times.

Subscriptions are always an explicit list: an empty one means the whole exchange (77.6% of September's rows were unrelated). Subscribe to each next window before it opens; the old desk's books started 31 s late. Hourly zstd Parquet with a manifest (rows, time range, sha256) and a `gaps` table; chrony; systemd `Restart=always`; hourly upload to R2 on a write-only token, `archive/` locked against deletes; healthchecks.io alerts after 5 silent minutes. Est. 120 MB a day, about $2.39 the first year. Day-one test: a read-only key streaming `orderbook_delta` from a datacenter IP for an hour; fallback, REST snapshots.

**Moving today's 27 GB** (old desk stopped, no build running): DuckDB attaches `prod.sqlite` read-only and copies each table by day to zstd Parquet; re-parse 09-14 to 09-17 from raw JSON (those archive files have zero prices); verify counts and sha256; upload to `archive/legacy/`; after 30 days, delete `prod.sqlite` (15.7 GB) and `data/raw` (11 GB).

**Backtests.** DuckDB reads the Parquet on R2 with a read-only secret; `desk-replay` runs the same C++ quoter over an exported day, filling only when a trade prints through our price.

**For the college test, record now:** everything in 5(d), hourly event discovery (baseball lists ~6 hours ahead) and a monthly D3/D2 scan.

## 8. Testing

- **Unit tests per worker.** md: books from captured frames (a missing field is NULL, never 0). gw: every transition, a reservation-sum check after each message, every fail-closed trigger, Kill overtaking 100 queued orders, click-then-Esc never rests, Kill with gw frozen. Risk book: parity with old `risk.cpp`. strat: golden quotes, pull rules, LIP scores worked by hand from the CFTC formula. web: Host and token checks, a full stream queue, 10 idle connections with a POST still under 5 ms.
- **Fake exchange** (`tests/fake_exchange/`), driven by a recorded tape. Faults: drop before reply, 5xx after accepting, a body cut mid-way, chunked bodies, 429, a 2 s stall, socket drops, sequence gaps, duplicate ids. Races: fill before the 201, cancel before the 201, a duplicate fill message, a fill during a cancel, 5,000 orders through the UNKNOWN table.
- **Kalshi demo checks** (`tools/demo_checks`): duplicate `client_order_id`; shortest expiry; expiry-only amend keeping queue place, and after a partial fill; post_only amended to a crossing price; group trigger and reset; self-trade cancel shapes; `reduce_only` on IOC; `target_balance_allocation`; private message shapes.
- **Latency:** every order logs its timeline; `tools/latency_report.py` gives p50/p90/p99 per segment.
- **Soak:** 4 hours on the fake exchange under ThreadSanitizer before Start goes live; 8 hours at 10× rates with random faults, under both sanitizers, by week 6.
- **Invariants** (`docs/invariants.md`, one test each, green before any live order): every kept or new rule in sections 2-3, plus old lessons: an empty subscription list is refused; SIGPIPE is ignored (it killed the process on a TLS reconnect); frames past 8,192 and 4,096 bytes arrive whole (both cut-offs happened).
- **UI:** `safaridriver` smoke tests at 1512×860 and 1920×1080.

## 9. Milestones

| Week | Delivers | Done when |
|---|---|---|
| **0** (Oct 6-11) | **First:** stop the old desk for good (`tradeit down`, which leaves `data/desk.stop`; `launchctl bootout gui/$(id -u)/com.kalshi-desk.supervisor`; delete the plist) and add an account lock (`data/kalshi-account.lock`, flock held by any process that can send orders; `deskd` won't start without it). Then the data move (no build during it), dead files, ccache, the `deskd` skeleton, fake exchange, demo checks | Nothing can start the old desk; a strategy edit rebuilds under 20 s; ~27 GB verified in R2 |
| **Recorder** (weeks 0-1) | VM, R2, recorder incl. `pyth_value`, strikes, KXHIGH books, college | 24 h with every gap logged, gaps under 1% of minutes |
| **1** (Oct 12-18) | Gateway; spare pool; Manual Take and Post on REST state; bare ladder and ticket; SSE and guards. Demo first, then 50 minimum-size real orders | p90 under 100 ms; Take fills at the shown price 90%+ in 1c books; nothing lives 2 s past expiry |
| **2** (Oct 19-25) | Private socket; fill rule; UNKNOWN table; risk book; Kill, `deskctl kill`, order groups, Stop; race tests | Pulling the network blocks orders; Kill clears all in under 2 s, even with gw frozen |
| **3** (Oct 26-Nov 1) | Maker engine; fillsim runs and the band replay; the 4-hour soak; then LIP-15 on natural gas, 10 a side, expire and re-post, no amends | **Start quotes by itself** for a full supervised session by Nov 1; Stop clears in under 2 s; share log reconciled with the ledger, zero payouts included |
| **4** (Nov 2-8) | LIP probe; full two-tab UI; daily scorecard; the band if its replay passed; arbitrage IOC | Probe share and payout measured per window |
| **5** (Nov 9-15) | Polymarket orders and sockets; Take routing; weather two-venue scan; X1 backtest; first logistic model; college analysis | Polymarket fills arrive by push; X1 direction chosen; 5(d) gate result |
| **6** (Nov 16-22) | X1 live; overlay at tip-off; 8-hour soak; old code deleted **Nov 22** | LIP-15 200-fill review if reached |
| **7-8** | 5(d) IOC lane if the gate passed; Polymarket reward test; amends if demo-proven; wide mode; XGBoost if it wins | 7 days without a manual intervention |

**If week 3 slips, cut scope, not tests:** Kalshi's target allocation set by hand, one order group per shard, a bare UI.

**You must do these yourself:**
- Approve or run the old-desk shutdown; `tradeit down` doesn't cancel exchange orders, so check Kalshi for resting ones.
- Close perps and move the $29 back.
- Create a Kalshi demo account and key, and a read-only key for the VM.
- Run the Advanced-tier upgrade once one API order exists.
- Sign up for Oracle (card, pay-as-you-go, $1 alert, US home region), Cloudflare R2 and healthchecks.io; `brew install ccache duckdb`.
- Keep NTP on, use a wired network, turn sleep off during trading hours.
- Check Polymarket for assigned campaigns; ask your friend for his period, capital and drawdown.

## 10. Risks, and what would make us stop or change course

- **LIP ends or shrinks** (Kalshi can change it any time; it runs to 2027-01-01), or daytime share sits far below the overnight snapshots. → Keep LIP-15 only if its markout alone is positive.
- **The anchor never replicates.** → LIP-15 stays microprice-only at a 1c floor, and no FX.
- **We are the slow maker:** a week more than 1c worse than the tape, or the 200-fill retire rule. → Cut size and windows before adding lanes.
- **The rebuild slips** (the infrastructure review estimated 4-5 weeks; changes here have needed 2-3 review rounds). → Cut scope as above.
- **Order bugs:** a duplicate order, an unexplained position, a runaway fill. → Stop Systematic, write the test, resume. Order groups, the account lock and the floor bound the damage.
- **No Oracle capacity** → a $5-8 VPS.
- **Overall stop:** after 4 live weeks, if no lane passes its rule and Systematic is down over $40 including LIP, stop Systematic; keep Manual and the recorder.

## 11. Decisions still yours

1. **Systematic daily stop:** $25, $30 or $40, with the $150 combined floor. **Recommend $30**: it fires on about 13% of days at zero edge and a $20 daily standard deviation.
2. **Trading hours.** LIP-15 earns only while the Mac is on and awake. **Recommend** hours you can supervise for two weeks; add an always-on home machine if LIP-15 pays.
3. **Manual during a private-socket outage:** block, allow, or block with a logged Shift+click override. **Recommend the override**; positions on screen may be wrong then.
4. **First LIP size:** 10 a side, 30, or both. **Recommend both:** 10 a side for markouts plus the 30-a-side probe, since 10-lots mostly earn $0 of LIP.
5. **ESPN direction.** **Recommend** letting the backtest choose (likely against stale ESPN), traded as IOC.
6. **Polymarket's first job:** routing only, or also a 2-week reward-quoter test capped at $20. **Recommend both**, the test retired if rebate plus rewards don't beat the 60 s markout.
7. **Old data.** **Recommend** archiving to R2, a cold copy for 30 days, then deleting the 27 GB from the Mac.
8. **Systematic in Kalshi subaccount 1.** Positions would never net with yours, and Stop could use a subaccount cancel-all; but the 70% becomes cash set aside, and self-trade prevention across subaccounts is undocumented. **Recommend** one account with the handover rule for now; test on demo, revisit in week 4.