# The women's college basketball model (plan section 5(f))

The model's fair for a KXNCAAWBGAME market: the probability that the market's YES team wins, from
the game state and the pregame Kalshi price. `tools/train_wcbb.py` fits it and writes
`models/wcbb_logit_vN.json`; deskd (`strat/wcbb_model.h`) loads that file, refuses it unless its
golden rows reproduce to 1e-6, and shows the fair on the Manual ladders (book.ref.fair) from the
first listed game. Systematic use waits for the plan's gate: 150+ settled games, a held-out Brier
score at least the Kalshi mid's, and positive markouts on overlay clicks.

## Inputs (one row per game state)

| name | meaning |
|---|---|
| `t_left_s` | seconds left in regulation (40 minutes = 2400 s); 0 in overtime |
| `margin` | the YES team's score minus the opponent's |
| `possession` | +1 the YES team has the ball, -1 the opponent, 0 unknown |
| `p0` | the pregame YES probability: the Kalshi mid at the scheduled tip (spread 15c or less), clamped to [0.01, 0.99] |

## Features (the only names a model file may use)

With `tau = max(t_left_s, 0) / 2400` and `logit(x) = ln(x / (1 - x))`:

| name | value |
|---|---|
| `logit_p0` | logit(p0) |
| `margin` | margin |
| `margin_over_sd` | margin / sqrt(tau + 0.01) |
| `logit_p0_x_tau` | logit(p0) * tau |
| `possession` | possession |
| `possession_over_sd` | possession / sqrt(tau + 0.01) |
| `tau` | tau |

## The file

```json
{
  "model": "wcbb_logit",
  "version": 1,
  "trained": "2026-11-01",
  "seasons": ["2024-25", "2025-26"],
  "n_games": 1234,
  "brier": {"model": 0.123, "kalshi_mid": 0.125, "stern": 0.130, "held_out": "2025-26"},
  "features": ["logit_p0", "margin_over_sd", "logit_p0_x_tau", "possession"],
  "mean": [0.0, 0.0, 0.0, 0.0],
  "scale": [1.0, 1.0, 1.0, 1.0],
  "coef": [1.0, 0.5, -0.3, 0.1],
  "intercept": 0.0,
  "golden": [{"in": {"t_left_s": 1200, "margin": 5, "possession": 1, "p0": 0.6}, "p": 0.7}]
}
```

p = 1 / (1 + exp(-(intercept + sum_i coef[i] * (f_i - mean[i]) / scale[i]))). `golden` holds 10
rows spread over the inputs, with p as the trainer computed it; a loader whose own p differs from
any by more than 1e-6 refuses the file and says which row.
