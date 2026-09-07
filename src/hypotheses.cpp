// The research registry. One row per hypothesis; status moves through
// idea -> collecting -> testing -> supported | rejected. docs/hypotheses.md has the long form.
#include "desk/server.h"

namespace desk {

void seed_hypotheses(Store& store) {
  struct H { const char *key, *cat, *title, *statement, *data, *test; };
  static const H rows[] = {
      // ---- Sports ----
      {"SPORT-1", "Sports", "EPL tied at 80', No-on-draw underpriced",
       "Among EPL matches tied at 80:00, a model using team strength (pre-match Elo / closing line) and live "
       "state (xG so far, shots, cards) identifies No-on-draw contracts priced below their eventual win "
       "probability after trading costs.",
       "In-play quotes for KXEPLGAME-*-TIE (quotes table, every top-of-book change); a live score/xG feed with "
       "minute stamps (API-Football or a free scores API); pre-match closing lines for team strength.",
       "For each match tied at 80:00: record NO price, model P(tie broken). Realized: settle YES/NO. Test "
       "mean(net edge) > 0 with bootstrap CI over >= 60 matches; check calibration by model-p bucket."},
      {"SPORT-2", "Sports", "Kalshi vs sharp closing line (CLV)",
       "When Kalshi's pre-kickoff mid deviates >= 4 cents from the de-vigged Pinnacle closing line, the sharp "
       "line is right more often; trading toward it has positive edge after the maker fee.",
       "Pre-kickoff quotes for KXEPLGAME/KXNFLGAME/KXNBAGAME (quotes table); Pinnacle or consensus closing "
       "odds (The Odds API or similar).",
       "Paired Brier: closing-line probability vs Kalshi mid over settled games; realized edge of trades in the "
       "direction of the sharp line when |diff| >= 4c, net of fees."},
      {"SPORT-3", "Sports", "Longshot bias in game markets",
       "Game contracts priced <= 10c win less often than their price implies; buying NO at 90-95c is positive "
       "expectation after the fee (which is small at extreme prices).",
       "Settled game markets and their last traded prices (Kalshi public /markets/trades, backfillable now).",
       "Bucket last prices (1-5c, 6-10c, 11-20c); compare win rate to mean price with binomial CIs; "
       "compute net return of buying NO at the ask in each bucket."},
      // ---- Crypto ----
      {"CRYPTO-1", "Crypto", "Hourly BTC/ETH brackets lag spot",
       "Bracket quotes on KXBTC/KXETH hourly markets update slower than spot; a lognormal fair from live spot "
       "with realized vol finds quotes >= 3c from fair that can be hit as a maker before they move.",
       "Live spot samples (ext_prices, every 20s); bracket quotes (quotes table); fills and settlements.",
       "Realized net edge of crypto_lognormal proposals (report by_strategy); adverse-selection check: spot move "
       "between proposal and fill vs the direction traded; Brier of fair vs market mid on settled brackets."},
      {"CRYPTO-2", "Crypto", "Bracket-implied vol exceeds realized vol",
       "The vol implied by the whole bracket price distribution each hour is systematically above realized "
       "1-hour vol, so far-from-spot brackets are overpriced; buying NO on tails is positive after fees.",
       "Full bracket curve snapshots per hour (quotes table) and spot history (ext_prices).",
       "Fit implied vol per hour by least squares on bracket mids; compare to realized vol distribution; "
       "simulate NO-on-tails returns at the ask net of fees."},
      {"CRYPTO-3", "Crypto", "Ladder bids sum past a dollar often enough to pay",
       "In a mutually exclusive event at most one bracket settles YES, so selling YES across brackets whose "
       "bids sum above $1 pays at least (legs - 1) whatever happens. On jumpy crypto ladders that sum crosses "
       "1 while the book is stale on one side; the question is how often, at what size, and whether all legs "
       "fill before it closes.",
       "Full ladder quote snapshots per event (quotes table), event_meta.mutually_exclusive, fills.",
       "Count of minutes per day where the bid sum exceeds 1 + fees by series and time to close; realized "
       "fills vs the locked edge (slippage and unfilled legs are the whole risk); size available at the touch."},
      {"CRYPTO-4", "Crypto", "Multi-coin coverage adds edge, not just rows",
       "The lognormal fair that CRYPTO-1 tests on BTC/ETH transfers to smaller coins (SOL, XRP, DOGE, LTC, ADA, "
       "AVAX, LINK), where books are thinner and quotes staler, so the same rule should find more mispricing "
       "per unit of size - if the wider spreads do not eat it.",
       "Coinbase spot per coin (ext_prices), bracket quotes per coin, settlements.",
       "Realized edge and Brier by coin, side by side with BTC/ETH; median spread and displayed size by coin; "
       "adverse selection between proposal and fill by coin."},
      {"WX-1c", "Climate and Weather", "Day-ahead temperature model vs the tape",
       "REJECTED 2026-09-06. The desk's day-ahead fair (NWS/ECMWF point forecast + bias, sigma 2.5F) "
       "was scored against 1,404 settled bracket markets using the archived one-day-ahead ECMWF run "
       "(Open-Meteo previous-runs). Market Brier 0.086, model 0.124, at every decision point from noon "
       "the day before to 6am on the day. The trading rule lost 3-5c per contract at edge thresholds of "
       "0.05, 0.08 and 0.12, with 90% intervals entirely below zero. Sweeping bias and sigma IN SAMPLE "
       "reached only 0.122, and blending toward the market improved Brier monotonically to a weight of "
       "1.0: the model carries no information the market has not already priced.",
       "hist_markets/hist_trades (research backfill), archived day-ahead forecasts (research/om_dayahead.csv).",
       "research/wx_dayahead.py. Re-run it if the model gains a source the market lacks; the gate to "
       "reopen is a Brier below the tape's out of sample, not a nicer-looking fair."},
      {"WX-1d", "Climate and Weather", "Same-day model after the daily peak",
       "REJECTED 2026-09-06. WX-1b only ever tested hours 9-16, so the evening - when the day's maximum "
       "is already an observed number and only the sensor shift is unknown - was untested. Extending the "
       "study to hours 9-23 over 2,448 settled markets: the market wins at every hour and pulls away after "
       "the peak (20:00 Brier 0.0011 market against 0.079 model). The market resolves the METAR-to-climate-"
       "report shift better than the measured +0/+1/+2 mixture does, so there is nothing left to trade.",
       "asos_*.csv hourly observations 2022-2026, hist_markets/hist_trades.",
       "research/wx_evening.py. A shift model built on 5-minute ASOS rather than hourly METAR is the only "
       "route back in, and the evening market Brier of 0.001 leaves almost nothing to win."},
      // ---- Commodities ----
      {"COMM-1", "Commodities", "WTI/NatGas settlement brackets vs futures",
       "In the last two hours before settlement, a fair from the front-month futures price with a short-horizon "
       "vol beats Kalshi bracket prices on KXWTI/KXNATGAS.",
       "Front-month futures quotes (delayed is fine for research; live for trading); quotes table; settlements.",
       "Paired Brier futures-fair vs market mid; realized edge of proposals; check the settlement price "
       "definition (daily settlement, not last trade)."},
      {"COMM-2", "Commodities", "EIA inventory markets vs API print",
       "The API Tuesday inventory number predicts the direction of the EIA Wednesday surprise; Kalshi weekly "
       "inventory brackets do not fully adjust after the API print.",
       "API and EIA weekly releases (manual or scraped); quotes around Tuesday 4:30pm ET.",
       "Regress EIA surprise on API surprise; test post-API price moves vs eventual settlement."},
      // ---- Climate ----
      {"WX-1", "Climate and Weather", "Same-day highs: observed max + remaining forecast",
       "After 1pm local, observed running max plus the remaining hourly forecast beats bracket prices; edge "
       "concentrates in the 'or above / or below' tails once the day's peak has passed.",
       "NWS observations and hourly forecast (ext_weather, built in); quotes table; settlements. Note: rules "
       "now cite The Weather Company as the settlement source - verify its value equals the NWS CLI daily max "
       "on past settled markets before trusting the obs feed.",
       "Realized edge of weather_nws proposals by hour-of-day bucket; Brier vs market mid by lead time; "
       "settlement-source agreement rate (TWC vs NWS CLI) on >= 60 past days."},
      {"WX-2", "Climate and Weather", "Day-ahead brackets: station-calibrated sigma",
       "A per-station forecast-error sigma (from NBM or from our own forecast-vs-settle history) prices the "
       "center bracket better than the market, which underprices it when models agree.",
       "ext_weather history (forecast vs settled value) per station; NBM if a fetcher is added.",
       "Estimate sigma per station from >= 40 days of history; Brier of N(fc, sigma_station) vs market."},
      // ---- Economics ----
      {"ECON-1", "Economics", "CPI brackets vs nowcast in the final 48h",
       "In the 48 hours before the CPI print, a distribution centered on the Cleveland Fed nowcast blended "
       "with consensus prices the KXCPI brackets better than the market, which overprices the tails.",
       "Cleveland Fed inflation nowcast (public), consensus estimate, KXCPI quotes, BLS outcomes.",
       "Paired Brier per release; realized edge of NO-on-tails; small n - accumulate 6+ prints before judging."},
      {"ECON-2", "Economics", "Fed decision markets vs fed funds futures",
       "When Kalshi Fed-decision prices diverge >= 5c from CME FedWatch-implied probabilities, prices revert "
       "toward futures before the meeting.",
       "FedWatch probabilities (daily), Kalshi Fed series quotes.",
       "Event study of divergences; realized edge trading toward futures with maker orders."},
      // ---- Financials ----
      {"FIN-1", "Financials", "Index close brackets lag the tape",
       "In the last 30 minutes before the close, a lognormal fair from the live index level with VIX/VXN-implied "
       "vol identifies stale KXNASDAQ100/KXINX brackets with >= 3c net edge.",
       "Real-time index level (thinkorswim / IBKR feed), VIX/VXN, quotes table, settlements.",
       "Realized edge of proposals in the final 30 minutes; Brier vs market mid; fill rate as maker."},
      {"FIN-2", "Financials", "Longshot bias in daily index brackets",
       "Daily index brackets priced <= 5c settle YES less often than priced; buying NO at 95c+ is positive after "
       "the (tiny) fee at extreme prices.",
       "Settled KXNASDAQ100/KXINX markets with last prices (public trades, backfillable).",
       "Same bucket test as SPORT-3."},
      // ---- Science and Technology ----
      {"SCI-1", "Science and Technology", "Release-date markets overprice on-time delivery",
       "Markets of the form 'X released by date D' price YES above the historical on-time rate for announced "
       "product/launch dates, which slip more often than not.",
       "Settled sci/tech date markets and their pre-settlement prices; a base-rate table of announced vs actual dates.",
       "Compare final-week YES prices to settle rate; test NO-at-ask returns net of fees."},
      // ---- Suggested additions ----
      {"TRANS-1", "Transportation", "TSA throughput brackets vs seasonal model",
       "A seasonal + year-over-year model of TSA checkpoint counts prices weekly/monthly throughput brackets "
       "better than the market.",
       "TSA daily throughput table (public), quotes table, settlements.",
       "Paired Brier vs market; realized edge of proposals; check weekend/holiday effects."},
      {"CO-1", "Companies", "Earnings beat/miss base rate",
       "'Beat consensus' markets are priced below the historical beat rate (~70% of S&P companies beat EPS "
       "consensus), so YES-on-beat is underpriced for large caps.",
       "Earnings calendar with consensus, beat history by company, quotes table.",
       "Compare pre-release YES price to realized beat rate by sector and size; net return of YES at ask."},
  };
  for (const auto& r : rows) {
    Hypothesis h;
    h.key = r.key;
    h.category = r.cat;
    h.title = r.title;
    h.statement = r.statement;
    h.data_needed = r.data;
    h.test = r.test;
    h.status = "";  // keep whatever status the row already has
    store.upsert_hypothesis(h);
  }
  store.exec("UPDATE hypotheses SET status='idea' WHERE status IS NULL OR status=''");
}

}  // namespace desk
