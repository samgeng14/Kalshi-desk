#!/usr/bin/env python3
"""What a study is entitled to claim, and what it takes to claim a fill.

Two of the weather studies in docs/hypotheses.md report a "ret" column next to their Brier
scores. Those columns were computed by taking the decision at hour h and then filling at the next
print on our side of the book within the following THREE HOURS. A print three hours later is not
evidence that an order submitted at hour h would have filled; it is evidence that somebody traded
later, possibly at a price that moved precisely because the thing we were predicting had become
clearer. Any edge measured that way is partly hindsight.

So a study has to declare what it is:

  PREDICTION_ONLY   compares a model against prices. May conclude that a model is better or worse
                    calibrated than the market. May NOT report a return.
  EXECUTION         additionally claims a trade would have happened. Needs a quote that was
                    executable at the decision time, with size, and a fill within a stated latency
                    budget. Anything else is prediction with a return column bolted on.

The desk cannot currently run an execution study of any kind. As of 2026-09-07 the `books` table
holds zero rows across both databases: no order book has ever been recorded, so no depth existed
at any decision point in this desk's history. Top-of-book quotes with size go back only to
2026-09-05. The recorder added in session 3 is what makes future execution studies possible; it
cannot make past ones possible.
"""
import collections

PREDICTION_ONLY = "prediction_only"
EXECUTION = "execution"

# A decision this desk makes is acted on within seconds, not hours. A fill further away than this
# is a different trade at a different time under different information.
DEFAULT_MAX_LAG_S = 60


def banner(kind, reason=""):
    """Print what this study may and may not conclude, before any numbers appear."""
    line = "=" * 78
    print(line)
    if kind == EXECUTION:
        print("STUDY: EXECUTION - claims about fills are in scope")
    else:
        print("STUDY: PREDICTION ONLY - calibration only. Any return column below is NOT")
        print("       evidence of a tradable edge and must not be quoted as one.")
    if reason:
        for chunk in reason.split("\n"):
            print("       " + chunk)
    print(line)


def fill_at(prints, t_decision, want_side, max_lag_s=DEFAULT_MAX_LAG_S):
    """First print on `want_side` within the latency budget after the decision.

    `prints` is [(ts, yes_price, taker_side)] sorted by ts. Returns (price, lag) or (None, None).
    Deliberately refuses anything later than the budget rather than reaching for the next print
    whenever it happens: the whole point is that a decision either could be acted on promptly or
    could not be acted on at all.
    """
    for ts, px, side in prints:
        if ts < t_decision:
            continue
        if ts > t_decision + max_lag_s:
            return None, None
        if side == want_side:
            return (px if want_side == "yes" else 1 - px), ts - t_decision
    return None, None


def fill_report(lags, misses):
    """How much of the study survived contact with a latency budget."""
    total = len(lags) + misses
    if not total:
        print("  no decisions")
        return
    print(f"  decisions {total}   filled {len(lags)} ({len(lags)/total:.1%})   "
          f"no fill in budget {misses} ({misses/total:.1%})")
    if lags:
        s = sorted(lags)
        print(f"  fill lag: median {s[len(s)//2]}s   p90 {s[int(0.9*len(s))]}s   max {max(s)}s")


def require_tables(conn, tables):
    """Abort loudly when the data a study needs is not there any more."""
    have = {r[0] for r in conn.execute("SELECT name FROM sqlite_master WHERE type='table'")}
    missing = [t for t in tables if t not in have]
    if missing:
        raise SystemExit(
            f"missing tables: {', '.join(missing)}.\n"
            "This study cannot be reproduced from the current database. The tables it read were\n"
            "not retained, which is the argument for the session-3 recorder rather than a reason\n"
            "to trust the numbers already written down from it.")
