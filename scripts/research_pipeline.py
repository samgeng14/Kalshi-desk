#!/usr/bin/env python3
"""One command that says where the research stands and what is unlocked.

    python3 scripts/research_pipeline.py

Six stages. Each declares what it needs, and stays LOCKED until the data exists. The point is that
nobody has to remember what the next step was, and no stage can be run early by accident -- which
is the failure mode that turns a pre-registration into decoration.

  1  data quality audit          >= 24h recorder      scripts/live_data_audit.py
     feed semantics audit                             scripts/feed_audit.py
  2  BTC/ETH signal screen       >= 24h + P1 pass     scripts/btc_eth_screen.py
  3  impulse model               >= 7d  + P1 pass     scripts/impulse_model.py
  4  execution / fill simulator  any L2 data          scripts/execution_sim.py
  5  live paper trader           P3 candidate         scripts/paper_trader.py
  6  $50 live validation         P5 pass + a human    scripts/live_trader.py

Stage 6 never unlocks automatically. It requires LIVE_TRADING=true set by hand, after the gates.
"""
import argparse, csv, glob, gzip, json, os, subprocess, sys, time
from collections import defaultdict
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RAW = os.path.join(ROOT, 'data', 'raw')
REPORTS = os.path.join(ROOT, 'reports')

# The hurdles every stage is measured against. Established in research/minute_bar_alpha_results.md
# and research/impulse_viability.md; they are inputs here, not things to rediscover.
COSTS = {'maker_maker': 10.0, 'maker_taker': 17.0, 'taker_taker': 24.0}
REJECTED_BASELINE_BPS = 2.8      # best robust minute-bar directional alpha
SCOREBOARD = os.path.join(REPORTS, 'model_scoreboard.csv')
SCOREBOARD_COLS = [
    'model', 'market', 'horizon', 'test_period', 'signals', 'trades', 'fills',
    'direction_accuracy', 'gross_bps', 'fees_bps', 'slippage_bps', 'adverse_selection_bps',
    'net_bps', 'gross_pnl', 'net_pnl', 'sharpe', 'profit_factor', 'max_drawdown',
    'maker_entry_pct', 'maker_exit_pct', 'bootstrap_low', 'bootstrap_high', 'status', 'notes']
VALID_STATUS = ['REJECTED_NO_SIGNAL', 'REJECTED_COST', 'REJECTED_NONROBUST',
                'REJECTED_EXECUTION', 'PROMISING', 'PAPER_TEST', 'LIVE_CANDIDATE',
                'LIVE_VALIDATION', 'ACCEPTED']


def raw_coverage():
    """Hours of recorder data, and whether it is still running.

    Measured from file mtimes rather than by decompressing 500MB of L2 to read timestamps.
    """
    files = glob.glob(os.path.join(RAW, 'source=*', 'hour=*', '*.jsonl.gz'))
    if not files:
        return {'hours': 0.0, 'files': 0, 'bytes': 0, 'sources': [], 'last_write': None,
                'running': False, 'gap_hours': []}
    hours = sorted({os.path.basename(os.path.dirname(f)).split('=', 1)[1] for f in files})
    srcs = sorted({os.path.basename(os.path.dirname(os.path.dirname(f))).split('=', 1)[1]
                   for f in files})
    last = max(os.path.getmtime(f) for f in files)
    # Hours the recorder should have covered but did not: a restart gap, not a quiet market.
    have = set(hours)
    span = []
    if len(hours) > 1:
        t0 = datetime.strptime(hours[0], '%Y-%m-%dT%H').replace(tzinfo=timezone.utc)
        t1 = datetime.strptime(hours[-1], '%Y-%m-%dT%H').replace(tzinfo=timezone.utc)
        n = int((t1 - t0).total_seconds() // 3600)
        for i in range(n + 1):
            h = (t0.timestamp() + i * 3600)
            key = datetime.fromtimestamp(h, timezone.utc).strftime('%Y-%m-%dT%H')
            span.append(key)
    missing = [h for h in span if h not in have]
    return {'hours': len(hours) - len(missing), 'files': len(files),
            'bytes': sum(os.path.getsize(f) for f in files), 'sources': srcs,
            'last_write': last, 'running': (time.time() - last) < 600,
            'gap_hours': missing, 'first': hours[0], 'last': hours[-1]}


def read_scoreboard():
    if not os.path.exists(SCOREBOARD):
        return []
    with open(SCOREBOARD) as f:
        return list(csv.DictReader(f))


def _verdict(name):
    p = os.path.join(REPORTS, name)
    if not os.path.exists(p):
        return None
    head = open(p).read(4000)
    if 'VERDICT: PASS' in head:
        return True
    if 'VERDICT: FAIL' in head:
        return False
    return None


def phase1_passed():
    """Phase 1 needs BOTH audits to pass. Absence is not a pass.

    `live_data_audit.md` says the data arrived -- coverage, gaps, staleness, price alignment.
    `feed_audit.md` says it means what the code thinks it means -- whether the trade/cancel
    field exists, which side `taker_side` names, what the sequence counter counts, whether
    snapshots carry a book, how far off the clock is.

    The second exists because the first passed while five of those were wrong. A volume audit
    cannot catch a semantics error: the rows are all there, they just say something else.
    """
    a, b = _verdict('live_data_audit.md'), _verdict('feed_audit.md')
    if a is False or b is False:
        return False
    if a is True and b is True:
        return True
    return None


def best_by_market(rows, market):
    """The best candidate for a market: highest net bps among things not rejected."""
    live = [r for r in rows if r.get('market') == market
            and not (r.get('status') or '').startswith('REJECTED')]
    if not live:
        return None
    def netof(r):
        try: return float(r.get('net_bps') or -999)
        except ValueError: return -999
    b = max(live, key=netof)
    return b if netof(b) > -999 else None


def fmt_bytes(n):
    for u in ('B', 'KB', 'MB', 'GB'):
        if n < 1024 or u == 'GB':
            return f"{n:.1f}{u}"
        n /= 1024


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--run', action='store_true',
                    help='actually run any stage whose gate is satisfied')
    ap.add_argument('--force-phase', type=int, help='run one phase regardless of its gate')
    a = ap.parse_args()

    cov = raw_coverage()
    rows = read_scoreboard()
    p1 = phase1_passed()
    hrs = cov['hours']

    print("=" * 74)
    print("KALSHI PERPS RESEARCH PIPELINE")
    print("=" * 74)

    # ---- 2. is the data healthy -------------------------------------------------------------
    print("\nDATA")
    if not cov['files']:
        print("  no recorder data at all. start: python3 scripts/perps_recorder.py")
    else:
        age_min = (time.time() - cov['last_write']) / 60
        state = "RUNNING" if cov['running'] else f"STOPPED {age_min:.0f} min ago"
        print(f"  recorder        {state}")
        print(f"  coverage        {hrs} clean hours ({cov['first']} -> {cov['last']})")
        print(f"  volume          {cov['files']} files, {fmt_bytes(cov['bytes'])}")
        print(f"  sources         {', '.join(cov['sources'])}")
        if cov['gap_hours']:
            print(f"  MISSING HOURS   {len(cov['gap_hours'])}: {', '.join(cov['gap_hours'][:6])}"
                  + ("..." if len(cov['gap_hours']) > 6 else ""))
            print(f"                  a recorder restart, not a quiet market. These hours are")
            print(f"                  excluded from the coverage figure above.")
        if not cov['running']:
            print(f"  ACTION          the recorder is not running and time cannot be backfilled:")
            print(f"                  python3 scripts/perps_recorder.py")

    # ---- 1/6. stage gates --------------------------------------------------------------------
    def gate(n, name, need, ok, script, note=''):
        if ok is True:
            s = 'READY'
        elif ok is None:
            s = 'BLOCKED'
        else:
            s = f'LOCKED'
        mark = {'READY': ' OK ', 'LOCKED': 'lock', 'BLOCKED': 'STOP'}[s]
        print(f"  [{mark}] {n}. {name:28s} {s:8s} {need}")
        if note:
            print(f"         {note}")
        return s == 'READY'

    print("\nSTAGES")
    missing = [n for n, v in (('live_data_audit', _verdict('live_data_audit.md')),
                              ('feed_audit', _verdict('feed_audit.md'))) if v is None]
    r1 = gate(1, 'data quality audit',
              f'needs 24h, have {hrs}h' + (f'; not yet run: {", ".join(missing)}' if missing else ''),
              hrs >= 24, 'live_data_audit.py + feed_audit.py')
    r2 = gate(2, 'BTC/ETH signal screen',
              'needs 24h + phase 1 PASS' + ('' if p1 is not False else ' -- PHASE 1 FAILED'),
              (hrs >= 24 and p1 is True) if p1 is not False else None, 'btc_eth_screen.py')
    r3 = gate(3, 'impulse model', f'needs 7d, have {hrs/24:.1f}d',
              hrs >= 168 and p1 is True, 'impulse_model.py')
    r4 = gate(4, 'execution simulator', f'needs any L2, have {hrs}h',
              cov['files'] > 0, 'execution_sim.py')
    # A candidate is a MEASURED positive net result, not a hopeful status. Letting a placeholder
    # row unlock paper trading is exactly the kind of soft gate this pipeline exists to prevent.
    def netbps(r):
        try: return float(r.get('net_bps') or '')
        except (TypeError, ValueError): return None
    cand = [r for r in rows
            if (r.get('status') or '') in ('PROMISING', 'LIVE_CANDIDATE')
            and netbps(r) is not None and netbps(r) > 0
            and (r.get('trades') or '').strip() not in ('', '0')]
    promising = sum(1 for r in rows if (r.get('status') or '') == 'PROMISING')
    r5 = gate(5, 'live paper trader',
              f'needs a MEASURED positive net result, have {len(cand)}'
              + (f' ({promising} marked promising but unmeasured)' if promising and not cand else ''),
              bool(cand), 'paper_trader.py')
    passed_paper = [r for r in rows if (r.get('status') or '') == 'PAPER_TEST']
    r6 = gate(6, '$50 live validation',
              'needs paper PASS and LIVE_TRADING=true by hand',
              False, 'live_trader.py',
              note='never unlocks automatically, by design')

    # ---- 3/4/5. what has been tested ---------------------------------------------------------
    print("\nMODELS TESTED")
    if not rows:
        print("  scoreboard is empty")
    else:
        by = defaultdict(int)
        for r in rows:
            by[r.get('status', '?')] += 1
        for s in VALID_STATUS:
            if by.get(s):
                print(f"  {s:22s} {by[s]:4d}")
        unknown = {k: v for k, v in by.items() if k not in VALID_STATUS}
        if unknown:
            print(f"  UNRECOGNISED STATUS    {unknown}")

    positive = []
    for r in rows:
        try:
            if float(r.get('net_bps') or -999) > 0 and not (r.get('status') or '').startswith('REJ'):
                positive.append(r)
        except ValueError:
            pass
    print(f"\n  positive after costs: {len(positive)}")
    if not positive:
        print(f"  Nothing has beaten the {REJECTED_BASELINE_BPS} bps baseline net of the "
              f"{COSTS['maker_taker']:.0f} bps maker/taker hurdle.")

    # ---- 8/9/10. the best current strategy per market ----------------------------------------
    print("\nBEST CURRENT STRATEGY")
    for mkt in ('KXBTCPERP', 'KXETHPERP'):
        b = best_by_market(rows, mkt)
        label = mkt.replace('KX', '').replace('PERP', '')
        if not b:
            print(f"  {label:5s} none. every tested model is rejected or untested.")
            continue
        print(f"  {label:5s} {b.get('model','?')} @ {b.get('horizon','?')}")
        print(f"        net {b.get('net_bps','?')} bps/trade | "
              f"acc {b.get('direction_accuracy','?')} | "
              f"trades {b.get('trades','?')} | status {b.get('status','?')}")

    # ---- 6/7. unlocks ------------------------------------------------------------------------
    print("\nUNLOCKS")
    print(f"  paper trading   {'YES' if r5 else 'NO'}")
    print(f"  $50 live        {'YES' if (passed_paper and os.environ.get('LIVE_TRADING')=='true') else 'NO'}"
          + ("" if passed_paper else "  (no strategy has passed paper)"))

    # ---- act ---------------------------------------------------------------------------------
    plan = []
    if a.force_phase:
        plan = [a.force_phase]
    elif a.run:
        if r1: plan.append(1)
        if r2: plan.append(2)
        if r3: plan.append(3)
        if r4: plan.append(4)
    scripts = {1: 'live_data_audit.py && python3 scripts/feed_audit.py',
               2: 'btc_eth_screen.py', 3: 'impulse_model.py',
               4: 'execution_sim.py', 5: 'paper_trader.py', 6: 'live_trader.py'}
    if plan:
        print("\nRUNNING")
        for n in plan:
            path = os.path.join(ROOT, 'scripts', scripts[n])
            if not os.path.exists(path):
                print(f"  phase {n}: {scripts[n]} not implemented yet, skipping")
                continue
            print(f"  phase {n}: {scripts[n]}")
            rc = subprocess.call([sys.executable, path])
            if rc != 0:
                print(f"  phase {n} exited {rc}; stopping rather than running later stages "
                      f"on a failed one")
                break
            if n == 1 and phase1_passed() is not True:
                print("  phase 1 did not PASS; later stages stay locked")
                break
    else:
        print("\n  --run executes every stage whose gate is satisfied.")
        nxt = ("wait for 24h of recorder data" if hrs < 24 else
               "run phase 1" if not os.path.exists(os.path.join(REPORTS, 'feed_audit.md'))
               else "run phase 2")
        print(f"  next: {nxt}")
    print()


if __name__ == '__main__':
    main()
