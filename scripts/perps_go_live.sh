#!/usr/bin/env bash
# Take the perps rule live, sized as a measurement.
#
# No directional model survived the research (docs/perps-models.md): 341 signal/horizon/instrument
# combinations across six model families, 17 expected to survive by chance, 7 survived and all of
# them marginal. This does not go live because
# an edge was found. It goes live because the fill rate on a post-only entry is the input to every
# future decision here and paper provably cannot measure it -- three days of paper produced 38
# entries and zero fills, because the paper model requires the touch to trade THROUGH the posted
# price while a real maker fill happens AT it.
#
# config/desk.json only SEEDS a fresh database. Once data/prod.sqlite holds a perps_algo:v1 row
# that row wins for every key it contains, so settings are pushed here. Settings FIRST, mode
# second: the rule is already `running`, so flipping the mode first would go live on whatever the
# database still holds (as of 2026-09-11 that was KXBTCPERP and KXETHPERP at one contract).
set -euo pipefail
H=${DESK:-http://127.0.0.1:8787}
NOTIONAL=${NOTIONAL:-5}
# Every mutating /api/ route needs the X-Desk header: the server refuses a POST without it, so a
# cross-origin page cannot drive this desk with a plain form. Value is not checked, only presence.
post() { curl -sS -X POST "$1" -H 'content-type: application/json' -H 'X-Desk: 1' \
              -w '\n%{http_code}' "${@:2}" | python3 -c '
import sys, json
body = sys.stdin.read().rsplit("\n", 1)
code = body[1].strip() if len(body) > 1 else "?"
if code != "200":
    try: msg = json.loads(body[0]).get("error", body[0])
    except Exception: msg = body[0].strip() or "(no body)"
    print("HTTP %s: %s" % (code, msg), file=sys.stderr); sys.exit(1)
print(body[0])'; }
show() { python3 -c '
import sys, json
raw = sys.stdin.read()
try: o = json.loads(raw)
except Exception:
    print("not JSON: " + raw[:300], file=sys.stderr); sys.exit(1)
for k in sys.argv[1].split("."): o = o.get(k, {}) if isinstance(o, dict) else {}
print(json.dumps(o, indent=2))' "$1"; }

# A "400 unknown perps setting: notional_dollars" below means the running binary predates the
# dollar-sizing change: tradeit down, rebuild, tradeit up. tradeit does not build anything itself.
if ! curl -fsS -o /dev/null --max-time 3 "$H/api/perps"; then
  echo "the desk is not answering on $H."
  echo "start it first:  ./scripts/tradeit up      (or: ./build/kalshi-desk serve)"
  exit 1
fi

echo "== before =="
curl -fsS "$H/api/perps" | show algo.settings

echo
echo "== 1. settings: 8 instruments, \$${NOTIONAL} an entry, entry 22 bps =="
settings_out=$(post "$H/api/perps/algo" -d "{
  \"settings\": {
    \"markets\": [\"KXXRPPERP\",\"KXADAPERP\",\"KXVVVPERP\",\"KXAAVEPERP\",
                  \"KXBCHPERP\",\"KXWLDPERP\",\"KXNEARPERP\",\"KXBNBPERP\"],
    \"notional_dollars\": ${NOTIONAL},
    \"entry_bps\": 22,
    \"exit_bps\": 5,
    \"stop_bps\": 60,
    \"max_hold_minutes\": 120,
    \"daily_loss_stop\": 1.5,
    \"entry_ttl_seconds\": 90,
    \"cooldown_minutes\": 30
  }}") || exit 1
echo "$settings_out" | show algo.settings

echo
echo "== 2. sync the margined instance =="
# A freshly started desk has not read the perp markets or the account yet, and the rails refuse an
# order against an account snapshot older than 180s. Force one rather than letting the checks below
# fail on a cold start and read as "these instruments do not exist".
sync_out=$(post "$H/api/perps/sync" -d '{}') || exit 1
for i in $(seq 1 12); do
  read -r pub acct mkts <<<"$(curl -fsS "$H/api/perps" | python3 -c '
import sys,json,time
d=json.load(sys.stdin); now=time.time()
print(int(now-(d.get("public_ts") or 0)), int(now-(d.get("account_ts") or 0)), len(d.get("markets") or []))')"
  [ "$mkts" -gt 0 ] && [ "$acct" -lt 180 ] && break
  printf "  waiting for the first sync (%s markets, account %ss old)\r" "$mkts" "$acct"
  sleep 3
done
echo "  public data ${pub}s old, account ${acct}s old, ${mkts} markets"
if [ "$mkts" -eq 0 ]; then
  echo "  the margined instance returned no markets. Check the perps tab and 'last_error' in"
  echo "  $H/api/perps before going further; nothing below will be meaningful."
  exit 1
fi

echo
echo "== 3. every instrument must buy at least one contract and stay inside the cap =="
curl -fsS "$H/api/perps" | python3 -c '
import sys,json,time
d=json.load(sys.stdin); a=d.get("algo",{}); s=a.get("settings",{}); rails=d.get("rails",{})
mk={m["ticker"]:m for m in d.get("markets",[])}
nd=s.get("notional_dollars",0); cap=rails.get("max_order_notional",0)
ok=True
def chk(c,label):
    global ok
    print(("  ok   " if c else "  FAIL ")+label); ok = ok and c
chk(nd>0, "sized in dollars: $%s an entry"%nd)
chk(s.get("entry_bps")==22, "entry_bps = 22, the floor the code enforces  got %s"%s.get("entry_bps"))
chk(s.get("daily_loss_stop",99)<=1.5, "daily loss stop $%s"%s.get("daily_loss_stop"))
for t in s.get("markets",[]):
    m=mk.get(t)
    if not m: print("  FAIL %s: no quote from the exchange (delisted, or halted right now)"%t); ok=False; continue
    mark=m.get("mark") or 0; bid=m.get("bid") or 0; ask=m.get("ask") or 0
    n=int(nd/mark) if mark>0 else 0
    sp=(ask-bid)/((ask+bid)/2)*1e4 if bid>0 and ask>0 else 0
    chk(n>=1 and n*mark<=cap, "%-14s %3d ct = $%4.2f  spread %5.1f bps  (cap $%.0f)"%(t,n,n*mark,sp,cap))
chk(d.get("armed"), "armed (Kalshi key loaded); live is refused without it")
chk(d.get("enabled"), "perps enabled in config")
chk(not a.get("halted"), "not halted  %s"%(a.get("halt_reason") or ""))
chk(a.get("running"), "rule is running")
chk(not d.get("last_error"), "no venue error  %s"%(d.get("last_error") or ""))
age=time.time()-(d.get("account_ts") or 0)
chk(age<180, "account snapshot %.0fs old (rails refuse over 180s)"%age)
eq=(d.get("balance") or {}).get("equity") or 0
tot=rails.get("max_total_notional") or 0
print("\n  peak leverage %.2fx on $%.2f | round trip $%.4f | 60bps stop $%.3f | daily stop $%s"%(
      tot/eq if eq else 0, eq, nd*17/1e4, nd*s.get("stop_bps",60)/1e4, s.get("daily_loss_stop")))
sys.exit(0 if ok else 1)'

cat <<'NOTE'

  Read this before answering.
  No model predicted returns: 341 combinations across basis fade, basis follow, index lead-lag,
  perp momentum, BTC leading the alts, and the touch basis net of the spread; 17 would survive
  by chance; 7 did, all marginal. Funding is zero on 18 of 20. You are not buying an edge, you are
  buying the fill rate, at about a cent a round trip. Check it with:

      python3 scripts/perp_fills.py data/prod.sqlite live

  Stop when the fill rate is known, or when it has cost more than the answer is worth.
NOTE
read -rp "== 4. flip to LIVE money? [type LIVE] " a
[ "$a" = LIVE ] || { echo "left in paper."; exit 0; }
live_out=$(post "$H/api/perps/algo" -d '{"paper": false}') || exit 1
echo "$live_out" | show algo
echo
echo "stop with:"
echo "  curl -X POST $H/api/perps/algo -H 'content-type: application/json' -d '{\"running\": false}'"
