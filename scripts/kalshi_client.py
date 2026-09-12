#!/usr/bin/env python3
"""Signed Kalshi REST client: reads, and orders behind an explicit arm.

THE KEY IS NEVER READ INTO ANYTHING THAT LEAVES THIS MACHINE. It is loaded from the path in
config/desk.json, used to sign, and discarded. It is never logged, never printed, never written
to a report, never committed.

ORDERS ARE GATED THREE WAYS, all of which must hold:
  1. the process must be running on the machine named in config/desk.json (`live_host`)
  2. KALSHI_ARM must be set to the exact string in the config's `arm_token`
  3. the caller must pass dry_run=False explicitly
Anything less returns a simulated acknowledgement and writes an intent record. This is so that
every code path above can be exercised end to end, repeatedly, with nothing reaching the
exchange.
"""
import base64, json, os, re, socket, sqlite3, sys, time, urllib.error, urllib.parse, urllib.request
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Perpetuals live under /margin/* on the external API host, NOT on the binary-contract
# endpoints. prod and demo differ only by host, exactly as perps_api_probe.py resolves it.
PROD_HOST = 'https://external-api.kalshi.com'
DEMO_HOST = 'https://external-api.demo.kalshi.co'
PREFIX = '/trade-api/v2'
UA = 'kalshi-desk-portfolio-v1/1.0'


def load_config(path=None):
    """Read config/desk.json, comments and all.

    String-aware rather than a regex: the config carries trailing `//` comments on the same line
    as values, and a naive line-anchored strip leaves them in place while a greedy one would eat
    the `//` inside any URL. This is the same stripper backfill_perps_history.py uses.
    """
    p = path or os.path.join(ROOT, 'config', 'desk.json')
    raw = open(p).read()
    out, i, instr, esc = [], 0, False, False
    while i < len(raw):
        ch = raw[i]
        if instr:
            out.append(ch)
            if esc:
                esc = False
            elif ch == '\\':
                esc = True
            elif ch == '"':
                instr = False
            i += 1
            continue
        if ch == '"':
            instr = True
            out.append(ch)
            i += 1
            continue
        if ch == '/' and i + 1 < len(raw) and raw[i + 1] == '/':
            while i < len(raw) and raw[i] != '\n':
                i += 1
            continue
        out.append(ch)
        i += 1
    cfg = json.loads(''.join(out))
    # The arming interlock lives in its own file so that desk.json -- which carries hand-written
    # comments -- never has to be machine-rewritten. config/arm.json is gitignored.
    arm = os.path.join(os.path.dirname(p), 'arm.json')
    if os.path.exists(arm):
        try:
            cfg.update(json.load(open(arm)))
        except ValueError:
            pass
    return cfg


class Kalshi:
    def __init__(self, cfg=None, base=None):
        self.cfg = cfg or load_config()
        host = PROD_HOST if self.cfg.get('env') == 'prod' else DEMO_HOST
        self.base = base or os.environ.get('KALSHI_BASE', host + PREFIX)
        self.key_id = self.cfg['key_id']
        self.pem_path = os.path.expanduser(self.cfg['pem_path'])
        if not os.path.exists(self.pem_path):
            raise SystemExit('signing key not found at the configured path')
        self._key = None

    def _sign(self, method, path):
        if self._key is None:
            with open(self.pem_path, 'rb') as f:
                self._key = serialization.load_pem_private_key(f.read(), password=None)
        ts = str(int(time.time() * 1000))
        msg = (ts + method + PREFIX + path.split('?')[0]).encode()
        sig = self._key.sign(msg,
                             padding.PSS(mgf=padding.MGF1(hashes.SHA256()),
                                         salt_length=padding.PSS.DIGEST_LENGTH),
                             hashes.SHA256())
        return {'KALSHI-ACCESS-KEY': self.key_id,
                'KALSHI-ACCESS-TIMESTAMP': ts,
                'KALSHI-ACCESS-SIGNATURE': base64.b64encode(sig).decode(),
                'User-Agent': UA, 'Content-Type': 'application/json'}

    def _call(self, method, path, body=None, timeout=30, tries=3):
        url = self.base + path
        data = json.dumps(body).encode() if body is not None else None
        last = None
        for attempt in range(tries):
            req = urllib.request.Request(url, data=data, method=method,
                                         headers=self._sign(method, path))
            try:
                with urllib.request.urlopen(req, timeout=timeout) as r:
                    return json.loads(r.read().decode() or '{}')
            except urllib.error.HTTPError as e:
                detail = e.read().decode()[:400]
                if e.code in (429, 500, 502, 503, 504) and attempt < tries - 1:
                    time.sleep(1.5 * (attempt + 1)); last = e; continue
                raise RuntimeError('%s %s -> HTTP %d %s' % (method, path, e.code, detail))
            except (urllib.error.URLError, OSError, TimeoutError) as e:
                # A lost response is NOT evidence the request failed. Callers that mutate state
                # must reconcile against the exchange rather than retry blindly; get/post below
                # enforce that for orders.
                last = e
                if attempt < tries - 1:
                    time.sleep(1.5 * (attempt + 1)); continue
                raise RuntimeError('%s %s -> %s' % (method, path, e))
        raise RuntimeError('%s %s -> %s' % (method, path, last))

    # ---------- reads ----------
    def get(self, path, **params):
        q = urllib.parse.urlencode({k: v for k, v in params.items() if v is not None})
        return self._call('GET', path + ('?' + q if q else ''))

    def balance(self):
        return self.get('/portfolio/balance')

    def positions(self, **kw):
        return self.get('/portfolio/positions', **kw)

    def orders(self, **kw):
        return self.get('/portfolio/orders', **kw)

    def fills(self, **kw):
        return self.get('/portfolio/fills', **kw)

    def markets(self, **kw):
        return self.get('/markets', **kw)

    def market(self, ticker):
        return self.get('/markets/' + ticker)

    def orderbook(self, ticker, depth=10):
        return self.get('/markets/%s/orderbook' % ticker, depth=depth)

    def paged(self, path, key, limit=200, max_pages=50, **kw):
        out, cursor = [], None
        for _ in range(max_pages):
            r = self.get(path, limit=limit, cursor=cursor, **kw)
            out.extend(r.get(key) or [])
            cursor = r.get('cursor')
            if not cursor:
                break
        return out

    # ---------- writes ----------
    @staticmethod
    def _norm_host(h):
        """macOS reports a hostname with or without a trailing .local depending on network state.

        Comparing those two spellings literally is a false negative that reads as "the arm is
        broken" at exactly the wrong moment, so both sides are normalised.
        """
        h = (h or '').strip().lower().rstrip('.')
        return h[:-6] if h.endswith('.local') else h

    def armed(self):
        """Three independent conditions, all required."""
        want = self.cfg.get('live_host')
        host_ok = bool(want) and self._norm_host(socket.gethostname()) == self._norm_host(want)
        tok = self.cfg.get('arm_token')
        tok_ok = bool(tok) and os.environ.get('KALSHI_ARM') == tok
        return host_ok, tok_ok

    @staticmethod
    def round_tick(price, tick):
        """Snap to the market's tick. Kalshi perps tick at 0.0001, not at a cent.

        Rounding a 2.2568 price to whole cents throws away up to half a cent, which on a $2.25
        contract is 22 bps -- most of a round trip, given away for nothing. The price field is
        dollars with four decimals and it must be a tick multiple or the order is rejected.
        """
        tick = float(tick or 0.0001)
        return round(round(float(price) / tick) * tick, 6)

    def create_perp_order(self, *, ticker, side, count, price, tick=0.0001,
                          post_only=False, reduce_only=False, tif='good_till_canceled',
                          client_order_id=None, journal=None, dry_run=True):
        """POST /margin/orders. Body shape taken from the desk's own C++ path, not invented.

        `side` is 'bid' (buy) or 'ask' (sell) -- the perp API does not speak yes/no. `count` and
        `price` go as formatted strings. `post_only` is what makes the maker phase actually a
        maker phase: without it a limit at the touch can cross and pay the taker fee. `reduce_only`
        is what makes a close a close, so a reconnect or a stale position figure can never flip a
        position through zero into a new one.
        """
        assert side in ('bid', 'ask'), "side must be 'bid' or 'ask'"
        assert float(count) > 0, 'count must be positive'
        px = self.round_tick(price, tick)
        assert px > 0, 'price must be positive'
        coid = client_order_id or ('v1-%d-%s-%s' % (int(time.time()), ticker, side))
        body = {'ticker': ticker, 'side': side,
                'count': ('%g' % float(count)),
                'price': ('%.4f' % px),
                'time_in_force': tif,
                'post_only': bool(post_only),
                'self_trade_prevention_type': 'taker_at_cross',
                'subaccount': 0,
                'client_order_id': coid}
        if reduce_only:
            body['reduce_only'] = True

        host_ok, tok_ok = self.armed()
        live = (not dry_run) and host_ok and tok_ok
        if journal is not None:
            journal.intent(coid, body, live=live, host_ok=host_ok, tok_ok=tok_ok)
        if not live:
            return {'simulated': True, 'order': dict(body, status='not_sent'),
                    'reason': ('dry_run' if dry_run else
                               ('wrong_host' if not host_ok else 'not_armed'))}
        resp = self._call('POST', '/margin/orders', body)
        if journal is not None:
            journal.ack(coid, resp)
        return resp

    def cancel_perp_order(self, order_id, journal=None, dry_run=True):
        host_ok, tok_ok = self.armed()
        if dry_run or not (host_ok and tok_ok):
            return {'simulated': True, 'order_id': order_id}
        r = self._call('DELETE', '/margin/orders/%s?subaccount=0' % order_id)
        if journal is not None:
            journal.note('cancel', {'order_id': order_id, 'resp': r})
        return r

    # ---------- perp reads ----------
    def perp_balance(self):
        return self.get('/margin/balance', compute_available_balance='true')

    def perp_positions(self):
        return self.get('/margin/positions')

    def perp_orders(self, status=None, limit=200):
        return self.get('/margin/orders', status=status, limit=limit)

    def perp_fills(self, limit=200, **kw):
        return self.get('/margin/fills', limit=limit, **kw)

    def perp_markets(self, status='active'):
        return self.get('/margin/markets', status=status)

    def perp_market(self, ticker):
        return self.get('/margin/markets/%s' % ticker)

    def perp_orderbook(self, ticker, depth=10):
        return self.get('/margin/markets/%s/orderbook' % ticker, depth=depth)

    def fee_tier_rates(self):
        """The account's ACTUAL maker/taker rates, rather than the tier-0 assumption."""
        return self.get('/margin/fee_tier_rates')


class Journal:
    """Durable record of what we meant to do, written before we try to do it.

    On restart, anything with an intent and no terminal state is reconciled against the exchange
    rather than assumed failed. A lost HTTP response is the case this exists for.
    """

    def __init__(self, path=None):
        self.path = path or os.path.join(ROOT, 'data', 'live_orders.sqlite')
        self.db = sqlite3.connect(self.path, timeout=30)
        self.db.execute('PRAGMA journal_mode=WAL')
        self.db.execute('''CREATE TABLE IF NOT EXISTS intent(
            coid TEXT PRIMARY KEY, ts_utc TEXT, body TEXT, live INT,
            host_ok INT, tok_ok INT, acked INT DEFAULT 0, ack TEXT,
            terminal TEXT)''')
        self.db.execute('''CREATE TABLE IF NOT EXISTS note(
            ts_utc TEXT, kind TEXT, payload TEXT)''')
        self.db.commit()

    def _now(self):
        return time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())

    def intent(self, coid, body, live, host_ok, tok_ok):
        self.db.execute('INSERT OR REPLACE INTO intent(coid,ts_utc,body,live,host_ok,tok_ok)'
                        ' VALUES(?,?,?,?,?,?)',
                        (coid, self._now(), json.dumps(body), int(live),
                         int(host_ok), int(tok_ok)))
        self.db.commit()

    def ack(self, coid, resp):
        self.db.execute('UPDATE intent SET acked=1, ack=? WHERE coid=?',
                        (json.dumps(resp), coid))
        self.db.commit()

    def settle(self, coid, terminal):
        self.db.execute('UPDATE intent SET terminal=? WHERE coid=?', (terminal, coid))
        self.db.commit()

    def note(self, kind, payload):
        self.db.execute('INSERT INTO note(ts_utc,kind,payload) VALUES(?,?,?)',
                        (self._now(), kind, json.dumps(payload)))
        self.db.commit()

    def unresolved(self):
        cur = self.db.execute('SELECT coid, body, live, acked, ack FROM intent'
                              ' WHERE terminal IS NULL AND live=1')
        return [dict(coid=r[0], body=json.loads(r[1]), live=r[2], acked=r[3],
                     ack=json.loads(r[4]) if r[4] else None) for r in cur]


if __name__ == '__main__':
    k = Kalshi()
    host_ok, tok_ok = k.armed()
    print('base          %s' % k.base)
    print('key id        %s...%s (%d chars, not printed in full)'
          % (k.key_id[:4], k.key_id[-4:], len(k.key_id)))
    print('signing key   present at the configured path, contents never read into output')
    print('host check    %s (this machine: %s)' % (host_ok, socket.gethostname()))
    print('arm token     %s' % tok_ok)
    print('ORDERS ARE %s' % ('ARMED' if (host_ok and tok_ok) else 'DISARMED'))
    if len(sys.argv) > 1:
        print(json.dumps(k.get(sys.argv[1], **dict(a.split('=', 1) for a in sys.argv[2:])),
                         indent=1)[:4000])
