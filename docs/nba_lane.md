# The NBA lanes: X1, N1 and N2 (plan 5(e), decisions 2)

Three IOC lanes on KXNBAGAME, the NBA's game markets, each off by default and gated by evidence.
The operator asked for NBA; the evidence says its books are efficient, so the expected outcome is
that X1 and N1 stay in shadow and spend nothing. The code makes that a measurement.

| lane | name | what it takes | budget | per market | retired after |
|---|---|---|---|---|---|
| X1 | ESPN EXP | ESPN's win probability against Kalshi's price, followed or faded as its backtest chose | $5 | 4 | 100 trades, or $20 lost |
| N1 | NBA MODEL | the NBA model's fair against a stale price, in play | $20 | 10 | $20 lost (and its markout hold) |
| N2 | NBA FAV 90-95 | a 90-95c favourite before the tip, 5 contracts held to settlement | $15 | 5 | 60 trades, or $15 lost |

All three share the NBA's $10 game cap in gw (X1's and N1's event cap; N2's 5 contracts a market
bound its game), the $15 correlated group per game, the $5 daily IOC fee budget with C1 and S1,
and the 70% ceiling: a lane is refused on when the budgets of the lanes on would pass it
(decisions 1).

## The evidence (why the expected verdict is shadow)

- docs/edge_results.json hunt:sports: NBA takers lost 1.78c a contract; no momentum; no
  logistic recalibration beat the Kalshi mid out of sample. The one NBA cell above the desk's
  bar is pregame favourites with the ask in [0.90, 0.95): +5.43 +- 1.78c, 76 of 78 won, with
  the advice "forward test at 5-10 contracts for >= 60 trades with a -$15 stop". N2 is that
  forward test, exactly.
- reports/espn_leadlag.md: following ESPN on the plan's trigger lost $1.23 a trade (n 106, t
  -1.83), and much of Kalshi's move came 30 s or more before ESPN published. X1's backtest picks
  the better direction (follow or fade); by plan 5(e)'s "a reading that is not negative" it may
  well pick neither.
- Two losses in 78 make N2's cell: five losers would erase it. Hence 5 contracts and the stops.

## Turning them on

deskd's config (`config/desk.json`'s `deskd` section, or the PAPER/LIVE configs tools/desk.py
writes):

| key | default | meaning |
|---|---|---|
| `nba_lanes` | false | builds X1's, N1's and N2's engines, each once its gate loads (with `systematic_orders`) |
| `nba_series` | ["KXNBAGAME"] | the series all three trade |
| `nba_model` | "" = models/nba_logit_v1.json | the NBA model: N1's fair, and the NBA ladders' whenever the file exists |
| `lane_shadow` | true | X1, N1 and N2 record what they would take while they may not post |
| `gates_dir` | "" = <data_dir>/gates | where each lane's gate file is |

PAPER's config is `~/.kalshi/deskd-demo.json` and LIVE's `~/.kalshi/deskd-prod.json`; tools/desk.py
gives each desk its data dir, so `gates_dir` is `<data>/demo/gates` on PAPER and
`<data>/prod/gates` on LIVE (`<data>`: `~/Library/Application Support/kalshi-desk` on a Mac,
`${XDG_DATA_HOME:-~/.local/share}/kalshi-desk` elsewhere).

A lane trades only when all of these hold: `nba_lanes` and `systematic_orders` on; its gate file
`<gates_dir>/<lane>.json` loads and is live (read once at start: a new gate needs a restart); the
operator turns the lane on (`POST /api/lane/<id>`, the SYSTEMATIC tab) and presses Start. A gated
lane's LaneSet is refused BAD_REQUEST "lane X is gated: <why>", its orders LANE_OFF; a retired
lane likewise, until `tools/desk.py lane-reset LANE [live]` with that desk stopped.

`/ops/status` `nba` shows the switches, the model (path, sha256), and for each lane its gate
(path, stage, verdict, tool), the rule it opens (X1 and N1: mode and edge; N2: count and band),
why it is off, and its engine (per game, the last decision and its pull; takes, shadows, pulls,
refusals; N1's hold).

## The gate files (strat/lane_gate.h)

One file per lane, written by its tool, never by hand on a real desk:

| lane | tool | params deskd reads | model |
|---|---|---|---|
| X1 | `tools/x1_backtest.py --write-gate` | `direction` follow or fade, `thr` (dollars) | none |
| N1 | `tools/nba_gate.py n1`, then `shadow` | `thr` (dollars) | `model_sha256`: the model file deskd loads |
| N2 | `tools/nba_gate.py n2` | `count` 5, `fav_lo` 0.90, `fav_hi` 0.95 | none |

Only verdict PASS at stage live opens a lane. Stage shadow builds the engine but keeps the lane
gated: it records SHADOW rows only. A FAIL, a missing file, another model's sha (a retrained N1
model closes its gate), a direction neither follow nor fade, a thr outside (0, 1), or N2 params
other than what N2 trades each keep the lane off and say why. N2 trades take_params' FAV_PREGAME
values, so its gate must have measured exactly them.

## The rules (strat/take.h, take_params)

One take engine (TakeEngine) for every IOC lane, the same rule the gate tools replay in Python
(tools/take_rule.py; tests/deskd/fixtures/x1_golden.json and n1_golden.json pin both):

- **X1** (ESPN_FOLLOW or ESPN_FADE): $2 clips of at most 4 contracts, 4 a market, 4 takes a game
  10 s apart, ESPN's number first seen at most 30 s before, both books two-sided within 3c, the
  team's price 5-95c, no overtime nor the last 2:00 of regulation; the gate's thr as its edge.
- **N1** (MODEL): the same checks against the NBA model's fair (GameFair: the pregame price is
  the two markets' effective mid before the tip, 15c wide at most; then the model on the score),
  10 contracts a market, the gate's thr (10c until measured). It holds new takes while its
  fills' net 60 s markout averages at or under 0 after 100.
- **N2** (FAV_PREGAME): from 60 to 2 minutes before ESPN's scheduled start, while the game is
  still "pre" and its score at most 60 s old: the team whose price (its ask, or 1 - the other
  market's bid) is in [0.90, 0.95) is bought up to 5 contracts held, both books within 10c. No
  edge: the band is the rule. It holds to settlement.

A take is one IOC at the touch through gw (cid S<lane>-..., the lane's order group on send-k2),
only while the lane is on, Systematic runs, the private feed is up and neither market is
Manual's. Each new score state is a signal run, in which each team is taken (or recorded) once.

**N2 and shadow.** A pregame state lasts until the tip, so N2's signal run is the whole pregame.
A favourite N2 sees while it may not post (off, Systematic stopped) is a SHADOW row that uses
that run: it will not buy that game later in the pregame unless the price leaves its band for
10 s and comes back. Turn N2 on and press Start before the first window opens (60 minutes
before the day's first tip).

**Before the tip.** With `lane_shadow` on, each built engine watches its games' books all day,
on or off, so N1 has each game's pregame price. With it off, a lane watches its games only while
it is on: turn N1 on before the tip (a game it did not watch pregame has no p0 and pulls NO_P0),
and know that N1 turned on mid-game takes the first state its feeds report as new (SIGNAL_OLD
dates it by its first sighting alone).

## What they record

Every signal goes to ops.sqlite's `signals` table (store/store.h): lane, mode, TAKE (sent; again
as REFUSED with gw's code if refused), SHADOW (what it would take while it may not post) or PULL,
the order's market, side, price and contracts, the fair (none for N2), the edge, the score, and
10, 60 and 300 s markouts at the mid. N1's last gate (`nba_gate.py shadow`) reads its SHADOW rows;
the nightly `ops-research.sqlite` copy is the one to read.

ESPN: with X1's or N1's engine built, deskd reads the NBA scoreboard every 3 s around and during
a game (fast_series), summaries every 10 s; N2 alone needs no more than the 20 s scoreboard.

## The operator's order (the Mac and the VM; agents never run these on data/)

1. The recorder (VM) records KXNBAGAME's socket books and ESPN's NBA scoreboard and plays
   (REC_GAME_SERIES, REC_SUMMARY_LEAGUES). Let it run a week.
2. `python3 tools/nba_latency.py --data <recorder dir>`: the NBA's ESPN lag; its p90 is the
   `--lag-s` the gates use (41.5 s until measured).
3. X1: `python3 tools/x1_backtest.py --db <the old desk's extract> --data <recorder dir>
   --write-gate <gates_dir>/X1.json`. The direction and thr are chosen on the extract's train
   dates; live only if the NBA replication is not negative on 30+ recorded games.
4. N1: `tools/nba_panel.py ... --lag-s <lag> --out panel.csv`, `tools/train_bball.py --league
   nba --panel panel.csv --out models/nba_logit_v1.json`, then `tools/nba_gate.py n1 --panel
   panel.csv --model models/nba_logit_v1.json --lag-s <lag> --write <gates_dir>/N1.json` (G1 the
   held-out Brier, G2 the replay). When X1 is live too, add `--with-x1 <gates_dir>/X1.json`: X1's
   rule is replayed beside N1 and its takes count in N1's inventory room, as gw counts them. A
   pass is stage shadow. After 14+ days of shadow: `nba_gate.py shadow --ops <data_dir>/
   ops-research.sqlite --data <recorder dir> --gate <gates_dir>/N1.json --model <same> --write
   <gates_dir>/N1.json --force` (G3; a pass is stage live). Expect a FAIL at G1.
5. N2: `python3 tools/nba_gate.py n2 --data <recorder dir> --write <gates_dir>/N2.json`: this
   season's books show the favourites can be bought (10+ games filled to 5 contracts). A pass
   is stage live.
6. On PAPER: add `"nba_lanes": true` under `"deskd"` in `~/.kalshi/deskd-demo.json`, with the
   gates in `<data>/demo/gates`, and restart the desk (`python3 tools/desk.py`). Read what loaded:

   ```sh
   curl -s http://127.0.0.1:8788/ops/status | python3 -c \
       'import json, sys; print(json.dumps(json.load(sys.stdin)["nba"], indent=1))'
   ```

   Each lane's `why` is "" when its gate is live, else the gate's reason (or the stage-shadow
   text); `rule` is what it trades. Turn the lane on in the SYSTEMATIC tab and press Start.
   LIVE is the operator's alone (`deskd-prod.json`, `<data>/prod/gates`, port 8789).
7. `python3 tools/scorecard.py [paper|live]`: X1 RETIRE after 100 trades unless its net 60 s
   markout is positive (gw retires it at 100 trades whatever they made, its cap: the verdict says
   whether another 100 are worth a lane-reset); N1 RETIRE after 100 trades unless its net 60 s
   markout is positive, or once down $20; N2 WAIT under 60 trades or while a position is still
   to settle, then HOLD on a positive settlement P&L, else RETIRE (or once down $15). N1's and
   N2's dollars come from the desk's systematic.json (gw's count of each lane's life). gw
   retires each lane by itself; the scorecard is advice.

## The sandbox

`python3 tools/desk.py sim --nba` (tools/deskd_sim.py): the fake exchange lists today's
KXNBAGAME Lakers at Boston and deskd reads a loopback stand-in for ESPN (`deskd --sim ...
--espn-url http://127.0.0.1:PORT`; refused without --sim or off loopback, so the sandbox never
reaches ESPN). deskd runs the synthetic model and sandbox gates (X1 shadow, N1 and N2 live,
marked SANDBOX) in its temporary data dir. Boston becomes the 0.92 favourite 3 minutes in (N2
buys 5 if it is on and Systematic runs by then), the tip comes at 15 minutes, and at 16 the
Lakers go 5 up with 5:00 left against Boston's stale book (N1 takes the Lakers; X1's fade is a
SHADOW row). `deskd_sim.py --check` runs it in a minute: ctest `deskd-sim-nba`.

## Tests

deskd-take (the rule, each pull, the engine; N2's engine_fav), deskd-lane_gate (each gate file
the tools write loads as deskd reads it), deskd-runtime_nba (X1 and N1 end to end: gates, one N1
take in its group, X1 in shadow, Stop, KILL, ESPN silent, the game cap; N2: its gates, a SHADOW
row at stage shadow, one take of the favourite's 5 when live, then HELD), deskd-sim-nba, and the
tools' self-tests (x1-backtest, nba-panel, nba-latency, nba-gate, scorecard, take-rule).

## Not confirmed here (no network in the build container)

ESPN's NBA scoreboard and summary shapes (the fixtures are hand-built), KXNBAGAME's fee
multiplier (1.0 assumed), whether Kalshi's demo lists NBA at all (if not, PAPER cannot trade
these lanes and only the sandbox shows them), the in-game depth at the touch, and the real ESPN
lag. No NBA model ships: N1 has no engine until one is trained and its gate written.
