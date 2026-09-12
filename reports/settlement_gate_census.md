# Settlement-gate census: which models can pass, and on how many markets

Generated 2026-09-12 15:48:47 from `prod.sqlite`. Read-only.

Desk's last write: 15:34:01 (**886s ago**). Every freshness test below is measured against that cycle, not wall clock.

> The desk has not written for 14 minutes. Market status and books are as of its last cycle.

Active markets: **4583**, of which **4583** are in `watch_series`.


## Per model: covered, and how much of that clears the settlement gate

| model | mode | covers | watched & VERIFIED | pass rate | of those, two-sided book |
|---|---|---|---|---|---|
| `crypto_lognormal` | research | 2948 | **2948** | 100.0% | **114** |
| `xvenue` | manual | 778 | **0** | 0.0% | **0** |
| `(uncovered)` | - | 385 | **0** | 0.0% | **0** |
| `sports_inplay` | research | 258 | **0** | 0.0% | **0** |
| `weather_nws` | manual | 111 | **0** | 0.0% | **0** |
| `sports_live` | research | 103 | **103** | 100.0% | **91** |

## Markets a passing model could verify, taken first by a model that cannot

| model that would verify | taken by | markets |
|---|---|---|
| `sports_live` | `xvenue` | 604 |
| `sports_live` | `xvenue (valid fair)` | 316 |
| `sports_live` | `xvenue (NO usable fair)` | 288 |

## The researchable universe, by series

| model | series | verified | two-sided book | median volume | median OI | median spread |
|---|---|---|---|---|---|---|
| `crypto_lognormal` | `KXETH` | 1640 | **44** | 0.000 | 0.000 | 0.040 |
| `crypto_lognormal` | `KXBTC` | 1308 | **70** | 0.000 | 0.000 | 0.030 |
| `sports_live` | `KXMLBGAME` | 68 | **60** | 462617.710 | 294275.250 | 0.010 |
| `sports_live` | `KXNCAAFGAME` | 27 | **24** | 34634.780 | 28660.640 | 0.010 |
| `sports_live` | `KXNBAGAME` | 6 | **6** | 10987.330 | 5568.990 | 0.060 |
| `sports_live` | `KXNFLGAME` | 2 | **1** | 24288319.250 | 13676436.760 | 0.010 |

## Why each zero is a zero

**`weather_nws`** - the gate rejects any contract whose rules name The Weather Company. Of 111 active weather markets, **111 name TWC and 0 name the NWS**. Kalshi moved the settlement authority to TWC on 2026-08-27 (docs/settlement.md), so the predicate now excludes the entire live universe. `docs/settlement.md` records TWC agreeing with the NWS CLI maximum 6 times on one date - evidence, not the standing check the gate would need.

**`sports_inplay`** - the gate requires a series mapped `soccer_90`. Listed soccer series: `KXBUNDESLIGAGAME`, `KXEPLGAME`, `KXLALIGAGAME`, `KXLIGUE1GAME`, `KXMLSGAME`, `KXSERIEAGAME`. Mapped: **none**. The one mapped series (`KXBRASILEIROGAME`) lists no active market, so no covered contract can pass. Unlock is reading 6 series' rules and extending `settlement_specs()` - no model change.

**`xvenue`** - returns `false` from the gate by design (payoff parity with Polymarket was read for some leagues, never verified), and it sits *second* in the registry, ahead of every model that can pass. At the desk's last cycle it held **778** pairs, of which **396 produced a usable fair** (two-sided and <=90s, what `XVenue::fair` requires) and **382 did not**. Either way the gate blocks it, so its whole book can only ever be Research - and it takes **604** game markets that `sports_live` verifies unconditionally, **288 of them while producing no usable fair of its own**.


## Where research effort belongs

Ranked by verifiable markets that actually have a price to be wrong about.

| lane | verifiable markets | live books | verdict |
|---|---|---|---|
| `sports_live` | 707 | 91 + the blocked 604 | **research this first** - tightest books on the desk |
| `crypto_lognormal` | 2948 | 114 | second - passes everywhere, priced almost nowhere |
| `sports_totals` | passes unconditionally | **0** | no universe: `params.sports_totals.markets` is empty |
| `manual_fair` | passes by definition | your own | not a research lane |
| `xvenue`, `weather_nws`, `sports_inplay` | **0** | - | no effort until the gate can be passed |

Two unlocks, both data rather than model:

1. Registry order. `xvenue` is 2nd in `strategies/registry.cpp`, ahead of every model that can pass; moving it below `sports_live` moves **604** markets from unverified to verified without touching a model.

2. Soccer specs. Reading the 6 listed soccer series' rules and extending `settlement_specs()` with `soccer_90` would let `sports_inplay` reach its **258** covered markets.

Weather is a measurement project before it is a lane: the standing TWC-vs-CLI check that `docs/settlement.md` asks for has to exist before the predicate can honestly accept TWC.

