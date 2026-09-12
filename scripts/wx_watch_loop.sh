#!/usr/bin/env bash
# Keeps the weather settlement comparison collecting. api.weather.gov serves roughly a fortnight
# of CLI products per location, so a station-day nobody collected inside that window is gone for
# good - which is the whole reason this runs on a clock instead of on demand.
#
#   nohup scripts/wx_watch_loop.sh >/dev/null 2>&1 &
#
# Deliberately separate from recorder_supervise.sh: that process is recording market data and
# must not be restarted to change anything here. The collector is idempotent, so an extra run
# costs nothing and a missed one is caught by the next.
set -uo pipefail
cd "$(dirname "$0")/.."
LOG="$PWD/data/wx_watch.log"
HB="$PWD/data/wx_watch.heartbeat"
EVERY_S=${EVERY_S:-3600}

echo "wx settlement watch up $(date -u +%FT%TZ)" >> "$LOG"
while true; do
  echo "=== run $(date -u +%FT%TZ) ===" >> "$LOG"
  if python3 -u research/wx_settlement_watch.py >> "$LOG" 2>&1; then
    echo "$(date -u +%FT%TZ) ok" > "$HB"
  else
    echo "$(date -u +%FT%TZ) FAILED" > "$HB"
    echo "collector exited non-zero $(date -u +%FT%TZ)" >> "$LOG"
  fi
  sleep "$EVERY_S"
done
