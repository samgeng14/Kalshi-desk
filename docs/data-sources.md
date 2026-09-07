# Data sources

Checked from the build sandbox on 2026-09-05. "verified" = the endpoint returned the data
shown, no key. "key" = free registration. "Mac only" = the host blocks datacenter IPs but works
from a home connection or a browser. Everything the desk fetches lands in SQLite
(`ext_prices`, `ext_weather`, `quotes`, ...) so research never depends on the source staying up.

## Kalshi itself (verified)

| what | endpoint | notes |
|---|---|---|
| series metadata, fees | `GET /series`, `GET /series/{t}` | `fee_type`, `fee_multiplier`, `category`, `frequency`; 13.8k series |
| open markets, top of book | `GET /markets?series_ticker=&status=open` | `yes_bid/ask_dollars`, sizes, strikes, rules; public |
| full book | `GET /markets/{t}/orderbook` | public |
| trade tape (history!) | `GET /markets/trades?ticker=&min_ts=` | backfill for SPORT-3 / FIN-2 / CRYPTO-2 |
| candlesticks | `GET /series/{s}/markets/{t}/candlesticks` | hourly/daily OHLC |
| settlement source (weather) | `weather.com/kalshi` | JS page; the number the exchange settles on. Scrape from a browser for the WX-1 agreement check |

## Crypto

| what | source | status | feeds |
|---|---|---|---|
| spot BTC/ETH | `api.coinbase.com/v2/prices/{sym}-USD/spot` | verified, wired | crypto_lognormal |
| implied vol index (30d) | Deribit `public/get_volatility_index_data?currency=BTC` (DVOL) | verified, **wired** | vol input; BTC 38, ETH 51 on 9/5 |
| option surface (mark_iv per strike/expiry) | Deribit `public/get_book_summary_by_currency?currency=BTC&kind=option` | verified | short-dated skew for tails (CRYPTO-2) |
| settlement index BRTI/ERTI | CF Benchmarks | registration | Coinbase/Kraken/Bitstamp composite; spot is a near-perfect proxy |

DVOL is annualized and 30-day; intraday realized vol clusters, so the strategy blends
DVOL with its own 1-minute realized estimate (`ext_prices`).

## Climate and Weather

| what | source | status | feeds |
|---|---|---|---|
| hourly forecast (NWS point) | `api.weather.gov/points/{lat},{lon}` → `forecastHourly` | verified, wired | weather_nws |
| station observations | `api.weather.gov/stations/{KNYC}/observations?limit=60` | verified, wired | same-day running max |
| ensemble forecast (ECMWF 51 members, GFS 31) | `ensemble-api.open-meteo.com/v1/ensemble?models=ecmwf_ifs025&hourly=temperature_2m` | verified, **wired** | per-station daily-max mean and spread (WX-2); NYC 9/6: mean 74.3, sd 1.1 |
| NBM (National Blend of Models) text | NOMADS `blend/prod` | not reachable from here | optional later |
| settlement value | The Weather Company via `weather.com/kalshi` | JS page | WX-1 agreement check |

Ensemble spread is under-dispersed; the strategy inflates it (×1.6) and floors sigma at 1.5F.

## Sports

| what | source | status | feeds |
|---|---|---|---|
| live scores, clock, box score | ESPN `site.api.espn.com/apis/site/v2/sports/soccer/eng.1/scoreboard` and `.../summary?event=` | verified | SPORT-1 live state: score, minute, shots, on target, possession, cards, corners; `keyEvents` with clock; pre-match odds in `pickcenter` |
| sharp pre-match odds (Pinnacle) | The Odds API `v4/sports/soccer_epl/odds?regions=eu&markets=h2h` | key (free tier, ~500 req/mo) | SPORT-2 closing line; historical odds from 2020 on paid tiers |
| xG (post-match) | understat.com (page-embedded JSON) | scrape | SPORT-1 model fitting, not live |
| fixtures/results backup | football-data.org v4 | key (free tier: PL) | |

ESPN has no live xG. Score + minute + shots on target + red cards + pre-match line is enough
for a first SPORT-1 model; add xG later if the residuals say so.

## Commodities

| what | source | status | feeds |
|---|---|---|---|
| WTI / NatGas front month (delayed 10-15 min) | Yahoo `query1.finance.yahoo.com/v8/finance/chart/CL%3DF` | Mac only (429 from datacenter) | COMM-1; settlement is the CME daily settle, published ~2:30pm ET |
| daily spot/settle history | EIA API v2 (`api.eia.gov`, series RWTC etc.) | key (free) | COMM-1 backtest, COMM-2 |
| weekly inventories | EIA Weekly Petroleum Status Report | key | COMM-2 |
| CME site | cmegroup.com quotes JSON | blocked (anti-scrape) | do not use |

## Economics

| what | source | status | feeds |
|---|---|---|---|
| CPI / core CPI nowcast | Cleveland Fed inflation nowcasting page (HTML tables, updated each business day ~10am ET) | verified (scrape) | ECON-1; 9/4 print: Sep CPI m/m 0.38, core 0.19 |
| release calendar, actuals | BLS API v2 `api.bls.gov/publicAPI/v2/timeseries/data/CUSR0000SA0` | key (free; unkeyed quota is shared and exhausted) | ECON-1 outcomes |
| fed funds futures (ZQ) | Yahoo `ZQ=F` or CME FedWatch page | Mac only / manual | ECON-2 |

## Financials

| what | source | status | feeds |
|---|---|---|---|
| index level (real time) | Schwab Trader API (free with a Schwab/thinkorswim account) or IBKR TWS API | account | FIN-1 needs sub-minute data |
| index proxy (real time, free) | Alpaca Market Data (IEX feed) or Finnhub: QQQ, SPY | key (free) | QQQ ≈ NDX/41, SPY ≈ SPX/10; good enough for the last-30-min model |
| VIX / VXN | Yahoo `^VIX`, `^VXN` (delayed) | Mac only | vol input |
| delayed everything | Yahoo chart API | Mac only | research |

## Science and Technology

No feed. Build a base-rate table from settled Kalshi date markets (`GET /markets?status=settled&series_ticker=`)
plus the announced date at market open. Manual, ~1 hour of work per 50 markets.

## Suggested categories

| what | source | status |
|---|---|---|
| TSA checkpoint throughput (daily) | tsa.gov/travel/passenger-volumes | Mac only (Akamai blocks datacenter); HTML table, one row per day |
| earnings calendar with consensus EPS and # estimates | Nasdaq `api.nasdaq.com/api/calendar/earnings?date=YYYY-MM-DD` | verified |

## Priority order for wiring

1. Done today: Deribit DVOL (crypto vol), Open-Meteo ensemble (weather sigma).
2. ESPN live state logger for EPL (no key): start collecting the SPORT-1 sample now, model later.
3. The Odds API key → pre-match Pinnacle line (SPORT-2, and the team-strength input to SPORT-1).
4. Cleveland Fed scraper + BLS key (ECON-1) — two files, run daily.
5. Yahoo delayed futures from the Mac (COMM-1), then EIA key for history.
6. Real-time index feed (FIN-1) only when you decide to trade the close; Schwab or IBKR.
