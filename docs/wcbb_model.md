# The basketball models: women's college and NBA (plan section 5(f))

The model's fair for a game market (KXNCAAWBGAME, KXNBAGAME): the probability that the market's
YES team wins, from the game state and the pregame Kalshi price. `tools/train_bball.py --league
wcbb|nba` fits it and writes `models/wcbb_logit_vN.json` or `models/nba_logit_vN.json`
(`tools/train_wcbb.py` is `--league wcbb` under its old name); deskd (`strat/wcbb_model.h`, and
`strat/bball_model.h` for both leagues) loads that file, refuses it unless its golden rows
reproduce to 1e-6, and shows the fair on the Manual ladders (book.ref.fair) from the first listed
game. Systematic use waits for the plan's gate: 150+ settled games, a held-out Brier score at
least the Kalshi mid's, and positive markouts on overlay clicks.

## Leagues

| league | model | regulation | in overtime |
|---|---|---|---|
| `wcbb` | `wcbb_logit` | 4 x 600 s = 2400 s | `t_left_s` 0 (the overtime clock is not used) |
| `nba` | `nba_logit` | 4 x 720 s = 2880 s | `t_left_s` is the overtime clock, 0 to 300 s |

From ESPN's period and the period's clock: periods 1 to 4 with a clock of at most one period give
(4 - period) x period length + clock, `ot` false; a later period is overtime, `ot` true, with
`t_left_s` the clock (NBA, clock at most 300 s) or 0 (wcbb, clock at most 600 s). Period 0 or a
clock past those limits is no state, and no fair.

The NBA's model (KXNBAGAME, the N1 lane) is the same regression in the same file format, with
model `nba_logit`. deskd reads both leagues through one loader, `strat/bball_model.h`
(`strat/wcbb_model.h` is it with the league fixed to wcbb), and `tools/train_bball.py --league
nba` fits it.

## Leagues

| league | model | series | regulation (R) | in overtime |
|---|---|---|---|---|
| `wcbb` | `wcbb_logit` | KXNCAAWBGAME | 4 x 600 s = 2400 s | `t_left_s` 0, `ot` 1 |
| `nba` | `nba_logit` | KXNBAGAME | 4 x 720 s = 2880 s | `t_left_s` the overtime clock (at most 300 s), `ot` 1 |

From ESPN's period and the period's clock: periods 1 to 4 give (4 - period) x period length +
clock; a later period is overtime. A clock longer than a regulation period, or in NBA overtime
longer than 300 s, is no state, and so is period 0 or an unread clock.

## Inputs (one row per game state)

| name | meaning |
|---|---|
| `t_left_s` | seconds left in regulation (R); in overtime 0 (wcbb) or the overtime clock (nba) |
| `margin` | the YES team's score minus the opponent's |
| `possession` | +1 the YES team has the ball, -1 the opponent, 0 unknown |
| `p0` | the pregame YES probability: the Kalshi mid at the scheduled tip (spread 15c or less), clamped to [0.01, 0.99] |
| `ot` | true in overtime, else false |

## Features (the only names a model file may use)

With `tau = max(t_left_s, 0) / R` (R the league's regulation: 2400 wcbb, 2880 nba) and
`logit(x) = ln(x / (1 - x))`:

| name | value |
|---|---|
| `logit_p0` | logit(p0) |
| `margin` | margin |
| `margin_over_sd` | margin / sqrt(tau + 0.01) |
| `logit_p0_x_tau` | logit(p0) * tau |
| `logit_p0_x_sqrt_tau` | logit(p0) * sqrt(tau) |
| `possession` | possession |
| `possession_over_sd` | possession / sqrt(tau + 0.01) |
| `tau` | tau |
| `ot` | 1 in overtime, else 0 |

In NBA overtime tau is the overtime clock over 2880 (at most 300/2880); in wcbb overtime it is 0.

## The file

```json
{
  "model": "wcbb_logit",
  "league": "wcbb",
  "regulation_s": 2400,
  "version": 1,
  "trained": "2026-11-01",
  "seasons": ["2024-25", "2025-26"],
  "n_games": 1234,
  "brier": {"model": 0.123, "kalshi_mid": 0.125, "espn": 0.127, "stern": 0.130, "stern_ot": 0.129, "held_out": "2025-26"},
  "features": ["logit_p0", "margin_over_sd", "logit_p0_x_tau", "ot"],
  "mean": [0.0, 0.0, 0.0, 0.0],
  "scale": [1.0, 1.0, 1.0, 1.0],
  "coef": [1.0, 0.5, -0.3, 0.1],
  "intercept": 0.0,
  "golden": [{"in": {"t_left_s": 1200, "margin": 5, "possession": 1, "p0": 0.6, "ot": false}, "p": 0.7}]
}
```

p = 1 / (1 + exp(-(intercept + sum_i coef[i] * (f_i - mean[i]) / scale[i]))). `golden` holds 10
rows spread over the inputs, with p as the trainer computed it; a loader whose own p differs from
any by more than 1e-6 refuses the file and says which row. `model` is `wcbb_logit` or `nba_logit`;
`league` and `regulation_s`, when present, must be that model's league's (a file without them,
written before the NBA's, takes them from `model`). A golden row's `ot` is a boolean, false when
absent. deskd keeps the sha256 of the file's bytes, which is how a gate file
(`gates_dir/<lane>.json`) names the model it measured. An NBA file's golden rows include an
overtime row (3:00 left in overtime); a wcbb file's include one (t_left_s 0) when it uses `ot`.

`model` names the league; `league` must agree with it and `regulation_s` must be that league's (a
loader refuses `nba_logit` with `regulation_s` 2400, naming both). A file written before these
keys existed (wcbb, no `league`, no `regulation_s`, golden rows without `ot`) reads as
`wcbb`, 2400, `ot` false. In `brier`, `kalshi_mid` and `espn` are null when the panel had none;
`stern_ot` is Stern's model with a tie at the horn going to overtime (train_bball.py's docstring).
A gate names a model by the file's sha256 (`sha256sum`, the trainer prints it).
