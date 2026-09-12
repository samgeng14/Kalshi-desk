# Where the data lives

Measured 2026-09-11 on `data/prod.sqlite`.

## The problem, in numbers

| object | size |
|---|---:|
| `quotes` table | 659.8 MB |
| `idx_quotes_ticker` | 301.4 MB |
| **quotes, total** | **961 MB** |
| observations | 54.8 MB |
| model_fairs (+ index) | 72.3 MB |
| everything else | ~87 MB |
| **database** | **1.17 GB** |

Quotes are **82% of the database**, there is no retention on them anywhere in the code, and the
file grows about 750 MB a day. The disk has hit 97-99% three times and a full disk is what
corrupted this database on 2026-09-07 and again a few days later.

## The history is not the problem

The same 6.86M quote rows, exported as gzipped CSV:

| form | size | vs sqlite |
|---|---:|---:|
| sqlite (table + index) | 961 MB | — |
| csv, uncompressed | 613 MB | 1.6x |
| **csv, gzip -6** | **119 MB** | **8.1x** |

A b-tree with a secondary index is the wrong shape for an append-only time series nobody updates.
Parquet with zstd and dictionary-encoded tickers would be smaller again; gzip is the conservative
number because it needs nothing installed.

## What to do

`scripts/archive_quotes.py` moves quotes older than N days into one gzipped file per day under
`data/archive/`, then deletes them from the live database. The order matters and the script
enforces it: every day file is written, re-read byte for byte, and row-counted through the same
reader research will use, and only then is anything deleted. It refuses to run at all while the
desk is writing, because the delete would contend and the vacuum is impossible.

```
scripts/tradeit down
python3 scripts/archive_quotes.py --days 7            # dry run, changes nothing
python3 scripts/archive_quotes.py --days 7 --apply --vacuum
scripts/tradeit up
```

Keeping seven days hot should take the database from 1.17 GB to roughly 250 MB, with about
120 MB of archive beside it.

Nothing is lost for research. DuckDB reads the archive in place, no import step:

```sql
select ticker, count(*), avg(yes_ask - yes_bid)
from read_csv_auto('data/archive/quotes-2026-*.csv.gz')
group by 1 order by 2 desc;
```

## Offsite

Not Google Drive. The archive is a pile of immutable day files, which is exactly what object
storage is for, and the one that fits best is **Cloudflare R2**: 10 GB stored free, 1M writes and
10M reads a month free, and **egress is free at any volume** — which is the part that matters,
because a research archive is pulled back far more often than it is written. Past the free tier
it is $0.015 per GB per month, so the whole archive costs cents even when it is years long.
At roughly 40 MB of compressed quotes a day, 10 GB is about eight months before a bill starts.

R2 speaks S3, so no bespoke client is needed:

```
brew install rclone
rclone config          # New remote -> s3 -> Cloudflare R2 -> paste the R2 token
rclone sync data/archive r2:kalshi-desk-archive --progress
```

Two alternatives considered and why they lost:

- **Backblaze B2** — also 10 GB free, but egress is only free up to 3x stored per month. Fine
  until a backfill wants to read the whole archive twice.
- **Turso** — 5 GB free and it *is* libSQL, so the archive would stay queryable as SQL without
  DuckDB. Tempting, but it stores the data in the same b-tree shape that caused this problem, so
  5 GB there holds far less history than 10 GB of compressed files.

## What is still unfixed

The desk itself has no retention policy: it will refill `quotes` at 750 MB a day whether or not
the archive exists. Archiving on a schedule is a chore that will be forgotten. The real fix is a
retention setting in the engine that rolls quotes out on its own, and that is not written yet.
