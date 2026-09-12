#!/usr/bin/env python3
"""Do sibling books CONSTRAIN a withheld contract, or merely surround it?

The final gate on the sibling-surface lane. No entropy objective, no Normal assumption, no
midpoint. Treat every sibling's book as an interval constraint and ask for the tightest
mathematically defensible range for the withheld contract:

    x_j >= 0,  sum_j x_j = 1        (normalization only where exhaustiveness is VERIFIED)
    bid_i <= A_i x <= ask_i         for every sibling i
    p_min = min A_t x,   p_max = max A_t x

A signal exists only when the target's executable price lies OUTSIDE that interval after fees. If
it sits inside, the siblings have told us nothing about it, however confident a point estimate
would have sounded.

THE LP IS CLOSED FORM HERE, AND THAT IS THE WHOLE RESULT
On a verified exhaustive partition each contract IS one terminal state, so A_i picks out x_i and
the program collapses:

    x_t = 1 - sum_{j != t} x_j
    p_min = 1 - sum_{j != t} ask_j      (push every sibling to its ceiling)
    p_max = 1 - sum_{j != t} bid_j      (push every sibling to its floor)

so the feasible width is

    p_max - p_min = sum_{j != t} (ask_j - bid_j)

**the sum of the siblings' spreads.** It grows linearly in the number of brackets, and it does so
no matter how clever the estimator is, because this is a bound on what ANY coherent method could
know - max entropy included. That is the same structural defect the midpoint version had, promoted
from an empirical artifact to a proof.

The closed form is checked against brute-force enumeration in the self-test rather than asserted.

  python3 research/loo_feasible.py --self-test
  python3 research/loo_feasible.py --category Crypto
"""
import argparse, math, os, random, sqlite3, statistics, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "research"))
from loo_surface import load, snapshots, fee_per_contract, boot  # noqa: E402


def feasible_interval(sib_bids, sib_asks):
    """Tightest coherent range for the withheld bracket. Returns (p_min, p_max) or None.

    A recorded ask of 0 (or 1) means there is NO OFFER, not that the contract's probability is at
    most zero. Reading it as a ceiling is how a missing quote turns into a hard constraint and
    manufactures a tight interval out of an empty book: before this guard, crypto produced 163
    signals all claiming a guaranteed edge over five cents, every one of them from an absent offer.
    An absent side simply imposes nothing, so its bound goes to the vacuous end.
    """
    sib_asks = [a if 0 < a < 1 else 1.0 for a in sib_asks]
    sib_bids = [b if 0 < b < 1 else 0.0 for b in sib_bids]
    sa, sb = sum(sib_asks), sum(sib_bids)
    if sb > 1.0 + 1e-9:
        return None                      # siblings alone already exceed certainty: incoherent book
    return (max(0.0, 1.0 - sa), min(1.0, 1.0 - sb))


def brute_force_interval(sib_bids, sib_asks, steps=40):
    """Enumerate the sibling box on a grid and read off the target's range. Self-test only."""
    n = len(sib_bids)
    lo, hi = 1.0, 0.0
    def rec(i, acc):
        nonlocal lo, hi
        if acc > 1.0 + 1e-12:
            return
        if i == n:
            t = 1.0 - acc
            if t >= -1e-12:
                lo = min(lo, max(0.0, t)); hi = max(hi, max(0.0, t))
            return
        b, a = sib_bids[i], sib_asks[i]
        for s in range(steps + 1):
            rec(i + 1, acc + b + (a - b) * s / steps)
    rec(0, 0.0)
    return (lo, hi)


def gate(conn, mk, parts, skew, buf, thresh):
    widths, inside, out_edges, trades = [], 0, defaultdict(int), []
    n_targets = 0
    for ev, order in sorted(parts.items()):
        traded = set()
        for ts, fresh in snapshots(conn, order, skew):
            bids = {t: fresh[t][1] for t in order}
            asks = {t: fresh[t][2] for t in order}
            for t in order:
                sb = [bids[j] for j in order if j != t]
                sa = [asks[j] for j in order if j != t]
                iv = feasible_interval(sb, sa)
                if iv is None:
                    continue
                p_min, p_max = iv
                n_targets += 1
                widths.append(p_max - p_min)
                ya, yb = asks[t], bids[t]
                f = mk[t]["fee"]
                res = 1.0 if mk[t]["res"] == "yes" else 0.0
                # the target's own executable prices, never used to build the interval
                yes_px, no_px = ya, 1.0 - yb
                cands = []
                if 0 < yes_px < 1:
                    e = p_min - yes_px - fee_per_contract(yes_px, f) - buf
                    cands.append((e, "yes", yes_px, fee_per_contract(yes_px, f)))
                if 0 < no_px < 1:
                    e = (1.0 - p_max) - no_px - fee_per_contract(no_px, f) - buf
                    cands.append((e, "no", no_px, fee_per_contract(no_px, f)))
                best = max(cands, default=None, key=lambda c: c[0])
                if best is None or best[0] <= 0:
                    inside += 1
                    continue
                for c in (0.01, 0.02, 0.03, 0.05):
                    if best[0] > c:
                        out_edges[c] += 1
                if best[0] >= thresh and t not in traded:
                    e, side, px, fee = best
                    won = (res == 1.0) if side == "yes" else (res == 0.0)
                    trades.append((ev, e, (1.0 if won else 0.0) - px - fee))
                    traded.add(t)
    return widths, inside, out_edges, trades, n_targets


def self_test():
    # the closed form must agree with brute-force enumeration of the sibling box
    for sb, sa in (([0.10, 0.20, 0.30], [0.12, 0.24, 0.33]),
                   ([0.05, 0.05, 0.05], [0.40, 0.40, 0.05]),
                   ([0.00, 0.00], [0.10, 0.10])):
        cf = feasible_interval(sb, sa)
        bf = brute_force_interval(sb, sa)
        assert abs(cf[0] - bf[0]) < 1e-6 and abs(cf[1] - bf[1]) < 1e-6, (cf, bf)
    # width is exactly the sum of sibling spreads, before clamping
    sb, sa = [0.10, 0.20, 0.30], [0.12, 0.24, 0.33]
    lo, hi = feasible_interval(sb, sa)
    assert abs((hi - lo) - sum(a - b for b, a in zip(sb, sa))) < 1e-12  # (bid, ask)
    # a book whose siblings alone exceed certainty is incoherent, not merely wide
    assert feasible_interval([0.6, 0.6], [0.7, 0.7]) is None
    # an absent offer must constrain NOTHING, not pin the sibling at zero
    assert feasible_interval([0.0, 0.0], [0.0, 0.0]) == (0.0, 1.0), "absent ask used as a ceiling"
    assert feasible_interval([0.2, 0.0], [0.25, 0.0])[0] == 0.0
    # tight siblings pin the target; wide ones do not
    tight = feasible_interval([0.30, 0.30], [0.31, 0.31])
    wide = feasible_interval([0.05, 0.05], [0.55, 0.55])
    assert (tight[1] - tight[0]) < 0.03 and (wide[1] - wide[0]) >= 0.89
    print("loo_feasible self-test: PASS  closed form matches brute-force enumeration, width is")
    print("  exactly the sum of sibling spreads, incoherent books are refused, and tight siblings")
    print("  pin the target while wide ones do not.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=os.path.join(ROOT, "data", "prod.sqlite"))
    ap.add_argument("--category", default="")
    ap.add_argument("--skew", type=float, default=5.0)
    ap.add_argument("--buffer", type=float, default=0.0)
    ap.add_argument("--threshold", type=float, default=0.01)
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    conn = sqlite3.connect(f"file:{a.db}?mode=ro", uri=True)
    mk, parts = load(conn, a.category, 30, 4)
    w, inside, oe, tr, n = gate(conn, mk, parts, a.skew, a.buffer, a.threshold)
    if not w:
        print(f"{a.category}: no synchronized snapshots"); return
    w.sort()
    q = lambda p: w[min(len(w) - 1, int(p * len(w)))]
    print(f"\n=== {a.category or 'ALL'} ===")
    print(f"  events {len(parts)}   withheld-target evaluations {n:,}")
    print(f"  feasible interval width:  p10 {q(.10):.3f}   median {q(.50):.3f}   p90 {q(.90):.3f}")
    print(f"  target price lies INSIDE the feasible interval: {100*inside/max(n,1):.1f}%")
    for c in (0.01, 0.02, 0.03, 0.05):
        print(f"  outside-interval edge > {int(c*100)}c: {oe[c]:,}  ({100*oe[c]/max(n,1):.3f}%)")
    if tr:
        p = [x[2] for x in tr]
        m, lo, hi = boot(p, [x[0] for x in tr])
        print(f"  trades {len(tr)}  mean {100*m:+.2f}c  CI [{100*lo:+.2f}, {100*hi:+.2f}]")
        by = defaultdict(list)
        for ev, e, pl in tr:
            lab = "1-2c" if e < .02 else "2-3c" if e < .03 else "3-5c" if e < .05 else "5c+"
            by[lab].append(pl)
        for lab in ("1-2c", "2-3c", "3-5c", "5c+"):
            if lab in by:
                v = by[lab]
                print(f"    guaranteed {lab}: n={len(v):4d}  realized {100*sum(v)/len(v):+.2f}c")
    else:
        print("  no trade ever cleared the guaranteed-edge threshold")


if __name__ == "__main__":
    main()
