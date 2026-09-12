#!/usr/bin/env python3
"""Subscribe and listen, then report exactly what arrived, per channel and per market.

The recorder connected to the perps socket, got three subscribe acknowledgements and eighteen
orderbook snapshots, and then received nothing for a minute. A snapshot with no deltas is a book
that goes stale immediately, so this establishes which channels actually push for perp markets
before any more time is spent recording nothing.

Each channel is tried on its own so one bad name cannot suppress the others.

  python3 scripts/ws_listen.py
  python3 scripts/ws_listen.py --seconds 45 --tickers KXBTCPERP KXETHPERP
"""
import argparse, asyncio, base64, collections, json, os, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The working perps socket: documented host, undocumented path. See ws_probe.py.
URL = 'wss://external-api-margin-ws.kalshi.com/trade-api/ws/v2/margin'
SIGN = '/trade-api/ws/v2/margin'


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
    return cfg['key_id'], os.path.expanduser(cfg['pem_path'])


def make_signer(key_id, pem_path):
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding
    key = serialization.load_pem_private_key(open(pem_path, 'rb').read(), password=None)

    def headers(method, path):
        ts = str(int(time.time() * 1000))
        sig = key.sign((ts + method + path).encode(),
                       padding.PSS(mgf=padding.MGF1(hashes.SHA256()),
                                   salt_length=hashes.SHA256.digest_size),
                       hashes.SHA256())
        return {'KALSHI-ACCESS-KEY': key_id, 'KALSHI-ACCESS-TIMESTAMP': ts,
                'KALSHI-ACCESS-SIGNATURE': base64.b64encode(sig).decode()}
    return headers


def connect(url, headers):
    import inspect, websockets
    try:
        ps = inspect.signature(websockets.connect).parameters
        kw = 'additional_headers' if 'additional_headers' in ps else 'extra_headers'
    except (TypeError, ValueError):
        kw = 'additional_headers'
    return websockets.connect(url, **{kw: headers}, open_timeout=10, max_queue=8192)


async def listen(channel, tickers, seconds, hdrs, extra=None):
    types = collections.Counter()
    per_ticker = collections.Counter()
    first, errors = [], []
    params = {'channels': [channel], 'market_tickers': tickers}
    if extra: params.update(extra)
    try:
        async with connect(URL, hdrs('GET', SIGN)) as ws:
            await ws.send(json.dumps({'id': 1, 'cmd': 'subscribe', 'params': params}))
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=max(0.5, end - time.monotonic()))
                except asyncio.TimeoutError:
                    break
                m = json.loads(raw)
                t = m.get('type', '?')
                types[t] += 1
                if t == 'error':
                    errors.append(json.dumps(m)[:160])
                body = m.get('msg') or {}
                tk = body.get('market_ticker') or body.get('ticker')
                if tk: per_ticker[tk] += 1
                if len(first) < 2 and t not in ('subscribed',):
                    first.append(json.dumps(m)[:200])
    except Exception as e:
        return {'channel': channel, 'error': f"{type(e).__name__}: {str(e)[:110]}"}
    return {'channel': channel, 'types': dict(types), 'tickers': dict(per_ticker.most_common(5)),
            'first': first, 'errors': errors[:2]}


async def main(a):
    key_id, pem = load_config()
    hdrs = make_signer(key_id, pem)
    print(f"{URL}\nlistening {a.seconds}s per channel on {len(a.tickers)} markets: "
          f"{', '.join(a.tickers[:4])}{'...' if len(a.tickers) > 4 else ''}\n")
    trials = [('orderbook_delta', None), ('orderbook_delta', {'send_initial_snapshot': True}),
              ('ticker', None), ('ticker_v2', None), ('trade', None), ('fill', None)]
    for ch, extra in trials:
        r = await listen(ch, a.tickers, a.seconds, hdrs, extra)
        tag = ch + ('  +snapshot' if extra else '')
        if r.get('error'):
            print(f"  {tag:28s} ERROR {r['error']}")
            continue
        tot = sum(r['types'].values())
        print(f"  {tag:28s} {tot:5d} msgs  {r['types']}")
        if r['tickers']: print(f"  {'':28s}       top markets: {r['tickers']}")
        for e in r['errors']: print(f"  {'':28s}       ERROR {e}")
        for f in r['first']: print(f"  {'':28s}       {f}")
    print("\nA channel that acknowledges the subscribe and then sends nothing is not pushing for\n"
          "these markets. One that sends a snapshot and no deltas leaves a book that goes stale.")


ap = argparse.ArgumentParser()
ap.add_argument('--seconds', type=float, default=20)
ap.add_argument('--tickers', nargs='*',
                default=['KXBTCPERP', 'KXETHPERP', 'KXSOLPERP', 'KXXRPPERP'])
a = ap.parse_args()
try:
    import websockets  # noqa
except ImportError:
    sys.exit("need `pip install websockets`")
asyncio.run(main(a))
