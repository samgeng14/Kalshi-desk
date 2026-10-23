#!/usr/bin/env bash
# Controlled Ubuntu updates for the deskd-recorder VM. Run ON the VM (copy it over first):
#
#   scp -i ~/.ssh/oracle_deskd tools/vm/update.sh ubuntu@64.181.211.165:
#   ssh -i ~/.ssh/oracle_deskd ubuntu@64.181.211.165 './update.sh'            # plan only (default)
#   ssh -i ~/.ssh/oracle_deskd ubuntu@64.181.211.165 './update.sh --apply'    # install updates
#   ssh -i ~/.ssh/oracle_deskd ubuntu@64.181.211.165 './update.sh --reboot'   # reboot if required and idle
#
# Safety rules:
#   - existing configuration files are always kept (dpkg --force-confold); nothing is removed
#     (no autoremove, no dist-upgrade that would remove packages)
#   - it refuses to run while another apt/dpkg holds the lock
#   - it refuses to reboot while a recorder or deskd unit/process is active, unless --force-busy
#   - it never touches recorder or deskd units, files or credentials
set -euo pipefail
MODE=plan; FORCE_BUSY=0
for a in "$@"; do
  case "$a" in
    --apply) MODE=apply ;;
    --reboot) MODE=reboot ;;
    --force-busy) FORCE_BUSY=1 ;;
    *) echo "unknown option $a"; exit 2 ;;
  esac
done
log() { printf '%s %s\n' "$(date -u +%FT%TZ)" "$*"; }
busy() {
  # anything of ours that a reboot would interrupt
  local found=0
  for u in $(systemctl list-units --type=service --state=active --no-legend 2>/dev/null | awk '{print $1}' | grep -Ei 'recorder|deskd|kalshi' || true); do
    log "active unit: $u"; found=1
  done
  if pgrep -fa 'rec\.py|recorder|deskd' | grep -v -e update.sh -e pgrep >/dev/null 2>&1; then
    pgrep -fa 'rec\.py|recorder|deskd' | grep -v -e update.sh -e pgrep | awk '{print "active process: pid " $1 " " $2}'; found=1
  fi
  return $((1-found))
}
apt_locked() { sudo fuser /var/lib/dpkg/lock-frontend /var/lib/dpkg/lock >/dev/null 2>&1; }

log "mode: $MODE"
[ -f /var/run/reboot-required ] && log "reboot currently required by: $(tr '\n' ' ' < /var/run/reboot-required.pkgs 2>/dev/null)" || log "no reboot currently required"

case "$MODE" in
  plan)
    log "refreshing the package index (read-only for installed software)"
    sudo apt-get update -qq
    log "what --apply would install (simulation):"
    sudo apt-get -s -o Dpkg::Options::=--force-confold upgrade | grep -E '^(Inst|Conf)' | sed 's/^/  /' || echo "  nothing to upgrade"
    log "held back (would need a dist-upgrade; not done by this script):"
    apt-get -s upgrade 2>/dev/null | sed -n '/kept back/,/^[0-9]/p' | sed 's/^/  /'
    busy && log "NOTE: recorder/deskd activity found; --reboot would refuse" || log "no recorder/deskd activity"
    ;;
  apply)
    if apt_locked; then log "apt/dpkg is busy (unattended-upgrades?); try again later"; exit 1; fi
    log "installing updates, keeping every existing config file"
    sudo apt-get update -qq
    sudo DEBIAN_FRONTEND=noninteractive apt-get -y \
      -o Dpkg::Options::=--force-confold -o Dpkg::Options::=--force-confdef upgrade
    log "done; reboot required: $([ -f /var/run/reboot-required ] && echo yes || echo no)"
    ;;
  reboot)
    if [ ! -f /var/run/reboot-required ]; then log "no reboot required; nothing to do"; exit 0; fi
    if apt_locked; then log "apt/dpkg is busy; not rebooting mid-install"; exit 1; fi
    if busy && [ "$FORCE_BUSY" -ne 1 ]; then log "refusing to reboot: stop the units above first, or pass --force-busy"; exit 1; fi
    log "rebooting in 1 minute (cancel with: sudo shutdown -c)"
    sudo shutdown -r +1 "deskd-recorder: kernel/library updates"
    ;;
esac
