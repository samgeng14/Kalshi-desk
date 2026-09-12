#!/usr/bin/env python3
"""Monthly 1-hour klines, back to the start of each symbol's history.

WHY. The daily 1m archive this project used covers 400 days, which gives a 160-bar held-out
slice at 12-hour bars and 80 day-clusters for the block bootstrap. That is not enough to tell a
12-hour directional signal from the drift of a bull sample: the decisive gate -- does the signal
beat the better of always-long and always-short -- is failed by a pure-noise sweep 95% of the
time at that length, so failing it says nothing. The monthly 1h files go back to 2020 and weigh
about 33 KB each, so five to six years of history costs roughly 30 MB.

Same discipline as binance_history.py: Content-Length checked, testzip() run, manifest of
url/bytes/sha256, atomic rename, resumable.
"""
import argparse, hashlib, json, os, sys, time, urllib.error, urllib.request, zipfile

BASE = 'https://data.binance.vision/data/futures/um/monthly/klines/{s}/{iv}/{s}-{iv}-{m}.zip'


class Truncated(Exception):
    pass


def months(start, end):
    y, m = int(start[:4]), int(start[5:7])
    ey, em = int(end[:4]), int(end[5:7])
    while (y, m) <= (ey, em):
        yield '%04d-%02d' % (y, m)
        m += 1
        if m == 13:
            y, m = y + 1, 1


def fetch(url, dest, tries=4):
    """Download once, verify twice, rename last.

    The archive drops connections under a fast loop -- RemoteDisconnected, which is an OSError and
    not a urllib error, so an earlier version of this let it escape and kill the run mid-asset.
    Retries with backoff, and lets a 404 through immediately because that is an answer, not a
    failure: a month before the symbol listed does not exist.
    """
    tmp = dest + '.part'
    req = urllib.request.Request(url, headers={'User-Agent': 'kalshi-desk/1.0'})
    last = None
    for attempt in range(tries):
        try:
            with urllib.request.urlopen(req, timeout=90) as r:
                declared = r.headers.get('Content-Length')
                data = r.read()
            break
        except urllib.error.HTTPError:
            raise
        except (urllib.error.URLError, OSError) as e:
            last = e
            time.sleep(1.5 * (attempt + 1))
    else:
        raise Truncated('gave up after %d attempts: %s' % (tries, last))
    if declared is not None and len(data) != int(declared):
        raise Truncated('got %d bytes, Content-Length said %s' % (len(data), declared))
    with open(tmp, 'wb') as f:
        f.write(data)
    try:
        with zipfile.ZipFile(tmp) as z:
            bad = z.testzip()
            if bad is not None:
                raise Truncated('corrupt member %s' % bad)
    except zipfile.BadZipFile as e:
        os.unlink(tmp)
        raise Truncated('not a zip: %s' % e)
    os.replace(tmp, dest)       # only a verified file ever appears under the real name
    return len(data), hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'data', 'binance'))
    ap.add_argument('--assets', nargs='+', required=True)
    ap.add_argument('--start', default='2020-01')
    ap.add_argument('--end', required=True)
    ap.add_argument('--interval', default='1h')
    args = ap.parse_args()

    root = os.path.join(args.data, 'klines_' + args.interval)
    man = os.path.join(args.data, 'binance_monthly_manifest.jsonl')
    have = set()
    if os.path.exists(man):
        for ln in open(man):
            try:
                have.add(json.loads(ln)['path'])
            except (ValueError, KeyError):
                pass

    got = miss = skip = 0
    with open(man, 'a') as mf:
        for a in args.assets:
            d = os.path.join(root, a)
            os.makedirs(d, exist_ok=True)
            first = None
            for m in months(args.start, args.end):
                rel = '%s/%s-%s-%s.zip' % (a, a, args.interval, m)
                dest = os.path.join(root, rel)
                if os.path.exists(dest) and rel in have:
                    skip += 1
                    first = first or m
                    continue
                url = BASE.format(s=a, iv=args.interval, m=m)
                try:
                    n, sha = fetch(url, dest)
                except urllib.error.HTTPError as e:
                    if e.code == 404:
                        miss += 1          # before the symbol listed, or after the last close
                        continue
                    print('  %s %s HTTP %s' % (a, m, e.code), file=sys.stderr)
                    continue
                except (Truncated, urllib.error.URLError, OSError) as e:
                    print('  %s %s %s' % (a, m, e), file=sys.stderr)
                    continue
                mf.write(json.dumps({'url': url, 'path': rel, 'bytes': n, 'sha256': sha,
                                     'fetched': time.strftime('%Y-%m-%dT%H:%M:%SZ',
                                                              time.gmtime())}) + '\n')
                mf.flush()
                got += 1
                first = first or m
                time.sleep(0.05)          # be a polite client; the archive drops fast loops
            have_n = len([f for f in os.listdir(d) if f.endswith('.zip')])
            print('  %-10s %3d files, from %s' % (a, have_n, first or '-'))
    print('fetched %d, already had %d, absent upstream %d' % (got, skip, miss))


if __name__ == '__main__':
    main()
