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
  OK=1
  # DISABLED 2026-09-12. Taking the snapshot means reading all 1.37GB of prod.sqlite through
  # SQLite over the bridge mount while the desk writes it. That is the heaviest possible version
  # of the access this was meant to avoid, and the desk died silently 90 seconds after one ran -
  # no exception, no log line, just gone. Snapshot from the Mac natively instead, or with the desk
  # stopped. Nothing in this loop may open the live database.
  #   bash scripts/wx_snapshot.sh
  python3 -u research/wx_settlement_watch.py >> "$LOG" 2>&1 || { OK=0; echo "settlement watch failed $(date -u +%FT%TZ)" >> "$LOG"; }
  # Discovery writes config/weather_discovered.json through a temp file, so a failed run leaves
  # yesterday's map in place rather than a truncated one. Cities list and delist through the day
  # as each local date rolls over, which is why this runs hourly rather than once a morning.
  python3 -u research/wx_discover.py >> "$LOG" 2>&1 || { OK=0; echo "discovery failed $(date -u +%FT%TZ)" >> "$LOG"; }
  if [ "$OK" = "1" ]; then echo "$(date -u +%FT%TZ) ok" > "$HB"; else echo "$(date -u +%FT%TZ) FAILED" > "$HB"; fi
  sleep "$EVERY_S"
done
