#!/usr/bin/env bash
# Read-only inventory of the deskd-recorder VM. Changes nothing: no apt update, no installs,
# no service changes, and it never prints file contents that could hold credentials (env
# files, keys, tokens are listed by name, size and mode only).
#
#   ssh -i ~/.ssh/oracle_deskd ubuntu@64.181.211.165 'bash -s' < tools/vm/inspect.sh > vm-inventory.txt
#
# sudo is used only non-interactively (sudo -n) for read commands; without it those lines say so.
set -u
sec() { printf '\n===== %s =====\n' "$1"; }
try() { "$@" 2>&1 || echo "(exit $?: $*)"; }
sudo_try() { if sudo -n true 2>/dev/null; then sudo -n "$@" 2>&1; else echo "(sudo needs a password; skipped: $*)"; fi; }

sec "host"
try hostnamectl
try uname -a
try cat /etc/os-release
try uptime
echo "time now (UTC): $(date -u +%FT%TZ)"

sec "pending reboot and updates"
if [ -f /var/run/reboot-required ]; then echo "REBOOT REQUIRED"; cat /var/run/reboot-required.pkgs 2>/dev/null; else echo "no reboot required"; fi
if [ -x /usr/lib/update-notifier/apt-check ]; then
  /usr/lib/update-notifier/apt-check --human-readable 2>&1
fi
echo "-- upgradable (from the current package cache; the cache is NOT refreshed):"
try apt list --upgradable 2>/dev/null | head -80
echo "-- unattended-upgrades:"
try systemctl is-enabled unattended-upgrades
try cat /etc/apt/apt.conf.d/20auto-upgrades
echo "-- last apt activity:"
try tail -n 5 /var/log/apt/history.log

sec "cpu, memory, disk"
try nproc
try lscpu
try free -h
try swapon --show
try df -hT -x tmpfs -x devtmpfs -x squashfs
try lsblk
echo "-- largest directories under / (one level, same filesystem):"
sudo_try du -xh --max-depth=1 / | sort -h | tail -15

sec "time sync"
try timedatectl
try chronyc tracking
try systemctl is-active chrony systemd-timesyncd

sec "network"
try ip -brief addr
echo "-- listening sockets:"
sudo_try ss -tulpn
try ss -tuln
echo "-- firewall:"
sudo_try ufw status verbose
sudo_try iptables -S
sudo_try nft list ruleset | head -60

sec "services"
try systemctl list-units --type=service --state=running --no-pager
try systemctl --failed --no-pager
echo "-- unit files outside the distro (/etc/systemd/system, user units):"
try ls -la /etc/systemd/system
try ls -la ~/.config/systemd/user
try systemctl --user list-units --type=service --no-pager
echo "-- timers:"
try systemctl list-timers --all --no-pager
echo "-- crontabs:"
try crontab -l
sudo_try ls -la /etc/cron.d /var/spool/cron/crontabs

sec "processes (top by memory)"
try ps -eo pid,user,etime,pcpu,pmem,rss,comm --sort=-rss | head -30   # no args: a command line can carry a token

sec "containers"
for c in docker podman; do command -v $c >/dev/null && { echo "-- $c:"; sudo_try $c ps -a; }; done
command -v docker >/dev/null || command -v podman >/dev/null || echo "no docker or podman"

sec "runtimes"
for b in python3 pip3 uv pipx git zstd rclone aws node gcc g++ cmake ninja; do
  printf '%-8s ' "$b"; command -v "$b" >/dev/null && ("$b" --version 2>&1 | head -1) || echo "not installed"
done
try python3 -c 'import sys; print(sys.version)'
for m in pyarrow duckdb websockets aiohttp httpx cryptography zstandard boto3; do
  python3 -c "import $m; print('$m', getattr($m,'__version__','?'))" 2>/dev/null || echo "$m: not installed"
done

sec "kalshi / deskd / recorder footprint (names only, never contents)"
for d in ~ /opt /srv /etc /var/lib /var/log; do
  sudo_try find "$d" -maxdepth 4 \( -iname '*kalshi*' -o -iname '*deskd*' -o -iname '*recorder*' -o -iname '*healthcheck*' -o -iname '*rclone*' -o -iname '*r2*' \) \
    -not -path '*/proc/*' -printf '%M %u %8s %TY-%Tm-%Td %p\n' 2>/dev/null | head -50
done
echo "-- home directory:"
try ls -la ~
echo "-- credential-looking files under ~ (mode and size only):"
find ~ -maxdepth 3 \( -name '*.pem' -o -name '*.key' -o -name '*.json' -o -name '.env*' -o -name '*token*' -o -name 'credentials*' \) \
  -not -path '*/.cache/*' -printf '%M %u %8s %p\n' 2>/dev/null | head -40
echo "-- ~/.ssh (names and modes only):"
try ls -la ~/.ssh

sec "logs and journal"
try journalctl --disk-usage
try cat /etc/systemd/journald.conf.d/*.conf
try ls -la /var/log | head -40

sec "users and access"
try id
try last -n 10
try grep -E '^(PasswordAuthentication|PermitRootLogin|PubkeyAuthentication)' /etc/ssh/sshd_config /etc/ssh/sshd_config.d/*.conf
echo "done: $(date -u +%FT%TZ)"
