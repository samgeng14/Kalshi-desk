#!/usr/bin/env python3
"""Data-integrity tests for scripts/perps_recorder.py. No network, no exchange.

The recorder's whole job is to not corrupt the record, and the two ways it could are the two
things tested hardest here: mixing a milliseconds feed with a seconds feed, and patching over a
sequence gap instead of flagging it.
"""
import gzip, json, os, shutil, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(ROOT, 'scripts', 'perps_recorder.py')).read().replace('\nmain()\n', '\n')
R = {'__file__': 'rec', '__name__': 'rec'}
exec(compile(src, 'rec', 'exec'), R)

checks = fails = 0
def check(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print('FAIL: ' + what)

# --- 1. units. The margin socket sends ms, the event socket sends s, and 1000x is the bug. ------
WALL = 1789107240                      # seconds
us_from_s, p1 = R['to_us'](WALL, 's')
us_from_ms, p2 = R['to_us'](WALL * 1000, 'ms')
us_from_us, p3 = R['to_us'](WALL * 1_000_000, 'us')
us_from_ns, p4 = R['to_us'](WALL * 1_000_000_000, 'ns')
us_from_iso, p5 = R['to_us']('2026-09-11T00:54:00Z', 'iso')
check(us_from_s == us_from_ms == us_from_us == us_from_ns == WALL * 1_000_000,
      f'all units converge on the same microsecond: {us_from_s} {us_from_ms} {us_from_us} {us_from_ns}')
check((p1, p2, p3, p4, p5) == ('s', 'ms', 'us', 'ns', 'iso'), 'precision tag records the wire unit')
check(us_from_iso > 0 and p5 == 'iso', 'iso8601 parses')
# the failure this is really guarding: a ms value read as if it were seconds
wrong, _ = R['to_us'](WALL * 1000, 's')
check(wrong == us_from_s * 1000, 'a ms value read as seconds is 1000x off -- which is why the '
                                 'unit is a required argument and never inferred')
none_us, none_p = R['to_us'](None, 'ms')
check(none_us is None and none_p == 'none', 'a missing timestamp is None/none, not 0')
check(R['to_us']('not-a-number', 'ms') == (None, 'none'), 'garbage does not become a timestamp')

# --- 2. stamp() carries precision and computes latency against the local receive clock ----------
rec = R['stamp']({'recv_ts_us': WALL * 1_000_000 + 5000}, WALL * 1000, 'ms')
check(rec['src_precision'] == 'ms', 'stamp records source precision')
check(rec['latency_us'] == 5000, f"latency is recv - src in us, got {rec.get('latency_us')}")
check(rec['proc_ts_us'] > 0, 'processing timestamp set')
norec = R['stamp']({'recv_ts_us': 1}, None, 'none')
check('latency_us' not in norec, 'no latency invented when the source carried no timestamp')

# --- 3. the sink writes gzipped JSONL partitioned by source and hour ---------------------------
tmp = tempfile.mkdtemp()
try:
    sink = R['Sink'](tmp)
    for i in range(5):
        sink.write('kalshi_margin', {'ev': 'orderbook_delta', 'seq': i})
    sink.write('ext_coinbase', {'ev': 'coinbase'})
    sink.close()
    found = []
    for d, _, fs in os.walk(tmp):
        for f in fs:
            if f.endswith('.jsonl.gz'): found.append(os.path.join(d, f))
    check(len(found) == 2, f'one file per source, got {len(found)}')
    check(any('source=kalshi_margin' in p for p in found), 'partitioned by source')
    check(any('hour=' in p for p in found), 'partitioned by UTC hour')
    marg = [p for p in found if 'kalshi_margin' in p][0]
    lines = gzip.open(marg, 'rt').read().strip().split('\n')
    check(len(lines) == 5, f'5 events written, got {len(lines)}')
    check(json.loads(lines[0])['seq'] == 0, 'round-trips as JSON')
    check(sink.counts['kalshi_margin'] == 5, 'counter tracks writes')
finally:
    shutil.rmtree(tmp)

# --- 4. sequence gaps are FLAGGED, never patched over ------------------------------------------
tmp = tempfile.mkdtemp()
try:
    sink = R['Sink'](tmp)
    logged = []
    # the REAL handler out of the recorder, not a copy of it
    on_msg, seq_state = R['margin_handler'](sink, logged.append)

    base = {'sid': 7, 'msg': {'market_ticker': 'KXBTCPERP', 'ts_ms': WALL * 1000}}
    on_msg({**base, 'type': 'orderbook_snapshot', 'seq': 100}, 1, 1)
    on_msg({**base, 'type': 'orderbook_delta', 'seq': 101}, 2, 2)
    on_msg({**base, 'type': 'orderbook_delta', 'seq': 102}, 3, 3)
    on_msg({**base, 'type': 'orderbook_delta', 'seq': 106}, 4, 4)   # gap: 103,104,105 missed
    on_msg({**base, 'type': 'orderbook_delta', 'seq': 107}, 5, 5)   # resumes cleanly after
    sink.close()
    path = [os.path.join(d, f) for d, _, fs in os.walk(tmp) for f in fs if f.endswith('.gz')][0]
    evs = [json.loads(l) for l in gzip.open(path, 'rt').read().strip().split('\n')]
    gaps = [e for e in evs if e['ev'] == '_gap']
    check(len(gaps) == 1, f'exactly one gap recorded, got {len(gaps)}')
    check(gaps[0]['expected'] == 103 and gaps[0]['got'] == 106,
          f"gap says what was missed: {gaps[0].get('expected')} -> {gaps[0].get('got')}")
    check(gaps[0]['ticker'] == 'KXBTCPERP', 'gap names the affected market')
    check(len([e for e in evs if e['ev'] != '_gap']) == 5, 'every frame still stored, gap or not')
    check(any('seq gap' in str(x) for x in logged), 'gap is surfaced to the operator too')
    # and no gap flagged on a clean run
    check(sum(1 for e in evs if e['ev'] == '_gap' and e['got'] == 107) == 0,
          'the frame after a gap is not itself flagged')
finally:
    shutil.rmtree(tmp)

# --- 5. the CF index handler: two millisecond fields, and Kalshi's own receipt time ------------
import gzip as _gz, json as _js, os as _os, shutil as _sh, tempfile as _tf
tmp = _tf.mkdtemp()
try:
    sink = R['Sink'](tmp)
    on_msg = R['index_handler'](sink)
    SRC_MS, RECV_MS = 1789107240323, 1789107240341     # 18ms apart, both epoch milliseconds
    on_msg({'type': 'cfbenchmarks_value_5hz', 'sid': 1, 'seq': 42,
            'msg': {'index_id': 'BRTI', 'value_usd': '68000.12000000',
                    'source_ts_ms': SRC_MS, 'received_at': RECV_MS}},
           SRC_MS * 1000 + 40_000, 1)
    sink.close()
    path = [_os.path.join(d, f) for d, _, fs in _os.walk(tmp) for f in fs if f.endswith('.gz')][0]
    e = [_js.loads(l) for l in _gz.open(path, 'rt').read().strip().split('\n')][0]
    check(e['src_ts_us'] == SRC_MS * 1000, f"source_ts_ms read as MILLISECONDS, got {e['src_ts_us']}")
    check(e['src_precision'] == 'ms', 'precision tag says ms')
    check(e['src_ts_us'] < 2_000_000_000_000_000, 'not 1000x too large (the seconds misread)')
    check(e['venue_recv_ts_us'] == RECV_MS * 1000, "Kalshi's own receipt time captured")
    check(e['venue_latency_us'] == 18_000,
          f"venue-side latency is 18ms measured entirely on their clock, got {e.get('venue_latency_us')}")
    check(e['latency_us'] == 40_000, f'our own receive latency is separate, got {e.get("latency_us")}')
    # a tick with no received_at must not invent one
    sink2 = R['Sink'](tmp + '2'); _os.makedirs(tmp + '2', exist_ok=True)
    R['index_handler'](sink2)({'type': 'x', 'msg': {'source_ts_ms': SRC_MS}}, 1, 1)
    sink2.close()
    p2 = [_os.path.join(d, f) for d, _, fs in _os.walk(tmp + '2') for f in fs if f.endswith('.gz')][0]
    e2 = [_js.loads(l) for l in _gz.open(p2, 'rt').read().strip().split('\n')][0]
    check('venue_latency_us' not in e2, 'no venue latency invented when received_at is absent')
    _sh.rmtree(tmp + '2', ignore_errors=True)
finally:
    _sh.rmtree(tmp)

# --- 6. clock skew detection --------------------------------------------------------------------
check(abs(R['clock_skew_us']()) < 1_000_000, 'wall and monotonic agree at rest')
check(R['now_us']() > 1_700_000_000_000_000, 'now_us is microseconds, not seconds or ms')
check(R['mono_us']() > 0, 'monotonic clock reads')

print(f'{checks} checks, {fails} failures')
sys.exit(1 if fails else 0)
