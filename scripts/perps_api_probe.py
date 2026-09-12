#!/usr/bin/env python3
"""Validate the margin endpoints against production and write down what actually came back.

The /margin/* paths used here were read from docs.kalshi.com/llms.txt on 2026-09-11, but a
documented endpoint is a claim and a production response is evidence. Undocumented or
recently-documented behaviour can change without notice, so this records the observed truth --
status, schema, pagination, real timestamp ranges, rate-limit headers -- and writes it to
research/margin_api_empirical_spec.md. Run it before the backfill and again whenever something
starts failing; a diff between two runs is the fastest way to see what the exchange changed.

Nothing here is fatal. An endpoint that 404s is recorded as absent and the probe continues, so a
removed endpoint produces a report rather than a stack trace.

  python3 scripts/perps_api_probe.py
  python3 scripts/perps_api_probe.py --ticker KXBTCPERP
"""
import argparse, base64, json, os, sys, time, urllib.error, urllib.parse, urllib.request
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PREFIX = '/trade-api/v2'
MAXLEN = 1200


def load_config():
    raw = open(os.path.join(ROOT, 'config', 'desk.json')).read()
    out, i, instr, esc = [], 0, False, False
    while i < len(raw):
        ch = raw[i]
        if instr:
            out.append(ch)
            if esc: esc = False
            elif ch == '\\': esc = True
            elif ch == '"': instr = False
            i += 1; continue
        if ch == '"': instr = True; out.append(ch); i += 1; continue
        if ch == '/' and i + 1 < len(raw) and raw[i + 1] == '/':
            while i < len(raw) and raw[i] != '\n': i += 1
            continue
        out.append(ch); i += 1
    cfg = json.loads(''.join(out))
    host = ('https://external-api.kalshi.com' if cfg.get('env') == 'prod'
            else 'https://external-api.demo.kalshi.co')
    return cfg['key_id'], os.path.expanduser(cfg['pem_path']), host + PREFIX


def make_signer(key_id, pem_path):
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import padding
    except ImportError:
        sys.exit("need `pip install cryptography`")
    key = serialization.load_pem_private_key(open(pem_path, 'rb').read(), password=None)

    def headers(method, path):
        ts = str(int(time.time() * 1000))
        sig = key.sign((ts + method + path).encode(),
                       padding.PSS(mgf=padding.MGF1(hashes.SHA256()),
                                   salt_length=hashes.SHA256.digest_size),
                       hashes.SHA256())
        return {'KALSHI-ACCESS-KEY': key_id, 'KALSHI-ACCESS-TIMESTAMP': ts,
                'KALSHI-ACCESS-SIGNATURE': base64.b64encode(sig).decode(),
                'accept': 'application/json'}
    return headers


def schema_of(v, depth=0):
    """A compact type sketch. Lists collapse to their first element: the shape is the point."""
    if depth > 4: return '...'
    if isinstance(v, dict):
        return {k: schema_of(x, depth + 1) for k, x in list(v.items())[:40]}
    if isinstance(v, list):
        return [schema_of(v[0], depth + 1)] if v else []
    if isinstance(v, bool): return 'bool'
    if isinstance(v, int): return 'int'
    if isinstance(v, float): return 'float'
    if v is None: return 'null'
    s = str(v)
    # Distinguishing a numeric string from a free string matters here: Kalshi sends prices and
    # fixed-point counts as strings, and treating them as floats silently loses precision.
    try:
        float(s); return 'str(numeric)'
    except ValueError:
        pass
    return 'str(iso8601)' if len(s) > 15 and s[4:5] == '-' and 'T' in s else 'str'


SENSITIVE = ('member_id', 'user_id', 'subaccount', 'client_order_id', 'order_id', 'trade_id',
             'fill_id', 'account', 'email')


def sanitize(v, depth=0):
    if depth > 4: return '...'
    if isinstance(v, dict):
        return {k: ('<redacted>' if any(s in k.lower() for s in SENSITIVE) else sanitize(x, depth + 1))
                for k, x in list(v.items())[:40]}
    if isinstance(v, list):
        return [sanitize(x, depth + 1) for x in v[:2]]
    return v


def probe(base, hdrs, method, path, params=None, note=''):
    url = base + path + (('?' + urllib.parse.urlencode(params)) if params else '')
    rec = {'path': path, 'params': params or {}, 'note': note, 'url_shown': path,
           'status': None, 'error': '', 'schema': None, 'sample': None, 'rate_headers': {},
           'count': None, 'cursor': None, 'elapsed_ms': None}
    t0 = time.time()
    try:
        req = urllib.request.Request(url, headers=hdrs(method, PREFIX + path))
        with urllib.request.urlopen(req, timeout=40) as r:
            rec['status'] = r.status
            body = json.loads(r.read().decode())
            rec['rate_headers'] = {k: v for k, v in r.headers.items()
                                   if 'ratelimit' in k.lower() or 'retry' in k.lower()
                                   or 'x-request' in k.lower()}
    except urllib.error.HTTPError as e:
        rec['status'] = e.code
        rec['error'] = e.read().decode()[:300]
        rec['elapsed_ms'] = int((time.time() - t0) * 1000)
        return rec, None
    except Exception as e:
        rec['status'] = 0
        rec['error'] = f"{type(e).__name__}: {e}"[:300]
        rec['elapsed_ms'] = int((time.time() - t0) * 1000)
        return rec, None
    rec['elapsed_ms'] = int((time.time() - t0) * 1000)
    rec['schema'] = schema_of(body)
    rec['sample'] = sanitize(body)
    rec['cursor'] = body.get('cursor') if isinstance(body, dict) else None
    if isinstance(body, dict):
        for k, v in body.items():
            if isinstance(v, list):
                rec['count'] = len(v); rec['list_key'] = k; break
    return rec, body


def fmt(o, limit=MAXLEN):
    s = json.dumps(o, indent=2, default=str)
    return s if len(s) <= limit else s[:limit] + "\n  ... truncated"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ticker', default=None, help='probe with this market; default = first active')
    ap.add_argument('--out', default=os.path.join(ROOT, 'research', 'margin_api_empirical_spec.md'))
    a = ap.parse_args()

    key_id, pem, base = load_config()
    hdrs = make_signer(key_id, pem)
    now = int(time.time())
    results = []

    print("probing /margin/markets ...")
    rec, body = probe(base, hdrs, 'GET', '/margin/markets', {'status': 'active'},
                      'the market list; also the source of mark, reference, leverage, contract_size')
    results.append(rec)
    markets = (body or {}).get('markets') or (body or {}).get('margin_markets') or []
    ticker = a.ticker or (markets[0].get('ticker') if markets else 'KXBTCPERP')
    print(f"  status {rec['status']}, {len(markets)} markets, probing with {ticker}")

    plan = [
        ('GET', '/margin/enabled', None, 'documented production-access check'),
        ('GET', '/margin/exchange_status', None, 'trading hours / maintenance'),
        ('GET', '/margin/account_api_limits', None, 'this account rate-limit tier'),
        ('GET', '/margin/risk_parameters', None, 'initial-margin multiplier, both liquidation thresholds'),
        ('GET', '/margin/fee_tier_rates', None, 'ACTUAL maker/taker rates for this account'),
        ('GET', '/margin/fee_tiers', None, 'tier table'),
        ('GET', f'/margin/markets/{ticker}', None, 'single market'),
        ('GET', f'/margin/markets/{ticker}/orderbook', {'depth': 10}, 'REST book snapshot'),
        ('GET', f'/margin/markets/{ticker}/candlesticks',
         {'start_ts': now - 3600, 'end_ts': now, 'period_interval': 1},
         'THE historical bar endpoint; check bid/ask/price OHLC are all present'),
        ('GET', '/margin/trades', {'ticker': ticker, 'limit': 5},
         'historical prints; check taker_side and cursor'),
        ('GET', '/margin/funding_rates/historical', {'start_ts': now - 7 * 86400, 'end_ts': now},
         'funding history'),
        ('GET', '/margin/funding_rates/estimate', {'ticker': ticker}, 'next funding estimate'),
        ('GET', '/margin/balance', {'compute_available_balance': 'true'}, 'account equity'),
        ('GET', '/margin/positions', None, 'open positions'),
        ('GET', '/margin/risk', None, 'per-position liquidation prices'),
    ]
    bodies = {}
    for method, path, params, note in plan:
        print(f"probing {path} ...")
        rec, body = probe(base, hdrs, method, path, params, note)
        results.append(rec); bodies[path] = body
        print(f"  {rec['status']}  {rec.get('count', '')} rows  {rec['elapsed_ms']}ms"
              + (f"  ERROR {rec['error'][:90]}" if rec['error'] else ""))
        time.sleep(0.2)

    # How far back do the bars actually go? Measured PER TICKER by binary search, because history
    # here is a per-contract fact and not a venue-wide one: Kalshi launched perps in late May 2026
    # with the first crypto contracts in early June, while gold and silver listed 2026-09-10.
    # Probing whichever ticker happens to sort first would answer the question for that contract
    # and quietly imply it about all of them.
    print("measuring candlestick history depth per ticker (binary search) ...")

    def has_data(tk, days_ago):
        s, e = now - days_ago * 86400, now - (days_ago - 1) * 86400
        _, b = probe(base, hdrs, 'GET', f'/margin/markets/{tk}/candlesticks',
                     {'start_ts': s, 'end_ts': e, 'period_interval': 60})
        return len((b or {}).get('candlesticks') or (b or {}).get('margin_candlesticks') or []) > 0

    CAP = 400   # Kalshi launched perps in late May 2026, so this is far beyond any real answer

    def first_day(tk):
        if not has_data(tk, 1): return 0
        lo, hi = 1, 2
        while hi <= CAP and has_data(tk, hi):       # double until it runs out
            lo, hi = hi, hi * 2
        if hi > CAP:
            # Doubling overshoots: 256 then 512. Returning CAP here would report 400 days for a
            # contract with 365, so test the cap itself and bisect below it instead of guessing.
            if has_data(tk, CAP): return CAP
            hi = CAP
        while lo + 1 < hi:                          # then bisect the boundary
            mid = (lo + hi) // 2
            if has_data(tk, mid): lo = mid
            else: hi = mid
        return lo

    depth = []
    probe_tickers = [m.get('ticker') for m in markets if m.get('ticker')]
    for tk in probe_tickers:
        d = first_day(tk)
        depth.append((tk, d))
        print(f"  {tk:14s} {d:4d} days of hourly history")
    depth.sort(key=lambda r: -r[1])
    earliest = depth[0][1] if depth else 0
    depth_note = depth

    # Pagination: does the cursor actually advance, or loop?
    print("checking /margin/trades pagination ...")
    pag = {'pages': 0, 'ids': 0, 'unique': 0, 'looped': False}
    seen, cursor = set(), None
    for _ in range(3):
        q = {'ticker': ticker, 'limit': 100}
        if cursor: q['cursor'] = cursor
        _, b = probe(base, hdrs, 'GET', '/margin/trades', q)
        tr = (b or {}).get('trades') or []
        if not tr: break
        before = len(seen)
        seen.update(x.get('trade_id') for x in tr)
        pag['pages'] += 1; pag['ids'] += len(tr)
        if len(seen) == before: pag['looped'] = True; break
        cursor = (b or {}).get('cursor')
        if not cursor: break
        time.sleep(0.2)
    pag['unique'] = len(seen)

    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, 'w') as f:
        w = f.write
        w("# Margin API: what production actually returns\n\n")
        w(f"Probed {datetime.now(timezone.utc):%Y-%m-%d %H:%M} UTC against `{base}` "
          f"with `{ticker}`.\n\n")
        w("The `/margin/*` paths were read from Kalshi's documentation index, but a documented\n"
          "endpoint is a claim and a response is evidence. This file is the evidence. Re-run\n"
          "`scripts/perps_api_probe.py` and diff when something breaks.\n\n")
        ok = [r for r in results if r['status'] == 200]
        bad = [r for r in results if r['status'] != 200]
        w(f"**{len(ok)} of {len(results)} endpoints returned 200.**\n\n")
        if bad:
            w("## Not available\n\n| endpoint | status | error |\n|---|---|---|\n")
            for r in bad:
                # hoisted out of the f-string: a backslash inside an f-string expression is a
                # syntax error before Python 3.12, and this has to run on whatever is installed
                msg = r['error'][:110].replace('|', r'\|')
                w(f"| `{r['path']}` | {r['status']} | {msg} |\n")
            w("\n")
        w("## Summary\n\n| endpoint | status | rows | ms | note |\n|---|---|---|---|---|\n")
        for r in results:
            w(f"| `{r['path']}` | {r['status']} | {r.get('count') if r.get('count') is not None else '-'} "
              f"| {r['elapsed_ms']} | {r['note']} |\n")
        w("\n## Candlestick history depth, per contract\n\n")
        w("Binary search on the oldest hourly window that still returns data. This is the single\n"
          "number that bounds the whole research plan, and it is **per contract**, not per venue.\n\n")
        w("| ticker | days of history | inferred first listing |\n|---|---|---|\n")
        for tk, d in depth_note:
            when = datetime.fromtimestamp(now - d * 86400, timezone.utc).strftime('%Y-%m-%d') if d else '-'
            w(f"| `{tk}` | {d} | {when} |\n")
        deep = [d for _, d in depth_note if d > 0]
        w(f"\n**Deepest: {max(deep) if deep else 0} days. Shallowest: {min(deep) if deep else 0}.** "
          f"Any request for more than the deepest figure returns empty windows, so `--days 90` is a\n"
          "request and not a fact. A pooled fit across contracts with this spread of history is\n"
          "mostly a statement about the oldest one; `reports/perps_data_audit.md` grades them.\n\n")
        w("## /margin/trades pagination\n\n```\n" + fmt(pag) + "\n```\n\n")
        w("A `looped: true` means the cursor stopped advancing and paging must be bounded by\n"
          "`min_ts` / `max_ts` rather than trusted to terminate.\n\n")
        w("## Schemas and samples\n\n")
        for r in results:
            w(f"### `{r['path']}`\n\n")
            w(f"- status **{r['status']}**, {r['elapsed_ms']}ms\n- params: `{json.dumps(r['params'])}`\n")
            if r['rate_headers']:
                w(f"- rate-limit headers: `{json.dumps(r['rate_headers'])}`\n")
            else:
                w("- rate-limit headers: none returned\n")
            if r['error']: w(f"- error: `{r['error'][:200]}`\n")
            if r['schema'] is not None:
                w("\n**schema**\n\n```json\n" + fmt(r['schema']) + "\n```\n")
                w("\n**sample** (ids redacted)\n\n```json\n" + fmt(r['sample']) + "\n```\n")
            w("\n")
    print(f"\nwrote {a.out}")
    print(f"{len(ok)}/{len(results)} endpoints returned 200")
    for r in bad: print(f"  ABSENT {r['path']}  status {r['status']}")


main()
