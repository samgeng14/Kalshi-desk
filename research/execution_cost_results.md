# Execution cost: the measured answer

17,853,866 recorded L2 events across 18 Kalshi perpetuals, 2.5 hours, 55,272 simulated orders,
40,358 fills. Four execution modes, two queue bounds, markouts to five minutes.

**72 (market, mode) pairs reached 50 or more fills on the pessimistic queue bound. Zero are
profitable.** Best −1.57 bps, median −7.26, worst −30.60.

That is not the interesting part. The interesting part is why, because it is the same reason in
every market and it is not a reason more data or a better model can change.

## The spread is not revenue

A passive fill earns the half-spread and then pays whatever the market takes back for having
traded with you. Measured across 18 markets, resting at the touch:

| market | spread | half-spread | 60s markout | kept | maker fee | net |
|---|---:|---:|---:|---:|---:|---:|
| LINK | 18.1 | 9.1 | −6.61 | **+2.45** | 5.0 | −2.66 |
| WLD | 16.6 | 8.3 | −8.76 | **−0.47** | 5.0 | −5.73 |
| BCH | 15.8 | 7.9 | −8.72 | **−0.81** | 5.0 | −5.98 |
| SUI | 15.4 | 7.7 | −5.55 | **+2.14** | 5.0 | −3.00 |
| KSHIB | 15.4 | 7.7 | −5.88 | **+1.81** | 5.0 | −3.32 |
| NEAR | 14.9 | 7.5 | −6.71 | **+0.74** | 5.0 | −4.24 |
| ZEC | 14.3 | 7.1 | −6.46 | **+0.66** | 5.0 | −4.42 |
| AAVE | 14.0 | 7.0 | −8.90 | **−1.88** | 5.0 | −7.01 |
| VVV | 10.0 | 5.0 | −6.28 | **−1.27** | 5.0 | −6.36 |
| ADA | 9.6 | 4.8 | −3.92 | **+0.87** | 5.0 | −3.37 |
| BNB | 9.6 | 4.8 | −6.05 | **−1.26** | 5.0 | −6.22 |
| DOGE | 8.3 | 4.1 | −4.75 | **−0.62** | 5.0 | −4.90 |
| HYPE | 7.0 | 3.5 | −3.19 | **+0.33** | 5.0 | −4.32 |
| LTC | 6.2 | 3.1 | −2.58 | **+0.50** | 5.0 | −4.11 |
| XRP | 4.4 | 2.2 | −2.59 | **−0.41** | 5.0 | −5.05 |
| SOL | 3.3 | 1.6 | −1.90 | **−0.25** | 5.0 | −4.85 |
| ETH | 0.8 | 0.4 | −0.97 | **−0.58** | 5.0 | −5.48 |
| BTC | 0.5 | 0.3 | −0.55 | **−0.29** | 5.0 | −5.22 |

**Correlation between the quoted spread and the 60-second markout: −0.905.**

The spread is priced. It is compensation for the information in the flow that crosses it, and the
compensation is nearly exact — the markout scales with the spread almost one for one. What
survives, the `kept` column, runs from −1.88 to +2.45 bps with a mean of **+0.09**.

Against a 5 bps maker fee, the fee is roughly **55× what the market leaves on the table**.

## This kills FAIR-MAKER as specified

The maker viability screen found 6 of 20 contracts with a median quoted gap above the 10 bps
maker/maker round trip — AAVE 15.3, ADA 13.5, WLD 13.2, BNB 10.8, VVV 10.4, KSHIB 10.1 — and
concluded those were the candidates. It was measuring the gross spread.

Four of those six are in the worst half of the `kept` column, and AAVE, the widest, is the single
worst market on the board at −1.88. The screen ranked the contracts by exactly the quantity that
turns out to predict adverse selection, so it ranked them close to backwards.

The earlier finding that the spread is widest where flow is thinnest (correlation −0.64 between
log notional and gap) had the right shape and the wrong conclusion. Thin flow does not mean an
unexploited spread. It means the few people who do trade there are the ones who had a reason to.

## BTC and ETH are the cheapest markets, not the worst

Crossing the spread, one way:

| market | cost to cross | of which fee |
|---|---:|---:|
| BTC | 12.32 | 12.00 |
| ETH | 12.50 | 12.00 |
| SOL | 14.12 | 12.00 |
| XRP | 14.54 | 12.00 |
| LTC | 15.58 | 12.00 |
| ... | | |
| BCH | 20.15 | 12.00 |
| WLD | 20.24 | 12.00 |
| LINK | 21.30 | 12.00 |

BTC's entire non-fee cost of crossing is 0.32 bps. Its book is 0.5 bps wide and its adverse
selection is 0.55 bps. Every microstructure quantity in that market is an order of magnitude
smaller than the fee charged to act on it.

This inverts the ranking the maker screen produced. On a fee-dominated venue the right market is
the tightest one, because the fee is flat and everything else is not.

## What every earlier study was too generous about

The minute-bar work, the impulse viability work and the maker screen all used a 17 bps
maker/taker round trip taken from the fee schedule. The measured round trip is worse:

- best passive entry measured: LINK `improve`, −1.57 bps net at 60s
- crossing out of LINK: 21.30 bps
- a realistic LINK round trip: about **25 bps**, not 17
- on BTC, where passive fills are nearly free of selection: about **17.5 bps**, which is the only
  place the old assumption was close

So every negative result already on the scoreboard is more negative than it was recorded as, and
the 33 Bonferroni survivors that lost 14–20 bps against a 17 bps hurdle lose more against this
one. Nothing is rescued by re-pricing. The direction of the correction is uniformly wrong for the
strategies.

## What this rejects, stated precisely

This experiment measured one thing:

```
E[kept] < 0     for an UNCONDITIONAL quoting policy
```

where `kept` is the half-spread earned at entry plus the markout after the fill. Orders were
placed on a fixed schedule, in every state, and the result is the average over all of them.

An earlier version of this document said "at 5 bps, no feature, horizon, model class or execution
mode closes the gap." **That claim is not supported by this experiment and has been withdrawn.**
An average over every fill is silent about whether a subset chosen in advance behaves
differently, and the −0.905 correlation is itself evidence of structure — precisely the thing a
conditional rule exists to exploit rather than average over. The untested claim is:

```
E[kept | state before the fill] > maker fee + exit cost   for some selection rule
```

That is a different question and it needs its own experiment, which is
`research/conditional_maker.md`. What this study rejects is indiscriminate quoting, not
conditional quoting, and not directional trading, and not funding capture.

## The fee is tiered, which changes the frame

The 5 bps maker and 12 bps taker figures used throughout are **tier 0**. Kalshi's perpetual
futures fees step down with 30-day trailing volume, verified against the CFTC rule filing of
24 June 2026 (`rules0624267243.pdf`, effective no earlier than 8 July 2026):

| 30d volume | taker | maker |
|---:|---:|---:|
| $0 | 12.0 | 5.0 |
| $100k | 10.0 | 4.0 |
| $300k | 8.0 | 3.2 |
| $1m | 6.0 | 2.4 |
| $3m | 5.0 | 2.0 |
| $10m | 4.0 | 1.6 |
| … | … | … |
| $3b | 2.6 | 0.6 |

Trailing volume counts both maker and taker volume, and tiers recompute daily.

So "quoting LINK loses money" and "quoting LINK is impossible on this venue" are different
claims, and only the first was measured. LINK `improve` keeps **+3.45 bps** before fees. It
crosses zero on the entry leg at **tier 2** — $300k of 30-day volume, 3.2 bps maker.

That does not help this account, and chasing the tier would be a mistake: volume is charged at
the rate held while it is generated, so trading up to tier 2 from zero costs about **$130** in
maker fees, and tier 3 about **$354**. On a $50 account that is not a discount, it is the account
several times over. The full arithmetic is in `reports/fee_ladder.md`.

## And there are no incentives to offset it

The effective cost of a maker fill is

```
F_effective = F_exchange − E[volume reward] − E[liquidity reward]
```

Both reward terms were checked against the live public endpoint rather than assumed to be zero.
Of **12,250 active incentive programs**, none covers a perpetual future: no PERP ticker appears
in any program and no `margin_maker_volume` or `margin_taker_volume` program exists, in any
status. The liquidity programs that do exist are also out of reach on size — the smallest Target
Size on the venue is 300 contracts a side, which costs about $294 of collateral to hold two-sided
on a binary, against a $50 account.

So on perps, `F_effective = F_exchange`. Details and the caveats in
`reports/perps_incentives.md`.

## The one lever, restated

- BTC: 0.5 bps of spread, 0.55 bps of adverse selection, 5 bps maker fee.
- Mean spread kept across all 18 markets after adverse selection: +0.09 bps.
- Best single market before fees: LINK `improve` at +3.45 bps.
- Tier-0 maker fee: 5 bps. No rebate available.

For unconditional quoting the gap is arithmetic and only the fee closes it. Whether a
*conditional* rule can find states where the kept spread is several times its unconditional mean
is the open question, and it is open because this study did not ask it.

## What this result does not say

- It is 2.5 hours of one session. Intraday, day-of-week and regime variation are not in it, and
  adverse selection is exactly the kind of quantity that changes with regime.
- The simulated orders were never in the book. Nobody queued behind them and nobody stepped in
  front, so passive fill rates are an upper bound, and `improve` most of all — its fills partly
  come from a price-priority rule that assumes our inside order would have been hit first.
- Markouts are measured against the mid. The exit is a second spread, and the round-trip figures
  above add it by assumption rather than by measurement.
- A resting order here never repositions. Real quoting cancels and replaces, which raises fill
  rate and worsens the markout at the same time — the net effect on this table is not known.

The pessimistic and optimistic queue bounds agree throughout: the largest gap between them on any
market is 1.0 percentage point of fill rate, and no conclusion here flips across it. That is the
one place this result is unusually safe.

---

## Correction, 2026-09-12: the maker fee in this table is wrong

The `maker fee` column says 5.0 bps throughout. The account's actual tier-0 maker rate, read from
`/margin/fee_tier_rates`, is **2.0 bps**. The taker rate of 12.0 bps is confirmed and unchanged.

That moves every `net` figure in the table above by +3.0 bps, and it moves the headline claim:
the best (market, mode) pair was reported at **-1.57 bps**, which at the correct fee is **+1.43
bps**. Several pairs cross zero.

**This does not reopen the lane, because a second error runs the other way.** The `kept` column is
computed as half-spread-at-post plus markout-from-fill, which credits a spread against a mid that
had already moved — the contamination documented in `reports/conditional_maker.md`, worth about
6.36 bps in the optimistic direction. So this table carries two errors of opposite sign: too
pessimistic by 3.0 on the fee, too optimistic by roughly 6.4 on the metric. The net is that the
maker lane is *worse* than reported, not better.

Neither number should be used as it stands. The lane needs a clean re-run at 2.0 bps with the
corrected `kept` definition before anything is concluded from it in either direction, and until
that happens `REJECTED_V1` stands on the round-trip evidence in `reports/exit_engine.md` rather
than on this table.
