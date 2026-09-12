# Sports totals and the overtime tail

Model: `include/desk/sports_models.h`, `src/strategies/sports_totals.cpp`.
Priors: `config/desk.json` under `params.sports_totals.leagues`.
Tests: `tests/test_pair_models.cpp` (`./build/pair-model-tests`).

## What it prices

P(final total over the line), conditional on what has been scored and how much regulation is
left. Soccer uses a Poisson or negative-binomial count of the goals still to come; basketball and
football use a normal for the points still to come, blended between the league prior and the pace
actually being played, plus an overtime mixture.

Every contract must be listed by ticker in `params.sports_totals.markets` with its rules text,
and the stored rules must still match at pricing time. Nothing is mapped yet, so the strategy
currently prices nothing. That is deliberate: a totals contract that silently picked up the wrong
league's overtime rule would be worse than no model. The league block carries the priors and the
market entry names its league, so adding a contract means a line, a rules string and a league.

The mode is `research`, and every estimate is returned `research_only`. It is scored against the
market on settled contracts; it does not propose.

## The overtime hypothesis, measured

Fitted 2026-09-11 from ESPN final scores and per-period linescores, August 2023 to September 2026:
NFL 858 games, NCAAF 2,988, NBA 4,187, NCAAMB 18,835.

| league | regulation total | sd | P(OT) season | points OT adds | sd |
|---|---:|---:|---:|---:|---:|
| NBA | 226.83 | 19.71 | 4.81% | 24.07 | 8.59 |
| NCAAMB | 146.17 | 18.36 | 5.30% | 24.23 | 10.80 |
| NFL | 45.06 | 13.66 | 5.37% | 5.22 | 2.90 |
| NCAAF | 53.16 | 16.34 | 4.18% | 12.27 | 7.51 |
| NHL | 5.95 | 2.34 | 22.04% | 1.00 | 0.03 |
| MLB | 8.91 | 4.65 | 13.41% | 2.79 | 2.10 |

Two things stand out. Overtime is rare but large: 24 points is 1.2 regulation standard deviations
in the NBA, so it moves the upper tail far more than its 4.8% frequency suggests. And it is not
evenly distributed across games. P(overtime) by margin entering the last regulation period:

| margin after Q3 | NBA | NFL | NCAAF |
|---|---:|---:|---:|
| level | **12.6%** | 10.0% | **17.5%** |
| 1-3 | 8.1% | 9.7% | 10.3% |
| 4-6 | 8.8% | 8.1% | 6.8% |
| 7-10 | 5.8% | 6.3% | 5.9% |
| 11-15 | 3.2% | 2.9% | 2.7% |
| 16+ | 0.7% | 0.0% | 0.4% |
| *season rate* | *4.8%* | *5.4%* | *4.2%* |

A level game late reaches overtime 2.6 times the base rate in the NBA and 4.2 times in college
football. A model carrying a constant P(overtime) is therefore wrong by a factor of three to four
in exactly the situation where overtime is worth the most, and wrong in the direction of
underpricing the over.

## How the conditional is computed

Overtime happens when and only when the regulation margin lands exactly on zero. If the margin
still to come is roughly normal with standard deviation `margin_sd * sqrt(fraction_left)`, then
P(overtime) is that normal's density at the current margin times the width of one tie on the
sport's scoring lattice:

    P(OT) = tie_width * phi(margin / sd) / sd,   sd = margin_sd * sqrt(fraction_left)

`tie_width` is fitted per league by maximum likelihood over every period boundary of all 26,868
games at once, so one number carries the sport's scoring granularity (NBA 1.64, NCAAF 1.91,
NCAAMB 1.99, NFL 1.60). Before tip-off there is no margin to condition on and the season base
rate is used instead.

Calibration at the last regulation break, observed against predicted:

| league | level | 1-3 | 4-6 | 7-10 | 11+ |
|---|---|---|---|---|---|
| NCAAMB | .090 / .085 | .084 / .083 | .075 / .074 | .057 / .057 | .018 / .020 |
| NBA | .126 / .130 | .081 / .118 | .088 / .079 | .058 / .033 | .017 / .002 |
| NCAAF | .175 / .101 | .103 / .095 | .068 / .083 | .059 / .055 | .010 / .008 |
| NFL | .100 / .136 | .097 / .118 | .081 / .078 | .063 / .032 | .012 / .001 |

College basketball is close to exact everywhere. The others are good where most overtime comes
from and understate it from large margins, because a trailing team deliberately plays higher
variance (fouling, onside kicks) in a way a random walk does not capture. Treat the large-margin
end as a floor, not an estimate.

## What is not done

- Soccer priors are not fitted. The EPL and MLS backfill has not finished, so no soccer league
  block exists and no soccer contract can be mapped yet.
- Nothing has been validated out of sample against settled Kalshi contracts. These are priors
  fitted to outcomes, not a demonstration of edge against a price. The Brier comparison on the
  Strategies tab is what would establish that, and it needs settled totals markets first.
- The pace blend weight (`w = elapsed / (elapsed + 0.5)`) is a guess, not a fit.
- NHL and MLB priors are measured above but have no code path: `kind` accepts only `soccer`,
  `basketball` and `football`. Hockey in particular would need a different treatment, since its
  overtime adds exactly one goal and ends on it.
