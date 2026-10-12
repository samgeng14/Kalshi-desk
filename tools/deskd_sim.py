#!/usr/bin/env python3
"""deskd against the fake exchange: a paper-trading sandbox with no network and no key.

    python3 tools/deskd_sim.py                 # then open the URL it prints
    python3 tools/deskd_sim.py --port 8790 --minutes 60

It starts build-deskd/fake-kalshi over TLS with a scripted market day and build-deskd/deskd
with --sim pointing at it: Manual and Systematic orders on, a throwaway RSA key (the fake does
not check signatures), its own data and lock directories under a temporary directory, removed
on exit. Ctrl-C stops both.

The market day (all on the fake's clock, starting now):
  - KXNATGAS15M windows every 15 minutes (the open one and the next two listed ahead), each with
    a strike, a two-sided street book and natural gas's pyth_value every half second, so the
    15-minute fair and lane M1 (LIP-15) have what they need
  - KXHIGHNY bands with a 0.97 favourite, for lane M2 (BAND 96-99)
  - KXSIM-PRACTICE, an ordinary 1c market with a book on both sides, for Manual clicks
  - the street trading now and then on every market
The fake is a sandbox: its matching, fees and books are close to Kalshi's, not Kalshi itself.
Nothing here reaches Kalshi, demo or production.
"""
import argparse
import json
import math
import os
import random
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build-deskd')
NATGAS = 'Commodities.Index.NATGAS/USD'


def window_ticker(close_s):
    t = time.gmtime(close_s - 5 * 3600)   # Kalshi names a window by its ET close (EST here; the fake does not care)
    return 'KXNATGAS15M-%02d%s%02d%02d%02d-%02d' % (
        t.tm_year % 100, ['JAN', 'FEB', 'MAR', 'APR', 'MAY', 'JUN', 'JUL', 'AUG', 'SEP', 'OCT', 'NOV', 'DEC'][t.tm_mon - 1],
        t.tm_mday, t.tm_hour, t.tm_min, t.tm_min)


def scenario(minutes, seed=7):
    """The fake's scenario lines (tests/fake_exchange/tape.h), t in ms from now."""
    rng = random.Random(seed)
    now_s = int(time.time())
    first_close = (now_s // 900 + 1) * 900
    ev = []
    spot = 3.10
    # 15-minute windows: open now, then every 15 minutes, each listed 20 minutes before it opens
    n_windows = minutes // 15 + 3
    for k in range(n_windows):
        close_s = first_close + 900 * k
        open_s = close_s - 900
        list_at = max(0, (open_s - 1200 - now_s) * 1000)
        tk = window_ticker(close_s)
        ev.append({'t': list_at, 'market': {
            'ticker': tk, 'exchange_index': 0, 'ticks': 'tapered_deci_cent', 'fee_type': 'quadratic', 'fee_multiplier': 0,
            'open_in_ms': (open_s - now_s) * 1000 - list_at, 'close_in_ms': (close_s - now_s) * 1000 - list_at,
            'strike_type': 'greater_or_equal', 'floor_strike': round(spot, 3)}})
        ev.append({'t': list_at, 'snapshot': {'ticker': tk, 'yes': [['0.4500', '40.00'], ['0.4400', '60.00']],
                                              'no': [['0.5100', '40.00'], ['0.5000', '60.00']]}})
    # weather: a favourite at 0.97 and the rest of the ladder
    for strike, bid in (('B70', '0.9700'), ('B72', '0.0200'), ('B68', '0.0100')):
        tk = 'KXHIGHNY-SIM-' + strike
        ev.append({'t': 0, 'market': {'ticker': tk, 'exchange_index': 0, 'ticks': 'linear_cent', 'close_in_ms': minutes * 60000 + 3600000}})
        b = float(bid)
        ev.append({'t': 0, 'snapshot': {'ticker': tk, 'yes': [[bid, '50.00']],
                                        'no': [['%.4f' % (1 - b - 0.02), '30.00']]}})
    # the practice market
    ev.append({'t': 0, 'market': {'ticker': 'KXSIM-PRACTICE', 'exchange_index': 0, 'ticks': 'linear_cent',
                                  'close_in_ms': minutes * 60000 + 3600000}})
    ev.append({'t': 0, 'snapshot': {'ticker': 'KXSIM-PRACTICE', 'yes': [['0.4000', '200.00'], ['0.3900', '300.00']],
                                    'no': [['0.5800', '150.00'], ['0.5700', '300.00']]}})
    # the underlying, every half second, a random walk; the street trades now and then
    for i in range(minutes * 120):
        t = i * 500
        spot = max(0.5, spot * math.exp(rng.gauss(0, 0.0004)))
        ev.append({'t': t, 'pyth': {'underlying': NATGAS, 'value': round(spot, 5)}})
        if i % 40 == 20:
            side = rng.choice(('bid', 'ask'))
            ev.append({'t': t, 'trade': {'ticker': 'KXSIM-PRACTICE', 'taker_book_side': side,
                                         'price': '0.4100' if side == 'bid' else '0.3900', 'count': '%d.00' % rng.randint(1, 5)}})
    ev.sort(key=lambda e: e['t'])
    return ev


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--port', type=int, default=8790, help='deskd web port (default 8790)')
    ap.add_argument('--minutes', type=int, default=120, help='how long the scripted day runs (default 120)')
    ap.add_argument('--keep', action='store_true', help='keep the temporary directory (logs, data) on exit')
    a = ap.parse_args()
    fake, deskd = os.path.join(BUILD, 'fake-kalshi'), os.path.join(BUILD, 'deskd')
    for b in (fake, deskd):
        if not os.path.exists(b):
            sys.exit('%s is missing: build first (cd build-deskd && nice -n 15 ninja -j 4 deskd fake-kalshi)' % b)
    work = tempfile.mkdtemp(prefix='deskd-sim-')
    procs = []
    try:
        for d in ('tls', 'data', 'lock'):
            os.mkdir(os.path.join(work, d))
        with open(os.path.join(work, 'scenario.jsonl'), 'w') as f:
            for e in scenario(a.minutes):
                f.write(json.dumps(e) + '\n')
        key = os.path.join(work, 'sim-key.pem')
        subprocess.run(['openssl', 'genrsa', '-out', key, '2048'], check=True, capture_output=True)
        os.chmod(key, 0o600)
        cfg = {'env': 'demo', 'key_id': 'sim', 'pem_path': key, 'key_type': 'rsa',
               'deskd': {'lock_dir': os.path.join(work, 'lock'), 'manual_orders': True, 'systematic_orders': True,
                         'lip_series': ['KXNATGAS15M'], 'band_series': ['KXHIGHNY']}}
        with open(os.path.join(work, 'deskd-sim.json'), 'w') as f:
            json.dump(cfg, f, indent=2)
        fake_log = open(os.path.join(work, 'fake.log'), 'w')
        procs.append(subprocess.Popen([fake, '--port', '0', '--scenario', os.path.join(work, 'scenario.jsonl'),
                                       '--auth', 'presence', '--tls', os.path.join(work, 'tls'), '--cash', '0:500'],
                                      stdout=subprocess.PIPE, stderr=fake_log, text=True))
        base = None
        for _ in range(50):
            line = procs[0].stdout.readline()
            if not line:
                break
            if 'REST https://127.0.0.1:' in line:
                base = line.split('REST ', 1)[1].strip()
                break
        if not base:
            sys.exit('fake-kalshi did not start; see %s/fake.log' % work)
        deskd_log = open(os.path.join(work, 'deskd.log'), 'w')
        procs.append(subprocess.Popen([deskd, '--config', os.path.join(work, 'deskd-sim.json'), '--data-dir',
                                       os.path.join(work, 'data'), '--port', str(a.port), '--sim', base,
                                       '--sim-ca', os.path.join(work, 'tls', 'ca.pem')],
                                      stdout=deskd_log, stderr=subprocess.STDOUT))
        print('fake exchange: %s ($500 on shard 0)' % base)
        print('deskd:         http://127.0.0.1:%d/   (log: %s/deskd.log)' % (a.port, work))
        print('Manual: click KXSIM-PRACTICE\'s ladder. Systematic: SYSTEMATIC tab, lane M1 or M2 on, Start once GROUPS passes.')
        print('Ctrl-C stops both.')
        sys.stdout.flush()
        while all(p.poll() is None for p in procs):
            time.sleep(0.5)
        for p, name in zip(procs, ('fake-kalshi', 'deskd')):
            if p.poll() is not None:
                print('%s exited (%s); see %s' % (name, p.returncode, work))
    except KeyboardInterrupt:
        pass
    finally:
        for p in reversed(procs):
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
                try:
                    p.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    p.kill()
        if a.keep:
            print('kept %s' % work)
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
