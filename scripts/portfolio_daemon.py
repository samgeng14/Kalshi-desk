#!/usr/bin/env python3
"""The algo. Start it, close the laptop, start it again; it picks up where it was.

DESIGNED AROUND HOW IT WILL ACTUALLY BE RUN. Not on a server. On a laptop that sleeps, gets
closed mid-rebalance, and comes back hours later. So there is no in-memory state worth keeping:
every loop re-reads positions, resting orders and the clock from the exchange and from disk, and
decides from those. Killing the process at any moment is always safe, and starting it again is
always correct.

THREE THINGS THAT MAKE THAT TRUE.

  Resting orders never outlive the process. Every exit path -- Ctrl-C, SIGTERM, an exception,
  the end of a rebalance -- cancels them. A laptop that sleeps for six hours cannot leave a
  maker order working at a price from this morning.

  Positions DO outlive the process. Stopping the algo is not closing the book. That separation
  is deliberate: stopping should never be a trading decision, and closing should never be a
  side effect of quitting. To close, run the flatten.

  A missed rebalance is run late, not skipped. If the laptop was off on Friday, the rebalance
  happens when the machine comes back. Skipping it would silently turn a weekly strategy into
  whatever-the-laptop-felt-like, and the backtest says nothing about that.

SLEEP DETECTION. macOS does not signal a process when the lid closes; it freezes and resumes. So
a loop that woke up to find hours of wall clock missing may have had orders working the whole
time at prices that are now nonsense. When the gap exceeds a threshold, everything resting is
cancelled and the cycle re-plans from the current book rather than trusting what it had.
"""
import argparse, datetime as dt, json, os, signal, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
STATE = os.path.join(ROOT, 'reports', 'daemon_state.json')
LOG = os.path.join(ROOT, 'reports', 'daemon.log')

_stop = {'now': False, 'why': ''}


def _handler(signum, frame):
    _stop['now'] = True
    _stop['why'] = {signal.SIGINT: 'Ctrl-C', signal.SIGTERM: 'SIGTERM'}.get(signum, str(signum))


def say(msg):
    line = '%s  %s' % (dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'), msg)
    print(line, flush=True)
    try:
        with open(LOG, 'a') as f:
            f.write(line + '\n')
    except OSError:
        pass


def load_state():
    if os.path.exists(STATE):
        try:
            return json.load(open(STATE))
        except ValueError:
            pass
    return {'last_rebalance_utc': None, 'last_decision_id': None, 'rebalances_done': 0}


def save_state(st):
    os.makedirs(os.path.dirname(STATE), exist_ok=True)
    tmp = STATE + '.tmp'
    with open(tmp, 'w') as f:
        json.dump(st, f, indent=1)
    os.replace(tmp, STATE)            # atomic: a torn state file is worse than none


def run(script, *a, timeout=2400):
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'scripts', script)] + list(a),
                       cwd=ROOT, capture_output=True, text=True, timeout=timeout)
    return r.returncode, (r.stdout or '') + (r.stderr or '')


def cancel_everything(why=''):
    """Nothing this process opened may outlive it. Called on every exit path."""
    try:
        from kalshi_client import Kalshi, Journal
        k, j = Kalshi(), Journal()
        orders = (k.get('/margin/orders', status='resting', limit=200).get('orders') or [])
        n = 0
        for o in orders:
            try:
                k.cancel_perp_order(o['order_id'], journal=j, dry_run=False)
                n += 1
            except RuntimeError as e:
                say('  cancel FAILED %s: %s' % (o.get('order_id'), str(e)[:90]))
        if n:
            say('cancelled %d resting order(s)%s' % (n, (' — ' + why) if why else ''))
        return n
    except (SystemExit, RuntimeError, ImportError, KeyError, TypeError, ValueError) as e:
        say('could not cancel resting orders: %s' % str(e)[:120])
        return -1


def book_now():
    try:
        from kalshi_client import Kalshi
        k = Kalshi()
        r = k.perp_positions()
        pos = {}
        for p in (r.get('positions') or r.get('market_positions') or []):
            tk = p.get('market_ticker') or p.get('ticker')
            n = float(p.get('position') or 0)
            if tk and abs(n) > 1e-12:
                pos[tk] = n
        resting = len(k.get('/margin/orders', status='resting', limit=200).get('orders') or [])
        return pos, resting
    except (SystemExit, RuntimeError, KeyError, TypeError, ValueError) as e:
        return None, str(e)[:100]


def due_at(st, cfg):
    if not st.get('last_rebalance_utc'):
        return dt.datetime.now(dt.timezone.utc)          # never run: due immediately
    last = dt.datetime.strptime(st['last_rebalance_utc'], '%Y-%m-%dT%H:%M:%SZ') \
        .replace(tzinfo=dt.timezone.utc)
    return last + dt.timedelta(hours=cfg['portfolio']['rebalance_hours'])


def rebalance(live, cfg, st):
    """Top up data, decide, execute. Any non-zero step aborts the cycle without trading."""
    say('rebalance starting')

    universe = ' '.join(cfg['signal_universe']['symbols'])
    month = dt.datetime.now(dt.timezone.utc).strftime('%Y-%m')
    first = dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-01')
    rc, out = run('binance_monthly_klines.py', '--assets', *cfg['signal_universe']['symbols'],
                  '--start', month, '--end', month, '--daily-from', first, timeout=1200)
    say('  data top-up rc=%d  %s' % (rc, out.strip().splitlines()[-1][:110] if out.strip() else ''))

    rc, out = run('kalshi_snapshot.py')
    if rc != 0:
        say('  snapshot failed; not trading this cycle\n%s' % out[-500:])
        return False
    say('  ' + (out.strip().splitlines() or [''])[1][:110])

    rc, out = run('portfolio_live.py')
    dec_path = os.path.join(ROOT, 'reports', 'decisions', 'latest.json')
    if not os.path.exists(dec_path):
        say('  no decision produced; not trading\n%s' % out[-500:])
        return False
    dec = json.load(open(dec_path))
    say('  decision %s  mode=%s  label=%s' % (dec['decided_at_utc'], dec['mode'],
                                              dec['pnl_label']))
    for k, v in dec.get('feasibility', {}).items():
        if not v['pass']:
            say('    gate FAIL %s %.4f > %.4f' % (k, v['value'], v['threshold']))

    args = ['--live'] if live else []
    rc, out = run('portfolio_execute.py', *args, timeout=2400)
    for ln in out.splitlines():
        if any(t in ln for t in ('MAKER', 'CROSS', 'SKIP', 'REJECT', 'BLOCK', 'DONE',
                                 'OVERFILLED', 'cancelled', 'final positions', 'resting orders')):
            say('    ' + ln.strip())
    if rc != 0:
        say('  executor exited %d' % rc)

    st['last_rebalance_utc'] = dec['decided_at_utc']
    st['last_decision_id'] = dec['decided_at_utc']
    st['rebalances_done'] = int(st.get('rebalances_done', 0)) + 1
    save_state(st)
    say('rebalance complete (#%d)' % st['rebalances_done'])
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--live', action='store_true', help='actually trade; needs KALSHI_ARM too')
    ap.add_argument('--once', action='store_true', help='one pass, then exit')
    ap.add_argument('--status', action='store_true', help='print state and exit')
    ap.add_argument('--force', action='store_true', help='rebalance now regardless of schedule')
    ap.add_argument('--poll-seconds', type=int, default=300)
    ap.add_argument('--sleep-gap-seconds', type=int, default=900,
                    help='a wall-clock jump larger than this is treated as a laptop sleep')
    args = ap.parse_args()

    cfg = json.load(open(os.path.join(ROOT, 'config', 'portfolio_v1.json')))
    st = load_state()

    if args.status:
        pos, resting = book_now()
        nxt = due_at(st, cfg)
        print('rebalances done   %s' % st.get('rebalances_done', 0))
        print('last rebalance    %s' % (st.get('last_rebalance_utc') or 'never'))
        print('next due          %s UTC (%s)'
              % (nxt.strftime('%Y-%m-%d %H:%M'),
                 'NOW' if dt.datetime.now(dt.timezone.utc) >= nxt
                 else '%.1f days' % ((nxt - dt.datetime.now(dt.timezone.utc)).total_seconds()
                                     / 86400)))
        if pos is None:
            print('book              unreadable (%s)' % resting)
        else:
            print('book              %s' % (', '.join('%s %+g' % kv for kv in sorted(pos.items()))
                                            or '(flat)'))
            print('resting orders    %d' % resting)
        return 0

    signal.signal(signal.SIGINT, _handler)
    signal.signal(signal.SIGTERM, _handler)

    say('daemon up  live=%s  rebalance every %dh  poll %ds'
        % (args.live, cfg['portfolio']['rebalance_hours'], args.poll_seconds))
    if args.live:
        try:
            from kalshi_client import Kalshi
            host_ok, tok_ok = Kalshi().armed()
            if not (host_ok and tok_ok):
                say('NOT ARMED (host_ok=%s token_ok=%s) — refusing to run live' % (host_ok, tok_ok))
                return 2
        except SystemExit as e:
            say('no signing key: %s' % e)
            return 2

    # Anything left resting from a previous process is from a previous world. Clear it before
    # deciding anything.
    if args.live:
        cancel_everything('startup: orders from a previous run are not this run\'s orders')

    pos, resting = book_now()
    if pos is not None:
        say('book on start: %s   resting %s'
            % (', '.join('%s %+g' % kv for kv in sorted(pos.items())) or '(flat)', resting))

    try:
        last_tick = time.time()
        while not _stop['now']:
            gap = time.time() - last_tick
            if gap > args.sleep_gap_seconds:
                say('wall clock jumped %.0f minutes — treating as a sleep; clearing and '
                    're-planning' % (gap / 60))
                if args.live:
                    cancel_everything('woke after a %.0f minute gap' % (gap / 60))
            last_tick = time.time()

            nxt = due_at(st, cfg)
            now = dt.datetime.now(dt.timezone.utc)
            if args.force or now >= nxt:
                if args.force:
                    say('forced rebalance')
                elif st.get('last_rebalance_utc'):
                    late = (now - nxt).total_seconds() / 3600
                    say('rebalance due %s UTC%s'
                        % (nxt.strftime('%Y-%m-%d %H:%M'),
                           ' (%.1fh late — running it rather than skipping)' % late
                           if late > 1 else ''))
                try:
                    rebalance(args.live, cfg, st)
                finally:
                    if args.live:
                        cancel_everything('end of rebalance')
                args.force = False
                if args.once:
                    break
            elif args.once:
                say('not due until %s UTC; nothing to do' % nxt.strftime('%Y-%m-%d %H:%M'))
                break

            slept = 0
            while slept < args.poll_seconds and not _stop['now']:
                time.sleep(min(5, args.poll_seconds - slept))
                slept += 5
    finally:
        if args.live:
            say('shutting down (%s) — cancelling resting orders, LEAVING POSITIONS OPEN'
                % (_stop['why'] or 'done'))
            cancel_everything('process exiting')
        save_state(st)
        pos, resting = book_now()
        if pos is not None:
            say('book at exit: %s   resting %s'
                % (', '.join('%s %+g' % kv for kv in sorted(pos.items())) or '(flat)', resting))
        say('to close the book: '
            'KALSHI_ARM=$KALSHI_ARM python3 scripts/portfolio_safety.py --flatten --live')
    return 0


if __name__ == '__main__':
    sys.exit(main())
