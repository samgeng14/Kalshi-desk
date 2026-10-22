#!/usr/bin/env python3
"""deskd against the fake exchange: a paper-trading sandbox with no network and no key.

    python3 tools/deskd_sim.py                 # then open the URL it prints
    python3 tools/deskd_sim.py --port 8790 --minutes 60
    python3 tools/deskd_sim.py --nba           # and an NBA game, with a stand-in for ESPN

It starts build-deskd/fake-kalshi over TLS with a scripted market day and build-deskd/deskd
with --sim pointing at it: Manual and Systematic orders on, a throwaway RSA key (the fake does
not check signatures), its own data and lock directories under a temporary directory, removed
on exit. Ctrl-C stops both.

The market day (all on the fake's clock, starting now):
  - KXNATGAS15M windows every 15 minutes (the open one and the next two listed ahead), each with
    a strike, a two-sided street book and natural gas's pyth_value every half second, so the
    15-minute fair and lane M1 (LIP-15) have what they need
  - today's KXHIGHNY bands with a 0.97 favourite, for lane M2 (BAND 96-99)
  - KXSIM-PRACTICE, an ordinary 1c market with a book on both sides, for Manual clicks
  - the street trading now and then on every market
The fake is a sandbox: its matching, fees and books are close to Kalshi's, not Kalshi itself.
Nothing here reaches Kalshi, demo or production.

--nba adds the NBA lanes (docs/nba_lane.md) and a game for them: KXNBAGAME's Lakers at Boston,
scheduled about 15 minutes on, and a stand-in for ESPN's NBA scoreboard on loopback, which deskd
reads with --espn-url (only with --sim, only on loopback: the sandbox never reaches ESPN). deskd
runs with deskd.nba_lanes, the synthetic NBA model (tests/deskd/fixtures/nba_synthetic.json) and
hand-written gates in its data dir marked SANDBOX: X1 at stage shadow (fade at 12c), N1 live at
10c, N2 live. Every lane starts off; each records in shadow until the operator turns it on and
presses Start. The game, in minutes from the start:
  0     Boston 0.87 / 0.88, the Lakers 0.12 / 0.13: no favourite in N2's band yet
  3     Boston 0.91 / 0.92: N2 buys 5 if it is on and Systematic runs (else a SHADOW row)
  15    the tip; ESPN gives Boston 90%
  16    the Lakers 5 up with 5:00 left, ESPN 30%: Boston's book still 0.91 / 0.92, so N1 takes the
        Lakers at 0.09 (the model's fair about 0.77) and X1's fade is a SHADOW row
  18    Kalshi reprices (Boston 0.28 / 0.30); 19 the last minute; 21 final, the Lakers by 5; 22
        both markets close
--check runs that script in a minute (Boston's band at 5 s, the run at 20 s), waits until X1, N1
and N2 have each recorded the game in shadow with nothing sent, says what it saw, and exits 0 or
1 (ctest deskd-sim-nba).
"""
import argparse
import hashlib
import http.server
import json
import math
import os
import random
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build-deskd')
NATGAS = 'Commodities.Index.NATGAS/USD'


MONTHS = ['JAN', 'FEB', 'MAR', 'APR', 'MAY', 'JUN', 'JUL', 'AUG', 'SEP', 'OCT', 'NOV', 'DEC']
NBA_MODEL = os.path.join(ROOT, 'tests', 'deskd', 'fixtures', 'nba_synthetic.json')   # synthetic: the sandbox's only
# The NBA game's script, seconds from the start (--nba; the docstring's table), and --check's
NBA_TIMES = {'band': 180, 'tip': 900, 'run': 960, 'reprice': 1080, 'late': 1140, 'final': 1260, 'close': 1320}
NBA_CHECK_TIMES = {'band': 5, 'tip': 15, 'run': 20, 'reprice': 40, 'late': 45, 'final': 50, 'close': 55}
SANDBOX_TOOL = 'tools/deskd_sim.py --nba (SANDBOX: written for the fake exchange, not a measurement)'


def window_ticker(close_s):
    t = time.gmtime(close_s - 4 * 3600)   # Kalshi names a window by its ET close (EDT here; the fake does not care)
    return 'KXNATGAS15M-%02d%s%02d%02d%02d-%02d' % (t.tm_year % 100, MONTHS[t.tm_mon - 1], t.tm_mday, t.tm_hour, t.tm_min, t.tm_min)


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
    # weather: today's NYC high (named by its ET day, as Kalshi's), a favourite at 0.97 and the rest
    et = time.gmtime(now_s - 4 * 3600)
    day = '%02d%s%02d' % (et.tm_year % 100, MONTHS[et.tm_mon - 1], et.tm_mday)
    for strike, bid in (('B70', '0.9700'), ('B72', '0.0200'), ('B68', '0.0100')):
        tk = 'KXHIGHNY-%s-%s' % (day, strike)
        ev.append({'t': 0, 'market': {'ticker': tk, 'exchange_index': 0, 'ticks': 'linear_cent', 'open_in_ms': -60000,
                                      'close_in_ms': minutes * 60000 + 3600000}})
        b = float(bid)
        ev.append({'t': 0, 'snapshot': {'ticker': tk, 'yes': [[bid, '50.00']],
                                        'no': [['%.4f' % (1 - b - 0.02), '30.00']]}})
    # the practice market
    ev.append({'t': 0, 'market': {'ticker': 'KXSIM-PRACTICE', 'exchange_index': 0, 'ticks': 'linear_cent', 'open_in_ms': -60000,
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


def et_day(ts):
    """Kalshi's name for ts's Eastern day ('26OCT22'), as deskd dates an NBA event."""
    try:
        from zoneinfo import ZoneInfo
        import datetime
        t = datetime.datetime.fromtimestamp(ts, ZoneInfo('America/New_York')).timetuple()
    except Exception:   # no tz database: EDT, as the weather day above
        t = time.gmtime(ts - 4 * 3600)
    return '%02d%s%02d' % (t.tm_year % 100, MONTHS[t.tm_mon - 1], t.tm_mday)


def nba_event(now_s):
    """Today's game: its event and Boston's and the Lakers' markets."""
    ev = 'KXNBAGAME-%sLALBOS' % et_day(now_s)
    return ev, ev + '-BOS', ev + '-LAL'


def nba_scenario(now_s, times, minutes):
    """The game's markets and books on the fake (scenario lines, t in ms from now): Boston's touch
    at each step of the script, the Lakers' its mirror, 30 deep; both close after the final."""
    _, bos, lal = nba_event(now_s)
    ev = []
    for tk in (bos, lal):
        ev.append({'t': 0, 'market': {'ticker': tk, 'exchange_index': 0, 'ticks': 'linear_cent', 'open_in_ms': -60000,
                                      'close_in_ms': minutes * 60000 + 3600000}})
    for at, bid, ask in ((0, 0.87, 0.88), (times['band'], 0.91, 0.92), (times['reprice'], 0.28, 0.30),
                         (times['late'], 0.04, 0.05)):
        ev.append({'t': at * 1000, 'snapshot': {'ticker': bos, 'yes': [['%.4f' % bid, '30.00']],
                                                'no': [['%.4f' % (1 - ask), '30.00']]}})
        ev.append({'t': at * 1000, 'snapshot': {'ticker': lal, 'yes': [['%.4f' % (1 - ask), '30.00']],
                                                'no': [['%.4f' % bid, '30.00']]}})
    for tk in (bos, lal):
        ev.append({'t': times['close'] * 1000, 'close': {'ticker': tk}})
    return ev


class EspnStandIn:
    """ESPN's NBA scoreboard for the scripted game, on loopback (deskd --espn-url): the Lakers at
    Boston, scheduled at start_s, played by times from t0: before the tip, the tip, the Lakers 5
    up with 5:00 left, the last minute, the final; ESPN's number in situation.lastPlay. Anything
    else (a summary, another league) is 404: deskd then reads the scoreboard's number."""

    def __init__(self, t0, start_s, times):
        self.t0, self.start_s, self.times = t0, start_s, times
        board = self.scoreboard

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                if not self.path.startswith('/apis/site/v2/sports/basketball/nba/scoreboard'):
                    self.send_error(404)
                    return
                body = json.dumps(board(time.time())).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass
        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.url = 'http://127.0.0.1:%d' % self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def scoreboard(self, now_s):
        t, x = now_s - self.t0, self.times
        if t < x['tip']:
            state, period, clock, bos, lal, play = 'pre', 0, '0:00', 0, 0, None
        elif t < x['run']:
            state, period, clock, bos, lal, play = 'in', 1, '12:00', 0, 0, (1, 0.90)
        elif t < x['late']:
            state, period, clock, bos, lal, play = 'in', 4, '5:00', 95, 100, (2, 0.30)
        elif t < x['final']:
            state, period, clock, bos, lal, play = 'in', 4, '1:00', 99, 104, (3, 0.05)
        else:
            state, period, clock, bos, lal, play = 'post', 4, '0:00', 101, 106, None

        def side(home_away, abbr, name, score):
            return {'homeAway': home_away, 'score': str(score), 'team': {'abbreviation': abbr, 'displayName': name}}
        c = {'status': {'period': period, 'displayClock': clock,
                        'type': {'state': state, 'shortDetail': 'Final' if state == 'post' else '-'}},
             'competitors': [side('away', 'LAL', 'Los Angeles Lakers', lal), side('home', 'BOS', 'Boston Celtics', bos)]}
        if play:
            c['situation'] = {'lastPlay': {'id': '40190200%d' % play[0],
                                           'probability': {'homeWinPercentage': play[1], 'tiePercentage': 0.0}}}
        return {'events': [{'id': '401902000', 'date': time.strftime('%Y-%m-%dT%H:%MZ', time.gmtime(self.start_s)),
                            'competitions': [c]}]}

    def stop(self):
        self.server.shutdown()


def sandbox_gates(gates_dir):
    """The NBA lanes' gates in deskd's data dir (strat/lane_gate.h's format), hand-written for the
    sandbox and marked so: X1 at stage shadow (fade, 12c), N1 live at 10c on the synthetic model,
    N2 live (5 contracts, 0.90-0.95). Each lane still starts off."""
    with open(NBA_MODEL, 'rb') as fh:
        sha = hashlib.sha256(fh.read()).hexdigest()
    os.makedirs(gates_dir)
    computed = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
    for lane, stage, params, model in (('X1', 'shadow', {'direction': 'fade', 'thr': 0.12}, None),
                                       ('N1', 'live', {'thr': 0.10}, sha),
                                       ('N2', 'live', {'count': 5, 'fav_lo': 0.90, 'fav_hi': 0.95}, None)):
        g = {'lane': lane, 'stage': stage, 'verdict': 'PASS', 'why': None, 'computed': computed, 'tool': SANDBOX_TOOL,
             'params': params, 'evidence': {}}
        if model:
            g['model_sha256'] = model
        with open(os.path.join(gates_dir, lane + '.json'), 'w') as fh:
            json.dump(g, fh, indent=1)


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def check_nba(port, bos, gates_dir, procs, limit_s=150):
    """--check: deskd read the stand-in's game, and X1, N1 and N2 each recorded it in shadow with
    nothing sent. 0 or 1."""
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))   # loopback, never a proxy
    status, lanes = {}, ('X1', 'N1', 'N2')
    end = time.time() + limit_s
    while time.time() < end and all(p.poll() is None for p in procs):
        try:
            with opener.open('http://127.0.0.1:%d/ops/status' % port, timeout=2) as r:
                status = json.load(r)
        except (OSError, ValueError):
            pass
        nba = status.get('nba') or {}
        if all(((nba.get(lane) or {}).get('engine') or {}).get('shadows', 0) >= 1 for lane in lanes):
            break
        time.sleep(0.5)
    nba, espn = status.get('nba') or {}, status.get('espn') or {}
    eng = {lane: (nba.get(lane) or {}).get('engine') or {} for lane in lanes}
    shadow_why = os.path.join(gates_dir, 'X1.json') + ': stage shadow (shadow signals only until its gate is live)'
    checks = [
        ('deskd runs the NBA lanes on the sandbox gates: X1 at stage shadow, N1 and N2 live',
         nba.get('enabled') is True and (nba.get('X1') or {}).get('why') == shadow_why
         and (nba.get('N1') or {}).get('why') == '' and (nba.get('N2') or {}).get('why') == ''),
        ("ESPN's scoreboard came from the stand-in, the game matched",
         espn.get('requests', 0) > 0 and bos in (espn.get('markets') or {})),
        ("N2 recorded Boston, a 0.92 favourite before the tip, in shadow", eng['N2'].get('shadows', 0) >= 1),
        ("N1 recorded the Lakers' run against Boston's stale book in shadow", eng['N1'].get('shadows', 0) >= 1),
        ("X1 recorded its fade of ESPN's number in shadow", eng['X1'].get('shadows', 0) >= 1),
        ('every lane off: nothing sent', all(e.get('takes', -1) == 0 for e in eng.values())),
    ]
    for what, ok in checks:
        print('%s %s' % ('ok  ' if ok else 'FAIL', what))
    if not all(ok for _, ok in checks):
        print('nba: %s\nespn: %s' % (json.dumps(nba)[:4000], json.dumps(espn)[:2000]))
        return 1
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--port', type=int, help='deskd web port (default 8790; --check: a free one)')
    ap.add_argument('--minutes', type=int, default=120, help='how long the scripted day runs (default 120)')
    ap.add_argument('--keep', action='store_true', help='keep the temporary directory (logs, data) on exit')
    ap.add_argument('--nba', action='store_true', help='an NBA game for X1, N1 and N2, with a loopback stand-in for ESPN')
    ap.add_argument('--check', action='store_true', help='--nba in a minute: check the lanes recorded it, then exit')
    ap.add_argument('--build', default=BUILD, help='the build directory with deskd and fake-kalshi (default build-deskd)')
    a = ap.parse_args()
    if a.check:
        a.nba, a.minutes = True, 5
    port = a.port or (free_port() if a.check else 8790)
    fake, deskd = os.path.join(a.build, 'fake-kalshi'), os.path.join(a.build, 'deskd')
    for b in (fake, deskd):
        if not os.path.exists(b):
            sys.exit('%s is missing: build first (cd build-deskd && nice -n 15 ninja -j 4 deskd fake-kalshi)' % b)
    # SIGTERM stops both as Ctrl-C does (a background job ignores SIGINT)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    work = tempfile.mkdtemp(prefix='deskd-sim-')
    procs, espn, rc = [], None, 0
    try:
        for d in ('tls', 'data', 'lock'):
            os.mkdir(os.path.join(work, d))
        now_s = time.time()
        times = NBA_CHECK_TIMES if a.check else NBA_TIMES
        events = scenario(a.minutes)
        if a.nba:
            events = sorted(events + nba_scenario(now_s, times, a.minutes), key=lambda e: e['t'])
            espn = EspnStandIn(now_s, (int(now_s) // 60 + 16) * 60, times)   # the tip 15-16 minutes on
            sandbox_gates(os.path.join(work, 'data', 'gates'))
        with open(os.path.join(work, 'scenario.jsonl'), 'w') as f:
            for e in events:
                f.write(json.dumps(e) + '\n')
        key = os.path.join(work, 'sim-key.pem')
        subprocess.run(['openssl', 'genrsa', '-out', key, '2048'], check=True, capture_output=True)
        os.chmod(key, 0o600)
        cfg = {'env': 'demo', 'key_id': 'sim', 'pem_path': key, 'key_type': 'rsa',
               'deskd': {'lock_dir': os.path.join(work, 'lock'), 'manual_orders': True, 'systematic_orders': True,
                         'lip_series': ['KXNATGAS15M'], 'band_series': ['KXHIGHNY']}}
        if a.nba:
            cfg['deskd'].update({'nba_lanes': True, 'nba_model': NBA_MODEL, 'lane_shadow': True})
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
                                       os.path.join(work, 'data'), '--port', str(port), '--sim', base,
                                       '--sim-ca', os.path.join(work, 'tls', 'ca.pem')]
                                      + (['--espn-url', espn.url] if espn else []),
                                      stdout=deskd_log, stderr=subprocess.STDOUT))
        print('fake exchange: %s ($500 on shard 0)' % base)
        print('deskd:         http://127.0.0.1:%d/   (log: %s/deskd.log)' % (port, work))
        if a.nba:
            ev, bos, _ = nba_event(now_s)
            print('NBA:           %s, the Lakers at Boston, tip %s UTC; ESPN is the stand-in at %s' % (
                ev, time.strftime('%H:%M', time.gmtime(espn.start_s)), espn.url))
            print('               gates (SANDBOX, not measurements): %s/data/gates; every lane starts off' % work)
        if a.check:
            rc = check_nba(port, bos, os.path.join(work, 'data', 'gates'), procs)
            return rc
        print('Manual: click KXSIM-PRACTICE\'s ladder. Systematic: SYSTEMATIC tab, lane M1 or M2 on, Start once GROUPS passes.')
        if a.nba:
            print('NBA: N2 on and Start within 3 minutes, and at +3:00 it buys Boston, the 0.92 favourite; N1 on, and at')
            print('     +16:00 it takes the Lakers against Boston\'s stale book. Off, each records SHADOW rows (X1 always).')
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
        if espn:
            espn.stop()
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
    return rc


if __name__ == '__main__':
    sys.exit(main())
