#!/usr/bin/env bash
# Install or update the deskd recorder on the VM (docs/deploy-oracle.md). Run ON the VM, from the
# unpacked release that deploy.sh copied over:
#
#   ./setup.sh                 plan: say what it would do, change nothing (the default)
#   ./setup.sh --apply         do it: base system, user, dirs, release, venv, units (never replacing your files)
#   ./setup.sh --apply --start also enable and start the recorder and the upload timer
#   ./setup.sh --apply --replace-units   replace changed systemd units (the old ones kept as .bak)
#
# The base system first, in the same mode: base.sh (chrony with timesyncd off, a swap file, the
# journal capped at 1 GB, security-only unattended upgrades without automatic reboots).
#
# Safety rules:
#   - nothing is deleted: every release stays under /opt/deskd-recorder/releases/, the venv and
#     data are only added to
#   - an existing /etc/deskd-recorder/recorder.env or credential is never touched; a changed unit
#     file is left alone (the new one written beside it as .new) unless --replace-units
#   - it starts nothing without --start, and --start refuses while the read-only key's files are
#     missing; the upload timer starts only once the R2 token exists
#   - the recorder has no order route at all (recorder/kalshi.py); this script cannot add one
set -euo pipefail
MODE=plan; START=0; REPLACE_UNITS=0
for a in "$@"; do
  case "$a" in
    --apply) MODE=apply ;;
    --start) START=1 ;;
    --replace-units) REPLACE_UNITS=1 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done
[ "$START" = 1 ] && [ "$MODE" != apply ] && { echo "--start needs --apply" >&2; exit 2; }

SRC=$(cd "$(dirname "$0")/../.." && pwd)            # the release: holds recorder/
[ -f "$SRC/recorder/rec.py" ] || { echo "run setup.sh from an unpacked release (no $SRC/recorder/rec.py)" >&2; exit 2; }
REL_ID=$(cat "$SRC/RELEASE" 2>/dev/null || date -u +%Y%m%dT%H%M%SZ)
OPT=/opt/deskd-recorder
DATA=/var/lib/deskd-recorder
ETC=/etc/deskd-recorder
CRED=$ETC/credentials
UNITS="deskd-recorder.service deskd-recorder-upload.service deskd-recorder-upload.timer"

log() { printf '%s %s\n' "$(date -u +%FT%TZ)" "$*"; }
act() {   # act "what" cmd...: run it with --apply, else say it
  local what=$1; shift
  if [ "$MODE" = apply ]; then log "do: $what"; "$@"; else log "would: $what"; fi
}
SUDO=sudo; [ "$(id -u)" = 0 ] && SUDO=

log "setup mode: $MODE, release $REL_ID from $SRC"

# 0. the base system, in this mode (base.sh says what it does and keeps every changed file)
if [ "$MODE" = apply ]; then bash "$SRC/recorder/deploy/base.sh" --apply; else bash "$SRC/recorder/deploy/base.sh"; fi

# 1. packages: python3-venv only (pip comes with the venv). confold: existing configs are kept.
# Installed means dpkg's state: dpkg -s also succeeds for a package removed but not purged.
if [ "$(dpkg-query -W -f='${db:Status-Status}' python3-venv 2>/dev/null)" != installed ]; then
  act "apt-get install python3-venv" $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y \
    -o Dpkg::Options::=--force-confold --no-remove python3-venv
else
  log "ok: python3-venv installed"
fi

# 2. the service user: no shell, no home, no sudo
if id recorder >/dev/null 2>&1; then
  log "ok: user recorder exists"
else
  act "add system user recorder" $SUDO useradd --system --no-create-home --shell /usr/sbin/nologin recorder
fi

# 3. directories (made if missing; existing ones keep their owner and mode)
mkd() {   # mkd dir owner mode
  if [ -d "$1" ]; then log "ok: $1 exists"; else act "mkdir $1 ($2 $3)" $SUDO install -d -o "${2%%:*}" -g "${2##*:}" -m "$3" "$1"; fi
}
mkd $OPT root:root 0755
mkd $OPT/releases root:root 0755
mkd $DATA recorder:recorder 0750
mkd $ETC root:root 0755
mkd $CRED root:root 0700

# 4. this release, then the `current` link (switched atomically; old releases stay)
DEST=$OPT/releases/$REL_ID
if [ -d "$DEST" ]; then
  log "ok: release $REL_ID already in place"
else
  act "copy release to $DEST" $SUDO sh -c "mkdir -p '$DEST' && cp -R '$SRC/recorder' '$DEST/' && chmod -R a+rX,go-w '$DEST'"
fi
CUR=$(readlink "$OPT/current" 2>/dev/null || true)
if [ "$CUR" = "$DEST" ]; then
  log "ok: current -> $DEST"
else
  act "point $OPT/current at $DEST (was ${CUR:-none})" $SUDO sh -c "ln -sfn '$DEST' '$OPT/current.new' && mv -T '$OPT/current.new' '$OPT/current'"
fi

# 5. the venv: made once, the pinned requirements installed into it (adds or upgrades, never removes)
if [ -x $OPT/.venv/bin/python ]; then
  log "ok: venv exists"
else
  act "python3 -m venv $OPT/.venv" $SUDO python3 -m venv $OPT/.venv
fi
act "pip install -r requirements.txt (pinned) into the venv" $SUDO $OPT/.venv/bin/pip install --quiet --disable-pip-version-check \
  -r "$SRC/recorder/requirements.txt"

# 6. settings: recorder.env only where none exists
if $SUDO test -f $ETC/recorder.env; then
  log "ok: $ETC/recorder.env exists (kept as it is)"
else
  act "install $ETC/recorder.env from recorder.env.example" $SUDO install -o root -g root -m 0644 \
    "$SRC/recorder/deploy/recorder.env.example" $ETC/recorder.env
fi

# 7. systemd units: new ones installed; a changed one kept unless --replace-units
changed_units=0
for u in $UNITS; do
  new="$SRC/recorder/deploy/$u" cur="/etc/systemd/system/$u"
  if [ ! -f "$cur" ]; then
    act "install unit $u" $SUDO install -o root -g root -m 0644 "$new" "$cur"; changed_units=1
  elif cmp -s "$new" "$cur"; then
    log "ok: unit $u is current"
  elif [ "$REPLACE_UNITS" = 1 ]; then
    act "replace unit $u (old one kept as $cur.bak)" $SUDO sh -c "cp -p '$cur' '$cur.bak' && install -o root -g root -m 0644 '$new' '$cur'"
    changed_units=1
  else
    log "differs: unit $u (yours kept; the new one goes to $cur.new; --replace-units to switch)"
    diff -u "$cur" "$new" | sed 's/^/    /' | head -40 || true
    act "write $cur.new" $SUDO install -o root -g root -m 0644 "$new" "$cur.new"
  fi
done
[ "$changed_units" = 1 ] && act "systemctl daemon-reload" $SUDO systemctl daemon-reload

# 8. credentials: only reported (you put them there; README.md "On the VM")
have() { $SUDO test -s "$CRED/$1"; }
missing=0
for c in kalshi-readonly.pem kalshi-readonly.id; do
  if have $c; then log "ok: credential $c"; else log "MISSING: $CRED/$c (the read-only Kalshi key; required)"; missing=1; fi
done
have r2-write.token && log "ok: credential r2-write.token" || log "missing: $CRED/r2-write.token (uploads stay off until it exists)"
have healthchecks.url && log "ok: credential healthchecks.url" || log "optional: $CRED/healthchecks.url"
have healthchecks-upload.url && log "ok: credential healthchecks-upload.url" || log "optional: $CRED/healthchecks-upload.url"

# 9. start, only when asked
if [ "$START" = 1 ]; then
  [ "$missing" = 1 ] && { log "not starting: the read-only key is missing"; exit 3; }
  act "enable and (re)start deskd-recorder" $SUDO sh -c 'systemctl enable deskd-recorder.service && systemctl restart deskd-recorder.service'
  if have r2-write.token; then
    act "enable the upload timer" $SUDO systemctl enable --now deskd-recorder-upload.timer
  else
    log "upload timer not enabled: no R2 token yet"
  fi
  sleep 5
  $SUDO systemctl --no-pager --lines=0 status deskd-recorder.service | head -5 || true
elif systemctl is-active --quiet deskd-recorder.service 2>/dev/null && [ "$MODE" = apply ] && [ "$CUR" != "$DEST" ]; then
  log "deskd-recorder is running the old release: restart it to switch (setup.sh --apply --start)"
fi
log "done ($MODE)"
