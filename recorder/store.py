"""Rows to disk: an append-only spool per table and hour, compacted to zstd Parquet once the hour
is over, with a manifest row per file and a gaps table.

    <data>/spool/<table>/<bucket>.jsonl                       rows as written, flushed every second
    <data>/parquet/<table>/date=YYYY-MM-DD/<table>-<bucket>[.N].parquet
    <data>/manifest.jsonl                                      one line per Parquet file

The spool is the durable copy: a crash or a SIGTERM loses at most the last second (a torn last
line is counted as a gap, never fatal), and a restart inside the same hour appends to the same
spool. Compaction runs only for buckets that are over, so a file is written once; if a clock
step puts rows into a bucket already compacted, they become part .1, .2, ... rather than
overwrite it. A recorder must not die because pyarrow is missing (scripts/perps_recorder.py's
rule): without it the spool simply stays, and compaction runs once it is installed.
"""
import glob
import hashlib
import json
import os
import time
from datetime import datetime, timezone

SCORE_COLUMNS = [('recv_us', 'int64'), ('prev_us', 'int64'), ('league', 'string'), ('game_id', 'string'),
                 ('start_time', 'string'), ('state', 'string'), ('status', 'string'), ('period', 'string'),
                 ('clock', 'string'), ('home', 'string'), ('away', 'string'), ('home_score', 'int32'),
                 ('away_score', 'int32'), ('detail', 'string'), ('raw', 'string')]

SCHEMAS = {
    # every frame of the market socket, raw: no parse decision is baked in at write time
    'kalshi_ws': [('recv_us', 'int64'), ('mono_us', 'int64'), ('conn', 'int32'), ('type', 'string'),
                  ('sid', 'int64'), ('seq', 'int64'), ('ticker', 'string'), ('raw', 'string')],
    # a market's REST row each time a field the research reads changed (strikes, status, results)
    'kalshi_markets': [('recv_us', 'int64'), ('series', 'string'), ('ticker', 'string'), ('status', 'string'),
                       ('open_time', 'string'), ('close_time', 'string'), ('strike_type', 'string'),
                       ('floor_strike', 'float64'), ('cap_strike', 'float64'), ('result', 'string'),
                       ('expiration_value', 'string'), ('raw', 'string')],
    # every disconnect, sequence gap, refused subscription, failed discovery or poll, torn spool
    # line, clock step and restart; source names the feed, from_us/to_us the stretch when known
    'gaps': [('recv_us', 'int64'), ('source', 'string'), ('kind', 'string'), ('detail', 'string'),
             ('from_us', 'int64'), ('to_us', 'int64')],
    # a market's book by public REST (plan 5(d), and the fallback while the socket cannot
    # subscribe), written only when it differs from the last one recorded for it: recv_us is when
    # this book was first seen, sent_us when that poll left (the book held at some instant
    # between the two), prev_us the poll before it (the change came after that), polls how many
    # polls had seen the previous book. kind: top (GET /markets?tickers=, best prices and sizes)
    # or depth (GET /markets/{t}/orderbook, every level in raw). mode: college or fallback.
    'kalshi_rest_books': [('recv_us', 'int64'), ('sent_us', 'int64'), ('prev_us', 'int64'), ('polls', 'int32'),
                          ('kind', 'string'), ('mode', 'string'), ('ticker', 'string'),
                          ('yes_bid', 'float64'), ('yes_bid_size', 'float64'), ('yes_ask', 'float64'),
                          ('yes_ask_size', 'float64'), ('last_price', 'float64'), ('volume', 'float64'),
                          ('raw', 'string')],
    # public prints by REST over a window the socket may have missed (a disconnect, a sequence
    # gap on trades) and every hour; each trade_id once. reason: why its window was read
    'kalshi_trades_rest': [('recv_us', 'int64'), ('trade_id', 'string'), ('ticker', 'string'),
                           ('created_time', 'string'), ('created_us', 'int64'), ('yes_price', 'float64'),
                           ('no_price', 'float64'), ('count', 'float64'), ('taker_side', 'string'),
                           ('reason', 'string'), ('raw', 'string')],
    # GET /incentive_programs every hour, all pages, the types kept chosen here (the venue ignores
    # its own filter); sweep_us groups one sweep's rows
    'kalshi_incentives': [('recv_us', 'int64'), ('sweep_us', 'int64'), ('market_ticker', 'string'),
                          ('incentive_type', 'string'), ('period_reward', 'float64'), ('target_size', 'float64'),
                          ('discount_factor_bps', 'float64'), ('start_date', 'string'), ('end_date', 'string'),
                          ('paid_out', 'bool_'), ('raw', 'string')],
    # every open market once a day (GET /markets, all pages); raw without the rules text
    'kalshi_markets_daily': [('recv_us', 'int64'), ('sweep_us', 'int64'), ('ticker', 'string'),
                             ('event_ticker', 'string'), ('status', 'string'), ('open_time', 'string'),
                             ('close_time', 'string'), ('volume', 'float64'), ('open_interest', 'float64'),
                             ('yes_bid', 'float64'), ('yes_ask', 'float64'), ('raw', 'string')],
    # a scoreboard's game each time its state or score changed (recorder/scores.py): recv_us the
    # poll that first showed it, prev_us the poll before (the change came after that)
    'espn_scores': SCORE_COLUMNS,
    'ncaa_scores': SCORE_COLUMNS,
    # the control decile (plan 5(d)): every game of the control series on its date, ranked by
    # pregame volume when the pick was made, the picked ones flagged
    'college_control': [('recv_us', 'int64'), ('series', 'string'), ('game_date', 'string'),
                        ('event_ticker', 'string'), ('volume', 'float64'), ('rank', 'int32'), ('events', 'int32'),
                        ('picked', 'bool_')],
}

GRACE_US = 5_000_000      # a bucket is compacted this long after it ends
FSYNC_EVERY_S = 5


def now_us():
    return time.time_ns() // 1000


def mono_us():
    return time.monotonic_ns() // 1000


class Store:
    def __init__(self, root, bucket_s=3600, log=print):
        self.root = root
        self.bucket_us = bucket_s * 1_000_000
        self.log = log
        self._files = {}           # (table, bucket_start_us) -> file
        self._last_fsync = 0.0
        self._warned_pyarrow = False
        self.rows_written = 0
        os.makedirs(os.path.join(root, 'spool'), exist_ok=True)
        os.makedirs(os.path.join(root, 'parquet'), exist_ok=True)

    # ---- names -------------------------------------------------------------------------------
    def bucket_of(self, t_us):
        return t_us - t_us % self.bucket_us

    def bucket_name(self, start_us):
        dt = datetime.fromtimestamp(start_us / 1e6, tz=timezone.utc)
        return dt.strftime('%Y%m%dT%H' if self.bucket_us == 3_600_000_000 else '%Y%m%dT%H%M%S')

    def _bucket_start(self, name):
        fmt = '%Y%m%dT%H' if len(name) == 11 else '%Y%m%dT%H%M%S'
        return int(datetime.strptime(name, fmt).replace(tzinfo=timezone.utc).timestamp()) * 1_000_000

    def spool_path(self, table, start_us):
        return os.path.join(self.root, 'spool', table, self.bucket_name(start_us) + '.jsonl')

    # ---- writing -----------------------------------------------------------------------------
    def write(self, table, row):
        if table not in SCHEMAS:
            raise KeyError('unknown table %s' % table)
        start = self.bucket_of(row['recv_us'])
        f = self._files.get((table, start))
        if f is None:
            path = self.spool_path(table, start)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            f = self._files[(table, start)] = open(path, 'a', encoding='utf-8')
        f.write(json.dumps(row, separators=(',', ':')) + '\n')
        self.rows_written += 1

    def flush(self, fsync=None):
        due = fsync if fsync is not None else time.monotonic() - self._last_fsync >= FSYNC_EVERY_S
        for f in self._files.values():
            f.flush()
            if due:
                os.fsync(f.fileno())
        if due:
            self._last_fsync = time.monotonic()

    def close(self):
        self.flush(fsync=True)
        for f in self._files.values():
            f.close()
        self._files.clear()

    # ---- compaction --------------------------------------------------------------------------
    def finished(self, t_us=None):
        """Close the spools of buckets that are over; every finished spool on disk (this run's
        and any a crash left) as (table, bucket start, path), oldest first."""
        t_us = now_us() if t_us is None else t_us
        for key in [k for k in self._files if k[1] + self.bucket_us + GRACE_US <= t_us]:
            f = self._files.pop(key)
            f.flush()
            os.fsync(f.fileno())
            f.close()
        out = []
        for path in sorted(glob.glob(os.path.join(self.root, 'spool', '*', '*.jsonl'))):
            table = os.path.basename(os.path.dirname(path))
            try:
                start = self._bucket_start(os.path.basename(path)[:-len('.jsonl')])
            except ValueError:
                continue
            if table in SCHEMAS and (table, start) not in self._files and start + self.bucket_us + GRACE_US <= t_us:
                out.append((table, start, path))
        return out

    def roll(self, t_us=None):
        """finished() and compact() in one go, the torn lines' gap rows written; the Parquet
        paths written. The recorder runs the two halves apart: compaction in a thread."""
        written = []
        for table, start, path in self.finished(t_us):
            r = self.compact(table, start, path)
            if r:
                self.note_compacted(r)
                if r['path']:
                    written.append(r['path'])
        return written

    def note_compacted(self, r):
        if r['torn']:
            self.write('gaps', {'recv_us': now_us(), 'source': 'store', 'kind': 'spool_torn',
                                'detail': '%d unreadable line(s) in %s' % (r['torn'], r['spool']),
                                'from_us': r['from_us'], 'to_us': r['to_us']})

    def compact(self, table, start_us, spool):
        """One finished spool to Parquet, its manifest row, the spool removed. Never writes a row
        itself, so it may run in another thread while the owner keeps writing."""
        try:
            import pyarrow as pa
            import pyarrow.parquet as pq
        except ImportError:
            if not self._warned_pyarrow:
                self.log('store: pyarrow is not installed: spools are kept until it is')
                self._warned_pyarrow = True
            return None
        rows, torn = [], 0
        with open(spool, 'r', encoding='utf-8', errors='replace') as f:
            for line in f:
                try:
                    rows.append(json.loads(line))
                except ValueError:
                    torn += 1
        schema = pa.schema([(n, getattr(pa, t)()) for n, t in SCHEMAS[table]])
        date = datetime.fromtimestamp(start_us / 1e6, tz=timezone.utc).strftime('%Y-%m-%d')
        d = os.path.join(self.root, 'parquet', table, 'date=' + date)
        os.makedirs(d, exist_ok=True)
        base = '%s-%s' % (table, self.bucket_name(start_us))
        final, part = os.path.join(d, base + '.parquet'), 0
        while os.path.exists(final):   # a clock step wrote into a bucket already compacted
            part += 1
            final = os.path.join(d, '%s.%d.parquet' % (base, part))
        if rows:
            tmp = final + '.tmp'
            pq.write_table(pa.Table.from_pylist(rows, schema=schema), tmp, compression='zstd')
            with open(tmp, 'rb') as f:
                os.fsync(f.fileno())
            os.replace(tmp, final)
            with open(final, 'rb') as f:
                body = f.read()
            ts = [r['recv_us'] for r in rows]
            entry = {'table': table, 'bucket': self.bucket_name(start_us), 'path': os.path.relpath(final, self.root),
                     'rows': len(rows), 't_min_us': min(ts), 't_max_us': max(ts),
                     'sha256': hashlib.sha256(body).hexdigest(), 'bytes': len(body), 'torn': torn,
                     'written_us': now_us()}
            with open(os.path.join(self.root, 'manifest.jsonl'), 'a', encoding='utf-8') as m:
                m.write(json.dumps(entry, separators=(',', ':')) + '\n')
                m.flush()
                os.fsync(m.fileno())
        os.unlink(spool)
        self.log('store: %s %s: %d rows%s -> %s' % (table, self.bucket_name(start_us), len(rows),
                                                    ', %d torn' % torn if torn else '',
                                                    os.path.relpath(final, self.root) if rows else 'nothing'))
        return {'path': final if rows else None, 'rows': len(rows), 'torn': torn, 'spool': os.path.basename(spool),
                'from_us': start_us, 'to_us': start_us + self.bucket_us}

    # ---- reading back ------------------------------------------------------------------------
    def last_recv_us(self, table=None):
        """The newest row on disk, of one table or (None) of any: the last complete line of each
        spool (only its tail is read; a spool holds an hour of frames) and the manifest's newest
        file. None when there is none (a new data directory). A restart reads this before
        writing anything: how long the recorder was down, and since when the socket was."""
        last = None
        for path in glob.glob(os.path.join(self.root, 'spool', table or '*', '*.jsonl')):
            t = _last_recv(path)
            if t is not None and (last is None or t > last):
                last = t
        for e in read_manifest(self.root):
            if table is not None and e.get('table') != table:
                continue
            t = e.get('t_max_us')
            if isinstance(t, int) and (last is None or t > last):
                last = t
        return last

    def read_recent(self, table, since_us):
        """A table's rows received since since_us: its spools, and its Parquet files when pyarrow
        is there (without it the Parquet part is skipped, as compaction is)."""
        for path in sorted(glob.glob(os.path.join(self.root, 'spool', table, '*.jsonl'))):
            try:
                start = self._bucket_start(os.path.basename(path)[:-len('.jsonl')])
            except ValueError:
                continue
            if start + self.bucket_us < since_us:
                continue
            with open(path, 'r', encoding='utf-8', errors='replace') as f:
                for line in f:
                    try:
                        r = json.loads(line)
                    except ValueError:
                        continue
                    if isinstance(r, dict) and r.get('recv_us', 0) >= since_us:
                        yield r
        try:
            import pyarrow.parquet as pq
        except ImportError:
            return
        for e in read_manifest(self.root):
            if e.get('table') != table or e.get('t_max_us', 0) < since_us:
                continue
            path = os.path.join(self.root, e['path'])
            if not os.path.exists(path):
                continue
            for r in pq.read_table(path).to_pylist():
                if r.get('recv_us', 0) >= since_us:
                    yield r


def _last_recv(path, tail=65536):
    try:
        with open(path, 'rb') as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - tail))
            lines = f.read().split(b'\n')
    except OSError:
        return None
    for line in reversed(lines):
        try:
            r = json.loads(line)
        except ValueError:
            continue   # the torn last line of a crash, or the cut first line of the tail
        if isinstance(r, dict) and isinstance(r.get('recv_us'), int):
            return r['recv_us']
    return None


class State:
    """A few facts that must survive a restart (the day's control picks, the day the daily sweep
    ran, the trade backfill windows not yet read), in <data>/state/recorder.json, replaced
    atomically. Losing it costs a repeat sweep, a re-pick or a backfill window, never data."""

    def __init__(self, root, log=print):
        self.path = os.path.join(root, 'state', 'recorder.json')
        self.log = log
        self.data = {}
        try:
            with open(self.path, encoding='utf-8') as f:
                d = json.load(f)
            self.data = d if isinstance(d, dict) else {}
        except FileNotFoundError:
            pass
        except (OSError, ValueError) as e:
            log('state: %s unreadable, starting empty: %s' % (self.path, e))

    def get(self, key, default=None):
        return self.data.get(key, default)

    def set(self, key, value):
        self.data[key] = value
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        tmp = self.path + '.tmp'
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump(self.data, f, separators=(',', ':'), sort_keys=True)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, self.path)


def read_manifest(root):
    p = os.path.join(root, 'manifest.jsonl')
    if not os.path.exists(p):
        return []
    out = []
    with open(p, encoding='utf-8') as f:
        for line in f:
            try:
                out.append(json.loads(line))
            except ValueError:
                pass
    return out
