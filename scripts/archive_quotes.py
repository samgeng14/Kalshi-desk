#!/usr/bin/env python3
"""Move old rows out of the live database into compressed day files.

Why this exists: on 2026-09-11 `quotes` was 659.8 MB with a 301.4 MB index, together 82% of a
1.17 GB database that grows about 750 MB a day and has no retention. A full disk is what
corrupted this database twice. The history is not the problem; keeping it in a b-tree with an
index on it is. On 2026-09-23 the raw commodity WebSocket capture (commodity_book_events) was
38.6M rows and 13.5 GB, growing about 12M rows and 4 GB a day, against 31 GB free.

  python3 scripts/archive_quotes.py --table commodity_book_events --days 3           # archive only
  python3 scripts/archive_quotes.py --table commodity_book_events --days 3 --apply   # and delete
  python3 scripts/archive_quotes.py --table commodity_book_events --days 3 --apply --vacuum
  python3 scripts/archive_quotes.py --days 7 --apply                                  # quotes

The desk must be stopped for ANY run, not only --apply: the archive reads every archived row, and
nothing on the research side reads data/prod.sqlite while the desk is running.

HOW IT STAYS SAFE AND FAST (rewritten 2026-09-23).
- Rows are selected by rowid range, not by the time column. None of these tables has an index on
  its time column, so "WHERE ts >= day AND ts < next" was a full scan of the table for every day,
  three times over (count, export, delete): hours on a 13 GB table. Rows are appended in arrival
  order, so the day boundaries are found by binary search over rowid (a few dozen indexed probes)
  and each day is one sequential range. A row whose clock sits a few milliseconds across midnight
  from its neighbours lands in the adjacent day's file; the count of such rows is reported. Every
  row is still archived exactly once.
- Each day file is written, then read back from disk and checked (row count and SHA-256 of the
  decompressed bytes) before its manifest (<file>.json: rowid range, rows, sha) is written.
- Deletion uses the manifests' rowid ranges only, in ONE transaction: if any day's delete removes a
  different number of rows than its manifest archived, everything is rolled back and nothing is
  deleted. A day already deleted by an earlier run is marked in its manifest and skipped.
- When the delete removes most of the table, the table's own indexes are dropped for the delete
  and re-created afterwards, inside the same transaction: removing tens of millions of entries from
  an index one by one is the slowest thing SQLite does.

Reading it back for research, no import needed:
  duckdb -c "select * from read_csv_auto('data/archive/quotes-2026-09-*.csv.gz')"
"""
import argparse, csv, fcntl, gzip, hashlib, json, os, shutil, sqlite3, sys, time
from datetime import datetime, timezone

QUOTE_COLS = ['id', 'ts', 'ticker', 'yes_bid', 'yes_ask', 'no_bid', 'no_ask', 'last',
              'yes_bid_size', 'yes_ask_size', 'volume', 'open_interest']
# table -> (columns, time column, units of that column per second)
TABLES = {
    'quotes': (QUOTE_COLS, 'ts', 1),
    'commodity_book_events': (['id', 'ts_received_ms', 'ts_exchange_ms', 'ticker', 'kind', 'sequence', 'side',
                               'price', 'delta', 'raw_json'], 'ts_received_ms', 1000),
    'commodity_quotes': (['id', 'ts_received_ms', 'ts_exchange_ms', 'ticker', 'yes_bid', 'yes_ask', 'yes_bid_size',
                          'yes_ask_size', 'last', 'volume', 'open_interest', 'raw_json'], 'ts_received_ms', 1000),
}
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def day_of(ts):
    return datetime.fromtimestamp(ts, timezone.utc).strftime('%Y-%m-%d')


def day_start(day):
    return int(datetime.strptime(day, '%Y-%m-%d').replace(tzinfo=timezone.utc).timestamp())


def take_desk_lock(db):
    """Take the desk's own lock (<db>.lock, flock) for the whole run and keep it: while this script
    holds it a desk cannot start on this database (it exits 'another kalshi-desk is running' and a
    supervisor waits). Returns the open fd, or None when a desk holds it now."""
    fd = os.open(db + '.lock', os.O_RDWR | os.O_CREAT, 0o644)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        os.close(fd)
        return None
    return fd


def supervisor_alive(db):
    """A running scripts/desk_supervise.sh would restart the desk mid-run."""
    pid_path = os.path.join(os.path.dirname(os.path.abspath(db)), 'desk_supervisor.pid')
    try:
        pid = int(open(pid_path).read().strip())
        os.kill(pid, 0)
    except (OSError, ValueError):
        return False
    try:
        import subprocess
        cmd = subprocess.run(['/bin/ps', '-p', str(pid), '-o', 'command='], capture_output=True, text=True).stdout
    except OSError:
        return True
    return 'desk_supervise' in cmd


def fsync_path(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def wal_growing(db):
    """A second opinion for a writer that is not the desk: the WAL changing size under us."""
    wal = db + '-wal'
    if not os.path.exists(wal):
        return False
    size = os.path.getsize(wal)
    time.sleep(2.0)
    return os.path.exists(wal) and os.path.getsize(wal) != size


def boundary(db, table, tcol, t_units, lo, hi):
    """Smallest rowid cut such that rows at or above it have tcol >= t_units, rows below it less.

    f(m) = tcol of the first row with rowid >= m is non-decreasing in m for an append-only table,
    so this is a binary search over the rowid space, each probe one indexed seek."""
    while lo < hi:
        mid = (lo + hi) // 2
        r = db.execute(f'SELECT id, {tcol} FROM {table} WHERE id >= ? ORDER BY id LIMIT 1', (mid,)).fetchone()
        if r is None or (r[1] is not None and r[1] >= t_units):
            hi = mid
        else:
            lo = r[0] + 1
    return lo


def write_day(cursor, cols, path, level, t_index, lo_t, hi_t):
    """Stream one rowid range to a gzip file, then read it back and compare. An archive nobody has
    read is not a backup. Returns (rows, raw bytes, gz bytes, sha, rows outside the day)."""
    tmp = path + '.part'
    h = hashlib.sha256()
    rows = raw = outside = 0
    first = last = None
    with gzip.open(tmp, 'wt', compresslevel=level, newline='') as f:
        class Tee:
            def write(self, x):
                nonlocal raw
                b = x.encode()
                h.update(b)
                raw += len(b)
                return f.write(x)
        w = csv.writer(Tee())
        w.writerow(cols)
        for row in cursor:
            w.writerow(row)
            rows += 1
            if first is None:
                first = [row[0], row[t_index]]
            last = [row[0], row[t_index]]
            t = row[t_index]
            if t is not None and not (lo_t <= t < hi_t):
                outside += 1
    back = hashlib.sha256()
    with gzip.open(tmp, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            back.update(chunk)
    back_rows = -1
    with gzip.open(tmp, 'rt', newline='') as f:
        for _ in csv.reader(f):
            back_rows += 1
    if back.hexdigest() != h.hexdigest() or back_rows != rows:
        os.remove(tmp)
        raise RuntimeError(f'{path}: archive did not read back identical ({back_rows} of {rows} rows)')
    fsync_path(tmp)
    os.replace(tmp, path)
    fsync_path(os.path.dirname(os.path.abspath(path)))
    return rows, raw, os.path.getsize(path), h.hexdigest(), outside, first, last


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--db', default=os.path.join(ROOT, 'data', 'prod.sqlite'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'data', 'archive'))
    ap.add_argument('--table', default='quotes', choices=sorted(TABLES))
    ap.add_argument('--days', type=int, default=7, help='whole UTC days to keep in the live database')
    ap.add_argument('--apply', action='store_true', help='delete what has been archived and verified')
    ap.add_argument('--vacuum', action='store_true', help='rebuild the file so the disk gets the pages back')
    ap.add_argument('--level', type=int, default=6, help='gzip level (1 fastest .. 9 smallest)')
    a = ap.parse_args()

    cols, tcol, unit = TABLES[a.table]
    table = a.table
    t_index = cols.index(tcol)
    cutoff = day_start(day_of(int(time.time()) - a.days * 86400))
    print(f'{table}: keeping rows from {day_of(cutoff)} on ({a.days}d); archiving whole days before it')

    if supervisor_alive(a.db):
        sys.exit('the desk supervisor is running and would restart the desk during this run. Stop both first:\n'
                 '  scripts/tradeit down   (then re-run, then scripts/tradeit up)')
    lock_fd = take_desk_lock(a.db)   # held until this process exits
    if lock_fd is None:
        sys.exit('the desk is running on this database. Stop it first; this reads (and with --apply deletes)\n'
                 'every archived row:\n  scripts/tradeit down   (then re-run, then scripts/tradeit up)')
    db_real = os.path.realpath(a.db)
    if a.apply and wal_growing(a.db):
        sys.exit('something is writing to this database (its WAL is changing size). Stop it first.')

    os.makedirs(a.out, exist_ok=True)
    db = sqlite3.connect(a.db if a.apply else f'file:{a.db}?mode=ro', uri=not a.apply, timeout=60)
    db.execute('pragma busy_timeout=30000')
    lo_id, hi_id = db.execute(f'SELECT MIN(id), MAX(id) FROM {table}').fetchone()
    if lo_id is None:
        print('the table is empty'); return
    first_t = db.execute(f'SELECT {tcol} FROM {table} WHERE id=?', (lo_id,)).fetchone()[0]
    days = []
    if first_t is not None and first_t // unit < cutoff:
        d = day_start(day_of(first_t // unit))
        while d < cutoff:
            days.append(d)
            d += 86400
    # The rowid cut at each day boundary, and at the cutoff itself.
    cuts = {d: boundary(db, table, tcol, d * unit, lo_id, hi_id + 1) for d in days + [cutoff]}

    plan = []   # (day, manifest path, manifest)
    new_rows = new_gz = 0
    started = time.time()
    # Days archived by an earlier run are still in the plan, whatever the rows now say.
    for mpath in sorted(p for p in os.listdir(a.out) if p.startswith(table + '-') and p.endswith('.csv.gz.json')):
        m = json.load(open(os.path.join(a.out, mpath)))
        # Only days before today's cutoff: a manifest from a run with a smaller --days must not
        # delete a day this run was told to keep.
        if m.get('table') == table and day_start(m['day']) < cutoff and day_start(m['day']) not in cuts:
            state = 'deleted' if m.get('deleted') else 'archived'
            print(f'  {m["day"]}  {m["rows"]:>11,} rows  already {state} (rowids {m["lo_id"]}..{m["hi_id"] - 1})')
            plan.append((m['day'], os.path.join(a.out, mpath), m))
    def manifest_for(day):
        mp = os.path.join(a.out, f'{table}-{day}.csv.gz.json')
        return json.load(open(mp)) if os.path.exists(mp) else None

    for d in days:
        lo, hi = cuts[d], cuts[d + 86400]
        day = day_of(d)
        # Chain onto neighbouring days archived by earlier runs, so two runs never overlap (which
        # would block the delete) and never leave rows between them unarchived.
        prev, nxt = manifest_for(day_of(d - 86400)), manifest_for(day_of(d + 86400))
        if prev and prev.get('db') in (None, db_real):
            lo = max(lo, prev['hi_id'])
        if nxt and nxt.get('db') in (None, db_real):
            hi = min(hi, nxt['lo_id'])
        path = os.path.join(a.out, f'{table}-{day}.csv.gz')
        mpath = path + '.json'
        if os.path.exists(mpath):
            m = json.load(open(mpath))
            state = 'deleted' if m.get('deleted') else 'archived'
            print(f'  {day}  {m["rows"]:>11,} rows  already {state} (rowids {m["lo_id"]}..{m["hi_id"] - 1})')
            plan.append((day, mpath, m))
            continue
        if os.path.exists(path):
            sys.exit(f'{path} exists without its manifest (an older version of this script, or an interrupted '
                     'run). Move it aside and re-run. Nothing deleted.')
        if lo >= hi:
            continue
        t0 = time.time()
        cur = db.execute(f"SELECT {','.join(cols)} FROM {table} WHERE id >= ? AND id < ? ORDER BY id", (lo, hi))
        rows, raw, gz, sha, outside, first, last = write_day(cur, cols, path, a.level, t_index, d * unit,
                                                             (d + 86400) * unit)
        if rows == 0:
            os.remove(path)
            continue
        # The manifest names the database and the first and last archived rows, so a delete can only
        # ever run against the rows this file holds.
        m = {'table': table, 'day': day, 'db': db_real, 'lo_id': lo, 'hi_id': hi, 'rows': rows, 'sha256': sha,
             'first': first, 'last': last, 'rows_outside_day': outside,
             'created_utc': datetime.now(timezone.utc).isoformat(), 'deleted': False}
        with open(mpath + '.part', 'w') as f:
            json.dump(m, f, indent=1)
            f.flush()
            os.fsync(f.fileno())
        os.replace(mpath + '.part', mpath)
        fsync_path(a.out)
        print(f'  {day}  {rows:>11,} rows  {raw / 1e6:>9.1f} MB -> {gz / 1e6:>8.1f} MB  {time.time() - t0:6.0f}s'
              + (f'  ({outside} rows stamped just outside the day)' if outside else ''))
        plan.append((day, mpath, m))
        new_rows += rows
        new_gz += gz
    print(f'\n{new_rows:,} rows newly archived into {new_gz / 1e6:.0f} MB in {time.time() - started:.0f}s')

    pending = [(day, mp, m) for day, mp, m in plan if not m.get('deleted')]
    # Every manifest must come from this database and still describe the rows it would delete.
    # A range already empty was deleted by a run that died before it could mark its manifests: mark
    # it now instead of refusing forever.
    already = []
    for day, mp, m in pending:
        if m.get('db') != db_real:
            sys.exit(f'{mp} was written for {m.get("db") or "an unknown database"}, not {db_real}. Nothing deleted.')
        head = db.execute(f'SELECT id, {tcol} FROM {table} WHERE id >= ? AND id < ? ORDER BY id LIMIT 1',
                          (m['lo_id'], m['hi_id'])).fetchone()
        if head is None:
            already.append((day, mp, m))
            continue
        tail = db.execute(f'SELECT id, {tcol} FROM {table} WHERE id >= ? AND id < ? ORDER BY id DESC LIMIT 1',
                          (m['lo_id'], m['hi_id'])).fetchone()
        if list(head) != list(m.get('first') or []) or list(tail) != list(m.get('last') or []):
            sys.exit(f'{day}: the rows in the database no longer match {mp} (first {list(head)} vs {m.get("first")}, '
                     f'last {list(tail)} vs {m.get("last")}). Nothing deleted.')
    for day, mp, m in already:
        m['deleted'] = True
        m['deleted_utc'] = m.get('deleted_utc') or 'found already deleted ' + datetime.now(timezone.utc).isoformat()
        with open(mp + '.part', 'w') as f:
            json.dump(m, f, indent=1)
        os.replace(mp + '.part', mp)
        print(f'  {day}: already gone from the database; manifest marked deleted')
    pending = [x for x in pending if x not in already]
    if not a.apply:
        if pending:
            print(f'\nnothing deleted: {len(pending)} archived day(s) wait for --apply.')
        return
    if not pending:
        print('every archived day is already deleted from the database')
    else:
        to_delete = sum(m['rows'] for _, _, m in pending)
        total = db.execute(f'SELECT COUNT(*) FROM {table}').fetchone()[0]
        idx = db.execute("SELECT name, sql FROM sqlite_master WHERE type='index' AND tbl_name=? AND sql IS NOT NULL",
                         (table,)).fetchall()
        drop = bool(idx) and total > 0 and to_delete > 0.5 * total
        t0 = time.time()
        db.isolation_level = None
        db.execute('BEGIN IMMEDIATE')
        try:
            if drop:
                for name, _ in idx:
                    db.execute(f'DROP INDEX "{name}"')
            for day, mp, m in pending:
                n = db.execute(f'DELETE FROM {table} WHERE id >= ? AND id < ?', (m['lo_id'], m['hi_id'])).rowcount
                if n != m['rows']:
                    raise RuntimeError(f'{day}: the delete would remove {n:,} rows but {m["rows"]:,} were archived')
            if drop:
                for _, sql in idx:
                    db.execute(sql)
            db.execute('COMMIT')
        except Exception as e:
            db.execute('ROLLBACK')
            sys.exit(f'{e}\nrolled back: nothing was deleted.')
        for day, mp, m in pending:
            m['deleted'] = True
            m['deleted_utc'] = datetime.now(timezone.utc).isoformat()
            with open(mp + '.part', 'w') as f:
                json.dump(m, f, indent=1)
            os.replace(mp + '.part', mp)
        print(f'deleted {to_delete:,} archived rows in {time.time() - t0:.0f}s'
              + (' (indexes dropped for the delete and re-created)' if drop else ''))

    if a.vacuum:
        before = os.path.getsize(a.db)
        free = shutil.disk_usage(os.path.dirname(os.path.abspath(a.db))).free
        # VACUUM writes a complete copy of what is left before it replaces the file.
        pages = db.execute('PRAGMA page_count').fetchone()[0]
        free_pages = db.execute('PRAGMA freelist_count').fetchone()[0]
        page = db.execute('PRAGMA page_size').fetchone()[0]
        need = (pages - free_pages) * page
        # In WAL mode VACUUM writes the new database through the WAL before checkpointing it into the
        # file, so it needs about twice what is kept.
        if free < need * 2.2:
            sys.exit(f'not vacuuming: it needs about {need * 2.2 / 1e9:.1f} GB free ({need / 1e9:.1f} GB kept, written '
                     f'twice in WAL mode) and {free / 1e9:.1f} GB is. '
                     'The deleted pages are reused by new rows in the meantime.')
        print(f'vacuuming {before / 1e9:.2f} GB (about {need / 1e9:.1f} GB kept; this rewrites the file)...')
        t0 = time.time()
        db.execute('VACUUM')
        after = os.path.getsize(a.db)
        print(f'  {before / 1e9:.2f} GB -> {after / 1e9:.2f} GB, {(before - after) / 1e9:.2f} GB back, '
              f'{time.time() - t0:.0f}s')
    elif a.apply:
        print('the pages are free inside the file (new rows reuse them) but not returned to the disk; '
              're-run with --vacuum to shrink the file')


if __name__ == '__main__':
    main()
