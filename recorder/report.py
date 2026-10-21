"""The recorder's gaps report: minutes with gaps per source per day and their share, and rows and
bytes per table, over the local Parquet and spool. The milestone check (plan section 9): "24 h
with every gap logged, gaps under 1% of minutes".

    python -m recorder.report                       # /var/lib/deskd-recorder, every day on disk
    python -m recorder.report --data /tmp/rec --days 2
    python -m recorder.report --json

A minute has a gap when a gaps row covers it (from_us..to_us, or its own recv_us when it has no
span), or when the socket recorded nothing in it (silent_ws: pyth_value alone sends several frames
a second, so a silent minute is an outage even if nothing logged one, e.g. a dead VM). Each day's
share is over the minutes the recorder covered that day (its first to last row, plus any gap
minutes). fallback_on/off rows say what the REST books did; they are not gaps.

Parquet is read with pyarrow, else duckdb; without either the Parquet files are counted from the
manifest only (rows and bytes) and their gaps and minutes skipped, which the report says.
"""
import argparse
import glob
import json
import os
import sys
from datetime import datetime, timezone

from .store import read_manifest

MIN_US = 60_000_000
DAY_MIN = 1440
INFO_KINDS = ('fallback_on', 'fallback_off')
MAX_SPAN_MIN = 7 * DAY_MIN    # a gap row claiming more than a week is cut to its last week
GOAL = 0.01


def day_of(minute):
    return datetime.fromtimestamp(minute * 60, tz=timezone.utc).strftime('%Y-%m-%d')


def gap_minutes(g):
    """The minutes a gaps row covers: from_us..to_us; a missing end is the row's own recv_us."""
    recv, a, b = g.get('recv_us'), g.get('from_us'), g.get('to_us')
    a = a if a is not None else (recv if recv is not None else b)
    b = b if b is not None else (recv if recv is not None else a)
    if a is None or b is None:
        return range(0)
    if a > b:
        a, b = b, a
    lo, hi = a // MIN_US, b // MIN_US
    return range(max(lo, hi - MAX_SPAN_MIN), hi + 1)


# ---- reading ---------------------------------------------------------------------------------------

def parquet_engine():
    try:
        import pyarrow.parquet  # noqa: F401
        return 'pyarrow'
    except ImportError:
        pass
    try:
        import duckdb  # noqa: F401
        return 'duckdb'
    except ImportError:
        return None


def read_parquet(path, columns, engine):
    if engine == 'pyarrow':
        import pyarrow.parquet as pq
        return pq.read_table(path, columns=columns).to_pylist()
    import duckdb
    con = duckdb.connect()
    try:
        cur = con.execute('SELECT %s FROM read_parquet(?)' % ', '.join('"%s"' % c for c in columns), [path])
        return [dict(zip(columns, r)) for r in cur.fetchall()]
    finally:
        con.close()


def parquet_minutes(path, engine):
    """The distinct minutes of a file's recv_us, without loading an hour of frames as rows."""
    if engine == 'pyarrow':
        import pyarrow.compute as pc
        import pyarrow.parquet as pq
        col = pq.read_table(path, columns=['recv_us']).column('recv_us')
        return set(pc.unique(pc.divide(col, MIN_US)).to_pylist()) - {None}
    import duckdb
    con = duckdb.connect()
    try:
        return {r[0] for r in con.execute('SELECT DISTINCT recv_us // %d FROM read_parquet(?)' % MIN_US,
                                          [path]).fetchall() if r[0] is not None}
    finally:
        con.close()


def spool_rows(path):
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            try:
                r = json.loads(line)
            except ValueError:
                continue   # a torn last line
            if isinstance(r, dict):
                yield r


def spool_span(path):
    """(rows, first recv_us, last recv_us) of a spool."""
    n, lo, hi = 0, None, None
    for r in spool_rows(path):
        t = r.get('recv_us')
        n += 1
        if isinstance(t, int):
            lo = t if lo is None else min(lo, t)
            hi = t if hi is None else max(hi, t)
    return n, lo, hi


class Data:
    """What is on disk under a data directory: the manifest's Parquet files and the spools."""

    def __init__(self, root, engine):
        self.root = root
        self.engine = engine
        self.files = []   # (table, kind, path, rows, bytes, t_min_us, t_max_us)
        for e in read_manifest(root):
            path = os.path.join(root, e['path'])
            if os.path.exists(path):
                self.files.append((e['table'], 'parquet', path, e.get('rows', 0), e.get('bytes', 0),
                                   e.get('t_min_us'), e.get('t_max_us')))
        for path in sorted(glob.glob(os.path.join(root, 'spool', '*', '*.jsonl'))):
            n, lo, hi = spool_span(path)
            self.files.append((os.path.basename(os.path.dirname(path)), 'spool', path, n, os.path.getsize(path),
                               lo, hi))

    def readable(self, kind):
        return kind == 'spool' or self.engine is not None

    def skipped(self):
        return sum(1 for f in self.files if not self.readable(f[1]))

    def gaps(self):
        for table, kind, path, *_ in self.files:
            if table != 'gaps' or not self.readable(kind):
                continue
            rows = spool_rows(path) if kind == 'spool' else read_parquet(
                path, ['recv_us', 'source', 'kind', 'from_us', 'to_us'], self.engine)
            for g in rows:
                yield g

    def ws_minutes(self):
        out = set()
        for table, kind, path, *_ in self.files:
            if table != 'kalshi_ws' or not self.readable(kind):
                continue
            if kind == 'spool':
                out |= {r['recv_us'] // MIN_US for r in spool_rows(path) if isinstance(r.get('recv_us'), int)}
            else:
                out |= parquet_minutes(path, self.engine)
        return out


# ---- the report ------------------------------------------------------------------------------------

def build(root, days=None, engine='auto'):
    engine = parquet_engine() if engine == 'auto' else engine
    d = Data(root, engine)
    tables, span = {}, {}   # span: day -> set of minutes from first to last row
    for table, kind, path, rows, size, lo, hi in d.files:
        day = day_of(lo // MIN_US) if lo is not None else 'unknown'
        t = tables.setdefault(table, {}).setdefault(day, {'files': 0, 'rows': 0, 'bytes': 0})
        t['files'] += 1
        t['rows'] += rows or 0
        t['bytes'] += size or 0
        if lo is not None and hi is not None and d.readable(kind):
            # each day's first to last row: the minutes the recorder covered (a dead hour between
            # two files is inside the span, and so counted as silent). Only files whose minutes
            # are read: an unread one would look silent.
            _widen(span, lo // MIN_US, hi // MIN_US)
    by_source = {}   # source -> set of gap minutes
    info = {}
    for g in d.gaps():
        if g.get('kind') in INFO_KINDS:
            info[g.get('source')] = info.get(g.get('source'), 0) + 1
            continue
        by_source.setdefault(g.get('source') or '?', set()).update(gap_minutes(g))
    ws = d.ws_minutes()
    observed = set()
    for day, (lo, hi) in span.items():
        if lo is not None:
            observed.update(range(lo, hi + 1))
    for mins in by_source.values():
        observed |= mins
    silent = {m for m in observed if m not in ws} if any(f[0] == 'kalshi_ws' and d.readable(f[1]) for f in d.files) \
        else set()
    any_gap = set(silent)
    for mins in by_source.values():
        any_gap |= mins
    per_day = {}
    for m in observed:
        per_day.setdefault(day_of(m), set()).add(m)
    all_days = sorted(per_day)
    keep = all_days[-days:] if days else all_days
    out_days = {}
    for day in keep:
        mins = per_day[day]
        row = {'minutes': len(mins), 'sources': {}, 'silent_ws': len(silent & mins), 'any': len(any_gap & mins)}
        for src, gm in sorted(by_source.items()):
            n = len(gm & mins)
            if n:
                row['sources'][src] = n
        row['pct'] = row['any'] / row['minutes'] if row['minutes'] else 0.0
        row['full_day'] = row['minutes'] >= DAY_MIN
        out_days[day] = row
    last24 = None
    if observed:
        end = max(observed)
        window = set(range(end - DAY_MIN + 1, end + 1))
        covered = window & observed
        n = len(any_gap & window)
        last24 = {'end': datetime.fromtimestamp((end + 1) * 60, tz=timezone.utc).strftime('%Y-%m-%dT%H:%MZ'),
                  'minutes': len(covered), 'gap_minutes': n, 'pct': n / DAY_MIN,
                  'complete': len(covered) == DAY_MIN,
                  'pass': len(covered) == DAY_MIN and n / DAY_MIN < GOAL}
    return {'root': root, 'engine': engine, 'skipped_files': d.skipped(), 'info': info,
            'tables': {t: {day: v for day, v in sorted(byday.items()) if not days or day in keep}
                       for t, byday in sorted(tables.items())},
            'days': out_days, 'last_24h': last24}


def _widen(span, first, last):
    """Spread [first, last] minutes over the days it touches, each day's [lo, hi] widened."""
    m = first
    while m <= last:
        day_end = (m // DAY_MIN + 1) * DAY_MIN - 1
        hi = min(last, day_end)
        s = span.setdefault(day_of(m), [None, None])
        s[0] = m if s[0] is None else min(s[0], m)
        s[1] = hi if s[1] is None else max(s[1], hi)
        m = hi + 1


def human(n):
    for unit in ('B', 'kB', 'MB', 'GB'):
        if n < 1024 or unit == 'GB':
            return ('%d %s' % (n, unit)) if unit == 'B' else '%.1f %s' % (n, unit)
        n /= 1024.0
    return str(n)


def render(rep):
    lines = ['recorder data in %s (Parquet read with %s)' % (rep['root'], rep['engine'] or 'nothing: install pyarrow '
                                                                                        'or duckdb')]
    if rep['skipped_files']:
        lines.append('  %d Parquet file(s) counted from the manifest only: their gaps and minutes are not in the '
                     'figures below' % rep['skipped_files'])
    lines += ['', 'rows and bytes per table and day', '  %-10s  %-22s %6s %12s %10s' % ('date', 'table', 'files',
                                                                                        'rows', 'bytes')]
    for table, byday in rep['tables'].items():
        for day, v in byday.items():
            lines.append('  %-10s  %-22s %6d %12s %10s' % (day, table, v['files'], '{:,}'.format(v['rows']),
                                                          human(v['bytes'])))
    sources = sorted({s for v in rep['days'].values() for s in v['sources']})
    lines += ['', 'minutes with a gap per source and day (of the minutes recorded that day)',
              '  %-10s %8s ' % ('date', 'minutes') + ''.join('%11s ' % s[:11] for s in sources + ['silent_ws']) +
              '%6s %7s' % ('any', 'share')]
    for day, v in rep['days'].items():
        lines.append('  %-10s %8d ' % (day, v['minutes']) +
                     ''.join('%11d ' % v['sources'].get(s, 0) for s in sources) + '%11d ' % v['silent_ws'] +
                     '%6d %6.2f%%%s' % (v['any'], 100 * v['pct'], '' if v['full_day'] else '  (part of a day)'))
    if rep['info']:
        lines.append('  not gaps: ' + ', '.join('%s %d' % kv for kv in sorted(rep['info'].items())))
    l24 = rep['last_24h']
    lines.append('')
    if l24 is None:
        lines.append('last 24 h: no data')
    elif not l24['complete']:
        lines.append('last 24 h to %s: only %d of 1440 minutes recorded so far (%d with a gap): not yet a full day'
                     % (l24['end'], l24['minutes'], l24['gap_minutes']))
    else:
        lines.append('last 24 h to %s: %d of 1440 minutes with a gap (%.2f%%): %s (the milestone: under 1%%)' % (
            l24['end'], l24['gap_minutes'], 100 * l24['pct'], 'PASS' if l24['pass'] else 'FAIL'))
    return '\n'.join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--data', default=os.environ.get('REC_DATA_DIR', '/var/lib/deskd-recorder'))
    ap.add_argument('--days', type=int, default=0, help='only the last N days')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    if not os.path.isdir(a.data):
        print('report: no data directory %s' % a.data, file=sys.stderr)
        return 2
    rep = build(a.data, a.days or None)
    print(json.dumps(rep, indent=1, sort_keys=True) if a.json else render(rep))
    return 0


if __name__ == '__main__':
    sys.exit(main())
