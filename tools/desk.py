#!/usr/bin/env python3
"""Start the desk: PAPER (the Kalshi demo account, play money) or LIVE (production, real money).

    python3 tools/desk.py setup                  # once: PAPER's config and data dir
    python3 tools/desk.py setup live             # once, for LIVE (key ~/.kalshi/prod.json, or --key FILE)
    python3 tools/desk.py                        # the desk: PAPER, and LIVE too after you type LIVE
    python3 tools/desk.py paper | live           # one of them alone
    python3 tools/desk.py sim                    # the fake exchange, no network, no key
    python3 tools/desk.py status                 # both, through deskctl
    python3 tools/desk.py kill paper|live        # KILL, as the screen's button

One desk, one browser tab, a PAPER | LIVE toggle in the header. Underneath, each account is its
own deskd (its own lock, order ledger, venue connections; PAPER on port 8788, LIVE on 8789), so a
fault in one never touches the other's orders; the toggle flips the tab between them, asking
first before LIVE. Nothing here starts LIVE by itself: only from a terminal, after you type LIVE.

Files (setup writes them only when missing; it never overwrites one):
  paper: key ~/.kalshi/demo.json, config ~/.kalshi/deskd-demo.json, data <data>/demo
  live:  key ~/.kalshi/prod.json (or --key), config ~/.kalshi/deskd-prod.json, data <data>/prod
  <data>: ~/Library/Application Support/kalshi-desk on a Mac, else $XDG_DATA_HOME/kalshi-desk
A key file is {"env": "demo"|"prod", "key_id", "private_key_path" (or "pem_path"), "key_type"}.
The paper config turns Manual and Systematic orders on; the live one Manual only (Systematic
stays off until you set "systematic_orders": true in it yourself).
"""
import argparse
import json
import os
import signal
import subprocess
import sys
import time
import urllib.request
import webbrowser

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build-deskd')
HOME = os.path.expanduser('~')
if sys.platform == 'darwin':
    DATA = os.path.join(HOME, 'Library', 'Application Support', 'kalshi-desk')
else:
    DATA = os.path.join(os.environ.get('XDG_DATA_HOME') or os.path.join(HOME, '.local', 'share'), 'kalshi-desk')

DESKS = {
    'paper': {'env': 'demo', 'key': '~/.kalshi/demo.json', 'config': '~/.kalshi/deskd-demo.json',
              'data': os.path.join(DATA, 'demo'), 'port': 8788, 'peer': 8789, 'flags': [],
              'what': 'PAPER: the Kalshi demo account (play money)', 'systematic': True},
    'live': {'env': 'prod', 'key': '~/.kalshi/prod.json', 'config': '~/.kalshi/deskd-prod.json',
             'data': os.path.join(DATA, 'prod'), 'port': 8789, 'peer': 8788, 'flags': ['--live'],
             'what': 'LIVE: the production Kalshi account (real money)', 'systematic': False},
}


def say(msg):
    print('desk: ' + msg, flush=True)


def die(msg, code=2):
    print('desk: ' + msg, file=sys.stderr, flush=True)
    sys.exit(code)


def read_json(path):
    """deskd's configs are JSON with // comments (as the old desk's); strip whole-line ones."""
    with open(path, encoding='utf-8') as f:
        text = '\n'.join(line for line in f.read().splitlines() if not line.lstrip().startswith('//'))
    return json.loads(text)


_noted = set()


def binary(name):
    path = os.path.join(BUILD, name)
    if name in _noted:
        return path
    _noted.add(name)
    if not os.path.exists(path):
        die('%s is missing: build it first (cd build-deskd && nice -n 15 ninja -j 4 deskd deskctl)' % path)
    # a stale binary runs yesterday's desk: say so (the build is yours to run)
    built = os.path.getmtime(path)
    for d in ('src/deskd', 'include/deskd'):
        for base, _, files in os.walk(os.path.join(ROOT, d)):
            for f in files:
                if os.path.getmtime(os.path.join(base, f)) > built:
                    say('note: %s is older than %s; rebuild with: cd build-deskd && nice -n 15 ninja -j 4' % (
                        name, os.path.relpath(os.path.join(base, f), ROOT)))
                    return path
    return path


# ---- setup -----------------------------------------------------------------------------------

def setup(which, key_override):
    d = DESKS[which]
    key_path = os.path.expanduser(key_override or d['key'])
    config = os.path.expanduser(d['config'])
    if os.path.exists(config):
        say('%s config %s exists: kept as it is' % (which, config))
    else:
        if not os.path.exists(key_path):
            die('no key file %s: %s' % (key_path, 'save the demo key there (Kalshi demo > API keys)' if which == 'paper'
                                       else 'save a production API key there, or pass --key FILE'))
        try:
            key = read_json(key_path)
        except (OSError, ValueError) as e:
            die('%s: %s' % (key_path, e))
        if key.get('env') != d['env']:
            die('%s says env %r; the %s desk takes an env %r key' % (key_path, key.get('env'), which, d['env']))
        pem = key.get('private_key_path') or key.get('pem_path') or ''
        if not key.get('key_id') or not pem:
            die('%s needs key_id and private_key_path (or pem_path)' % key_path)
        if not os.path.exists(os.path.expanduser(pem)):
            die('%s names the private key %s, which does not exist' % (key_path, pem))
        cfg = {'env': d['env'], 'key_id': key['key_id'], 'pem_path': pem, 'key_type': key.get('key_type', 'rsa'),
               'deskd': {'manual_orders': True, 'systematic_orders': d['systematic'], 'daily_stop_dollars': 30}}
        os.makedirs(os.path.dirname(config), exist_ok=True)
        fd = os.open(config, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as f:
            f.write('// written by tools/desk.py setup %s from %s\n' % (which, key_path))
            if which == 'live':
                f.write('// Systematic stays off on the live desk until you set "systematic_orders": true here.\n')
            json.dump(cfg, f, indent=2)
            f.write('\n')
        say('%s config written: %s (Manual on, Systematic %s)' % (which, config, 'on' if d['systematic'] else 'off'))
    if not os.path.isdir(d['data']):
        os.makedirs(d['data'], mode=0o700)
        say('%s data dir made: %s' % (which, d['data']))
    else:
        say('%s data dir: %s' % (which, d['data']))
    say('start the desk with: python3 tools/desk.py' + ('  (type LIVE when it asks)' if which == 'live' else ''))


# ---- run -------------------------------------------------------------------------------------

def up(port, timeout_s):
    end = time.time() + timeout_s
    while time.time() < end:
        try:
            with urllib.request.urlopen('http://127.0.0.1:%d/api/health' % port, timeout=1) as r:
                if r.status == 200:
                    return True
        except OSError:
            pass
        time.sleep(0.3)
    return False


def prepare(which, port=None, required=True):
    """The deskd command for one desk, or None (with required, a missing setup ends here)."""
    d = DESKS[which]
    deskd = binary('deskd')
    config = os.path.expanduser(d['config'])
    hint = 'python3 tools/desk.py setup%s' % (' live --key FILE' if which == 'live' else '')
    if not os.path.exists(config) or not os.path.isdir(d['data']):
        if required:
            die('%s is not set up (no %s or %s): run %s first' % (which, config, d['data'], hint))
        return None
    try:
        cfg = read_json(config)
    except (OSError, ValueError) as e:
        die('%s: %s' % (config, e))
    if cfg.get('env') != d['env']:
        die('%s says env %r: the %s desk needs %r' % (config, cfg.get('env'), which, d['env']))
    port = port or d['port']
    return {'which': which, 'port': port, 'config': config, 'cfg': cfg,
            'cmd': [deskd, '--config', config, '--data-dir', d['data'], '--port', str(port)] + d['flags']}


def confirm_live(spec, optional):
    """LIVE starts only from a terminal, after LIVE is typed; optional: Enter skips it."""
    d, cfg = DESKS['live'], spec['cfg'].get('deskd') or {}
    if not sys.stdin.isatty():
        if optional:
            say('LIVE not started: it starts only from a terminal, after you type LIVE')
            return False
        die('the live desk starts only from a terminal, after you type LIVE')
    print('\n  %s\n  config     %s\n  data       %s\n  Manual     %s\n  Systematic %s\n'
          % (d['what'], spec['config'], d['data'], 'on' if cfg.get('manual_orders') else 'off',
             'ON' if cfg.get('systematic_orders') else 'off'))
    ans = input('  Type LIVE to start it%s: ' % (' too (Enter: PAPER only)' if optional else '')).strip()
    if ans == 'LIVE':
        return True
    if not optional:
        die('not started')
    say('LIVE not started; the toggle says so if you flip to it')
    return False


def launch(specs, open_browser):
    """Run the desks in this terminal, each log line labelled; Ctrl-C (or SIGTERM) stops them all."""
    import threading
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    procs = []

    def relay(p, label):
        for line in p.stdout:
            sys.stdout.write(label + line)
            sys.stdout.flush()

    try:
        for spec in specs:
            p = subprocess.Popen(spec['cmd'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
            procs.append((spec, p))
            threading.Thread(target=relay, args=(p, '%-6s' % spec['which'].upper()), daemon=True).start()
        for spec, p in procs:
            if up(spec['port'], 20):
                say('%s up on http://127.0.0.1:%d/' % (DESKS[spec['which']]['what'], spec['port']))
            elif p.poll() is None:
                say('%s not answering on port %d yet; see its log' % (spec['which'], spec['port']))
        first = procs[0][0]
        if open_browser and procs[0][1].poll() is None:
            webbrowser.open('http://127.0.0.1:%d/' % first['port'])
        say('the PAPER | LIVE toggle in the header flips the tab; Ctrl-C stops %s' % ('both' if len(procs) > 1 else 'it'))
        while all(p.poll() is None for _, p in procs):
            time.sleep(0.5)
        for spec, p in procs:
            if p.poll() is not None:
                say('%s exited (%s)%s' % (spec['which'], p.returncode, '; stopping the other' if len(procs) > 1 else ''))
    except KeyboardInterrupt:
        pass
    finally:
        for _, p in procs:
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
        for _, p in procs:
            try:
                p.wait(timeout=15)
            except subprocess.TimeoutExpired:
                p.kill()
    return max([p.returncode or 0 for _, p in procs] or [0])


def deskctl(args):
    return subprocess.call([binary('deskctl')] + args)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], usage=__doc__.split('\n\n')[1])
    ap.add_argument('cmd', nargs='?', default='up', choices=['up', 'setup', 'paper', 'live', 'sim', 'status', 'kill'])
    ap.add_argument('which', nargs='?', help='setup and kill: paper or live')
    ap.add_argument('--key', help='setup: the key file (default ~/.kalshi/demo.json, ~/.kalshi/prod.json)')
    ap.add_argument('--port', type=int, help='paper and live: the web port (default 8788 paper, 8789 live)')
    ap.add_argument('--no-open', action='store_true', help='do not open the browser')
    a, rest = ap.parse_known_args()
    if a.cmd == 'setup':
        which = a.which or 'paper'
        if which not in DESKS:
            die('setup paper or setup live')
        setup(which, a.key)
    elif a.cmd == 'up':
        # PAPER always; LIVE too when it is set up and you type LIVE. One tab, the toggle flips it.
        if a.port:
            die('--port is for paper or live alone: the toggle needs the two default ports')
        specs = [prepare('paper')]
        live = prepare('live', required=False)
        if live is None:
            say('LIVE is not set up (python3 tools/desk.py setup live --key FILE): PAPER only')
        elif confirm_live(live, optional=True):
            specs.append(live)
        sys.exit(launch(specs, not a.no_open))
    elif a.cmd in ('paper', 'live'):
        spec = prepare(a.cmd, a.port)
        if a.cmd == 'live':
            confirm_live(spec, optional=False)
        sys.exit(launch([spec], not a.no_open))
    elif a.cmd == 'sim':
        if a.port:
            rest += ['--port', str(a.port)]
        os.execv(sys.executable, [sys.executable, os.path.join(ROOT, 'tools', 'deskd_sim.py')] + rest)
    elif a.cmd == 'status':
        code = 0
        for which in ('paper', 'live'):
            print('%s (port %d):' % (which, DESKS[which]['port']), flush=True)
            code = max(code, deskctl(['--port', str(DESKS[which]['port']), 'status']))
        sys.exit(code)
    elif a.cmd == 'kill':
        if a.which not in DESKS:
            die('kill paper or kill live')
        sys.exit(deskctl(['--port', str(DESKS[a.which]['port']), 'kill']))


if __name__ == '__main__':
    main()
