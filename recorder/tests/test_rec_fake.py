"""The recorder against the fake exchange (tests/fake_exchange, built as build-deskd/fake-kalshi),
as a process, over TLS, with ESPN's and NCAA.com's scoreboards served from loopback (fixtures):
the windows it subscribes (the open one and the next, not one hours away), the weather market, a
women's college game and the thinnest men's game (the others not), an NBA game in its window
(socket books only), books, trades, lifecycle and pyth_value to the spool, the college books by
REST, a restart's trades backfilled by REST, the daily market sweep and the incentive sweep,
each window's REST row with its strike, the NBA game in progress's plays from its summary, a
clean exit on SIGTERM (systemd's stop), and the spools compacted to Parquet afterwards.

Nothing here leaves the machine: Kalshi is the fake on 127.0.0.1 and the scoreboards a local
server; the recorder's environment is scrubbed of every REC_, KALSHI_ and credential setting."""
import http.server
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest

from recorder.tests.util import HAVE_CRYPTO, HAVE_PYARROW, HAVE_WEBSOCKETS, ROOT, fixture

FAKE = os.environ.get('FAKE_KALSHI', os.path.join(ROOT, 'build-deskd', 'fake-kalshi'))
NATGAS = 'Commodities.Index.NATGAS/USD'
MON = 'JAN FEB MAR APR MAY JUN JUL AUG SEP OCT NOV DEC'.split()


def today():
    t = time.gmtime()
    return '%02d%s%02d' % (t.tm_year % 100, MON[t.tm_mon - 1], t.tm_mday)


D = today()
UCONN, SC = 'KXNCAAWBGAME-%sUCONNSC-UCONN' % D, 'KXNCAAWBGAME-%sUCONNSC-SC' % D
MB_BUSY, MB_THIN, MB_THIN2 = ('KXNCAAMBGAME-%sAAABBB-AAA' % D, 'KXNCAAMBGAME-%sCCCDDD-CCC' % D,
                              'KXNCAAMBGAME-%sEEEFFF-EEE' % D)
NBA = 'KXNBAGAME-%sLALBOS-BOS' % D   # the game group; REC_GAME_PRE_S below opens its window all day


def game(ticker):
    return {'t': 0, 'market': {'ticker': ticker, 'exchange_index': 0, 'ticks': 'linear_cent', 'close_in_ms': 10800000}}


SCENARIO = [
    {'t': 0, 'market': {'ticker': 'KXNATGAS15M-26OCT101500-00', 'exchange_index': 2, 'ticks': 'tapered_deci_cent',
                        'close_in_ms': 900000, 'strike_type': 'greater_or_equal', 'floor_strike': 3.105}},
    {'t': 0, 'market': {'ticker': 'KXNATGAS15M-26OCT101515-15', 'exchange_index': 2, 'ticks': 'tapered_deci_cent',
                        'open_in_ms': 600000, 'close_in_ms': 1500000, 'strike_type': 'greater_or_equal'}},
    {'t': 0, 'market': {'ticker': 'KXNATGAS15M-26OCT101800-00', 'exchange_index': 2, 'ticks': 'tapered_deci_cent',
                        'open_in_ms': 9000000, 'close_in_ms': 9900000}},
    {'t': 0, 'market': {'ticker': 'KXHIGHNY-26OCT10-B70', 'exchange_index': 0, 'ticks': 'linear_cent', 'close_in_ms': 36000000}},
    game(UCONN), game(SC), game(MB_BUSY), game(MB_THIN), game(MB_THIN2), game(NBA),
    {'t': 0, 'snapshot': {'ticker': NBA, 'yes': [['0.6000', '300.00']], 'no': [['0.3800', '250.00']]}},
    {'t': 0, 'snapshot': {'ticker': 'KXNATGAS15M-26OCT101500-00', 'yes': [['0.4500', '100.00']], 'no': [['0.5300', '80.00']]}},
    {'t': 0, 'snapshot': {'ticker': 'KXHIGHNY-26OCT10-B70', 'yes': [['0.9600', '50.00']], 'no': [['0.0200', '40.00']]}},
    {'t': 0, 'snapshot': {'ticker': UCONN, 'yes': [['0.4100', '12.00']], 'no': [['0.5500', '7.00']]}},
    {'t': 0, 'snapshot': {'ticker': MB_BUSY, 'yes': [['0.5000', '10.00']], 'no': [['0.4800', '10.00']]}},
    # before the recorder is up: read back by the restart's REST backfill, and the busy men's game's volume
    {'t': 200, 'trade': {'ticker': MB_BUSY, 'taker_book_side': 'bid', 'price': '0.5200', 'count': '3.00'}},
    {'t': 300, 'trade': {'ticker': UCONN, 'taker_book_side': 'bid', 'price': '0.4500', 'count': '2.00'}},
] + [{'t': 4000 + 250 * i, 'pyth': {'underlying': NATGAS, 'value': 3.10 + 0.001 * i}} for i in range(8)] + [
    {'t': 5000, 'delta': {'ticker': 'KXNATGAS15M-26OCT101500-00', 'side': 'yes', 'price': '0.4600', 'delta': '10.00'}},
    {'t': 5500, 'trade': {'ticker': 'KXHIGHNY-26OCT10-B70', 'taker_book_side': 'bid', 'price': '0.9800', 'count': '3.00'}},
]


class Boards(http.server.BaseHTTPRequestHandler):
    """ESPN's and NCAA.com's scoreboards, from the fixtures, on loopback."""
    paths = []

    def do_GET(self):
        Boards.paths.append(self.path)
        if self.path.startswith('/apis/site/v2/sports/basketball/nba/scoreboard?'):
            body = json.dumps(fixture('espn_nba_scoreboard.json')).encode()
        elif self.path.startswith('/apis/site/v2/sports/basketball/nba/summary?event='):
            body = json.dumps(fixture('espn_nba_summary.json')).encode()
        elif '/apis/site/v2/sports/' in self.path and '/scoreboard?' in self.path:
            body = json.dumps(fixture('espn_wcbb_scoreboard.json')).encode()
        elif self.path.startswith('/casablanca/scoreboard/') and self.path.endswith('/scoreboard.json'):
            body = json.dumps(fixture('ncaa_scoreboard.json')).encode()
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def clean_env():
    """The recorder's environment: nothing of the developer's recorder, Kalshi or R2 settings (no
    real host, no healthchecks ping, no credentials directory)."""
    return {k: v for k, v in os.environ.items()
            if not k.startswith(('REC_', 'KALSHI_', 'R2_')) and k not in ('CREDENTIALS_DIRECTORY', 'NOTIFY_SOCKET')}


@unittest.skipUnless(HAVE_PYARROW and HAVE_CRYPTO and HAVE_WEBSOCKETS,
                     'pyarrow, cryptography and websockets are needed (pip install -r recorder/requirements.txt)')
@unittest.skipUnless(os.path.exists(FAKE), 'build build-deskd/fake-kalshi first (or set FAKE_KALSHI)')
class RecorderAgainstFake(unittest.TestCase):
    def test_record(self):
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric import ed25519
        d = tempfile.mkdtemp()
        fake = rec = None
        boards = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Boards)
        threading.Thread(target=boards.serve_forever, daemon=True).start()
        try:
            with open(os.path.join(d, 'scen.jsonl'), 'w') as f:
                f.write('\n'.join(json.dumps(e) for e in sorted(SCENARIO, key=lambda e: e['t'])) + '\n')
            os.mkdir(os.path.join(d, 'tls'))
            data = os.path.join(d, 'data')
            # a previous run's last row, half a minute ago: this start is a restart (a gap, backfilled)
            from recorder.store import Store
            prev = Store(data, 3600, lambda m: None)
            prev.write('gaps', {'recv_us': int((time.time() - 30) * 1e6), 'source': 'test', 'kind': 'previous_run',
                                'detail': '', 'from_us': None, 'to_us': None})
            prev.close()
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
            local = 'http://127.0.0.1:%d' % boards.server_address[1]
            env = dict(clean_env(), KALSHI_REST='https://127.0.0.1:%d/trade-api/v2' % port,
                       KALSHI_WS='wss://127.0.0.1:%d/trade-api/ws/v2' % port, REC_DATA_DIR=data,
                       REC_CA_FILE=os.path.join(d, 'tls', 'ca.pem'), KALSHI_KEY_FILE=os.path.join(d, 'key.pem'),
                       KALSHI_KEY_ID_FILE=os.path.join(d, 'key.id'), REC_BOOK_SERIES='KXNATGAS15M,KXHIGHNY',
                       REC_COLLEGE_SERIES='KXNCAAWBGAME', REC_CONTROL_SERIES='KXNCAAMBGAME', REC_CONTROL_PICK_UTC_H='0',
                       REC_GAME_SERIES='KXNBAGAME', REC_GAME_PRE_S='86400',
                       REC_DISCOVER_S='1', REC_LEAD_S='1200', REC_REST_RPS='15', REC_REST_BOOK_RPS='15',
                       REC_REST_BOOK_S='1', REC_MARKETS_DAILY_UTC_H='0', REC_SCORES_S='3', REC_SUMMARY_S='3',
                       REC_ESPN_URL=local + '/apis/site/v2/sports',
                       REC_ESPN_LEAGUES='basketball/womens-college-basketball:50,basketball/nba',
                       REC_SUMMARY_LEAGUES='basketball/nba',
                       REC_NCAA_URL=local + '/casablanca/scoreboard', REC_NCAA_SPORTS='basketball-women/d1')
            rec = subprocess.Popen([sys.executable, '-m', 'recorder.rec'], cwd=ROOT, env=env, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
            time.sleep(8)
            rec.send_signal(signal.SIGTERM)
            out, _ = rec.communicate(timeout=20)
            self.assertEqual(rec.returncode, 0, out)
            self.assertIn('recorder: stopped', out)
            # every spool, compacted as the next run (or hour) would
            from recorder.store import read_manifest
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
            # the windows and the weather market; the women's game; of the men's, only the thinnest;
            # the NBA game in its window
            self.assertEqual(booked, {'KXNATGAS15M-26OCT101500-00', 'KXNATGAS15M-26OCT101515-15', 'KXHIGHNY-26OCT10-B70',
                                      UCONN, SC, MB_THIN, NBA}, out)
            channels = {json.loads(r['raw'])['msg']['channel'] for r in ws if r['type'] == 'subscribed'}
            self.assertIn('market_lifecycle_v2', channels)
            pyth = [json.loads(r['raw'])['msg']['value_usd'] for r in ws if r['type'] == 'pyth_value']
            self.assertGreaterEqual(len(pyth), 6)
            strikes = {r['ticker']: r['floor_strike'] for r in mk}
            self.assertEqual(strikes.get('KXNATGAS15M-26OCT101500-00'), 3.105)
            self.assertIn('KXNATGAS15M-26OCT101800-00', strikes)   # listed (recorded), not subscribed
            self.assertIn(MB_BUSY, strikes)                         # every men's game listed, one recorded
            control = {r['event_ticker']: r['picked'] for r in tables.get('college_control', [])}
            self.assertEqual(control, {MB_BUSY.rsplit('-', 1)[0]: False, MB_THIN.rsplit('-', 1)[0]: True,
                                       MB_THIN2.rsplit('-', 1)[0]: False})
            # the college books by REST: top of book for every college market, depth for the moving
            # ones; the NBA game's books are the socket's only
            books = tables.get('kalshi_rest_books', [])
            self.assertEqual({r['ticker'] for r in books if r['kind'] == 'top'}, {UCONN, SC, MB_THIN})
            depth = {r['ticker']: r for r in books if r['kind'] == 'depth'}
            self.assertIn(UCONN, depth)
            self.assertEqual((depth[UCONN]['yes_bid'], depth[UCONN]['yes_bid_size']), (0.41, 12.0))
            self.assertEqual({r['mode'] for r in books}, {'college'})
            # the trade before the recorder started, read back over the restart's window, once
            trades = tables.get('kalshi_trades_rest', [])
            mine = [t for t in trades if t['ticker'] == UCONN]
            self.assertEqual(len(mine), 1, trades)
            self.assertEqual((mine[0]['yes_price'], mine[0]['count'], mine[0]['reason']), (0.45, 2.0, 'restart'))
            self.assertEqual(len({t['trade_id'] for t in trades}), len(trades))
            daily = {r['ticker'] for r in tables.get('kalshi_markets_daily', [])}
            self.assertTrue({UCONN, SC, MB_BUSY, 'KXHIGHNY-26OCT10-B70'} <= daily, daily)
            self.assertIn('incentives: 0 kept', out)   # the fake lists none; asked, answered, no gap
            espn = tables.get('espn_scores', [])
            self.assertEqual(sorted(r['game_id'] for r in espn if r['league'] != 'basketball/nba'),
                             ['401820001', '401820002', '401820003'])
            self.assertEqual(sorted(r['game_id'] for r in espn if r['league'] == 'basketball/nba'),
                             ['401810100', '401810101', '401810102'])
            self.assertEqual(len(tables.get('ncaa_scores', [])), 3)
            self.assertTrue(any('/basketball/womens-college-basketball/scoreboard?dates=' in p and 'groups=50' in p
                                for p in Boards.paths), Boards.paths)
            # the NBA game in progress: its summary read, each play once; never the others'
            plays = tables.get('espn_plays', [])
            self.assertEqual(sorted(r['play_id'][-3:] for r in plays), ['001', '004', '140', '142'])
            self.assertEqual({p.split('event=')[1] for p in Boards.paths if '/summary?' in p}, {'401810101'})
            gaps = tables.get('gaps', [])
            kinds = {(g['source'], g['kind']) for g in gaps}
            self.assertIn(('recorder', 'restart'), kinds)
            # every frame's subscription, in order: no sequence gap, no refusal, no failed poll
            self.assertEqual([g for g in gaps if g['kind'] in ('seq_gap', 'subscribe_error', 'poll_failed',
                                                               'discovery_failed', 'sweep_failed')], [])
        finally:
            for p in (rec, fake):
                if p and p.poll() is None:
                    p.kill()
                    p.wait()
            if fake and fake.stdout:
                fake.stdout.close()
            boards.shutdown()
            boards.server_close()
            shutil.rmtree(d, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()
