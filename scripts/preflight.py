#!/usr/bin/env python3
"""One command that either says READY or names exactly what is broken.

WHY THIS EXISTS. The first live rebalance took seven attempts, and every failure surfaced one
command deep into a five-command sequence: snapshot looked fine, decision looked fine, then the
executor died on a field that had been null since step one. Each round cost a human a paste and a
wait. That is a terrible way to find bugs that a single pass could have found all at once.

So this runs the entire chain in dry mode and reports every problem it finds, not the first one:

    snapshot -> field completeness -> decision -> feasibility -> plan -> pre-trade screen

Nothing is sent. Nothing is armed. If it prints READY, the live command is the only thing left.
"""
import argparse, datetime as dt, json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))

# Every field a downstream tool dereferences, and who needs it. A null in this table is what
# turned into a TypeError or, worse, a silently resized portfolio.
MARKET_FIELDS = {
    'bid': 'executor prices the maker side from it',
    'ask': 'executor prices the cross from it',
    'mark': 'pre-trade screen computes gross and margin from it',
    'tick': 'order prices must be a multiple of it',
    'leverage': 'margin cap; a null here silently shrinks the book',
    'is_open': 'pre-trade screen rejects closed markets',
}
ACCOUNT_FIELDS = {
    'equity': 'every size, cap and drawdown figure is a fraction of it',
    'available': 'reported on the dashboard',
    'initial_margin': 'margin utilisation',
}


def run(cmd, label):
    r = subprocess.run([sys.executable] + cmd, cwd=ROOT, capture_output=True, text=True)
    ok = r.returncode == 0
    return ok, (r.stdout or '') + (r.stderr or ''), label


def check_snapshot(snap, tickers_needed):
    problems = []
    acct = snap.get('account') or {}
    for f, why in ACCOUNT_FIELDS.items():
        v = acct.get(f)
        if v is None:
            problems.append('account.%s is null — %s' % (f, why))
    if snap.get('source') != 'exchange':
        problems.append('snapshot came from the recorder tape, not the exchange; prices can lag '
                        'by minutes and an order priced from them may not cross')
    mk = {m['ticker']: m for m in snap.get('markets', [])}
    for tk in tickers_needed:
        m = mk.get(tk)
        if not m:
            problems.append('%s is not in the snapshot at all' % tk)
            continue
        for f, why in MARKET_FIELDS.items():
            if m.get(f) is None:
                problems.append('%s.%s is null — %s' % (tk, f, why))
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--recorder', action='store_true',
                    help='build the snapshot from the tape instead of the exchange')
    ap.add_argument('--skip-snapshot', action='store_true')
    args = ap.parse_args()

    print('PREFLIGHT  %s' % dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'))
    print('nothing below sends an order\n')
    fatal = []

    if not args.skip_snapshot:
        cmd = ['scripts/kalshi_snapshot.py'] + (['--recorder'] if args.recorder else [])
        ok, out, _ = run(cmd, 'snapshot')
        print('1. SNAPSHOT')
        for ln in out.strip().splitlines():
            print('   ' + ln)
        if not ok:
            print('\nFAILED at the snapshot. Nothing downstream can be trusted.')
            return 2
    else:
        print('1. SNAPSHOT  (skipped)')

    snap_path = os.path.join(ROOT, 'data', 'snapshots', 'kalshi_perp_snapshot.json')
    snap = json.load(open(snap_path))

    cfg = json.load(open(os.path.join(ROOT, 'config', 'portfolio_v1.json')))
    import portfolio_live as L
    needed = [t for t in L.KALSHI_TICKER.values() if t]

    print('\n2. FIELD COMPLETENESS')
    probs = check_snapshot(snap, needed)
    if probs:
        for p in probs:
            print('   MISSING  %s' % p)
        fatal += [p for p in probs if 'recorder tape' not in p]
    else:
        print('   every field every downstream tool dereferences is present')

    print('\n3. DECISION')
    ok, out, _ = run(['scripts/portfolio_live.py', '--label', 'preflight'], 'decision')
    tail = out.strip().splitlines()
    for ln in tail:
        if any(k in ln for k in ('mode ', 'bars ', 'equity ', 'PASS', 'FAIL', 'REFUSING',
                                 'Error', 'error', 'Traceback')):
            print('   ' + ln.strip())
    if not ok and 'not feasible' not in out.lower():
        fatal.append('the decision step failed outright')
        print('   ' + out.strip()[-400:])

    dec = json.load(open(os.path.join(ROOT, 'reports', 'decisions', 'latest.json')))
    print('\n4. FEASIBILITY   mode=%s   label=%s' % (dec['mode'], dec['pnl_label']))
    for k, v in dec.get('feasibility', {}).items():
        print('   %-34s %9.4f  limit %.4f  %s'
              % (k, v['value'], v['threshold'], 'PASS' if v['pass'] else 'FAIL'))
    for u in dec.get('unlisted_legs', []):
        print('   UNREPRESENTABLE  %s — %s' % (u['symbol'], u['reason']))

    print('\n5. PLAN AND PRE-TRADE SCREEN')
    ok, out, _ = run(['scripts/portfolio_execute.py', '--quick'], 'execute')
    if 'Traceback' in out:
        fatal.append('the executor raised while planning')
        print('   ' + out.strip()[-600:])
    else:
        for ln in out.strip().splitlines():
            if any(k in ln for k in ('MAKER', 'CROSS', 'SKIP', 'REJECT', 'BLOCK', 'NOTE',
                                     'leg(s)')):
                print('   ' + ln.strip())

    print('\n' + '=' * 72)
    if fatal:
        print('NOT READY — %d problem(s):' % len(fatal))
        for f in fatal:
            print('  %s' % f)
        return 2
    if dec['pnl_label'] != 'PORTFOLIO_V1':
        print('READY, but this rebalance is labelled %s rather than PORTFOLIO_V1.'
              % dec['pnl_label'])
        print('That is the expected outcome in roughly half of weeks. Trading it is fine;')
        print('its PnL just does not count as V1 performance.')
    else:
        print('READY — mode PORTFOLIO_V1, every gate passed, nothing sent.')
    print('\nThe only command left:')
    print('  KALSHI_ARM=$KALSHI_ARM python3 scripts/portfolio_execute.py --live')
    return 0


if __name__ == '__main__':
    sys.exit(main())
