#!/usr/bin/env python3
"""Fresh hourly bars for the live signal, with an explicit staleness guard.

THE SIGNAL IS ONLY AS CURRENT AS ITS LAST BAR, AND A STALE BAR IS A SILENT WRONG ANSWER. The
research panel is built from monthly 1h kline archives, which lag by up to a month. A 168-hour
momentum component computed on bars that end three weeks ago is not the frozen signal; it is a
different signal that happens to share its code. So this module assembles the panel from three
sources, newest last, and refuses to return anything whose final bar is older than `max_age_h`.

  1. monthly 1h klines      the bulk of history, through the last complete month
  2. daily 1m klines        aggregated to hourly, through yesterday
  3. the recorder's feed    the current partial day, if prod.sqlite has it

Every layer is optional except the first; whatever is present is used and the coverage is
reported, so the decision file records exactly which bars the trade was based on.
"""
import os, sqlite3, sys, zipfile
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOUR_MS = 3_600_000


def _read_kline_zip(path, out, bucket_ms=HOUR_MS):
    """Accumulate close-by-bucket from one kline zip. Works for 1m or 1h files alike."""
    try:
        with zipfile.ZipFile(path) as z:
            with z.open(z.namelist()[0]) as raw:
                for line in raw:
                    f = line.split(b',')
                    try:
                        t = int(f[0])
                        c = float(f[4])
                    except (ValueError, IndexError):
                        continue            # the header row lands here
                    b = t - (t % bucket_ms)
                    out[b] = c              # files are ordered, so the last write is the close
    except (zipfile.BadZipFile, OSError):
        pass


def closes_for(asset, data_dir=None):
    """Hourly closes keyed by bar-open ms, assembled newest-source-last."""
    data_dir = data_dir or os.path.join(ROOT, 'data', 'binance')
    out = {}
    for sub, bucket in (('klines_1h', HOUR_MS), ('klines', HOUR_MS)):
        d = os.path.join(data_dir, sub, asset)
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if fn.endswith('.zip'):
                _read_kline_zip(os.path.join(d, fn), out, bucket)
    return out


def recorder_topup(assets, db_path=None, since_ms=None):
    """Latest hourly closes from the recorder's own external price feed.

    Best effort by design: if the table is absent or shaped differently, the panel is simply a
    few hours shorter and the staleness guard decides whether that is acceptable. A live trading
    path must never fail closed on an optional data source, but it must also never pretend the
    data is there.
    """
    db_path = db_path or os.path.join(ROOT, 'data', 'prod.sqlite')
    out = {a: {} for a in assets}
    if not os.path.exists(db_path):
        return out, 'no prod.sqlite'
    try:
        db = sqlite3.connect('file:%s?mode=ro' % db_path, uri=True, timeout=20)
        cols = {r[1] for r in db.execute('PRAGMA table_info(ext_prices)')}
        if not cols:
            return out, 'no ext_prices table'
        tcol = 'ts' if 'ts' in cols else ('timestamp' if 'timestamp' in cols else None)
        scol = 'symbol' if 'symbol' in cols else ('ticker' if 'ticker' in cols else None)
        pcol = next((c for c in ('price', 'close', 'last', 'mid') if c in cols), None)
        if not (tcol and scol and pcol):
            return out, 'ext_prices columns are %s' % sorted(cols)
        q = 'SELECT %s,%s,%s FROM ext_prices' % (tcol, scol, pcol)
        args = ()
        if since_ms:
            q += ' WHERE %s >= ?' % tcol
            args = (since_ms / 1000.0,)
        n = 0
        for t, sym, px in db.execute(q, args):
            if px is None or t is None:
                continue
            ms = int(t * 1000) if float(t) < 1e11 else int(t)
            for a in assets:
                if sym and a[:-4].upper() in str(sym).upper():
                    out[a][ms - (ms % HOUR_MS)] = float(px)
                    n += 1
                    break
        return out, 'ext_prices contributed %d observations' % n
    except sqlite3.Error as e:
        return out, 'ext_prices unreadable: %s' % e


def build(universe, data_dir=None, use_recorder=True, max_age_h=None):
    """Aligned hourly log-return panel in bps, plus a provenance record.

    Returns (rets, ts, names, info). `info['stale_hours']` is how old the last common bar is; the
    caller decides what to do with it, but `max_age_h` will raise here rather than let a stale
    panel reach a live order path.
    """
    import time
    closes = {a: closes_for(a, data_dir) for a in universe}
    info = {'source_bars': {a: len(c) for a, c in closes.items()}}
    if use_recorder:
        latest = max((max(c) for c in closes.values() if c), default=0)
        top, note = recorder_topup(universe, since_ms=latest)
        info['recorder'] = note
        added = 0
        for a, d in top.items():
            for k, v in d.items():
                if k > (max(closes[a]) if closes[a] else 0):
                    closes[a][k] = v
                    added += 1
        info['recorder_bars_added'] = added

    closes = {a: c for a, c in closes.items() if len(c) > 2000}
    missing = [a for a in universe if a not in closes]
    if missing:
        info['dropped_for_insufficient_history'] = missing
    grids = [set(c) for c in closes.values()]
    common = sorted(set.intersection(*grids)) if grids else []
    if len(common) < 2000:
        raise SystemExit('only %d aligned hourly bars; refusing to build a signal' % len(common))
    names = sorted(closes)
    px = np.array([[closes[a][t] for a in names] for t in common], dtype=float)
    with np.errstate(divide='ignore', invalid='ignore'):
        rets = np.diff(np.log(px), axis=0) * 1e4
    ts = common[1:]
    ok = np.isfinite(rets).all(axis=1)
    rets = rets[ok]
    ts = [t for t, k in zip(ts, ok) if k]

    now_ms = int(time.time() * 1000)
    info['last_bar_ms'] = ts[-1]
    info['stale_hours'] = (now_ms - ts[-1]) / HOUR_MS
    info['bars'] = len(rets)
    info['names'] = names
    if max_age_h is not None and info['stale_hours'] > max_age_h:
        raise SystemExit('last bar is %.1f hours old (limit %.1f); refusing to trade a stale '
                         'signal' % (info['stale_hours'], max_age_h))
    return rets, np.asarray(ts), names, info


if __name__ == '__main__':
    import json
    uni = sys.argv[1:] or ['AAVEUSDT', 'ADAUSDT', 'AVAXUSDT', 'BCHUSDT', 'BNBUSDT', 'BTCUSDT',
                           'DOGEUSDT', 'ETHUSDT', 'LINKUSDT', 'LTCUSDT', 'NEARUSDT', 'SOLUSDT',
                           'XRPUSDT']
    r, t, n, info = build(uni)
    import datetime as dt
    print(json.dumps({k: v for k, v in info.items() if k != 'source_bars'}, indent=1))
    print('last bar %s UTC, %.1f hours old, %d bars, %d assets'
          % (dt.datetime.utcfromtimestamp(t[-1] / 1000).strftime('%Y-%m-%d %H:%M'),
             info['stale_hours'], len(r), len(n)))
