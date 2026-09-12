#!/usr/bin/env python3
"""How much room is there for a general constraint-arbitrage engine on this book?

The desk already runs two special cases of one idea - ladder_arb (sell every bracket of a
mutually exclusive event when the bids sum over $1) and dominance_arb (one contract's condition
implies another's). Both are routed `auto`. Neither has produced a single proposal in the whole
recorded history, against 4,701 proposals from the forecasting strategies.

That zero is not self-interpreting. It could mean the book never violates these constraints, or
it could mean the violations are real but smaller than `ladder_min_edge` (2 cents/contract), or
that fees eat them. Those imply completely different things about whether generalizing the engine
is worth the work, so this measures the DISTRIBUTION of violations rather than the count of
trades, over the recorded quote history.

The fee point is the one that decides it. Kalshi takes

    taker fee = ceil_centicent(M * 0.07 * C * P * (1-P))

PER LEG. So a basket's fee grows with its leg count, and is worst at mid prices: two legs at 0.50
cost 3.5 cents before any edge exists. Generalizing to bigger baskets makes that worse, not
better. If violations live below the fee floor, the answer is not a better solver.

Nothing here trades or proposes. It reads recorded quotes and reports.

  python3 research/constraint_scan.py --since-days 7 --out reports/constraint_scan.md
"""
import argparse, math, os, sqlite3, statistics, sys, time
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INF = 1e12


def fee_per_contract(price, mult=1.0, taker=True):
    """Kalshi's schedule, rounded up to a centicent exactly as src/fees.h does."""
    if price <= 0 or price >= 1:
        return 0.0
    rate = 0.07 if taker else 0.0175
    return math.ceil(mult * rate * price * (1.0 - price) * 10000.0) / 10000.0


def span_of(strike_type, floor_s, cap_s):
    """The settle values a contract pays on, as a closed interval. Mirrors ladder.cpp::span_of."""
    if strike_type == "less" and cap_s is not None:
        return (-INF, cap_s - 1)
    if strike_type in ("greater", "greater_or_equal") and floor_s is not None:
        return (floor_s + 1, INF)
    if strike_type == "between" and floor_s is not None and cap_s is not None:
        return (floor_s, cap_s)
    return None


def strictly_contains(outer, inner):
    return (outer[0] <= inner[0] + 1e-9 and outer[1] >= inner[1] - 1e-9
            and not (outer[0] >= inner[0] - 1e-9 and outer[1] <= inner[1] + 1e-9))


def load_reference(conn):
    mk = {}
    for tk, ev, ser, st, fl, cp in conn.execute(
            "SELECT ticker,event_ticker,series_ticker,strike_type,floor_strike,cap_strike FROM markets"):
        mk[tk] = {"event": ev, "series": ser, "span": span_of(st, fl, cp), "stype": st}
    fees = {}
    for ser, ftype, fmult in conn.execute("SELECT ticker,fee_type,fee_multiplier FROM series"):
        fees[ser] = fmult if fmult is not None else 1.0
    excl = {}
    for ev, me, ex in conn.execute("SELECT event_ticker,mutually_exclusive,exhaustive FROM event_meta"):
        excl[ev] = (me == 1, ex == 1)
    return mk, fees, excl


def scan(conn, mk, fees, excl, since_ts, max_events, verbose, args_max_leg_age=5, skip_events=0):
    by_event = defaultdict(list)
    for tk, info in mk.items():
        if info["event"]:
            by_event[info["event"]].append(tk)

    # Only events the exchange marks mutually exclusive: without that flag the sell-side argument
    # does not hold, and inferring exclusivity from strikes is the mistake this whole lane is
    # supposed to avoid.
    events = [e for e, ts in by_event.items() if len(ts) >= 2 and excl.get(e, (False, False))[0]]
    events.sort()
    events = events[skip_events:]
    if max_events:
        events = events[:max_events]

    out = {"sell": [], "dom": [], "fee_floor_sell": [], "fee_floor_dom": [],
           "events_scanned": 0, "events_with_quotes": 0, "ticks": 0}

    for ev in events:
        tickers = by_event[ev]
        qmarks = ",".join("?" for _ in tickers)
        rows = conn.execute(
            f"SELECT ts,ticker,yes_bid,yes_ask,yes_bid_size,yes_ask_size FROM quotes "
            f"WHERE ticker IN ({qmarks}) AND ts>=? ORDER BY ts", (*tickers, since_ts)).fetchall()
        out["events_scanned"] += 1
        if not rows:
            continue
        out["events_with_quotes"] += 1

        # Containment is a property of the strikes, not of the book, so it is settled once per
        # event. Most events are disjoint brackets with no containment at all, and those then cost
        # nothing per tick instead of an O(n^2) rescan.
        dom_pairs = []
        out["events_checked_for_containment"] = out.get("events_checked_for_containment", 0) + 1
        for th in tickers:
            sh = mk[th]["span"]
            if not sh:
                continue
            for te in tickers:
                if te != th and mk[te]["span"] and strictly_contains(mk[te]["span"], sh):
                    dom_pairs.append((th, te))
        if dom_pairs:
            out["events_with_containment"] = out.get("events_with_containment", 0) + 1

        live = {}
        best_sell = None     # (net, gross, fee, ts, nlegs)
        best_dom = None
        for ts, tk, yb, ya, ybs, yas in rows:
            live[tk] = (yb or 0.0, ya or 0.0, ybs or 0.0, yas or 0.0, ts)
            out["ticks"] += 1
            # Legs must be quoted at the SAME time or the basket is fiction. Without this the scan
            # happily combines a bracket's bid from early in the hour with another's from later,
            # and reports a 33-cent locked arbitrage that never existed in any single book. This
            # is the error the cross-venue literature calls mistiming, reproduced in one venue.
            fresh = {t: v for t, v in live.items() if ts - v[4] <= args_max_leg_age}

            # ---- sell-side ladder: buy NO on every bracket -------------------------------
            # Pays |S|-1 in every state where one bracket wins and |S| when none does, so it is
            # locked when the bids sum over $1 after the NO-side fees. Needs exclusivity only.
            legs = [(t, v) for t, v in fresh.items() if v[0] > 0 and v[2] > 0]
            if len(legs) >= 2:
                s = sum(v[0] for _, v in legs)
                f = sum(fee_per_contract(1.0 - v[0], fees.get(mk[t]["series"], 1.0)) for t, v in legs)
                gross, net = s - 1.0, s - 1.0 - f
                if best_sell is None or net > best_sell[0]:
                    best_sell = (net, gross, f, ts, len(legs))

            # ---- dominance: strict span containment --------------------------------------
            for th, te in dom_pairs:
                vh, ve = fresh.get(th), fresh.get(te)
                if not vh or not ve or vh[0] <= 0 or vh[2] <= 0:
                    continue
                if not (0 < ve[1] < 1) or ve[3] <= 0:
                    continue
                f = (fee_per_contract(1.0 - vh[0], fees.get(mk[th]["series"], 1.0))
                     + fee_per_contract(ve[1], fees.get(mk[te]["series"], 1.0)))
                gross = vh[0] - ve[1]
                net = gross - f
                if best_dom is None or net > best_dom[0]:
                    best_dom = (net, gross, f, ts, 2)

        if best_sell:
            out["sell"].append((ev, *best_sell))
            out["fee_floor_sell"].append(best_sell[2])
        if best_dom:
            out["dom"].append((ev, *best_dom))
            out["fee_floor_dom"].append(best_dom[2])
        if verbose and out["events_scanned"] % 25 == 0:
            print(f"  ... {out['events_scanned']}/{len(events)} events", file=sys.stderr)
    return out


def pct(xs, q):
    if not xs:
        return float("nan")
    xs = sorted(xs)
    return xs[min(len(xs) - 1, max(0, int(len(xs) * q)))]


def report(out, args, path):
    L = []
    W = L.append
    W("# Constraint arbitrage: how much room is actually there?\n")
    W(f"Every leg required within {args.max_leg_age:.0f}s of the others, so no basket is assembled "
      f"from quotes that never stood at the same moment.\n\n")
    W(f"Recorded quotes, {args.since_days} days back. {out['events_scanned']} mutually-exclusive "
      f"events examined, {out['events_with_quotes']} of them with quotes in the window, "
      f"{out['ticks']:,} quote updates replayed.\n")
    W("Each event contributes its single best moment, not every tick, so nothing below is one "
      "wide spread counted thousands of times.\n")

    for key, label, note in (("sell", "Sell-side ladder", "buy NO on every bracket; needs only the exclusivity flag"),
                             ("dom", "Dominance pair", "sell the harder contract, buy the easier one")):
        rows = out[key]
        W(f"\n## {label}\n\n*{note}*\n")
        if not rows:
            W("\nNo event ever presented this shape in the window.\n")
            continue
        nets = [r[1] for r in rows]
        gross = [r[2] for r in rows]
        feez = [r[3] for r in rows]
        W(f"\n{len(rows)} events presented the shape at least once.\n")
        W("\n| | best net edge / contract | gross (before fees) | fee paid |")
        W("\n|---|---:|---:|---:|\n")
        for q, nm in ((0.50, "median event"), (0.90, "90th pct"), (0.99, "99th pct")):
            W(f"| {nm} | {pct(nets,q):+.4f} | {pct(gross,q):+.4f} | {pct(feez,q):.4f} |\n")
        W(f"| **best single moment** | **{max(nets):+.4f}** | {max(gross):+.4f} | - |\n")
        n_pos = sum(1 for x in nets if x > 0)
        n_thresh = sum(1 for x in nets if x > 0.02)
        W(f"\n- events where the net edge was ever positive at all: **{n_pos} of {len(rows)}**\n")
        W(f"- events that ever cleared the live `ladder_min_edge` of $0.02: **{n_thresh}**\n")
        W(f"- median fee the basket had to overcome: **${statistics.median(feez):.4f}/contract**\n")
        if n_pos:
            W("\nBest events:\n\n| event | net | gross | fee | legs |\n|---|---:|---:|---:|---:|\n")
            for ev, net, g, f, ts, nl in sorted(rows, key=lambda r: -r[1])[:8]:
                W(f"| {ev[:46]} | {net:+.4f} | {g:+.4f} | {f:.4f} | {nl} |\n")

    W("\n### Why the dominance finder found nothing\n")
    W(f"\nOf the {out.get('events_checked_for_containment',0)} events examined, "
      f"**{out.get('events_with_containment',0)}** contain even one pair where one contract's "
      "condition implies another's.\n")
    W("\nThat is structural, not bad luck. `run_ladder_arb` groups markets by event and hands one "
      "event's contracts to the finder, but the brackets inside a mutually exclusive event are "
      "disjoint by construction - that is what makes the event exclusive. Containment can only "
      "appear BETWEEN events, or between series, which is precisely where the code never looks. "
      "`find_dominance_arb` has therefore never had a single candidate pair to price, and no "
      "threshold change would alter that. If dominance is wanted, the scan has to be re-wired to "
      "compare across events on the same underlying, and that needs a verified statement that the "
      "two events settle off the same source - the cross-event relation problem.\n")
    W("\n## What this means for constraint_arb_v2\n")
    allf = out["fee_floor_sell"] + out["fee_floor_dom"]
    if allf:
        W(f"\nThe median basket paid **${statistics.median(allf):.4f}/contract** in fees just to exist. "
          "Kalshi's fee is charged per leg and is largest at mid prices, so a solver that finds "
          "bigger baskets is buying a bigger fee bill. Any generalization has to earn its extra "
          "legs against that, and the fee is known before the edge is.\n")
    W("\nNothing here is a forecast and nothing here traded. It is the headroom measurement that "
      "should decide whether the engine is worth building, and at what threshold.\n")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "w").write("".join(L))
    return "".join(L)


def self_test():
    assert fee_per_contract(0.5) == math.ceil(0.07 * 0.25 * 10000) / 10000
    assert fee_per_contract(0.0) == 0.0 and fee_per_contract(1.0) == 0.0
    # the fee is a hump: most expensive in the middle, near-free at the extremes
    assert fee_per_contract(0.5) > fee_per_contract(0.9) > fee_per_contract(0.99)
    assert span_of("greater", 100, None) == (101, INF)
    assert span_of("less", None, 70) == (-INF, 69)
    assert span_of("between", 70, 74) == (70, 74)
    assert span_of("structured", None, None) is None
    # ">= 110" is strictly inside ">= 100", so the wider one contains it; not the reverse
    assert strictly_contains(span_of("greater", 100, None), span_of("greater", 110, None))
    assert not strictly_contains(span_of("greater", 110, None), span_of("greater", 100, None))
    assert not strictly_contains(span_of("greater", 100, None), span_of("greater", 100, None))
    # disjoint brackets never contain each other, which is why `between` ladders need the flag
    assert not strictly_contains(span_of("between", 70, 74), span_of("between", 75, 79))
    print("constraint_scan self-test: PASS  fees hump at the middle, spans and containment hold,")
    print("  and disjoint brackets are correctly refused as dominance pairs.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=os.path.join(ROOT, "data", "prod.sqlite"))
    ap.add_argument("--since-days", type=float, default=7.0)
    ap.add_argument("--max-events", type=int, default=0)
    ap.add_argument("--skip-events", type=int, default=0)
    ap.add_argument("--json-out", default="")
    ap.add_argument("--out", default=os.path.join(ROOT, "reports", "constraint_scan.md"))
    ap.add_argument("--max-leg-age", type=float, default=5.0,
                    help="every leg's quote must be this recent, in seconds, or the basket is not simultaneous")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    conn = sqlite3.connect(f"file:{args.db}?mode=ro", uri=True)
    mk, fees, excl = load_reference(conn)
    since = int(time.time() - args.since_days * 86400)
    out = scan(conn, mk, fees, excl, since, args.max_events, args.verbose, args.max_leg_age, args.skip_events)
    if args.json_out:
        import json as _j
        _j.dump(out, open(args.json_out, "w"))
        print(f"chunk: {out['events_scanned']} events, {out['ticks']:,} ticks -> {args.json_out}")
        return
    text = report(out, args, args.out)
    print(text[:1800])
    print(f"\nwrote {os.path.relpath(args.out, ROOT)}")


if __name__ == "__main__":
    main()
