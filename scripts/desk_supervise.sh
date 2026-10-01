#!/usr/bin/env bash
# Keeps the desk running. Started by `tradeit up`; stopped by `tradeit down`.
#
#   nohup scripts/desk_supervise.sh >/dev/null 2>&1 &
#
# Why this exists: between 2026-09-07 and 2026-09-17 the desk died at least eight times - seven
# uncaught SQLite errors and one silent kill - and each time it stayed down until somebody
# noticed. The last one, on 2026-09-17 16:24Z, left it down for six days.
#
# Liveness is judged by the desk's own clock, not by the process existing: /api/state reports
# last_cycle_ts, and a desk that is alive but has not cycled for STALL_S seconds is as down as one
# that exited. Three stale readings in a row (a minute apart at least) are required before a restart, because a laptop waking
# from sleep reads stale once and then catches up on its own.
#
# Exit codes from the desk: 0 is a clean shutdown (restarted unless a stop was asked for), 3 is
# "another desk holds the database lock" (wait, do not fight it), 75 is the desk asking for a
# fresh process after repeated storage failures. Anything else is a crash.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
CONFIG="${DESK_CONFIG:-$ROOT/config/desk.json}"
BINARY="$ROOT/build/kalshi-desk"
DATA="${DESK_DATA:-$ROOT/data}"   # overridable so a dry run never touches the live files
SUP_LOG="$DATA/supervisor.log"
PIDFILE="$DATA/desk_supervisor.pid"
STOPFILE="$DATA/desk.stop"
STALL_S="${DESK_STALL_SECONDS:-180}"
GRACE_S=120
# Until this run's first heartbeat, a long first pass is not a stall: the first cycle reconciles the
# account and refreshes every series (110 s on 2026-09-23). Only an old binary that never writes a
# heartbeat falls back to asking /api/state, and only after this long.
FIRST_PASS_S=600
mkdir -p "$DATA"

read -r ENV PORT DBPATH < <(python3 - "$CONFIG" "$ROOT" <<'EOF'
import json, os, re, sys
t = open(sys.argv[1]).read()
t = re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*[\s\S]*?\*/', lambda m: '' if m[0].startswith('/') else m[0], t)
c = json.loads(t)
db = os.path.expanduser(c.get('db_path', 'data/desk.sqlite'))
if not os.path.isabs(db): db = os.path.join(sys.argv[2], db)
print(c.get('env', 'demo'), int(c.get('http_port', 8787)), db)
EOF
)
HEARTBEAT="$DBPATH.heartbeat"
DESK_LOG="$DATA/${ENV}.log"

log() { echo "$(date -u +%FT%TZ) $*" >> "$SUP_LOG"; }

if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
  echo "supervisor already running (pid $(cat "$PIDFILE"))" >&2
  exit 1
fi
echo $$ > "$PIDFILE"
rm -f "$STOPFILE"
CHILD=""

stop() {
  touch "$STOPFILE"
  log "stop requested"
  if [ -n "$CHILD" ] && kill -0 "$CHILD" 2>/dev/null; then
    # The desk finishes its current cycle before it exits. Give it two minutes, then force it:
    # exiting here with the desk still alive would orphan it, and a later `tradeit up` would then
    # start a second supervisor next to a desk nobody is watching.
    kill -TERM "$CHILD" 2>/dev/null
    for _ in $(seq 1 240); do kill -0 "$CHILD" 2>/dev/null || break; sleep 0.5; done
    if kill -0 "$CHILD" 2>/dev/null; then
      log "desk still running 120s after SIGTERM; SIGKILL"
      kill -KILL "$CHILD" 2>/dev/null
    fi
  fi
  rm -f "$PIDFILE"
  exit 0
}
trap stop TERM INT

# Seconds since the desk's loop last wrote its heartbeat file (written after every pass). This is
# the liveness test: it does not depend on the desk's request threads, which a busy dashboard can
# tie up. Only a desk too old to write the file falls back to asking /api/state.
cycle_age() {
  if [ -f "$HEARTBEAT" ] && [ "$(stat -f %m "$HEARTBEAT")" -ge "$STARTED" ]; then
    local hb
    hb=$(cat "$HEARTBEAT" 2>/dev/null)
    case "$hb" in ''|*[!0-9]*) echo 0; return ;; esac   # mid-rename or partial: not a stall
    echo $(( $(date +%s) - hb ))
    return
  fi
  if (( $(date +%s) - STARTED < FIRST_PASS_S )); then echo 0; return; fi
  http_cycle_age
}

# Seconds since the desk's last completed cycle, or empty if the desk does not answer.
http_cycle_age() {
  curl -s -m 5 "http://127.0.0.1:${PORT}/api/state" 2>/dev/null | python3 -c '
import json, sys, time
try:
    d = json.load(sys.stdin)
    ts = d.get("last_cycle_ts") or 0
    print(int(time.time() - ts) if ts else "")
except Exception:
    print("")
' 2>/dev/null
}

log "supervisor up (env=$ENV port=$PORT stall=${STALL_S}s)"
BACKOFF=10
while [ ! -f "$STOPFILE" ]; do
  if [ ! -x "$BINARY" ]; then
    log "binary missing at $BINARY; retrying in 60s"
    sleep 60 & wait $!
    continue
  fi
  STARTED=$(date +%s)
  log "starting desk"
  "$BINARY" --config "$CONFIG" serve >> "$DESK_LOG" 2>&1 &
  CHILD=$!
  STRIKES=0
  while kill -0 "$CHILD" 2>/dev/null; do
    T0=$(date +%s)
    sleep 30 & wait $!
    [ -f "$STOPFILE" ] && break
    kill -0 "$CHILD" 2>/dev/null || break
    # A 30 s sleep that took much longer means the machine was asleep. Everything is stale for a
    # moment after that and the desk needs a pass to catch up: start counting again.
    if (( $(date +%s) - T0 > 90 )); then
      log "wall clock jumped $(( $(date +%s) - T0 ))s during a 30s wait (sleep?); strikes reset"
      STRIKES=0
      continue
    fi
    (( $(date +%s) - STARTED < GRACE_S )) && continue
    AGE=$(cycle_age)
    if [ -z "$AGE" ] || [ "$AGE" -gt "$STALL_S" ]; then
      STRIKES=$((STRIKES + 1))
      log "desk not cycling (age=${AGE:-no answer}), strike $STRIKES"
      if [ "$STRIKES" -ge 3 ]; then
        log "restarting stalled desk pid $CHILD"
        kill -TERM "$CHILD" 2>/dev/null
        for _ in $(seq 1 90); do kill -0 "$CHILD" 2>/dev/null || break; sleep 0.5; done
        kill -0 "$CHILD" 2>/dev/null && { log "desk ignored SIGTERM; SIGKILL"; kill -KILL "$CHILD" 2>/dev/null; }
        break
      fi
    else
      STRIKES=0
    fi
  done
  wait "$CHILD" 2>/dev/null
  CODE=$?
  CHILD=""
  LIVED=$(( $(date +%s) - STARTED ))
  log "desk exited code=$CODE after ${LIVED}s"
  [ -f "$STOPFILE" ] && break
  if [ "$CODE" -eq 3 ]; then
    log "another desk holds the database lock; waiting 60s"
    sleep 60 & wait $!
    continue
  fi
  # A desk that dies young is backed off exponentially up to five minutes; one that ran for a
  # while restarts quickly.
  if [ "$LIVED" -gt 600 ]; then BACKOFF=10; else BACKOFF=$(( BACKOFF * 2 > 300 ? 300 : BACKOFF * 2 )); fi
  log "restarting in ${BACKOFF}s"
  sleep "$BACKOFF" & wait $!
done
rm -f "$PIDFILE"
log "supervisor exiting"
