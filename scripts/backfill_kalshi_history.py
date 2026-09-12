#!/usr/bin/env python3
"""Backfill settled Kalshi markets and their trade history into data/research.sqlite.

Why: on 2026-09-11 the desk held 24,631 settled markets but its `quotes` table only reached back
to 2026-09-05. Six days. Every hypothesis in docs/hypotheses.md that needs a price at a horizon
before settlement was starved not of a source but of time, and waiting is the slowest way to fix
that when the exchange will sell you the past for free.

This pulls, per series: every settled market (outcome, close time, volume, open interest) and
then the public trade prints for it, which give a real executed price at every point in the
market's life rather than a quote the desk happened to be watching.

It writes to a SEPARATE database. Research must never contend with the file the desk trades from,
and that file is already 1.2 GB.

  python3 scripts/backfill_kalshi_history.py --series KXBTC KXETH --max-markets 400
  python3 scripts/backfill_kalshi_history.py --discover Crypto Sports --max-markets 2000

Resumable: markets and trade pages already fetched are skipped, so re-running continues.

NOTE: this runs on a machine that can reach api.elections.kalshi.com. The public endpoints need
no key. The /markets response shape was verified 2026-09-10; the trades shape is handled
defensively across the field names Kalshi has used, and the first page is printed so a mismatch
is visible immediately rather than silently stored as zeros.
"""
import argparse, json, os, sqlite3, sys, time, urllib.error, urllib.parse, urllib.request

BASE = 'https://api.elections.kalshi.com/trade-api/v2'
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SCHEMA = """
create table if not exists hist_markets(
  ticker text primary key, series_ticker text, event_ticker text, title text,
  result text, close_time text, open_time text, expiration_time text,
  volume real, open_interest real, last_price real, strike_type text,
  floor_strike real, cap_strike real, fetched_ts integer);
create index if not exists hist_markets_series on hist_markets(series_ticker, close_time);
create table if not exists hist_trades(
  -- Keyed on (ticker, trade_id), not trade_id alone: nothing documents that a trade id is unique
  -- across markets, and if it is only unique within one, a global key silently drops every trade
  -- whose id repeats in another market.
  ticker text, trade_id text, ts integer, yes_price real, no_price real,
  count real, taker_side text, primary key(ticker, trade_id));
create index if not exists hist_trades_ticker on hist_trades(ticker, ts);
create table if not exists hist_fetch(ticker text primary key, trades integer, done_ts integer);
"""


def get(url, tries=6):
    for i in range(tries):
        try:
            with urllib.request.urlopen(urllib.request.Request(url), timeout=30) as r:
                return json.loads(r.read().decode())
        except urllib.error.HTTPError as e:
            if e.code in (429, 503):
                time.sleep(2 + 3 * i)
                continue
            raise
        except Exception:
            if i == tries - 1:
                raise
            time.sleep(1.5 * (i + 1))
    return {}


def num(d, *keys):
    """Kalshi renamed these fields (yes_bid -> yes_bid_dollars, volume -> volume_fp). Reading a
    name that no longer exists returns None, and treating that as zero is how a live market gets
    recorded as dead. Take the first key that is actually present."""
    for k in keys:
        v = d.get(k)
        if v is not None:
            try:
                return float(v)
            except (TypeError, ValueError):
                pass
    return None


def settled_markets(series, cap):
    out, cursor = [], None
    while len(out) < cap:
        u = f'{BASE}/markets?series_ticker={urllib.parse.quote(series)}&status=settled&limit=200'
        if cursor:
            u += '&cursor=' + urllib.parse.quote(cursor)
        j = get(u)
        page = j.get('markets') or []
        out += page
        cursor = j.get('cursor')
        if not page or not cursor:
            break
        time.sleep(0.3)
    return out[:cap]


def trades_for(ticker, verbose=False):
    out, cursor, pages = [], None, 0
    while pages < 40:
        u = f'{BASE}/markets/trades?ticker={urllib.parse.quote(ticker)}&limit=1000'
        if cursor:
            u += '&cursor=' + urllib.parse.quote(cursor)
        j = get(u)
        page = j.get('trades') or []
        if verbose and pages == 0 and page:
            print('  first trade record: ' + json.dumps(page[0])[:220], flush=True)
        out += page
        cursor = j.get('cursor')
        pages += 1
        if not page or not cursor:
            break
        time.sleep(0.25)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--db', default=os.path.join(ROOT, 'data', 'research.sqlite'))
    ap.add_argument('--series', nargs='*', default=[])
    ap.add_argument('--discover', nargs='*', default=[], help='pull every series in these categories')
    ap.add_argument('--max-markets', type=int, default=500, help='per series')
    ap.add_argument('--no-trades', action='store_true', help='markets only; much faster')
    a = ap.parse_args()

    series = list(a.series)
    for cat in a.discover:
        j = get(f'{BASE}/series?category={urllib.parse.quote(cat)}')
        series += [s['ticker'] for s in (j.get('series') or [])]
        time.sleep(0.4)
    series = sorted(set(series))
    if not series:
        sys.exit('give --series TICKER... or --discover Category...')

    os.makedirs(os.path.dirname(a.db), exist_ok=True)
    db = sqlite3.connect(a.db, timeout=60)
    db.executescript(SCHEMA)
    db.commit()
    done = {r[0] for r in db.execute('select ticker from hist_fetch')}

    print(f'{len(series)} series, up to {a.max_markets} settled markets each -> {a.db}', flush=True)
    first = True
    for s in series:
        try:
            ms = settled_markets(s, a.max_markets)
        except Exception as e:
            print(f'  {s:22} markets failed: {str(e)[:70]}', flush=True)
            continue
        if not ms:
            continue
        rows = [(m.get('ticker'), s, m.get('event_ticker'), m.get('title'), m.get('result'),
                 m.get('close_time'), m.get('open_time'), m.get('expiration_time'),
                 num(m, 'volume_fp', 'volume'), num(m, 'open_interest_fp', 'open_interest'),
                 num(m, 'last_price_dollars', 'last_price'), m.get('strike_type'),
                 num(m, 'floor_strike'), num(m, 'cap_strike'), int(time.time())) for m in ms]
        db.executemany('insert or replace into hist_markets values (' + ','.join('?' * 15) + ')', rows)
        db.commit()
        n_tr = 0
        if not a.no_trades:
            for m in ms:
                t = m.get('ticker')
                if not t or t in done:
                    continue
                try:
                    tr = trades_for(t, verbose=first)
                    first = False
                except Exception as e:
                    print(f'    {t}: trades failed {str(e)[:50]}', flush=True)
                    continue
                trows = []
                for x in tr:
                    ts = x.get('created_time') or x.get('ts')
                    if isinstance(ts, str):
                        try:
                            ts = int(time.mktime(time.strptime(ts[:19], '%Y-%m-%dT%H:%M:%S')))
                        except ValueError:
                            ts = None
                    trows.append((t, x.get('trade_id') or f"{ts}:{x.get('count')}:{x.get('taker_side')}", ts,
                                  num(x, 'yes_price_dollars', 'yes_price'),
                                  num(x, 'no_price_dollars', 'no_price'),
                                  num(x, 'count_fp', 'count'), x.get('taker_side')))
                if trows:
                    db.executemany('insert or replace into hist_trades values (?,?,?,?,?,?,?)', trows)
                db.execute('insert or replace into hist_fetch values (?,?,?)', (t, len(trows), int(time.time())))
                db.commit()
                n_tr += len(trows)
                done.add(t)
        print(f'  {s:22} {len(ms):>5} settled markets, {n_tr:>7,} trades', flush=True)

    m = db.execute('select count(*) from hist_markets').fetchone()[0]
    t = db.execute('select count(*) from hist_trades').fetchone()[0]
    print(f'\n{m:,} settled markets, {t:,} trades in {a.db}')
    print('Sanity-check before trusting it: a settled market should have a nonzero result and')
    print('trades whose prices sit in (0,1). If prices come back as 0 or 100, the field names')
    print('changed again and num() needs the new one.')


if __name__ == '__main__':
    main()
