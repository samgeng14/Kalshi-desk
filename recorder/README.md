# deskd recorder

Kalshi's market data to hourly zstd Parquet, 24/7, on the Oracle VM (plan section 7;
`docs/deploy-oracle.md` is the deployment plan). Read-only: GETs and socket subscriptions, never
an order (`kalshi.py` has no write route). Every subscription and every REST poll is an explicit
list of markets; an empty list records nothing (to Kalshi it would mean the whole exchange).

What it records (`rec.py`):

- `orderbook_delta` and `trade` for every market of the book series (the ten 15-minute financial
  series and five weather highs by default), each window subscribed 20 minutes before it opens;
  `pyth_value` for every underlying; each market's REST row whenever its status, times, strike or
  result changes.
- Plan 5(d), the college and thin-sport games (`college.py`): every game of the college series
  (women's basketball, volleyball, college hockey, table tennis, AHL, Liiga, SHL, CS2), listed
  hourly, on the same socket lists and lead/linger rules; of men's college basketball (the
  control) only the thinnest tenth by pregame volume, picked once a day at 15 UTC and kept.
- `market_lifecycle_v2` on the recorded markets and the college games (raw, in `kalshi_ws`; a game
  closed, determined or settled leaves the lists at once instead of at the next hourly listing).
- By public REST (`polls.py`): the college books every 4 s (every market's top of book in batches,
  the whole book of those whose top moved in the last 5 minutes), and every recorded book once the
  socket's book subscription has been missing 5 minutes (the day-one fallback); trades over every
  window the socket may have missed (a disconnect, a sequence gap on trades, a restart) and every
  hour, for every market recordable in the window (one that closed while the recorder was down
  too), a window not read before a stop read after the restart; incentive programs hourly; every
  open market once a day.
- ESPN's and NCAA.com's scoreboards (`scores.py`) every 4 s while a game is on, each change of
  state or score written once with the local time it was first seen.
- A gaps table: disconnects and resumptions, sequence gaps, refused subscriptions, failed
  discovery and polls (one row spanning each outage), clock steps, restarts (from the socket's
  last frame on disk), torn spool lines.

`upload.py` sends finished hours to Cloudflare R2, verified by Content-MD5, on its own hourly timer.

```
<data>/spool/<table>/<hour>.jsonl      what was written, flushed every second
<data>/parquet/<table>/date=YYYY-MM-DD/<table>-<hour>.parquet
<data>/manifest.jsonl                  one row per Parquet file (rows, time range, sha256)
<data>/uploaded.jsonl                  one row per verified upload
<data>/state/recorder.json             the day's control picks, the day of the last daily sweep,
                                       the trade backfill windows not yet read
```

| Table | One row per | Times |
|---|---|---|
| `kalshi_ws` | socket frame, raw (books, trades, pyth_value, lifecycle, replies) | recv_us, mono_us |
| `kalshi_markets` | market whose status, times, strike or result changed | recv_us |
| `kalshi_rest_books` | REST book that differs from the last one recorded for it (`kind` top or depth, `mode` college or fallback) | recv_us = first seen; sent_us; prev_us = the poll before |
| `kalshi_trades_rest` | public print by REST, once per trade_id (`reason`: restart, disconnect, seq_gap, hourly) | recv_us, created_us |
| `kalshi_incentives` | incentive program of the kept types, every hour (`sweep_us` groups a sweep) | recv_us, sweep_us |
| `kalshi_markets_daily` | open market, once a day (raw without the rules text) | recv_us, sweep_us |
| `espn_scores`, `ncaa_scores` | game whose state, status, period, score or start changed (the clock alone is no change) | recv_us = first seen; prev_us = the poll before |
| `college_control` | control game on its date, ranked by volume when picked, `picked` flagged | recv_us |
| `gaps` | gap (source, kind, from_us..to_us) | recv_us |

## To confirm before November 2 (operator)

- **Series tickers** (`REC_COLLEGE_SERIES`, `REC_CONTROL_SERIES`; best guesses in
  `deploy/recorder.env.example` and `config.py` with where each came from). From the repo's own
  research: `KXNCAAWBGAME`, `KXNCAAMBGAME`, `KXNCAAWVMATCH`, `KXNCAAHOCKEYGAME`, `KXLIIGAGAME`; from
  the demo listing: `KXTTSTARMATCH` (other table tennis series may exist); guesses: `KXAHLGAME`,
  `KXSHLGAME`, `KXCS2GAME`. A wrong ticker lists nothing: check `kalshi_markets` after the first
  hour, or the log's `discovery:` lines.
- **ESPN league paths** for volleyball and college hockey (`REC_ESPN_LEAGUES`); basketball's are
  the ones deskd already reads (`groups=50`, Division I).
- **NCAA.com's scoreboard** (`REC_NCAA_URL`, `REC_NCAA_SPORTS`): `data.ncaa.com/casablanca/...`
  is unofficial and undocumented. The parser follows its known shape (`tests/fixtures/
  ncaa_scoreboard.json` is hand-built, not a capture). TODO: capture one live file per sport on the
  VM, replace the fixture, and set `REC_NCAA_SPORTS=off` if it is wrong (a failing feed only
  writes `ncaa` gap rows).
- **`market_lifecycle_v2` with a market list**: if the venue refuses it, `gaps` shows a
  `subscribe_error` on that channel and it is retried every 10 minutes; `REC_LIFECYCLE=0` stops it.
- **How many markets one subscription may carry**: with every listed game the socket lists grow
  from about 50 markets to several hundred. A refusal shows as `subscribe_error` gaps (the REST
  books still cover the college games). Likewise tickers per `GET /markets?tickers=` (100 per
  request; a refused batch halves itself).
- **`mve_filter=exclude`** on the daily `GET /markets` (leave out the multivariate combinations):
  TO CONFIRM; an unknown parameter is ignored and the sweep is then just bigger.
- The incentive types kept (`REC_INCENTIVE_TYPES`, default `liquidity,volume`).

## The gaps report (the "24 h, gaps under 1% of minutes" milestone)

```sh
python3 -m recorder.report --data /var/lib/deskd-recorder           # every day on disk
python3 -m recorder.report --data /var/lib/deskd-recorder --days 1 --json
```

Minutes with a gap per source and day, minutes the socket recorded nothing in (`silent_ws`: a dead
VM logs nothing, but leaves silence), their share of each day's recorded minutes, rows and bytes
per table, and a PASS/FAIL for the last 24 hours. Parquet is read with pyarrow, else duckdb;
without either it counts the Parquet files from the manifest and says so.

## Tests

```sh
python3 -m venv .venv && .venv/bin/pip install -r recorder/requirements.txt
.venv/bin/python -m unittest discover -s recorder/tests -t .
```

ctest runs the same (`recorder-unit`) with whatever `python3` it finds: tests that need pyarrow,
cryptography, websockets or duckdb skip without them. `test_rec_fake` runs the recorder against
`build-deskd/fake-kalshi` (build it first, or set `FAKE_KALSHI`) with the scoreboards served from
loopback; `test_base_sh` runs `deploy/base.sh` with every system command stubbed. No test touches
the network.

## Run by hand (a local soak)

Settings come from the environment, secrets from files (never the environment or a command line):

```sh
KALSHI_REST=https://demo-api.kalshi.co/trade-api/v2 KALSHI_WS=wss://demo-api.kalshi.co/trade-api/ws/v2 \
KALSHI_KEY_ID_FILE=~/.kalshi/rec.id KALSHI_KEY_FILE=~/.kalshi/rec.pem REC_DATA_DIR=/tmp/rec \
REC_ESPN_LEAGUES=off REC_NCAA_SPORTS=off \
  .venv/bin/python -m recorder.rec
```

(The scoreboards are on by default; `off` keeps a soak to Kalshi.) All settings:
`recorder/deploy/recorder.env.example` and `config.py`.

## On the VM

From the Mac, in the repo (the existing VM and key; `DESKD_VM`, `DESKD_VM_KEY` override them):

```sh
recorder/deploy/deploy.sh                    # plan: copies the committed recorder, says what setup would do
recorder/deploy/deploy.sh --apply            # installs: base system, user, dirs, release, venv, units, recorder.env
```

`setup.sh` first runs `base.sh` in the same mode: chrony (systemd-timesyncd off; apt may remove
only timesyncd's package to install it, anything more is refused), a 2 GB `/swapfile` by a swap
unit when the VM has no swap, journald capped at 1 GB, and unattended-upgrades for security
updates only with no automatic reboot. Neither script replaces an existing `recorder.env`,
credential, changed unit or changed config file (a changed one gets ours beside it as `.new`;
`--replace-units` switches units, keeping `.bak`), and neither deletes a release. Then put the
credentials on the VM, root-owned, 0400, in `/etc/deskd-recorder/credentials/`:

| File | What | Needed |
|---|---|---|
| `kalshi-readonly.pem`, `kalshi-readonly.id` | a production Kalshi API key created with **read access only** (never the trading key) | yes |
| `r2-write.token` | `{"account_id", "access_key_id", "secret_access_key", "bucket"}`, an R2 token that can only write the bucket | for uploads |
| `healthchecks.url`, `healthchecks-upload.url` | healthchecks.io ping URLs for the recorder and the uploads | optional |

```sh
recorder/deploy/deploy.sh --apply --start    # starts the recorder; the upload timer once the R2 token is there
ssh -i ~/.ssh/oracle_deskd ubuntu@64.181.211.165 'journalctl -u deskd-recorder -f'
```

systemd restarts it on a crash and on a stalled watchdog (no `WATCHDOG=1` unless the socket is
subscribed and discovery succeeds; a scoreboard or REST poll failing is a gap, not a restart). A
new deploy is a new release under `/opt/deskd-recorder/releases/`; `current` points at the
newest, and `--start` restarts onto it.
