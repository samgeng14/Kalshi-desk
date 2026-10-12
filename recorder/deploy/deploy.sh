#!/usr/bin/env bash
# Copy the committed recorder to the VM and run its setup.sh there. Run on the Mac, from the repo:
#
#   recorder/deploy/deploy.sh                    plan only: copy the release, show what setup would do
#   recorder/deploy/deploy.sh --apply            install it (never replacing your env, units or keys)
#   recorder/deploy/deploy.sh --apply --start    and start the recorder (and uploads, once R2's token is there)
#   recorder/deploy/deploy.sh --apply --replace-units   replace changed systemd units (old kept as .bak)
#
# The release is `git archive HEAD recorder/`: committed code only, never the working tree, so
# nothing uncommitted (or untracked, or secret) leaves the Mac. Host and key: the existing VM and
# key (docs/deploy-oracle.md); override with DESKD_VM=user@host and DESKD_VM_KEY=path.
set -euo pipefail
VM=${DESKD_VM:-ubuntu@64.181.211.165}
KEY=${DESKD_VM_KEY:-$HOME/.ssh/oracle_deskd}
ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
SHA=$(git -C "$ROOT" rev-parse --short=12 HEAD)
REL="$(date -u +%Y%m%dT%H%M%SZ)-$SHA"
SSH=(ssh -i "$KEY" -o BatchMode=yes -o ConnectTimeout=15 "$VM")

if ! git -C "$ROOT" diff --quiet HEAD -- recorder/; then
  echo "note: recorder/ has uncommitted changes; the VM gets HEAD ($SHA) without them" >&2
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir "$TMP/$REL"
git -C "$ROOT" archive HEAD recorder | tar -x -C "$TMP/$REL"
echo "$REL" > "$TMP/$REL/RELEASE"
tar -C "$TMP" -czf "$TMP/$REL.tgz" "$REL"

echo "deploy: release $REL to $VM"
# into ~/deskd-recorder-releases on the VM, unpacked beside its tarball (both kept: nothing there
# is ever deleted)
"${SSH[@]}" "mkdir -p deskd-recorder-releases"
scp -i "$KEY" -o BatchMode=yes -q "$TMP/$REL.tgz" "$VM:deskd-recorder-releases/"
"${SSH[@]}" "tar -xzf 'deskd-recorder-releases/$REL.tgz' -C deskd-recorder-releases && \
  bash 'deskd-recorder-releases/$REL/recorder/deploy/setup.sh' $(printf '%q ' "$@")"
