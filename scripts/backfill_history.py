#!/usr/bin/env python3
"""Backfill final scores + per-period linescores from ESPN into data/history.sqlite.

Separate file from the live desk DB on purpose: this must never contend with
prod.sqlite while the desk is trading.

Resumable: every (league, date) fetched is logged, so re-running skips work.
"""
import json, os, sqlite3, sys, time, urllib.request, urllib.error
from datetime import date, timedelta

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The bridge mount cannot do SQLite locking, so build in local scratch and
# publish a copy into the repo when done. KD_HIST_DB overrides.
DB = os.environ.get("KD_HIST_DB") or os.path.join(REPO, "data", "history.sqlite")
PUBLISH_TO = os.environ.get("KD_HIST_PUBLISH") or ""

# league -> (espn path, regulation periods, months in season, extra query)
LEAGUES = {
    "nfl":   ("football/nfl",                      4, {9,10,11,12,1,2},          ""),
    "ncaaf": ("football/college-football",         4, {8,9,10,11,12,1},          "&groups=80"),
    "nba":   ("basketball/nba",                    4, {10,11,12,1,2,3,4,5,6},    ""),
    "ncaab": ("basketball/mens-college-basketball",2, {11,12,1,2,3,4},           "&groups=50"),
    "nhl":   ("hockey/nhl",                        3, {10,11,12,1,2,3,4,5,6},    ""),
    "mlb":   ("baseball/mlb",                      9, {3,4,5,6,7,8,9,10,11},     ""),
    "epl":   ("soccer/eng.1",                      2, {8,9,10,11,12,1,2,3,4,5},  ""),
    "mls":   ("soccer/usa.1",                      2, {2,3,4,5,6,7,8,9,10,11,12},""),
}

SCHEMA = """
create table if not exists hist_games(
  espn_id text primary key,
  league text not null,
  gdate text not null,
  start_ts text,
  home text, away text,
  home_abbr text, away_abbr text,
  home_score int, away_score int,
  periods int,
  reg_home int, reg_away int,
  ls_home text, ls_away text,
  detail text,
  neutral int,
  completed int
);
create index if not exists hist_games_league_date on hist_games(league, gdate);
create table if not exists hist_fetch_log(
  league text not null, gdate text not null, n int, fetched_ts text,
  primary key(league, gdate)
);
"""

# ESPN 403s browser-like User-Agents on this endpoint; the urllib default passes.
UA = {}


def fetch(url, tries=4):
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers=UA)
            with urllib.request.urlopen(req, timeout=30) as r:
                return json.loads(r.read().decode())
        except Exception as e:
            if i == tries - 1:
                raise
            time.sleep(1.5 * (i + 1))
    return None


def linescores(c):
    out = []
    for x in c.get("linescores") or []:
        v = x.get("value")
        if v is None:
            v = x.get("displayValue")
        try:
            out.append(float(v))
        except (TypeError, ValueError):
            out.append(0.0)
    return out


def parse_event(league, reg, ev):
    comps = ev.get("competitions") or []
    if not comps:
        return None
    comp = comps[0]
    st = comp.get("status") or {}
    typ = st.get("type") or {}
    completed = 1 if typ.get("completed") else 0
    home = away = None
    for c in comp.get("competitors") or []:
        if c.get("homeAway") == "home":
            home = c
        elif c.get("homeAway") == "away":
            away = c
    if home is None or away is None:
        return None

    def score(c):
        try:
            return int(float(c.get("score")))
        except (TypeError, ValueError):
            return None

    lh, la = linescores(home), linescores(away)
    periods = st.get("period")
    try:
        periods = int(periods)
    except (TypeError, ValueError):
        periods = None
    # regulation subtotal: only trustworthy when we actually have per-period detail
    reg_h = int(sum(lh[:reg])) if len(lh) >= reg else None
    reg_a = int(sum(la[:reg])) if len(la) >= reg else None
    return (
        str(ev.get("id")), league, "",  # gdate filled by caller
        ev.get("date"),
        (home.get("team") or {}).get("displayName"),
        (away.get("team") or {}).get("displayName"),
        (home.get("team") or {}).get("abbreviation"),
        (away.get("team") or {}).get("abbreviation"),
        score(home), score(away),
        periods, reg_h, reg_a,
        json.dumps(lh), json.dumps(la),
        typ.get("detail"),
        1 if comp.get("neutralSite") else 0,
        completed,
    )


def daterange(a, b):
    d = a
    while d <= b:
        yield d
        d += timedelta(days=1)


def main():
    start = date.fromisoformat(sys.argv[1]) if len(sys.argv) > 1 else date(2023, 8, 1)
    end = date.fromisoformat(sys.argv[2]) if len(sys.argv) > 2 else date.today() - timedelta(days=1)
    only = sys.argv[3].split(",") if len(sys.argv) > 3 else list(LEAGUES)

    os.makedirs(os.path.dirname(DB), exist_ok=True)
    db = sqlite3.connect(DB, timeout=30)
    db.executescript(SCHEMA)
    db.commit()

    done = {(r[0], r[1]) for r in db.execute("select league, gdate from hist_fetch_log")}
    todo = []
    for lg in only:
        path, reg, months, extra = LEAGUES[lg]
        for d in daterange(start, end):
            if d.month not in months:
                continue
            ds = d.strftime("%Y%m%d")
            if (lg, ds) in done:
                continue
            todo.append((lg, ds))

    print(f"[backfill] {len(todo)} (league,date) pairs to fetch", flush=True)
    n_games = 0
    for i, (lg, ds) in enumerate(todo):
        path, reg, months, extra = LEAGUES[lg]
        url = f"https://site.api.espn.com/apis/site/v2/sports/{path}/scoreboard?dates={ds}&limit=500{extra}"
        try:
            j = fetch(url)
        except Exception as e:
            print(f"[backfill] FAIL {lg} {ds}: {e}", flush=True)
            continue
        rows = []
        for ev in j.get("events") or []:
            p = parse_event(lg, reg, ev)
            if p and p[-1] == 1:  # completed only
                rows.append((p[0], p[1], ds) + p[3:])
        if rows:
            db.executemany(
                "insert or replace into hist_games values (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", rows
            )
            n_games += len(rows)
        db.execute(
            "insert or replace into hist_fetch_log values (?,?,?,datetime('now'))",
            (lg, ds, len(rows)),
        )
        db.commit()
        if i % 100 == 0:
            print(f"[backfill] {i}/{len(todo)} pairs, {n_games} games", flush=True)
        time.sleep(0.12)

    print(f"[backfill] done: {n_games} games written", flush=True)
    if PUBLISH_TO:
        db.execute("pragma wal_checkpoint(TRUNCATE)")
        db.close()
        import shutil
        os.makedirs(os.path.dirname(PUBLISH_TO), exist_ok=True)
        shutil.copyfile(DB, PUBLISH_TO)
        print(f"[backfill] published -> {PUBLISH_TO}", flush=True)
        db = sqlite3.connect(DB, timeout=30)
    for r in db.execute("select league, count(*) from hist_games group by 1 order by 1"):
        print("   ", r[0], r[1], flush=True)


if __name__ == "__main__":
    main()
