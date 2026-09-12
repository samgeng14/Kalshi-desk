# Weather city coverage

Generated 2026-09-12 23:15:16Z. One row per listed daily temperature series with an open market. `verified` means the settlement product named in the contract's own rules exists at api.weather.gov, and so does the observing station - not that it appears in a hand-maintained table.


## Section 31: coverage

| | count |
|---|---|
| daily temperature series with an open market | **16** |
| settlement verified | **16** |
| of those, currently in `watch_series` | 1 |
| **verified but not watched** | **15** |
| in `watch_series` but not listed today | 7 |


## Every discovered city

| series | kind | city | product | station | auth | verified | in watchlist | note |
|---|---|---|---|---|---|---|---|---|
| `KXHIGHCHI` | high | Chicago | CLIMDW | KMDW | TWC | **yes** | yes |  |
| `KXHIGHDEN` | high | Denver | CLIDEN | KDEN | TWC | **yes** | **no** |  |
| `KXHIGHTATL` | high | Atlanta | CLIATL | KATL | TWC | **yes** | **no** |  |
| `KXHIGHTNOLA` | high | New Orleans | CLIMSY | KMSY | TWC | **yes** | **no** |  |
| `KXHIGHTPHX` | high | Phoenix | CLIPHX | KPHX | TWC | **yes** | **no** |  |
| `KXHIGHTSAN` | high | San Diego | CLISAN | KSAN | TWC | **yes** | **no** |  |
| `KXHIGHTSFO` | high | San Francisco | CLISFO | KSFO | TWC | **yes** | **no** |  |
| `KXHIGHTTTN` | high | Trenton | CLITTN | KTTN | TWC | **yes** | **no** |  |
| `KXLOWTDC` | low | Washington DC | CLIDCA | KDCA | TWC | **yes** | **no** |  |
| `KXLOWTHOU` | low | Houston | CLIHOU | KHOU | TWC | **yes** | **no** |  |
| `KXLOWTLV` | low | Las Vegas | CLILAS | KLAS | TWC | **yes** | **no** |  |
| `KXLOWTMIN` | low | Minneapolis | CLIMSP | KMSP | TWC | **yes** | **no** |  |
| `KXLOWTPHIL` | low | Philadelphia | CLIPHL | KPHL | TWC | **yes** | **no** |  |
| `KXLOWTSATX` | low | San Antonio | CLISAT | KSAT | TWC | **yes** | **no** |  |
| `KXLOWTSEA` | low | Seattle | CLISEA | KSEA | TWC | **yes** | **no** |  |
| `KXLOWTTTN` | low | Trenton | CLITTN | KTTN | TWC | **yes** | **no** |  |

## Watched but not listed

These are in `config/desk.json` and have no open market under that ticker today. A watchlist entry that names a series Kalshi does not list is a city the desk believes it is covering and is not.

- `KXDENHIGH`
- `KXHIGHAUS`
- `KXHIGHHOU`
- `KXHIGHLAX`
- `KXHIGHMIA`
- `KXHIGHNY`
- `KXHIGHPHIL`


## What this does to the minimum detectable edge

MDE falls as the square root of independent city-days. The settled sample behind `reports/weather_capacity.md` accrues at the rate of the watchlist; discovery raises that rate to the verified count above. Nothing about the model changes - the same estimator simply becomes testable sooner, which is the whole argument for doing discovery before anything else.
