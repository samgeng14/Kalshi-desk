# Can an options surface price a Kalshi contract at the horizon Kalshi settles on?

2026-09-12. `research/digital_surface.py`. The question is not whether a Kalshi contract is a
digital option, it plainly is, nor whether an options market prices digitals, it does. It is
whether the implied digital can be obtained **at the horizon the Kalshi contract actually settles
on**, because that is where the strategy either works or is an interpolation exercise.

## The machinery works, and that part is not in doubt

The digital is extracted two independent ways: model-free finite differences on quoted USD call
prices, and finite differences on Black-Scholes prices rebuilt from each strike's own `mark_iv`.
Across every strike and every expiry checked, **the two agree to within 0.0002**. So neither
Deribit's inverse (BTC-margined) payoff convention nor the choice of model is doing any real work,
and the extraction is sound and reusable.

## The problem is the calendar, and it is severe

| | |
|---|---|
| Deribit BTC/ETH options expire | daily, 08:00 UTC |
| Kalshi crypto markets settle | on the hour, UTC |
| clean expiry matches | **one per day per asset** |
| share of the watchlist that is hourly crypto | **31,023 of 33,027 markets** |

Every other hour requires pricing a horizon no option is listed at. The standard fix is to
interpolate total variance between bracketing expiries, and Deribit provides a free out-of-sample
test of exactly that: hold out a real expiry, rebuild it from the two around it, and score.

| bracket | held out | BTC mean err | BTC max | ETH mean err | ETH max |
|---|---|---:|---:|---:|---:|
| 1.9h -> 49.9h | 25.9h | **4.90c** | 8.97c | **3.24c** | 6.44c |
| 25.9h -> 73.9h | 49.9h | **1.71c** | 3.18c | **1.22c** | 2.31c |

Read together: interpolation is fine at multi-day horizons and degrades badly once the short end
enters the bracket. Reproduced independently from a second machine minutes later at 1.78c.

**The comparison that decides it.** The worked example for this lane treats a **5.6 cent** net
edge as the thing worth trading. On the hourly crypto markets the interpolation's own error is
3-5 cents and reaches 9. At that horizon we could not distinguish "Kalshi is mispriced by six
cents" from "our interpolation is off by six cents", which is not a strategy, it is a coin flip
wearing a volatility surface. Worse, for most of the day the nearest listed expiry is *further
out* than the Kalshi contract, so it is extrapolation below the shortest listed tenor rather than
interpolation between two.

## What data is actually reachable

| source | result |
|---|---|
| Deribit (BTC/ETH) | **HTTP 200**, free, public, and reachable from the desk's own machine |
| Yahoo `^NDX` chain | HTTP 429, unofficial, not a feed to build on |
| CBOE public chain | no public endpoint at the path tried |
| marketdata.app (SPX) | HTTP 401, paid |

So the index and commodity legs of this lane - Nasdaq, gold, WTI, natural gas, copper - need a
licensed options feed that this desk does not have. Without one, the lane is crypto-only.

## Verdict

The lane is real but its addressable set is far smaller than the 40% allocation assumes. It works
where a bracketing pair of expiries sits close in log-time, which means **multi-day crypto only**,
roughly 130 of the markets in the recorded set against 31,023 hourly ones. It does not work on the
hourly crypto markets, which is almost the entire watchlist.

Two things worth doing rather than abandoning it. Kalshi lists longer-dated BTC/ETH markets that
are not currently watched, and those are exactly where the method holds - adding them costs a
config line. And the surface is a good independent check on `crypto_lognormal`: where the two
disagree at a horizon the surface can price honestly, one of them is wrong, and that is worth
knowing whether or not it is ever traded.

Nothing here traded, and nothing here is a forecast. This is the feasibility gate, run before
the engineering rather than after it.
