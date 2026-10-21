#!/usr/bin/env python3
"""Do a series' recorded books hold LIP's target size? The join rule of plan section 5a, from the
recorder's data: "a series joins only when recorded books show 300 contracts a side in most
snapshots, since LIP pays nothing for a snapshot where the whole book lacks its target size."

Each market's book is rebuilt from the recorder's kalshi_ws frames as deskd reads them
(md/kalshi_md.cpp): orderbook_snapshot's msg.yes_dollars_fp and msg.no_dollars_fp ([[price, count],
...], the NO bids being YES asks at $1 - price) and orderbook_delta's msg.side, msg.price_dollars
and msg.delta_fp, prices dollars and counts fixed point, as strings (gw/kalshi_wire.cpp's
parse_px and parse_count: a price in whole 1/10,000 $, a count to 1/100).
  - sequence: Kalshi numbers each subscription's messages 1, 2, ... across all its markets, its
    ok replies too; a subscription is a (conn, sid). A number past the last + 1 is a gap: every
    book on that subscription is dropped until its market's next snapshot (the frame that shows
    the gap is applied only if it is a snapshot), and counted. A number at or below the last
    starts a new subscription (a restarted recorder numbers its connections from 1 again): its
    books are dropped, not counted. A gap on a subscription that carries no books (trades)
    drops nothing.
  - a frame on a new connection drops the old connection's books; so does each disconnect and
    restart the recorder wrote to its gaps table, from the socket's last frame before it.
  - a malformed or impossible frame (a delta below zero, a crossed book) drops that market's
    book until its next snapshot, as deskd does, and is counted as bad.

The sample (the rule chosen): once a minute, at 30 s past each whole UTC minute, the book as of
then (every frame received at or before it). Mid-minute keeps the samples off the windows' edges:
a 15-minute window keeps its 15, none at its open, when the book is often still empty (the
recorder subscribes before the open, and the first quotes come a moment after it), so the open
weighs no more than any other moment. A market is sampled inside its own recorded LIP program's
window (kalshi_incentives start_date to end_date, liquidity programs only), else from its
open_time to its close_time (kalshi_markets); a market with neither is not sampled. A sample
instant in that window with no live book (before the first snapshot, after a gap or a disconnect
until the next one) is not a sample: it is counted as missing. Instants outside the recorded data
are neither.

A side holds the target (the rule chosen, deskd's own LIP rule, strat/lip.h): its resting size,
summed from its best price outward over every level the book holds, is at least the target. The
program's discount weights each contract's share of the reward by its distance from the best
price; it does not decide whether the side holds the target, so every level counts. --touch
counts the best price's size alone instead. The YES bids are one side, the YES asks (NO bids) the
other.

The target: --target N when given; else the market's own recorded program's target_size; else
the latest target_size recorded for any market of its series (the hourly sweep can miss a
15-minute window's own program); else 300.

Per series: markets sampled, samples, the share of samples with the target on the YES bid, on the
YES ask and on both, missing minutes, the books dropped by sequence gaps, the targets used, and
the verdict: JOIN when both sides hold the target at once in more than half the samples (LIP pays
for a snapshot only then), else NOT YET.

Input: the recorder's data directory (<data>/spool/<table>/*.jsonl, and <data>/parquet/<table>/**
when pyarrow is installed): kalshi_ws (book frames), kalshi_markets (series, open and close
times), kalshi_incentives (programs), gaps (disconnects and restarts). Read-only.

Usage:
  python3 tools/lip_join.py --data DIR [--series S[,S...]] [--target N] [--touch] [--json]
  python3 tools/lip_join.py --self-test
"""
import argparse
import glob
import heapq
import json
import os
import re
import sys
import tempfile
from collections import Counter
from datetime import datetime, timezone

DEFAULT_TARGET = 300
UNIT = 10000      # prices in 1/10,000 $ (deskd's Px): $1
CENT = 100        # one cent in those units
CT = 100          # counts in 1/100 contract (deskd's Qty): one contract
MINUTE_US = 60_000_000
SAMPLE_AT_US = 30_000_000   # past each whole minute: off the windows' edges


# ---- the venue's numbers, as deskd parses them ------------------------------------------------
_FIXED = re.compile(r'^(-?)(\d{1,12})(?:\.(\d+))?$')


def fixed(s, max_dp, scale_dp, negative=False):
    """gw/kalshi_wire.cpp parse_fixed: a decimal string as an integer at scale_dp places."""
    m = _FIXED.match(s) if isinstance(s, str) else None
    if not m or (m.group(1) and not negative):
        return None
    frac = m.group(3) or ''
    if len(frac) > max_dp:
        return None
    v = int(m.group(2)) * 10 ** scale_dp + int((frac + '0' * scale_dp)[:scale_dp] or 0)
    return -v if m.group(1) else v


def px_of(s):
    """A dollars string ("0.4200") in 1/10,000 $; None unless a whole 1/10,000 $ in [0, $1]."""
    m = fixed(s, 6, 6)
    return None if m is None or m % 100 or m > 1_000_000 else m // 100


def qty_of(s, negative=False):
    """A fixed-point count ("300.00") in 1/100 contract."""
    return fixed(s, 2, 2, negative)


def us_of(text):
    if not isinstance(text, str) or not text:
        return None
    try:
        return int(datetime.fromisoformat(text.replace('Z', '+00:00')).timestamp() * 1_000_000)
    except ValueError:
        return None


def iso(t_us):
    return datetime.fromtimestamp(t_us / 1e6, tz=timezone.utc).strftime('%Y-%m-%d %H:%M:%S')


# ---- reading the recorder's tables ----------------------------------------------------------
def rows(data, table):
    """Every row of one table, oldest file first: the spool's JSON lines and the Parquet files
    (with pyarrow) in the order of their buckets, each file in the order it was written. The
    replay needs the frames in the order received."""
    files = [(os.path.basename(p)[:-len('.jsonl')], 1, p)
             for p in glob.glob(os.path.join(data, 'spool', table, '*.jsonl'))]
    parquet = glob.glob(os.path.join(data, 'parquet', table, '**', '*.parquet'), recursive=True)
    pq = None
    if parquet:
        try:
            import pyarrow.parquet as pq
        except ImportError:
            print('warning: %d Parquet file(s) of %s skipped: pyarrow is not installed' % (len(parquet), table),
                  file=sys.stderr)
            parquet = []
    for p in parquet:   # <table>-<bucket>[.N].parquet
        files.append((os.path.basename(p)[len(table) + 1:-len('.parquet')], 0, p))
    for _, spool, path in sorted(files):
        if spool:
            with open(path, encoding='utf-8') as fh:
                for line in fh:
                    try:
                        r = json.loads(line)
                    except ValueError:
                        continue   # a torn last line (the recorder counts it as a gap)
                    if isinstance(r, dict):
                        yield r
        else:
            for r in pq.read_table(path).to_pylist():
                yield r


def load_markets(data):
    """ticker -> {series, open_us, close_us, raw}: the last row recorded for it."""
    out = {}
    for r in rows(data, 'kalshi_markets'):
        tk = r.get('ticker')
        if not tk:
            continue
        prev = out.get(tk)
        t = r.get('recv_us') or 0
        if prev is None or t >= prev['recv_us']:
            out[tk] = {'recv_us': t, 'series': r.get('series') or tk.split('-')[0],
                       'open_us': us_of(r.get('open_time')), 'close_us': us_of(r.get('close_time')),
                       'raw': r.get('raw')}
    return out


def load_programs(data):
    """The liquidity programs recorded, each (market, start, end) once, its last sweep's terms:
    [{ticker, start_us, end_us, target (1/100 contract or None), discount_bps, reward (1/10,000 $),
    recv_us}]."""
    out = {}
    for r in rows(data, 'kalshi_incentives'):
        if r.get('incentive_type') != 'liquidity':
            continue
        tk, a, b = r.get('market_ticker'), us_of(r.get('start_date')), us_of(r.get('end_date'))
        if not tk or a is None or b is None or b <= a:
            continue
        t = r.get('recv_us') or 0
        key = (tk, a, b)
        if key in out and out[key]['recv_us'] > t:
            continue
        target = r.get('target_size')
        target = int(round(float(target) * CT)) if isinstance(target, (int, float)) and target > 0 else None
        disc = r.get('discount_factor_bps')
        reward = r.get('period_reward')
        out[key] = {'ticker': tk, 'start_us': a, 'end_us': b, 'target': target, 'recv_us': t,
                    'discount_bps': min(max(float(disc), 0.0), 10000.0) if isinstance(disc, (int, float)) else 0.0,
                    'reward': float(reward) if isinstance(reward, (int, float)) and reward > 0 else 0.0}
    return list(out.values())


def series_of(ticker, markets):
    m = markets.get(ticker)
    return m['series'] if m else ticker.split('-')[0]


class Terms:
    """The programs by market, and each series' latest recorded program (the fallback for a
    market whose own program the hourly sweep missed)."""

    def __init__(self, programs, markets):
        self.own, self.latest = {}, {}
        for p in sorted(programs, key=lambda p: (p['recv_us'], p['start_us'])):
            self.own.setdefault(p['ticker'], []).append(p)
            self.latest[series_of(p['ticker'], markets)] = p

    def target(self, ticker, series, own=None):
        """(target in 1/100 contract, where it came from)."""
        if own is not None and own['target']:
            return own['target'], 'program'
        p = self.latest.get(series)
        if p is not None and p['target']:
            return p['target'], 'series'
        return DEFAULT_TARGET * CT, 'default'


def down_instants(data):
    """When the socket's books stopped being known: the last frame before each disconnect and
    each restart the recorder wrote to its gaps table."""
    out = set()
    for r in rows(data, 'gaps'):
        if (r.get('source'), r.get('kind')) in (('kalshi_ws', 'disconnect'), ('recorder', 'restart')):
            t = r.get('from_us')
            if isinstance(t, int) and not isinstance(t, bool):
                out.add(t)
    return sorted(out)


# ---- the books ------------------------------------------------------------------------------
class Bad(Exception):
    pass


def snapshot_side(msg, key, no):
    """md/kalshi_md.cpp levels(): an omitted side is empty; anything malformed is Bad."""
    v = msg.get(key)
    if v is None:
        return {}
    if not isinstance(v, list):
        raise Bad('%s is not a list' % key)
    out = {}
    for lv in v:
        if not (isinstance(lv, list) and len(lv) == 2 and isinstance(lv[0], str) and isinstance(lv[1], str)):
            raise Bad('a %s level is not [price, count]' % key)
        p, q = px_of(lv[0]), qty_of(lv[1])
        if p is None or q is None or q <= 0:
            raise Bad('a bad %s price or count' % key)
        p = UNIT - p if no else p
        if p in out:
            raise Bad('a repeated %s price' % key)
        out[p] = q
    return out


class Book:
    __slots__ = ('bids', 'asks', 'stream')

    def __init__(self, bids, asks, stream):
        self.bids, self.asks, self.stream = bids, asks, stream   # YES price -> count

    def best_bid(self):
        return max(self.bids) if self.bids else None

    def best_ask(self):
        return min(self.asks) if self.asks else None

    def crossed(self):
        return bool(self.bids and self.asks) and max(self.bids) > min(self.asks)


class Replay:
    """The books rebuilt from kalshi_ws rows fed in the order received (row()), by the rules of
    the module docstring. books holds the live ones only. Hooks for a caller that simulates on
    them: on_drop(ticker) when a live book is dropped or replaced by a snapshot,
    on_level(ticker, bid, px, before, after, t_us) after a delta, on_trade(ticker, yes_px, count,
    taker_bought_yes, t_us) for each print."""

    def __init__(self, keep=None):
        self.keep = keep               # ticker -> bool; None keeps every market
        self.books = {}
        self.seqs = {}                 # (conn, sid) -> last seq
        self.book_streams, self.trade_streams = set(), set()
        self.conn = None
        self.gaps = 0                  # sequence gaps on book subscriptions
        self.trade_gaps = 0            # on trade subscriptions (prints lost)
        self.gap_drops = Counter()     # ticker -> books dropped by a gap
        self.bad = Counter()           # ticker -> malformed or impossible frames
        self.seen = set()              # markets kept with at least one book frame
        self.on_drop = self.on_level = self.on_trade = None

    def drop(self, ticker):
        if self.books.pop(ticker, None) is not None and self.on_drop:
            self.on_drop(ticker)

    def drop_where(self, pred, gap=False):
        for tk in [tk for tk, b in self.books.items() if pred(b)]:
            self.drop(tk)
            if gap:
                self.gap_drops[tk] += 1

    def drop_all(self):
        self.drop_where(lambda b: True)

    def row(self, r):
        conn, typ, sid, seq, t = r.get('conn'), r.get('type'), r.get('sid'), r.get('seq'), r.get('recv_us')
        if conn is not None and conn != self.conn:
            if self.conn is not None:
                self.drop_where(lambda b: b.stream[0] != conn)   # a new socket: the old one's books are gone
            self.conn = conn
        is_book = typ in ('orderbook_snapshot', 'orderbook_delta')
        stream = (conn, sid)
        if isinstance(sid, int) and isinstance(seq, int):
            if is_book:
                self.book_streams.add(stream)
            elif typ == 'trade':
                self.trade_streams.add(stream)
            last = self.seqs.get(stream)
            self.seqs[stream] = seq
            if last is not None and seq != last + 1:
                if seq <= last:
                    self.drop_where(lambda b: b.stream == stream)   # a new subscription, numbered afresh
                elif stream in self.book_streams:
                    self.gaps += 1
                    self.drop_where(lambda b: b.stream == stream, gap=True)
                elif stream in self.trade_streams:
                    self.trade_gaps += 1
        if not is_book and typ != 'trade':
            return
        tk = r.get('ticker')
        if not tk or (self.keep is not None and not self.keep(tk)):
            return
        try:
            msg = json.loads(r.get('raw') or '{}').get('msg')
        except (ValueError, AttributeError):
            msg = None
        if not isinstance(msg, dict):
            if is_book:
                self.bad[tk] += 1
                self.drop(tk)
            return
        if typ == 'trade':
            if self.on_trade:
                p, q, side = px_of(msg.get('yes_price_dollars')), qty_of(msg.get('count_fp')), msg.get('taker_side')
                if p is not None and q and q > 0 and side in ('yes', 'no'):
                    self.on_trade(tk, p, q, side == 'yes', t)
            return
        self.seen.add(tk)
        try:
            if typ == 'orderbook_snapshot':
                book = Book(snapshot_side(msg, 'yes_dollars_fp', False), snapshot_side(msg, 'no_dollars_fp', True), stream)
                if book.crossed():
                    raise Bad('a crossed snapshot')
                self.drop(tk)
                self.books[tk] = book
                return
            book = self.books.get(tk)
            if book is None or book.stream != stream:
                return   # no snapshot since the book was dropped, or another subscription's
            side, p, d = msg.get('side'), px_of(msg.get('price_dollars')), qty_of(msg.get('delta_fp'), True)
            if side not in ('yes', 'no') or p is None or d is None:
                raise Bad('a malformed delta')
            bid = side == 'yes'
            p = p if bid else UNIT - p
            levels = book.bids if bid else book.asks
            before = levels.get(p, 0)
            after = before + d
            if after < 0:
                raise Bad('a delta below zero')
            if after:
                levels[p] = after
            else:
                levels.pop(p, None)
            if book.crossed():
                raise Bad('a crossed book')
            if self.on_level and after != before:
                self.on_level(tk, bid, p, before, after, t)
        except Bad:
            self.bad[tk] += 1
            self.drop(tk)


def drive(data, rp, period_us, on_period, downs=(), timers=None, offset_us=0):
    """Feed every kalshi_ws row to rp in the order received. Before each row, in time order:
    on_period(t) at offset_us past each whole multiple of period_us, each timer (a heap of (t_us, n, fn)) and
    each down instant (every book dropped) strictly before it; at a tie, the period, then the
    timer, then the drop. Frames received at an instant count as before it. The data's span:
    the first and last row's recv_us (None, None when there is none)."""
    timers = [] if timers is None else timers
    downs, di = list(downs), 0
    nxt = first = last = None

    def advance(t):
        nonlocal nxt, di
        while True:
            best, kind = None, None
            if nxt is not None and nxt < t:
                best, kind = nxt, 0
            if timers and timers[0][0] < t and (best is None or timers[0][0] < best):
                best, kind = timers[0][0], 1
            if di < len(downs) and downs[di] < t and (best is None or downs[di] < best):
                best, kind = downs[di], 2
            if kind is None:
                return
            if kind == 0:
                on_period(nxt)
                nxt += period_us
            elif kind == 1:
                heapq.heappop(timers)[2]()
            else:
                rp.drop_all()
                di += 1

    for r in rows(data, 'kalshi_ws'):
        t = r.get('recv_us')
        if not isinstance(t, int) or isinstance(t, bool):
            continue
        if first is None:
            first = t
            nxt = -(-(t - offset_us) // period_us) * period_us + offset_us
            while di < len(downs) and downs[di] < t:
                di += 1
        advance(t)
        rp.row(r)
        last = t if last is None else max(last, t)
    if last is not None:
        advance(last + 1)
    return first, last


# ---- the join rule --------------------------------------------------------------------------
def side_size(levels, bid, touch):
    if not levels:
        return 0
    return levels[max(levels) if bid else min(levels)] if touch else sum(levels.values())


def windows(markets, terms, want, target=None):
    """[(start_us, end_us, ticker, series, target, source)] sorted: each market's sampling
    window(s), by the rule of the module docstring."""
    out = []
    for tk in sorted(set(markets) | set(terms.own)):
        ser = series_of(tk, markets)
        if not want(ser):
            continue
        own = terms.own.get(tk)
        spans = [(p['start_us'], p['end_us'], p) for p in own] if own else []
        m = markets.get(tk)
        if not spans and m and m['open_us'] is not None and m['close_us'] is not None and m['close_us'] > m['open_us']:
            spans = [(m['open_us'], m['close_us'], None)]
        for a, b, p in spans:
            tgt, src = (int(round(target * CT)), 'flag') if target is not None else terms.target(tk, ser, p)
            out.append((a, b, tk, ser, tgt, src))
    out.sort()
    return out


def analyse(data, series=None, target=None, touch=False):
    markets = load_markets(data)
    terms = Terms(load_programs(data), markets)
    want = (lambda s: s in series) if series else (lambda s: True)
    wins = windows(markets, terms, want, target)
    sampled_tickers = {w[2] for w in wins}
    cache = {}

    def keep(tk):
        v = cache.get(tk)
        if v is None:
            v = cache[tk] = want(series_of(tk, markets))
        return v

    rp = Replay(keep=keep)
    stats = {}
    active, nxt = {}, [0]

    def on_minute(m):
        while nxt[0] < len(wins) and wins[nxt[0]][0] <= m:
            active[nxt[0]] = wins[nxt[0]]
            nxt[0] += 1
        for k in [k for k, w in active.items() if w[1] <= m]:
            del active[k]
        for a, b, tk, ser, tgt, src in active.values():
            s = stats.setdefault(ser, {'markets': set(), 'samples': 0, 'bid': 0, 'ask': 0, 'both': 0, 'missing': 0,
                                       'targets': Counter()})
            book = rp.books.get(tk)
            if book is None:
                s['missing'] += 1
                continue
            s['samples'] += 1
            s['markets'].add(tk)
            s['targets'][(tgt, src)] += 1
            b = side_size(book.bids, True, touch) >= tgt
            a_ = side_size(book.asks, False, touch) >= tgt
            s['bid'] += b
            s['ask'] += a_
            s['both'] += b and a_

    downs = down_instants(data)
    first, last = drive(data, rp, MINUTE_US, on_minute, downs, offset_us=SAMPLE_AT_US)
    out = {}
    for ser in sorted(stats):
        s = stats[ser]
        n = s['samples']
        frac = {k: (s[k] / n if n else 0.0) for k in ('bid', 'ask', 'both')}
        out[ser] = {'markets': len(s['markets']), 'samples': n, 'yes_bid': frac['bid'], 'yes_ask': frac['ask'],
                    'both': frac['both'], 'missing': s['missing'],
                    'gaps': sum(c for tk, c in rp.gap_drops.items() if series_of(tk, markets) == ser),
                    'bad': sum(c for tk, c in rp.bad.items() if series_of(tk, markets) == ser),
                    'targets': {'%g %s' % (t / CT, src): c for (t, src), c in sorted(s['targets'].items())},
                    'verdict': 'JOIN' if frac['both'] > 0.5 else 'NOT YET'}
    return {'from': iso(first) if first is not None else None, 'to': iso(last) if last is not None else None,
            'rule': 'touch' if touch else 'book', 'book_gaps': rp.gaps, 'down': len(downs),
            'bad': sum(rp.bad.values()), 'unsampled': sorted(rp.seen - sampled_tickers), 'series': out}


def report(res):
    rule = ("the best price's size alone (--touch)" if res['rule'] == 'touch' else
            'its size from the best price outward, every level (deskd strat/lip.h)')
    lines = ['lip_join: %s to %s UTC; a side holds the target by %s' % (res['from'] or '-', res['to'] or '-', rule),
             'book sequence gaps %d, disconnects and restarts %d, bad frames %d; %d market(s) with books and no '
             'recorded window, not sampled' % (res['book_gaps'], res['down'], res['bad'], len(res['unsampled'])),
             '%-16s %7s %7s %7s %7s %7s %7s %5s  %-18s %s' % ('series', 'markets', 'samples', 'yes_bid', 'yes_ask',
                                                              'both', 'missing', 'gaps', 'target', 'verdict')]
    for ser, s in res['series'].items():
        lines.append('%-16s %7d %7d %6.1f%% %6.1f%% %6.1f%% %7d %5d  %-18s %s' % (
            ser, s['markets'], s['samples'], 100 * s['yes_bid'], 100 * s['yes_ask'], 100 * s['both'], s['missing'],
            s['gaps'], ', '.join(s['targets']) or '-', s['verdict']))
    if not res['series']:
        lines.append('(no market of the series asked for has a recorded window in the data)')
    return '\n'.join(lines)


def run(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--data', help="the recorder's data directory")
    ap.add_argument('--series', help='comma-separated series (default: every series with a recorded window)')
    ap.add_argument('--target', type=float, help='the target size in contracts, over any recorded program')
    ap.add_argument('--touch', action='store_true', help="count the best price's size alone")
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.data or not os.path.isdir(a.data):
        print('error: --data DIR (the recorder\'s data directory) is required', file=sys.stderr)
        return 2
    if a.target is not None and a.target <= 0:
        print('error: --target must be above 0', file=sys.stderr)
        return 2
    series = {s.strip() for s in a.series.split(',') if s.strip()} if a.series else None
    res = analyse(a.data, series, a.target, a.touch)
    print(json.dumps(res, indent=1) if a.json else report(res))
    return 0


# ---- self-test ------------------------------------------------------------------------------
class Spool:
    """Synthetic recorder data, in the recorder's shape (store.py's tables, rec.py's frame_row)."""

    def __init__(self, root):
        self.root = root

    def write(self, table, row, torn=False):
        d = os.path.join(self.root, 'spool', table)
        os.makedirs(d, exist_ok=True)
        bucket = datetime.fromtimestamp(row['recv_us'] / 1e6, tz=timezone.utc).strftime('%Y%m%dT%H')
        with open(os.path.join(d, bucket + '.jsonl'), 'a', encoding='utf-8') as fh:
            fh.write(json.dumps(row, separators=(',', ':'))[:30] if torn else json.dumps(row, separators=(',', ':')) + '\n')

    def frame(self, t_us, conn, typ, sid, seq, msg):
        raw = {'type': typ, 'sid': sid, 'seq': seq, 'msg': msg}
        self.write('kalshi_ws', {'recv_us': t_us, 'mono_us': t_us, 'conn': conn, 'type': typ, 'sid': sid, 'seq': seq,
                                 'ticker': msg.get('market_ticker'), 'raw': json.dumps(raw, separators=(',', ':'))})

    def snapshot(self, t_us, conn, sid, seq, tk, yes, no):
        def lv(side):
            return [['%.4f' % p, '%.2f' % q] for p, q in side]
        self.frame(t_us, conn, 'orderbook_snapshot', sid, seq,
                   {'market_ticker': tk, 'market_id': 'id-' + tk, 'yes_dollars_fp': lv(yes), 'no_dollars_fp': lv(no)})

    def delta(self, t_us, conn, sid, seq, tk, side, price, d):
        self.frame(t_us, conn, 'orderbook_delta', sid, seq,
                   {'market_ticker': tk, 'market_id': 'id-' + tk, 'side': side, 'price_dollars': '%.4f' % price,
                    'delta_fp': '%.2f' % d, 'ts': t_us // 1_000_000})

    def trade(self, t_us, conn, sid, seq, tk, yes_price, count, taker_side):
        self.frame(t_us, conn, 'trade', sid, seq,
                   {'trade_id': 'tr-%d' % t_us, 'market_ticker': tk, 'yes_price_dollars': '%.4f' % yes_price,
                    'no_price_dollars': '%.4f' % (1 - yes_price), 'count_fp': '%.2f' % count, 'taker_side': taker_side,
                    'ts': t_us // 1_000_000})

    def market(self, t_us, series, tk, open_us, close_us, raw=None):
        def rfc(u):
            return datetime.fromtimestamp(u / 1e6, tz=timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
        raw = dict(raw or {}, ticker=tk, open_time=rfc(open_us), close_time=rfc(close_us))
        self.write('kalshi_markets', {'recv_us': t_us, 'series': series, 'ticker': tk, 'status': 'active',
                                      'open_time': rfc(open_us), 'close_time': rfc(close_us),
                                      'raw': json.dumps(raw, separators=(',', ':'))})

    def program(self, t_us, tk, typ, start_us, end_us, target, discount_bps=5000, reward=200000):
        def rfc(u):
            return datetime.fromtimestamp(u / 1e6, tz=timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
        self.write('kalshi_incentives', {'recv_us': t_us, 'sweep_us': t_us, 'market_ticker': tk, 'incentive_type': typ,
                                         'period_reward': float(reward), 'target_size': float(target),
                                         'discount_factor_bps': float(discount_bps), 'start_date': rfc(start_us),
                                         'end_date': rfc(end_us), 'paid_out': False, 'raw': '{}'})


def self_test():
    fails = []

    def check(name, ok, detail=''):
        print('%s %s%s' % ('OK  ' if ok else 'FAIL', name, (': ' + str(detail)) if detail else ''))
        if not ok:
            fails.append(name)

    # deskd's units
    check('px_of: dollars strings in 1/10,000 $', px_of('0.4200') == 4200 and px_of('0.42') == 4200 and
          px_of('1.0000') == 10000 and px_of('0.42001') is None and px_of('-0.10') is None and px_of(0.42) is None)
    check('qty_of: counts to 1/100, negative only for a delta', qty_of('300.00') == 30000 and qty_of('12') == 1200
          and qty_of('-5.00') is None and qty_of('-5.00', True) == -500 and qty_of('1.234') is None)

    # A recorded stretch: books on subscription 1 of connection 1, trades on subscription 2,
    # a third market on subscription 3. t0 is 14:58 UTC, so the spool spans two hour files.
    t0 = int(datetime(2026, 10, 20, 14, 58, tzinfo=timezone.utc).timestamp()) * 1_000_000
    s = 1_000_000
    A, B, C = 'KXNATGAS15M-26OCT201500-00', 'KXPLATINUM15M-26OCT201500-00', 'KXNATGAS15M-26OCT201515-15'
    with tempfile.TemporaryDirectory() as d:
        sp = Spool(d)
        sp.market(t0 - 60 * s, 'KXNATGAS15M', A, t0, t0 + 360 * s)
        sp.market(t0 - 60 * s, 'KXPLATINUM15M', B, t0, t0 + 900 * s)
        sp.program(t0 - 60 * s, B, 'liquidity', t0, t0 + 360 * s, 299)   # B's window and target: its program's
        sp.program(t0 - 60 * s, A, 'volume', t0, t0 + 360 * s, 50)       # not a liquidity program: ignored
        # A holds 350 bid (200 at the touch) and 400 ask; B 300 a side
        sp.snapshot(t0 - 10 * s, 1, 1, 1, A, [(0.40, 200), (0.39, 150)], [(0.58, 400)])
        sp.snapshot(t0 - 9 * s, 1, 1, 2, B, [(0.30, 300)], [(0.68, 300)])
        sp.snapshot(t0 - 8 * s, 1, 3, 1, C, [(0.50, 900)], [(0.49, 900)])   # no window recorded for C
        sp.delta(t0 + 40 * s, 1, 1, 3, B, 'yes', 0.30, -1)                  # B's bid 299 from here
        sp.trade(t0 + 45 * s, 1, 2, 1, A, 0.40, 5, 'no')
        sp.trade(t0 + 50 * s, 1, 2, 3, A, 0.40, 5, 'no')                    # a trade gap: drops no book
        sp.delta(t0 + 100 * s, 1, 1, 4, A, 'yes', 0.40, -100)               # A's bid 250 from here
        sp.delta(t0 + 160 * s, 1, 1, 6, A, 'yes', 0.40, 500)                # seq 6 after 4: a gap, not applied
        sp.delta(t0 + 165 * s, 1, 1, 7, A, 'yes', 0.39, 1)                  # no snapshot yet: ignored
        sp.snapshot(t0 + 240 * s, 1, 1, 8, A, [(0.40, 200), (0.39, 150)], [(0.58, 400)])
        sp.snapshot(t0 + 241 * s, 1, 1, 9, B, [(0.30, 300)], [(0.68, 300)])
        sp.delta(t0 + 280 * s, 1, 1, 10, B, 'no', 0.68, 10)
        sp.write('gaps', {'recv_us': t0 + 340 * s, 'source': 'kalshi_ws', 'kind': 'disconnect', 'detail': 'test',
                          'from_us': t0 + 280 * s, 'to_us': None})
        sp.snapshot(t0 + 350 * s, 2, 1, 1, A, [(0.40, 900)], [(0.58, 900)])  # connection 2, after the last sample
        sp.write('kalshi_ws', {'recv_us': t0 + 100 * s, 'type': 'orderbook_delta'}, torn=True)
        # Samples at t0 + 30, 90, ... 330 s inside both windows. A: 350/400, 350/400, 250/400, (the
        # gap), 350/400, (the disconnect): the bid holds 300 in 3 of 4 samples, the ask in 4. B:
        # 300/300, 299/300, 299/300, (gap), 300/300, (disconnect).
        r = analyse(d)
        a, b = r['series'].get('KXNATGAS15M', {}), r['series'].get('KXPLATINUM15M', {})
        check('A: 4 samples, 2 missing (the gap, the disconnect), 1 market', (a.get('samples'), a.get('missing'),
              a.get('markets')) == (4, 2, 1), a)
        check('A: the bid holds 300 in 3 of 4, the ask in 4, both in 3: JOIN',
              (a.get('yes_bid'), a.get('yes_ask'), a.get('both'), a.get('verdict')) == (0.75, 1.0, 0.75, 'JOIN'), a)
        check('A: the sequence gap counted, its target the default', a.get('gaps') == 1 and r['book_gaps'] == 1
              and a.get('targets') == {'300 default': 4}, (a.get('gaps'), r['book_gaps'], a.get('targets')))
        check("B: its program's target (299) and window", (b.get('samples'), b.get('yes_bid'), b.get('verdict'),
              b.get('targets')) == (4, 1.0, 'JOIN', {'299 program': 4}), b)
        check('C: books without a window are not sampled', r['unsampled'] == [C], r['unsampled'])
        check('the disconnect is read from the gaps table', r['down'] == 1, r['down'])
        r300 = analyse(d, target=300)
        b3 = r300['series']['KXPLATINUM15M']
        check('--target 300: B holds it in 2 of 4, half is not most: NOT YET',
              (b3['yes_bid'], b3['both'], b3['verdict'], b3['targets']) == (0.5, 0.5, 'NOT YET', {'300 flag': 4}), b3)
        rt = analyse(d, series={'KXNATGAS15M'}, touch=True)
        at = rt['series']['KXNATGAS15M']
        check('--touch: A has 200 at its best bid, never 300: NOT YET',
              (at['yes_bid'], at['yes_ask'], at['both'], at['verdict']) == (0.0, 1.0, 0.0, 'NOT YET')
              and list(rt['series']) == ['KXNATGAS15M'], rt['series'])
        # a series' own latest program is the fallback target for a market without one
        sp.program(t0 - 30 * s, 'KXNATGAS15M-26OCT201445-45', 'liquidity', t0 - 900 * s, t0, 350)
        a2 = analyse(d, series={'KXNATGAS15M'})['series']['KXNATGAS15M']
        check("a series' latest program sets the target of a market without one (350)",
              (a2['targets'], a2['yes_bid'], a2['verdict']) == ({'350 series': 4}, 0.75, 'JOIN'), a2)
        check('the CLI runs', run(['--data', d, '--target', '300']) == 0)
        check('the CLI refuses a missing directory', run(['--data', os.path.join(d, 'none')]) == 2)

    # the replay's rules, on rows fed directly
    rp = Replay()

    def row(t, conn, typ, sid, seq, msg):
        return {'recv_us': t, 'conn': conn, 'type': typ, 'sid': sid, 'seq': seq, 'ticker': msg.get('market_ticker'),
                'raw': json.dumps({'type': typ, 'sid': sid, 'seq': seq, 'msg': msg})}
    snap = {'market_ticker': 'X', 'yes_dollars_fp': [['0.4000', '10.00']], 'no_dollars_fp': [['0.5500', '5.00']]}
    rp.row(row(1, 1, 'orderbook_snapshot', 1, 1, snap))
    check('a snapshot: NO bids are YES asks at $1 - price', rp.books['X'].bids == {4000: 1000}
          and rp.books['X'].asks == {4500: 500})
    rp.row(row(2, 1, 'orderbook_delta', 1, 2, {'market_ticker': 'X', 'side': 'no', 'price_dollars': '0.5500',
                                                'delta_fp': '-5.00'}))
    check('a delta to zero removes the level', rp.books['X'].asks == {})
    rp.row(row(3, 1, 'orderbook_delta', 1, 3, {'market_ticker': 'X', 'side': 'yes', 'price_dollars': '0.4000',
                                                'delta_fp': '-11.00'}))
    check('a delta below zero drops the book, counted as bad', 'X' not in rp.books and rp.bad['X'] == 1)
    rp.row(row(4, 1, 'orderbook_snapshot', 1, 4, snap))
    rp.row(row(5, 1, 'orderbook_snapshot', 1, 1, snap))
    check('a number at or below the last is a new subscription, not a gap', rp.gaps == 0 and 'X' in rp.books)
    rp.row(row(6, 2, 'ok', 1, 1, {}))
    check('a frame on a new connection drops the old books', 'X' not in rp.books)

    # A window whose book is empty at its open (subscribed 20 minutes before, as the recorder
    # does) and holds 300 a side from 2 s after it: every sample, mid-minute, holds the target
    t1 = int(datetime(2026, 10, 20, 15, 0, tzinfo=timezone.utc).timestamp()) * 1_000_000
    D = 'KXCOPPER15M-26OCT201515-15'
    with tempfile.TemporaryDirectory() as d:
        sp = Spool(d)
        sp.market(t1 - 1300 * s, 'KXCOPPER15M', D, t1, t1 + 900 * s)
        sp.program(t1 - 1300 * s, D, 'liquidity', t1, t1 + 900 * s, 300)
        sp.frame(t1 - 1200 * s, 1, 'orderbook_snapshot', 1, 1, {'market_ticker': D, 'market_id': 'id-' + D})
        sp.delta(t1 + 2 * s, 1, 1, 2, D, 'yes', 0.40, 300)
        sp.delta(t1 + 2 * s + 1000, 1, 1, 3, D, 'no', 0.58, 300)
        sp.delta(t1 + 899 * s, 1, 1, 4, D, 'yes', 0.39, 1)
        e = analyse(d)['series'].get('KXCOPPER15M', {})
        check('an empty book at the open, full from 2 s after: 15 samples, both sides in all of them',
              (e.get('samples'), e.get('missing'), e.get('both'), e.get('verdict')) == (15, 0, 1.0, 'JOIN'), e)
    print('lip_join self-test: %s' % ('ok' if not fails else 'FAILED (%d)' % len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(run(sys.argv[1:]))
