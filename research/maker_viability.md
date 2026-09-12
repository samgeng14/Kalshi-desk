# Is there enough spread on Kalshi perpetuals to make a market?

Run before building FAIR-MAKER, because the whole strategy rests on capturing spread and the
question of whether the spread exists is answerable in two minutes from data already on disk.

**The answer is yes, on six of twenty contracts, and the binding constraint is not spread.**

## The screen

A one-sided maker who buys the bid and sells the ask captures the full quoted gap. The
maker/maker round trip is 10 bps. So the gap has to exceed 10 bps before spread capture pays for
itself, and every basis point below that has to come from directional alpha, which the minute-bar
study has already shown tops out around 2.8 bps.

| contract | gap (med) | net of 10 | $/min | 0-vol min | verdict |
|---|---|---|---|---|---|
| KXAAVEPERP | 15.3 | **+5.3** | 34 | 80% | spread alone clears |
| KXADAPERP | 13.5 | **+3.5** | 1,003 | 70% | spread alone clears |
| KXWLDPERP | 13.2 | **+3.2** | 502 | 68% | spread alone clears |
| KXBNBPERP | 10.8 | **+0.8** | 46 | 64% | spread alone clears |
| KXVVVPERP | 10.4 | **+0.4** | 500 | 59% | spread alone clears |
| KXKSHIBPERP | 10.1 | **+0.1** | 501 | 54% | spread alone clears |
| KXNEARPERP | 8.2 | −1.8 | 899 | 32% | clears at p75 |
| KXBCHPERP | 7.3 | −2.7 | 255 | 47% | p95 only |
| KXLINKPERP | 6.5 | −3.5 | 327 | 59% | p95 only |
| KXDOGEPERP | 5.7 | −4.3 | 494 | 49% | never |
| KXZECPERP | 5.5 | −4.5 | 1,208 | 32% | p95 only |
| KXLTCPERP | 4.3 | −5.7 | 796 | 48% | never |
| KXSUIPERP | 3.8 | −6.2 | 2,511 | 45% | p95 only |
| KXXRPPERP | 3.6 | −6.4 | 411 | 33% | never |
| KXHYPEPERP | 3.0 | −7.0 | 2,206 | 12% | never |
| KXSOLPERP | 2.7 | −7.3 | 498 | 34% | never |
| KXETHPERP | 1.1 | −8.9 | 45,580 | 2% | never |
| KXBTCPERP | 0.5 | −9.5 | 66,688 | 1% | never |

**BTC and ETH are ruled out for SPREAD CAPTURE, by arithmetic.** A 0.5 bps quoted gap against a
10 bps round trip cannot be rescued by any model, so they are never markets to quote passively for
the spread alone.

**This says nothing about trading them.** They remain first-class trading markets under a
directional thesis, where the return comes from a move large enough to clear the fee rather than
from the spread. `research/impulse_viability.md` screens that separately and finds 10 to 15
opportunities a day on BTC above 25 bps. An earlier version of this document said they were "ruled
out permanently", which overstated a claim about market making into a claim about the venue.

## The structure that matters

**The correlation between log(notional per minute) and the quoted gap is −0.64.**

Spread is wide exactly where flow is thin. That is not an inefficiency waiting to be harvested, it
is the market working: a maker widens where adverse selection per trade is higher and where
inventory takes longer to clear. The six contracts whose spread clears the fee have a median of
**$501 per minute of notional and 68% of minutes with no trade at all**. The six tightest have
$2,334 per minute and 12% empty minutes.

There is no contract on this venue with both a wide spread and heavy flow. The strategy has to
pick one, and FAIR-MAKER picks width.

## The ceiling, and why it is not the forecast

If a maker captured **100%** of the flow at the median gap on every qualifying contract:

| contract | $/day at 100% capture | at 10% | at 2% |
|---|---|---|---|
| KXADAPERP | 505.51 | 50.55 | 10.11 |
| KXWLDPERP | 231.32 | 23.13 | 4.63 |
| KXVVVPERP | 28.80 | 2.88 | 0.58 |
| KXAAVEPERP | 25.95 | 2.59 | 0.52 |
| KXKSHIBPERP | 7.21 | 0.72 | 0.14 |
| KXBNBPERP | 5.30 | 0.53 | 0.11 |
| **whole book** | **804.10** | **80.41** | **16.08** |

Ten percent of all flow on a $49.86 account is $80 a day, which is 161% daily. That number is
**not a forecast and should not be quoted as one.** It is an upper bound constructed by assuming
away every hard part, and it is useful only for what it rules in: the venue is not so thin that
the idea is arithmetically impossible, which is what this screen was run to find out.

Four reasons the realistic figure is a small fraction of it:

1. **Queue.** The flow figure is all traded notional. A maker earns only the share where they are
   at the front of the queue, competing with everyone else quoting.
2. **Adverse selection.** The trades that reach a passive quote are not a random sample of flow.
   They are disproportionately the ones that knew something. This is what the markout model exists
   to measure and it comes straight off the top of the table above.
3. **Emptiness.** Sixty-eight percent of minutes in these contracts have no trade. Most of the
   time the strategy is posting and waiting, not earning.
4. **Own size.** At $5 a clip in a market doing $34 a minute, as AAVE does, the quote is a
   meaningful share of the book and moves the price it is trying to trade against.

## What this changes about FAIR-MAKER

- **This universe is for passive quoting only.** BTC, ETH and the other tight contracts are
  traded through BTC/ETH IMPULSE instead, on large directional moves. Two strategies, two universes,
  one venue.
- **Quote universe is six contracts, not twenty**: ADA, WLD, AAVE, VVV, KSHIB, BNB by median gap,
  with NEAR added when its gap is in the upper quartile. Ranked every second by expected net EV,
  not quoted simultaneously.
- **ADA and WLD are the only two with both width and flow.** $1,003 and $502 per minute against
  gaps of 13.5 and 13.2 bps. If the strategy works anywhere it works there first, and if it fails
  there the rest will not save it.
- **Both are grade C**, 11 and 14 days of history. The backfill cannot answer questions about them
  and the recorder has hours. This is a reason to wait for data, not to widen the universe.
- **The constraint to model is fill rate and adverse selection, not spread.** The spread is
  present and measured. Whether a passive quote gets filled at it without being run over is the
  entire question, and it is what Models 3 and 4 are for.

## Caveat on the measurement

`close_quote_gap_bps` is `ask_close - bid_close` from a 1-minute bar. Nothing guarantees the two
closes share a timestamp, so this is a **coarse liquidity proxy and not a contemporaneous spread**.
The L2 recorder is the only source of the real thing. Every number above is a screen that decides
where to point the recorder, and each one must be recomputed from L2 before a single quote is
posted against it.
