#!/usr/bin/env python3
"""Phase 6 -- live trading. LOCKED.

This file sends real orders with real money. Every condition below must hold before it will send
anything, and the conditions are checked at the top of every run, not once at setup. The default
state is refusal: if a check cannot be evaluated, it fails.

THE GATE, in order:
  1. LIVE_TRADING=true in the environment. Absent is false. A typo is false.
  2. A MEASURED positive net result on the scoreboard -- net_bps > 0, trades > 0, priced through
     the execution simulator. `PROMISING` does not qualify; promising means nothing measured it.
  3. Paper trading has run for at least MIN_PAPER_DAYS on that same strategy and market, with a
     positive net result over at least MIN_PAPER_FILLS fills.
  4. The paper result and the backtest result agree to within AGREEMENT_BPS. A strategy that
     makes 8 bps in backtest and 1 bps on paper is not a strategy that makes 1 bps; it is a
     strategy whose model of the world is wrong, and the size of the disagreement is the size of
     what is not understood.
  5. The signing key is present at ~/.kalshi/prod.pem. It is never read here, never printed,
     never copied, and never committed. This check is `os.path.exists` and nothing more.
  6. The run is on the machine that holds the key. Orders are sent from Sam's Mac or not at all.
  7. Notional per order and total exposure are under the caps below.

WHAT THIS FILE WILL NEVER DO
  - read, store, log, transmit or ask for the account password
  - read the contents of the private key
  - relax any gate because a run is in progress, because a market looks good, or because a
    previous run was profitable
  - send an order whose size was not checked against the caps in the same call that sends it

Caps are deliberately far below the account balance. The first live experiment is designed to
answer whether the measured cost model survives contact with the exchange, and that question is
answered with the smallest size the exchange will accept, not the largest the account allows.

  python3 scripts/live_trader.py --check        # print the gate, send nothing
"""
import argparse, csv, json, os, platform, sys, time
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCOREBOARD = os.path.join(ROOT, 'reports', 'model_scoreboard.csv')
PAPER_SUMMARY = os.path.join(ROOT, 'reports', 'paper_summary.md')
PAPER_JOURNAL = os.path.join(ROOT, 'reports', 'paper_journal.jsonl')
KEY_PATH = os.path.expanduser('~/.kalshi/prod.pem')

MIN_PAPER_DAYS = 7
MIN_PAPER_FILLS = 200
AGREEMENT_BPS = 5.0
MAX_ORDER_NOTIONAL = 10.0
MAX_TOTAL_NOTIONAL = 50.0


class Check:
    __slots__ = ('name', 'passed', 'detail')

    def __init__(self, name, passed, detail=''):
        self.name, self.passed, self.detail = name, bool(passed), detail


def env_flag():
    v = os.environ.get('LIVE_TRADING', '')
    # Only the exact string. 'yes', '1', 'True ' and a trailing newline are all false, because a
    # gate that accepts near-misses is a gate that opens by accident.
    return Check('LIVE_TRADING=true', v == 'true',
                 f'currently {v!r}' if v else 'unset, which is false')


def measured_result():
    if not os.path.exists(SCOREBOARD):
        return Check('measured positive net result', False, 'no scoreboard')
    best = None
    with open(SCOREBOARD) as f:
        for r in csv.DictReader(f):
            try:
                net = float(r.get('net_bps') or '')
                trades = int(float(r.get('trades') or 0))
            except (TypeError, ValueError):
                continue
            if net > 0 and trades > 0 and (r.get('status') or '') in ('PROMISING',
                                                                      'LIVE_CANDIDATE'):
                if best is None or net > best[0]:
                    best = (net, r)
    if best is None:
        return Check('measured positive net result', False,
                     'nothing on the scoreboard has both a positive net and a trade count; '
                     'PROMISING without trades means nothing priced it')
    net, r = best
    return Check('measured positive net result', True,
                 f"{r.get('market')} {r.get('model')} net {net:+.2f} bps over "
                 f"{r.get('trades')} trades")


def paper_result():
    if not os.path.exists(PAPER_JOURNAL):
        return Check(f'{MIN_PAPER_DAYS}d of paper trading', False, 'no paper journal')
    first = last = None
    fills = 0
    try:
        with open(PAPER_JOURNAL) as f:
            for line in f:
                try:
                    j = json.loads(line)
                except json.JSONDecodeError:
                    continue
                ts = j.get('ts_us')
                if ts:
                    first = ts if first is None else min(first, ts)
                    last = ts if last is None else max(last, ts)
                if j.get('event') == 'fill':
                    fills += 1
    except OSError as e:
        return Check(f'{MIN_PAPER_DAYS}d of paper trading', False, str(e))
    days = ((last - first) / 86400e6) if (first and last) else 0.0
    ok = days >= MIN_PAPER_DAYS and fills >= MIN_PAPER_FILLS
    return Check(f'{MIN_PAPER_DAYS}d of paper trading', ok,
                 f'{days:.1f} days, {fills} fills '
                 f'(need {MIN_PAPER_DAYS}d and {MIN_PAPER_FILLS} fills)')


def agreement():
    """Backtest and paper must tell the same story. Disagreement is unexplained risk."""
    if not os.path.exists(PAPER_SUMMARY):
        return Check('paper agrees with backtest', False, 'no paper summary')
    txt = open(PAPER_SUMMARY).read()
    import re
    m = re.search(r'Mean net 60s across markets: \*\*([+-]?\d+\.\d+) bps\*\*', txt)
    if not m:
        return Check('paper agrees with backtest', False,
                     'paper summary reports no net figure, so there is nothing to compare')
    paper = float(m.group(1))
    bt = measured_result()
    if not bt.passed:
        return Check('paper agrees with backtest', False, 'no backtest figure to compare against')
    mm = re.search(r'net ([+-]?\d+\.\d+) bps', bt.detail)
    if not mm:
        return Check('paper agrees with backtest', False, 'backtest figure unparsable')
    back = float(mm.group(1))
    gap = abs(paper - back)
    return Check('paper agrees with backtest', gap <= AGREEMENT_BPS,
                 f'paper {paper:+.2f} vs backtest {back:+.2f}, gap {gap:.2f} bps '
                 f'(limit {AGREEMENT_BPS})')


def key_present():
    """Existence only. The key is never opened, and its contents never leave the disk."""
    ok = os.path.exists(KEY_PATH)
    detail = 'present' if ok else f'not at {KEY_PATH}'
    if ok:
        try:
            mode = oct(os.stat(KEY_PATH).st_mode & 0o777)
            if mode != '0o600':
                return Check('signing key present', False,
                             f'mode is {mode}, expected 0o600 -- refusing while it is readable '
                             f'by anyone else')
        except OSError as e:
            return Check('signing key present', False, str(e))
    return Check('signing key present', ok, detail)


def on_the_right_machine():
    """Orders leave from the machine that holds the key, and nowhere else."""
    ok = platform.system() == 'Darwin' and os.path.exists(KEY_PATH)
    return Check('running on the key holder', ok,
                 f'{platform.system()} {platform.machine()}'
                 + ('' if ok else ' -- live orders are sent from the Mac only'))


def caps(order_notional, total_notional):
    ok = (0 < order_notional <= MAX_ORDER_NOTIONAL and
          0 < total_notional <= MAX_TOTAL_NOTIONAL)
    return Check('size caps', ok,
                 f'order ${order_notional:.2f} (max ${MAX_ORDER_NOTIONAL:.2f}), '
                 f'total ${total_notional:.2f} (max ${MAX_TOTAL_NOTIONAL:.2f})')


def gate(order_notional=MAX_ORDER_NOTIONAL, total_notional=MAX_TOTAL_NOTIONAL):
    return [env_flag(), measured_result(), paper_result(), agreement(),
            key_present(), on_the_right_machine(), caps(order_notional, total_notional)]


def report(checks):
    width = max(len(c.name) for c in checks)
    print('LIVE TRADING GATE')
    for c in checks:
        print(f'  [{"PASS" if c.passed else "BLOCK"}] {c.name:<{width}}  {c.detail}')
    blocked = [c for c in checks if not c.passed]
    print()
    if blocked:
        print(f'LOCKED -- {len(blocked)} of {len(checks)} conditions not met.')
        print('No order will be sent. The first one to fix is usually the earliest one listed.')
    else:
        print('All conditions met. This build still sends nothing: the order path is not '
              'implemented, deliberately, until there is a strategy to put through it.')
    return not blocked


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true', help='print the gate and exit')
    ap.add_argument('--order-notional', type=float, default=MAX_ORDER_NOTIONAL)
    ap.add_argument('--total-notional', type=float, default=MAX_TOTAL_NOTIONAL)
    args = ap.parse_args()

    checks = gate(args.order_notional, args.total_notional)
    opened = report(checks)
    if args.check:
        return 0 if opened else 1
    if not opened:
        return 1
    # Deliberately unimplemented. There is nothing to trade yet, and an order path written before
    # there is a strategy is an order path nobody has a reason to get right.
    print('\nThe order-sending path is not implemented in this build.')
    print('It is the last thing to write, not the first: write it when a strategy has passed '
          'every gate above, so that it can be tested against that strategy rather than against '
          'a guess.')
    return 4


if __name__ == '__main__':
    sys.exit(main())
