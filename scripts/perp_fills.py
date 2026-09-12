#!/usr/bin/env python3
"""The two numbers live perps trading is being run to find out.

No directional model survived the research (docs/perps-models.md), so the rule is not live to
make money. It is live to answer the one question paper trading provably cannot:

  1. FILL RATE. Entries are post-only at the touch. Three days of paper produced 38 entries and
     zero fills, because the paper model requires the touch to trade THROUGH the posted price
     while a real maker fill happens AT it. So paper's fill rate is not an estimate of anything.
  2. POST-FILL DRIFT. Where the price goes right after you are filled. If the fills you get are
     the ones somebody wanted to give you, the drift is against you and eats the half-spread --
     which is exactly what happened on 34 real maker fills in the event markets: +0.0078 of
     half-spread captured, -0.0102 of drift at 60s, netting nothing.

Both come out of perp_algo_trades and perp_quotes. Run it after a few days live.
"""
import sqlite3, sys, os, math

DB = sys.argv[1] if len(sys.argv) > 1 else "data/prod.sqlite"
MODE = sys.argv[2] if len(sys.argv) > 2 else "live"

c = sqlite3.connect("file:%s?mode=ro" % DB, uri=True)
rows = c.execute(
    "SELECT ticker,side,state,count,entry_price,entry_basis_bps,opened_ts,filled_ts,"
    "       realized_pnl,fees,reason FROM perp_algo_trades WHERE mode=? ORDER BY id", (MODE,)).fetchall()
if not rows:
    print("no %s trades yet." % MODE); sys.exit(0)

print("=" * 74)
print("1. FILL RATE  (post-only entries at the touch, mode=%s)" % MODE)
print("=" * 74)
per = {}
for t, side, state, n, px, bas, op, fl, pnl, fee, why in rows:
    d = per.setdefault(t, {"filled": 0, "pulled": 0, "other": 0})
    if state in ("open", "closed"):   d["filled"] += 1
    elif state == "canceled":         d["pulled"] += 1
    else:                             d["other"] += 1
tf = tp = 0
print(f"{'instrument':14s} {'filled':>7s} {'pulled':>7s} {'fill rate':>10s}")
for t in sorted(per):
    d = per[t]; tot = d["filled"] + d["pulled"]
    tf += d["filled"]; tp += d["pulled"]
    print(f"{t:14s} {d['filled']:7d} {d['pulled']:7d} {(d['filled']/tot*100 if tot else 0):9.1f}%")
tot = tf + tp
print(f"{'ALL':14s} {tf:7d} {tp:7d} {(tf/tot*100 if tot else 0):9.1f}%")
if tf < 20:
    print(f"\n  {tf} fills. Under about 20 this says nothing; keep it running.")

print()
print("=" * 74)
print("2. POST-FILL DRIFT  (mid against the fill, by seconds elapsed)")
print("=" * 74)
fills = [r for r in rows if r[7] and r[7] > 0 and r[4] > 0]
if not fills:
    print("  no fills yet.")
else:
    for horizon in (30, 60, 300):
        ds = []
        for t, side, state, n, px, bas, op, fl, pnl, fee, why in fills:
            q = c.execute("SELECT bid,ask FROM perp_quotes WHERE ticker=? AND ts>=? AND bid>0 AND ask>0 "
                          "AND bid<1e9 ORDER BY ts LIMIT 1", (t, fl + horizon)).fetchone()
            if not q: continue
            mid = .5 * (q[0] + q[1])
            # bid = we bought; drift in our favour is the mid going up.
            ds.append(((mid - px) if side == "bid" else (px - mid)) / px * 1e4)
        if not ds: continue
        m = sum(ds) / len(ds)
        sd = math.sqrt(sum((x - m) ** 2 for x in ds) / (len(ds) - 1)) if len(ds) > 1 else 0
        se = sd / math.sqrt(len(ds)) if ds else 0
        print(f"  t+{horizon:>3d}s  n={len(ds):4d}  drift {m:+7.2f} bps  "
              f"95% CI [{m-1.96*se:+7.2f}, {m+1.96*se:+7.2f}]"
              f"   {'in your favour' if m - 1.96*se > 0 else 'against you' if m + 1.96*se < 0 else 'indistinguishable from zero'}")
    print("\n  A round trip costs 17 bps (maker 5 in, taker 12 out). Drift has to beat that before")
    print("  any exit rule on top of it can matter.")

print()
print("=" * 74)
print("3. WHAT IT ACTUALLY COST")
print("=" * 74)
closed = [r for r in rows if r[2] == "closed"]
if closed:
    pnl = sum(r[8] or 0 for r in closed); fee = sum(r[9] or 0 for r in closed)
    print(f"  {len(closed)} closed trades   net ${pnl:+.4f}   fees ${fee:.4f}   gross ${pnl+fee:+.4f}")
    wins = sum(1 for r in closed if (r[8] or 0) > 0)
    print(f"  {wins}/{len(closed)} profitable ({wins/len(closed)*100:.0f}%)")
    why = {}
    for r in closed: why[(r[10] or "")[:34]] = why.get((r[10] or "")[:34], 0) + 1
    for k, v in sorted(why.items(), key=lambda x: -x[1]): print(f"    {v:4d}  {k}")
else:
    print("  nothing closed yet.")
