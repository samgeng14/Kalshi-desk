#!/usr/bin/env bash
# Apply the weather volume-mode settings, in the order that keeps them safe.
#
# Run it any time, including straight after `tradeit up`: it waits for the port instead of
# failing. `tradeit up` prints its banner before the HTTP server binds, which is why a curl
# pasted on the next line gets "Couldn't connect" and looks like the desk is broken.
#
# Order matters. The two AUTO strategies come off auto FIRST, because the same min_edge that
# governs what the board displays is also the floor the systematic engine sends on
# (systematic.cpp:404). Dropping it to a quarter-cent while sports_live and econ_nowcast are
# armed would lower the bar for real orders, not just for rows on a screen.
set -uo pipefail
HOST=${HOST:-localhost:8787}
H=(-H 'X-Desk: 1' -H 'content-type: application/json')
# Engine::cycle() holds engine_.mutex() for the whole cycle and 17 HTTP handlers need that same
# lock, so a request arriving mid-cycle waits for it. Cycles were 17-25s on 2026-09-12. Without a
# timeout this script just sits there looking frozen, which is exactly how the desk looked.
T=(--max-time 90 --connect-timeout 5)

printf 'Each request can wait a full engine cycle for the lock (17-25s). Be patient.\n'
printf 'waiting for %s ' "$HOST"
for _ in $(seq 1 60); do
  if curl -sf "http://$HOST/api/state" >/dev/null 2>&1; then printf ' up\n'; break; fi
  printf '.'; sleep 1
done
curl -sf "http://$HOST/api/state" >/dev/null 2>&1 || { printf '\nnever came up; is the desk running?\n'; exit 1; }

for s in sports_live econ_nowcast; do
  printf '%-14s -> manual ' "$s"
  code=$(curl -sS "${T[@]}" -o /tmp/wx_mode.out -w '%{http_code}' -X POST \
         "http://$HOST/api/strategy/$s/mode" "${H[@]}" -d '{"mode":"manual"}' 2>/dev/null)
  printf ' HTTP %s\n' "${code:-timeout}"
  [ "$code" = "200" ] || head -c 300 /tmp/wx_mode.out
done

# Settings live nested under "settings"; the top level of this endpoint accepts only
# settings / running / mode / clear_halt / release and rejects anything else by name.
printf 'settings         '
code=$(curl -sS "${T[@]}" -o /tmp/wx_set.out -w '%{http_code}' -X POST "http://$HOST/api/systematic" \
       "${H[@]}" -d '{"settings":{"min_edge":0.0025,"manual_min_edge":0.0025,"entry_style":"maker"}}' 2>/dev/null)
printf '  HTTP %s\n' "${code:-timeout}"
[ "$code" = "200" ] || head -c 400 /tmp/wx_set.out

printf '\n--- what actually took ---\n'
curl -sS "${T[@]}" "http://$HOST/api/systematic" | python3 -c '
import json,sys
d=json.load(sys.stdin); s=d.get("settings",d)
for k in ("min_edge","manual_min_edge","entry_style","min_win_probability","uncertainty_buffer"):
    print("  %-20s %s" % (k, s.get(k)))' 2>/dev/null || echo '  (could not read settings back)'
printf -- '--- anything still on auto ---\n'
curl -sS "${T[@]}" "http://$HOST/api/state" | python3 -c '
import json,sys
d=json.load(sys.stdin)
def walk(o):
    if isinstance(o,dict):
        if o.get("mode")=="auto" and o.get("name"): yield o["name"]
        for v in o.values():
            yield from walk(v)
    elif isinstance(o,list):
        for v in o: yield from walk(v)
names=sorted(set(walk(d)))
print("\n".join("  STILL AUTO: "+n for n in names) if names else "  none")' 2>/dev/null || echo "  (could not read state)"
