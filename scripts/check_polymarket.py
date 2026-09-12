#!/usr/bin/env python3
"""Is the Polymarket credential good, and if not, which part is wrong?

`401 API key not found` and `401 invalid signature` mean completely different things and the desk
logs both the same way. This separates them, and it reads the secret from the file rather than
taking it on a command line, so the key never lands in shell history or a chat transcript.

  python3 scripts/check_polymarket.py
"""
import base64, hashlib, json, os, re, sys, time, urllib.error, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def config():
    text = open(os.path.join(ROOT, 'config', 'desk.json')).read()
    text = re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*[\s\S]*?\*/',
                  lambda m: '' if m[0].startswith('/') else m[0], text)
    return json.loads(text).get('params', {}).get('polymarket', {})


def main():
    pc = config()
    key_id = pc.get('key_id', '')
    path = os.path.expanduser(pc.get('secret_path', ''))
    api = pc.get('api_base', 'https://api.polymarket.us')
    gw = pc.get('gateway_base', 'https://gateway.polymarket.us')
    print(f'key id      {key_id or "(not set)"}')
    print(f'secret file {path or "(not set)"}')

    if not key_id or not path:
        sys.exit('\nconfig incomplete: set params.polymarket.key_id and secret_path in config/desk.json')
    if not os.path.exists(path):
        # An absolute path written on another machine still names the right file inside the repo.
        fallback = os.path.join(ROOT, '.secrets', os.path.basename(path))
        if os.path.exists(fallback):
            print(f'            (not at that path here; using {fallback})')
            path = fallback
        else:
            sys.exit(f'\nno secret file at {path}')

    raw = open(path, 'rb').read().strip()
    try:
        seed = base64.b64decode(raw)
    except Exception as e:
        sys.exit(f'\nthe secret file is not valid base64: {e}')
    print(f'            {len(raw)} chars of base64 -> {len(seed)} bytes, mode {oct(os.stat(path).st_mode & 0o777)}')
    if len(seed) not in (32, 64):
        sys.exit('\nan Ed25519 secret must decode to 32 or 64 bytes. This one does not, so the file '
                 'is not the secret Polymarket issued.')

    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
        from cryptography.hazmat.primitives import serialization
    except ImportError:
        sys.exit('\npip install cryptography to run this check')
    sk = Ed25519PrivateKey.from_private_bytes(seed[:32])
    pub = sk.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    print(f'            public key fingerprint {hashlib.sha256(pub).hexdigest()[:16]}')
    if len(seed) == 64:
        ok = seed[32:] == pub
        print(f'            layout seed||public: {"consistent" if ok else "INCONSISTENT"}')
        if not ok:
            print('            the trailing 32 bytes are not this key\'s public half, so either the '
                  'secret is in another format or it was truncated in transit.')

    # 1. Public data needs no credential at all. If this fails it is the network, not the key.
    print('\npublic data (no credential):')
    try:
        with urllib.request.urlopen(gw + '/v1/markets?limit=1', timeout=20) as r:
            print(f'  {gw} -> {r.status}, reachable')
    except Exception as e:
        print(f'  {gw} -> {str(e)[:90]}')
        print('  (a DNS or network failure here is not a key problem)')

    # The one local variable that can cause a valid key to be rejected: Polymarket requires the
    # timestamp within 30 seconds of ITS clock, so a drifting Mac looks like a bad credential.
    print('\nclock:')
    try:
        with urllib.request.urlopen(gw + '/v1/markets?limit=1', timeout=20) as r:
            served = r.headers.get('Date')
        if served:
            import email.utils
            skew = time.time() - email.utils.parsedate_to_datetime(served).timestamp()
            verdict = 'fine' if abs(skew) < 20 else 'TOO FAR OFF: signatures will be rejected'
            print(f'  local clock is {skew:+.1f}s from the server, {verdict}')
    except Exception as e:
        print(f'  could not compare clocks: {str(e)[:60]}')

    # 2. Signed call. Ed25519 over timestamp_ms + METHOD + path, exactly as the docs specify:
    #    "{timestamp}{method}{path}", no separators, query string and body excluded.
    print('\nsigned call:')
    for path_ in ('/v1/account/balances', '/v1/positions'):
        ts = str(int(time.time() * 1000))
        sig = base64.b64encode(sk.sign((ts + 'GET' + path_).encode())).decode()
        req = urllib.request.Request(api + path_, method='GET', headers={
            'X-PM-Access-Key': key_id, 'X-PM-Timestamp': ts,
            'X-PM-Signature': sig, 'Content-Type': 'application/json'})
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                print(f'  GET {path_:22} {r.status} OK  {r.read(160).decode()[:150]}')
        except urllib.error.HTTPError as e:
            body = e.read().decode()[:180]
            print(f'  GET {path_:22} {e.code}  {body}')
            if e.code == 401 and 'not found' in body.lower():
                print('    -> Polymarket does not know this key id, so the signature was never')
                print('       checked. If a freshly created key gets this, the key is not the')
                print('       problem: docs.polymarket.us/api-reference/authentication requires')
                print('       identity verification to COMPLETE before an API key is provisioned,')
                print('       and a key generated while the account still says "verifying your')
                print('       info" is not recognised. Nothing in this repo can work around that.')
                print('       Check the app shows you approved, then generate a key and replace')
                print('       BOTH the key_id in config/desk.json and the secret file.')
            elif e.code == 401:
                print('    -> the key id is recognised but the signature is not. That is a signing')
                print('       problem, not a credential one: check the secret file was saved whole.')
            elif e.code == 403:
                print('    -> recognised and signed, but not permitted. The key may need API access')
                print('       enabled on the account, or trading is not approved yet.')
        except Exception as e:
            print(f'  GET {path_:22} {str(e)[:90]}')


if __name__ == '__main__':
    main()
