#!/usr/bin/env python3
"""The volleyball fair: a rally-point Markov model with one parameter, solved from the pregame price.

C1's volleyball fair (plan 5(d): "fair from a Markov point model"; the college lane's spec). It
is for a KXNCAAWVMATCH-style market: P(the market's YES team wins the match) from the sets and
points so far and the pregame YES price p0. deskd's C++ (strat/volley_model.h) implements the same
functions and must reproduce this file's golden rows to 1e-9, so what follows is the exact
definition, not a sketch.

The rules (RULES, women's college volleyball): best of 5 sets; sets 1 to 4 to 25 points, the
deciding 5th set to 15; a set is won by the first team to the target with a lead of at least 2
(win by 2, no cap). Rally scoring: every rally gives a point to one team.

The model: each rally is won by the YES team with the same probability q, independent of every
other rally, of who serves and of the score.

  D(q) = q^2 / (q^2 + (1 - q)^2)
         P(YES wins the set) from level at T-1 all or later: it must win two rallies in a row
         before losing two in a row, D = q^2 + 2 q (1 - q) D.

  set_p(q, a, b, T)   P(YES wins the set) from a (YES) to b (opponent) in a set to T, win by 2:
    1. a >= T - 1 and b >= T - 1 (the deuce zone), with d = a - b:
         d >= 2: 1;  d <= -2: 0;  d = 0: D;  d = 1: q + (1 - q) D;  d = -1: q D
    2. otherwise a >= T: 1 (the lead is at least 2);  b >= T: 0
    3. otherwise q set_p(a + 1, b) + (1 - q) set_p(a, b + 1)
  so set_p(q, 24, 0, 25) = 1 - (1 - q)^24 (1 - D): YES loses only by 24 straight rallies and then
  the deuce.

  match_p(q, sy, so, a, b)   P(YES wins the match) with sets sy (YES) to so won and a to b in the
  current set. need = (best_of + 1) / 2 = 3. sy >= need: 1; so >= need: 0. Otherwise, with the
  current set number k = sy + so + 1 and T(k) = 15 when k = best_of (the 5th) else 25,
    s = set_p(q, a, b, T(k)),  match_p = s M(sy + 1, so) + (1 - s) M(sy, so + 1)
  and M(x, y) = match_p(q, x, y, 0, 0), the match from the start of the next set.

  q_for_p0(p0)   the q that makes the pregame match price p0: p0 clamped to [0.02, 0.98], then
  bisection on match_p(q, 0, 0, 0, 0) = p0 over lo = 0.05, hi = 0.95 (match_p is increasing in
  q): repeat mid = (lo + hi) / 2; stop when mid equals lo or hi (no double strictly between);
  match_p(mid) < p0 sets lo = mid, else hi = mid; q = (lo + hi) / 2. None when p0 is missing or
  not finite, or when p0 is outside [match_p(0.05), match_p(0.95)] (never, for these rules).

  live_p(state, p0)   match_p(q_for_p0(p0), sets_yes, sets_opp, pts_yes, pts_opp) for a state
  {"sets_yes", "sets_opp", "pts_yes", "pts_opp"} of non-negative whole numbers; None for a
  malformed state or no q.

So p0 = 1/2 gives q = 1/2 and 1/2 at every level state; the model is symmetric (YES at p0 and the
opponent at 1 - p0, with the state mirrored, sum to 1); and in the 5th set the match is the set.

The model file (model_file(), --write FILE): {"model": "volley_markov", "version": 1, "rules":
{"best_of": 5, "set_to": 25, "last_set_to": 15, "win_by": 2}, "golden": [10 rows {"in":
{"sets_yes", "sets_opp", "pts_yes", "pts_opp", "p0"}, "p": live_p}]}, the golden rows spread over
the first set, deuces, a set point either way and the 5th set. The self-test prints them, checks
set_p and match_p against a second, independent computation (rally-by-rally forward
probabilities, no closed form), and when tests/deskd/fixtures/volleyball_golden.json exists (the
C++ step's golden file, in model_file's shape) reproduces every row of it to 1e-9.

Standard library only. No network, no data. Only the 2-point win is implemented (the closed-form
deuce); other rules refuse.

Usage:
  python3 tools/volleyball.py --p0 0.62 --state 1,0,20,18     # q and the live p
  python3 tools/volleyball.py --write models/volley_markov_v1.json [--force]
  python3 tools/volleyball.py --self-test
"""
import argparse
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

RULES = {"best_of": 5, "set_to": 25, "last_set_to": 15, "win_by": 2}
P0_MIN, P0_MAX = 0.02, 0.98
Q_LO, Q_HI = 0.05, 0.95
STATE_KEYS = ("sets_yes", "sets_opp", "pts_yes", "pts_opp")
GOLDEN_INPUTS = [
    # (sets_yes, sets_opp, pts_yes, pts_opp, p0): the first rally, an even and a lopsided price,
    # a first-set lead, a set point against, deuces, an advantage, and the 5th set to 15
    (0, 0, 0, 0, 0.5),
    (0, 0, 0, 0, 0.75),
    (0, 0, 10, 4, 0.4),
    (1, 0, 20, 22, 0.62),
    (1, 1, 24, 24, 0.3),
    (2, 1, 23, 24, 0.55),
    (0, 2, 26, 25, 0.8),
    (2, 2, 0, 0, 0.65),
    (2, 2, 13, 14, 0.45),
    (2, 2, 14, 14, 0.9),
]


def check_rules(rules):
    if rules.get("win_by") != 2:
        raise ValueError("only win by 2 is implemented (the closed-form deuce)")
    bo, st, lst = rules.get("best_of"), rules.get("set_to"), rules.get("last_set_to")
    if not all(isinstance(v, int) and not isinstance(v, bool) and v >= 1 for v in (bo, st, lst)) or bo % 2 == 0:
        raise ValueError(f"rules {rules}: best_of odd, set_to and last_set_to whole and positive")


def deuce_p(q):
    return q * q / (q * q + (1.0 - q) * (1.0 - q))


def set_p(q, a, b, target, win_by=2):
    """P(YES wins the set) from a-b in a set to target, win by 2 (see the module docstring)."""
    if win_by != 2:
        raise ValueError("only win by 2 is implemented (the closed-form deuce)")
    if not 0.0 <= q <= 1.0 or a < 0 or b < 0 or target < 1:
        raise ValueError(f"set_p: q {q}, score {a}-{b}, target {target}")
    d_q = deuce_p(q)
    memo = {}

    def p(x, y):
        if x >= target - 1 and y >= target - 1:
            d = x - y
            if d >= 2:
                return 1.0
            if d <= -2:
                return 0.0
            if d == 0:
                return d_q
            return q + (1.0 - q) * d_q if d == 1 else q * d_q
        if x >= target:
            return 1.0
        if y >= target:
            return 0.0
        v = memo.get((x, y))
        if v is None:
            v = memo[(x, y)] = q * p(x + 1, y) + (1.0 - q) * p(x, y + 1)
        return v

    return p(a, b)


def set_target(k, rules=RULES):
    """The points to win set number k (1-based)."""
    return rules["last_set_to"] if k == rules["best_of"] else rules["set_to"]


def match_p(q, sets_yes, sets_opp, pts_yes, pts_opp, rules=RULES):
    """P(YES wins the match) from the sets won and the current set's points."""
    check_rules(rules)
    need = (rules["best_of"] + 1) // 2
    if sets_yes >= need:
        return 1.0
    if sets_opp >= need:
        return 0.0
    memo, fresh = {}, {}   # fresh: a set from 0-0, by its target

    def m(sy, so):   # from the start of a set
        if sy >= need:
            return 1.0
        if so >= need:
            return 0.0
        v = memo.get((sy, so))
        if v is None:
            t = set_target(sy + so + 1, rules)
            if t not in fresh:
                fresh[t] = set_p(q, 0, 0, t)
            s = fresh[t]
            v = memo[(sy, so)] = s * m(sy + 1, so) + (1.0 - s) * m(sy, so + 1)
        return v

    s = set_p(q, pts_yes, pts_opp, set_target(sets_yes + sets_opp + 1, rules))
    return s * m(sets_yes + 1, sets_opp) + (1.0 - s) * m(sets_yes, sets_opp + 1)


def q_for_p0(p0, rules=RULES):
    """The rally probability that prices the match at p0 before the first rally; None for no p0."""
    if p0 is None or isinstance(p0, bool) or not isinstance(p0, (int, float)) or not math.isfinite(p0):
        return None
    p0 = min(max(float(p0), P0_MIN), P0_MAX)
    lo, hi = Q_LO, Q_HI
    if not match_p(lo, 0, 0, 0, 0, rules) <= p0 <= match_p(hi, 0, 0, 0, 0, rules):
        return None
    while True:
        mid = (lo + hi) / 2.0
        if mid == lo or mid == hi:
            break
        if match_p(mid, 0, 0, 0, 0, rules) < p0:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2.0


def _count(v):
    return isinstance(v, int) and not isinstance(v, bool) and v >= 0


def live_p(state, p0, rules=RULES):
    """P(YES wins the match) at state {sets_yes, sets_opp, pts_yes, pts_opp} from the pregame p0."""
    if not isinstance(state, dict) or not all(_count(state.get(k)) for k in STATE_KEYS):
        return None
    q = q_for_p0(p0, rules)
    if q is None:
        return None
    return match_p(q, state["sets_yes"], state["sets_opp"], state["pts_yes"], state["pts_opp"], rules)


def model_file(rules=RULES):
    check_rules(rules)
    golden = []
    for sy, so, a, b, p0 in GOLDEN_INPUTS:
        x = {"sets_yes": sy, "sets_opp": so, "pts_yes": a, "pts_opp": b, "p0": p0}
        golden.append({"in": x, "p": live_p(x, p0, rules)})
    return {"model": "volley_markov", "version": 1, "rules": dict(rules), "golden": golden}


def dumps_model(obj):
    """One key per line, one golden row per line; floats by repr (json's own)."""
    lines = []
    for k, v in obj.items():
        if k == "golden":
            inner = ",\n    ".join(json.dumps(g, allow_nan=False) for g in v)
            lines.append(f'  "golden": [\n    {inner}\n  ]')
        else:
            lines.append(f"  {json.dumps(k)}: {json.dumps(v, allow_nan=False)}")
    return "{\n" + ",\n".join(lines) + "\n}\n"


def golden_error(obj, tol=1e-9):
    """The first golden row of a model-file-shaped dict that live_p does not reproduce to tol, or
    a malformed key; None when every row does."""
    if not isinstance(obj, dict) or not isinstance(obj.get("golden"), list) or not obj["golden"]:
        return "no golden rows"
    rules = obj.get("rules", RULES)
    try:
        check_rules(rules)
    except ValueError as e:
        return str(e)
    for i, g in enumerate(obj["golden"]):
        x = g.get("in") if isinstance(g, dict) else None
        if not isinstance(x, dict) or any(k not in x for k in STATE_KEYS + ("p0",)) or not isinstance(g.get("p"), (int, float)):
            return f"golden row {i}: not {{\"in\": {{{', '.join(STATE_KEYS)}, p0}}, \"p\"}}"
        p = live_p(x, x["p0"], rules)
        if p is None or abs(p - g["p"]) > tol:
            return f"golden row {i}: {p!r} vs {g['p']!r}"
    return None


def write_model(path, force=False):
    if os.path.exists(path) and not force:
        raise OSError(f"{path} exists (a shipped version is never rewritten); --force or another path")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as fh:
        fh.write(dumps_model(model_file()))
    os.replace(tmp, path)


def run(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--p0", type=float, help="the pregame YES price, as a probability")
    ap.add_argument("--state", help="sets_yes,sets_opp,pts_yes,pts_opp (default 0,0,0,0)")
    ap.add_argument("--write", metavar="FILE", help="write the model file (rules and 10 golden rows)")
    ap.add_argument("--force", action="store_true", help="overwrite --write's file")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if a.write:
        try:
            write_model(a.write, a.force)
        except OSError as e:
            print(f"volleyball: {e}", file=sys.stderr)
            return 2
        print(f"wrote {a.write}")
        return 0
    if a.p0 is None:
        ap.error("--p0 (with --state), --write or --self-test")
    try:
        parts = [int(v) for v in (a.state or "0,0,0,0").split(",")]
    except ValueError:
        parts = []
    if len(parts) != 4:
        ap.error("--state: four whole numbers, sets_yes,sets_opp,pts_yes,pts_opp")
    state = dict(zip(STATE_KEYS, parts))
    q, p = q_for_p0(a.p0), live_p(state, a.p0)
    if p is None:
        print("volleyball: no fair (a negative state, or no q for that p0)", file=sys.stderr)
        return 2
    print(f"q {q:.6f}  p {p:.6f}  ({state['sets_yes']}-{state['sets_opp']} in sets, {state['pts_yes']}-{state['pts_opp']} "
          f"in set {state['sets_yes'] + state['sets_opp'] + 1})")
    return 0


# ---------------------------------------------------------------- self-test

def set_p_forward(q, a, b, target, rallies=600):
    """A second computation of set_p: the score's probabilities carried forward rally by rally,
    no closed form (the deuce cut after `rallies` rallies, a mass under (2q(1-q))^(rallies/2))."""
    if a >= target and a - b >= 2:
        return 1.0
    if b >= target and b - a >= 2:
        return 0.0
    dist, won = {(a, b): 1.0}, 0.0
    for _ in range(rallies):
        nd = {}
        for (x, y), w in dist.items():
            for (nx, ny), pw in (((x + 1, y), q), ((x, y + 1), 1.0 - q)):
                if nx >= target and nx - ny >= 2:
                    won += w * pw
                elif not (ny >= target and ny - nx >= 2):
                    nd[(nx, ny)] = nd.get((nx, ny), 0.0) + w * pw
        dist = nd
        if not dist:
            break
    return won


def match_p_forward(q, sy, so, a, b, rules=RULES):
    """A second computation of match_p: the sets' probabilities carried forward set by set."""
    need = (rules["best_of"] + 1) // 2
    dist, won = {(sy, so): 1.0}, 0.0
    first = True
    while dist:
        nd = {}
        for (x, y), w in dist.items():
            if x >= need:
                won += w
                continue
            if y >= need:
                continue
            t = set_target(x + y + 1, rules)
            s = set_p_forward(q, a, b, t) if first else set_p_forward(q, 0, 0, t)
            for k, pw in (((x + 1, y), s), ((x, y + 1), 1.0 - s)):
                nd[k] = nd.get(k, 0.0) + w * pw
        dist, first = nd, False
    return won


def self_test():
    fails = []

    def check(name, ok, detail=""):
        print("%s %s%s" % ("OK  " if ok else "FAIL", name, (": " + str(detail)) if detail else ""))
        if not ok:
            fails.append(name)

    qs = (0.3, 0.45, 0.5, 0.52, 0.55, 0.62, 0.8)
    check("set_p(0.5, a, a, T) = 1/2", all(abs(set_p(0.5, a, a, t) - 0.5) < 1e-15 for t in (15, 25) for a in (0, 7, 13, 14, 24, 30)))
    check("at the deuce set_p = q^2 / (q^2 + (1-q)^2)",
          all(set_p(q, a, a, t) == q * q / (q * q + (1 - q) ** 2) for q in qs for t, a in ((25, 24), (25, 31), (15, 14))))
    check("set_p(q, 25, 23, 25) = 1, set_p(q, 23, 25, 25) = 0, an advantage is q + (1-q) D and q D",
          all(set_p(q, 25, 23, 25) == 1.0 and set_p(q, 23, 25, 25) == 0.0 and set_p(q, 27, 29, 25) == 0.0
              and set_p(q, 26, 25, 25) == q + (1 - q) * deuce_p(q) and set_p(q, 25, 26, 25) == q * deuce_p(q) for q in qs))
    check("set_p(q, 24, 0, 25) = 1 - (1-q)^24 (1 - D(q)) by hand",
          all(abs(set_p(q, 24, 0, 25) - (1 - (1 - q) ** 24 * (1 - deuce_p(q)))) < 1e-15 for q in qs))
    check("a set to 2 by 2 from 0-0 is the deuce itself, D = q^2 + 2q(1-q)D",
          all(abs(set_p(q, 0, 0, 2) - deuce_p(q)) < 1e-15 for q in qs))
    worst = max(abs(set_p(q, a, b, t) - set_p_forward(q, a, b, t))
                for q in qs for t in (15, 25) for a, b in ((0, 0), (3, 9), (12, 12), (20, 23), (24, 23), (14, 13), (26, 26)))
    check("set_p equals the rally-by-rally forward computation (no closed form)", worst < 1e-12, f"worst {worst:.1e}")
    worst = max(abs(match_p(q, *st) - match_p_forward(q, *st))
                for q in (0.45, 0.5, 0.53, 0.6) for st in ((0, 0, 0, 0), (1, 0, 20, 22), (1, 2, 24, 25), (2, 2, 9, 11)))
    check("match_p equals the set-by-set forward computation", worst < 1e-12, f"worst {worst:.1e}")
    check("match_p(q) + match_p(1-q) = 1 at 0-0, and mirrored states",
          all(abs(match_p(q, 0, 0, 0, 0) + match_p(1 - q, 0, 0, 0, 0) - 1) < 1e-14
              and abs(match_p(q, 2, 1, 20, 17) + match_p(1 - q, 1, 2, 17, 20) - 1) < 1e-14 for q in qs))
    check("the 5th set plays to 15: at 2-2 the match is set_p to 15",
          all(match_p(q, 2, 2, a, b) == set_p(q, a, b, 15) for q in qs for a, b in ((0, 0), (13, 14), (16, 15))))
    check("finished matches: 3 sets won is 1, 3 lost is 0", match_p(0.4, 3, 1, 0, 0) == 1.0 and match_p(0.6, 2, 3, 5, 0) == 0.0)
    grid = [0.40 + 0.005 * i for i in range(41)]
    mp = [match_p(q, 0, 0, 0, 0) for q in grid]
    check("monotone in q", all(x < y for x, y in zip(mp, mp[1:])), f"{mp[0]:.4f} .. {mp[-1]:.4f}")
    by_pts = [match_p(0.5, 1, 1, a, 10) for a in range(0, 30)]
    by_opp = [match_p(0.5, 1, 1, 10, b) for b in range(0, 30)]
    check("monotone in points: up with YES's, down with the opponent's",
          all(x <= y for x, y in zip(by_pts, by_pts[1:])) and all(x >= y for x, y in zip(by_opp, by_opp[1:]))
          and by_pts[0] < by_pts[-1] and by_opp[0] > by_opp[-1])
    worst = max(abs(q_for_p0(match_p(q, 0, 0, 0, 0)) - q) for q in (0.44, 0.47, 0.5, 0.505, 0.53, 0.56))
    check("q_for_p0(match_p(q)) returns q to 1e-12", worst < 1e-12, f"worst {worst:.1e}")
    check("q_for_p0: 1/2 for 1/2, clamped to [0.02, 0.98], None without a price",
          q_for_p0(0.5) == 0.5 and q_for_p0(0.999) == q_for_p0(0.98) and q_for_p0(0.0) == q_for_p0(0.02)
          and q_for_p0(None) is None and q_for_p0(float("nan")) is None and q_for_p0(True) is None,
          f"q(0.5) {q_for_p0(0.5)!r} q(0.98) {q_for_p0(0.98):.6f} q(0.02) {q_for_p0(0.02):.6f}")
    st = {"sets_yes": 1, "sets_opp": 0, "pts_yes": 20, "pts_opp": 22}
    mirror = {"sets_yes": 0, "sets_opp": 1, "pts_yes": 22, "pts_opp": 20}
    check("live_p: p0 before the first rally; symmetric (YES at p0 + the opponent at 1 - p0 = 1)",
          all(abs(live_p(dict.fromkeys(STATE_KEYS, 0), p0) - p0) < 1e-12 for p0 in (0.1, 0.35, 0.5, 0.8))
          and all(abs(live_p(st, p0) + live_p(mirror, 1 - p0) - 1) < 1e-12 for p0 in (0.2, 0.5, 0.7)))
    check("live_p: a malformed state or no price gives None",
          live_p({"sets_yes": -1, "sets_opp": 0, "pts_yes": 0, "pts_opp": 0}, 0.5) is None
          and live_p({"sets_yes": 0, "sets_opp": 0, "pts_yes": 1.5, "pts_opp": 0}, 0.5) is None
          and live_p({"sets_yes": 0, "sets_opp": 0, "pts_yes": 0}, 0.5) is None and live_p(st, None) is None)
    try:
        match_p(0.5, 0, 0, 0, 0, dict(RULES, win_by=3))
        check("win by anything but 2 is refused", False)
    except ValueError:
        check("win by anything but 2 is refused", True)

    # the golden rows: emitted here (the C++ step's fixture must agree), reloaded, checked
    obj = model_file()
    print("     golden rows (model_file(), the shape of models/volley_markov_v1.json):")
    for g in obj["golden"]:
        print("       " + json.dumps(g))
    back = json.loads(dumps_model(obj))
    check("model_file: 10 golden rows that reload equal and reproduce", back == obj and len(obj["golden"]) == 10
          and golden_error(back) is None and obj["model"] == "volley_markov" and obj["version"] == 1 and obj["rules"] == RULES)
    rows = obj["golden"]
    check("golden: p0 1/2 at 0-0 is 1/2, p0 3/4 at 0-0 is 3/4, the 5th-set deuce is D(q)",
          rows[0]["p"] == 0.5 and abs(rows[1]["p"] - 0.75) < 1e-12 and rows[9]["in"]["pts_yes"] == 14
          and abs(rows[9]["p"] - deuce_p(q_for_p0(0.9))) < 1e-15)
    bad = json.loads(json.dumps(obj))
    bad["golden"][6]["p"] += 2e-9
    e = golden_error(bad)
    check("a golden row off by 2e-9 is refused, naming the row", e is not None and e.startswith("golden row 6"), e)
    fx = os.path.join(ROOT, "tests", "deskd", "fixtures", "volleyball_golden.json")
    if os.path.exists(fx):
        with open(fx) as fh:
            fobj = json.load(fh)
        e = golden_error(fobj if isinstance(fobj, dict) else {"golden": fobj})
        check("tests/deskd/fixtures/volleyball_golden.json: every row reproduces to 1e-9", e is None, e or "")
    else:
        print("     tests/deskd/fixtures/volleyball_golden.json: not present, skipped")
    print("volleyball self-test: %s" % ("ok" if not fails else "FAILED (%d)" % len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
