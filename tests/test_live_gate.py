#!/usr/bin/env python3
"""Tests for the live-trading gate and the paper trader's own gate.

These are the tests that matter most in the repository, because every other failure in this
project costs time and this one costs money. The thing being tested is not that the gate opens;
it is that the gate stays shut under every near-miss.
"""
import csv, json, os, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'scripts'))
import live_trader as LT
import paper_trader as PT

P = F = 0
def ok(c, l):
    global P, F
    if c: P += 1
    else:
        F += 1
        print(f'  FAIL {l}')


# ---- the environment flag: only the exact string ----------------------------------------------
for bad in ('', 'TRUE', 'True', '1', 'yes', 'true ', ' true', 'true\n', 'False', 'live'):
    os.environ['LIVE_TRADING'] = bad
    ok(not LT.env_flag().passed, f'env: {bad!r} does not open the gate')
os.environ['LIVE_TRADING'] = 'true'
ok(LT.env_flag().passed, 'env: only the exact string "true" opens it')
del os.environ['LIVE_TRADING']
ok(not LT.env_flag().passed, 'env: unset is closed')


# ---- the scoreboard: promising is not measured -------------------------------------------------
def scoreboard(d, rows):
    p = os.path.join(d, 'reports', 'model_scoreboard.csv')
    os.makedirs(os.path.dirname(p), exist_ok=True)
    cols = ['market', 'model', 'status', 'net_bps', 'trades']
    with open(p, 'w') as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for r in rows:
            w.writerow(r)
    return p


with tempfile.TemporaryDirectory() as d:
    old = LT.SCOREBOARD
    try:
        LT.SCOREBOARD = scoreboard(d, [])
        ok(not LT.measured_result().passed, 'scoreboard: empty is not a pass')

        LT.SCOREBOARD = scoreboard(d, [
            {'market': 'KXBTCPERP', 'model': 'ridge', 'status': 'PROMISING',
             'net_bps': '8.0', 'trades': ''}])
        c = LT.measured_result()
        ok(not c.passed, 'scoreboard: PROMISING with no trade count is not measured')
        ok('priced' in c.detail, 'scoreboard: and it says why')

        LT.SCOREBOARD = scoreboard(d, [
            {'market': 'KXBTCPERP', 'model': 'ridge', 'status': 'PROMISING',
             'net_bps': '8.0', 'trades': '0'}])
        ok(not LT.measured_result().passed, 'scoreboard: zero trades is not measured')

        LT.SCOREBOARD = scoreboard(d, [
            {'market': 'KXBTCPERP', 'model': 'ridge', 'status': 'REJECTED_COST',
             'net_bps': '8.0', 'trades': '500'}])
        ok(not LT.measured_result().passed, 'scoreboard: a rejected model never qualifies')

        LT.SCOREBOARD = scoreboard(d, [
            {'market': 'KXBTCPERP', 'model': 'ridge', 'status': 'PROMISING',
             'net_bps': '-1.0', 'trades': '500'}])
        ok(not LT.measured_result().passed, 'scoreboard: a negative net never qualifies')

        LT.SCOREBOARD = scoreboard(d, [
            {'market': 'KXBTCPERP', 'model': 'ridge', 'status': 'PROMISING',
             'net_bps': '8.0', 'trades': '500'}])
        c = LT.measured_result()
        ok(c.passed, 'scoreboard: positive net with real trades qualifies')
        ok('8.00' in c.detail and 'KXBTCPERP' in c.detail, 'scoreboard: and it names the result')
    finally:
        LT.SCOREBOARD = old


# ---- paper duration and fill count -------------------------------------------------------------
def journal(d, days, fills):
    p = os.path.join(d, 'paper_journal.jsonl')
    T0 = 1_789_000_000_000_000
    with open(p, 'w') as f:
        f.write(json.dumps({'ts_us': T0, 'event': 'place'}) + '\n')
        for i in range(fills):
            f.write(json.dumps({'ts_us': T0 + int(days * 86400e6 * i / max(fills, 1)),
                                'event': 'fill'}) + '\n')
        f.write(json.dumps({'ts_us': T0 + int(days * 86400e6), 'event': 'place'}) + '\n')
    return p


with tempfile.TemporaryDirectory() as d:
    old = LT.PAPER_JOURNAL
    try:
        LT.PAPER_JOURNAL = os.path.join(d, 'missing.jsonl')
        ok(not LT.paper_result().passed, 'paper: no journal is not a pass')
        LT.PAPER_JOURNAL = journal(d, 6.9, 500)
        ok(not LT.paper_result().passed, 'paper: 6.9 days is under the 7-day minimum')
        LT.PAPER_JOURNAL = journal(d, 30, 199)
        ok(not LT.paper_result().passed, 'paper: 199 fills is under the minimum, however long')
        LT.PAPER_JOURNAL = journal(d, 8, 250)
        ok(LT.paper_result().passed, 'paper: 8 days and 250 fills passes')
    finally:
        LT.PAPER_JOURNAL = old


# ---- backtest/paper agreement ------------------------------------------------------------------
with tempfile.TemporaryDirectory() as d:
    olds, oldp = LT.SCOREBOARD, LT.PAPER_SUMMARY
    try:
        LT.SCOREBOARD = scoreboard(d, [{'market': 'KXBTCPERP', 'model': 'ridge',
                                        'status': 'PROMISING', 'net_bps': '8.0',
                                        'trades': '500'}])
        LT.PAPER_SUMMARY = os.path.join(d, 'p.md')
        open(LT.PAPER_SUMMARY, 'w').write('nothing useful here\n')
        ok(not LT.agreement().passed, 'agreement: a summary with no net figure fails')

        open(LT.PAPER_SUMMARY, 'w').write(
            'Mean net 60s across markets: **+7.10 bps**\n')
        ok(LT.agreement().passed, 'agreement: 7.1 vs 8.0 is within 5 bps')

        open(LT.PAPER_SUMMARY, 'w').write(
            'Mean net 60s across markets: **+1.00 bps**\n')
        c = LT.agreement()
        ok(not c.passed, 'agreement: 1.0 vs 8.0 is a 7 bps gap and blocks')
        ok('gap 7.00' in c.detail, 'agreement: and the gap is stated')

        open(LT.PAPER_SUMMARY, 'w').write(
            'Mean net 60s across markets: **+20.00 bps**\n')
        ok(not LT.agreement().passed,
           'agreement: paper BETTER than backtest also blocks -- unexplained either way')
    finally:
        LT.SCOREBOARD, LT.PAPER_SUMMARY = olds, oldp


# ---- key handling -------------------------------------------------------------------------------
with tempfile.TemporaryDirectory() as d:
    oldk = LT.KEY_PATH
    try:
        LT.KEY_PATH = os.path.join(d, 'nope.pem')
        ok(not LT.key_present().passed, 'key: absent blocks')
        LT.KEY_PATH = os.path.join(d, 'prod.pem')
        open(LT.KEY_PATH, 'w').write('NOT-A-REAL-KEY\n')
        os.chmod(LT.KEY_PATH, 0o644)
        c = LT.key_present()
        ok(not c.passed, 'key: a world-readable key blocks')
        ok('0o644' in c.detail, 'key: and the mode is named')
        os.chmod(LT.KEY_PATH, 0o600)
        ok(LT.key_present().passed, 'key: present at 0600 passes')
        # The contents must never be read. If they were, this sentinel would appear somewhere.
        src = open(os.path.join(os.path.dirname(HERE), 'scripts', 'live_trader.py')).read()
        ok('open(KEY_PATH' not in src and 'read_text' not in src,
           'key: the file is never opened anywhere in live_trader.py')
        ok('password' not in src.lower().replace('the account password', ''),
           'key: the word password appears only in the promise never to touch one')
    finally:
        LT.KEY_PATH = oldk


# ---- caps ---------------------------------------------------------------------------------------
ok(not LT.caps(0, 50).passed, 'caps: a zero order size is not a valid size')
ok(not LT.caps(-1, 50).passed, 'caps: negative blocks')
ok(not LT.caps(10.01, 50).passed, 'caps: a cent over the per-order cap blocks')
ok(not LT.caps(10, 50.01).passed, 'caps: a cent over the total cap blocks')
ok(LT.caps(10, 50).passed, 'caps: exactly at the cap passes')
ok(LT.caps(1, 5).passed, 'caps: under the cap passes')


# ---- the whole gate -----------------------------------------------------------------------------
checks = LT.gate()
ok(len(checks) == 7, 'gate: all seven conditions are evaluated every time')
ok(not all(c.passed for c in checks), 'gate: closed in this environment')
ok(any(c.name == 'running on the key holder' and not c.passed for c in checks),
   'gate: a Linux container is never the key holder')


# ---- the paper trader's own gate ----------------------------------------------------------------
with tempfile.TemporaryDirectory() as d:
    old = PT.SCOREBOARD
    try:
        PT.SCOREBOARD = os.path.join(d, 'missing.csv')
        cands, why = PT.measured_candidates()
        ok(cands == [] and 'no scoreboard' in why, 'paper gate: no scoreboard, no candidates')
        PT.SCOREBOARD = scoreboard(d, [{'market': 'KXBTCPERP', 'model': 'ridge',
                                        'status': 'PROMISING', 'net_bps': '8.0', 'trades': ''}])
        cands, _ = PT.measured_candidates()
        ok(cands == [], 'paper gate: PROMISING without trades is not a candidate')
        ok(PT.Baseline.needs_gate is False, 'paper gate: the do-nothing baseline is never gated')
        ok(PT.JoinTheTouch.needs_gate is True, 'paper gate: a real strategy is gated')
    finally:
        PT.SCOREBOARD = old

# The baseline must be exactly inert.
b = PT.Baseline()
ok(b.on_book('KXBTCPERP', None, 0) == [], 'baseline: places nothing, whatever it is shown')

print(f'{P} passed, {F} failed')
sys.exit(1 if F else 0)
