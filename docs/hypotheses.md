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

### The 15-minute terminal-price model (registered 2026-09-11, before any result)

Four hypotheses about Kalshi's 15-minute gold, silver, WTI and natural-gas contracts, written
down before the model they test had scored a single settled window. The test for each is fixed
here so that it cannot be chosen after seeing the answer. Nothing below reports a result; each
says what would count as one and what sample it needs first. The settlement mechanics they all
depend on are in `docs/commodity-settlement.md`; the replay is `research/commodity_terminal.py`.

**COM-1 · The terminal distribution beats the market.** A zero-drift lognormal distribution of
the settlement reference at the window's close — built from the exchange's own published
reference value and a short-horizon realized volatility — scores a lower Brier on settled
contracts than the contemporaneous Kalshi mid. Everything else here rests on this one: if the
market is better calibrated, there is no mispricing to trade and COM-2 to COM-4 fall with it.
Data: `commodity_reference` (the settlement feed recorded tick by tick, with our receipt times),
`quotes` for the four 15M series, settlements. Test: replay with no look-ahead — every
probability uses only observations stamped at or before its own quote — then paired Brier
(model − market) with a bootstrap CI on the difference, log loss, and calibration by model-p
decile. **≥ 400 settled contracts across ≥ 2 underlyings** before anything is called supported;
an interval containing zero is inconclusive, not encouraging.

**COM-2 · Far strikes are overpriced at short horizon.** Contracts beyond about 1.5σ of the move
still to come trade above the terminal distribution's probability — the longshot bias at a
fifteen-minute horizon — so the cheap side of a far strike is positive expectation after the
taker fee. Test: buckets of |distance in σ| (0–0.5, 0.5–1, 1–1.5, 1.5–2.5, 2.5+), each bucket's
mean executable price against its realized settle rate with binomial CIs, then the net return of
taking the cheap side at the ask, fee included. ≥ 200 settled contracts per bucket, and only
where the book was deep enough to have filled.

**COM-3 · Kalshi lags a sudden move in the reference.** After the reference moves ≥ 1 one-minute
σ, the Kalshi mid takes seconds to reprice, and in that window the model leads the market rather
than following it. Data: tick-resolution `commodity_reference` and top-of-book at 1 s or better,
both stamped with **our** receipt times, so a lag we measure cannot be a lag we caused. Test:
event study in event time (mid response at 1 s, 5 s, 15 s, 60 s; lead-lag cross-correlation of
model p against the mid), then markout on the trades the model would have taken inside the
window, net of fees. It fails if the lead disappears once our own feed latency is subtracted.

**COM-4 · Ladder-implied vol exceeds realized vol.** The volatility implied by a whole window's
strike ladder is above the volatility the reference realizes over that window — COM-2 read off
the curve instead of off single strikes. Data: full ladder mid snapshots per window and
`commodity_reference` over the same window. Test: fit one σ per window by least squares on the
ladder mids with drift held at zero, compare with the window's realized σ, paired over ≥ 200
windows per underlying; then simulate selling the tails at the bid, fee included, to see whether
the gap is reachable or only visible.

What this model deliberately is not: it does not predict direction, it has no machine learning in
it, and it has no path to unattended execution in this session. `mu` is zero by construction. It
is a baseline whose only job is to say whether Kalshi misprices the terminal distribution at all.

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

## SPORT-STATE-1: the rate fit ran before the desk knew the match had started (2026-09-06)

Not a hypothesis about the world, a defect in the desk. `SportsInplay::prepare()` fitted scoring
rates in its second section and only ingested ESPN state in its fourth. On the first cycle after
any restart the state map was therefore empty, and an empty map is indistinguishable from
"before kick-off" if you only check `state != "pre"`.

What happened. The desk restarted at 19:44 UTC, mid-match, during the Marseille-Paris FC half
time (`ext_sports` shows a 536-second logging gap from 19:39:59 to 19:48:55). The fit read the
book as pre-match and calibrated a full-match Poisson to prices that already contained a 1-2
scoreline. `fair()` then applied the same goals a second time through the score term:

| | lam_home | lam_away | model P(home) | market |
|---|---|---|---|---|
| before the restart | 1.900 | 1.238 | 0.185 | 0.205 |
| after the restart  | 0.869 | 1.475 | 0.042 | 0.205 |

Nothing happened in the match between those two rows. The 50% market blend halved the error and
still left a 13.7-cent "edge" on the board, on which 8 contracts were bought at 0.79 whose true
edge was about +0.002. Worse, the contaminated rates were written to `kv` as
`lam:KXLIGUE1GAME-26SEP06OLMPAR = 0.87,1.48,pre` - tagged pre-match, so they would have survived
every later restart as though they were clean.

An earlier diagnosis in the trading log blamed ESPN reporting a `"halftime"` state that the code
did not recognise. That was wrong, and fixtures captured on 2026-09-06 (`tests/fixtures/espn/`)
say so: ESPN's `type.state` is only ever `pre`/`in`/`post`, and half time arrives as
`state="in"`, `type.name="STATUS_HALFTIME"`, `period=1`, `displayClock="45'+3'"`. The state field
was never the problem; the ordering was.

Fixed by making the lifecycle explicit rather than inferred:

- `MatchPhase` (unknown, prematch, first_half, halftime, second_half, stoppage, finished,
  suspended) derived from the provider's status block. Missing or unrecognised data maps to
  `unknown`, which is never `prematch`.
- `prepare()` ingests and settles the phase before any fitting.
- No calibration without fresh, first-hand state. A phase restored from `match_state` across a
  restart proves the match is under way - which keeps the pre-match branch shut - but cannot
  itself authorise a fit.
- Only a genuine pre-match fit is persisted as this match's rates.
- Every fit records its provenance in `rate_fits`: calibration (prematch/inplay), phase, minute,
  score, the three calibration prices, the rates, the residual, and whether it converged.
- Convergence is checked against the same snapshot the fit consumed. A fit that cannot reproduce
  its own calibration prices within `fit_tolerance` (0.02) is logged and never traded.
- An in-play calibration reproduces the book by construction, so it carries no independent
  information at its calibration point. `fair()` marks such a fair research-only until the score
  moves or `inplay_min_progress` (5 minutes) of clock has run.

Also fixed while capturing fixtures: `x["status"].value("period", 0)` threw on every scheduled
fixture, because ESPN sends `"period": null` before kick-off and nlohmann's `value()` throws on a
key that is present but of the wrong type. Ingest for not-yet-started matches was aborting.

This closes no hypothesis. It means the SPORT-1/2/3 in-play sample collected before 2026-09-06
contains fairs computed from contaminated rates wherever a restart crossed a live match, and
that sample should be filtered by `rate_fits` provenance before anything is concluded from it.

## WX-SETTLE-1: the desk is modelling the right station, and correcting it wrongly (2026-09-06)

Two questions, because Kalshi moved its weather settlement to The Weather Company on 2026-08-27
and the rules stopped naming the National Weather Service.

**Is the target right?** Yes. Intersecting the settled brackets for each station-day pins the
value Kalshi used to a one-degree interval; the NWS daily climate report for the same date falls
inside it at all six cities on 2026-09-05 (CLIAUS 100, CLIMDW 83, CLINYC 79, CLIMIA 92, CLIPHL 84,
CLILAX 81). TWC's number is the NWS single-station maximum, at least here. Six station-days on one
date is evidence, not proof, and the announcement explicitly reserves the right to derive a value
"from several observation sources", so this needs to become a standing check rather than a spot
one. Full detail in docs/settlement.md.

**Is the correction right?** No. The desk reads the hourly observation series and the settled value
comes from the continuous sensor record, so the desk's running maximum is biased low. The model
carried one global distribution for that gap - +0 63%, +1 31%, +2 6% - with a comment claiming 210
settled station-days behind it. There were none. It was fitted from which brackets won, and a
winning two-degree bracket is an interval-censored observation: it says the settled value lay in a
range, never what it was.

Measured properly, against the climate report itself (research/wx_settle_basis.py, 17 station-days):

| product | days | reports/day | mean shift |
|---|---|---|---|
| CLINYC | 7 | 38 | +0.87 F |
| CLIMIA / CLIMDW / CLILAX / CLIPHL | 2 each | 250 | +0.60 / +0.31 / +0.24 / +0.20 |
| CLIAUS | 2 | 250 | -0.24 F |

The gap is not a constant of the weather. It is the chance the hourly series missed the day's peak,
which belongs to the station's reporting frequency: Central Park files about 38 observations a day
against an airport's 250, and needs roughly four times the correction. A single global constant is
wrong in both directions at once, and in markets whose brackets are one or two degrees wide that is
not a rounding detail.

The shift is now per station and configurable. Both values are provisional - ten airport
station-days and seven at Central Park cannot support the tails, and api.weather.gov serves only
about a week of climate products, so the sample cannot be extended backwards from this source.
Session 3's recorder grows it; session 4 refits it.

Separately, a series whose rules nobody has read is now research-only in code, on that ground
alone. KXHIGHHOU, KXDENHIGH, KXHIGHTNYC and KXHIGHTCHI currently qualify.

## REPLAY-1: no execution study has ever been possible on this desk (2026-09-07)

Three facts, found while building the recorder.

**The `books` table has never held a row.** It is created at startup and nothing writes to it;
order books were only ever fetched on demand for the dashboard and thrown away. Across both
databases, zero rows. So at no decision point in this desk's history was there a recorded book,
and no study of what would have filled can be built from the past. That cannot be repaired
retroactively.

**The quote history is 32 hours long.** Two million rows, but spanning 2026-09-05 18:26 to
2026-09-07 02:19, top-of-book only, sampled about once a minute. Enough for a conservative
top-of-book fill model going forward; not depth, and not history.

**The weather studies cannot be reproduced.** WX-1b, WX-1c and WX-1d read `hist_markets` and
`hist_trades`. Neither table exists in either database any more. Their conclusions are written
down here as settled and nobody can re-run them.

### What this changes about WX-1b, WX-1c and WX-1d

Their Brier comparisons stand. The market beat the model at every hour and a day out, and that is
a calibration result which needs only prices and outcomes.

Their **return columns do not stand**, and should not be quoted. Those numbers came from taking
the decision at hour h and filling at the next print on our side within the following three hours.
A print three hours later is not evidence that an order submitted at hour h would have filled: it
is evidence somebody traded later, possibly at a price that moved because the thing being
predicted had become clearer. The trade simulation was hindsight-contaminated in a direction that
cannot be signed without the data that no longer exists.

This does not reverse the weather conclusion. It removes one of the two legs it was standing on.
The calibration leg alone is still enough to keep those models research-only.

### What is recorded now

An `observations` table that keeps four times apart instead of collapsing them into one:

| | |
|---|---|
| `valid_ts` | the time the value is about |
| `init_ts` | the model cycle a forecast came from - **not** when it could be read |
| `published_ts` | when the provider made it available; 0 means unknown, which is not zero lag |
| `received_ts` | when this desk actually had it, the only clock a live decision could use |

`ReplayCursor` serves rows by one of those clocks and has no way to be asked for anything later.
It counts what it withheld, so a replay can show it was offered the future and refused. It will
not run backwards. Under the published-time basis a row with unknown publication is withheld
rather than assumed instant, which is the specific mistake that turns NOAA's 12Z label into an
hour and a half of hindsight.

`Engine::record_books()` now snapshots the order book under every pending proposal and open
position, capped at twenty per cycle. Bounded on purpose: the book that matters later is the one a
decision was standing on, not all 2,192.

Studies must declare themselves prediction-only or execution (`research/execution.py`), and an
execution claim needs a fill inside a stated latency budget with the unfilled share reported
next to it. `wx_evening.py` and `wx_dayahead.py` are relabelled prediction-only.

## RISK-1: the desk could not explain its own cash, and a quarter of the account is invisible (2026-09-07)

Found while making the risk book authoritative. Two separate faults, one of which changed the
answer to a question Sam asked directly.

**The desk cannot see $25 of a $110 account.** On 2026-09-06 at 07:35 UTC, $25 moved to a
margined exchange instance. `/portfolio/balance` returns the sum of exchange shards 0-3, and that
instance is not among them: shard balances total $81.23 and `account.balance` is $81.23 to the
cent. So every equity figure, the daily stop, and every position cap has been computed on 77% of
the account, with no indication that the rest exists. `daily_equity` now reports
`untracked_transferred` and the engine says so out loud.

Whether that $25 is intact cannot be determined from here. All-time P&L is +$0.30 if it is and
-$24.70 if it is not, and the desk has no way to tell the difference. That is the finding.

**Cash reconciliation was impossible because closes were counted as purchases.** A Kalshi binary
pair nets: buying YES while holding NO on the same ticker closes the position and returns $1 of
collateral per contract immediately. Treating that as a purchase leaves the balance short by the
released collateral. Reconciling 2026-09-06 naively left **$46.00 unexplained**, which happened to
equal that day's settlement revenue exactly and so looked like a double count. With netting
modelled properly:

```
opens (cost)        52.78      settlement revenue   46.00
offsetting closes   46 contracts, $46.00 released
fees                 1.19      cashflows           -25.00
predicted cash move            -18.70
actual cash move               -18.70
UNEXPLAINED                      0.00
```

`reconcile_cash()` does this at runtime over the trading day and `daily_equity` carries the
residual. A balance the desk cannot account for no longer passes silently into the stop.

**Correction to the trading log.** On the evening of 2026-09-06 this desk was told its day was
-$17.06 and its all-time -$26.78. Both were wrong, and wrong the same way: they subtracted the
$25 transfer from equity without recognising it had also left the balance being measured. The
day was **+$10.02** and all-time is flat to within a dollar if the transferred $25 is intact. The
header figure that prompted the question was right and the correction offered against it was not.

## RISK-2: two contracts with different names were treated as opposite sides (2026-09-07)

On 2026-09-06 the desk held 7 Paris YES and then bought 8 Marseille NO in the same match. Those
are not opposite sides of anything. Paris YES pays if Paris wins; Marseille NO pays on a draw or
a Paris win. One outcome set contains the other, so the two legs win together in two states and
lose together in the third. The worst case went from $1.55 to $7.89 and the guard allowed it.

The guard asked: does the new leg pay in **any** state where the book is already at its worst? It
does - Marseille NO pays on a draw, and a draw was one of two equally bad states. So it passed.

The question it should have asked is the one the plan states: what happens to the worst case?

| | worst case | added cost | bought back |
|---|---|---|---|
| Paris YES, then Marseille NO | $1.55 -> $7.89 | $6.34 | **$0.00** |
| Paris YES, then Paris NO | $1.55 -> $0.05 | $5.55 | $7.05 |

`stack_check()` now compares the worst case before and after, and reports how much of the new
leg's cost was bought back. A leg whose full cost lands on the worst case has no state in which it
pays while the book is losing: it is the same bet again, whatever the two contracts are called.

Two consequences worth stating, because both look like bugs and are not:

**When two states tie for worst, no single leg can hedge.** A lone Paris position loses equally on
a Marseille win and on a draw. Covering one deepens the other by the whole cost. To cover two
states you have to buy in two of them, or buy the leg that pays in both - Paris NO. The old test
suite asserted that a single opposing bracket was a hedge; it was not, and the state it deepened
on 2026-09-05 is the one that settled.

**A three-way match is exhaustive even though it has no strikes.** `event_structure` proved
exhaustiveness by showing brackets tile the number line, which a 1X2 market cannot do. Without it
the book carried a "nothing settles yes" state that cannot happen, and in that phantom state every
leg loses - which makes every genuine hedge look like it buys nothing back. A match settled on the
result after ninety minutes plus stoppage ends home, draw or away, so `soccer_90` events with
three markets are now exhaustive by construction.

Also closed: hand tickets used to skip the stacking check outright ("Tickets can override"). They
still are not blocked, because a ticket is a deliberate act, but the check now runs and the result
is written to the event log. A manual click can still concentrate an event; it can no longer do it
without anything saying so.

## RISK-3: no model on this desk has earned Kelly sizing (2026-09-07)

Kelly answers "how much should I bet given this probability" and is worth exactly what the
probability is worth. The desk sized every proposal with quarter-Kelly regardless, which means an
unvalidated estimate was allowed to ask for a large allocation on its own say-so.

The gate is now: a strategy is sized by Kelly only once it has logged at least 200 fairs on
markets that have since settled, and scored a Brier no worse than the market mid it was quoting
against on those same markets. That is a paired comparison against the only opponent that matters
- a model can be well calibrated in absolute terms and still be strictly worse than the price.

Run against the live database on 2026-09-07:

| strategy | settled samples | Brier, model | Brier, market | sized by |
|---|---|---|---|---|
| `crypto_lognormal` | 4,292 | 0.1058 | **0.1009** | flat cap |
| `sports_live` | 626 | 0.0834 | **0.0723** | flat cap |
| `weather_nws` | 344 | 0.2946 | **0.1591** | flat cap |
| `sports_inplay` | 125 | 0.2587 | **0.2339** | flat cap |

**Every model on the desk is worse calibrated than the price it trades against.** All four fall
back to a flat cap of 2% of bankroll per order. `ladder_arb` is exempt, because a locked basket
does not rest on an estimate at all, and hand tickets are the operator's own decision.

`crypto_lognormal` is the closest, 5% worse in relative terms over 4,292 settled markets, which is
the one worth trying to close. `weather_nws` is 85% worse, which is the same verdict WX-1b, WX-1c
and WX-1d reached by three other routes.

Also added, so that every entry path meets the same limits: a per-order cap ($10 default, the
outermost check, refused before anything portfolio-shaped is consulted) and a daily turnover cap
($100 default). Turnover is a limit in its own right rather than a proxy for exposure: the taker
fee is 7% of p(1-p) each way, so round-tripping a position is the reliable way to lose money
slowly while every individual trade still looks fair.

## EXEC-1: the desk could not close a position, and killed itself trying (2026-09-07)

Found while checking why the universe had not rolled over: it had not rolled over because the
desk stopped running at 03:29, and it stopped because it had killed itself thirteen minutes
earlier.

```
09-06 19:35  EXIT REJECTED KXMLBGAME-26SEP061340SFNYM-SF
09-06 19:52  EXIT REJECTED KXMLBGAME-26SEP061340SFNYM-SF
09-07 01:47  EXIT REJECTED KXNCAAFGAME-26SEP06LOUMISS-MISS  -> KILL
09-07 03:16  EXIT REJECTED KXNCAAFGAME-26SEP06LOUMISS-MISS  -> KILL
   {"code":"invalid_order","details":"reduce_only can only be used with IoC orders"}
```

Two independent faults, either of which alone would have been survivable.

**The order was malformed.** `submit_exit` set `reduce_only` on every exit and chose
`good_till_canceled` for maker-style ones. Kalshi accepts `reduce_only` only on
immediate-or-cancel orders, so every resting exit was refused on sight. The `close()` path happens
to pair `reduce_only` with IoC and works, which is why closing by hand always worked and the
automated exit never did. This is the plan's own warning about confirming exchange semantics
rather than trusting a local label, arriving as a live incident.

**The refusal killed the desk.** A rejected order trips the kill switch, on the reasoning that the
desk and the exchange disagreeing about what is allowed means nothing more should go out until a
human looks. That is right for an entry and exactly backwards for an exit: it stops the machinery
that was trying to close a position and leaves the position open. The desk then sat dead for nine
and a half hours holding 5 short NCAAF contracts it had been trying to sell.

Fixed both ways. A resting exit no longer carries the exchange flag - the protection is enforced
locally instead, by sizing the order to the position that exists at submit time and cancelling
anything still resting in the kill sweep - and the proposal note records that the guarantee is
local rather than exchange-enforced. A refused exit now marks the proposal failed, logs that the
position is still open, and continues; only a refused entry still kills.

The kill switch was still on when this was found, and `sports_live` had been switched to `auto`
at 00:33, so the desk was unattended, killed, and holding.

## EXIT-1: what a position cost is not an argument for keeping it (2026-09-07)

Sam asked, of a Sassuolo position: what is a good place to exit, how much to take profit. The
honest answer is that "take profit" is the wrong frame, and the desk had no function that said so.

A $1 binary settles at $1 if the side held wins, so the expected settlement value of a position is
the probability of that times the size. What selling returns is the executable bid times the size,
minus the fee the sale itself costs. Holding is worth doing when the first exceeds the second.
That is the whole comparison.

The entry price is in neither number. It fixes the profit that gets reported and says nothing
about whether holding from here is attractive. The plan puts it exactly: a bid of $0.72 means
opposite things at a fair value of $0.66 and $0.85, and in neither case does it matter whether the
contracts were bought at $0.20 or $0.90.

`exit_decision()` makes that comparison and explains itself in a sentence per position:

| fair | bid | per contract | action |
|---|---|---|---|
| 0.66 | 0.72 | -0.045 | **close** - the market is paying more than the position is worth |
| 0.70 | 0.72 | -0.006 | **reduce** - close to fair, take part off |
| 0.85 | 0.72 | +0.144 | **hold** - selling gives away the reason for holding |

Three cases that are not the arithmetic, and matter more:

- **A stale model must not force a liquidation.** A number too old to enter on is too old to sell
  on. The position is frozen and additions stop; it is not dumped.
- **No model is not a reason to sell.** An uncovered market returns `unpriced` and holds, rather
  than acting on the absence of information.
- **A one-cent bid is not an exit.** Below `min_bid` there is no exit at any price, whatever the
  model thinks, and saying so is more useful than a technically-executable giveaway.

Every position in the risk book now carries its action and the sentence behind it. What remains
from session 6 is the trigger side: a goal, a card or a VAR correction should force a
reassessment and invalidate decisions made under the previous state, and the replay comparing
hold-to-settlement against fixed take-profit against advantage-gone needs the recorded sample
that started accumulating today.

## How a hypothesis becomes a strategy

1. Write the fair-value function (`fair(market) → p, sigma`) and its data fetch in `prepare()`.
2. Run it in `manual` mode with `market_weight` 0.5 for ≥ 2 weeks. Approve only what you would
   have traded by hand; the queue is the record.
3. Read the by-strategy row in the gate report. Move to `bot` only when the CI excludes zero,
   then lower `market_weight`.

---

# Results, 2026-09-11

Run `python3 scripts/test_hypotheses.py`. Only one of the 23 could be answered from what the desk
had recorded; the rest are blocked on data, and the section after this says what to do about that
rather than leaving it as an excuse.

## LONGSHOT (the executable core of SPORT-3, FIN-2 and the tail half of CRYPTO-3): rejected except in the last minutes

The claim: contracts priced in single digits settle YES less often than their price implies, so
selling them pays after fees. Tested as the trade actually available — sell YES on every leg of an
event whose mid sits between 2c and 10c, as a taker, held to settlement.

| price taken | legs | events | priced | settled YES | P&L per event | 95% CI | return on capital |
|---|---:|---:|---:|---:|---:|---|---:|
| at the close | 885 | 134 | 0.0334 | 0.0113 | +0.0501 | [+0.0050, +0.0952] | +0.77% |
| 5 min before | 825 | 132 | 0.0345 | 0.0109 | +0.0575 | [+0.0144, +0.1007] | +0.94% |
| 15 min before | 763 | 132 | 0.0380 | 0.0183 | +0.0266 | [−0.0268, +0.0799] | +0.47% |
| **30 min before** | 687 | 126 | 0.0425 | 0.0393 | **−0.0688** | **[−0.1368, −0.0009]** | **−1.30%** |

Thirty minutes before the close a 4.25c contract settles YES 3.93% of the time. **The price is
right.** The apparent edge appears only inside the last few minutes and it is the book lagging an
outcome that is already nearly decided, not a standing mispricing of longshots. At a 30-minute
horizon the trade loses, with the whole interval below zero.

Two things made the first cut of this look four times better, and both are worth naming because
they are easy to repeat:

- **The maker price.** Selling YES means lifting the NO offer at `1 - yes_bid`. Resting at
  `1 - yes_ask` assumes a fill nobody promised, and turned +0.77% into +2.9%.
- **No horizon.** Scoring at the last quote before settlement measures how fast the book updates,
  not whether the price was wrong.

What survives is small and awkward: under 1% on capital, tied up until expiration, available only
in the final minutes of a market, where you are the one crossing a thin spread. It is not a
strategy. It is a reason to look at where the book is slowest.

## What blocks the other 22, and what to do

The binding constraint is **not sources, it is time**: `quotes` reaches back to 2026-09-05. Six
days. Every hypothesis that needs a price at a horizon before settlement is starved of history,
and waiting is the slowest possible fix when the exchange gives the past away.

`scripts/backfill_kalshi_history.py` takes it: per series, every settled market with its outcome,
and the public trade prints across each market's whole life, into a separate `data/research.sqlite`.
Trade prints are better than the desk's own quotes for this, because they are executions rather
than a book the desk happened to be watching.

```
python3 scripts/backfill_kalshi_history.py --discover Crypto --max-markets 2000
python3 scripts/backfill_kalshi_history.py --series KXNFLGAME KXNBAGAME KXMLBGAME KXNCAAFGAME
```

That unblocks, with no new source at all: SPORT-3, FIN-2, SCI-1, CO-1 (the priced half), CRYPTO-2,
CRYPTO-3, WX-1, WX-2, COMM-1 and TRANS-1.

Still needing an outside source, none of them paid:

| hypothesis | needs | where |
|---|---|---|
| SPORT-2 | de-vigged closing line | The Odds API free tier, 500 requests a month |
| ECON-1 | inflation nowcast | Cleveland Fed, public CSV |
| ECON-2 | rate probabilities | CME FedWatch |
| TRANS-1 | checkpoint counts | TSA publishes a daily table |
| COMM-2 | weekly inventories | EIA open data API |

Needing data that must be recorded going forward and cannot be backfilled at all: MICRO-1,
SPORT-4 and WX-3 all turn on **receipt** times — when this desk learned a thing, against when it
happened. `include/desk/record.h` already carries the four-timestamp provenance for exactly this;
it is not yet wired into the book recorder. Until it is, any claim about reacting faster than the
market is untestable in principle, not just unmeasured.
