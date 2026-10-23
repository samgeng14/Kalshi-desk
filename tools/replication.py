#!/usr/bin/env python3
"""Does Kalshi's pyth_value reproduce each 15-minute window's strike? The anchor gate, from the
recorder's data (plan section 5a: "the VM records pyth_value and strikes from week 0 for
tools/replication.py").

A KX...15M window's strike is the one-minute Pyth candle close at its open. deskd checks it live
(strat/fair.cpp): the last pyth_value in the minute before the open, against the window's
floor_strike, to one unit in the strike's last stated decimal. The plan lets LIP-15 use the model
as its anchor only after 50 windows in a row reproduce. deskd counts only windows that open while
it is running; this counts the same thing over everything the recorder kept, with the same rules:

  - the symbol: a pattern that is a recorded underlying_ticker exactly, else the first recorded
    symbol containing a pattern (case-insensitive), in the order of deskd's table
  - the close: the last value in [open - 60 s, open), at source_ts_ms (recv time when absent)
  - a match: |close - strike| <= 10^-d (+1e-12), d the series' stated decimals, else the strike's
  - a window with no value in that minute is not counted (as deskd does); a mismatch resets the run

Input: the recorder's data directory (<data>/spool/<table>/*.jsonl, and <data>/parquet/<table>/**
when pyarrow is installed): kalshi_ws rows of type pyth_value, kalshi_markets rows (the strikes).
Read-only, unless --write-replication: it then writes deskd's replication file (data_dir/
replication.json, strat/fair.h) from these counts, never over an existing one without --force.

Usage:
  python3 tools/replication.py --data DIR [--json] [--write-replication FILE [--force]]
  python3 tools/replication.py --self-test
"""
import argparse
import glob
import json
import math
import os
import re
import sys
import tempfile
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GATE = 50

# deskd's table (src/deskd/strat/fair.cpp financial_underlyings): key, series, symbol patterns
# (exact names first), the strike's stated decimals (-1: read off the strike). The self-test
# checks this copy against the C++ one.
UNDERLYINGS = [
    ('gold', 'KXGOLD15M', ['Metal.Index.GOLD/USD', 'gold', 'xau'], 2),
    ('silver', 'KXSILVER15M', ['Metal.Index.SILVER/USD', 'silver', 'xag'], 3),
    ('wti', 'KXWTI15M', ['Commodities.Index.PYTHOIL/USD', 'pythoil', 'wti'], 2),
    ('natgas', 'KXNATGAS15M', ['Commodities.Index.NATGAS/USD', 'natgas'], 5),
    ('copper', 'KXCOPPER15M', ['Commodities.Index.CU/USD', 'copper'], 5),
    ('platinum', 'KXPLATINUM15M', ['Metal.XPT/USD', 'xpt', 'platinum'], -1),
    ('palladium', 'KXPALLADIUM15M', ['Metal.XPD/USD', 'xpd', 'palladium'], -1),
    ('eurusd', 'KXEURUSD15M', ['FX.EUR/USD', 'EUR/USD'], -1),
    ('gbpusd', 'KXGBPUSD15M', ['FX.GBP/USD', 'GBP/USD'], -1),
    ('usdjpy', 'KXUSDJPY15M', ['FX.USD/JPY', 'USD/JPY'], -1),
]


def decimals_of(v):
    s = ('%.8f' % v).rstrip('0')
    return len(s) - s.index('.') - 1 if '.' in s else 0


def same_strike(close, strike, decimals):
    if not (math.isfinite(close) and math.isfinite(strike)) or close <= 0 or strike <= 0:
        return False
    d = decimals if decimals >= 0 else decimals_of(strike)
    return abs(close - strike) <= 10.0 ** -d + 1e-12


def symbol_for(symbols, patterns):
    for p in patterns:
        if p in symbols:
            return p
    for p in patterns:
        want = p.lower()
        for s in sorted(symbols):
            if want in s.lower():
                return s
    return ''


def minute_close(ticks, open_ms):
    """ticks: [(ms, value)] oldest first; the last value in [open - 60 s, open)."""
    out = None
    for t, v in ticks:
        if v <= 0:
            continue
        if t >= open_ms:
            break
        if t >= open_ms - 60000:
            out = v
    return out


def ts_ms(text):
    if not text:
        return None
    try:
        return int(datetime.fromisoformat(text.replace('Z', '+00:00')).timestamp() * 1000)
    except ValueError:
        return None


# ---- reading the recorder's tables ----------------------------------------------------------
def rows(data, table):
    """Every row of one table: the spool's JSON lines, then the Parquet files (with pyarrow)."""
    for path in sorted(glob.glob(os.path.join(data, 'spool', table, '*.jsonl'))):
        with open(path, encoding='utf-8') as fh:
            for line in fh:
                try:
                    yield json.loads(line)
                except ValueError:
                    continue   # a torn last line (the recorder counts it as a gap)
    files = sorted(glob.glob(os.path.join(data, 'parquet', table, '**', '*.parquet'), recursive=True))
    if not files:
        return
    try:
        import pyarrow.parquet as pq
    except ImportError:
        print('warning: %d Parquet file(s) of %s skipped: pyarrow is not installed' % (len(files), table),
              file=sys.stderr)
        return
    for path in files:
        for r in pq.read_table(path).to_pylist():
            yield r


def pyth_ticks(data):
    """symbol -> [(ms, value)] sorted, from kalshi_ws's pyth_value frames."""
    out = {}
    for r in rows(data, 'kalshi_ws'):
        if r.get('type') != 'pyth_value':
            continue
        try:
            msg = json.loads(r.get('raw') or '{}').get('msg') or {}
        except ValueError:
            continue
        sym, v = msg.get('underlying_ticker'), msg.get('value_usd')
        try:
            v = float(v)
        except (TypeError, ValueError):
            continue
        if not isinstance(sym, str) or not sym or not math.isfinite(v) or v <= 0:
            continue
        t = msg.get('source_ts_ms')
        t = int(t) if isinstance(t, (int, float)) and t > 0 else int((r.get('recv_us') or 0) // 1000)
        out.setdefault(sym, []).append((t, v))
    for ticks in out.values():
        ticks.sort()
    return out


def windows(data):
    """ticker -> (series, open_ms, floor_strike), the last recorded row with both."""
    out = {}
    for r in rows(data, 'kalshi_markets'):
        tk, strike, open_ms = r.get('ticker'), r.get('floor_strike'), ts_ms(r.get('open_time'))
        if not tk or open_ms is None or strike is None:
            continue
        try:
            strike = float(strike)
        except (TypeError, ValueError):
            continue
        out[tk] = (r.get('series') or tk.split('-')[0], open_ms, strike)
    return out


# ---- the count ------------------------------------------------------------------------------
def replicate(ticks, wins):
    """Per underlying: checked, matched, consecutive (the run up to the last window), longest run,
    and the last window's detail; windows in open order."""
    symbols = set(ticks)
    result = {}
    for key, series, patterns, decimals in UNDERLYINGS:
        mine = sorted((o, tk, s) for tk, (ser, o, s) in wins.items() if ser == series)
        if not mine:
            continue
        sym = symbol_for(symbols, patterns)
        r = {'symbol': sym, 'windows': len(mine), 'checked': 0, 'matched': 0, 'consecutive': 0, 'longest': 0,
             'last_open_ms': 0, 'last_strike': 0.0, 'last_close': 0.0, 'last_ok': False, 'misses': []}
        for open_ms, tk, strike in mine:
            close = minute_close(ticks.get(sym, []), open_ms) if sym else None
            if close is None:
                continue   # no value in the minute before the open: not counted
            ok = same_strike(close, strike, decimals)
            r['checked'] += 1
            if ok:
                r['matched'] += 1
                r['consecutive'] += 1
                r['longest'] = max(r['longest'], r['consecutive'])
            else:
                r['consecutive'] = 0
                r['misses'].append({'ticker': tk, 'strike': strike, 'close': close})
            r.update(last_open_ms=open_ms, last_strike=strike, last_close=close, last_ok=ok)
        result[key] = r
    return result


def replication_file(result):
    """deskd's data_dir/replication.json (strat/fair.cpp FinancialFair::persist)."""
    return {'v': 1, 'replication': {k: {f: r[f] for f in ('checked', 'matched', 'consecutive', 'last_open_ms',
                                                         'last_strike', 'last_close', 'last_ok')}
                                     for k, r in result.items() if r['checked']}}


def report(result):
    lines = ['%-10s %-32s %7s %7s %7s %11s %7s  %s' % ('underlying', 'symbol', 'windows', 'checked', 'matched',
                                                       'consecutive', 'longest', 'gate')]
    for k, r in result.items():
        lines.append('%-10s %-32s %7d %7d %7d %11d %7d  %s' % (
            k, r['symbol'] or '(not recorded)', r['windows'], r['checked'], r['matched'], r['consecutive'],
            r['longest'], 'OPEN' if r['consecutive'] >= GATE else 'shut'))
        for m in r['misses'][-3:]:
            lines.append('    miss %s: strike %s, the feed closed at %s' % (m['ticker'], m['strike'], m['close']))
    return '\n'.join(lines)


def run(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--data', help="the recorder's data directory")
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--write-replication', metavar='FILE', help="write deskd's replication.json from the counts")
    ap.add_argument('--force', action='store_true', help='replace an existing --write-replication file')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.data or not os.path.isdir(a.data):
        print('error: --data DIR (the recorder\'s data directory) is required', file=sys.stderr)
        return 2
    result = replicate(pyth_ticks(a.data), windows(a.data))
    print(json.dumps(result, indent=1) if a.json else report(result))
    if a.write_replication:
        if os.path.exists(a.write_replication) and not a.force:
            print('error: %s exists; --force replaces it' % a.write_replication, file=sys.stderr)
            return 2
        tmp = a.write_replication + '.tmp'
        with open(tmp, 'w') as fh:
            json.dump(replication_file(result), fh)
        os.replace(tmp, a.write_replication)
        print('wrote %s' % a.write_replication)
    return 0


# ---- self-test ------------------------------------------------------------------------------
def self_test():
    fails = []

    def check(name, ok, detail=''):
        print('%s %s%s' % ('OK  ' if ok else 'FAIL', name, (': ' + str(detail)) if detail else ''))
        if not ok:
            fails.append(name)

    # the Python table is deskd's
    src = open(os.path.join(ROOT, 'src', 'deskd', 'strat', 'fair.cpp'), encoding='utf-8').read()
    cpp = []
    for m in re.finditer(r'\{"(\w+)", "(\w+)", \{([^}]*)\}, (-?\d+)\}', src):
        pats = re.findall(r'"([^"]*)"', m.group(3))
        cpp.append((m.group(1), m.group(2), pats, int(m.group(4))))
    check("the underlyings are deskd's (strat/fair.cpp)", cpp == UNDERLYINGS, cpp if cpp != UNDERLYINGS else '')

    # deskd's strike rule
    check('same_strike: within one unit of the last stated decimal', same_strike(2650.504, 2650.50, 2))
    check('same_strike: past it', not same_strike(2650.52, 2650.50, 2))
    check('same_strike: decimals read off the strike', same_strike(1.08455, 1.0845, -1) and not same_strike(1.0847, 1.0845, -1))
    check('symbol_for: exact first, then a substring', symbol_for({'Metal.Index.GOLD/USD', 'xgold'}, ['Metal.Index.GOLD/USD', 'gold'])
          == 'Metal.Index.GOLD/USD' and symbol_for({'Crypto.GOLD.x'}, ['Metal.Index.GOLD/USD', 'gold']) == 'Crypto.GOLD.x')
    check('minute_close: the last value in the minute before', minute_close([(1000, 1.0), (59000, 2.0), (60000, 3.0)], 60000) == 2.0
          and minute_close([(100, 1.0)], 120000) is None)

    # a recorded day: gold windows every 15 minutes; strikes the feed's close (to 2 places), one
    # off by 3 cents, one with no value in its minute; plus spool lines in the recorder's shape
    t0 = int(datetime(2026, 10, 20, 0, 0, tzinfo=timezone.utc).timestamp() * 1000)
    sym = 'Metal.Index.GOLD/USD'
    ticks, price = [], 2650.0
    for i in range(0, 6 * 3600, 5):   # every 5 s for six hours
        price += math.sin(i / 300.0) * 0.07
        ticks.append((t0 + i * 1000, round(price, 4)))
    gap_open = t0 + 4 * 3600 * 1000   # no values in the minute before this one
    ticks = [(t, v) for t, v in ticks if not (gap_open - 70000 <= t < gap_open)]
    wins = {}
    for w in range(1, 24):
        o = t0 + w * 900000
        close = minute_close(ticks, o)
        strike = round(close, 2) if close is not None else 2650.0
        if w == 15:
            strike = round(close + 0.03, 2)
        wins['KXGOLD15M-26OCT20%04d-%02d' % (w, w)] = ('KXGOLD15M', o, strike)
    r = replicate({sym: ticks}, wins)['gold']
    check('windows counted: the gap one is not', r['checked'] == 22 and r['windows'] == 23, r)
    # windows 1-14 match, 15 misses, 16 has no value in its minute, 17-23 match
    check('a 3-cent miss resets the run', r['matched'] == 21 and r['longest'] == 14 and r['consecutive'] == 7, r)
    check('the miss is named', len(r['misses']) == 1 and r['misses'][0]['ticker'].endswith('0015-15'), r['misses'])
    rf = replication_file({'gold': r})
    check("deskd's file shape", rf['v'] == 1 and set(rf['replication']['gold']) ==
          {'checked', 'matched', 'consecutive', 'last_open_ms', 'last_strike', 'last_close', 'last_ok'})

    # the CLI over spool files, and the file it writes
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, 'spool', 'kalshi_ws'))
        os.makedirs(os.path.join(d, 'spool', 'kalshi_markets'))
        with open(os.path.join(d, 'spool', 'kalshi_ws', '2026102000.jsonl'), 'w') as fh:
            for t, v in ticks:
                raw = {'type': 'pyth_value', 'msg': {'underlying_ticker': sym, 'value_usd': str(v), 'source_ts_ms': t}}
                fh.write(json.dumps({'recv_us': (t + 300) * 1000, 'type': 'pyth_value', 'raw': json.dumps(raw)}) + '\n')
            fh.write('{"recv_us": 1, "type": "pyth_va')   # a torn last line
        with open(os.path.join(d, 'spool', 'kalshi_markets', '2026102000.jsonl'), 'w') as fh:
            for tk, (ser, o, s) in wins.items():
                iso = datetime.fromtimestamp(o / 1000, tz=timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
                fh.write(json.dumps({'series': ser, 'ticker': tk, 'open_time': iso, 'floor_strike': s}) + '\n')
        out = os.path.join(d, 'replication.json')
        check('the CLI runs', run(['--data', d, '--write-replication', out]) == 0)
        got = json.load(open(out))['replication']['gold']
        check('it reads the spool as replicate() does', got['checked'] == 22 and got['consecutive'] == 7, got)
        check('an existing file is not replaced without --force', run(['--data', d, '--write-replication', out]) == 2)
    print('replication self-test: %s' % ('ok' if not fails else 'FAILED (%d)' % len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(run(sys.argv[1:]))
