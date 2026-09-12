#!/usr/bin/env python3
"""Find the WebSocket URL and signed path that Kalshi actually accepts.

The margin socket answers 404 to the documented `wss://external-api-margin-ws.kalshi.com/`. A 404
on a handshake is a routing answer, not an auth answer, but some servers return 404 rather than
401 to avoid confirming an endpoint exists, so both the URL path and the path used for the RSA
signature are worth varying. Ten-ish handshakes settles it in seconds.

Prints one line per attempt: the URL, the signed path, and the HTTP status or the frame received.

  python3 scripts/ws_probe.py
"""
import asyncio, base64, json, os, ssl, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PREFIX = '/trade-api/v2'

MARGIN_HOSTS = ['wss://external-api-margin-ws.kalshi.com',
                'wss://external-api-ws.kalshi.com',
                'wss://api.elections.kalshi.com']
EVENT_HOSTS = ['wss://external-api-ws.kalshi.com',
               'wss://api.elections.kalshi.com']
URL_PATHS = ['/', '/v2', '/ws', '/ws/v2', '/trade-api/ws/v2',
             '/trade-api/ws/v2/margin', '/margin/ws/v2', '/trade-api/margin/ws/v2']
SIGN_PATHS = [None, '/', '/trade-api/ws/v2', PREFIX]      # None = mirror the URL path


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


def connect(url, headers, **kw):
    import websockets, inspect
    try:
        ps = inspect.signature(websockets.connect).parameters
        kwname = 'additional_headers' if 'additional_headers' in ps else 'extra_headers'
    except (TypeError, ValueError):
        kwname = 'additional_headers'
    kw[kwname] = headers
    return websockets.connect(url, **kw)


async def attempt(url, signed, hdrs, sub):
    try:
        async with connect(url, hdrs('GET', signed), open_timeout=8, close_timeout=2) as ws:
            await ws.send(json.dumps(sub))
            # Keep reading past the subscribe ack: the question is whether DATA arrives, and an
            # envelope with no book in it is not data. This is the check that was missing.
            deadline = time.monotonic() + 8
            saw = []
            while time.monotonic() < deadline:
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=deadline - time.monotonic())
                except asyncio.TimeoutError:
                    break
                msg = json.loads(raw)
                kind = msg.get('type', '?')
                saw.append(kind)
                body = msg.get('msg') or {}
                has_book = bool(body.get('bid') or body.get('ask') or body.get('yes') or
                                body.get('no') or body.get('price') or body.get('value_usd'))
                if kind not in ('subscribed', 'ok') and has_book:
                    return f"DATA  {kind}  {json.dumps(msg)[:150]}"
                if kind == 'error':
                    return f"ERROR {json.dumps(msg)[:150]}"
            if saw:
                return f"EMPTY frames but no book: {dict((k, saw.count(k)) for k in set(saw))}"
            return "OPEN  (connected, nothing within 8s)"
    except Exception as e:
        name = type(e).__name__
        s = str(e)
        if 'HTTP' in s:
            return f"{name}: {s[:70]}"
        return f"{name}: {s[:70]}"


async def main():
    key_id, pem = load_config()
    hdrs = make_signer(key_id, pem)
    margin_sub = {'id': 1, 'cmd': 'subscribe',
                  'params': {'channels': ['ticker'], 'market_tickers': ['KXBTCPERP']}}
    # index_ids, and CF Benchmarks index IDs rather than Kalshi tickers
    event_sub = {'id': 1, 'cmd': 'subscribe',
                 'params': {'channels': ['cfbenchmarks_value_5hz'], 'index_ids': ['BRTI']}}

    for label, hosts, sub in (('MARGIN', MARGIN_HOSTS, margin_sub),
                              ('EVENT ', EVENT_HOSTS, event_sub)):
        print(f"\n=== {label} ===")
        found = False
        for host in hosts:
            for upath in URL_PATHS:
                for spath in SIGN_PATHS:
                    signed = upath if spath is None else spath
                    url = host + upath
                    res = await attempt(url, signed, hdrs, sub)
                    ok = res.startswith('DATA')
                    if ok or ('404' not in res and 'ConnectionClosed' not in res):
                        print(f"  {'OK ' if ok else '   '} {url:52s} signed={signed:20s} {res}")
                    if ok:
                        found = True
                        break
                if found: break
            if found: break
        if not found:
            print("  nothing opened. Re-running with every attempt shown:")
            for host in hosts:
                for upath in URL_PATHS:
                    res = await attempt(host + upath, upath, hdrs, sub)
                    print(f"      {host + upath:52s} signed={upath:20s} {res}")


try:
    import websockets  # noqa
except ImportError:
    sys.exit("need `pip install websockets`")
asyncio.run(main())
