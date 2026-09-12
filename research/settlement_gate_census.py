#!/usr/bin/env python3
"""Which Kalshi markets can actually pass the settlement-verification gate, and under which model.

The desk refuses to trade a contract whose settlement source has not been read and mapped
(probability.cpp::settlement_verified_for). A research program aimed at a model that can never
clear that gate produces numbers nobody is allowed to act on, so this measures the gate FIRST:
for every open market in the watched universe it works out which strategy claims it (registry
order - the first strategy that covers a market wins) and whether that strategy's settlement
predicate is satisfied for that specific contract.

Two distinct quantities, and confusing them is the whole trap:
  covered_by     - who prices it today, which is who gets to it first in the registry.
  verifiable_by  - every model whose settlement predicate the contract satisfies, ignoring order.
A market with a non-empty verifiable_by but a covered_by that fails the gate is a market the
desk is capable of verifying and is throwing away on registry precedence.

No writes. Read-only against prod.sqlite.
"""
import argparse, json, os, re, sqlite3, sys, time
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def default_db(root):
    """The snapshot, not the live file.

    A SQLite connection opened `mode=ro` still participates in WAL shared memory: it maps and
    updates `<db>-shm`. That was measured here, not assumed - with the desk stopped, a single
    read-only query moved the -shm mtime. Over the bridge mount this research runs on, that is a
    second process touching the writer's locking state from a different operating system, and
    prod.log carries three SQLite failures in one day ("database disk image is malformed", "file
    is not a database", "disk I/O error") on a file that passes quick_check. Read the snapshot.
    """
    snap = os.path.join(root, "data", "snapshots", "prod-latest.sqlite")
    return snap if os.path.exists(snap) else os.path.join(root, "data", "prod.sqlite")

# ---- settlement_specs(), transcribed from src/universe.cpp -------------------------------------
# product "" means nobody has read that series' rules; the gate treats that as unverified.
SPECS = {
    "KXHIGHAUS":  ("weather_cli", "CLIAUS"),
    "KXHIGHCHI":  ("weather_cli", "CLIMDW"),
    "KXHIGHNY":   ("weather_cli", "CLINYC"),
    "KXHIGHMIA":  ("weather_cli", "CLIMIA"),
    "KXHIGHPHIL": ("weather_cli", "CLIPHL"),
    "KXHIGHLAX":  ("weather_cli", "CLILAX"),
    "KXLOWTCHI":  ("weather_cli", "CLIMDW"),
    "KXLOWTMIN":  ("weather_cli", "CLIMSP"),
    "KXHIGHHOU":  ("weather_cli", ""),
    "KXDENHIGH":  ("weather_cli", ""),
    "KXBRASILEIROGAME": ("soccer_90", "bra.1"),
}

# ---- leagues, transcribed from the two sports strategies ---------------------------------------
INPLAY_LEAGUES = {
    "KXEPLGAME","KXLALIGAGAME","KXSERIEAGAME","KXBUNDESLIGAGAME","KXLIGUE1GAME","KXUCLGAME",
    "KXUELGAME","KXMLSGAME","KXEFLCHAMPIONSHIPGAME","KXEREDIVISIEGAME","KXLIGAPORTUGALGAME",
    "KXBRASILEIROGAME","KXLIGAMXGAME","KXSCOTTISHPREMGAME","KXBELGIANPLGAME","KXSUPERLIGGAME",
    "KXARGPREMDIVGAME","KXBUNDESLIGA2GAME","KXLALIGA2GAME","KXFACUPGAME",
}
LIVE_LEAGUES = {
    "KXNFLGAME","KXNCAAFGAME","KXNBAGAME","KXWNBAGAME","KXNCAAMBGAME","KXNCAAWBGAME",
    "KXMLBGAME","KXNHLGAME",
}
CRYPTO_SYMBOLS = {"KXBTC": "BTC", "KXETH": "ETH", "KXBTCD": "BTC", "KXETHD": "ETH"}


def load_jsonc(path):
    s = open(path).read()
    out, i, n, instr, esc = [], 0, len(s), False, False
    while i < n:
        ch = s[i]
        if instr:
            out.append(ch)
            if esc: esc = False
            elif ch == "\\": esc = True
            elif ch == '"': instr = False
            i += 1; continue
        if ch == '"': instr = True; out.append(ch); i += 1; continue
        if ch == "/" and i + 1 < n and s[i+1] == "/":
            while i < n and s[i] != "\n": i += 1
            continue
        if ch == "/" and i + 1 < n and s[i+1] == "*":
            i += 2
            while i + 1 < n and not (s[i] == "*" and s[i+1] == "/"): i += 1
            i += 2; continue
        out.append(ch); i += 1
    return json.loads(re.sub(r",(\s*[}\]])", r"\1", "".join(out)))


# ---- the gate, transcribed from src/probability.cpp::settlement_verified_for -------------------
def weather_source_supported(rules):
    r = (rules or "").lower()
    return "weather company" not in r and ("national weather service" in r or "nws" in r)


def settlement_verified(model, m):
    series, rules = m["series_ticker"], (m["rules_primary"] or "")
    if model == "weather_nws":
        spec = SPECS.get(series)
        return weather_source_supported(rules) and bool(spec and spec[1])
    if model == "sports_inplay":
        spec = SPECS.get(series)
        return bool(spec and spec[0] == "soccer_90")
    if model == "sports_totals": return True
    if model == "sports_live":   return True
    if model == "crypto_lognormal":
        r = rules.lower()
        return "cf benchmarks" in r or "real-time index" in r
    if model == "manual_fair":   return True
    return False   # xvenue and everything unmapped


# ---- covers(), transcribed from each strategy, in registry order -------------------------------
def build_covers(cfg, params, pairs, totals_tickers, commodity_series):
    def manual_fair(m, cat):  return m["ticker"] in MANUAL_FAIRS
    def xvenue(m, cat):       return m["ticker"] in pairs
    def weather(m, cat):
        return cat == "Climate and Weather" and m["series_ticker"] in WX_STATIONS \
               and m["strike_type"] in ("greater", "less", "between")
    def crypto(m, cat):
        return cat == "Crypto" and m["series_ticker"] in CRYPTO_SYMBOLS \
               and m["strike_type"] in ("greater", "less", "between") \
               and (m["frequency"] or "") in ("hourly", "daily", "custom")
    def totals(m, cat):       return m["ticker"] in totals_tickers
    def inplay(m, cat):
        if cat != "Sports" or m["series_ticker"] not in INPLAY_LEAGUES: return False
        t = m["ticker"] or ""
        return (m["yes_sub_title"] == "Tie" or " wins" in (m["title"] or "")
                or t.endswith("-TIE") or " wins the " in (m["rules_primary"] or ""))
    def live(m, cat):
        return cat == "Sports" and m["series_ticker"] in LIVE_LEAGUES and m["strike_type"] == "structured"
    def commodity_terminal(m, cat):  return m["series_ticker"] in commodity_series
    return [("manual_fair", manual_fair), ("xvenue", xvenue), ("weather_nws", weather),
            ("crypto_lognormal", crypto), ("sports_totals", totals), ("sports_inplay", inplay),
            ("sports_live", live), ("commodity_terminal", commodity_terminal)]


MANUAL_FAIRS = set()
WX_STATIONS = set()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=default_db(ROOT))
    ap.add_argument("--config", default=os.path.join(ROOT, "config", "desk.json"))
    ap.add_argument("--out", default=os.path.join(ROOT, "reports", "settlement_gate_census.md"))
    a = ap.parse_args()

    cfg = load_jsonc(a.config)
    params = cfg.get("params", {})
    modes = cfg.get("strategies", {})
    modes.setdefault("xvenue", "manual")
    watch = set(cfg.get("watch_series", []))

    global WX_STATIONS, MANUAL_FAIRS
    WX_STATIONS = {s for s, (k, _) in SPECS.items() if k == "weather_cli"}
    WX_STATIONS |= set((params.get("weather_nws", {}).get("stations", {}) or {}).keys())
    commodity_series = set()
    for k, v in params.get("commodity", {}).items():
        if isinstance(v, dict):
            for s in v.get("series", []) or []: commodity_series.add(s)
    for s in watch:
        if s.endswith("15M"): commodity_series.add(s)
    totals_tickers = set((params.get("sports_totals", {}).get("markets", {}) or {}).keys())

    con = sqlite3.connect("file:%s?mode=ro" % a.db, uri=True)
    con.row_factory = sqlite3.Row
    cats = {r["ticker"]: (r["category"], r["frequency"]) for r in con.execute("SELECT ticker,category,frequency FROM series")}
    # The desk's own last write, NOT wall clock. Measuring a feed's freshness against wall clock
    # when the writer has stopped reports the writer's outage as the feed's staleness: it is how
    # "xvenue has no fresh quote" was nearly filed against Polymarket when the desk had been down
    # twelve minutes and 396 of its pairs had been fine at the last cycle.
    cycle = con.execute("SELECT MAX(updated_ts) FROM markets").fetchone()[0] or 0
    pairs, pair_valid = set(), set()
    try:
        for r in con.execute(
            "SELECT p.kalshi_ticker t, q.bid b, q.ask k, q.quote_ts qt FROM poly_pairs p"
            " JOIN poly_markets q ON q.slug=p.slug WHERE q.closed=0"):
            pairs.add(r["t"])
            two = r["b"] > 0 and r["k"] > 0 and r["k"] > r["b"] and r["k"] < 1
            if two and r["qt"] > 0 and cycle - r["qt"] <= 90: pair_valid.add(r["t"])
    except sqlite3.Error:
        pass
    mfs = set()
    for r in con.execute("SELECT DISTINCT ticker FROM model_fairs WHERE strategy='manual_fair'"):
        mfs.add(r[0])
    MANUAL_FAIRS = mfs

    rows = [dict(r) for r in con.execute(
        "SELECT ticker,event_ticker,series_ticker,title,yes_sub_title,strike_type,status,"
        "rules_primary,yes_bid,yes_ask,volume,open_interest,close_time FROM markets WHERE status='active'")]
    for m in rows:
        cat, freq = cats.get(m["series_ticker"], ("", ""))
        m["category"], m["frequency"] = cat, freq

    chain = build_covers(cfg, params, pairs, totals_tickers, commodity_series)
    ALL_MODELS = ["manual_fair", "xvenue", "weather_nws", "crypto_lognormal",
                  "sports_totals", "sports_inplay", "sports_live", "commodity_terminal"]

    stat = defaultdict(lambda: defaultdict(int))
    stolen = defaultdict(lambda: defaultdict(int))   # verifiable model -> who took it
    per_series = defaultdict(lambda: defaultdict(int))
    liq = defaultdict(list)                          # (model, series) -> per-market liquidity
    unwatched_verifiable = defaultdict(int)
    # A market with no two-sided book is not researchable however well it settles: there is no
    # price to be right or wrong about. Counted separately from the gate on purpose.
    live_book = defaultdict(int)

    for m in rows:
        cat = m["category"]
        owner = None
        for name, fn in chain:
            if fn(m, cat): owner = name; break
        verifiable = [n for n in ALL_MODELS if n != "manual_fair"
                      and any(nm == n for nm, fn in chain if fn(m, cat)) and settlement_verified(n, m)]
        # a model can only verify a contract it covers; manual_fair covers only what you priced
        watched = m["series_ticker"] in watch
        key = owner or "(uncovered)"
        stat[key]["covered"] += 1
        if watched: stat[key]["watched"] += 1
        ok = settlement_verified(owner, m) if owner else False
        if ok:
            stat[key]["verified"] += 1
            if watched:
                stat[key]["watched_verified"] += 1
                per_series[key][m["series_ticker"]] += 1
                b, k = m["yes_bid"] or 0, m["yes_ask"] or 0
                two = b > 0 and k > 0 and k > b
                sp = (k - b) if two else None
                liq[(key, m["series_ticker"])].append((m["volume"] or 0, m["open_interest"] or 0, sp))
                if two:
                    live_book[(key, m["series_ticker"])] += 1
                    stat[key]["verified_live_book"] += 1
        for v in verifiable:
            if v != owner:
                stolen[v][key] += 1
                if key == "xvenue":
                    stolen[v]["xvenue (valid fair)" if m["ticker"] in pair_valid
                              else "xvenue (NO usable fair)"] += 1
                if not watched: unwatched_verifiable[v] += 1

    def pct(a, b): return "0.0%%" if not b else "%.1f%%" % (100.0 * a / b)

    L = []
    L.append("# Settlement-gate census: which models can pass, and on how many markets\n")
    L.append("Generated %s from `%s`. Read-only.\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), os.path.basename(a.db)))
    age = int(time.time()) - cycle
    L.append("Desk's last write: %s (**%ds ago**). Every freshness test below is measured against "
             "that cycle, not wall clock.\n" % (time.strftime("%H:%M:%S", time.localtime(cycle)), age))
    if age > 120:
        L.append("> The desk has not written for %d minutes. Market status and books are as of its "
                 "last cycle.\n" % (age // 60))
    L.append("Active markets: **%d**, of which **%d** are in `watch_series`.\n" % (
        len(rows), sum(1 for m in rows if m["series_ticker"] in watch)))
    L.append("\n## Per model: covered, and how much of that clears the settlement gate\n")
    L.append("| model | mode | covers | watched & VERIFIED | pass rate | of those, two-sided book |")
    L.append("|---|---|---|---|---|---|")
    for k in sorted(stat, key=lambda x: -stat[x]["watched"]):
        v = stat[k]
        L.append("| `%s` | %s | %d | **%d** | %s | **%d** |" % (
            k, modes.get(k, "off" if k != "(uncovered)" else "-"),
            v["watched"], v["watched_verified"], pct(v["watched_verified"], v["watched"]),
            v["verified_live_book"]))
    L.append("\n## Markets a passing model could verify, taken first by a model that cannot\n")
    if not stolen:
        L.append("None: registry precedence costs the desk nothing right now.\n")
    else:
        L.append("| model that would verify | taken by | markets |")
        L.append("|---|---|---|")
        for v in sorted(stolen, key=lambda x: -sum(stolen[x].values())):
            for who, n in sorted(stolen[v].items(), key=lambda kv: -kv[1]):
                L.append("| `%s` | `%s` | %d |" % (v, who, n))
    L.append("\n## The researchable universe, by series\n")
    if not per_series:
        L.append("**Empty. No watched active market clears the settlement gate under the model that prices it.**\n")
    else:
        L.append("| model | series | verified | two-sided book | median volume | median OI | median spread |")
        L.append("|---|---|---|---|---|---|---|")
        med = lambda xs: ("%.3f" % xs[len(xs)//2]) if xs else "-"
        for k in sorted(per_series, key=lambda x: -sum(per_series[x].values())):
            for ser, n in sorted(per_series[k].items(), key=lambda kv: -kv[1]):
                cell = liq[(k, ser)]
                vols = sorted(v for v, _, _ in cell)
                ois = sorted(o for _, o, _ in cell)
                sps = sorted(x for _, _, x in cell if x is not None)
                L.append("| `%s` | `%s` | %d | **%d** | %s | %s | %s |" % (
                    k, ser, n, live_book[(k, ser)], med(vols), med(ois), med(sps)))
    # ---- why each zero is a zero. A census that reports 0 without the mechanism is not actionable.
    now = int(time.time())
    L.append("\n## Why each zero is a zero\n")

    wx = [m for m in rows if m["category"] == "Climate and Weather" and m["series_ticker"] in WX_STATIONS]
    twc = sum(1 for m in wx if "weather company" in (m["rules_primary"] or "").lower())
    nws = sum(1 for m in wx if weather_source_supported(m["rules_primary"]))
    L.append("**`weather_nws`** - the gate rejects any contract whose rules name The Weather Company. "
             "Of %d active weather markets, **%d name TWC and %d name the NWS**. Kalshi moved the "
             "settlement authority to TWC on 2026-08-27 (docs/settlement.md), so the predicate now "
             "excludes the entire live universe. `docs/settlement.md` records TWC agreeing with the NWS "
             "CLI maximum 6 times on one date - evidence, not the standing check the gate would need.\n"
             % (len(wx), twc, nws))

    soccer_live = sorted({m["series_ticker"] for m in rows
                          if m["series_ticker"] in INPLAY_LEAGUES})
    mapped = sorted(s for s in soccer_live if (SPECS.get(s) or ("", ""))[0] == "soccer_90")
    L.append("**`sports_inplay`** - the gate requires a series mapped `soccer_90`. Listed soccer series: "
             "%s. Mapped: %s. The one mapped series (`KXBRASILEIROGAME`) lists no active market, so no "
             "covered contract can pass. Unlock is reading %d series' rules and extending "
             "`settlement_specs()` - no model change.\n"
             % (", ".join("`%s`" % x for x in soccer_live) or "none",
                ", ".join("`%s`" % x for x in mapped) or "**none**", len(soccer_live)))

    L.append("**`xvenue`** - returns `false` from the gate by design (payoff parity with Polymarket was read "
             "for some leagues, never verified), and it sits *second* in the registry, ahead of every model "
             "that can pass. At the desk's last cycle it held **%d** pairs, of which **%d produced a usable "
             "fair** (two-sided and <=90s, what `XVenue::fair` requires) and **%d did not**. Either way the "
             "gate blocks it, so its whole book can only ever be Research - and it takes **%d** game markets "
             "that `sports_live` verifies unconditionally, **%d of them while producing no usable fair of its "
             "own**.\n" % (len(pairs), len(pair_valid), len(pairs) - len(pair_valid),
                           stolen["sports_live"].get("xvenue", 0),
                           stolen["sports_live"].get("xvenue (NO usable fair)", 0)))

    L.append("\n## Where research effort belongs\n")
    L.append("Ranked by verifiable markets that actually have a price to be wrong about.\n")
    L.append("| lane | verifiable markets | live books | verdict |")
    L.append("|---|---|---|---|")
    sl = stat["sports_live"]["watched_verified"] + stolen["sports_live"].get("xvenue", 0)
    L.append("| `sports_live` | %d | %d + the blocked %d | **research this first** - tightest books on the desk |"
             % (sl, stat["sports_live"]["verified_live_book"], stolen["sports_live"].get("xvenue", 0)))
    L.append("| `crypto_lognormal` | %d | %d | second - passes everywhere, priced almost nowhere |"
             % (stat["crypto_lognormal"]["watched_verified"], stat["crypto_lognormal"]["verified_live_book"]))
    L.append("| `sports_totals` | passes unconditionally | **0** | no universe: `params.sports_totals.markets` is empty |")
    L.append("| `manual_fair` | passes by definition | your own | not a research lane |")
    L.append("| `xvenue`, `weather_nws`, `sports_inplay` | **0** | - | no effort until the gate can be passed |")
    L.append("\nTwo unlocks, both data rather than model:\n")
    L.append("1. Registry order. `xvenue` is 2nd in `strategies/registry.cpp`, ahead of every model that can "
             "pass; moving it below `sports_live` moves **%d** markets from unverified to verified without "
             "touching a model.\n" % stolen["sports_live"].get("xvenue", 0))
    L.append("2. Soccer specs. Reading the 6 listed soccer series' rules and extending `settlement_specs()` "
             "with `soccer_90` would let `sports_inplay` reach its **%d** covered markets.\n"
             % stat["sports_inplay"]["watched"])
    L.append("Weather is a measurement project before it is a lane: the standing TWC-vs-CLI check that "
             "`docs/settlement.md` asks for has to exist before the predicate can honestly accept TWC.\n")
    L.append("")
    open(a.out, "w").write("\n".join(L))
    print("\n".join(L))
    print("\nwrote %s" % a.out)


if __name__ == "__main__":
    main()
