# Kalshi incentive audit

Fetched 12,250 programs over 13 pages (status `active`).  
Account equity assumed: **$50**

## The headline

**No incentive program covers any perpetual future.** Not one of the 12,250 programs fetched has a PERP ticker or a `margin_*_volume` type.

So the effective maker fee on perps is the exchange fee, with no reward term:

```
F_effective = F_exchange - 0 - 0 = F_exchange
```

This is a finding, not an absence of one. It means the repricing in `reports/fee_ladder.md` is the whole economics, and there is no incentive path that closes the gap without a tier change.

## Why the server filter was not trusted

The documented `type` query parameter is ignored by the API. Asking for `type=margin_taker_volume` returned 1,000 records whose own `incentive_type` field reads `liquidity` or `volume`. Every classification here is done client-side on the record. Had the server filter been believed, this audit would have reported a thousand perpetual-futures volume programs that do not exist.

## What is running

| incentive_type | programs | total pool | median pool |
|---|---:|---:|---:|
| `liquidity` | 12,245 | $867,678 | $45.00 |
| `volume` | 5 | $2,500 | $500.00 |

## Liquidity programs and the Target Size floor

A snapshot scores only when BOTH sides carry at least the Target Size, so qualifying costs twice the target in notional. That floor, not competition for the pool, is what decides whether a small account can participate.

| target size | programs | median pool | collateral to qualify (two-sided) | reachable |
|---:|---:|---:|---:|---|
| 300 | 74 | $90.91 | $294 | 0 of 74 |
| 1,000 | 12,169 | $45.00 | $980 | 0 of 12169 |
| 2,500 | 2 | $100.00 | $2,450 | 0 of 2 |

**0 of 12,245 liquidity programs are reachable at $50.**

## What this does to the effective fee

```
F_effective = F_exchange - E[volume reward] - E[liquidity reward]

perps:  E[volume reward]    = 0   (no margin_*_volume program exists)
        E[liquidity reward] = 0   (no perp appears in any liquidity program)
        F_effective         = F_exchange
```

## Caveats

- This is one snapshot. Programs start and end continuously, and the reward page updates daily; an audit from last week is not evidence about this week.
- `status=active` shows what is running now. A program that starts tomorrow is invisible to it, so this should be re-run before any decision that depends on it.
- Reward amounts are in centi-cents in the API. A factor of 10,000 error here would make a $20 pool look like a $200,000 one.
- Eligibility beyond size is not checked. Employees, affiliates, introducing brokers and international users are excluded by the program terms, and payouts above the IRS reporting threshold require a verified SSN.

