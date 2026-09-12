#!/usr/bin/env bash
# Apply the mode and settings changes WITHOUT the HTTP API, with the desk stopped.
#
# Why this exists: every mutating handler takes engine_.mutex(), and Engine::cycle() holds that
# same mutex for the whole cycle - including the blocking HTTP calls it makes to api.weather.gov,
# ESPN and Polymarket. On 2026-09-12 a POST to /api/strategy/.../mode timed out at 90 seconds.
# The settings the desk reads at startup live in kv, so with the desk stopped they can simply be
# written there, which is what this does.
#
# RUN THIS IN YOUR OWN TERMINAL on the Mac, not through any tooling: it writes to prod.sqlite, and
# that database must only ever be opened natively, never across a mounted filesystem.
set -uo pipefail
cd "$(dirname "$0")/.."

if curl -sf --max-time 3 http://localhost:8787/api/state >/dev/null 2>&1; then
  echo "The desk is still answering on 8787. Stop it first:  tradeit down"
  exit 1
fi
if [ ! -f data/prod.sqlite ]; then echo "no data/prod.sqlite here"; exit 1; fi

python3 - <<'PY'
import json, sqlite3, sys

db = sqlite3.connect("data/prod.sqlite")
db.execute("PRAGMA busy_timeout=5000")

def kv_get(k):
    r = db.execute("SELECT value FROM kv WHERE key=?", (k,)).fetchone()
    return r[0] if r else None

def kv_set(k, v):
    db.execute("INSERT INTO kv(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", (k, v))

# 1. The two strategies that are armed. min_edge is the floor the systematic engine SENDS on,
#    not only what the board displays, so these come off auto before the floor drops.
for s in ("sports_live", "econ_nowcast"):
    before = kv_get("mode:" + s)
    kv_set("mode:" + s, "manual")
    print("  mode:%-14s %s -> manual" % (s, before))

# 2. The systematic settings. Only the three keys change; everything else is left exactly as it is,
#    because this object also carries the day's state and the halt flags.
raw = kv_get("systematic:v2")
if raw is None:
    print("  systematic:v2 missing - start the desk once, then re-run", file=sys.stderr)
else:
    d = json.loads(raw)
    s = d.setdefault("settings", {})
    for k, v in (("min_edge", 0.0025), ("manual_min_edge", 0.0025), ("entry_style", "maker")):
        print("  settings.%-16s %s -> %s" % (k, s.get(k), v))
        s[k] = v
    kv_set("systematic:v2", json.dumps(d))

db.commit()
db.close()
print("\nwritten. start the desk:  tradeit up")
PY
