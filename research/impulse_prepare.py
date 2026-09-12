#!/usr/bin/env python3
"""Turn the raw recordings into one aligned, compact dataset for the impulse study.

The recorder writes gzipped JSONL per (source, hour), deliberately unparsed, because the parse you
regret is the one you baked in at write time. That is the right way to record and the wrong way to
analyse: every study would otherwise re-parse 800 MB to answer a question about ten minutes.

So this is the one place where a parsing decision is made, it is made once, and it is written down
here rather than spread across the study:

  - external venues are reduced to top-of-book (bid, ask) with OUR receive timestamp in
    microseconds, because that is the clock a strategy on this desk would actually have had
  - Kalshi's perp `ticker` gives the quoted price, and is kept apart from
  - Kalshi's `orderbook_delta` stream, which is what says whether a quantity could be executed
  - the CF index is kept because it is what the perp settles against

Nothing is aggregated, smoothed or resampled. Every row keeps both clocks where the venue gives
two, so a lag measured later can be attributed to the venue or to our network rather than assumed.

Usage:
  python3 research/impulse_prepare.py --raw data/raw --out data/impulse.sqlite
  python3 research/impulse_prepare.py --coins BTC ETH --hours 2026-09-11T18 2026-09-11T19
  python3 research/impulse_prepare.py --self-test
"""
import argparse
import glob
import gzip
import json
import os
import sqlite3
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(HERE, '..', 'data', 'raw')
OUT = os.path.join(HERE, '..', 'data', 'impulse.sqlite')

# One coin, five names. Kept in one place so a study never has to know a venue's spelling.
COINS = {
    'BTC': {'coinbase': 'BTC-USD', 'binance': 'BTCUSDT', 'kraken': 'BTC/USD',
            'hyperliquid': 'BTC', 'index': 'BTCUSD_RTI', 'kalshi': 'KXBTCPERP'},
    'ETH': {'coinbase': 'ETH-USD', 'binance': 'ETHUSDT', 'kraken': 'ETH/USD',
            'hyperliquid': 'ETH', 'index': 'ETHUSD_RTI', 'kalshi': 'KXETHPERP'},
    'SOL': {'coinbase': 'SOL-USD', 'binance': 'SOLUSDT', 'kraken': 'SOL/USD',
            'hyperliquid': 'SOL', 'index': 'SOLUSD_RTI', 'kalshi': 'KXSOLPERP'},
    'XRP': {'coinbase': 'XRP-USD', 'binance': 'XRPUSDT', 'kraken': 'XRP/USD',
            'hyperliquid': 'XRP', 'index': 'XRPUSD_RTI', 'kalshi': 'KXXRPPERP'},
}

SCHEMA = """
CREATE TABLE IF NOT EXISTS ext_quote(
  ts_us INTEGER, src_ts_us INTEGER, venue TEXT, coin TEXT, bid REAL, ask REAL);
CREATE INDEX IF NOT EXISTS idx_ext_quote ON ext_quote(coin, ts_us);
CREATE TABLE IF NOT EXISTS index_value(
  ts_us INTEGER, src_ts_us INTEGER, coin TEXT, value REAL);
CREATE INDEX IF NOT EXISTS idx_index_value ON index_value(coin, ts_us);
CREATE TABLE IF NOT EXISTS kalshi_ticker(
  ts_us INTEGER, src_ts_us INTEGER, coin TEXT, ticker TEXT, bid REAL, ask REAL, last REAL,
  bid_size REAL, ask_size REAL);
CREATE INDEX IF NOT EXISTS idx_kalshi_ticker ON kalshi_ticker(coin, ts_us);
CREATE TABLE IF NOT EXISTS kalshi_book(
  ts_us INTEGER, src_ts_us INTEGER, coin TEXT, ticker TEXT, seq INTEGER, kind TEXT,
  side TEXT, price REAL, delta REAL);
CREATE INDEX IF NOT EXISTS idx_kalshi_book ON kalshi_book(coin, ts_us);
-- The delta stream compressed to what it can honestly say. No snapshot was ever captured on this
-- recording, so the absolute book cannot be rebuilt from it and any "best bid" derived from it
-- would be invented. What the stream CAN say is when the book was touched and where, at a
-- resolution the 1 Hz ticker channel does not reach - so it is kept as activity per 100 ms.
CREATE TABLE IF NOT EXISTS book_activity(
  bucket_us INTEGER, coin TEXT, side TEXT, events INTEGER, min_price REAL, max_price REAL,
  net_delta REAL, seq_gaps INTEGER, PRIMARY KEY(bucket_us, coin, side));
CREATE TABLE IF NOT EXISTS kalshi_trade(
  ts_us INTEGER, src_ts_us INTEGER, coin TEXT, ticker TEXT, price REAL, count REAL, taker_side TEXT);
CREATE INDEX IF NOT EXISTS idx_kalshi_trade ON kalshi_trade(coin, ts_us);
CREATE TABLE IF NOT EXISTS prepare_log(
  ts INTEGER, source TEXT, hour TEXT, lines INTEGER, kept INTEGER, seconds REAL,
  truncated INTEGER);
"""


def f(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def coin_of(venue_key, symbol, coins):
    for coin, names in coins.items():
        if names.get(venue_key) == symbol:
            return coin
    return None


def handle(source, rec, coins, out):
    """Append zero or more (table, row) to `out`. One place, one parsing decision each."""
    recv = rec.get('recv_ts_us')
    src = rec.get('src_ts_us')
    msg = rec.get('msg')
    if recv is None or msg is None:
        return
    if source == 'ext_coinbase':
        if not isinstance(msg, dict) or msg.get('type') != 'ticker':
            return
        coin = coin_of('coinbase', msg.get('product_id'), coins)
        if coin:
            out.append(('ext_quote', (recv, src, 'coinbase', coin,
                                      f(msg.get('best_bid')), f(msg.get('best_ask')))))
    elif source == 'ext_binance':
        # bookTicker: b/B bid, a/A ask. No source timestamp on this stream, which is itself a fact
        # worth keeping: any lead-lag involving binance is measured on our receive clock only.
        if not isinstance(msg, dict) or 's' not in msg:
            return
        coin = coin_of('binance', msg.get('s'), coins)
        if coin:
            out.append(('ext_quote', (recv, src, 'binance', coin, f(msg.get('b')), f(msg.get('a')))))
    elif source == 'ext_kraken':
        if not isinstance(msg, list):
            return
        for t in msg:
            if not isinstance(t, dict):
                continue
            coin = coin_of('kraken', t.get('symbol'), coins)
            if coin:
                out.append(('ext_quote', (recv, src, 'kraken', coin, f(t.get('bid')), f(t.get('ask')))))
    elif source == 'ext_hyperliquid':
        if not isinstance(msg, dict) or 'levels' not in msg:
            return
        coin = coin_of('hyperliquid', msg.get('coin'), coins)
        levels = msg.get('levels')
        if not coin or not isinstance(levels, list) or len(levels) < 2:
            return
        bids, asks = levels[0], levels[1]
        bid = f(bids[0].get('px')) if bids else None
        ask = f(asks[0].get('px')) if asks else None
        out.append(('ext_quote', (recv, src, 'hyperliquid', coin, bid, ask)))
    elif source == 'kalshi_index':
        if not isinstance(msg, dict):
            return
        coin = coin_of('index', msg.get('index_id'), coins)
        if coin:
            out.append(('index_value', (recv, src, coin, f(msg.get('value_usd')))))
    elif source == 'kalshi_margin':
        if not isinstance(msg, dict):
            return
        ticker = msg.get('market_ticker')
        coin = coin_of('kalshi', ticker, coins)
        if not coin:
            return
        ev = rec.get('ev')
        if ev == 'ticker':
            out.append(('kalshi_ticker', (recv, src, coin, ticker, f(msg.get('bid')), f(msg.get('ask')),
                                          f(msg.get('price')), f(msg.get('bid_size_fp')),
                                          f(msg.get('ask_size_fp')))))
        elif ev in ('orderbook_delta', 'orderbook_snapshot'):
            out.append(('kalshi_book', (recv, src, coin, ticker, rec.get('seq'), ev,
                                        msg.get('side'), f(msg.get('price')), f(msg.get('delta')))))
        elif ev == 'trade':
            out.append(('kalshi_trade', (recv, src, coin, ticker, f(msg.get('price')),
                                         f(msg.get('count')), msg.get('taker_side'))))


COLUMNS = {
    'ext_quote': 6, 'index_value': 4, 'kalshi_ticker': 9, 'kalshi_book': 9, 'kalshi_trade': 7,
}


BOOK_BUCKET_US = 100_000


def run(raw, out_path, coins, hours=None, progress=True, sources=None, book_mode='aggregate'):
    con = sqlite3.connect(out_path)
    con.executescript(SCHEMA)
    con.execute('PRAGMA synchronous=OFF')
    con.execute('PRAGMA journal_mode=MEMORY')
    sources = sources or ['ext_coinbase', 'ext_binance', 'ext_kraken', 'ext_hyperliquid',
                          'kalshi_index', 'kalshi_margin']
    totals = {}
    for source in sources:
        for path in sorted(glob.glob(os.path.join(raw, f'source={source}', 'hour=*', 'events.jsonl.gz'))):
            hour = path.split('hour=')[1].split(os.sep)[0]
            if hours and hour not in hours:
                continue
            t0 = time.time()
            rows, lines, kept, truncated = [], 0, 0, False
            activity, last_seq, gaps = {}, {}, 0
            # The recorder may be writing this very file. A gzip member that has not been closed
            # raises at the end; that is the normal state of the current hour, not a failure, so
            # take what is there and record that it was cut short.
            try:
                with gzip.open(path, 'rt') as fh:
                    for line in fh:
                        lines += 1
                        try:
                            rec = json.loads(line)
                        except Exception:
                            continue
                        before = len(rows)
                        handle(source, rec, coins, rows)
                        # The book stream is 300x the ticker stream and cannot be anchored, so by
                        # default it is folded into activity buckets rather than stored row by row.
                        if book_mode != 'raw':
                            keep = []
                            for table, row in rows[before:]:
                                if table != 'kalshi_book':
                                    keep.append((table, row))
                                    continue
                                ts, _src, coin, _tk, seq, _kind, side, price, delta = row
                                if side not in ('bid', 'ask') or price is None:
                                    continue
                                if seq is not None:
                                    prev = last_seq.get(coin)
                                    if prev is not None and seq > prev + 1:
                                        gaps += 1
                                    last_seq[coin] = seq
                                if book_mode == 'skip':
                                    continue
                                key = ((ts // BOOK_BUCKET_US) * BOOK_BUCKET_US, coin, side)
                                a = activity.get(key)
                                if a is None:
                                    activity[key] = [1, price, price, delta or 0.0, 0]
                                else:
                                    a[0] += 1
                                    a[1] = min(a[1], price)
                                    a[2] = max(a[2], price)
                                    a[3] += (delta or 0.0)
                            del rows[before:]
                            rows.extend(keep)
                        kept += len(rows) - before
                        if len(rows) >= 20000:
                            flush(con, rows)
                            rows = []
            except (EOFError, OSError, gzip.BadGzipFile) as e:
                truncated = True
                if progress:
                    print(f'{source:18} {hour}  still being written ({type(e).__name__}); '
                          f'keeping the {lines:,} lines that were complete', flush=True)
            flush(con, rows)
            if activity:
                con.executemany(
                    'INSERT INTO book_activity VALUES(?,?,?,?,?,?,?,?) ON CONFLICT DO UPDATE SET '
                    'events=events+excluded.events, min_price=MIN(min_price,excluded.min_price), '
                    'max_price=MAX(max_price,excluded.max_price), net_delta=net_delta+excluded.net_delta',
                    [(k[0], k[1], k[2], v[0], v[1], v[2], v[3], 0) for k, v in activity.items()])
                kept += len(activity)
            if gaps:
                con.execute('UPDATE book_activity SET seq_gaps=? WHERE bucket_us=(SELECT MAX(bucket_us) '
                            'FROM book_activity)', (gaps,))
            con.execute('INSERT INTO prepare_log VALUES(?,?,?,?,?,?,?)',
                        (int(time.time()), source, hour, lines, kept, time.time() - t0,
                         1 if truncated else 0))
            con.commit()
            totals[source] = totals.get(source, 0) + kept
            if progress:
                print(f'{source:18} {hour}  {lines:>9,} lines  {kept:>8,} kept  '
                      f'{time.time()-t0:5.1f}s', flush=True)
    con.commit()
    if progress:
        print()
        for t in COLUMNS:
            n = con.execute(f'SELECT COUNT(*) FROM {t}').fetchone()[0]
            print(f'  {t:16} {n:>10,}')
    con.close()
    return totals


def flush(con, rows):
    by_table = {}
    for table, row in rows:
        by_table.setdefault(table, []).append(row)
    for table, rs in by_table.items():
        marks = ','.join('?' * COLUMNS[table])
        con.executemany(f'INSERT INTO {table} VALUES({marks})', rs)


def self_test():
    coins = {'BTC': COINS['BTC']}
    out = []
    handle('ext_coinbase', {'recv_ts_us': 10, 'src_ts_us': 9,
                            'msg': {'type': 'ticker', 'product_id': 'BTC-USD',
                                    'best_bid': '77250.01', 'best_ask': '77250.02'}}, coins, out)
    assert out == [('ext_quote', (10, 9, 'coinbase', 'BTC', 77250.01, 77250.02))], out
    out = []
    handle('ext_binance', {'recv_ts_us': 11, 'src_ts_us': None,
                           'msg': {'s': 'BTCUSDT', 'b': '1.0', 'a': '2.0'}}, coins, out)
    assert out[0][1][2] == 'binance' and out[0][1][4] == 1.0
    out = []
    handle('ext_kraken', {'recv_ts_us': 12, 'src_ts_us': 12,
                          'msg': [{'symbol': 'BTC/USD', 'bid': 3.0, 'ask': 4.0}]}, coins, out)
    assert out[0][1][4] == 3.0 and out[0][1][3] == 'BTC'
    out = []
    handle('ext_hyperliquid', {'recv_ts_us': 13, 'src_ts_us': 13,
                               'msg': {'coin': 'BTC', 'levels': [[{'px': '5.0'}], [{'px': '6.0'}]]}}, coins, out)
    assert out[0][1][4] == 5.0 and out[0][1][5] == 6.0
    out = []
    handle('kalshi_margin', {'recv_ts_us': 14, 'src_ts_us': 14, 'ev': 'orderbook_delta', 'seq': 77,
                             'msg': {'market_ticker': 'KXBTCPERP', 'price': '77.25', 'delta': '-5.00',
                                     'side': 'bid'}}, coins, out)
    assert out[0][0] == 'kalshi_book' and out[0][1][4] == 77 and out[0][1][6] == 'bid'
    out = []
    handle('kalshi_margin', {'recv_ts_us': 15, 'src_ts_us': 15, 'ev': 'ticker',
                             'msg': {'market_ticker': 'KXBTCPERP', 'bid': '0.70', 'ask': '0.71',
                                     'price': '0.705', 'bid_size_fp': '100.00', 'ask_size_fp': '200.00'}}, coins, out)
    assert out[0][0] == 'kalshi_ticker' and out[0][1][4] == 0.70
    # A coin we are not studying is dropped, not mislabelled.
    out = []
    handle('kalshi_margin', {'recv_ts_us': 16, 'ev': 'ticker',
                             'msg': {'market_ticker': 'KXDOGEPERP', 'bid': '1'}}, coins, out)
    assert out == []
    # Malformed input costs a row, never the run.
    for bad in ({'recv_ts_us': None}, {'msg': None}, {'recv_ts_us': 1, 'msg': []}):
        out = []
        handle('ext_coinbase', bad, coins, out)
        assert out == []

    # End to end against a tiny file laid out the way the recorder lays them out.
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 'source=ext_coinbase', 'hour=2026-09-11T18')
        os.makedirs(p)
        with gzip.open(os.path.join(p, 'events.jsonl.gz'), 'wt') as fh:
            for i in range(50):
                fh.write(json.dumps({'recv_ts_us': 1000 + i, 'src_ts_us': 999 + i,
                                     'msg': {'type': 'ticker', 'product_id': 'BTC-USD',
                                             'best_bid': 100 + i, 'best_ask': 101 + i}}) + '\n')
            fh.write('{not json\n')
        db = os.path.join(d, 'impulse.sqlite')
        run(d, db, coins, progress=False)
        con = sqlite3.connect(db)
        assert con.execute('SELECT COUNT(*) FROM ext_quote').fetchone()[0] == 50
        assert con.execute('SELECT COUNT(*) FROM prepare_log').fetchone()[0] == 1
        assert con.execute('SELECT lines FROM prepare_log').fetchone()[0] == 51   # the bad line counted
        # A file the recorder is still writing is normal, not a failure.
        import io
        p2 = os.path.join(d, 'source=ext_coinbase', 'hour=2026-09-11T19')
        os.makedirs(p2)
        buf = io.BytesIO()
        with gzip.GzipFile(fileobj=buf, mode='wb') as z:
            for i in range(30):
                z.write((json.dumps({'recv_ts_us': 2000 + i, 'src_ts_us': 1999 + i,
                                     'msg': {'type': 'ticker', 'product_id': 'BTC-USD',
                                             'best_bid': 1, 'best_ask': 2}}) + '\n').encode())
        with open(os.path.join(p2, 'events.jsonl.gz'), 'wb') as fh:
            fh.write(buf.getvalue()[:-40])          # cut the end-of-stream marker off
        db2 = os.path.join(d, 'impulse2.sqlite')
        run(d, db2, coins, progress=False)
        con2 = sqlite3.connect(db2)
        got = con2.execute('SELECT COUNT(*) FROM ext_quote').fetchone()[0]
        assert got >= 50, got                        # the complete hour, plus whatever survived
        assert con2.execute('SELECT SUM(truncated) FROM prepare_log').fetchone()[0] >= 1
    print('self-test OK: every venue parses to the same shape, an unknown coin is dropped rather')
    print('  than mislabelled, malformed input costs a row rather than the run, and the end-to-end')
    print('  pass over a recorder-shaped directory lands 50 quotes.')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--raw', default=RAW)
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--coins', nargs='*', default=['BTC', 'ETH'])
    ap.add_argument('--hours', nargs='*', default=None)
    ap.add_argument('--sources', nargs='*', default=None)
    ap.add_argument('--book-mode', default='aggregate', choices=['raw', 'aggregate', 'skip'],
                    help='raw keeps every delta (large, and unanchored so it cannot give a book); '
                         'aggregate keeps activity per 100ms, which is what it can honestly say')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return
    coins = {c: COINS[c] for c in args.coins if c in COINS}
    if not coins:
        raise SystemExit(f'no known coins in {args.coins}; known: {sorted(COINS)}')
    run(args.raw, args.out, coins, args.hours, sources=args.sources, book_mode=args.book_mode)


if __name__ == '__main__':
    main()
