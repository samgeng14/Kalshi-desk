#!/usr/bin/env python3
"""Backfill Kalshi perpetual futures history into data/perps_research.sqlite.

The premise that this desk has been working from -- "Kalshi exposes no historical endpoints for
margin/perps, so record forward and wait" -- is wrong. Three historical endpoints exist:

  GET /margin/markets/{ticker}/candlesticks   1m / 1h / 1d OHLC of BID, ASK and TRADE price,
                                              plus volume, notional volume and open interest
  GET /margin/trades                          every public print with taker_side, cursor-paged,
                                              min_ts / max_ts
  GET /margin/funding_rates/historical        every funding application with the mark at the time

So the 2.4 days of 17-second polled snapshots in data/prod.sqlite are not the sample. Minute bars
reaching back as far as the exchange keeps them are, and they arrive in minutes rather than weeks.

What this does NOT replace: sub-minute microstructure. Lead-lag at 100-500ms, queue position and
orderbook imbalance need the tick recorder, because a 1-minute bar cannot answer them. Run both.

Auth: RSA-PSS over (timestamp_ms + METHOD + path), exactly as src/auth.cpp does it. The private
key is read from the path in config/desk.json and never leaves this machine.

  python3 scripts/backfill_perps_history.py --days 90
  python3 scripts/backfill_perps_history.py --days 30 --tickers KXBTCPERP KXETHPERP
  python3 scripts/backfill_perps_history.py --days 7 --trades      # also pull prints (slow)

Resumable: each (ticker, period, day) chunk already stored is skipped, so re-running continues.
"""
import argparse, base64, json, os, sqlite3, sys, time
import urllib.error, urllib.request
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PREFIX = '/trade-api/v2'

SCHEMA = """
create table if not exists perp_bars(
  ticker text, period integer, end_ts integer,
  bid_open real, bid_high real, bid_low real, bid_close real,
  ask_open real, ask_high real, ask_low real, ask_close real,
  px_open real, px_high real, px_low real, px_close real, px_mean real, px_prev real,
  volume real, volume_notional real, open_interest real, oi_notional real,
  primary key(ticker, period, end_ts));
create index if not exists perp_bars_t on perp_bars(ticker, period, end_ts);
create table if not exists perp_hist_trades(
  -- keyed with the ticker: nothing documents a trade id as unique across markets
  ticker text, trade_id text, ts integer, price real, count real, taker_side text,
  primary key(ticker, trade_id));
create index if not exists perp_hist_trades_t on perp_hist_trades(ticker, ts);
create table if not exists perp_funding(
  ticker text, funding_ts integer, rate real, mark real,
  primary key(ticker, funding_ts));
create table if not exists perp_meta(
  ticker text primary key, title text, contract_size real, tick real, max_leverage real,
  status text, fetched_ts integer);
create table if not exists fetch_log(
  ticker text, kind text, chunk text, rows integer, done_ts integer,
  primary key(ticker, kind, chunk));
"""


def load_config():
    """Read key id and pem path out of config/desk.json, comments and all."""
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
    base = ('https://external-api.kalshi.com' if cfg.get('env') == 'prod'
            else 'https://external-api.demo.kalshi.co') + PREFIX
    return cfg['key_id'], os.path.expanduser(cfg['pem_path']), base


def make_signer(key_id, pem_path):
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import padding
    except ImportError:
        sys.exit("need `pip install cryptography`")
    with open(pem_path, 'rb') as f:
        key = serialization.load_pem_private_key(f.read(), password=None)

    def headers(method, path):
        ts = str(int(time.time() * 1000))
        sig = key.sign((ts + method + path).encode(),
                       padding.PSS(mgf=padding.MGF1(hashes.SHA256()),
                                   salt_length=hashes.SHA256.digest_size),
                       hashes.SHA256())
        return {'KALSHI-ACCESS-KEY': key_id,
                'KALSHI-ACCESS-TIMESTAMP': ts,
                'KALSHI-ACCESS-SIGNATURE': base64.b64encode(sig).decode(),
                'accept': 'application/json'}
    return headers


class Api:
    def __init__(self, base, headers_fn, rps=6.0):
        self.base, self.headers_fn, self.gap, self.last = base, headers_fn, 1.0 / rps, 0.0

    def get(self, path, params=None, tries=6):
        import urllib.parse
        qs = ('?' + urllib.parse.urlencode(params)) if params else ''
        for attempt in range(tries):
            wait = self.gap - (time.time() - self.last)
            if wait > 0: time.sleep(wait)
            self.last = time.time()
            req = urllib.request.Request(self.base + path + qs,
                                         headers=self.headers_fn('GET', PREFIX + path))
            try:
                with urllib.request.urlopen(req, timeout=40) as r:
                    return json.loads(r.read().decode())
            except urllib.error.HTTPError as e:
                body = e.read().decode()[:200]
                if e.code in (429, 500, 502, 503, 504):
                    time.sleep(1.5 * (attempt + 1) ** 2); continue
                raise SystemExit(f"HTTP {e.code} on {path}{qs}\n{body}")
            except Exception:
                if attempt == tries - 1: raise
                time.sleep(1.5 * (attempt + 1))
        raise SystemExit("giving up on " + path)


# Kalshi encodes "no order on this side" as INT64_MAX, which arrives as 922337203685477.6 after the
# 10^4 fixed-point scaling. It is not a price. Left in, one such bar produces two astronomical
# returns -- into it and out of it -- which is enough to move a decile mean to 1e14 bps and make a
# feature look like a 300-trillion-basis-point edge. This exact sentinel was already fixed once in
# src/perps.cpp; it was not carried into the research path.
SENTINEL = 1e9


def px(v):
    """A price, or 0 if the field is the no-order sentinel, negative, or not finite."""
    try:
        f = float(v)
    except (TypeError, ValueError):
        return 0.0
    return f if (f == f and 0.0 < f < SENTINEL) else 0.0


def num(d, *keys, default=0.0):
    for k in keys:
        if isinstance(d, dict) and k in d and d[k] is not None:
            try: return float(d[k])
            except (TypeError, ValueError): pass
    return default


def ohlc(d, key):
    """OHLC with the no-order sentinel stripped, so it never reaches the database."""
    o = d.get(key) if isinstance(d, dict) else None
    if not isinstance(o, dict): return (0.0, 0.0, 0.0, 0.0)
    return (px(o.get('open')), px(o.get('high')), px(o.get('low')), px(o.get('close')))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--days', type=int, default=90)
    ap.add_argument('--period', type=int, default=1, choices=[1, 60, 1440],
                    help='1=minute bars, 60=hourly, 1440=daily')
    ap.add_argument('--tickers', nargs='*')
    ap.add_argument('--trades', action='store_true', help='also pull public prints (slow)')
    ap.add_argument('--span-days', type=int, default=0,
                    help='days per candlestick request; 0 measures the limit automatically')
    ap.add_argument('--db', default=os.path.join(ROOT, 'data', 'perps_research.sqlite'))
    a = ap.parse_args()

    key_id, pem, base = load_config()
    api = Api(base, make_signer(key_id, pem))
    os.makedirs(os.path.dirname(a.db), exist_ok=True)
    db = sqlite3.connect(a.db)
    db.executescript(SCHEMA)

    mk = api.get('/margin/markets', {'status': 'active'})
    markets = mk.get('markets') or mk.get('margin_markets') or []
    if not markets:
        sys.exit("no markets returned; response keys: " + ",".join(mk.keys()))
    rows = [(m.get('ticker'), m.get('title', ''), num(m, 'contract_size'),
             num(m, 'tick_size', 'tick', default=0.0001), num(m, 'leverage', 'max_leverage'),
             m.get('status', ''), int(time.time())) for m in markets if m.get('ticker')]
    db.executemany("insert or replace into perp_meta values(?,?,?,?,?,?,?)", rows)
    db.commit()
    tickers = a.tickers or sorted(r[0] for r in rows)
    print(f"{len(rows)} perp markets listed; backfilling {len(tickers)}")

    now = int(time.time())
    start = now - a.days * 86400

    def probe_span(t):
        """How wide a window will the endpoint actually serve for this period?

        One day per call means 90 calls a ticker and 1,800 for the book. The endpoint may serve
        far more per request, but the limit is not documented, so measure it: ask for a window and
        check whether the reply is as full as it should be. A truncated reply means the cap was
        hit, and we step down rather than silently losing the older part of every window.
        """
        if a.span_days: return a.span_days * 86400
        per_day = 1440 if a.period == 1 else (24 if a.period == 60 else 1)
        for days in (30, 14, 7, 3, 1):
            e = now; s0 = e - days * 86400
            try:
                j = api.get(f'/margin/markets/{t}/candlesticks',
                            {'start_ts': s0, 'end_ts': e, 'period_interval': a.period})
            except SystemExit:
                continue
            got = len(j.get('candlesticks') or j.get('margin_candlesticks') or [])
            # 90% of a full window means it served the whole range; much less means a row cap.
            if got >= 0.9 * days * per_day:
                print(f"  window probe: {days}d returns {got:,} bars -- using {days}d per call")
                return days * 86400
            print(f"  window probe: {days}d returns only {got:,} of ~{days*per_day:,}")
        return 86400

    # Funding first, not last. It is one call for the whole book, and running it at the end meant
    # an interrupted backfill -- which is the normal case for a job this long -- lost it entirely.
    try:
        j = api.get('/margin/funding_rates/historical', {'start_ts': start, 'end_ts': now})
        fr = j.get('funding_rates') or []
        db.executemany("insert or replace into perp_funding values(?,?,?,?)",
                       [(x.get('market_ticker'),
                         int(datetime.fromisoformat(x['funding_time'].replace('Z', '+00:00')).timestamp())
                         if x.get('funding_time') else 0,
                         num(x, 'funding_rate'), num(x, 'mark_price'))
                        for x in fr if x.get('market_ticker')])
        db.commit()
        print(f"funding applications: {len(fr):,}")
    except SystemExit as ex:
        print("funding history unavailable:", ex)

    span = probe_span(tickers[0]) if a.period == 1 else 30 * 86400
    # Resume by TIME COVERAGE, not by chunk key. The key used to be "period:start", which silently
    # assumed a fixed window: change the window size and every key misses, so a resumed run would
    # re-fetch history it already had. Coverage is the thing that actually matters.
    covered = {}
    for t, chunk in db.execute("select ticker,chunk from fetch_log where kind='bars'"):
        parts = str(chunk).split(':')
        try:
            if len(parts) >= 3:                       # "period:start:end", written from here on
                per, c0, c1 = int(parts[0]), int(parts[1]), int(parts[2])
            else:                                     # "period:start", older rows: one day wide
                per, c0 = int(parts[0]), int(parts[1]); c1 = c0 + 86400
        except ValueError:
            continue
        if per == a.period:
            covered.setdefault(t, []).append((c0, c1))
    for t in covered:                                  # merge so containment is one pass
        iv, merged = sorted(covered[t]), []
        for c0, c1 in iv:
            if merged and c0 <= merged[-1][1]: merged[-1] = (merged[-1][0], max(merged[-1][1], c1))
            else: merged.append((c0, c1))
        covered[t] = merged

    def is_covered(t, s0, e0):
        for c0, c1 in covered.get(t, []):
            if c0 <= s0 and e0 <= c1: return True
        return False
    windows = list(range(start, now, span))
    # Count only what THIS run has to do. Counting windows a previous run already covered made the
    # fraction jump to 38% in the first seconds and the ETA collapse to "0m left".
    todo_at_start = sum(1 for tk in tickers for s0 in windows
                        if not is_covered(tk, s0, min(s0 + span, now)))
    t_start = time.time()
    print(f"  {todo_at_start:,} windows to fetch ({len(tickers)} tickers x {len(windows)} windows, "
          f"minus what is already stored)")

    for ti, t in enumerate(tickers, 1):
        got = 0
        for s in range(start, now, span):
            e = min(s + span, now)
            chunk = f"{a.period}:{s}:{e}"
            if is_covered(t, s, e): continue
            try:
                j = api.get(f'/margin/markets/{t}/candlesticks',
                            {'start_ts': s, 'end_ts': e, 'period_interval': a.period})
            except SystemExit as ex:
                print(f"  {t} {datetime.fromtimestamp(s, timezone.utc):%Y-%m-%d}: {ex}")
                continue
            cs = j.get('candlesticks') or j.get('margin_candlesticks') or []
            out = []
            for c in cs:
                b, k, p = ohlc(c, 'bid'), ohlc(c, 'ask'), ohlc(c, 'price')
                out.append((t, a.period, int(num(c, 'end_period_ts')),
                            b[0], b[1], b[2], b[3], k[0], k[1], k[2], k[3],
                            p[0], p[1], p[2], p[3],
                            num(c.get('price') or {}, 'mean'), num(c.get('price') or {}, 'previous'),
                            num(c, 'volume'), num(c, 'volume_notional_value_dollars'),
                            num(c, 'open_interest'), num(c, 'open_interest_notional_value_dollars')))
            if out:
                db.executemany("insert or replace into perp_bars values(" + ",".join("?" * 21) + ")", out)
            db.execute("insert or replace into fetch_log values(?,?,?,?,?)",
                       (t, 'bars', chunk, len(out), int(time.time())))
            db.commit()
            covered.setdefault(t, []).append((s, e))
            got += len(out)
        # Progress with an ETA: this is a long job and an interrupted one is resumable, so the
        # useful thing to show is how much is left rather than only what has finished.
        remaining = sum(1 for tk in tickers[ti:] for s0 in windows
                        if not is_covered(tk, s0, min(s0 + span, now)))
        frac = (1.0 - remaining / todo_at_start) if todo_at_start else 1.0
        el = time.time() - t_start
        eta = (el / frac - el) if frac > 0.02 else float('nan')
        eta_s = (f"{eta/60:.0f}m left" if eta == eta and eta >= 60 else
                 f"{eta:.0f}s left" if eta == eta else "estimating")
        print(f"[{ti}/{len(tickers)}] {t:14s} {got:7d} bars   {frac*100:5.1f}%  {eta_s}")

        if a.trades:
            cursor, n = None, 0
            while True:
                q = {'ticker': t, 'limit': 1000, 'min_ts': start, 'max_ts': now}
                if cursor: q['cursor'] = cursor
                j = api.get('/margin/trades', q)
                tr = j.get('trades') or []
                if not tr: break
                db.executemany("insert or replace into perp_hist_trades values(?,?,?,?,?,?)",
                               [(t, x.get('trade_id'),
                                 int(datetime.fromisoformat(
                                     x['created_time'].replace('Z', '+00:00')).timestamp())
                                 if x.get('created_time') else 0,
                                 num(x, 'price'), num(x, 'count'), x.get('taker_side', ''))
                                for x in tr if x.get('trade_id')])
                db.commit(); n += len(tr)
                cursor = j.get('cursor')
                if not cursor: break
            print(f"{'':20s} {n:7d} prints")

    print()
    for t, n, lo, hi in db.execute(
            "select ticker,count(*),min(end_ts),max(end_ts) from perp_bars group by ticker order by ticker"):
        print(f"  {t:14s} {n:7d} bars  {datetime.fromtimestamp(lo, timezone.utc):%Y-%m-%d} "
              f"-> {datetime.fromtimestamp(hi, timezone.utc):%Y-%m-%d}")
    tot = db.execute("select count(*) from perp_bars").fetchone()[0]
    print(f"\n{tot:,} bars in {a.db}")
    capped = [t for t, lo in db.execute(
        "select ticker, min(end_ts) from perp_bars where period=? group by ticker", (a.period,))
        if lo - start < 2 * 86400]
    if capped:
        print(f"\n{len(capped)} contracts reach back to the --days {a.days} boundary itself "
              f"({', '.join(capped[:6])}{'...' if len(capped) > 6 else ''}).")
        print(f"That is this request binding, not the exchange running out. Kalshi launched perps "
              f"in late May 2026, so try --days {a.days + 40} to get the rest.")


try:
    main()
except KeyboardInterrupt:
    # Every completed chunk is committed and logged in fetch_log, so this is a pause, not a loss.
    print("\n\ninterrupted. Every chunk already fetched is committed -- re-run the same command "
          "and it resumes where it stopped.")
    raise SystemExit(130)
