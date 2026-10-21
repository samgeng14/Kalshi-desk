"""Signing, the subscription commands (never an empty list), and the settings' guards."""
import base64
import json
import os
import tempfile
import unittest

from recorder import config, kalshi
from recorder.tests.util import HAVE_CRYPTO

if HAVE_CRYPTO:
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ed25519, padding, rsa

NEEDS_CRYPTO = unittest.skipUnless(HAVE_CRYPTO, 'cryptography is not installed (pip install -r recorder/requirements.txt)')


def pem(key):
    return key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                             serialization.NoEncryption())


@NEEDS_CRYPTO
class SignerTest(unittest.TestCase):
    def test_ed25519_and_rsa(self):
        e = ed25519.Ed25519PrivateKey.generate()
        s = kalshi.Signer('key-1', pem(e))
        self.assertEqual(s.kind, 'ed25519')
        h = s.headers('GET', '/trade-api/ws/v2?x=1', ts_ms=1791648000123)
        self.assertEqual(h['KALSHI-ACCESS-KEY'], 'key-1')
        self.assertEqual(h['KALSHI-ACCESS-TIMESTAMP'], '1791648000123')
        # the query is not signed
        e.public_key().verify(base64.b64decode(h['KALSHI-ACCESS-SIGNATURE']), b'1791648000123GET/trade-api/ws/v2')
        r = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        s = kalshi.Signer('key-2', pem(r))
        self.assertEqual(s.kind, 'rsa')
        h = s.headers('GET', '/trade-api/v2/markets', ts_ms=5)
        r.public_key().verify(base64.b64decode(h['KALSHI-ACCESS-SIGNATURE']), b'5GET/trade-api/v2/markets',
                              padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=padding.PSS.DIGEST_LENGTH),
                              hashes.SHA256())
        self.assertNotIn('key-2', repr(s))


class CommandTest(unittest.TestCase):
    def test_explicit_lists(self):
        with self.assertRaises(ValueError):
            kalshi.subscribe_cmd(1, 'orderbook_delta', set())
        c = json.loads(kalshi.subscribe_cmd(1, 'orderbook_delta', {'B', 'A'}))
        self.assertEqual(c, {'id': 1, 'cmd': 'subscribe', 'params': {'channels': ['orderbook_delta'], 'market_tickers': ['A', 'B']}})
        u = json.loads(kalshi.update_cmd(2, 7, 'add_markets', {'C'}, True))
        self.assertEqual(u['params'], {'sids': [7], 'market_tickers': ['C'], 'action': 'add_markets', 'send_initial_snapshot': True})
        self.assertNotIn('send_initial_snapshot', json.loads(kalshi.update_cmd(3, 7, 'delete_markets', {'C'}, True))['params'])
        with self.assertRaises(ValueError):
            kalshi.update_cmd(4, 7, 'add_markets', set(), True)
        with self.assertRaises(ValueError):
            kalshi.update_cmd(4, 7, 'get_snapshot', {'C'}, True)
        self.assertEqual(json.loads(kalshi.pyth_cmd(5))['params'], {'channels': ['pyth_value'], 'underlying_tickers': ['all']})

    def test_rest_has_only_get(self):
        self.assertEqual([m for m in dir(kalshi.Rest) if not m.startswith('_')], ['get', 'markets'])


@NEEDS_CRYPTO
class ConfigTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        key = os.path.join(self.dir.name, 'k.pem')
        with open(key, 'wb') as f:
            f.write(pem(ed25519.Ed25519PrivateKey.generate()))
        kid = os.path.join(self.dir.name, 'k.id')
        with open(kid, 'w') as f:
            f.write('key-1\n')
        self.env = {'KALSHI_REST': 'https://api.elections.kalshi.com/trade-api/v2',
                    'KALSHI_WS': 'wss://api.elections.kalshi.com/trade-api/ws/v2',
                    'KALSHI_KEY_FILE': key, 'KALSHI_KEY_ID_FILE': kid, 'REC_DATA_DIR': self.dir.name}

    def tearDown(self):
        self.dir.cleanup()

    def test_load(self):
        s = config.load(dict(self.env))
        self.assertEqual(s.key_id, 'key-1')
        self.assertIn('KXNATGAS15M', s.book_series)
        self.assertIn('KXHIGHNY', s.book_series)
        self.assertEqual((s.lead_s, s.linger_s, s.bucket_s), (1200, 300, 3600))

    def test_guards(self):
        for bad in [{'KALSHI_REST': ''},                                    # no default host
                    {'KALSHI_REST': 'http://api.elections.kalshi.com/x'},   # plain http off loopback
                    {'KALSHI_WS': 'ws://example.com/x'},
                    {'REC_CA_FILE': '/tmp/ca.pem'},                          # a test CA off loopback
                    {'REC_BUCKET_S': '7'},                                   # must divide an hour
                    {'KALSHI_KEY_FILE': ''}]:
            env = dict(self.env)
            env.update(bad)
            with self.assertRaises(ValueError, msg=str(bad)):
                config.load(env)
        env = dict(self.env)
        env.update({'KALSHI_REST': 'http://127.0.0.1:9/trade-api/v2', 'KALSHI_WS': 'ws://127.0.0.1:9/trade-api/ws/v2',
                    'REC_CA_FILE': '/tmp/ca.pem', 'REC_BOOK_SERIES': 'A, B'})
        self.assertEqual(config.load(env).book_series, ['A', 'B'])

    def test_credentials_directory_first(self):
        env = dict(self.env)
        d = os.path.join(self.dir.name, 'creds')
        os.mkdir(d)
        with open(os.path.join(d, 'kalshi-key-id'), 'w') as f:
            f.write('from-systemd')
        with open(os.path.join(d, 'kalshi-key.pem'), 'wb') as f:
            f.write(pem(ed25519.Ed25519PrivateKey.generate()))
        env['CREDENTIALS_DIRECTORY'] = d
        self.assertEqual(config.load(env).key_id, 'from-systemd')


if __name__ == '__main__':
    unittest.main()
