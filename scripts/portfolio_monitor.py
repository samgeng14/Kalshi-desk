#!/usr/bin/env python3
"""Between rebalances, something has to be watching. This is it.

THE GAP THIS CLOSES. The drawdown guards in config/portfolio_v1.json -- warn at 5%, halt new risk
at 10% -- only evaluate when a human runs a script. Between a Friday rebalance and the next one
that is seven days in which the book can do anything and nobody is told. A limit that is only
checked when you happen to look is not a limit.

Read-only. It never sends an order, never needs the arm token, and exits non-zero when something
wants attention so a scheduler can act on it.

WHAT IT CHECKS
  equity against the baseline captured flat before the first rebalance
  positions against the book the last decision intended
  resting orders, which should be zero between rebalances
  funding accrued on the open book
  whether the next rebalance is due
"""
import argparse, datetime as dt, json, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--quiet', action='store_true', help='print only when something is wrong')
    args = ap.parse_args()

    cfg = json.load(open(os.path.join(ROOT, 'config', 'portfolio_v1.json')))
    lim = cfg['canary_risk_limits']
    state_path = os.path.join(ROOT, 'reports', 'canary_state.json')
    dec_path = os.path.join(ROOT, 'reports', 'decisions', 'latest.json')
    baseline = json.load(open(state_path))['equity_start'] if os.path.exists(state_path) else None
    decision = json.load(open(dec_path)) if os.path.exists(dec_path) else {}

    alerts, notes = [], []
    try:
        import kalshi_snapshot as KS
        acct, positions, markets = KS.from_api()
        source = 'exchange'
    except (SystemExit, RuntimeError, ImportError, KeyError, TypeError, ValueError) as e:
        alerts.append('cannot reach the exchange: %s' % str(e)[:100])
        print('MONITOR  cannot read the account — %s' % str(e)[:120])
        return 2

    eq = float(acct['equity'])
    held = {p['ticker']: int(round(float(p['position']))) for p in positions}
    want = {r['kalshi']: int(r['contracts'])
            for r in decision.get('integer_book', []) if r['kalshi']}
    mk = {m['ticker']: m for m in markets}

    dd = ((baseline - eq) / baseline * 100.0) if baseline else None
    if dd is not None:
        if dd >= lim['drawdown_halt_new_risk_pct']:
            alerts.append('DRAWDOWN %.2f%% — at or past the halt threshold (%.0f%%). '
                          'Open no new risk; review before the next rebalance.'
                          % (dd, lim['drawdown_halt_new_risk_pct']))
        elif dd >= lim['drawdown_warning_pct']:
            alerts.append('drawdown %.2f%% — past the warning threshold (%.0f%%)'
                          % (dd, lim['drawdown_warning_pct']))

    # Position drift. Between rebalances the book should be exactly what the decision intended.
    # Anything else means a fill landed late, a liquidation happened, or something traded that
    # this system did not send.
    for tk in sorted(set(list(held) + list(want))):
        h, w = held.get(tk, 0), want.get(tk, 0)
        if h != w:
            sev = alerts if abs(h) > abs(w) else notes
            sev.append('%s holds %+d against an intended %+d' % (tk, h, w))

    try:
        from kalshi_client import Kalshi
        resting = (Kalshi().get('/margin/orders', status='resting', limit=200)
                   .get('orders') or [])
        if resting:
            alerts.append('%d order(s) resting between rebalances; there should be none'
                          % len(resting))
    except (SystemExit, RuntimeError, KeyError, TypeError, ValueError):
        notes.append('could not check resting orders')

    gross = sum(abs(n) * float(mk[t]['mark'] or 0) for t, n in held.items() if t in mk)
    net = sum(n * float(mk[t]['mark'] or 0) for t, n in held.items() if t in mk)
    fund = sum(-n * float(mk[t]['mark'] or 0) * float(mk[t].get('funding_rate') or 0)
               for t, n in held.items() if t in mk)

    nxt = None
    if decision.get('decided_at_utc'):
        nxt = (dt.datetime.strptime(decision['decided_at_utc'], '%Y-%m-%dT%H:%M:%SZ')
               .replace(tzinfo=dt.timezone.utc)
               + dt.timedelta(hours=cfg['portfolio']['rebalance_hours']))
        if dt.datetime.now(dt.timezone.utc) >= nxt:
            notes.append('the next rebalance is due (%s UTC)' % nxt.strftime('%Y-%m-%d %H:%M'))

    if args.quiet and not alerts:
        return 0

    print('MONITOR  %s   source %s'
          % (dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'), source))
    print('equity      $%.4f' % eq, end='')
    if baseline:
        print('   baseline $%.4f   drawdown %+.2f%%' % (baseline, dd))
    else:
        print('   (no baseline recorded)')
    print('gross       $%.4f (%.2fx equity)   net $%+.4f (%+.2fx)'
          % (gross, gross / eq, net, net / eq))
    print('funding     $%+.5f per period on the open book' % fund)
    if nxt:
        left = (nxt - dt.datetime.now(dt.timezone.utc)).total_seconds() / 86400.0
        print('next rebal  %s UTC (%.1f days)' % (nxt.strftime('%Y-%m-%d %H:%M'), left))
    print('positions   ' + (', '.join('%s %+d' % (t, n) for t, n in sorted(held.items()))
                            or '(flat)'))
    for n in notes:
        print('  note   %s' % n)
    for a in alerts:
        print('  ALERT  %s' % a)
    if not alerts:
        print('\nnothing wants attention.')
    return 1 if alerts else 0


if __name__ == '__main__':
    sys.exit(main())
