# Which model works on which Kalshi perp

Short answer: none of the ones tested, on this data. Written down so the next person does not
spend another day rediscovering it.

## What was tested

All 154,135 stored perp quotes, 20 instruments, 2.37 days at ~17-second resolution
(`scripts/perp_signals.py`). Five model families, each asked the same question -- does this signal
predict the perp's next move -- rather than asked to produce a backtest:

| family | signal at time t | fade if r < 0, follow if r > 0 |
|---|---|---|
| basis reversion / momentum | `(mid − index) / index` | the current rule assumes fade |
| index lead-lag | index return over the last 60s | does the perp chase the index? |
| perp momentum / reversal | perp mid return over the last 60s | |
| BTC cross-lead | BTC index return over the last 120s | does BTC lead the alts? |
| funding carry | funding rate | zero on 18 of 20 instruments: dead on arrival |

Horizons 1, 5, 15 and 30 minutes. Train on the first 60% of each instrument, confirm the sign and
significance on the held-out 40%.

## The result

**341 signal/horizon/instrument combinations across all six families. 17 expected to survive by
chance at 5%. 7 survived** -- and four of those were the basic families below
— KXADAPERP, KXBCHPERP, KXLINKPERP, KXXRPPERP, every one at r ≈ −0.08 and t ≈ 2.4, all at the
1-minute horizon. Fewer survivors than chance produces, at exactly the effect size chance
produces. The BTC cross-lead family was worse than chance on its own: 55 tested, 0 survived.

There is no directional edge in this data.

## The two mistakes that made it look like there was one

The first pass reported a surviving signal on **all 16 liquid instruments, every one negative**. A
result that uniform is not sixteen discoveries, it is one bug. There were two.

**Overlapping windows.** Quotes arrive every ~17s. Sampling every quote for a 30-minute forward
return gives ~106 overlapping observations per independent one, and every t-statistic is inflated
by about √106 ≈ 10. A reported t of −10 was really a t of −1. Fixed by stepping the sample forward
by a full horizon, so no two observations share a second. This alone took KXVVVPERP from
"r = −0.167, t = −10.0" to "r = −0.15, t = −0.9".

**Bid-ask bounce.** Signal and target were both built from `mid = (bid+ask)/2`. When the book
flickers, `mid(t)` is noisy, and any signal containing `mid(t)` predicts the reversal of its own
noise — which is why `basis` and `perp_ret` came back negative everywhere. Fixed by entering at
the *next* observation after the signal, so signal noise and entry noise are different draws.

Both corrections are in `scripts/perp_signals.py` and documented in its header. Any future study
on this data needs both.

## Why the basis was never going to work here

| | spread | median \|basis\| | p95 \|basis\| |
|---|---|---|---|
| KXBTCPERP | 0.4 bps | 2.2 | 4.7 |
| KXETHPERP | 0.4 bps | 0.8 | 3.1 |
| KXVVVPERP | 11.0 bps | 5.8 | 81.1 |
| KXNEARPERP | 17.0 bps | 3.2 | 16.6 |
| KXLINKPERP | 18.0 bps | 2.1 | 8.3 |

The instruments with a clean basis (BTC, ETH, GOLD, SILVER quote inside 0.5 bps) never dislocate
far enough to clear a 17 bps round trip. The instruments that dislocate (VVV, ZEC) have spreads of
11 to 15 bps, so most of what a "22 bps basis excursion" measures there is the spread itself: the
mid sitting on one side of a wide book, not a price away from fair. **No instrument on this venue
has both a tight spread and a large basis**, and the basis rule needs both.

The code makes this concrete. Whatever `entry_bps` is set to, entries are floored at 22 bps:

```cpp
double needed = fee_bps(t,false) + fee_bps(t,true) + 5.0;   // 5 + 12 + 5
if (std::fabs(basis) < std::max(entry_bps, needed)) continue;
```

On instruments whose whole p95 basis is under 10 bps, that floor is never reached. On instruments
where it is reached, the excursion is about one and a half spreads.

## What is worth doing next

Not another regression on this data. The open question is mechanical, not directional:

1. **Does a post-only entry at the touch fill?** Paper says 0 of 38, but paper is wrong by
   construction: it requires the touch to trade *through* the posted price, while a real maker
   fill happens *at* it. Only live trading answers this.
2. **What does the price do in the 60 seconds after a fill?** If the fills you get are the ones
   somebody wanted to give you, the drift eats the half-spread. That is what happened on 34 real
   maker fills in the event markets: +0.0078 captured, −0.0102 of drift at 60s, net nothing.

`scripts/perp_fills.py` reports both. The rule is configured to acquire them at $5 an entry --
about a cent a round trip -- across eight instruments spanning 3.7 to 18.6 bps of spread, so the
answer describes the book and not one market. That is what live perps trading is for right now.

## The touch-basis re-test, run

The spread contamination above has an obvious fix, so it was tried rather than left as a
suggestion: measure the dislocation at the side you would actually trade, and count it only when
the **whole book** is on one side of the index (rich even at the bid, or cheap even at the ask).
A wide spread cannot manufacture that signal.

**58 tested, 3 expected by chance, 3 survived** -- KXETHPERP at 1m and 5m, which are the same
signal at adjacent horizons and not independent, and KXSUIPERP at 5m. That is chance, exactly.

It also showed why the tight-spread instruments look different: the index sits outside the book
99% of the time on BTC, 95% on SILVER, 93% on GOLD and 64% on ETH. That is a standing offset
between the index and the perp, not a dislocation that reverts. On the alts it is 3% to 47%.

And the ETH result, the most tempting number in the table, cannot be traded whatever its t-stat:
ETH's p95 |basis| is 3.1 bps against a 17 bps round trip. The statistics and the economics have to
both work, and here only one of them does.

## Still untested

Order-book imbalance -- the most promising thing left, and it needs depth stored, which
`perp_quotes` does not keep (bid and ask only, no sizes). Adding size columns costs nothing and
would make the question askable in a week. Beyond that: horizons longer than 30 minutes, which
need more than 2.4 days of history.
