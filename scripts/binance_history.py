#!/usr/bin/env python3
"""Fetch Binance USD-M perpetual history from the public archive. Deterministic and resumable.

The archive at data.binance.vision serves immutable daily and monthly dumps. The REST API is
geo-blocked from here (451); the dumps are not, and they are what the quarter-hour paper uses:
aggregate trades with `transact_time`, `price`, `quantity`, `is_buyer_maker`.

RAW DATA IS IMMUTABLE AND REPRODUCIBLE. Full aggregate-trade history for six assets over a year
is tens of gigabytes, which is not worth storing when every byte of it can be re-fetched from a
fixed URL. So this keeps the small files (klines) as raw, and for aggregate trades it keeps the
MANIFEST -- url, size, sha256 of every file processed -- alongside the derived boundary windows.
Anyone can reproduce the derived data byte-for-byte from the manifest; nobody has to host 40 GB
to do it. A checksum mismatch on re-fetch is a loud failure rather than a silent redownload.

  python3 scripts/binance_history.py --klines --days 90
  python3 scripts/binance_history.py --aggtrades --days 90 --assets BTCUSDT ETHUSDT
"""
import argparse, datetime as dt, hashlib, json, os, sys, urllib.request, zipfile

BASE = 'https://data.binance.vision/data/futures/um/daily'
ASSETS = ['BTCUSDT', 'ETHUSDT', 'XRPUSDT', 'SOLUSDT', 'DOGEUSDT', 'ADAUSDT']


def url_for(kind, asset, day, interval='1m'):
    d = day.strftime('%Y-%m-%d')
    if kind == 'klines':
        return f'{BASE}/klines/{asset}/{interval}/{asset}-{interval}-{d}.zip'
    return f'{BASE}/aggTrades/{asset}/{asset}-aggTrades-{d}.zip'


class Truncated(Exception):
    pass


def fetch(url, dest, timeout=180, verify_zip=True):
    """Download to a temp path, VERIFY, then move.

    Writing to `.part` and renaming stops a half-written file from being mistaken for a complete
    one on the next run, and it is not enough. A connection that closes early gives urllib a
    clean EOF and no exception, so the truncated bytes land as a well-formed-looking file. One
    such file appeared in 540 downloads: a 786,432-byte ETHUSDT day that begins with a valid zip
    header and ends in the middle of a member. It killed the extraction, which was the lucky
    outcome -- a truncation inside the CSV would have silently dropped half a day of trades from
    the sample instead.

    So the length is checked against Content-Length and the archive is opened before the rename.
    """
    tmp = dest + '.part'
    req = urllib.request.Request(url, headers={'User-Agent': 'kalshi-desk-research/1'})
    with urllib.request.urlopen(req, timeout=timeout) as r, open(tmp, 'wb') as f:
        expect = r.headers.get('Content-Length')
        expect = int(expect) if expect and expect.isdigit() else None
        h = hashlib.sha256()
        n = 0
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
            h.update(chunk)
            n += len(chunk)
    if expect is not None and n != expect:
        os.unlink(tmp)
        raise Truncated(f'{n} of {expect} bytes')
    if verify_zip and dest.endswith('.zip'):
        try:
            with zipfile.ZipFile(tmp) as z:
                if z.testzip() is not None:
                    raise zipfile.BadZipFile('member CRC failed')
        except zipfile.BadZipFile as e:
            os.unlink(tmp)
            raise Truncated(f'not a readable zip: {e}')
    os.replace(tmp, dest)
    return n, h.hexdigest()


def verify_tree(root):
    """Re-check everything already on disk. Cheap, and it turns a silent corruption into a list."""
    bad = []
    for dirpath, _, files in os.walk(root):
        for fn in files:
            if not fn.endswith('.zip'):
                continue
            p = os.path.join(dirpath, fn)
            try:
                with zipfile.ZipFile(p) as z:
                    z.namelist()
            except Exception as e:
                bad.append((p, type(e).__name__))
    return bad


def days_back(n, end=None):
    # The archive publishes a day after it closes, so yesterday is the newest reliable file.
    end = end or (dt.date.today() - dt.timedelta(days=2))
    return [end - dt.timedelta(days=i) for i in range(n - 1, -1, -1)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--assets', nargs='*', default=ASSETS)
    ap.add_argument('--days', type=int, default=90)
    ap.add_argument('--end', default='')
    ap.add_argument('--klines', action='store_true')
    ap.add_argument('--aggtrades', action='store_true')
    ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), 'data', 'binance'))
    ap.add_argument('--manifest', default='')
    ap.add_argument('--verify', action='store_true',
                    help='re-check every zip already on disk and delete the broken ones')
    args = ap.parse_args()
    if args.verify:
        bad = verify_tree(args.out)
        for p, e in bad:
            print(f'  BAD {p}: {e}')
            os.unlink(p)
        print(f'verified {args.out}: {len(bad)} broken file(s) removed')
        if not bad:
            return
    end = dt.date.fromisoformat(args.end) if args.end else None
    days = days_back(args.days, end)
    kinds = ([k for k, on in (('klines', args.klines), ('aggTrades', args.aggtrades)) if on]
             or ['klines'])

    man_path = args.manifest or os.path.join(args.out, 'manifest.jsonl')
    os.makedirs(os.path.dirname(man_path), exist_ok=True)
    have = set()
    if os.path.exists(man_path):
        with open(man_path) as f:
            for line in f:
                try:
                    have.add(json.loads(line)['url'])
                except (json.JSONDecodeError, KeyError):
                    continue

    ok = skipped = missing = failed = 0
    total_bytes = 0
    with open(man_path, 'a') as man:
        for kind in kinds:
            for asset in args.assets:
                sub = os.path.join(args.out, kind, asset)
                os.makedirs(sub, exist_ok=True)
                for d in days:
                    u = url_for(kind, asset, d)
                    dest = os.path.join(sub, os.path.basename(u))
                    if u in have and os.path.exists(dest):
                        skipped += 1
                        continue
                    try:
                        n, sha = fetch(u, dest)
                    except Truncated as e:
                        failed += 1
                        print(f'  TRUNCATED {u}: {e}', file=sys.stderr)
                        continue
                    except urllib.error.HTTPError as e:
                        if e.code == 404:
                            missing += 1
                        else:
                            failed += 1
                            print(f'  {e.code} {u}', file=sys.stderr)
                        continue
                    except Exception as e:
                        failed += 1
                        print(f'  ERR {type(e).__name__} {u}', file=sys.stderr)
                        continue
                    man.write(json.dumps({'url': u, 'path': os.path.relpath(dest, args.out),
                                          'bytes': n, 'sha256': sha,
                                          'fetched': dt.datetime.now(dt.timezone.utc)
                                          .isoformat()}) + '\n')
                    man.flush()
                    ok += 1
                    total_bytes += n
                    if ok % 25 == 0:
                        print(f'  {ok} files, {total_bytes/1e6:.0f} MB', file=sys.stderr)
    print(f'fetched {ok}, skipped {skipped}, absent-from-archive {missing}, failed {failed}, '
          f'{total_bytes/1e6:.0f} MB')
    print(f'manifest: {man_path}')


if __name__ == '__main__':
    main()
