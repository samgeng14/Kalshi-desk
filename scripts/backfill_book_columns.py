#!/usr/bin/env python3
"""Recompute the parsed columns of commodity_book_events and commodity_quotes from raw_json.

Why this exists: until 2026-09-24 the recorder (MarketQuoteFeed) read book deltas from keys the
venue never sends (`price`, `delta` instead of `price_dollars`, `delta_fp`), so every delta row
has price 0 and delta 0; and it divided the ticker's *_fp fields by 100, so every non-zero size,
volume and open interest in commodity_quotes is 100 times too small. Snapshots were stored with
side '' and price, delta and venue time 0, which read as values. raw_json holds every frame as
the venue sent it, so all of it can be recomputed. This does that, the same way the fixed recorder
now fills the columns:

  delta     price = msg.price_dollars, delta = msg.delta_fp, side = msg.side when 'yes' or 'no',
            ts_exchange_ms = msg.ts_ms; a field that is absent is NULL, never 0
  snapshot  side, price and delta NULL (a snapshot has none); ts_exchange_ms = msg.ts_ms or NULL
  ticker    yes_bid_size, yes_ask_size, volume, open_interest = the *_fp strings as contracts
  a raw_json that is not JSON: those columns NULL (unknown), never rescaled

Every value comes from raw_json, so running it again changes nothing. Rows already written by the
fixed recorder come out the same; rows it stored without their frame (see --trim) are skipped.

  python3 scripts/backfill_book_columns.py --dry-run          # counts only
  python3 scripts/backfill_book_columns.py                    # recompute and verify
  python3 scripts/backfill_book_columns.py --trim             # then drop frames the columns hold

--trim clears raw_json on delta rows whose columns provably equal the frame (compared field by
field after the recompute) and whose frame has no key beyond the documented ones: top level type,
sid, seq, msg; msg market_ticker, market_id, price_dollars, delta_fp, side, ts, ts_ms. A frame with
anything else keeps it; the venue documents client_order_id and subaccount, present only when our
own order moved the book. What a trimmed row loses is the sid (in the 'subscribed'/'connect'
rows), market_id (one per market, in the snapshot and ticker rows) and the microseconds of `ts`.
Snapshots, replies and markers keep their frames. Archive first (scripts/archive_quotes.py): the
day files keep raw_json whole. A trim shrinks rows where they stand, so the space stays inside the
table's pages, and new rows (appended at the end) do not reuse it: on 2026-09-24 trimming 16.0M
deltas left 4.86 GB unused inside 6.25 GB of pages and only 0.61 GB of free pages. The disk gets it
back with a VACUUM (scripts/archive_quotes.py ... --vacuum), with the desk stopped.

Like the archive, it runs only with the desk and its supervisor stopped, and holds the desk's lock
for the whole run, so neither can start underneath it. Batches of --batch rows, each its own
transaction, with the WAL checkpointed after each so it never grows past one batch.
"""
import argparse, os, shutil, sqlite3, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from archive_quotes import take_desk_lock, supervisor_alive  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BOOK_DELTA = """UPDATE commodity_book_events SET
  price = CAST(json_extract(raw_json, '$.msg.price_dollars') AS REAL),
  delta = CAST(json_extract(raw_json, '$.msg.delta_fp') AS REAL),
  side = CASE WHEN json_extract(raw_json, '$.msg.side') IN ('yes', 'no')
              THEN json_extract(raw_json, '$.msg.side') END,
  ts_exchange_ms = json_extract(raw_json, '$.msg.ts_ms')
WHERE id BETWEEN ? AND ? AND kind = 'orderbook_delta' AND raw_json IS NOT NULL AND json_valid(raw_json)"""
BOOK_SNAP = """UPDATE commodity_book_events SET side = NULL, price = NULL, delta = NULL,
  ts_exchange_ms = CASE WHEN json_valid(raw_json) THEN json_extract(raw_json, '$.msg.ts_ms') END
WHERE id BETWEEN ? AND ? AND kind = 'orderbook_snapshot'"""
BOOK_BAD = """UPDATE commodity_book_events SET price = NULL, delta = NULL
WHERE id BETWEEN ? AND ? AND kind = 'orderbook_delta' AND raw_json IS NOT NULL AND NOT json_valid(raw_json)"""
QUOTES = """UPDATE commodity_quotes SET
  yes_bid_size = CAST(json_extract(raw_json, '$.msg.yes_bid_size_fp') AS REAL),
  yes_ask_size = CAST(json_extract(raw_json, '$.msg.yes_ask_size_fp') AS REAL),
  volume = CAST(json_extract(raw_json, '$.msg.volume_fp') AS REAL),
  open_interest = CAST(json_extract(raw_json, '$.msg.open_interest_fp') AS REAL)
WHERE id BETWEEN ? AND ? AND raw_json IS NOT NULL AND json_valid(raw_json)"""
QUOTES_BAD = """UPDATE commodity_quotes SET yes_bid_size = NULL, yes_ask_size = NULL, volume = NULL,
  open_interest = NULL
WHERE id BETWEEN ? AND ? AND raw_json IS NOT NULL AND NOT json_valid(raw_json)"""

# A delta row whose columns hold its whole frame: every documented field present and equal to its
# column, and no key outside the documented set.
HELD = """kind = 'orderbook_delta' AND raw_json IS NOT NULL AND json_valid(raw_json)
  AND NOT EXISTS (SELECT 1 FROM json_each(raw_json) WHERE key NOT IN ('type', 'sid', 'seq', 'msg'))
  AND NOT EXISTS (SELECT 1 FROM json_each(raw_json, '$.msg') WHERE key NOT IN
                  ('market_ticker', 'market_id', 'price_dollars', 'delta_fp', 'side', 'ts', 'ts_ms'))
  AND price IS NOT NULL AND price = CAST(json_extract(raw_json, '$.msg.price_dollars') AS REAL)
  AND delta IS NOT NULL AND delta = CAST(json_extract(raw_json, '$.msg.delta_fp') AS REAL)
  AND side IN ('yes', 'no') AND side = json_extract(raw_json, '$.msg.side')
  AND ts_exchange_ms IS NOT NULL AND ts_exchange_ms = json_extract(raw_json, '$.msg.ts_ms')
  AND sequence IS NOT NULL AND sequence = json_extract(raw_json, '$.seq')
  AND ticker = json_extract(raw_json, '$.msg.market_ticker')"""
TRIM = f"UPDATE commodity_book_events SET raw_json = NULL WHERE id BETWEEN ? AND ? AND {HELD}"

# After the recompute, rows whose columns still disagree with their frame (NULL-safe). Must be 0.
VERIFY_BOOK = """SELECT COUNT(*) FROM commodity_book_events WHERE kind = 'orderbook_delta'
  AND raw_json IS NOT NULL AND json_valid(raw_json) AND (
    price IS NOT CAST(json_extract(raw_json, '$.msg.price_dollars') AS REAL)
    OR delta IS NOT CAST(json_extract(raw_json, '$.msg.delta_fp') AS REAL))"""
VERIFY_QUOTES = """SELECT COUNT(*) FROM commodity_quotes WHERE raw_json IS NOT NULL AND json_valid(raw_json) AND (
    yes_bid_size IS NOT CAST(json_extract(raw_json, '$.msg.yes_bid_size_fp') AS REAL)
    OR volume IS NOT CAST(json_extract(raw_json, '$.msg.volume_fp') AS REAL))"""


def batches(db, table, batch):
    lo, hi = db.execute(f"SELECT MIN(id), MAX(id) FROM {table}").fetchone()
    if lo is None:
        return
    for a in range(lo, hi + 1, batch):
        yield a, min(a + batch - 1, hi)


def apply(db, table, stmts, batch, label):
    t0 = time.time()
    changed = 0
    for a, b in batches(db, table, batch):
        db.execute("BEGIN IMMEDIATE")
        try:
            for s in stmts:
                changed += db.execute(s, (a, b)).rowcount
            db.execute("COMMIT")
        except BaseException:
            db.execute("ROLLBACK")
            raise
        db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    dt = time.time() - t0
    print(f"{label}: {changed:,} row updates in {dt:.0f}s")
    return changed


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--db', default=None, help='database (default: data/prod.sqlite)')
    ap.add_argument('--batch', type=int, default=100000)
    ap.add_argument('--dry-run', action='store_true', help='count what would change; write nothing')
    ap.add_argument('--trim', action='store_true', help='after the recompute, drop frames the columns hold')
    a = ap.parse_args()
    db_path = os.path.abspath(a.db or os.path.join(ROOT, 'data', 'prod.sqlite'))
    if not os.path.exists(db_path):
        sys.exit(f'no database at {db_path}')
    if supervisor_alive(db_path):
        sys.exit('the desk supervisor is running; stop it first (scripts/tradeit down)')
    lock = take_desk_lock(db_path)   # held until the process exits
    if lock is None:
        sys.exit('the desk is running on this database; stop it first (scripts/tradeit down)')
    free_gb = shutil.disk_usage(os.path.dirname(db_path)).free / 1e9
    if not a.dry_run and free_gb < 3:
        sys.exit(f'{free_gb:.1f} GB free; the recompute needs a few GB of headroom')

    db = sqlite3.connect(db_path, isolation_level=None, timeout=5)
    if db.execute("PRAGMA journal_mode").fetchone()[0].lower() != 'wal':
        sys.exit('expected a WAL-mode database')

    counts = db.execute("""SELECT
        SUM(kind = 'orderbook_delta'), SUM(kind = 'orderbook_snapshot'),
        SUM(kind = 'orderbook_delta' AND raw_json IS NULL),
        SUM(kind = 'orderbook_delta' AND raw_json IS NOT NULL AND NOT json_valid(raw_json)),
        SUM(kind = 'orderbook_delta' AND (price = 0 OR delta = 0))
      FROM commodity_book_events""").fetchone()
    q = db.execute("""SELECT COUNT(*), SUM(raw_json IS NOT NULL AND NOT json_valid(raw_json))
      FROM commodity_quotes""").fetchone()
    print(f"book: {counts[0] or 0:,} deltas ({counts[4] or 0:,} with price or delta 0, "
          f"{counts[2] or 0:,} already without a frame, {counts[3] or 0:,} unreadable), "
          f"{counts[1] or 0:,} snapshots")
    print(f"quotes: {q[0]:,} rows ({q[1] or 0:,} unreadable)")
    if a.dry_run:
        mism_b = db.execute(VERIFY_BOOK).fetchone()[0]
        mism_q = db.execute(VERIFY_QUOTES).fetchone()[0]
        print(f"dry run: {mism_b:,} delta rows and {mism_q:,} ticker rows disagree with their frame now")
        if a.trim:
            print(f"dry run: {db.execute(f'SELECT COUNT(*) FROM commodity_book_events WHERE {HELD}').fetchone()[0]:,} "
                  "delta rows are held whole by their columns right now (the recompute comes first)")
        return

    apply(db, 'commodity_book_events', [BOOK_DELTA, BOOK_SNAP, BOOK_BAD], a.batch, 'book')
    apply(db, 'commodity_quotes', [QUOTES, QUOTES_BAD], a.batch, 'quotes')
    mism_b = db.execute(VERIFY_BOOK).fetchone()[0]
    mism_q = db.execute(VERIFY_QUOTES).fetchone()[0]
    print(f"verify: {mism_b:,} delta rows and {mism_q:,} ticker rows disagree with their frame (must be 0)")
    if mism_b or mism_q:
        sys.exit('verification failed; not trimming')
    if a.trim:
        apply(db, 'commodity_book_events', [TRIM], a.batch, 'trim')
        left = db.execute("SELECT COUNT(*) FROM commodity_book_events WHERE kind = 'orderbook_delta' "
                          "AND raw_json IS NOT NULL").fetchone()[0]
        print(f"trim: {left:,} delta rows keep their frame (a key the columns do not hold, or unreadable)")
    db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    db.close()


if __name__ == '__main__':
    main()
