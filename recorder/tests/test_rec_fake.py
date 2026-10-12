"""The recorder against the fake exchange (tests/fake_exchange, built as build-deskd/fake-kalshi),
as a process, over TLS: the windows it subscribes (the open one and the next, not one hours away),
the weather market, books, trades and pyth_value to the spool, each window's REST row with its
strike, a clean exit on SIGTERM (systemd's stop), and the spools compacted to Parquet afterwards."""
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import importlib.util
import unittest

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FAKE = os.environ.get('FAKE_KALSHI', os.path.join(ROOT, 'build-deskd', 'fake-kalshi'))
NATGAS = 'Commodities.Index.NATGAS/USD'

SCENARIO = [
    {'t': 0, 'market': {'ticker': 'KXNATGAS15M-26OCT101500-00', 'exchange_index': 2, 'ticks': 'tapered_deci_cent',
                        'close_in_ms': 900000, 'strike_type': 'greater_or_equal', 'floor_strike': 3.105}},
    {'t': 0, 'market': {'ticker': 'KXNATGAS15M-26OCT101515-15', 'exchange_index': 2, 'ticks': 'tapered_deci_cent',
                        'open_in_ms': 600000, 'close_in_ms': 1500000, 'strike_type': 'greater_or_equal'}},
    {'t': 0, 'market': {'ticker': 'KXNATGAS15M-26OCT101800-00', 'exchange_index': 2, 'ticks': 'tapered_deci_cent',
                        'open_in_ms': 9000000, 'close_in_ms': 9900000}},
    {'t': 0, 'market': {'ticker': 'KXHIGHNY-26OCT10-B70', 'exchange_index': 0, 'ticks': 'linear_cent', 'close_in_ms': 36000000}},
    {'t': 0, 'snapshot': {'ticker': 'KXNATGAS15M-26OCT101500-00', 'yes': [['0.4500', '100.00']], 'no': [['0.5300', '80.00']]}},
    {'t': 0, 'snapshot': {'ticker': 'KXHIGHNY-26OCT10-B70', 'yes': [['0.9600', '50.00']], 'no': [['0.0200', '40.00']]}},
] + [{'t': 4000 + 250 * i, 'pyth': {'underlying': NATGAS, 'value': 3.10 + 0.001 * i}} for i in range(8)] + [
    {'t': 5000, 'delta': {'ticker': 'KXNATGAS15M-26OCT101500-00', 'side': 'yes', 'price': '0.4600', 'delta': '10.00'}},
    {'t': 5500, 'trade': {'ticker': 'KXHIGHNY-26OCT10-B70', 'taker_book_side': 'bid', 'price': '0.9800', 'count': '3.00'}},
]


HAVE_PYARROW = importlib.util.find_spec('pyarrow') is not None


@unittest.skipUnless(HAVE_PYARROW, 'pyarrow is not installed (pip install -r recorder/requirements.txt)')
@unittest.skipUnless(os.path.exists(FAKE), 'build build-deskd/fake-kalshi first (or set FAKE_KALSHI)')
class RecorderAgainstFake(unittest.TestCase):
    def test_record(self):
        d = tempfile.mkdtemp()
        fake = rec = None
        try:
            with open(os.path.join(d, 'scen.jsonl'), 'w') as f:
                f.write('\n'.join(json.dumps(e) for e in sorted(SCENARIO, key=lambda e: e['t'])) + '\n')
            os.mkdir(os.path.join(d, 'tls'))
            fake = subprocess.Popen([FAKE, '--port', '0', '--scenario', os.path.join(d, 'scen.jsonl'), '--auth', 'presence',
                                     '--tls', os.path.join(d, 'tls')], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            port = None
            for _ in range(20):
                line = fake.stdout.readline()
                if 'REST https://127.0.0.1:' in line:
                    port = int(line.rsplit(':', 1)[1].split('/')[0])
                    break
            self.assertIsNotNone(port, 'the fake did not start')
            key = ed25519.Ed25519PrivateKey.generate()
            with open(os.path.join(d, 'key.pem'), 'wb') as f:
                f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                          serialization.NoEncryption()))
            with open(os.path.join(d, 'key.id'), 'w') as f:
                f.write('fake-key')
            data = os.path.join(d, 'data')
            env = dict(os.environ, KALSHI_REST='https://127.0.0.1:%d/trade-api/v2' % port,
                       KALSHI_WS='wss://127.0.0.1:%d/trade-api/ws/v2' % port, REC_DATA_DIR=data,
                       REC_CA_FILE=os.path.join(d, 'tls', 'ca.pem'), KALSHI_KEY_FILE=os.path.join(d, 'key.pem'),
                       KALSHI_KEY_ID_FILE=os.path.join(d, 'key.id'), REC_BOOK_SERIES='KXNATGAS15M,KXHIGHNY',
                       REC_DISCOVER_S='1', REC_LEAD_S='1200')
            env.pop('NOTIFY_SOCKET', None)
            rec = subprocess.Popen([sys.executable, '-m', 'recorder.rec'], cwd=ROOT, env=env, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
            time.sleep(7.5)
            rec.send_signal(signal.SIGTERM)
            out, _ = rec.communicate(timeout=20)
            self.assertEqual(rec.returncode, 0, out)
            self.assertIn('recorder: stopped', out)
            # every spool, compacted as the next run (or hour) would
            sys.path.insert(0, ROOT)
            from recorder.store import Store, read_manifest
            import pyarrow.parquet as pq
            s = Store(data, 3600, lambda m: None)
            s.roll(int(time.time() * 1e6) + 10 * 3600 * 1_000_000)
            tables = {}
            for e in read_manifest(data):
                tables.setdefault(e['table'], []).extend(pq.read_table(os.path.join(data, e['path'])).to_pylist())
            ws, mk = tables.get('kalshi_ws', []), tables.get('kalshi_markets', [])
            types = {r['type'] for r in ws}
            self.assertTrue({'subscribed', 'orderbook_snapshot', 'orderbook_delta', 'pyth_value'} <= types, types)
            self.assertIn('trade', types)
            booked = {r['ticker'] for r in ws if r['type'] == 'orderbook_snapshot'}
            self.assertEqual(booked, {'KXNATGAS15M-26OCT101500-00', 'KXNATGAS15M-26OCT101515-15', 'KXHIGHNY-26OCT10-B70'})
            pyth = [json.loads(r['raw'])['msg']['value_usd'] for r in ws if r['type'] == 'pyth_value']
            self.assertGreaterEqual(len(pyth), 6)
            strikes = {r['ticker']: r['floor_strike'] for r in mk}
            self.assertEqual(strikes.get('KXNATGAS15M-26OCT101500-00'), 3.105)
            self.assertIn('KXNATGAS15M-26OCT101800-00', strikes)   # listed (recorded), not subscribed
            # every frame's subscription, in order: no sequence gap and no empty market list sent
            self.assertEqual([g for g in tables.get('gaps', []) if g['kind'] == 'seq_gap'], [])
        finally:
            for p in (rec, fake):
                if p and p.poll() is None:
                    p.kill()
                    p.wait()
            if fake and fake.stdout:
                fake.stdout.close()
            shutil.rmtree(d, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()
