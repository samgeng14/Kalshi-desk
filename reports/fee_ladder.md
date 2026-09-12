> **CORRECTED 2026-09-12 — THIS IS NOT THIS ACCOUNT'S SCHEDULE.**
>
> The ladder below was taken from a CFTC rule filing and treated as authoritative. The account's
> own `/margin/fee_tier_rates` endpoint disagrees, and it is the authority:
>
> | tier | maker bps | taker bps |
> |---:|---:|---:|
> | 0 | **2.0** | 12.0 |
> | 1 | 1.5 | 10.0 |
> | 2 | 1.2 | 6.0 |
> | 3 | 0.0 | 2.0 |
> | 4 | 0.0 | 1.5 |
> | 5 | 0.0 | 1.0 |
>
> Schedule name: `kalshi_prime`. Six tiers, not eleven. **Taker at tier 0 matches at 12.0 bps and
> is confirmed independently by recorded fills** (XRP paid 0.0103 on 8.5026 of notional, ETH
> 0.0090 on 7.4484 — both 12.1 bps including rounding). **Maker does not: 2.0, not 5.0.**
>
> Every maker figure below is therefore wrong by a factor of 2.5, and every "tier that would allow
> it" column is too pessimistic. The taker figures stand. The filing was read correctly; it simply
> describes a different schedule than the one this account trades on, and the account API should
> have been asked first.

# Fee ladder and repricing

Schedule verified against: CFTC rule filing rules0624267243.pdf, 24 June 2026, effective no earlier than 8 July 2026

Trailing volume counts both maker and taker volume; tiers recompute daily.

## The ladder

| tier | 30d volume | taker bps | maker bps | maker/maker round trip | maker/taker |
|---:|---:|---:|---:|---:|---:|
| 0 | $0 | 12.0 | 5.0 | 10.0 | 17.0 |
| 1 | $100,000 | 10.0 | 4.0 | 8.0 | 14.0 |
| 2 | $300,000 | 8.0 | 3.2 | 6.4 | 11.2 |
| 3 | $1,000,000 | 6.0 | 2.4 | 4.8 | 8.4 |
| 4 | $3,000,000 | 5.0 | 2.0 | 4.0 | 7.0 |
| 5 | $10,000,000 | 4.0 | 1.6 | 3.2 | 5.6 |
| 6 | $30,000,000 | 3.5 | 1.4 | 2.8 | 4.9 |
| 7 | $100,000,000 | 3.2 | 1.2 | 2.4 | 4.4 |
| 8 | $300,000,000 | 3.0 | 1.0 | 2.0 | 4.0 |
| 9 | $1,000,000,000 | 2.8 | 0.8 | 1.6 | 3.6 |
| 10 | $3,000,000,000 | 2.6 | 0.6 | 1.2 | 3.2 |

## This account

- **tier not resolved.** not requested; pass --account to try to resolve it
- every figure below is therefore labelled by tier rather than reported as "the" number, and tier 0 is used wherever one number is needed

## What each measured strategy keeps before fees

Gross is the half-spread plus the 60-second markout: the spread revenue that survives adverse selection. It is what any fee has to come out of.

| market | mode | fills | gross bps | break-even maker fee | tier that would allow it | 30d volume needed | fees to trade there |
|---|---|---:|---:|---:|---:|---:|---:|
| KXLINKPERP | improve | 152 | +3.45 | 3.45 | 2 | $300,000 | $130 |
| KXKSHIBPERP | improve | 174 | +3.15 | 3.15 | 3 | $1,000,000 | $354 |
| KXSUIPERP | improve | 191 | +2.59 | 2.59 | 3 | $1,000,000 | $354 |
| KXLINKPERP | join | 138 | +2.45 | 2.45 | 3 | $1,000,000 | $354 |
| KXSUIPERP | join | 184 | +2.14 | 2.14 | 4 | $3,000,000 | $834 |
| KXBCHPERP | improve | 178 | +1.97 | 1.97 | 5 | $10,000,000 | $2,234 |
| KXKSHIBPERP | join | 153 | +1.81 | 1.81 | 5 | $10,000,000 | $2,234 |
| KXZECPERP | improve | 376 | +1.26 | 1.26 | 7 | $100,000,000 | $15,234 |
| KXADAPERP | join | 340 | +0.87 | 0.87 | 9 | $1,000,000,000 | $109,234 |
| KXNEARPERP | join | 424 | +0.74 | 0.74 | 10 | $3,000,000,000 | $269,234 |
| KXNEARPERP | improve | 439 | +0.73 | 0.73 | 10 | $3,000,000,000 | $269,234 |
| KXZECPERP | join | 365 | +0.66 | 0.66 | 10 | $3,000,000,000 | $269,234 |
| KXLTCPERP | improve | 332 | +0.53 | 0.53 | below the 0.6 bps floor -- no tier reaches it | n/a | n/a |
| KXLTCPERP | join | 322 | +0.50 | 0.50 | below the 0.6 bps floor -- no tier reaches it | n/a | n/a |
| KXHYPEPERP | improve | 411 | +0.44 | 0.44 | below the 0.6 bps floor -- no tier reaches it | n/a | n/a |
| KXHYPEPERP | join | 403 | +0.33 | 0.33 | below the 0.6 bps floor -- no tier reaches it | n/a | n/a |
| KXWLDPERP | improve | 249 | +0.20 | 0.20 | below the 0.6 bps floor -- no tier reaches it | n/a | n/a |
| KXBTCPERP | improve | 690 | -0.11 | none -- loses before fees | n/a | n/a | n/a |
| KXBNBPERP | improve | 236 | -0.12 | none -- loses before fees | n/a | n/a | n/a |
| KXETHPERP | improve | 491 | -0.17 | none -- loses before fees | n/a | n/a | n/a |
| KXDOGEPERP | improve | 282 | -0.22 | none -- loses before fees | n/a | n/a | n/a |
| KXSOLPERP | improve | 485 | -0.25 | none -- loses before fees | n/a | n/a | n/a |
| KXSOLPERP | join | 473 | -0.25 | none -- loses before fees | n/a | n/a | n/a |
| KXBTCPERP | join | 702 | -0.29 | none -- loses before fees | n/a | n/a | n/a |
| KXXRPPERP | join | 503 | -0.41 | none -- loses before fees | n/a | n/a | n/a |
| KXWLDPERP | join | 208 | -0.47 | none -- loses before fees | n/a | n/a | n/a |
| KXETHPERP | join | 705 | -0.58 | none -- loses before fees | n/a | n/a | n/a |
| KXXRPPERP | improve | 547 | -0.59 | none -- loses before fees | n/a | n/a | n/a |
| KXDOGEPERP | join | 274 | -0.62 | none -- loses before fees | n/a | n/a | n/a |
| KXAAVEPERP | improve | 208 | -0.69 | none -- loses before fees | n/a | n/a | n/a |
| KXBCHPERP | join | 136 | -0.81 | none -- loses before fees | n/a | n/a | n/a |
| KXADAPERP | improve | 459 | -0.82 | none -- loses before fees | n/a | n/a | n/a |
| KXVVVPERP | improve | 488 | -0.93 | none -- loses before fees | n/a | n/a | n/a |
| KXBNBPERP | join | 167 | -1.26 | none -- loses before fees | n/a | n/a | n/a |
| KXVVVPERP | join | 474 | -1.27 | none -- loses before fees | n/a | n/a | n/a |
| KXAAVEPERP | join | 188 | -1.88 | none -- loses before fees | n/a | n/a | n/a |

## Net at every tier, entry leg only

Entry as maker, no exit cost. This is the most generous framing there is, and it is shown so that the tier at which each line crosses zero is visible.

| market | mode | t0 | t1 | t2 | t3 | t4 | t5 | t6 | t7 | t8 | t9 | t10 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| KXLINKPERP | improve | -1.5 | -0.5 | +0.3 | +1.1 | +1.5 | +1.9 | +2.1 | +2.3 | +2.5 | +2.7 | +2.9 |
| KXKSHIBPERP | improve | -1.9 | -0.9 | -0.1 | +0.7 | +1.1 | +1.5 | +1.7 | +1.9 | +2.1 | +2.3 | +2.5 |
| KXSUIPERP | improve | -2.4 | -1.4 | -0.6 | +0.2 | +0.6 | +1.0 | +1.2 | +1.4 | +1.6 | +1.8 | +2.0 |
| KXLINKPERP | join | -2.6 | -1.6 | -0.8 | +0.0 | +0.4 | +0.8 | +1.0 | +1.2 | +1.4 | +1.6 | +1.8 |
| KXSUIPERP | join | -2.9 | -1.9 | -1.1 | -0.3 | +0.1 | +0.5 | +0.7 | +0.9 | +1.1 | +1.3 | +1.5 |
| KXBCHPERP | improve | -3.0 | -2.0 | -1.2 | -0.4 | -0.0 | +0.4 | +0.6 | +0.8 | +1.0 | +1.2 | +1.4 |
| KXKSHIBPERP | join | -3.2 | -2.2 | -1.4 | -0.6 | -0.2 | +0.2 | +0.4 | +0.6 | +0.8 | +1.0 | +1.2 |
| KXZECPERP | improve | -3.7 | -2.7 | -1.9 | -1.1 | -0.7 | -0.3 | -0.1 | +0.1 | +0.3 | +0.5 | +0.7 |
| KXADAPERP | join | -4.1 | -3.1 | -2.3 | -1.5 | -1.1 | -0.7 | -0.5 | -0.3 | -0.1 | +0.1 | +0.3 |
| KXNEARPERP | join | -4.3 | -3.3 | -2.5 | -1.7 | -1.3 | -0.9 | -0.7 | -0.5 | -0.3 | -0.1 | +0.1 |
| KXNEARPERP | improve | -4.3 | -3.3 | -2.5 | -1.7 | -1.3 | -0.9 | -0.7 | -0.5 | -0.3 | -0.1 | +0.1 |
| KXZECPERP | join | -4.3 | -3.3 | -2.5 | -1.7 | -1.3 | -0.9 | -0.7 | -0.5 | -0.3 | -0.1 | +0.1 |

The first strategy to cross zero does so at **tier 2** ($300,000 of 30-day volume, maker 3.2 bps).

Trading up to that tier from zero costs about **$130** in maker fees along the way, paid at the higher rates held while generating the volume. That is the number that decides whether chasing a tier is a strategy or a way to lose the account.

## The trap

| target tier | 30d volume | maker fee there | fees to trade up to it |
|---:|---:|---:|---:|
| 1 | $100,000 | 4.0 | $50 |
| 2 | $300,000 | 3.2 | $130 |
| 3 | $1,000,000 | 2.4 | $354 |
| 4 | $3,000,000 | 2.0 | $834 |
| 5 | $10,000,000 | 1.6 | $2,234 |

Volume is charged at the rate held while it is generated, so the ladder cannot be climbed cheaply. Reaching tier 3 costs roughly the cost of a small car in fees. No account should chase a tier before it has a strategy that is profitable at the tier it already holds.

