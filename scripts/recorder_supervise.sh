#!/usr/bin/env bash
# Keeps perps_recorder.py alive. Wall-clock time cannot be backfilled, so the only thing that
# matters here is that a crash, a dropped socket or a closed terminal never ends the recording.
#
#   nohup scripts/recorder_supervise.sh >/dev/null 2>&1 &
#
# Liveness is checked by the FILE the recorder writes, not by the process being alive: a recorder
# that is running but has stopped writing is the failure that cost us six hours, and a pgrep would
# have reported it healthy the whole time.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
LOG="$ROOT/data/recorder.log"
HB="$ROOT/data/recorder.heartbeat"
STALL_S=300

mkdir -p "$ROOT/data"
echo "supervisor up $(date -u +%FT%TZ)" >> "$LOG"

while true; do
  echo "=== start $(date -u +%FT%TZ) ===" >> "$LOG"
  python3 -u scripts/perps_recorder.py >> "$LOG" 2>&1 &
  PID=$!
  echo "$PID $(date -u +%FT%TZ)" > "$HB"

  while kill -0 "$PID" 2>/dev/null; do
    sleep 30
    NEW=$(find "$ROOT/data/raw/source=kalshi_margin" -name 'events.jsonl.gz' -newermt "-${STALL_S} seconds" 2>/dev/null | head -1)
    if [ -z "$NEW" ]; then
      echo "STALL: no write in ${STALL_S}s, killing $PID $(date -u +%FT%TZ)" >> "$LOG"
      kill "$PID" 2>/dev/null
      sleep 3
      kill -9 "$PID" 2>/dev/null
      break
    fi
    echo "$PID $(date -u +%FT%TZ) ok" > "$HB"
  done

  echo "=== exited $(date -u +%FT%TZ), restarting in 10s ===" >> "$LOG"
  sleep 10
done
