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
  X1, after 100 trades (5(e)): RETIRE unless the net 60 s markout is positive.
  C1, as gw retires it (5(d), gw/sys_book.h's MARKOUT rule): WAIT under 100 trades or while
  fewer than 50 of its fills are marked at 300 s; then HOLD if their net 300 s markout (contract-
  weighted, each fill's own fee) is positive, else RETIRE; and at any count once down $20.
  N1 (nba.json): after 100 trades RETIRE unless the net 60 s markout is positive; and at any
  count once its life is down $20.
  N2, the pregame favourites' forward test (decisions 2: 5 contracts held to settlement, 60
  trades or -$15): WAIT under 60 trades, or while a position of its is still to settle; then HOLD
  if its settlement P&L is positive, else RETIRE; and at any count once it is down $15.
  S1, the arbitrage baskets (5(c)): its baskets complete, unwound (a missed leg sold back) or
  missed (the first leg filled nothing), its legging against the $5 pause, and what its locked
  baskets settled for. PAUSE once its legging since the last reset is down $5, or s1.json could
  not be read (the engine then sends no new basket until S1 is turned off and on); and once its
  settled P&L is under what its baskets locked by over a cent (a basket paid less than its
  settlement states promised: keep S1 off until that is understood). WAIT while it has only
  scanned, for its first week of scans (the evidence before trade: the events with a lock and the
  best of them); else HOLD.

C1's, N1's and N2's dollars, and C1's and N2's trades, are gw's own count of each lane's life: the
desk's saved Systematic book, systematic.json beside ops.sqlite (--book; gw/sys_book.h: trades are
orders with a fill, P&L is realized less fees, settlements included), since ops.sqlite keeps no
settlements; C1's 300 s markouts are the ones gw counted there. gw retires each lane on its own
rule (its loss rule also counts open positions at the mark); a lane-reset clears that life, and so
this count. S1's baskets and scans are ops.sqlite's baskets and arb_scans rows; its legging since
the last reset is its engine's, s1.json beside ops.sqlite (--s1; without it, the baskets' sum);
its settled P&L is S1's life in the book, compared once S1 holds nothing with the edge_real of the
baskets that ended since that life began, each leg holding the same (a basket's lock to settlement
plus its round trips: settlement can pay more, never less).

The verdict is advice: sizes and lanes are the operator's to change. Read-only.

Usage:
  python3 tools/scorecard.py                 the paper desk's ops.sqlite, the last 7 days
  python3 tools/scorecard.py live --days 1   the live desk's
  python3 tools/scorecard.py --db PATH [--book PATH] [--s1 PATH] --json
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
N1_LOSS, N2_LOSS, N2_TRADES = 20.0, 15.0, 60   # gw's default_lanes: N1 retired once down $20, N2 after 60 trades or $15
C1_LOSS, C1_TRADES, C1_MARKED = 20.0, 100, 50   # C1 after 100 trades with 50 fills marked at 300 s, or $20 (SysBook::kRetireMarked)
S1_PAUSE, S1_SCAN_DAYS = 5.0, 7   # S1 pauses new baskets once legging is down $5 (5(c)); a week of scans first


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


def load_book(path):
    """Each lane's life in deskd's saved Systematic book (gw/sys_book.h to_json, v 1): {'lives':
    {lane: trades, pnl in dollars (realized less fees), first_ms (its first trade since it began or
    was reset), retired, why, marked (fills marked at the lane's retire horizon), mo_sum (their net
    markout, Money x Qty) and mo_ctrs (their contracts, Qty)}, 'open': {lane: contracts held}}; None
    when there is no such file or it is not one."""
    try:
        with open(path) as fh:
            j = json.load(fh)
    except (OSError, ValueError):
        return None
    if not isinstance(j, dict) or j.get('v') != 1:
        return None
    lives = {}
    for lane, x in (j.get('life') or {}).items():
        lives[lane] = {'trades': int(x.get('trades', 0)), 'pnl': (x.get('real', 0) - x.get('fees', 0)) / 1e6,
                       'first_ms': int(x.get('first_ms', 0)), 'retired': bool(x.get('retired')), 'why': x.get('retired_why', ''),
                       'marked': int(x.get('mo_n', 0)), 'mo_sum': int(x.get('mo_sum', 0)),
                       'mo_ctrs': int(x.get('mo_ctrs', 0))}
    held = {}
    for lane, d in (j.get('lanes') or {}).items():   # each open position: [contracts x 100, its cost]
        held[lane] = sum(abs(v[0]) for v in (d.get('inv') or {}).values()) / 100
    return {'lives': lives, 'open': held}


def load_arb(db):
    """S1's rows in ops.sqlite: its baskets (each one's last row; legs parsed, its why and whether it
    is frozen for good from the row's json) and its finder's scans; [] where the desk has none."""
    con = sqlite3.connect('file:%s?mode=ro' % db, uri=True)
    con.row_factory = sqlite3.Row
    out = {}
    for name, sql in (('baskets', "SELECT id, event, kind, state, legs, qty, edge_real, legging, t0_ms, t_end_ms, json FROM baskets"),
                      ('scans', "SELECT ts_ms, event, kind, qty, edge, fired FROM arb_scans")):
        try:
            out[name] = [dict(r) for r in con.execute(sql)]
        except sqlite3.OperationalError:   # an ops.sqlite from before S1's tables
            out[name] = []
    con.close()
    for b in out['baskets']:
        j = json.loads(b.pop('json') or '{}')
        b['why'], b['stuck'] = j.get('why') or '', bool(j.get('stuck'))
        b['legs'] = json.loads(b['legs']) if isinstance(b['legs'], str) else []
    return out


def load_s1(path):
    """S1's engine state (strat/arb.h, <data_dir>/s1.json, v 1): {'legging': dollars since the last
    reset, 'error': why the file could not be read ('' when it could), 'open': baskets open}; None
    when there is no such file or it is not one."""
    try:
        with open(path) as fh:
            j = json.load(fh)
    except (OSError, ValueError):
        return None
    if not isinstance(j, dict) or j.get('v') != 1:
        return None
    return {'legging': j.get('legging', 0) / 1e6, 'error': j.get('error') or '', 'open': len(j.get('baskets') or [])}


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


def rule(lane, fills, windows, book=None, arb=None):
    """The plan's rule for one lane over all its fills (C1's, N1's and N2's lives from book,
    load_book's; S1's rows and state from arb, load_arb's with 's1' load_s1's): (verdict, why,
    numbers)."""
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
    life = book['lives'].get(lane) if book else None
    if lane == 'N2':
        if book is None:
            return 'WAIT', 'no Systematic book (systematic.json, --book): its trades and settlements are there', nums
        trades, pnl = (life['trades'], life['pnl']) if life else (0, 0.0)
        held = book['open'].get(lane, 0.0)
        nums.update({'trades': trades, 'settled_pnl': pnl, 'unsettled': held})
        if pnl <= -N2_LOSS:
            return 'RETIRE', 'down $%.2f, its $%.0f stop' % (-pnl, N2_LOSS), nums
        if trades < N2_TRADES:
            return 'WAIT', '%d of %d trades' % (trades, N2_TRADES), nums
        if held:
            return 'WAIT', '%d trades, %g contracts still to settle' % (trades, held), nums
        return ('HOLD', 'settlement P&L %s over %d trades' % (usd(pnl), trades), nums) if pnl > 0 else \
               ('RETIRE', 'settlement P&L %s over %d trades, not positive' % (usd(pnl), trades), nums)
    if lane == 'C1':   # gw's MARKOUT rule on gw's own count: the 300 s markouts it was sent
        if book is None:
            return 'WAIT', 'no Systematic book (systematic.json, --book): its trades and 300 s markouts are there', nums
        life = life or {'trades': 0, 'pnl': 0.0, 'marked': 0, 'mo_sum': 0, 'mo_ctrs': 0}
        mo300 = life['mo_sum'] / life['mo_ctrs'] / 10000 if life['mo_ctrs'] else None   # Money x Qty / Qty, in dollars
        nums.update({'trades': life['trades'], 'life_pnl': life['pnl'], 'marked300': life['marked'], 'mo300_net': mo300})
        if life['pnl'] <= -C1_LOSS:
            return 'RETIRE', 'down $%.2f over its life, past $%.0f' % (-life['pnl'], C1_LOSS), nums
        if life['trades'] < C1_TRADES:
            return 'WAIT', '%d of %d trades' % (life['trades'], C1_TRADES), nums
        if life['marked'] < C1_MARKED:
            return 'WAIT', '%d trades, %d of %d fills marked at 300 s' % (life['trades'], life['marked'], C1_MARKED), nums
        how = 'net 300 s markout %s over %d marked' % (cents(mo300), life['marked'])
        return ('HOLD', how, nums) if life['mo_sum'] > 0 else ('RETIRE', how + ', not positive', nums)
    if lane == 'S1':
        return s1_rule(arb or {}, book, nums)
    if lane in ('X1', 'N1'):
        if lane == 'N1' and life:
            nums['life_pnl'] = life['pnl']
            if life['pnl'] <= -N1_LOSS:
                return 'RETIRE', 'down $%.2f over its life, past $%.0f' % (-life['pnl'], N1_LOSS), nums
        if n < 100:
            return 'WAIT', '%d of 100 trades' % n, nums
        fees = sum(f['fee'] or 0 for f in mine) / ctrs if ctrs else 0.0
        net = m - fees
        return ('HOLD', 'net 60 s markout %.2fc' % (net * 100), nums) if net > 0 else \
               ('RETIRE', 'net 60 s markout %.2fc, not positive' % (net * 100), nums)
    return 'INFO', 'no plan rule for this lane', nums


def s1_rule(arb, book, nums):
    """S1's rule (5(c)) from its baskets, scans and engine state, and its life in book."""
    baskets, scans, state = arb.get('baskets') or [], arb.get('scans') or [], arb.get('s1')
    done = [b for b in baskets if b['state'] == 'DONE']
    count = {k: sum(1 for b in done if b['why'].startswith(w)) for k, w in
             (('complete', 'complete'), ('unwound', 'unwound'), ('missed', 'the first leg'), ('closed', 'its markets have closed'))}
    stuck = sum(1 for b in baskets if b['stuck'])
    legging = state['legging'] if state else sum(b['legging'] or 0 for b in done)
    locks = [s for s in scans if s['kind']]
    scan_days = len({et_day(s['ts_ms']) for s in scans})
    best = max(locks, key=lambda s: s['edge'] or 0) if locks else None
    nums.update({'baskets': len(baskets), 'open': len(baskets) - len(done), 'stuck': stuck, 'legging': round(legging, 4),
                 'legging_from': 's1.json' if state else 'baskets', 'scan_days': scan_days,
                 'events_scanned': len({s['event'] for s in scans}), 'events_locked': len({s['event'] for s in locks}),
                 'best_edge': best['edge'] if best else None, 'best_qty': best['qty'] if best else None, **count})
    if state and state['error']:
        return 'PAUSE', 'its engine cannot read s1.json (%s): no new basket until S1 is turned off and on' % state['error'], nums
    if legging <= -S1_PAUSE:
        return 'PAUSE', 'legging %s since the last reset, past -$%.0f: no new basket until S1 is turned off and on' % (
            usd(legging), S1_PAUSE), nums
    scanned = 'locks in %d of %d events over %d day(s) of scans%s' % (
        nums['events_locked'], nums['events_scanned'], scan_days,
        ', the best %s on %g' % (usd(best['edge']), best['qty']) if best else '')
    if not baskets:
        if scan_days < S1_SCAN_DAYS:
            return 'WAIT', 'scan only: %s; a week of scans comes first' % scanned, nums
        return 'INFO', 'scan only: %s; trading them is the operator\'s call (deskd.arb_mode)' % scanned, nums
    how = '%d basket(s): %d complete, %d unwound, %d missed%s%s; legging %s of -$%.0f' % (
        len(baskets), count['complete'], count['unwound'], count['missed'],
        ', %d ended by its markets\' close' % count['closed'] if count['closed'] else '',
        ', %d frozen for good (a CRIT alert named each)' % stuck if stuck else '', usd(legging), S1_PAUSE)
    # what the locks settled for: S1's life once it holds nothing, against the baskets of that life
    life = book['lives'].get('S1') if book else None
    held = book['open'].get('S1', 0.0) if book else 0.0
    if book is None:
        return 'HOLD', how + '; no Systematic book (systematic.json, --book): its settlements are there', nums
    if life is None:
        return 'HOLD', how + '; the Systematic book has no life of S1\'s yet', nums
    mine = [b for b in done if (b['t_end_ms'] or 0) >= life['first_ms']]   # this life's: a lane-reset starts another
    locked = sum(b['edge_real'] or 0 for b in mine)
    nums.update({'life_pnl': life['pnl'], 'locked': round(locked, 4), 'unsettled': held})
    if held:
        return 'HOLD', how + '; %g contracts still to settle' % held, nums
    if len(baskets) > len(done):
        return 'HOLD', how + '; a basket still open', nums
    if any(len({leg.get('filled') for leg in b['legs']}) > 1 for b in mine):   # a leg held more than another
        return 'HOLD', how + '; settled %s, a basket held an excess to settlement' % usd(life['pnl']), nums
    if life['pnl'] < locked - 0.01:
        return 'PAUSE', how + '; settled %s, under the %s its baskets locked: a basket paid less than its states ' \
            'promised' % (usd(life['pnl']), usd(locked)), nums
    return 'HOLD', how + '; settled %s on %s locked' % (usd(life['pnl']), usd(locked)), nums


def cents(x):
    return '-' if x is None else '%+.2fc' % (x * 100)


def usd(x):
    return ('-$%.2f' if x < 0 else '$%.2f') % abs(x)


def book_path(db):
    """The desk's saved Systematic book: systematic.json in ops.sqlite's data dir."""
    return os.path.join(os.path.dirname(os.path.abspath(db)), 'systematic.json')


def s1_path(db):
    """S1's engine state: s1.json in ops.sqlite's data dir."""
    return os.path.join(os.path.dirname(os.path.abspath(db)), 's1.json')


def report(db, days_back, as_json, now_ms=None, book=None, s1=None):
    now_ms = now_ms or int(datetime.datetime.now(datetime.timezone.utc).timestamp() * 1000)
    since = now_ms - days_back * 86400000
    recent, every, windows = load(db, since)
    rows = days(recent, [w for w in windows if (w['start_ms'] or 0) >= since])
    book = book or book_path(db)
    lives = load_book(book)
    s1 = s1 or s1_path(db)
    arb = load_arb(db)
    arb['s1'] = load_s1(s1)
    lanes = sorted({f['lane'] for f in every} | set(lives['lives'] if lives else ()) |
                   ({'S1'} if arb['baskets'] or arb['scans'] or arb['s1'] else set()))
    rules = {lane: dict(zip(('verdict', 'why', 'numbers'), rule(lane, every, windows, lives, arb))) for lane in lanes}
    if as_json:
        print(json.dumps({'db': db, 'book': book if lives else None, 's1': s1 if arb['s1'] else None, 'days': rows,
                          'rules': rules}, indent=1, default=str))
        return rows, rules
    print('scorecard: %s, the last %g day(s)%s' % (db, days_back, '' if ET else ' (UTC days: no tz database)'))
    print('lanes\' lives: %s' % (book if lives else 'none (no Systematic book at %s)' % book))
    if 'S1' in rules:
        print('S1\'s legging: %s' % (s1 if arb['s1'] else 'its baskets\' sum (no s1.json at %s)' % s1))
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

    # N1 and N2, with the desk's Systematic book beside ops.sqlite (gw/sys_book.h to_json: dollars
    # in micro-dollars, positions in hundredths of a contract)
    def n1_fills(k, first):
        con = sqlite3.connect(db)
        for i in range(first, first + k):   # 2 contracts each, a 2c fee: 1c a contract
            fid = 'n1-%d' % i
            con.execute("INSERT INTO fills VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                        (fid, 'SN1-%d' % i, 'o', 'KXNBAGAME-X', 'BUY', 2, 0.5, 0.02, 1, now - i * 60000, 0, 0, 'SYS', 'N1', '{}'))
            con.execute("INSERT INTO markouts VALUES(?,?,?,?,?)", (fid, '60s', 0.005, 0.5, '{}'))
        con.commit()
        con.close()

    def book(life, inv=None, first_ms=0):
        with open(book_path(db), 'w') as fh:
            json.dump({'v': 1, 'saved_ms': now, 'day_start_ms': now, 'trades': [],
                       'lanes': {lane: {'inv': x} for lane, x in (inv or {}).items()},
                       'life': {lane: {'trades': t, 'real': int(real * 1e6), 'fees': int(fees * 1e6), 'first_ms': first_ms,
                                       'retired': False,
                                       'retired_ms': 0, 'retired_why': '',
                                       **dict(zip(('mo_n', 'mo_ctrs', 'mo_sum'), mo))}
                                for lane, (t, real, fees, *mo) in life.items()},
                       'halt': None}, fh)
    n1_fills(40, 0)
    _, rules = report(db, 1, False, now)
    check(rules['N1']['verdict'] == 'WAIT' and rules['N1']['why'] == '40 of 100 trades', 'N1 waits for 100 trades')
    check('N2' not in rules, 'no N2 without fills or a book')
    book({'N1': (40, -20.5, 0.8)})
    _, rules = report(db, 1, False, now)
    check(rules['N1']['verdict'] == 'RETIRE' and rules['N1']['why'] == 'down $21.30 over its life, past $20',
          'N1 retires down $20 at any count (the book: realized less fees)')
    book({'N1': (110, 1.0, 2.2)})
    n1_fills(70, 40)   # 110 trades: gross +0.5c, the fee 1c a contract
    _, rules = report(db, 1, False, now)
    check(rules['N1']['verdict'] == 'RETIRE' and rules['N1']['why'] == 'net 60 s markout -0.50c, not positive',
          'N1 retires after 100 trades on a net 60 s markout not positive')
    # N2: 5 contracts of a 0.92 favourite a trade, held to settlement
    tk = 'KXNBAGAME-26OCT22LALBOS-BOS'
    for life, inv, want, why, what in (
            ((40, 2.0, 0.12), None, 'WAIT', '40 of 60 trades', 'N2 waits under 60 trades'),
            ((60, 1.6, 1.5), {tk: [500, 4600000]}, 'WAIT', '60 trades, 5 contracts still to settle',
             'N2 waits while a position is still to settle'),
            ((60, 4.4, 1.6), None, 'HOLD', 'settlement P&L $2.80 over 60 trades', 'N2 holds on a positive settlement P&L'),
            ((61, -2.4, 1.6), None, 'RETIRE', 'settlement P&L -$4.00 over 61 trades, not positive',
             'N2 retires on a negative settlement P&L'),
            ((22, -14.6, 0.5), None, 'RETIRE', 'down $15.10, its $15 stop', 'N2 retires down $15 at any count')):
        book({'N2': life}, {'N2': inv} if inv else None)
        _, rules = report(db, 1, False, now)
        check(rules['N2']['verdict'] == want and rules['N2']['why'] == why, what)
    # C1: 120 fills of 3 contracts whose 60 s markout nets +2c (gross +3c, a 1c fee a contract);
    # gw's rule reads the 300 s markouts it was sent, from the book (marked fills, their contracts
    # x 100, and their net markout in Money x Qty: 60 fills of 3 at +2.5c is 250 x 18000)
    con = sqlite3.connect(db)
    for i in range(120):
        fid = 'c1-%d' % i
        con.execute("INSERT INTO fills VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                    (fid, 'SC1-%d' % i, 'o', 'KXNCAAWBGAME-X', 'BUY', 3, 0.45, 0.03, 1, now - i * 60000, 0, 0, 'SYS', 'C1', '{}'))
        con.execute("INSERT INTO markouts VALUES(?,?,?,?,?)", (fid, '60s', 0.03, 0.48, '{}'))
    con.commit()
    con.close()
    for life, want, why, what in (
            ((40, 1.0, 0.4, 20, 6000, 250 * 6000), 'WAIT', '40 of 100 trades', 'C1 waits under 100 trades'),
            ((120, 2.0, 1.2, 30, 9000, 250 * 9000), 'WAIT', '120 trades, 30 of 50 fills marked at 300 s',
             'C1 waits for 50 fills marked at 300 s'),
            ((120, 4.0, 1.2, 60, 18000, 250 * 18000), 'HOLD', 'net 300 s markout +2.50c over 60 marked',
             'C1 holds on a positive net 300 s markout'),
            ((120, 1.0, 1.2, 60, 18000, -150 * 18000), 'RETIRE', 'net 300 s markout -1.50c over 60 marked, not positive',
             'C1 retires on its net 300 s markout, whatever its 60 s one'),
            ((130, 0.0, 0.0, 60, 18000, 0), 'RETIRE', 'net 300 s markout +0.00c over 60 marked, not positive',
             'C1 retires on a net 300 s markout of 0, as gw does'),
            ((30, -19.0, 1.5, 10, 3000, 250 * 3000), 'RETIRE', 'down $20.50 over its life, past $20',
             'C1 retires down $20 at any count')):
        book({'C1': life})
        _, rules = report(db, 1, False, now)
        check(rules['C1']['verdict'] == want and rules['C1']['why'] == why, what)
    # S1 (5(c)): nothing of it in an ops.sqlite from before its tables; then the finder's scans, its
    # baskets (each one's last row, as the store keeps them), s1.json and its life in the book
    check('S1' not in rules, 'no S1 without its tables, its rows or s1.json')
    con = sqlite3.connect(db)
    con.executescript("""
CREATE TABLE arb_scans(ts_ms INTEGER NOT NULL, event TEXT NOT NULL, kind TEXT, mode TEXT, legs TEXT, qty REAL, edge REAL,
  cost REAL, payout_min REAL, fired INTEGER, why TEXT, json TEXT NOT NULL, PRIMARY KEY(event, ts_ms));
CREATE TABLE baskets(id TEXT PRIMARY KEY, event TEXT, kind TEXT, state TEXT, legs TEXT, qty REAL, edge_plan REAL,
  edge_real REAL, legging REAL, t0_ms INTEGER, t_end_ms INTEGER, json TEXT NOT NULL);
""")

    def scans(first_day, n_days):   # a scan a day of two events, NY locking 4c on 2 contracts, CHI never
        for d in range(first_day, first_day + n_days):
            for ev, kind, qty, edge in (('KXHIGHNY-26OCT20', 'NO_SET', 2, 0.04), ('KXHIGHCHI-26OCT20', None, None, None)):
                con.execute("INSERT INTO arb_scans VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                            (now - d * 86400000, ev, kind, 'scan', '[]', qty, edge, None, None, 0, 'scan mode', '{}'))
        con.commit()
    scans(0, 3)
    _, rules = report(db, 1, False, now)
    check(rules['S1']['verdict'] == 'WAIT' and rules['S1']['why'] ==
          'scan only: locks in 1 of 2 events over 3 day(s) of scans, the best $0.04 on 2; a week of scans comes first',
          'S1 waits for a week of scans, the locks they found said')
    scans(3, 4)
    _, rules = report(db, 1, False, now)
    check(rules['S1']['verdict'] == 'INFO' and 'over 7 day(s) of scans' in rules['S1']['why'] and
          rules['S1']['why'].endswith('trading them is the operator\'s call (deskd.arb_mode)'), 'S1 after a week\'s scans: the operator\'s call')

    def basket(i, why, filled, edge_real, legging, t_end, state='DONE', stuck=False):
        con.execute("INSERT OR REPLACE INTO baskets VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                    ('S1-b%d' % i, 'KXHIGHNY-26OCT20', 'NO_SET', state,
                     json.dumps([{'tk': 'KXHIGHNY-26OCT20-B%d' % k, 'filled': f} for k, f in enumerate(filled)]), 2, 0.04,
                     edge_real, legging, t_end - 2000, t_end, json.dumps({'why': why, 'stuck': stuck})))
        con.commit()
    basket(1, 'complete: every leg holds 2.00', (2, 2), 0.08, 0.0, now - 4 * 3600000)
    basket(2, 'complete: every leg holds 2.00', (2, 2), 0.06, -0.02, now - 3 * 3600000)   # a completion a tick through
    basket(3, 'unwound: every leg holds 0.00', (0, 0), -0.05, -0.05, now - 2 * 3600000)
    basket(4, 'the first leg filled nothing', (0, 0), 0.0, 0.0, now - 3600000)
    s1_file = s1_path(db)

    def s1_state(legging, error=''):
        with open(s1_file, 'w') as fh:
            json.dump({'v': 1, 'seq': 4, 'legging': int(round(legging * 1e6)), 'error': error, 'baskets': []}, fh)
    tally = '4 basket(s): 2 complete, 1 unwound, 1 missed; legging '
    for life, inv, first_ms, state, want, why, what in (
            ((3, 0.2, 0.05), {'KXHIGHNY-26OCT20-B0': [-200, 1200000]}, 0, None, 'HOLD',
             tally + '-$0.07 of -$5; 2 contracts still to settle',
             'S1 holds while its baskets settle (legging from the baskets without s1.json)'),
            ((3, 0.2, 0.08), None, 0, None, 'HOLD', tally + '-$0.07 of -$5; settled $0.12 on $0.09 locked',
             'S1 holds once its locks settled for at least what they locked'),
            ((3, 0.13, 0.08), None, 0, None, 'PAUSE',
             tally + '-$0.07 of -$5; settled $0.05, under the $0.09 its baskets locked: a basket paid less than its '
             'states promised', 'S1 pauses when its settlements fall under its locks'),
            ((2, 0.13, 0.08), None, now - 2 * 3600000 - 1000, None, 'HOLD',
             tally + '-$0.07 of -$5; settled $0.05 on -$0.05 locked', 'S1 counts the locks of its life since a lane-reset'),
            ((3, 0.2, 0.08), None, 0, (-0.02, ''), 'HOLD', tally + '-$0.02 of -$5; settled $0.12 on $0.09 locked',
             'S1\'s legging since the last reset is s1.json\'s'),
            ((3, 0.2, 0.08), None, 0, (-5.1, ''), 'PAUSE',
             'legging -$5.10 since the last reset, past -$5: no new basket until S1 is turned off and on',
             'S1 pauses once legging is down $5'),
            ((3, 0.2, 0.08), None, 0, (0.0, 'not a version 1 file'), 'PAUSE',
             'its engine cannot read s1.json (not a version 1 file): no new basket until S1 is turned off and on',
             'S1 pauses while its engine could not read s1.json')):
        book({'S1': life}, {'S1': inv} if inv else None, first_ms)
        if state:
            s1_state(*state)
        elif os.path.exists(s1_file):
            os.remove(s1_file)
        _, rules = report(db, 1, False, now)
        check(rules['S1']['verdict'] == want and rules['S1']['why'] == why, what)
    os.remove(s1_file)
    basket(5, 'its markets have closed: what it holds settles', (2, 1), 0.03, 0.0, now - 1800000)
    book({'S1': (5, 0.2, 0.08)})
    _, rules = report(db, 1, False, now)
    check(rules['S1']['verdict'] == 'HOLD' and rules['S1']['why'].endswith('settled $0.12, a basket held an excess to settlement'),
          'S1 does not judge its locks while a basket held an excess to settlement')
    basket(6, '10 unwind passes spent, 1.00 KXHIGHNY-26OCT20-B0 over the complete set held', (2, 1), 0.0, 0.0, now, 'FROZEN', True)
    _, rules = report(db, 1, False, now)
    check(rules['S1']['verdict'] == 'HOLD' and rules['S1']['why'] ==
          '6 basket(s): 2 complete, 1 unwound, 1 missed, 1 ended by its markets\' close, 1 frozen for good (a CRIT alert '
          'named each); legging -$0.07 of -$5; a basket still open', 'S1 names a basket frozen for good')
    os.remove(book_path(db))
    _, rules = report(db, 1, False, now)
    check(rules['S1']['verdict'] == 'HOLD' and rules['S1']['why'].endswith('no Systematic book (systematic.json, --book): '
                                                                           'its settlements are there'),
          'no book: S1 on its baskets alone')
    check(rules['N1']['verdict'] == 'RETIRE' and 'N2' not in rules, 'no book: N1 on its markouts alone')
    check(rules['C1']['verdict'] == 'WAIT' and rules['C1']['why'].startswith('no Systematic book'),
          'no book: C1 waits, its 300 s markouts are gw\'s')
    print('self-test: ' + ('OK' if ok else 'FAILED'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('desk', nargs='?', choices=('paper', 'live'), default='paper')
    ap.add_argument('--db', help='an ops.sqlite path (default: the desk\'s)')
    ap.add_argument('--book', help='the desk\'s systematic.json (default: beside the ops.sqlite)')
    ap.add_argument('--s1', help='the desk\'s s1.json, S1\'s engine state (default: beside the ops.sqlite)')
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
    report(db, a.days, a.json, book=a.book, s1=a.s1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
