#!/usr/bin/env python3
"""Which signal, if any, predicts a perp's next move -- per instrument. Honest version.

Two things wreck a study like this, and the first pass at it hit both:

1. OVERLAPPING WINDOWS. Quotes arrive every ~17s. Sampling every quote for a 30-minute forward
   return gives ~106 overlapping observations per independent one, which inflates every t-statistic
   by about sqrt(106) = 10x. A t of -10 is really a t of -1. Fixed by stepping the sample forward
   by the full horizon, so no two observations share a second.

2. BID-ASK BOUNCE. The signal and the target are both built from mid = (bid+ask)/2. If the book
   flickers, mid(t) is noisy, and any signal containing mid(t) predicts the reversal of its own
   noise. Every signal came back negative for every instrument on the first pass, which is that
   artefact rather than sixteen discoveries. Fixed by entering at the NEXT observation after the
   signal, so signal noise and entry noise are different draws.

Signals at time t: basis (mid-index)/index; idx_ret and perp_ret over the last `lag` seconds.
Target: return from the next observation after t, to t+h, in bps. Negative correlation = fade,
positive = follow.
"""
import sqlite3, sys, math, random

DB = sys.argv[1] if len(sys.argv) > 1 else "data/prod.sqlite"
LAG = 60
HORIZONS = [60, 300, 900, 1800]
MIN_N = 60          # independent observations, not quotes

def load(db):
    c = sqlite3.connect("file:%s?mode=ro" % db, uri=True)
    out = {}
    for ts, t, b, a, ref in c.execute(
            "SELECT ts,ticker,bid,ask,reference FROM perp_quotes "
            "WHERE bid>0 AND ask>0 AND bid<1e9 AND ask<1e9 AND reference>0 ORDER BY ts"):
        out.setdefault(t, []).append((ts, .5 * (b + a), ref, b, a))
    return out

def before(q, i, dt):
    j = i
    while j > 0 and q[j][0] > q[i][0] - dt: j -= 1
    return j if q[j][0] <= q[i][0] - dt else None

def after(q, i, dt):
    j = i
    while j < len(q) - 1 and q[j][0] < q[i][0] + dt: j += 1
    return j if q[j][0] >= q[i][0] + dt else None

def pearson(xs, ys):
    n = len(xs)
    if n < 20: return float('nan'), float('nan'), n
    mx, my = sum(xs) / n, sum(ys) / n
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs); syy = sum((y - my) ** 2 for y in ys)
    if sxx <= 0 or syy <= 0: return float('nan'), float('nan'), n
    r = max(-0.999999, min(0.999999, sxy / math.sqrt(sxx * syy)))
    return r, r * math.sqrt((n - 2) / (1 - r * r)), n

def build(q, h):
    """Non-overlapping. Signal at i, entry at i+1, exit h seconds after i."""
    rows, i = [], 0
    while i < len(q) - 1:
        lag_i, fwd_i = before(q, i, LAG), after(q, i, h)
        if lag_i is None or fwd_i is None or lag_i == i:
            i += 1; continue
        entry_i = i + 1
        if entry_i >= fwd_i:
            i += 1; continue
        ts, mid, ref, b, a = q[i]
        _, mid0, ref0, _, _ = q[lag_i]
        _, entry, _, _, _ = q[entry_i]
        _, exit_, _, _, _ = q[fwd_i]
        if min(mid, ref, mid0, ref0, entry, exit_) > 0:
            rows.append(((mid - ref) / ref * 1e4,
                         (ref - ref0) / ref0 * 1e4,
                         (mid - mid0) / mid0 * 1e4,
                         (exit_ - entry) / entry * 1e4))
        i = fwd_i          # step a whole horizon: no observation shares a second with another
    return rows

def main():
    data = load(DB)
    names = ["basis", "idx_ret", "perp_ret"]
    tested = 0
    print("non-overlapping windows; entry one observation after the signal\n")
    print(f"{'instrument':14s} {'h':>4s} {'n':>5s} " + " ".join(f"{n:>16s}" for n in names))
    survivors = {}
    for t in sorted(data):
        q = data[t]
        best = None
        for h in HORIZONS:
            rows = build(q, h)
            if len(rows) < MIN_N: continue
            cut = int(.6 * len(rows)); tr, te = rows[:cut], rows[cut:]
            cells = []
            for k, nm in enumerate(names):
                r1, t1, n1 = pearson([x[k] for x in tr], [x[3] for x in tr])
                r2, t2, n2 = pearson([x[k] for x in te], [x[3] for x in te])
                tested += 1
                ok = (r1 == r1 and r2 == r2 and r1 * r2 > 0 and abs(t1) > 2 and abs(t2) > 2)
                cells.append(f"{r1:+.2f}/{r2:+.2f} t{t2:+4.1f}{'*' if ok else ' '}")
                if ok and (best is None or abs(t2) > best[3]):
                    best = (nm, h, r2, abs(t2))
            print(f"{t:14s} {h//60:3d}m {len(rows):5d} " + " ".join(f"{c:>16s}" for c in cells))
        if best: survivors[t] = best
    print("\n" + "=" * 78)
    print(f"{tested} signal/horizon/instrument combinations tested.")
    print(f"At a 5% false-positive rate, {tested*0.05:.0f} would survive by chance alone.")
    print(f"Survived: {len(survivors)}")
    for t, b in sorted(survivors.items()):
        print(f"  {t:14s} {b[0]:9s} {b[1]//60:3d}m  r_test={b[2]:+.3f} t={b[3]:.1f} -> "
              f"{'follow' if b[2] > 0 else 'fade'}")
    if not survivors: print("  none")

main()
