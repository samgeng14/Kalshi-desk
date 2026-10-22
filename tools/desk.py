#!/usr/bin/env python3
"""Start the desk: PAPER (the Kalshi demo account, play money) or LIVE (production, real money).

    python3 tools/desk.py setup                  # once: PAPER's config and data dir
    python3 tools/desk.py setup live             # once, for LIVE (key ~/.kalshi/prod.json, or --key FILE)
    python3 tools/desk.py                        # the desk: PAPER, and LIVE too after you type LIVE
    python3 tools/desk.py paper | live           # one of them alone
    python3 tools/desk.py sim [--minutes N]      # the fake exchange, no network, no key (sim --help)
    python3 tools/desk.py status                 # both, through deskctl
    python3 tools/desk.py report [paper|live]    # what went wrong: status, KILLs, refusals, the log's tail
    python3 tools/desk.py kill paper|live        # KILL, as the screen's button
    python3 tools/desk.py lane-reset LANE [live] # a retired lane's fresh life, with its desk stopped

One desk, one browser tab, a PAPER | LIVE toggle in the header. Underneath, each account is its
own deskd (its own lock, order ledger, venue connections; PAPER on port 8788, LIVE on 8789), so a
fault in one never touches the other's orders; the toggle flips the tab between them, asking
first before LIVE. Nothing here starts LIVE by itself: only from a terminal, after you type LIVE.

Files (setup writes a config when missing; an existing one gets only its key fields from the key
file and its order switches on, the previous version kept beside it as .bak-<time>):
  paper: key ~/.kalshi/demo.json, config ~/.kalshi/deskd-demo.json, data <data>/demo
  live:  key ~/.kalshi/prod.json (or --key), config ~/.kalshi/deskd-prod.json, data <data>/prod
Each desk's log goes to the terminal and to <data>/<demo|prod>/deskd.log (20 MB, then .1).
  <data>: ~/Library/Application Support/kalshi-desk on a Mac, else $XDG_DATA_HOME/kalshi-desk
A key file is {"env": "demo"|"prod", "key_id", "private_key_path" (or "pem_path"), "key_type"}.
PAPER gets Manual and Systematic orders on; LIVE Manual only (Systematic stays off until you
set "systematic_orders": true in ~/.kalshi/deskd-prod.json yourself).
"""
import argparse
import json
import os
import shutil
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

def load_key(which, key_path):
    """The key file's fields deskd's config carries; never printed."""
    d = DESKS[which]
    if not os.path.exists(key_path):
        die('no key file %s: %s' % (key_path, 'save the demo key there (Kalshi demo > API keys)' if which == 'paper'
                                   else 'save your production API key there, or pass --key FILE'))
    try:
        key = read_json(key_path)
    except (OSError, ValueError) as e:
        die('%s: %s' % (key_path, e))
    if key.get('env') != d['env']:
        die('%s says env %r; the %s desk takes an env %r key' % (key_path, key.get('env'), which, d['env']))
    key_id = str(key.get('key_id') or '').strip()
    pem = key.get('private_key_path') or key.get('pem_path') or ''
    if not key_id or 'PUT-YOUR' in key_id.upper():
        die('%s has no real key_id yet: open it (open -e %s), put your Kalshi API key ID in place of the '
            'placeholder, save, and run this again' % (key_path, key_path))
    if not pem:
        die('%s needs private_key_path (or pem_path)' % key_path)
    if not os.path.exists(os.path.expanduser(pem)):
        die('%s names the private key %s, which does not exist' % (key_path, pem))
    return {'key_id': key_id, 'pem_path': pem, 'key_type': key.get('key_type')}


def write_config(path, cfg, header):
    tmp = path + '.tmp'
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as f:
        f.write(header)
        json.dump(cfg, f, indent=2)
        f.write('\n')
    os.replace(tmp, path)


def setup(which, key_override):
    """The account's deskd config from its key file: written when missing; when it exists, its key
    fields follow the key file and its order switches are turned on (PAPER: Manual and Systematic;
    LIVE: Manual only), the previous file kept beside it. Everything else in it is left alone."""
    d = DESKS[which]
    key_path = os.path.expanduser(key_override or d['key'])
    config = os.path.expanduser(d['config'])
    key = load_key(which, key_path)
    want_on = ['manual_orders', 'systematic_orders'] if which == 'paper' else ['manual_orders']
    header = '// tools/desk.py setup %s, from %s%s\n' % (which, key_path, '' if which == 'paper' else
        '\n// Systematic stays off on the live desk until you set "systematic_orders": true here.')
    if not os.path.exists(config):
        cfg = {'env': d['env'], 'key_id': key['key_id'], 'pem_path': key['pem_path'], 'key_type': key['key_type'] or 'rsa',
               'deskd': {'manual_orders': True, 'systematic_orders': d['systematic'], 'daily_stop_dollars': 30}}
        os.makedirs(os.path.dirname(config), exist_ok=True)
        write_config(config, cfg, header)
        say('%s config written: %s' % (which, config))
    else:
        try:
            cfg = read_json(config)
        except (OSError, ValueError) as e:
            die('%s is not readable (%s): fix it or move it aside, then run setup again' % (config, e))
        changed = []
        if cfg.get('env') != d['env']:
            cfg['env'] = d['env']; changed.append('env')
        if cfg.get('key_id') != key['key_id']:
            cfg['key_id'] = key['key_id']; changed.append('key_id')
        if os.path.expanduser(cfg.get('pem_path') or '') != os.path.expanduser(key['pem_path']):
            cfg['pem_path'] = key['pem_path']; changed.append('pem_path')
        if key['key_type'] and cfg.get('key_type', 'rsa') != key['key_type']:
            cfg['key_type'] = key['key_type']; changed.append('key_type')
        if not isinstance(cfg.get('deskd'), dict):
            cfg['deskd'] = {}
        for k in want_on:
            if cfg['deskd'].get(k) is not True:
                cfg['deskd'][k] = True; changed.append(k)
        if changed:
            bak = '%s.bak-%s' % (config, time.strftime('%Y%m%d-%H%M%S'))
            shutil.copy2(config, bak)
            os.chmod(bak, 0o600)
            write_config(config, cfg, header)
            say('%s config updated: %s (%s; the previous one is %s)' % (which, config, ', '.join(changed), bak))
        else:
            say('%s config up to date: %s' % (which, config))
    dd = cfg.get('deskd') or {}
    say('%s: Manual %s, Systematic %s' % (which.upper(), 'on' if dd.get('manual_orders') else 'off',
                                         'on' if dd.get('systematic_orders') else 'off'))
    if not os.path.isdir(d['data']):
        os.makedirs(d['data'], mode=0o700)
        say('%s data dir made: %s' % (which, d['data']))
    else:
        say('%s data dir: %s' % (which, d['data']))
    say('start the desk with: python3 tools/desk.py' + ('  (type LIVE when it asks)' if which == 'live' else ''))


# ---- run -------------------------------------------------------------------------------------

def running(port):
    """The deskd answering on this port now: its /ops/status (pid, mode, ...), or None."""
    try:
        with urllib.request.urlopen('http://127.0.0.1:%d/ops/status' % port, timeout=1) as r:
            return json.loads(r.read())
    except (OSError, ValueError):
        return None


def up(port, pid, timeout_s):
    """True once the deskd we started (this pid, not another on the port) answers."""
    end = time.time() + timeout_s
    while time.time() < end:
        st = running(port)
        if st and st.get('pid') == pid:
            return True
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
    if 'PUT-YOUR' in str(cfg.get('key_id') or '').upper():
        die('%s has the placeholder key_id: put your real key ID in %s, then python3 tools/desk.py setup%s'
            % (config, d['key'], ' live' if which == 'live' else ''))
    port = port or d['port']
    # the screen: always this checkout's, whatever the build or the config names
    ui = os.path.join(ROOT, 'ui', 'desk')
    if not os.access(os.path.join(ui, 'index.html'), os.R_OK):
        die('%s/index.html is missing or unreadable: this checkout is incomplete (git status; git checkout -- ui)' % ui)
    return {'which': which, 'port': port, 'config': config, 'cfg': cfg,
            'cmd': [deskd, '--config', config, '--data-dir', d['data'], '--ui-dir', ui, '--port', str(port)] + d['flags']}


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

    def relay(p, label, path):
        # the terminal, and the desk's log file: `desk.py report` reads it back after a problem
        log = open_log(path)
        for line in p.stdout:
            sys.stdout.write(label + line)
            sys.stdout.flush()
            if log:
                try:
                    log.write(line)
                    if log.tell() > LOG_MAX:
                        log.close()
                        os.replace(path, path + '.1')
                        log = open_log(path)
                except OSError:
                    log = None

    # a desk already on one of the ports (an older run still going): say which, start nothing
    for spec in specs:
        st = running(spec['port'])
        if st:
            since = time.strftime('%Y-%m-%d %H:%M', time.localtime(st.get('started_ms', 0) / 1000)) if st.get('started_ms') else '?'
            die('port %d already has a desk running (pid %s, %s, since %s): stop it with  kill %s  then run this again'
                % (spec['port'], st.get('pid'), st.get('mode'), since, st.get('pid')))
    try:
        for spec in specs:
            p = subprocess.Popen(spec['cmd'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
            procs.append((spec, p))
            threading.Thread(target=relay, args=(p, '%-6s' % spec['which'].upper(), log_path(spec['which'])),
                             daemon=True).start()
        for spec, p in procs:
            if up(spec['port'], p.pid, 20):
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
                say('%s stopped (exit %s)%s' % (spec['which'].upper(), p.returncode, '; stopping the other' if len(procs) > 1 else ''))
                if p.returncode == 3:
                    say('it could not take its account: if its line above names another desk\'s pid, stop that one'
                        ' (kill <pid>) and run this again')
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


def sim(args):
    """tools/deskd_sim.py with these arguments as typed: its own --port, --minutes and --keep."""
    os.execv(sys.executable, [sys.executable, os.path.join(ROOT, 'tools', 'deskd_sim.py')] + args)


def lane_reset(which, lane):
    """deskctl lane-reset on this desk's own config and data dir: the lane's life (trades, markouts,
    retirement) cleared from its saved book. Not while the desk runs (one on another port holds the
    account lock, which deskctl refuses)."""
    d = DESKS[which]
    st = running(d['port'])
    if st:
        die('the %s desk is running (pid %s): stop it (Ctrl-C where it runs, or kill %s), then run this again'
            % (which, st.get('pid'), st.get('pid')))
    return deskctl(['lane-reset', lane, '--config', os.path.expanduser(d['config']), '--data-dir', d['data']] + d['flags'])


# ---- the log and the report ------------------------------------------------------------------

LOG_MAX = 20 * 1024 * 1024


def log_path(which):
    return os.path.join(DESKS[which]['data'], 'deskd.log')


def open_log(path):
    try:
        f = open(path, 'a', encoding='utf-8', buffering=1)
        f.write('---- desk.py started this desk %s ----\n' % time.strftime('%Y-%m-%d %H:%M:%S %z'))
        return f
    except OSError as e:
        say('cannot write the log %s (%s): the terminal still has it' % (path, e))
        return None


# what to look at first in a log: a KILL and why, a refusal, a stopped sender, an error
REPORT_MARKS = ('KILL', 'refused', 'STALE', 'stopped', '[error]', '[warn]', 'not sent', 'not placed', 'outcome unknown', 'account read shows', 'never rested', 'contradicts', 'UNKNOWN')


def report(which):
    """Everything needed to tell what went wrong, to paste back: no key, key id or signature is in it."""
    d = DESKS[which]
    print('==== %s (port %d) ====' % (which, d['port']))
    st = running(d['port'])
    if st:
        run = st.get('run') or {}
        print('running: pid %s, %s; run %s%s' % (st.get('pid'), st.get('mode'), run.get('state'),
                                                 ' (%s)' % run['why'] if run.get('why') else ''))
        senders = st.get('senders') or {}
        for name, s in sorted(senders.items()):
            if isinstance(s, dict) and (s.get('why') or s.get('healthy') is False):
                print('  sender %s: %s' % (name, s.get('why') or 'not healthy'))
        rest = st.get('rest') or {}
        if rest.get('error'):
            print('  rest: %s' % rest['error'])
        feeds = st.get('feeds') or {}
        for name in ('KX', 'KXP'):
            f = feeds.get(name) or {}
            if f and (not f.get('up') or f.get('gap')):
                print('  feed %s: up %s gap %s' % (name, f.get('up'), f.get('gap')))
        gw = st.get('gw') or {}
        if gw.get('ledger_why'):
            print('  ledger: %s' % gw['ledger_why'])
    else:
        print('not running now')
    path = log_path(which)
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            lines = f.read().splitlines()
    except OSError:
        print('no log at %s (the log file starts with this version of desk.py)' % path)
        return
    start = max((i for i, l in enumerate(lines) if l.startswith('---- desk.py started')), default=0)
    lines = lines[start:]
    marked = [l for l in lines if any(m in l for m in REPORT_MARKS)]
    print('-- the last run\'s KILLs, refusals, warnings and errors (%d of %d lines) --' % (min(len(marked), 120), len(marked)))
    for l in marked[-120:]:
        print(l)
    print('-- its last 60 lines --')
    for l in lines[-60:]:
        print(l)


def main():
    # sim's arguments are deskd_sim.py's, passed on untouched: the parser below would take the 30 of
    # `sim --minutes 30` for `which` and leave deskd_sim.py a bare --minutes
    args = sys.argv[1:]
    if args[:1] == ['sim']:
        sim(args[1:])
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], usage=__doc__.split('\n\n')[1])
    ap.add_argument('cmd', nargs='?', default='up',
                    choices=['up', 'setup', 'paper', 'live', 'sim', 'status', 'kill', 'report', 'lane-reset'])
    ap.add_argument('which', nargs='?', help='setup, kill and report: paper or live; lane-reset: the lane')
    ap.add_argument('desk', nargs='?', help='lane-reset: paper (default) or live')
    ap.add_argument('--key', help='setup: the key file (default ~/.kalshi/demo.json, ~/.kalshi/prod.json)')
    ap.add_argument('--port', type=int, help='paper and live: the web port (default 8788 paper, 8789 live)')
    ap.add_argument('--no-open', action='store_true', help='do not open the browser')
    a, _ = ap.parse_known_args()
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
        # desk.py's options before the word (desk.py --port 8791 sim): all of it but the word, as typed
        args.remove('sim')
        sim(args)
    elif a.cmd == 'status':
        code = 0
        for which in ('paper', 'live'):
            print('%s (port %d):' % (which, DESKS[which]['port']), flush=True)
            code = max(code, deskctl(['--port', str(DESKS[which]['port']), 'status']))
        sys.exit(code)
    elif a.cmd == 'report':
        for which in ([a.which] if a.which else ['paper', 'live']):
            if which not in DESKS:
                die('report paper or report live')
            report(which)
    elif a.cmd == 'kill':
        if a.which not in DESKS:
            die('kill paper or kill live')
        sys.exit(deskctl(['--port', str(DESKS[a.which]['port']), 'kill']))
    elif a.cmd == 'lane-reset':
        # lane-reset LANE [paper|live]: the lane comes where the other commands take the desk
        lane, which = a.which, a.desk or 'paper'
        if not lane or which not in DESKS:
            die('lane-reset LANE, or lane-reset LANE live')
        sys.exit(lane_reset(which, lane))


if __name__ == '__main__':
    main()
