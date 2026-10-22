# The college lane: C1 COLLEGE TAKE (plan 5(d) and 5(f))

One IOC lane on thin college games: women's basketball (KXNCAAWBGAME) and women's volleyball
(KXNCAAWVMATCH). It takes a quote that has stood since before the score changed and now sits 20c
or more, net of the taker fee, away from the model's fair. It is off by default and gated by this
season's own evidence: plan 5(d)'s order is "record now, overlay in November, an IOC lane only if
the test passes", and the code makes that a measurement.

| lane | series | fair | budget | a game | a market | retired after |
|---|---|---|---|---|---|---|
| C1 | KXNCAAWBGAME, KXNCAAWVMATCH | the WCBB model (5(f)); the volleyball Markov model | $40 | $10 | 20 contracts | 100 trades with the net 300 s markout not positive, or $20 lost |

C1 also shares the $15 correlated group per game, the $5 daily IOC fee budget (with S1, X1, N1
and N2: rebates never count), the $30 daily stop, the $150 floor and the 70% ceiling: a lane is
refused on when the budgets of the lanes on would pass it (decisions 1). PAPER's $500 holds every
lane; on a ~$140 LIVE account (a ~$99 ceiling) C1's $40 fits beside M1's $55 and little else.

## The evidence

- docs/edge_results.json, women's basketball in play (verified independently): in thin games a
  10c-net IOC rule held to settlement made +16.5c +- 2.7c a contract out of sample (141 of 234
  games), $2.57 a game at $5 clips and $1.59 at $2. The edge decays with the quote's staleness
  (+5.8c at 5 minutes, 0 at 10) and falls to +3.7c with a 120 s lag at the worst quote. Buying the
  team that just extended its lead made +20c, the trailer +1.9c. Resting quotes miss it.
- Volleyball (KXNCAAWVMATCH): the hunt's 275 matches could not be backtested at all: ESPN
  publishes no play-by-play, and the Kalshi-only rules were null. Its only in-set state is ESPN's
  line scores, if they carry the set's points live (TO CONFIRM). It is measured on this season's
  recordings alone, and stays off unless that passes.
- Plan section 1: "real but sparse", $3-6.5 a game at most. So the gate replays every case
  exactly as C1 trades it (an IOC at the shown price, filled 2 s later only if the touch is still
  there) on the season's own recorded books and scores, and C1 starts at $2 clips.

## Turning it on

deskd's config (`config/desk.json`'s `deskd` section, or the PAPER/LIVE configs tools/desk.py
writes, `~/.kalshi/deskd-demo.json` and `deskd-prod.json`):

| key | default | meaning |
|---|---|---|
| `college.enabled` | false | builds C1's engine and reads its gate (with `systematic_orders`) |
| `college.series` | ["KXNCAAWBGAME", "KXNCAAWVMATCH"] | the series C1 may trade, each only once its gate passes it |
| `college.volley_model` | "" = models/volley_markov_v1.json | the volleyball model: C1's fair, and the volleyball ladders' |
| `college.clip_dollars` | 2 | a take's cost, fee included: $1 to $5 |
| `college.game_dollars` | 10 | C1's worst case in one game (gw's event cap): the clip to $15 |
| `college.lane_dollars` | 40 | C1's budget: the game's to $40 |
| `college.max_score_age_s` | 5 | the score's last poll at most this old: 1 to 5 (plan 5(d)) |
| `wcbb_model` | "" = models/wcbb_logit_v1.json | the WCBB model: C1's basketball fair, and the ladders' whenever the file exists |
| `gates_dir` | "" = <data_dir>/gates | where each lane's gate file is |

An unknown key, a series that is not a ticker, or an amount outside its range refuses the start
and names the field. The model paths are in the source tree deskd was built from; tools/desk.py
gives each desk its data dir, so `gates_dir` is `<data>/demo/gates` on PAPER and
`<data>/prod/gates` on LIVE (`<data>`: `~/Library/Application Support/kalshi-desk` on a Mac,
`${XDG_DATA_HOME:-~/.local/share}/kalshi-desk` elsewhere).

C1 trades only when all of these hold: `college.enabled` and `systematic_orders` on;
`<gates_dir>/C1.json` loads, is live, and passed the series on the model deskd loaded for it (read
once at start: a new gate or a new model needs a restart); the operator turns C1 on
(`POST /api/lane/C1`, the SYSTEMATIC tab) and presses Start. With `college.enabled` off, LaneSet is
refused "lane C1 has no engine yet"; with no gate, BAD_REQUEST "lane C1 is gated: no gate file
<path>", and its orders LANE_OFF; a gate that opens no series names each one's reason.

## The gate file (gates_dir/C1.json, strat/lane_gate.h)

Written by `tools/college_gate.py --write-gate`, never by hand on a real desk:

```json
{"lane": "C1", "stage": "live", "verdict": "PASS", "why": null,
 "computed": "2026-11-20T18:40:00Z", "tool": "tools/college_gate.py",
 "params": {"edge": 0.2, "exec_lag_s": 2, "max_score_age_s": 5, "clip": 2.0, "game_cap": 10.0,
            "inventory": 20, "thin": 20000, "min_cases": 50, "min_t": 2.0, "horizon": "300s",
            "leader_only": false},
 "series": {"KXNCAAWBGAME": {"pass": true, "why": "", "level": "point", "leader_only": false,
                             "model": "wcbb_logit", "model_sha256": "0091408c..."},
            "KXNCAAWVMATCH": {"pass": true, "why": "", "level": "point", "leader_only": false,
                              "model": "volley_markov", "model_sha256": "20c2c8c6..."}},
 "evidence": {"data_from": "...", "data_to": "...", "fee_mult": 1.0, "series": {}, "wcbb_f": {},
              "control": null}}
```

(tests/deskd/fixtures/college_gate_ok.json is a whole one, from the tool's synthetic season.)
deskd reads `params.edge` (dollars a contract, never under 10c: C1's edge for every series) and,
per series, `pass`, `level` ("point", or "set": only within 120 s of a set's end; "" is point,
anything else closes the series), `leader_only`, and `model_sha256`, which must be the sha256 of
the model file deskd loaded for that series (a retrained model closes its series until the gate is
run on it). It has no top-level `model_sha256`. Only verdict PASS at stage live opens anything; a
FAIL, a missing file or another model's sha keeps the series (or the lane) off and says why. A
gate at stage shadow keeps C1 off too: C1 records no SHADOW rows (the gate measures the recorder's
data instead).

The verdict, per series (decisions 8): at least 50 taken cases, a positive contract-weighted mean
net 300 s markout, and a game-clustered t of at least 2.0. Volleyball is measured at level point
first, then set. KXNCAAWBGAME also needs plan 5(f)'s gate, folded into its `pass` and `why`: 150+
settled games recorded with the model's fair, the model's Brier on them at least as good as the
effective mid's, and 20+ Manual fills on the series (the overlay clicks) with a positive net 60 s
markout. `params.clip`, `game_cap` and `max_score_age_s` are what the gate measured; deskd trades
its config's, so keep them the same.

## The rule (strat/take.h, take_params' COLLEGE)

One take engine (TakeEngine) for every IOC lane, the same rule tools/college_gate.py replays in
Python (tools/take_rule.py; tests/deskd/fixtures/college_take_cases.json pins both):

- **A game** is its two markets, one for each team. C1 looks at its series' games dated today or
  yesterday (ET).
- **The fair** (GameFair, strat/game_fair.h): the pregame price p0 is the effective mid of the two
  books while the score says pre, before the scheduled start, at 15c wide or less (kept in
  `data_dir/game_p0.json` for 12 hours, so a restart mid-game keeps it). In play, the WCBB model
  prices the period, clock and margin; the volleyball model the sets and points.
- **A take** is one IOC at the touch through gw (cid SC1-..., C1's order group on send-k2): buy a
  team at its ask or sell the other market at its bid, whichever passes every check with the
  largest edge, the fair less the price less the fee a contract at least the gate's edge (20c);
  sized to the clip, the touch, the game's room ($10 less C1's worst case there) and the market's
  room (20 contracts of Systematic's position, every lane's).
- **Only** with the score at most 5 s old and in play; a thin game (both markets' in-game volume
  under 20,000 contracts; unknown counts as not thin); a touch first seen before the score state
  was (a stale quote), the state's change dated by a poll at most 10 s before it; only the
  leading team if the gate says leader_only; volleyball at level point only with the set's points
  read, at level set only within 120 s of a set's end; 5 s after any take in the game; each team
  once per score state (a signal run).
- **And gw's checks:** the lane on, Systematic running, no KILL or HALT, the private feed up, not a
  market Manual holds, the $10 event cap, the $15 group, the 70% ceiling, the daily stop, the
  floor, the $5 IOC fee budget, Systematic's 80% of the write budget.

**The score** (ref/scores.h, ScoreMerge; tools/college_gate.py's Score is the same rule). ESPN
(ref-espn) and NCAA.com (ref-ncaa, its own data-only connection to data.ncaa.com) each read the
watched games of C1's tradable series every 3 s from 15 minutes before the start and in play;
every other game every 20 s (ESPN) or 30 s (NCAA.com). A source is fresh when its last poll is at
most `max_score_age_s` old. A final from either source ends the
game, however old. Both fresh: they must agree on the state, the scores and the period (NCAA.com's
"HALF", "OT" and "2OT" read no period), else there is no score (NO_SCORE, naming both). One
fresh: that one. Neither: the fresher, which SCORE_OLD then pulls. A state is dated by the first
source to see it, and keeps that date while it lasts, whichever source is fresh, so a source
dropping out for a moment never starts a second run in the same state.

**Before the tip, and mid-game.** C1 watches its games' books only while it is on, and p0 comes
from those books before the start: a game it did not watch pregame (no ladder open, no pin) has
no p0 and pulls NO_P0 all game, which fails closed. Turn C1 on before the day's first tip (Start
can come later). Turned on, or deskd started, mid-game: each game needs one more score change
before C1 can take, since the feeds that only began to read it cannot date the state they found.

## What it records and shows

- ops.sqlite `signals` (store/store.h): TAKE (sent; again as REFUSED with gw's code if refused)
  and PULL (a route that had the edge but was not stale, or was the trailer, or out of band),
  with the market, side, price, contracts, fair, edge, score, its source and age, and 10, 60 and
  300 s markouts. No SHADOW rows: C1 has none.
- ops.sqlite `scores`: each score state each source saw, with its first sighting and the poll
  before it; `markouts`: every fill's 1, 10, 60 and 300 s markout, gross and net of its fee (the
  300 s one is stored, not streamed). C1's net 300 s markouts go to gw, which counts them toward
  its retire rule.
- `/ops/status` `college`: `enabled`; `why` it trades nothing (the switch that is off, the gate's
  reason, or each series'); `gates_dir`; `gate` (path, stage, verdict, computed, tool);
  `tradable` ("" or why not, per series); `rules` (edge, level, leader_only per series); `clip`,
  `game`, `budget`, `max_score_age_s`; `volley_model` (path, loaded, sha256); and `engine`: takes,
  pulls, refusals, `markouts_to_gw`, and per game its state, source, score age, fair, takes and
  last pull with why (NO_P0, SCORE_OLD, VOLUME, NO_EDGE, TOUCH_AFTER_SCORE, BLOCKED "Systematic
  is not running", ...). Beside it: `espn` and `ncaa` (each feed's `fast_series`, requests,
  failures and the markets it shows), `scores` (rows, the markets whose sources disagree),
  `game_fair` (`models`: each series' model and its sha256, to compare with the gate's; `p0`:
  each game's pregame price) and `wcbb` (the model's path, or why it is not loaded). The log says
  "C1's gate opens ..." or "C1 gated: ..." at start.

## Retirement and lane-reset (gw/sys_book.h, decisions 9)

gw retires C1 by itself: after 100 trades (orders with a fill), once 50 of its fills are marked at
300 s, if their contract-weighted net 300 s markout is 0 or less; and at any count once its life
is down $20 (realized less fees, plus its open positions at the mark; one with no mark, as just
after a restart, counts at its best, so gw waits only while that could bring it back inside). A
voided market's position leaves the book at its cost. Retired: C1's working orders are cancelled
(why RETIRED), the lane goes off with a CRIT alert, LaneSet is refused "lane C1 is retired: <why>
(deskctl lane-reset C1, with deskd stopped)", and its orders LANE_OFF. It stays retired across
restarts (systematic.json).

To give it a fresh life, with that desk stopped: `python3 tools/desk.py lane-reset C1` (for LIVE,
`lane-reset C1 live`). It prints what it cleared (trades, P&L, fills marked). The next start has C1
off, with no trades, markouts or retirement; its open positions stay in the book and its loss rule
still counts them. A retirement is the plan's verdict on the lane: reset it only on new evidence,
a gate run again on the season so far.

## The operator's order (the Mac and the VM; agents never run these on data/)

1. **Record the season** (VM, plan section 7). The recorder records every game of
   REC_COLLEGE_SERIES (KXNCAAWBGAME and KXNCAAWVMATCH among them; the thin men's decile as the
   control): their books by socket and by REST every 4 s, and ESPN's and NCAA.com's scores with
   first-seen times and the poll before (prev_us). Women's basketball opens Nov 2. After the first
   night, `python3 -m recorder.report --data /var/lib/deskd-recorder --days 1`, and confirm the
   series tickers in `kalshi_markets`, the volleyball ESPN path and whether its `espn_scores` rows
   come a point or a set at a time, and NCAA.com's rows (recorder/README.md, "To confirm").
   The gate needs weeks of games: 50+ taken cases a series.
2. **The models.**
   - WCBB: on the Mac, from the WCBB panel (the script's docstring turns wbpanel.pkl into the
     CSV), `python3 tools/train_bball.py --league wcbb --panel wbpanel.csv` writes
     models/wcbb_logit_v1.json (never over a file without `--force`; it prints the sha256;
     docs/wcbb_model.md is the format). Train it before the season: with it the desk
     shows the fair on KXNCAAWBGAME ladders, and your Manual fills there are 5(f)'s overlay
     clicks, which the gate needs (20+, with a positive net 60 s markout).
   - Volleyball: models/volley_markov_v1.json is committed (`tools/volleyball.py --write`) and has
     no fitted parameter (its one is solved from each game's p0). Rewrite it only if the rules
     change: `python3 tools/volleyball.py --write models/volley_markov_v1.json --force`, then run
     the gate again.

   The gate names each model by its sha256, so the files the gate reads must be byte for byte the
   ones the desk loads (`sha256sum models/*.json` on both machines).
3. **The gate**, on the Mac with the recorder's data directory copied down (pyarrow installed, or
   its Parquet hours are skipped with a warning):

   ```sh
   python3 tools/college_gate.py --data <recorder data dir> \
       --wcbb models/wcbb_logit_v1.json --volley models/volley_markov_v1.json \
       --ops-research "<data>/prod/ops-research.sqlite" \
       --write-gate "<data>/demo/gates/C1.json"
   ```

   `--data` is required (no default). `--ops-research` has none either: it is the desk where you
   made the overlay clicks (`<data>/demo/...` for PAPER's), and without it KXNCAAWBGAME cannot
   pass. `--wcbb` and `--volley` default to the repo's models/, which deskd loads too. An existing
   gate is replaced only with `--force`. The defaults are the plan's: `--edge-c 20` (at least 10),
   `--clip 2`, `--game-cap 10`, `--max-score-age-s 5`, `--exec-lag-s 2`, `--min-cases 50`,
   `--min-t 2.0`. The report gives each series' cases, mean, t and verdict, with the 300 s mean by
   side: if the trailer's is not positive, run it again with `--leader-only`. A FAIL is written
   too, and keeps C1 off with its reason.
4. **Enable C1 on PAPER.** In `~/.kalshi/deskd-demo.json` (`systematic_orders` is already on
   there), add `"college": {"enabled": true}` under `"deskd"`, with the gate at
   `<data>/demo/gates/C1.json`. Restart the desk: `python3 tools/desk.py` (or `paper`).
5. **Read `/ops/status` `college`:**

   ```sh
   curl -s http://127.0.0.1:8788/ops/status | python3 -c \
       'import json, sys; print(json.dumps(json.load(sys.stdin)["college"], indent=1))'
   ```

   `why` "" and `tradable` "" for each series the gate opened, `rules` their edge, level and
   leader_only; otherwise `why` names the switch that is off, the gate file's reason, or each
   series' (another model, did not pass). Port 8789 is LIVE's.
6. **Trade.** Turn C1 on in the SYSTEMATIC tab before the day's first tip, and press Start (its
   preflight: KEY, CASH, SHARD, CLK, GROUPS). `engine.games` says what each game waits for. If
   demo lists no college games (TO CONFIRM), PAPER's C1 has nothing to trade: the gate loading
   and the lane row are then all PAPER shows.
7. **Each day,** `python3 tools/scorecard.py`: C1 WAITs under 100 trades or 50 fills marked at
   300 s, then HOLD on a positive net 300 s markout, else RETIRE; RETIRE at any count once down
   $20 (from the desk's systematic.json: gw's own count). gw retires the lane by itself; the
   scorecard is advice.
8. **Retired:** the lane row says why and a CRIT alert said when. Stop the desk, `python3
   tools/desk.py lane-reset C1`, start it, turn C1 on: only after the gate passes again.
9. **LIVE is the operator's alone.** The same in `~/.kalshi/deskd-prod.json` (set
   `systematic_orders` and `college.enabled` yourself), the gate at `<data>/prod/gates/C1.json`,
   `lane-reset C1 live`. Clips start at $2; raise `clip_dollars` (to $5 at most) only after 50+
   live fills whose net 300 s markout is within 3c of the gate's, and restart.

There is no sandbox for C1 (`tools/desk.py sim` has no college day): deskd-runtime_college runs
it end to end against the fake exchange.

## Tests

deskd-take (the rule, each pull, the engine; college_take_cases.json, shared with take_rule.py),
deskd-lane_gate (college_gate_ok.json loads as deskd reads it, both series tradable on their
models), deskd-ncaa (NCAA.com's scoreboards and the merge), deskd-game_fair, deskd-volley,
deskd-wcbb and deskd-bball (the models), deskd-ops (the college section's keys and ranges),
deskd-sys_book and deskd-sys_gate (the retire rules, retirement across a restart, lane-reset, the
event cap), deskd-runtime_college (C1 end to end: no gate, a live gate, one SC1- take in its group
and its signals row, KILL, scores that stop, the game cap, the loss rule across a restart), and
the tools' self-tests (college-gate, take-rule, volleyball, train-bball, scorecard).

## Not confirmed here (no network in the build container)

ESPN's volleyball league path and whether its line scores carry the set's points live;
NCAA.com's casablanca scoreboard shapes and period words (the fixtures are hand-built); the
series tickers and their fee multipliers (the gate assumes 1 where a market row gives none, and
says so); whether Kalshi's demo lists college games; the depth at the touch in thin books; ESPN's
real lag in women's basketball; how often the two scoreboards disagree in play; whether a void
refunds fees. No WCBB model ships: KXNCAAWBGAME cannot pass until one is trained.
