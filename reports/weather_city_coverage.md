# Weather city coverage

Generated 2026-09-12 22:48:31Z. One row per listed daily temperature series with an open market. `verified` means the settlement product named in the contract's own rules exists at api.weather.gov, and so does the observing station - not that it appears in a hand-maintained table.


## Section 31: coverage

| | count |
|---|---|
| daily temperature series with an open market | **22** |
| settlement verified | **22** |
| of those, currently in `watch_series` | 5 |
| **verified but not watched** | **17** |
| in `watch_series` but not listed today | 3 |


## Every discovered city

| series | kind | city | product | station | auth | verified | in watchlist | note |
|---|---|---|---|---|---|---|---|---|
| `KXHIGHCHI` | high | Chicago | CLIMDW | KMDW | TWC | **yes** | yes |  |
| `KXHIGHDEN` | high | Denver | CLIDEN | KDEN | TWC | **yes** | **no** |  |
| `KXHIGHLAX` | high | Los Angeles | CLILAX | KLAX | TWC | **yes** | yes |  |
| `KXHIGHMIA` | high | Miami | CLIMIA | KMIA | TWC | **yes** | yes |  |
| `KXHIGHNY` | high | New York City | CLINYC | KNYC | TWC | **yes** | yes |  |
| `KXHIGHPHIL` | high | Philadelphia | CLIPHL | KPHL | TWC | **yes** | yes |  |
| `KXHIGHTATL` | high | Atlanta | CLIATL | KATL | TWC | **yes** | **no** |  |
| `KXHIGHTBOS` | high | Boston | CLIBOS | KBOS | TWC | **yes** | **no** |  |
| `KXHIGHTDAL` | high | Dallas | CLIDFW | KDFW | TWC | **yes** | **no** |  |
| `KXHIGHTDC` | high | Washington DC | CLIDCA | KDCA | TWC | **yes** | **no** |  |
| `KXHIGHTEWR` | high | Newark | CLIEWR | KEWR | TWC | **yes** | **no** |  |
| `KXHIGHTHOU` | high | Houston | CLIHOU | KHOU | TWC | **yes** | **no** |  |
| `KXHIGHTMIN` | high | Minneapolis | CLIMSP | KMSP | TWC | **yes** | **no** |  |
| `KXHIGHTOKC` | high | Oklahoma City | CLIOKC | KOKC | TWC | **yes** | **no** |  |
| `KXHIGHTPHX` | high | Phoenix | CLIPHX | KPHX | TWC | **yes** | **no** |  |
| `KXHIGHTSAN` | high | San Diego | CLISAN | KSAN | TWC | **yes** | **no** |  |
| `KXHIGHTSATX` | high | San Antonio | CLISAT | KSAT | TWC | **yes** | **no** |  |
| `KXHIGHTSEA` | high | Seattle | CLISEA | KSEA | TWC | **yes** | **no** |  |
| `KXLOWTBOS` | low | Boston | CLIBOS | KBOS | TWC | **yes** | **no** |  |
| `KXLOWTDC` | low | Washington DC | CLIDCA | KDCA | TWC | **yes** | **no** |  |
| `KXLOWTPHIL` | low | Philadelphia | CLIPHL | KPHL | TWC | **yes** | **no** |  |
| `KXLOWTTTN` | low | Trenton | CLITTN | KTTN | TWC | **yes** | **no** |  |

## Watched but not listed

These are in `config/desk.json` and have no open market under that ticker today. A watchlist entry that names a series Kalshi does not list is a city the desk believes it is covering and is not.

- `KXDENHIGH`
- `KXHIGHAUS`
- `KXHIGHHOU`


## What this does to the minimum detectable edge

MDE falls as the square root of independent city-days. The settled sample behind `reports/weather_capacity.md` accrues at the rate of the watchlist; discovery raises that rate to the verified count above. Nothing about the model changes - the same estimator simply becomes testable sooner, which is the whole argument for doing discovery before anything else.
