#!/usr/bin/env bash
# The VM's base system for the recorder (docs/deploy-oracle.md sections 2, 5, 6 and 9 step 3).
# setup.sh runs it first, in its own mode; it can also run alone, ON the VM:
#
#   ./base.sh            plan: say what it would do, change nothing (the default)
#   ./base.sh --apply    do it
#
# What it sees to:
#   - chrony keeps the clock (the recorder stamps first-seen times, so the offset matters), and
#     systemd-timesyncd is off: two NTP clients fight. Ubuntu's chrony package conflicts with
#     timesyncd's, so apt removes that one package; if apt would remove anything else, the
#     install is refused. timesyncd is turned off only once chrony is installed and running.
#   - swap: a SWAP_MB (2048) MB /swapfile, enabled by a systemd swap unit (fstab is not edited),
#     and only when the VM has no swap at all
#   - the journal capped at 1 GB (SystemMaxUse=1G), in a journald drop-in
#   - unattended-upgrades for security updates only, never rebooting by itself (the operator
#     reboots with tools/vm/update.sh --reboot when the recorder can stop)
#
# Safety rules (setup.sh's): a file that exists and differs is never replaced; ours is written
# beside it as .new and the difference shown. Nothing is removed except timesyncd's package as
# above. Every command is printed before it runs, and nothing runs without --apply.
#
# DESKD_BASE_ROOT (tests only) puts every file it reads or writes under another root; commands
# come from PATH, so a test stubs them.
set -euo pipefail
MODE=plan
for a in "$@"; do
  case "$a" in
    --apply) MODE=apply ;;
    -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done
R=${DESKD_BASE_ROOT:-}
SWAP_MB=${SWAP_MB:-2048}
log() { printf '%s %s\n' "$(date -u +%FT%TZ)" "$*"; }
act() {   # act "what" cmd...: run it with --apply, else say it
  local what=$1; shift
  if [ "$MODE" = apply ]; then log "do: $what"; "$@"; else log "would: $what"; fi
}
SUDO=sudo; [ "$(id -u)" = 0 ] && SUDO=
APT="env DEBIAN_FRONTEND=noninteractive apt-get -y -o Dpkg::Options::=--force-confold"
# dpkg's install state: dpkg -s also succeeds for a package removed but not purged (config-files)
installed() { [ "$(dpkg-query -W -f='${db:Status-Status}' "$1" 2>/dev/null)" = installed ]; }
unit_on() { systemctl is-enabled --quiet "$1" 2>/dev/null && systemctl is-active --quiet "$1" 2>/dev/null; }
unit_any() { systemctl is-enabled --quiet "$1" 2>/dev/null || systemctl is-active --quiet "$1" 2>/dev/null; }

UPDATED=0
apt_update() {   # once, before the first install
  [ "$UPDATED" = 1 ] && return 0
  UPDATED=1
  act "apt-get update" $SUDO apt-get update -qq
}

# put PATH CONTENT: a config file installed (0644) only where none exists. One that differs is
# kept and ours written beside it as PATH.new. CHANGED=1 when PATH itself was written.
CHANGED=0
put() {
  local path=$1 content=$2 tmp
  CHANGED=0
  tmp=$(mktemp)
  printf '%s\n' "$content" > "$tmp"
  if [ ! -e "$path" ]; then
    act "write $path" $SUDO install -D -m 0644 "$tmp" "$path"
    CHANGED=1
  elif cmp -s "$tmp" "$path"; then
    log "ok: $path is current"
  else
    log "differs: $path (yours kept; ours goes to $path.new)"
    diff -u "$path" "$tmp" | sed 's/^/    /' | head -30 || true
    act "write $path.new" $SUDO install -D -m 0644 "$tmp" "$path.new"
  fi
  rm -f "$tmp"
}

log "base mode: $MODE"

# 1. chrony, and systemd-timesyncd off
CHRONY=0
if installed chrony; then
  log "ok: chrony installed"
  CHRONY=1
else
  apt_update
  removes=$(apt-get -s install chrony 2>/dev/null | awk '/^Remv /{print $2}' | sort -u | tr '\n' ' ')
  others=$(printf '%s\n' $removes | grep -vx 'systemd-timesyncd' || true)
  if [ -n "$others" ]; then
    log "REFUSED: installing chrony would also remove: $others; install it by hand (timesyncd left on)"
  else
    act "apt-get install chrony${removes:+ (apt removes ${removes% }: it conflicts with chrony)}" \
      $SUDO $APT install chrony
    CHRONY=1
  fi
fi
if [ "$CHRONY" = 1 ]; then
  # chrony up first, timesyncd off only once it runs: a failure never leaves the VM without NTP
  if unit_on chrony; then
    log "ok: chrony enabled and running"
  elif ! act "enable and start chrony" $SUDO systemctl enable --now chrony; then
    log "FAILED: systemctl enable --now chrony (see systemctl status chrony)"
  fi
  if ! unit_any systemd-timesyncd; then
    log "ok: systemd-timesyncd is off"
  elif [ "$MODE" = apply ] && ! systemctl is-active --quiet chrony 2>/dev/null; then
    log "REFUSED: chrony is not running; systemd-timesyncd left on"
  else
    act "stop and disable systemd-timesyncd (chrony keeps the clock)" $SUDO systemctl disable --now systemd-timesyncd
  fi
fi

# 2. swap, only on a VM with none
SWAP_UNIT='# deskd recorder (recorder/deploy/base.sh): a swap file as a cushion for the hourly Parquet
# writes (docs/deploy-oracle.md section 2). A unit, so /etc/fstab is never edited.
[Unit]
Description=deskd recorder swap file

[Swap]
What=/swapfile

[Install]
WantedBy=swap.target'
if [ -n "$(swapon --show --noheadings 2>/dev/null || true)" ]; then
  log "ok: swap is on: $(swapon --show --noheadings 2>/dev/null | awk '{print $1 " " $3}' | tr '\n' ' ')"
elif grep -qs '^[^#]*[[:space:]]swap[[:space:]]' "$R/etc/fstab"; then
  log "note: /etc/fstab names a swap that is not on; left to you"
else
  SW="$R/swapfile"
  if [ -e "$SW" ]; then
    if [ "$(blkid -p -s TYPE -o value "$SW" 2>/dev/null || true)" = swap ]; then
      log "ok: $SW exists and is swap space"
    else
      log "REFUSED: $SW exists and is not swap space; not touching it"
      SW=
    fi
  else
    act "make $SW (${SWAP_MB} MB, mode 0600, mkswap)" \
      $SUDO sh -c "fallocate -l ${SWAP_MB}M '$SW' && chmod 600 '$SW' && mkswap '$SW' >/dev/null"
  fi
  if [ -n "$SW" ]; then
    put "$R/etc/systemd/system/swapfile.swap" "$SWAP_UNIT"
    act "enable and start swapfile.swap" $SUDO sh -c 'systemctl daemon-reload && systemctl enable --now swapfile.swap'
  fi
fi

# 3. the journal capped at 1 GB
put "$R/etc/systemd/journald.conf.d/deskd.conf" '# deskd recorder (recorder/deploy/base.sh): docs/deploy-oracle.md section 5
[Journal]
SystemMaxUse=1G'
[ "$CHANGED" = 1 ] && act "restart systemd-journald (it reads the cap when it starts)" $SUDO systemctl restart systemd-journald

# 4. security updates only, never an automatic reboot
if installed unattended-upgrades; then
  log "ok: unattended-upgrades installed"
else
  apt_update
  act "apt-get install unattended-upgrades" $SUDO $APT --no-remove install unattended-upgrades
fi
# read after Ubuntu's own 50unattended-upgrades: #clear drops its lists before ours. The release
# pocket itself never changes after release day; Ubuntu lists it so a security fix can pull a
# dependency from it. No -updates pocket: security only.
put "$R/etc/apt/apt.conf.d/52deskd-unattended-upgrades" '// deskd recorder (recorder/deploy/base.sh): security updates only, no automatic reboot
// (the operator reboots with tools/vm/update.sh --reboot), nothing removed automatically.
#clear Unattended-Upgrade::Allowed-Origins;
#clear Unattended-Upgrade::Origins-Pattern;
Unattended-Upgrade::Allowed-Origins {
        "${distro_id}:${distro_codename}";
        "${distro_id}:${distro_codename}-security";
        "${distro_id}ESMApps:${distro_codename}-apps-security";
        "${distro_id}ESM:${distro_codename}-infra-security";
};
Unattended-Upgrade::Automatic-Reboot "false";
Unattended-Upgrade::Remove-Unused-Dependencies "false";
Unattended-Upgrade::Remove-New-Unused-Dependencies "false";'
put "$R/etc/apt/apt.conf.d/20auto-upgrades" 'APT::Periodic::Update-Package-Lists "1";
APT::Periodic::Unattended-Upgrade "1";'
for t in apt-daily.timer apt-daily-upgrade.timer; do
  if systemctl is-enabled --quiet "$t" 2>/dev/null; then log "ok: $t enabled"; else log "note: $t is not enabled (Ubuntu enables it; left to you)"; fi
done

log "base done ($MODE)"
