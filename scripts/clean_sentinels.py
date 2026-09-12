#!/usr/bin/env python3
"""Strip the INT64_MAX no-order sentinel out of an already-backfilled perps database.

Kalshi encodes "no order on this side" as INT64_MAX, which arrives as 922337203685477.6 after the
10^4 fixed-point scaling. `backfill_perps_history.py` now rejects it at ingest, but rows written
before that fix still carry it, and one poisoned bar produces two astronomical returns -- into it
and out of it -- which was enough to report a 300-trillion-basis-point edge.

Sets any OHLC field at or above 1e9 to 0, which is what "no quote on this side" already means
everywhere else in this codebase. Prints what it changed and verifies afterwards.

  python3 scripts/clean_sentinels.py
  python3 scripts/clean_sentinels.py --dry-run
"""
import argparse, os, sqlite3, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COLS = ['bid_open', 'bid_high', 'bid_low', 'bid_close',
        'ask_open', 'ask_high', 'ask_low', 'ask_close',
        'px_open', 'px_high', 'px_low', 'px_close', 'px_mean', 'px_prev']
SENTINEL = 1e9

ap = argparse.ArgumentParser()
ap.add_argument('--db', default=os.path.join(ROOT, 'data', 'perps_research.sqlite'))
ap.add_argument('--dry-run', action='store_true')
a = ap.parse_args()
if not os.path.exists(a.db):
    sys.exit(f"{a.db} not found")

db = sqlite3.connect(a.db)
where = " or ".join(f"{c} >= {SENTINEL}" for c in COLS)
affected = db.execute(f"select count(*) from perp_bars where {where}").fetchone()[0]
print(f"{affected:,} bars carry at least one sentinel field")
if affected:
    print("\nby ticker:")
    for t, n in db.execute(f"select ticker, count(*) from perp_bars where {where} "
                           f"group by ticker order by 2 desc"):
        print(f"  {t:14s} {n:,}")
    print("\nby column:")
    for c in COLS:
        n = db.execute(f"select count(*) from perp_bars where {c} >= {SENTINEL}").fetchone()[0]
        if n: print(f"  {c:10s} {n:,}")

if a.dry_run:
    print("\ndry run: nothing written")
    sys.exit(0)
if affected:
    sets = ", ".join(f"{c} = case when {c} >= {SENTINEL} then 0 else {c} end" for c in COLS)
    db.execute(f"update perp_bars set {sets} where {where}")
    db.commit()
    left = db.execute(f"select count(*) from perp_bars where {where}").fetchone()[0]
    print(f"\ncleaned. {left:,} bars still carry a sentinel (should be 0)")
    if left: sys.exit(1)

mx = db.execute("select max(ask_close), max(bid_close), max(px_close) from perp_bars").fetchone()
print(f"max close prices now: ask {mx[0]:.4f}  bid {mx[1]:.4f}  trade {mx[2]:.4f}")
print("\nre-run: python3 scripts/alpha_decay.py --tickers ...")
