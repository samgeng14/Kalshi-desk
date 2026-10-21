#!/usr/bin/env python3
"""The daily scorecard per Systematic lane, and the plan's lane rules, from deskd's ops.sqlite.

Plan section 9, week 4: "daily scorecard". Per lane and ET day: fills, contracts, fees, the mean
1 s / 10 s / 60 s markout per contract (how far the mid moved our way after each fill), the 60 s
markout in dollars, and the day's LIP estimate (lip_windows: an estimate until reconciled with
the payouts). Then each lane's rule over all its fills so far:

  LIP-15 (M1) and the probe (P1), after 200 fills (plan section 5(a)): the measure is the 60 s
  markout plus LIP dollars, per contract.
      above 0, with its 80% lower bound above -0.3c      SCALE UP one step
      between -0.3c and 0                                 HALVE
      below -1.5c, or our share under 5% in over half
      the LIP windows                                     RETIRE
      otherwise                                           HOLD
  BAND (M2), after 100 fills (5(b)): PAUSE below -1c a contract.
  X1 and C1, after 100 trades (5(c), 5(e)): RETIRE unless the net 60 s markout is positive.

The verdict is advice: sizes and lanes are the operator's to change. Read-only.

Usage:
  python3 tools/scorecard.py                 the paper desk's ops.sqlite, the last 7 days
  python3 tools/scorecard.py live --days 1   the live desk's
  python3 tools/scorecard.py --db PATH --json
  python3 tools/scorecard.py --self-test
"""
import argparse
import datetime
import json
import math
import os
import sqlite3
import sys
import tempfile

try:
    from zoneinfo import ZoneInfo
    ET = ZoneInfo('America/New_York')
except Exception:   # no tz database: ET days become UTC days, said in the output
    ET = None

HOME = os.path.expanduser('~')
if sys.platform == 'darwin':
    DATA = os.path.join(HOME, 'Library', 'Application Support', 'kalshi-desk')
else:
    DATA = os.path.join(os.environ.get('XDG_DATA_HOME') or os.path.join(HOME, '.local', 'share'), 'kalshi-desk')
DESK_DB = {'paper': os.path.join(DATA, 'demo', 'ops.sqlite'), 'live': os.path.join(DATA, 'prod', 'ops.sqlite')}

Z80 = 0.8416   # one-sided 80%: mean - Z80 * sd / sqrt(n)
LIP_LANES = ('M1', 'P1')


def et_day(ms):
    t = datetime.datetime.fromtimestamp(ms / 1000, ET or datetime.timezone.utc)
    return t.strftime('%Y-%m-%d')


def load(db, since_ms):
    """Fills (with their markouts) and LIP windows from since_ms, and every lane fill for the rules."""
    con = sqlite3.connect('file:%s?mode=ro' % db, uri=True)
    con.row_factory = sqlite3.Row
    fills = [dict(r) for r in con.execute(
        "SELECT fid, lane, own, tk, qty, px, fee, ts_ms FROM fills WHERE own = 'SYS' AND lane IS NOT NULL ORDER BY ts_ms")]
    mo = {}
    for r in con.execute("SELECT fid, h, mo FROM markouts"):
        mo.setdefault(r['fid'], {})[r['h']] = r['mo']
    for f in fills:
        f['mo'] = mo.get(f['fid'], {})
    windows = [dict(r) for r in con.execute(
        "SELECT tk, start_ms, mean_share, est, scored_s, quoted_s FROM lip_windows ORDER BY start_ms")]
    con.close()
    return [f for f in fills if (f['ts_ms'] or 0) >= since_ms], fills, windows


def mean(xs):
    return sum(xs) / len(xs) if xs else None


def days(fills, windows):
    """{(day, lane): row} for the scorecard."""
    out = {}
    for f in fills:
        k = (et_day(f['ts_ms']), f['lane'])
        r = out.setdefault(k, {'fills': 0, 'ctrs': 0.0, 'fees': 0.0, 'mo': {'1s': [], '10s': [], '60s': []}, 'mo60_usd': 0.0})
        r['fills'] += 1
        r['ctrs'] += f['qty'] or 0
        r['fees'] += f['fee'] or 0
        for h in ('1s', '10s', '60s'):
            if f['mo'].get(h) is not None:
                r['mo'][h].append(f['mo'][h])
        if f['mo'].get('60s') is not None:
            r['mo60_usd'] += f['mo']['60s'] * (f['qty'] or 0)
    lip = {}
    for w in windows:
        d = et_day(w['start_ms'])
        lip[d] = lip.get(d, 0.0) + (w['est'] or 0)
    rows = []
    for (d, lane), r in sorted(out.items()):
        rows.append({'day': d, 'lane': lane, 'fills': r['fills'], 'ctrs': round(r['ctrs'], 2), 'fees': round(r['fees'], 4),
                     'mo1': mean(r['mo']['1s']), 'mo10': mean(r['mo']['10s']), 'mo60': mean(r['mo']['60s']),
                     'mo60_usd': round(r['mo60_usd'], 4), 'lip_est': round(lip.get(d, 0.0), 4) if lane in LIP_LANES else None})
    return rows


def rule(lane, fills, windows):
    """The plan's rule for one lane over all its fills: (verdict, why, numbers)."""
    mine = [f for f in fills if f['lane'] == lane and f['mo'].get('60s') is not None]
    n = len(mine)
    ctrs = sum(f['qty'] or 0 for f in mine)
    per = [f['mo']['60s'] for f in mine]           # per contract, each fill weighted alike
    m = mean(per)
    sd = math.sqrt(sum((x - m) ** 2 for x in per) / (n - 1)) if n > 1 else None
    nums = {'fills': n, 'contracts': round(ctrs, 2), 'mo60': m, 'sd': sd}
    if lane in LIP_LANES:
        lip = sum(w['est'] or 0 for w in windows)
        lip_per = lip / ctrs if ctrs else 0.0
        measure = m + lip_per if m is not None else None
        low = measure - Z80 * sd / math.sqrt(n) if measure is not None and sd is not None and n > 1 else None
        scored = [w for w in windows if (w['quoted_s'] or 0) > 0]
        thin = sum(1 for w in scored if (w['mean_share'] or 0) < 0.05)
        nums.update({'lip_per_ctr': lip_per, 'measure': measure, 'low80': low, 'windows': len(scored), 'share_under_5pct': thin})
        if n < 200:
            return 'WAIT', '%d of 200 fills' % n, nums
        if measure < -0.015:
            return 'RETIRE', 'measure %.2fc a contract, under -1.5c' % (measure * 100), nums
        if scored and thin * 2 > len(scored):
            return 'RETIRE', 'our share under 5%% in %d of %d windows' % (thin, len(scored)), nums
        if measure > 0 and low is not None and low > -0.003:
            return 'SCALE UP', 'measure %.2fc, 80%% lower bound %.2fc' % (measure * 100, low * 100), nums
        if -0.003 <= measure <= 0:
            return 'HALVE', 'measure %.2fc, between -0.3c and 0' % (measure * 100), nums
        return 'HOLD', 'measure %.2fc' % (measure * 100), nums
    if lane == 'M2':
        if n < 100:
            return 'WAIT', '%d of 100 fills' % n, nums
        return ('PAUSE', '60 s markout %.2fc a contract, under -1c' % (m * 100), nums) if m < -0.01 else \
               ('HOLD', '60 s markout %.2fc a contract' % (m * 100), nums)
    if lane in ('X1', 'C1'):
        if n < 100:
            return 'WAIT', '%d of 100 trades' % n, nums
        fees = sum(f['fee'] or 0 for f in mine) / ctrs if ctrs else 0.0
        net = m - fees
        return ('HOLD', 'net 60 s markout %.2fc' % (net * 100), nums) if net > 0 else \
               ('RETIRE', 'net 60 s markout %.2fc, not positive' % (net * 100), nums)
    return 'INFO', 'no plan rule for this lane', nums


def cents(x):
    return '-' if x is None else '%+.2fc' % (x * 100)


def report(db, days_back, as_json, now_ms=None):
    now_ms = now_ms or int(datetime.datetime.now(datetime.timezone.utc).timestamp() * 1000)
    since = now_ms - days_back * 86400000
    recent, every, windows = load(db, since)
    rows = days(recent, [w for w in windows if (w['start_ms'] or 0) >= since])
    lanes = sorted({f['lane'] for f in every})
    rules = {lane: dict(zip(('verdict', 'why', 'numbers'), rule(lane, every, windows))) for lane in lanes}
    if as_json:
        print(json.dumps({'db': db, 'days': rows, 'rules': rules}, indent=1, default=str))
        return rows, rules
    print('scorecard: %s, the last %g day(s)%s' % (db, days_back, '' if ET else ' (UTC days: no tz database)'))
    print('%-10s %-4s %6s %8s %8s %8s %8s %8s %9s %9s' % ('day', 'lane', 'fills', 'ctrs', 'fees', 'mo1', 'mo10', 'mo60', 'mo60 $', 'LIP est'))
    for r in rows:
        print('%-10s %-4s %6d %8.2f %8.4f %8s %8s %8s %9.4f %9s' % (
            r['day'], r['lane'], r['fills'], r['ctrs'], r['fees'], cents(r['mo1']), cents(r['mo10']), cents(r['mo60']),
            r['mo60_usd'], '-' if r['lip_est'] is None else '%.4f' % r['lip_est']))
    if not rows:
        print('(no Systematic fills in this period)')
    print('\nrules (all fills so far; LIP is an estimate until payouts are reconciled):')
    for lane, r in rules.items():
        print('  %-4s %-9s %s' % (lane, r['verdict'], r['why']))
    return rows, rules


def self_test():
    d = tempfile.mkdtemp()
    db = os.path.join(d, 'ops.sqlite')
    con = sqlite3.connect(db)
    con.executescript("""
CREATE TABLE fills(fid TEXT PRIMARY KEY, cid TEXT, oid TEXT, tk TEXT, side TEXT, qty REAL, px REAL, fee REAL,
  taker INTEGER, ts_ms INTEGER, recv_ms INTEGER, shard INTEGER, own TEXT, lane TEXT, json TEXT NOT NULL);
CREATE TABLE markouts(fid TEXT NOT NULL, h TEXT NOT NULL, mo REAL, mid REAL, json TEXT NOT NULL, PRIMARY KEY(fid, h));
CREATE TABLE lip_windows(tk TEXT NOT NULL, start_ms INTEGER NOT NULL, end_ms INTEGER, seconds INTEGER, scored_s INTEGER,
  quoted_s INTEGER, mean_share REAL, est REAL, reward REAL, target REAL, discount_bps INTEGER, json TEXT NOT NULL,
  PRIMARY KEY(tk, start_ms));
""")
    now = 1792600000000
    # M1: 250 fills of 10, 60 s markout alternating +0.3c / -0.1c (mean +0.1c), and $5 of LIP over
    # 2,500 contracts (+0.2c a contract): measure +0.3c, well clear of the 80% bound
    for i in range(250):
        fid = 'm1-%d' % i
        con.execute("INSERT INTO fills VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                    (fid, 'c', 'o', 'KXNATGAS15M-X', 'BUY', 10, 0.4, 0, 0, now - i * 60000, 0, 0, 'SYS', 'M1', '{}'))
        con.execute("INSERT INTO markouts VALUES(?,?,?,?,?)", (fid, '60s', 0.003 if i % 2 else -0.001, 0.4, '{}'))
        con.execute("INSERT INTO markouts VALUES(?,?,?,?,?)", (fid, '10s', 0.001, 0.4, '{}'))
    for i in range(10):
        con.execute("INSERT INTO lip_windows VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                    ('KXNATGAS15M-%d' % i, now - i * 900000, now, 900, 600, 900, 0.2, 0.5, 20, 300, 5000, '{}'))
    # M2: 120 fills, 60 s markout -1.5c: PAUSE
    for i in range(120):
        fid = 'm2-%d' % i
        con.execute("INSERT INTO fills VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                    (fid, 'c', 'o', 'KXHIGHNY-X', 'BUY', 5, 0.97, 0, 0, now - i * 60000, 0, 0, 'SYS', 'M2', '{}'))
        con.execute("INSERT INTO markouts VALUES(?,?,?,?,?)", (fid, '60s', -0.015, 0.97, '{}'))
    # D1: 3 fills, no rule
    for i in range(3):
        con.execute("INSERT INTO fills VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                    ('d1-%d' % i, 'c', 'o', 'T', 'SELL', 1, 0.5, 0.01, 1, now - i * 1000, 0, 0, 'SYS', 'D1', '{}'))
    con.commit()
    con.close()
    rows, rules = report(db, 1, False, now)
    ok = True

    def check(cond, what):
        nonlocal ok
        print(('ok   ' if cond else 'FAIL ') + what)
        ok = ok and cond
    check(rules['M1']['verdict'] == 'SCALE UP', 'M1 scales up: measure +0.3c, lower bound over -0.3c')
    check(abs(rules['M1']['numbers']['measure'] - 0.003) < 1e-9, 'M1 measure = 60 s markout + LIP per contract')
    check(rules['M2']['verdict'] == 'PAUSE', 'M2 pauses under -1c after 100 fills')
    check(rules['D1']['verdict'] == 'INFO', 'D1 has no rule')
    m1 = [r for r in rows if r['lane'] == 'M1']
    check(m1 and sum(r['fills'] for r in m1) == 250 and m1[0]['lip_est'] == 5.0, 'the last day: 250 M1 fills and $5 of LIP')
    # a share under 5% in most windows retires it whatever the markout
    con = sqlite3.connect(db)
    con.execute("UPDATE lip_windows SET mean_share = 0.01")
    con.commit()
    con.close()
    _, rules = report(db, 1, True, now)
    check(rules['M1']['verdict'] == 'RETIRE', 'M1 retires on a share under 5% in over half the windows')
    print('self-test: ' + ('OK' if ok else 'FAILED'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('desk', nargs='?', choices=('paper', 'live'), default='paper')
    ap.add_argument('--db', help='an ops.sqlite path (default: the desk\'s)')
    ap.add_argument('--days', type=float, default=7.0)
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    db = a.db or DESK_DB[a.desk]
    if not os.path.exists(db):
        print('scorecard: no %s (has this desk run with its store on?)' % db, file=sys.stderr)
        return 2
    report(db, a.days, a.json)
    return 0


if __name__ == '__main__':
    sys.exit(main())
