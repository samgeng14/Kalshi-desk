#!/usr/bin/env bash
# Starts the desk's supervisor at login, from ~/Library/LaunchAgents/com.kalshi-desk.supervisor.plist.
#
# Why this exists: on 2026-09-28 the laptop restarted without a clean shutdown at 21:02Z, and the
# desk stayed down for three days because nothing started the supervisor again. A reboot is the
# one failure desk_supervise.sh cannot recover from on its own.
#
# `tradeit down` is the operator saying "stay down": it leaves data/desk.stop behind, and while
# that file exists a login starts nothing. `tradeit up` removes it. A supervisor already running
# (started by tradeit) makes desk_supervise.sh exit at once, so the two cannot double up.
#
# To stop starting at login: launchctl bootout gui/$(id -u)/com.kalshi-desk.supervisor, and delete
# the plist.
set -uo pipefail
cd "$(dirname "$0")/.."
DATA="${DESK_DATA:-$PWD/data}"
if [ -f "$DATA/desk.stop" ]; then
  echo "$(date -u +%FT%TZ) login: desk.stop present (tradeit down was the last word); not starting" >> "$DATA/supervisor.log"
  exit 0
fi
echo "$(date -u +%FT%TZ) login: starting the supervisor" >> "$DATA/supervisor.log"
exec scripts/desk_supervise.sh >> "$DATA/supervisor.log" 2>&1
