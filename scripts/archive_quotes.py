#!/usr/bin/env python3
"""Move old quotes out of the live database into compressed day files.

Why this exists: on 2026-09-11 `quotes` was 659.8 MB with a 301.4 MB index, together 82% of a
1.17 GB database that grows about 750 MB a day and has no retention. A full disk is what
corrupted this database twice. The same 6.86M rows are ~119 MB as gzipped CSV, so the history is
not the problem; keeping it in a b-tree with an index on it is.

The order of operations is the whole point. Nothing is deleted until the archive has been read
back and checked row for row, and the delete and the reclaim happen only with the desk stopped.

  python3 scripts/archive_quotes.py --days 7              # what it would do, changes nothing
  python3 scripts/archive_quotes.py --days 7 --apply      # archive, verify, delete
  python3 scripts/archive_quotes.py --days 7 --apply --vacuum   # and hand the pages back to the disk

Reading it back for research, no import needed:
  duckdb -c "select * from read_csv_auto('data/archive/quotes-2026-09-*.csv.gz')"
"""
import argparse, csv, gzip, hashlib, io, os, sqlite3, sys, time
from datetime import datetime, timezone

COLS = ['id','ts','ticker','yes_bid','yes_ask','no_bid','no_ask','last',
        'yes_bid_size','yes_ask_size','volume','open_interest']
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def day_of(ts):
    return datetime.fromtimestamp(ts, timezone.utc).strftime('%Y-%m-%d')


def desk_running(db):
    """A live writer makes the delete contend and the vacuum impossible. Detect it rather than
    discover it halfway through."""
    wal = db + '-wal'
    if not os.path.exists(wal):
        return False
    size = os.path.getsize(wal)
    time.sleep(2.0)
    return os.path.exists(wal) and os.path.getsize(wal) != size


def write_day(rows, path):
    """Write one day, then read it straight back. An archive nobody has read is not a backup."""
    buf = io.StringIO()
    w = csv.writer(buf)
    w.writerow(COLS)
    w.writerows(rows)
    payload = buf.getvalue().encode()
    tmp = path + '.part'
    with gzip.open(tmp, 'wb', compresslevel=6) as f:
        f.write(payload)
    with gzip.open(tmp, 'rb') as f:
        back = f.read()
    if back != payload:
        os.remove(tmp)
        raise RuntimeError(f'{path}: archive did not read back byte-identical')
    os.replace(tmp, path)
    return len(payload), os.path.getsize(path), hashlib.sha256(payload).hexdigest()[:16]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--db', default=os.path.join(ROOT, 'data', 'prod.sqlite'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'data', 'archive'))
    ap.add_argument('--days', type=int, default=7, help='days of quotes to keep in the live database')
    ap.add_argument('--apply', action='store_true', help='actually delete what has been archived')
    ap.add_argument('--vacuum', action='store_true', help='rebuild the file so the disk gets the pages back')
    a = ap.parse_args()

    cutoff = int(time.time()) - a.days * 86400
    print(f'keeping quotes newer than {day_of(cutoff)} ({a.days}d); archiving everything older')

    if a.apply and desk_running(a.db):
        sys.exit('the desk is writing to this database right now. Stop it first:\n'
                 '  scripts/tradeit down   (then re-run, then scripts/tradeit up)')

    os.makedirs(a.out, exist_ok=True)
    db = sqlite3.connect(a.db, timeout=60)
    db.execute('pragma busy_timeout=30000')

    days = [r[0] for r in db.execute(
        "select strftime('%Y-%m-%d', ts, 'unixepoch') d from quotes where ts < ? "
        "group by d order by d", (cutoff,))]
    if not days:
        print('nothing old enough to archive'); return

    total_rows = total_gz = 0
    archived = []
    for d in days:
        lo = int(datetime.strptime(d, '%Y-%m-%d').replace(tzinfo=timezone.utc).timestamp())
        hi = lo + 86400
        rows = list(db.execute(
            f"select {','.join(COLS)} from quotes where ts >= ? and ts < ? order by id", (lo, hi)))
        if not rows:
            continue
        path = os.path.join(a.out, f'quotes-{d}.csv.gz')
        if os.path.exists(path):
            print(f'  {d}  {len(rows):>9,} rows  already archived, skipping')
            continue
        raw, gz, digest = write_day(rows, path)
        # Read the file back through the same reader research will use and count it again.
        with gzip.open(path, 'rt') as f:
            back = sum(1 for _ in csv.reader(f)) - 1
        if back != len(rows):
            sys.exit(f'{path}: wrote {len(rows)} rows, read back {back}. Nothing deleted.')
        print(f'  {d}  {len(rows):>9,} rows  {raw/1e6:>7.1f} MB -> {gz/1e6:>6.1f} MB  sha {digest}')
        archived.append((lo, hi, len(rows)))
        total_rows += len(rows); total_gz += gz

    if not archived:
        print('every eligible day is already archived'); return
    print(f'\n{total_rows:,} rows archived into {total_gz/1e6:.0f} MB across {len(archived)} day files')

    if not a.apply:
        print('\ndry run: nothing was deleted. Re-run with --apply to remove the archived rows.')
        return

    deleted = 0
    for lo, hi, n in archived:
        cur = db.execute('delete from quotes where ts >= ? and ts < ?', (lo, hi))
        deleted += cur.rowcount
        db.commit()
    print(f'deleted {deleted:,} archived rows from the live database')

    if a.vacuum:
        before = os.path.getsize(a.db)
        print('vacuuming (this rewrites the file and needs room for a second copy)...')
        db.execute('vacuum')
        after = os.path.getsize(a.db)
        print(f'  {before/1e9:.2f} GB -> {after/1e9:.2f} GB, {(before-after)/1e9:.2f} GB back')
    else:
        print('the pages are free inside the file but not returned to the disk; re-run with '
              '--vacuum when you can spare the room for a rewrite')


if __name__ == '__main__':
    main()
