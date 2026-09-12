# Is there any external price move left after realistic entry latency?

First pass, 2026-09-12. Source: `research/impulse_study.py` on 14.6 hours of the session-13
recorder's output (BTC and ETH, four external venues, Kalshi's perp ticker and book stream).
Raw table output alongside this file in `impulse_first_pass.txt`.

**Answer: no, and the reason is the opposite of the one the thesis assumed. On this recording
Kalshi's perp leads the external composite rather than lagging it.**

## What was measured

| | |
|---|---|
| recording | 14.6 hours, 2026-09-11T13 to 2026-09-12T04, with a ~7 hour gap where the recorder was down |
| external venues | Coinbase 292,602 quotes, Binance.us 148,672, Kraken 27,488, Hyperliquid 9,062 |
| events | ETH 526, BTC 164, at a 3 bps cross-venue move over 1 s with a 2 s cooldown |
| entry | taker, at the first Kalshi quote at or after signal + 250 ms |
| achieved latency | ETH median 788 ms, p90 1,182 ms (set by Kalshi's quote cadence, not by us) |
| causal audit | 8,704 realized components checked for ETH, 0 refused |

## The resolution limit, stated before the results

Kalshi's `ticker` channel publishes **once per second** per contract (median gap 1,000 ms, p90
1,004 ms, measured). Nothing about sub-second price response is measured here, and the tables say
"not measured" rather than interpolating.

The `orderbook_delta` stream is three hundred times denser and would give millisecond timing, but
**no snapshot was captured in this recording**, so the absolute book cannot be rebuilt from it and
no price is taken from it. It is used only for "when was the book busy", and even that came with a
control that refused to support a strong reading (below).

## The finding

ETH, 2-5 bps external moves, n=479. Percentages are of each series' own eventual move at 10 s.

| delay | external composite | | Kalshi quoted | |
|---|---:|---:|---:|---:|
| 2 s | +1.43 bps | 41% | **+3.19 bps** | **92%** |
| 5 s | +2.05 bps | 59% | +3.39 bps | 98% |
| 10 s | +2.16 bps | 62% | +3.11 bps | 90% |

At the first moment we can measure it, Kalshi has priced 92% of a move the external venues have
only completed 41% of. On 5-10 bps moves it reaches 122% of the move by 10 s: it overshoots.

That is what the markout table then shows, on the trade we could actually have made:

| horizon | n | mean markout | 90% CI (by event) | hit rate |
|---|---:|---:|---|---:|
| 250 ms | 512 | **-0.79 bps** | [-1.05, -0.54] | 38.1% |
| 500 ms | 512 | **-1.17 bps** | [-1.97, -0.61] | 38.1% |
| 1 s | 512 | **-0.98 bps** | [-1.77, -0.39] | 39.3% |
| 2 s | 512 | **-0.99 bps** | [-1.83, -0.35] | 42.2% |
| 5 s | 512 | -0.63 bps | [-1.52, +0.13] | 45.7% |
| 60 s | 512 | +0.75 bps | [-1.53, +2.91] | 47.1% |

Not "no edge". **Negative edge, with the interval excluding zero out to two seconds.** Entering in
the direction of an external impulse, 788 ms late, loses about a basis point on the midpoint over
the next two seconds and is right 38% of the time. The move is not merely gone by the time we can
act; we are on the wrong side of the reversion that follows it.

Executable P&L is -25.4 bps per trade at every horizon, which is the 24 bps of taker fees plus the
spread. That number carries no information about the signal and is reported only because leaving
it out would be worse.

## Baselines

Every direction rule was tested on the chronologically last 128 events. All of them lose on the
midpoint markout except B1, and B1 is "trade the direction the Kalshi perp has already moved",
which agrees with the external impulse 80% of the time and is a momentum bet on the venue itself
rather than a cross-venue lag trade.

| baseline | n | markout bps | agrees with impulse |
|---|---:|---:|---:|
| B0 random | 128 | -2.24 | 51% |
| B1 target venue (the perp's own move) | 111 | +1.12 | 80% |
| B2 Coinbase | 128 | -2.28 | 100% |
| B3 cross-venue median | 128 | -2.28 | 100% |
| B4 threshold only | 128 | -2.28 | 100% |
| B5 composite minus target (the lag baseline) | 128 | -1.48 | 74% |

B5 is the lag thesis stated as a rule: trade the part of the external move the perp has not taken
yet. It loses.

## What would overturn this

One caveat matters more than the rest. **Our "external composite" is Coinbase, Binance.us, Kraken
and Hyperliquid, and binance.com answers HTTP 451 from a US address.** If the venue that actually
leads global crypto price discovery is outside this set, then what has been measured is "Kalshi
leads these four venues", which is a weaker claim than "Kalshi leads the market" and is consistent
with both Kalshi and the true leader being ahead of our composite. Settling it needs a feed from a
venue that is plausibly the leader, from an address that can reach it.

The others, in order:

- **One session, and a quiet one.** 526 ETH events and 164 BTC events over 14.6 hours with a gap in
  the middle. Nothing here speaks to a volatile session, a scheduled release, or a weekend.
- **Sub-second response is unmeasured.** Kalshi's ticker is 1 Hz. This cuts against the lag thesis
  rather than for it - the first measurable point already shows 92% - but a faster channel could
  show the path there looks different from what the endpoints imply.
- **The book-activity control was ambiguous and is reported as such.** Book messages were already
  elevated (1.36x at -500 ms) before the detector fired, so the post-event elevation is not
  evidence of a reaction. An event stamped at the end of a one-second detection window is already
  up to a second old, which is enough to explain it. No claim about reaction time is made from it.

## What to fix in the recorder

The delta stream is unusable for price because no snapshot was captured. On the next restart of
`scripts/perps_recorder.py`, request one (`send_initial_snapshot`) or re-subscribe so the venue
sends it. That would turn the densest stream in the recording from a timing proxy into an
executable book, and it is the single change that would most improve every study here.

The recorder was deliberately **not** restarted to make this change, because it has been running
continuously since 13:00 and continuity is worth more than the fix.

## Status

CONDITIONAL-IMPULSE-V1: **no lag found, lane not pursued.** The measurement stands on one quiet
session against a possibly-lagging composite, so it is a reason to stop rather than a proof of
impossibility. Reopening it needs a leading external venue and a session with some volatility in
it, not a better model.
