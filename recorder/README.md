# deskd recorder

Kalshi's market data to hourly zstd Parquet, 24/7, on the Oracle VM (plan section 7;
`docs/deploy-oracle.md` is the deployment plan). Read-only: GETs and socket subscriptions, never
an order (`kalshi.py` has no write route).

What it records (`rec.py`): `orderbook_delta` and `trade` for every market of the book series
(the ten 15-minute financial series and five weather highs by default), each window subscribed
20 minutes before it opens; `pyth_value` for every underlying; each market's REST row whenever
its status, times, strike or result changes; and a gaps table (disconnects, sequence gaps,
refused subscriptions, clock steps, torn spool lines). `upload.py` sends finished hours to
Cloudflare R2, verified by Content-MD5, on its own hourly timer.

```
<data>/spool/<table>/<hour>.jsonl      what was written, flushed every second
<data>/parquet/<table>/date=YYYY-MM-DD/<table>-<hour>.parquet
<data>/manifest.jsonl                  one row per Parquet file (rows, time range, sha256)
<data>/uploaded.jsonl                  one row per verified upload
```

## Tests

```sh
python3 -m venv .venv && .venv/bin/pip install -r recorder/requirements.txt
.venv/bin/python -m unittest discover -s recorder/tests -t .
```

`test_rec_fake` runs the recorder against `build-deskd/fake-kalshi` (build it first). Without
pyarrow the Parquet tests skip.

## Run by hand (a local soak)

Settings come from the environment, secrets from files (never the environment or a command line):

```sh
KALSHI_REST=https://demo-api.kalshi.co/trade-api/v2 KALSHI_WS=wss://demo-api.kalshi.co/trade-api/ws/v2 \
KALSHI_KEY_ID_FILE=~/.kalshi/rec.id KALSHI_KEY_FILE=~/.kalshi/rec.pem REC_DATA_DIR=/tmp/rec \
  .venv/bin/python -m recorder.rec
```

All settings: `recorder/deploy/recorder.env.example` and `config.py`.

## On the VM

From the Mac, in the repo (the existing VM and key; `DESKD_VM`, `DESKD_VM_KEY` override them):

```sh
recorder/deploy/deploy.sh                    # plan: copies the committed recorder, says what setup would do
recorder/deploy/deploy.sh --apply            # installs: user, dirs, release, venv, units, recorder.env
```

`setup.sh` never replaces an existing `recorder.env`, credential or changed unit (a changed unit
is written beside it as `.new`; `--replace-units` switches, keeping `.bak`) and never deletes a
release. Then put the credentials on the VM, root-owned, 0400, in
`/etc/deskd-recorder/credentials/`:

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
subscribed and discovery succeeds). A new deploy is a new release under
`/opt/deskd-recorder/releases/`; `current` points at the newest, and `--start` restarts onto it.
