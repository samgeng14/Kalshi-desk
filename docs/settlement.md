# Settlement mapping

What each contract actually settles on, read from Kalshi's own rules text rather than assumed.
A series with no verified entry here is research-only in code: `weather_nws` refuses to produce a
tradable fair for any series whose `cli_product` is empty. Modelling a target nobody has
identified is how a desk spends two months fitting the wrong station.

## The source changed between July and September 2026

Both forms are still present in the local database, which is how the change was found.

| | rules text |
|---|---|
| July 2026 | "If the highest temperature recorded at **Chicago Midway, IL** for July 09, 2026 is between 86-87° according to **the National Weather Service's Climatological Report (Daily)**" |
| September 2026 | "If the maximum temperature recorded at **Chicago (CLIMDW)** for Sep 6, 2026, is greater than 82° fahrenheit according to **The Weather Company**" |

Kalshi announced the Weather Company partnership on 2026-08-27. The authority is now TWC, and the
named location moved from prose to a product code. TWC's own statement in that announcement is
deliberately broad: values carry "a documented method ... whether it comes from one official
station or is derived from several observation sources." So the rules do not, by themselves,
promise that the NWS product governs.

**Any study that pools July and September settlements is pooling two settlement regimes.**

## Verified: TWC's settled value equals the NWS single-station CLI maximum

Tested by intersecting the settled brackets for each station-day, which pins the value Kalshi used
to a one-degree interval, then reading the NWS climate report for the same date.

| product | settled value in | NWS CLI max, Sep 5 2026 | agrees |
|---|---|---|---|
| CLIAUS | [100, 101] | 100 | yes |
| CLIMDW | [83, 84] | 83 | yes |
| CLINYC | [79, 80] | 79 | yes |
| CLIMIA | [92, 93] | 92 | yes |
| CLIPHL | [84, 85] | 84 | yes |
| CLILAX | > 80 | 81 | yes |

Six for six, on one date. That is evidence the desk is modelling the right target, not proof that
TWC never diverges from the NWS product. Session 3's recorder should store the CLI text and the
settled result side by side so this becomes a standing check rather than a one-off.

## The verified map

Held in code as `settlement_specs()` in `src/universe.cpp`, which is the single source of truth;
this table is its prose form. `weather_nws` reads its station, zone and product from there.

| series | station | climate product | site | reports/day | verified |
|---|---|---|---|---|---|
| `KXHIGHAUS` | KAUS | CLIAUS | Austin-Bergstrom | ~250 | yes |
| `KXHIGHCHI` | KMDW | CLIMDW | Chicago-Midway | ~250 | yes |
| `KXHIGHNY` | KNYC | CLINYC | Central Park | **~38** | yes |
| `KXHIGHMIA` | KMIA | CLIMIA | Miami | ~250 | yes |
| `KXHIGHPHIL` | KPHL | CLIPHL | Philadelphia | ~250 | yes |
| `KXHIGHLAX` | KLAX | CLILAX | Los Angeles Intl | ~250 | yes |
| `KXLOWTCHI` (daily low) | KMDW | CLIMDW | Chicago-Midway | ~250 | yes |
| `KXLOWTMIN` (daily low) | KMSP | CLIMSP | Minneapolis-St Paul | ~250 | yes |
| `KXHIGHHOU` | KIAH | — | — | — | **no listed market to read** |
| `KXDENHIGH` | KDEN | — | — | — | **no listed market to read** |

`KXHIGHTNYC` and `KXHIGHTCHI` were in the station table and are gone: Kalshi returns 404 for both
series. `KXHIGHHOU` and `KXDENHIGH` exist as series but listed no market on 2026-09-06, so their
rules could not be read; they stay research-only.

Observation period is the local calendar day and rounding is to whole degrees Fahrenheit. Markets
close at **01:00 local** the following morning, not local midnight - 06:00Z for the central
cities, 05:00Z New York, 08:00Z Los Angeles - which is the one-hour window in which the day is
complete and the market still trades. Kalshi's expected expiration is 19:00Z that day. The climate
report publishes between about 05:50Z and 08:40Z, so the settling value is public roughly eleven
hours before Kalshi settles on it, with the market already closed. Session 3 should read it and
check TWC against it as a standing test rather than the one-off above.

Postponement and revision handling is not established: CLI products are reissued as corrections
and which issuance TWC takes is unknown. `research/wx_settle_basis.py` takes the latest. Open.

The daily-low shift is assumed symmetric with the daily high and has not been measured. The
mechanism is the same - the hourly series can miss the trough exactly as it misses the peak - but
that is an argument, not a measurement.

## Soccer

| series | league | settles on | zone |
|---|---|---|---|
| `KXBRASILEIROGAME` | Brasileiro Serie A (`bra.1`) | result after 90 minutes plus stoppage | America/Sao_Paulo |

Read from the rules on 2026-09-06: *"after 90 minutes plus stoppage time (does not include extra
time or penalties)"*. That is why `MatchPhase::Stoppage` is not a tradable phase - a model pricing
extra time would be pricing something the contract does not pay on.

One league to start, as the plan asks. Every other soccer series the desk watches has no
settlement entry, so the universe marks it unknown rather than assuming this wording carries over.

Soccer times run the other way round from weather: the close is a backstop three days after
kick-off while the expected expiration is hours after it, so the universe takes whichever comes
first. Taking the later would leave a finished match listed as tradable for days.

## The universe

`build_universe()` classifies every contract in its own zone: `future`, `tomorrow`, `today`,
`day_done` (its day is over locally, trading has not closed), `awaiting_settlement`, `settled`, or
`unknown`. `unknown` is a real answer - a contract with no settlement mapping is kept and marked,
never quietly dropped, because the gap is the finding.

Discovery reruns every five minutes and immediately whenever any settlement zone's local date
turns over. That trigger is the point: New York rolls three hours before Los Angeles, and a
universe refreshed on a single clock would keep offering yesterday's Los Angeles contract as
today's for those three hours.

## The shift the desk has to carry

The desk reads `api.weather.gov/stations/<id>/observations`, the hourly series. Settlement is the
climate report's maximum, taken from the continuous sensor record. The hourly series cannot see a
peak that falls between its reports, so it is biased low, and the size of that bias is a property
of how often the station reports rather than of the weather.

Measured directly against the climate report by `research/wx_settle_basis.py` on 2026-09-06:

| product | days | reports/day | mean shift | worst |
|---|---|---|---|---|
| CLINYC | 7 | 38 | **+0.87 °F** | +2.0 |
| CLIMIA | 2 | 250 | +0.60 | +0.6 |
| CLIMDW | 2 | 250 | +0.31 | +0.6 |
| CLILAX | 2 | 250 | +0.24 | +0.4 |
| CLIPHL | 2 | 250 | +0.20 | +0.6 |
| CLIAUS | 2 | 250 | −0.24 | −0.1 |

The model previously applied one global distribution — +0 63%, +1 31%, +2 6% — to every station,
and its comment claimed 210 settled station-days of support. It had none: that distribution was
fitted from which brackets won, and a winning two-degree bracket says the settled value fell in a
range, not what it was. The shift is now per station: `{0: 0.60, 1: 0.40}` for airport sites and
`{0: 0.29, 1: 0.57, 2: 0.14}` for Central Park, overridable per station in config.

Both are provisional. Ten airport station-days and seven at Central Park cannot support the tails,
and `api.weather.gov` only serves about a week of climate products, so the sample cannot be
extended backwards from this source. Session 3's recorder is what makes it grow; session 4 refits
it. What is already solid is the shape of the finding: **Central Park needs roughly four times the
correction an airport needs, and a single global constant is wrong in both directions at once.**
