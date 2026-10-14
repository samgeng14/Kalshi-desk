"""Kalshi, read-only: request signing, REST GETs, and the WebSocket's subscription commands.

There is no write route here and none may be added: the recorder runs on a read-only key on a
cloud VM, and an order from it would be a bug twice over. Rest.get is the only HTTP method.
Subscriptions are always an explicit list: an empty market list means the whole exchange to
Kalshi (77.6% of the old recorder's September rows were unrelated), so it is refused here.
"""
import base64
import json
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ed25519, padding, rsa

UA = 'deskd-recorder/1'


class Signer:
    """KALSHI-ACCESS-* headers: timestamp ms + method + path (no query), RSA-PSS SHA-256 with a
    digest-length salt, or pure Ed25519 (as deskd's gw/kalshi_sign)."""

    def __init__(self, key_id, pem):
        self.key_id = key_id
        self._key = serialization.load_pem_private_key(pem, password=None)
        if isinstance(self._key, ed25519.Ed25519PrivateKey):
            self.kind = 'ed25519'
        elif isinstance(self._key, rsa.RSAPrivateKey):
            self.kind = 'rsa'
        else:
            raise ValueError('the key is neither Ed25519 nor RSA')

    @staticmethod
    def message(ts_ms, method, path):
        return (str(ts_ms) + method + path.split('?')[0]).encode()

    def sign(self, ts_ms, method, path):
        msg = self.message(ts_ms, method, path)
        if self.kind == 'ed25519':
            sig = self._key.sign(msg)
        else:
            sig = self._key.sign(msg, padding.PSS(mgf=padding.MGF1(hashes.SHA256()),
                                                  salt_length=padding.PSS.DIGEST_LENGTH), hashes.SHA256())
        return base64.b64encode(sig).decode()

    def headers(self, method, path, ts_ms=None):
        ts = int(time.time() * 1000) if ts_ms is None else ts_ms
        return {'KALSHI-ACCESS-KEY': self.key_id, 'KALSHI-ACCESS-TIMESTAMP': str(ts),
                'KALSHI-ACCESS-SIGNATURE': self.sign(ts, method, path)}

    def __repr__(self):   # never the key or its id
        return '<Signer %s>' % self.kind


def ssl_context(ca_file=''):
    ctx = ssl.create_default_context()
    if ca_file:
        ctx.load_verify_locations(ca_file)
    return ctx


class HttpError(Exception):
    def __init__(self, status, body):
        super().__init__('HTTP %d: %s' % (status, body[:200]))
        self.status = status


class Rest:
    """Blocking GETs (run them in a thread). Signed, though the market routes are public."""

    def __init__(self, base_url, signer=None, ca_file='', timeout=10):
        self.base = base_url.rstrip('/')
        self.base_path = urllib.parse.urlparse(self.base).path
        self.signer = signer
        self.ctx = ssl_context(ca_file) if self.base.startswith('https') else None
        self.timeout = timeout

    def get(self, route, params=None):
        query = urllib.parse.urlencode({k: v for k, v in (params or {}).items() if v not in (None, '')})
        path = self.base_path + route
        req = urllib.request.Request(self.base + route + ('?' + query if query else ''), method='GET')
        req.add_header('Accept', 'application/json')
        req.add_header('User-Agent', UA)
        if self.signer:
            for k, v in self.signer.headers('GET', path).items():
                req.add_header(k, v)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout, context=self.ctx) as r:
                return json.loads(r.read())
        except urllib.error.HTTPError as e:
            raise HttpError(e.code, e.read().decode('utf-8', 'replace')) from None

    def markets(self, series, status, min_close_ts=None, max_pages=10):
        """Every market of a series in a status, through the cursor."""
        out, cursor = [], ''
        for _ in range(max_pages):
            page = self.get('/markets', {'series_ticker': series, 'status': status, 'limit': 1000,
                                         'cursor': cursor, 'min_close_ts': min_close_ts})
            out.extend(page.get('markets') or [])
            cursor = page.get('cursor') or ''
            if not cursor:
                break
        return out


# ---- WebSocket commands ------------------------------------------------------------------------

def subscribe_cmd(cmd_id, channel, tickers):
    if not tickers:
        raise ValueError('an empty market list subscribes the whole exchange: refused')
    return json.dumps({'id': cmd_id, 'cmd': 'subscribe',
                       'params': {'channels': [channel], 'market_tickers': sorted(tickers)}})


def pyth_cmd(cmd_id):
    # pyth_value takes underlyings, not markets; every underlying is a few messages a second
    return json.dumps({'id': cmd_id, 'cmd': 'subscribe',
                       'params': {'channels': ['pyth_value'], 'underlying_tickers': ['all']}})


def update_cmd(cmd_id, sid, action, tickers, snapshot):
    if not tickers:
        raise ValueError('an update names its markets')
    if action not in ('add_markets', 'delete_markets'):
        raise ValueError('unknown update action %r' % action)
    params = {'sids': [sid], 'market_tickers': sorted(tickers), 'action': action}
    if action == 'add_markets' and snapshot:
        params['send_initial_snapshot'] = True   # a delta stream joined mid-flight is not a book
    return json.dumps({'id': cmd_id, 'cmd': 'update_subscription', 'params': params})
