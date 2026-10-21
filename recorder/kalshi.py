"""Kalshi, read-only: request signing, REST GETs, the WebSocket's subscription commands, and the
parsers for what the REST routes and the lifecycle channel return.

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
from datetime import datetime, timezone

UA = 'deskd-recorder/1'


class Signer:
    """KALSHI-ACCESS-* headers: timestamp ms + method + path (no query), RSA-PSS SHA-256 with a
    digest-length salt, or pure Ed25519 (as deskd's gw/kalshi_sign)."""

    def __init__(self, key_id, pem):
        # imported here, not at the top: the parsers below are pure and their tests run on a
        # bare python3 (ctest on the Mac) that may not have cryptography
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric import ed25519, rsa
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
            from cryptography.hazmat.primitives import hashes
            from cryptography.hazmat.primitives.asymmetric import padding
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


# ---- what the REST routes and the lifecycle channel return ------------------------------------
# The venue moved from integer cents (yes_bid, volume) to fixed-point strings (yes_bid_dollars,
# volume_fp); both are read, the new name first. A missing or unreadable field is None, never 0:
# a thin book with no ask is not an ask at 0.

def num(v):
    """A venue number, a JSON number or a fixed-point string, as a float; None when absent."""
    if v is None or isinstance(v, bool):
        return None
    if isinstance(v, (int, float)):
        return float(v)
    if isinstance(v, str) and v.strip():
        try:
            return float(v)
        except ValueError:
            return None
    return None


def dollars(row, key):
    """<key>_dollars, else the cents field <key> / 100."""
    d = num(row.get(key + '_dollars'))
    if d is not None:
        return d
    c = num(row.get(key))
    return None if c is None else c / 100.0


def count(row, key):
    """<key>_fp (contracts, fixed point), else <key>."""
    v = num(row.get(key + '_fp'))
    return v if v is not None else num(row.get(key))


def cursor_of(page):
    """The next page's cursor: /markets and /markets/trades call it cursor, /incentive_programs
    next_cursor. '' at the end."""
    c = page.get('cursor') or page.get('next_cursor') or ''
    return c if isinstance(c, str) else ''


def top_of_book(m):
    """A /markets row's best prices and sizes, its last price and volume: the row's own fields (the
    venue derives the YES ask from the best NO bid)."""
    return {'yes_bid': dollars(m, 'yes_bid'), 'yes_bid_size': count(m, 'yes_bid_size'),
            'yes_ask': dollars(m, 'yes_ask'), 'yes_ask_size': count(m, 'yes_ask_size'),
            'last_price': dollars(m, 'last_price'), 'volume': count(m, 'volume')}


def _levels(side, cents):
    out = []
    for lv in side or []:
        if not isinstance(lv, (list, tuple)) or len(lv) < 2:
            continue
        p, q = num(lv[0]), num(lv[1])
        if p is None or q is None:
            continue
        out.append((round(p / 100.0, 6) if cents else p, q))
    return sorted(out, reverse=True)   # best (highest bid) first, whatever order the venue sent


def book_levels(resp):
    """GET /markets/{ticker}/orderbook as (yes, no): each a list of (price in dollars, contracts),
    best first. Shapes read: orderbook_fp {yes_dollars, no_dollars} (fixed-point strings, what the
    fake and the venue send now), orderbook {yes_dollars, no_dollars}, and the older orderbook
    {yes, no} in cents. None when the reply holds no book at all."""
    for key in ('orderbook_fp', 'orderbook'):
        b = resp.get(key) if isinstance(resp, dict) else None
        if not isinstance(b, dict):
            continue
        if b.get('yes_dollars') is not None or b.get('no_dollars') is not None:
            return _levels(b.get('yes_dollars'), False), _levels(b.get('no_dollars'), False)
        if b.get('yes') is not None or b.get('no') is not None:
            return _levels(b.get('yes'), True), _levels(b.get('no'), True)
        return [], []   # a book with no level on either side
    return None


def best_of(yes, no):
    """The best YES bid and its size, and the YES ask: a NO bid at p is a YES offer at 1 - p."""
    out = {'yes_bid': None, 'yes_bid_size': None, 'yes_ask': None, 'yes_ask_size': None}
    if yes:
        out['yes_bid'], out['yes_bid_size'] = yes[0]
    if no:
        out['yes_ask'], out['yes_ask_size'] = round(1.0 - no[0][0], 6), no[0][1]
    return out


def iso_us(rfc):
    """RFC 3339 to epoch microseconds; None when absent or unreadable."""
    if not isinstance(rfc, str) or not rfc:
        return None
    try:
        return int(datetime.fromisoformat(rfc.replace('Z', '+00:00')).timestamp() * 1_000_000)
    except ValueError:
        return None


def text_of(row, k):
    v = row.get(k)
    return None if v is None or v == '' else str(v)


def trade_fields(t):
    """One print of GET /markets/trades."""
    yes, no = dollars(t, 'yes_price'), dollars(t, 'no_price')
    if no is None and yes is not None:
        no = round(1.0 - yes, 6)
    return {'trade_id': text_of(t, 'trade_id'), 'ticker': text_of(t, 'ticker') or text_of(t, 'market_ticker'),
            'created_time': text_of(t, 'created_time'), 'created_us': iso_us(t.get('created_time')),
            'yes_price': yes, 'no_price': no, 'count': count(t, 'count'), 'taker_side': text_of(t, 'taker_side')}


def incentive_fields(p):
    """One program of GET /incentive_programs. period_reward as the venue sends it (1/10,000 $ by
    scripts/incentive_audit.py), not converted."""
    paid = p.get('paid_out')
    return {'market_ticker': text_of(p, 'market_ticker'), 'incentive_type': text_of(p, 'incentive_type'),
            'period_reward': num(p.get('period_reward')), 'target_size': count(p, 'target_size'),
            'discount_factor_bps': num(p.get('discount_factor_bps')), 'start_date': text_of(p, 'start_date'),
            'end_date': text_of(p, 'end_date'), 'paid_out': paid if isinstance(paid, bool) else None}


def keep_incentive(p, types):
    """The venue ignores GET /incentive_programs' type filter (scripts/incentive_audit.py: asking for
    margin_taker_volume returned liquidity programs), so the types are chosen here, from each
    program's own incentive_type."""
    return isinstance(p, dict) and p.get('incentive_type') in types


def lifecycle_fields(j):
    """A market_lifecycle_v2 frame's market, what happened (created, activated, deactivated,
    close_date_updated, determined, settled) and its times, Unix seconds as sent; None for any
    other frame. A field the venue leaves out is None."""
    if not isinstance(j, dict) or j.get('type') != 'market_lifecycle_v2' or not isinstance(j.get('msg'), dict):
        return None
    msg = j['msg']
    t = msg.get('market_ticker')
    if not isinstance(t, str) or not t:
        return None

    def ts(k):
        v = msg.get(k)
        return v if isinstance(v, int) and not isinstance(v, bool) else None
    et, deact, result = msg.get('event_type'), msg.get('is_deactivated'), msg.get('result')
    return {'ticker': t, 'event_type': et if isinstance(et, str) else None,
            'open_ts': ts('open_ts'), 'close_ts': ts('close_ts'),
            'determination_ts': ts('determination_ts'), 'settled_ts': ts('settled_ts'),
            'result': result if isinstance(result, str) else None,
            'is_deactivated': deact if isinstance(deact, bool) else None}
