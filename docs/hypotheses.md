# Hypothesis registry

Status vocabulary: idea → collecting → testing → supported | rejected. The same rows are seeded
into the `hypotheses` table and editable on the dashboard. A hypothesis is only "supported" when
the realized net edge (after fees) has a bootstrap 90% CI above zero, or the paired Brier
difference vs the market is negative with n ≥ 100.

Two facts that shape everything below, checked against the exchange on 2026-09-05:

- Fees come from the series object (`fee_type`, `fee_multiplier`). Makers pay nothing on
  `quadratic` series (weather, crypto hourly, most sports); they pay `0.0175·P(1−P)` on
  `quadratic_with_maker_fees` series (KXCPI, KXNBA, KXEPLGAME, ...). Every hypothesis is stated
  net of the fee that actually applies.
- Weather rules now name **The Weather Company** as the settlement source ("the official and
  final value ... is the maximum/minimum temperature as reported by the Weather Company"),
  not the NWS CLI report directly. The NWS obs feed is still the right live input, but WX-1
  includes an explicit settlement-source agreement check before the strategy is trusted.

## Sports

**SPORT-1 · EPL tied at 80', No-on-draw underpriced.** Among EPL matches tied at 80:00, a model
using team strength (pre-match Elo or closing line) and live state (xG so far, shots, cards)
identifies No-on-draw contracts priced below their eventual win probability after costs.
Data: in-play quotes for `KXEPLGAME-*-TIE` (the `quotes` table logs every top-of-book change),
a live score/xG feed with minute stamps, pre-match closing lines. Test: for each match tied at
80:00 record the NO price and the model's P(tie broken); settle; mean net edge with bootstrap CI
over ≥ 60 matches; calibration by bucket. Note the maker fee applies on KXEPLGAME.

**SPORT-2 · Kalshi vs sharp closing line.** When the pre-kickoff mid deviates ≥ 4c from the
de-vigged Pinnacle close, trading toward the sharp line is positive after the maker fee.
Data: pre-kickoff quotes, Pinnacle/consensus closing odds. Test: paired Brier and realized edge.

**SPORT-4 · Public live win probability as a maker's fair (`sports_live`).** In US team-sport
game markets, quoting maker-only around ESPN's live win probability (or a lead/time model from
the score, clock and the sportsbook line where ESPN has no play-by-play) at fair ± 6c, only
where the book is ≥ 6c wide, with reduce-only exits at fair + 3c, earns more from the spread
than it loses to takers who see the play before the feed does. Data: `ext_wp` (every 30 s per
live game: ESPN wp, lead/time wp, line, both Kalshi mids) plus settled results. Test:
`research wp-score`: Brier of ESPN / lead-time / market by league and period, judged by games
not rows; then the simulated rule return. Decision: `auto` only when the market's Brier is not
better than ESPN's in the periods traded and the rule's 90% CI clears zero on ≥ 150 games. It
also needs the account to be allowed to open Sports positions, which it is not today.

**SPORT-3 · Longshot bias in game markets.** Contracts ≤ 10c win less often than priced; NO at
90–95c is positive after the (small) fee at extreme prices. Data: settled game markets and last
prices via public `/markets/trades` (backfillable now). Test: win rate by price bucket with
binomial CIs; net return of NO at the ask.

## Crypto

**CRYPTO-1 · Hourly BTC/ETH brackets lag spot.** Bracket quotes update slower than spot; a
lognormal fair from live spot with realized vol finds quotes ≥ 3c from fair that fill as a
maker before they move. Data: `ext_prices` (spot every 20 s), `quotes`, fills, settlements.
Test: realized edge of `crypto_lognormal` proposals; adverse-selection check (spot move between
proposal and fill vs direction traded); Brier vs market mid.

**CRYPTO-2 · Bracket-implied vol exceeds realized vol.** First live snapshot (Sat 2026-09-05,
BTC ≈ 79.9k, 34 min to settle): the at-spot $100 bracket was 40–41c; a 55% annual vol puts it
near 11%, and even 20% vol only reaches ~30%. Either weekend realized vol is far below annual
vol, or the market overprices the center. Data: full bracket curves per hour (`quotes`), spot
history. Test: fit implied vol per hour from bracket mids; compare to realized 1-hour vol;
simulate NO-on-tails at the ask net of fees.

## Commodities

**COMM-1 · WTI/NatGas settlement brackets vs futures.** In the last two hours before settlement,
a fair from the front-month futures price with a short-horizon vol beats bracket prices.
Data: futures quotes (delayed is fine for research), `quotes`, settlements. Check the settlement
definition (daily settlement price of the named contract, not last trade).

**COMM-2 · EIA inventory markets vs the API print.** The API Tuesday number predicts the EIA
Wednesday surprise; weekly inventory brackets do not fully adjust after the API print.

## Climate and Weather

**WX-1 · Same-day highs: observed max + remaining forecast.** After 1pm local, the observed
running max plus the remaining hourly forecast beats bracket prices; edge concentrates in the
"or above / or below" tails once the day's peak has passed. Built: `weather_nws` (`ext_weather`
logs forecast and obs). Test: realized edge by hour-of-day bucket; Brier vs market by lead
time; **settlement-source agreement** (Weather Company value vs NWS CLI daily max) on ≥ 60
settled days before trusting the obs feed.

**WX-2 · Day-ahead brackets: station-calibrated sigma.** A per-station forecast-error sigma
prices the center bracket better than the market, which underprices it when models agree.
Data: `ext_weather` forecast-vs-settle history per station. Test after ≥ 40 days per station.

## Economics

**ECON-1 · CPI brackets vs nowcast in the final 48h.** A distribution centered on the Cleveland
Fed nowcast blended with consensus prices `KXCPI` brackets better than the market, which
overprices the tails. Few events: accumulate ≥ 6 prints before judging. Maker fee applies.

**ECON-2 · Fed decision markets vs fed funds futures.** Divergences ≥ 5c from FedWatch-implied
probabilities revert toward futures before the meeting.

## Financials

**FIN-1 · Index close brackets lag the tape.** In the last 30 minutes, a lognormal fair from the
live index with VIX/VXN-implied vol finds stale `KXNASDAQ100`/`KXINX` brackets. Needs a
real-time index feed (thinkorswim / IBKR).

**FIN-2 · Longshot bias in daily index brackets.** Same test as SPORT-3 on index brackets.

## Science and Technology

**SCI-1 · Release-date markets overprice on-time delivery.** "X released by date D" markets price
YES above the historical on-time rate for announced dates. Data: settled date markets and
final-week prices; a base-rate table.

## Suggested additions

**TRANS-1 · TSA throughput brackets vs a seasonal model** (Transportation). Public daily TSA
counts; weekly/monthly brackets; seasonal + YoY model.

**CO-1 · Earnings beat/miss base rate** (Companies). Large caps beat EPS consensus ~70% of the
time; check whether "beat" markets are priced below that.

## Research protocol (the EPL one, written out; every other row follows the same shape)

**SPORT-1, step by step**

1. Collector (built, running): `sports_inplay` logs every live match in the watched leagues to
   `ext_sports` every 30 s: minute, score, red cards, shots on target, possession, the pre-match
   goal rates fitted from Kalshi's own prices (Dixon-Coles rho −0.13), the model's H/D/A, and the
   Kalshi mids for the home, tie and away markets at that moment.
2. Sample definition: one row per match, the last `ext_sports` row with `minute` in [78, 82] and
   `home_score == away_score`. Outcome from `markets.result` of the tie market (YES = draw).
   Independent events = matches. Target n = 120 tied-at-80 matches. With 6 leagues that is
   roughly 6 weeks of weekends; EPL alone would take a season.
3. Baselines to beat: (a) the market mid at 80' (mkt_draw); (b) the Poisson model's p_draw.
   Report paired Brier of each against the outcome, plus the realized return of buying NO at
   the 80' ask net of the KXEPLGAME maker/taker fee, with a bootstrap 90% CI.
4. Model refinement, only after the sample exists: logistic regression of P(tie broken | tied at
   80') on lam_home + lam_away (pre-match total goals), |lam_home − lam_away| (imbalance),
   red-card difference, shots-on-target difference, minute. Five features, 120 rows: fit with
   L2 regularisation and report out-of-sample by league-fold.
5. Decision rule: supported if NO-at-ask net return has a 90% CI above 0 on the held-out folds
   and the refined model's Brier beats the market's. Then `sports_inplay` gets a `tied_late`
   mode that only trades that state, `market_weight` drops from 0.5 to 0.2, and the auto rule
   opens with `max_cost` $4.
6. Known confounders to check before believing anything: (a) settlement uses 90 minutes plus
   stoppage, no extra time (rules text confirms); (b) markets with `yes_ask_size` 0 at 80' are
   untradeable and must be excluded from the return calculation, not from the Brier sample;
   (c) ESPN's clock lags TV by 20–60 s, so the row at "80'" is really 80–81.

**The same protocol for the other rows** (collector → sample definition → baseline → refinement → rule):

| key | collector (status) | sample unit / target n | baseline | first refinement |
|---|---|---|---|---|
| CRYPTO-1 | crypto_lognormal + ext_prices (running) | one proposal per (bracket, hour); 300 | market mid at proposal time | adverse-selection filter: skip when spot moved > 0.3σ in the last 60 s |
| CRYPTO-2 | quotes + ext_prices (running) | one hour per coin; 200 hours | DVOL-implied bracket probabilities | implied vs realized 1-h vol regression; trade only when implied/realized > 1.3 |
| WX-1 | weather_nws + ext_weather (running) | one (station, day); 150 | market mid at 13:00 local | per-station bias and sigma from forecast-vs-settle residuals; settlement-source agreement first |
| WX-2 | ext_weather ensemble columns (running) | one (station, day); 200 | NWS point forecast with sigma 2.5 | inflate factor per station from ensemble spread vs realized error |
| SPORT-2 | needs The Odds API key (30 min to wire) | one match; 200 | Kalshi pre-kickoff mid | trade toward the Pinnacle line when |diff| ≥ 4c after fee |
| SPORT-3 | trades backfill (public, run once) | one settled market; 2,000 | price = probability | bucketed win rates; NO at ask returns |
| ECON-1 | Cleveland Fed scraper (2 h to wire) + BLS key | one release; 6 before judging | market mid 48 h before print | nowcast blend weight fitted on history |
| COMM-1 | Yahoo delayed futures from the Mac (1 h) | one settlement day; 60 | market mid 2 h before settle | short-horizon vol from the futures tape |
| FIN-1 | real-time index feed (needs Schwab or IBKR) | one close; 60 | market mid 30 min before close | VIX-implied vol for the last half hour |

Order of research effort: SPORT-1 and CRYPTO-1/2 and WX-1/2 are collecting now at zero cost.
SPORT-2 and SPORT-3 are a key and a script away. ECON/COMM/FIN wait for data plumbing.

## First results (2026-09-05, `kalshi-desk research`)

- **WX-1 settlement source.** 210 settled station-days (NYC, CHI, MIA, LAX, AUS, PHL, Aug 14 to Sep 4):
  the hourly METAR daily max equals the settled bracket 63% of the time; when it misses, the
  settled value is 1F higher (31%) or 2F higher (6%), never lower (one 5F outlier). Continuous
  sensor readings beat hourly samples. The live model now treats the running max as
  obs + {0: 0.63, +1: 0.31, +2: 0.06} and the forecast max gets +0.4F. Status: supported (the
  mechanism), and the same-day tail trade should price that shift in.
- **SPORT-1 first look.** 119 settled matches (EPL, La Liga, Bundesliga, Serie A, Ligue 1, Aug 16
  to Sep 5), 31 level at 80', 22 with a clean 80' print. 54.5% stayed level. Market Brier at 80'
  0.227, Poisson model 0.258, naive 0.65 rule 0.259: the market at 80' is better than the model
  so far. NO-at-ask net of the maker fee: +0.07 per $, 90% CI [-0.35, +0.53]. Status: collecting;
  nothing to trade on yet. Three matches were dropped because the tape at "kickoff + 98 min" was
  clearly after a late goal; the 80' clock alignment needs goal-time anchoring before n = 120.
- **SPORT-3 (EPL, pre-kickoff prices).** 15 markets priced 6c to 35c before kickoff, zero settled
  YES. n is tiny; run `research longshot` after the multi-league backfill finishes.

## Second results (2026-09-06, after the 2026-09-05 post-mortem)

- **WX-1b · Same-day climatology vs the tape (`research wx-intraday`, 8 stations, 2,448 settled
  markets, Jun 30 to Sep 4, 2.59M trades).** The model: remaining rise from the running METAR max
  drawn from 2022–2025 history for the same station, month ±1, hour and one-hour trend, plus the
  settlement shift. Out of sample by construction (fit years < 2026). The market wins at every
  hour: Brier market 0.089 → 0.028 (9h → 16h) vs model 0.136 → 0.079, and the market is still
  better on the rows where the two disagree by ≥ 8 points. Trading those disagreements as a
  taker, filled at the next print on our side, loses 0.4c to 5.7c per dollar at every hour
  (n 300–950 per hour; 9h and 16h CIs exclude zero on the losing side). Status: **rejected** as
  a standalone same-day model. Consequence in code: same-day weather is research-only
  (`params.weather_nws.same_day_trading`), and any model ≥ 30 points from the mid is
  research-only (`risk.max_disagreement`). Next: WX-1c, a forecast-aware model backtested on
  archived NBM/ECMWF runs (Open-Meteo previous-runs API) rather than climatology.
- **Settlement shift, larger sample.** 408 YES brackets: settled = METAR max +0 in 65%, +1 in
  30%, +2 in 5%, other <1%. The {0.63, 0.31, 0.06} mixture stands.
- **SPORT-3 · Longshot bias, 963 soccer markets (7 leagues, price 135 min before settlement).**
  Win rate equals price in every bucket (|z| < 1.5 everywhere); NO at the ask loses net of the
  taker fee in every bucket except one noisy 0.51–0.66 cell. Status: **rejected** for the major
  leagues. The long-tail leagues (National League, Serie C, USL, Faroe Islands...) are not yet
  backfilled; test them before assuming they differ.
- **CRYPTO-2 · Implied vs realized vol, first look (4 hourly BTC events, Sat 2026-09-05).** The
  bracket curve 30 minutes before settlement implied 13–27% annualized while DVOL was 38%, and
  it tracked the realized vol of the previous 3 hours (5–31%). A DVOL-based fair priced the
  at-spot bracket at 0.17 against a market of 0.42–0.49. Status: collecting; the strategy now
  uses 3-hour realized vol with DVOL only as a band and is research-only until n ≥ 100 hours.
- **Market microstructure check (2026-09-06 02:50Z open books).** Long-tail soccer (EFL League
  One, Serie C, USL) is quoted two-sided at 9–10c wide with 200–500 contracts a side by market
  makers, with zero pre-kickoff volume; D3 football/basketball and FCS series have never listed
  a market. Pure liquidity provision there has no flow to capture pre-game and faces faster
  feeds in-game; it is a fill-rate/adverse-selection experiment, not an edge.

## Post-mortem: the first five live trades (2026-09-05)

All five were `weather_nws` same-day proposals approved by hand between 11:32 and 11:48 PT.
`raw` is the model before the 50/50 shrink toward the mid; the market column is the mid at the
time; the settled value is The Weather Company number implied by the bracket that resolved YES.

| market | trade | raw | market | settled | result |
|---|---|---|---|---|---|
| KXHIGHNY-26SEP05-T79 (<79°) | NO ×13 @0.23 | 0.10 | 0.78 | 77 → YES | closed early at YES 0.01: **+$9.88** (a resting NO bid at 0.99 was there to hit; the position itself was losing) |
| KXHIGHCHI-26SEP05-B83.5 (83–84°) | NO ×88 @0.06 taker, 2 filled | 0.50 | 0.94 | 83–84 → YES | −$0.12 |
| KXHIGHAUS-26SEP05-B96.5 (96–97°) | YES ×2 @0.10 | 0.60 | 0.09 | 100 → NO | −$0.20 |
| KXHIGHLAX-26SEP05-B79.5 (79–80°) | YES ×18 @0.33 | 0.68 | 0.34 | 81 → NO | −$5.94 |
| KXHIGHLAX-26SEP05-T80 (>80°) | NO ×13 @0.30 | 0.32 | 0.69 | 81 → YES | −$3.90 |

Net ≈ −$0.30 ("1 win, 4 losses, even"). Read it the other way: the model was on the wrong side
of the market five times out of five, the market was right five times out of five, and the
one "win" was a gift from someone else's resting order, not from the model. Without it the day
was −$10.2, which is exactly the 10% daily stop (it tripped at 20:08Z on the marks).

What the model got wrong, in code terms:

1. It centered on the NWS hourly forecast max for the *remaining* hours and treated the
   observed running max only as a floor. When the day is already running warmer than the
   forecast (LAX was 79 at 11:30 against an hourly forecast of ~76; AUS 95 at 13:30 against
   ~92), the remaining hours run warm too. Fix: carry `resid_k`·(obs − forecast at that hour)
   into the remaining forecast and never center below the running max while heating remains.
2. The same-day sigma floor was 1.5°F. AUS finished +4°F over its forecast. Floor is now 2.0°F.
3. The observation window was `limit=60`, so by evening the early-afternoon max had scrolled
   out (KMDW showed 82.4 at 13:27 and 80.6 at 21:04). Observations are now pulled from local
   midnight and the running extremes never move backwards.
4. Nothing stopped a 30-point disagreement with a liquid market from becoming an "edge". A
   simple model 30+ points from a two-sided market is far more likely to be wrong than the
   market; such fairs are now research-only (`risk.max_disagreement`, all five would have
   been blocked).
5. WX-1b below says the market beats the intraday model at every hour of the day. Same-day
   weather proposals are therefore research-only until a forecast-aware model (WX-1c) proves
   itself on archived forecasts; `params.weather_nws.same_day_trading` is the opt-in.

Two things the day also revealed about the account itself: crypto orders were rejected with
`user_not_found` because Kalshi keeps crypto on a separate exchange shard that needs its own
collateral (Account → Exchange indexes on the website; the desk never moves funds), and the
Ligue 1 order was rejected with a 403 because the account is not allowed to open positions in
Sports, Elections, Politics, Culture, Tech & Science or Mentions. SPORT-1/2/3 continue as
research (the collector keeps running in `research` mode); they cannot be traded from this
account as it stands.

## How a hypothesis becomes a strategy

1. Write the fair-value function (`fair(market) → p, sigma`) and its data fetch in `prepare()`.
2. Run it in `manual` mode with `market_weight` 0.5 for ≥ 2 weeks. Approve only what you would
   have traded by hand; the queue is the record.
3. Read the by-strategy row in the gate report. Move to `bot` only when the CI excludes zero,
   then lower `market_weight`.
