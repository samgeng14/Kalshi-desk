# deskd-recorder on Oracle Cloud: deployment plan (2026-10-10)

Status: **plan only.** Nothing below has been run on the VM from this repository. The handoff
(`docs/handoff-deskd.md`, 2026-10-07) says "Nothing installed yet"; `tools/vm/inspect.sh`
confirms or corrects that before any change.

| | |
|---|---|
| Instance | `deskd-recorder`, `VM.Standard.A1.Flex`, 1 OCPU (Ampere arm64), 6 GB, ~145 GB boot volume, Ubuntu 24.04 |
| Address | public 64.181.211.165, private 10.0.0.176 |
| Access | `ssh -i ~/.ssh/oracle_deskd ubuntu@64.181.211.165` (operator's Mac only) |

## 1. What runs where (approved plan, sections 1, 2 and 7)

| Component | Where | Why |
|---|---|---|
| `deskd` (C++: market data, gateway, Manual and Systematic orders, UI on 127.0.0.1:8788) | **The Mac** | "Orders leave only from the Mac." The trading key never sits on a cloud VM; the UI is loopback-only by design (Host/Origin guards) |
| `deskctl` (KILL when deskd hangs) | The Mac | Must run next to deskd |
| Recorder (`recorder/rec.py`, Python asyncio) | **The Oracle VM, 24/7** | Read-only production key; records order books, trades, lifecycle, Polymarket books and scores; hourly zstd Parquet to Cloudflare R2 |
| Research / backtests | The Mac (DuckDB over R2, read-only secret) | |

Nothing in `desk/deskd` today needs the VM. The recorder is the only VM workload, and its code
lives on `desk/recorder` (an untracked, unfinished `recorder/` on the Mac; not on GitHub yet).

**Running trading 24/7 on the VM is a change to the approved plan** (decision 2 says: add an
always-on machine only if LIP-15 pays after two supervised weeks). It would put a
trade-enabled production key on a cloud host, needs an authenticated tunnel for the UI, and
deskd has only been built and tested on macOS and (since 2026-10-10) Linux x86-64, not arm64
Linux. Recommended: keep the VM read-only now; revisit after week 4 with LIP results. Section 8
lists what a 24/7 deskd would need if you decide otherwise.

## 2. Is the VM big enough?

| Resource | Recorder need (est.) | VM | Verdict |
|---|---|---|---|
| CPU | WebSocket JSON parsing for explicit subscription lists plus hourly Parquet encode: 5-20% of one core, short spikes at the hour | 1 Ampere core | Enough. Do not build deskd here (C++ at -j1 would take minutes and starve the recorder) |
| Memory | Python + pyarrow buffers for one hour of rows: 0.3-1 GB | 6 GB | Ample; add a 2 GB swap file as a cushion |
| Disk | ~120 MB/day Parquet (plan section 7); keep 14 days locally after upload ≈ 2 GB; journald capped at 1 GB | ~145 GB | Ample (years of headroom) |
| Network | A few hundred kbit/s in; ~120 MB/day out to R2 | Always Free egress 10 TB/month | Ample |

**Risk: Oracle reclaims "idle" Always Free instances.** Oracle stops an Always Free instance
it deems idle: over 7 days, CPU p95, network and (A1 only) memory utilization all below a
threshold (reported as 15% since April 2023; check Oracle's current "Always Free Resources"
page). A light recorder can sit below all three. Oracle's own advice is converting the account
to Pay As You Go, which keeps Always Free usage free of charge and is not subject to idle
reclamation. Do this before relying on 24/7 recording.

## 3. Layout on the VM

```
/opt/deskd-recorder/            code (git checkout of desk/recorder, read-only to the service)
/opt/deskd-recorder/.venv/      Python virtualenv (pinned requirements)
/var/lib/deskd-recorder/        Parquet staging, manifest, gaps table (service-owned, 0750)
/etc/deskd-recorder/recorder.env   non-secret settings (subscription lists, R2 bucket name)
/etc/deskd-recorder/credentials/   secrets (root:root 0700; files 0400), loaded via systemd credentials
```

A dedicated system user `recorder` (no shell, no sudo) runs the service.

## 4. Automatic restarts

`/etc/systemd/system/deskd-recorder.service` (installed by the recorder's `deploy.sh`):

```ini
[Unit]
Description=deskd recorder (read-only Kalshi/Polymarket/scores to Parquet)
Wants=network-online.target chrony.service
After=network-online.target chrony.service

[Service]
User=recorder
WorkingDirectory=/opt/deskd-recorder
EnvironmentFile=/etc/deskd-recorder/recorder.env
LoadCredential=kalshi-key.pem:/etc/deskd-recorder/credentials/kalshi-readonly.pem
LoadCredential=kalshi-key-id:/etc/deskd-recorder/credentials/kalshi-readonly.id
LoadCredential=r2-token:/etc/deskd-recorder/credentials/r2-write.token
LoadCredential=hc-url:/etc/deskd-recorder/credentials/healthchecks.url
ExecStart=/opt/deskd-recorder/.venv/bin/python -m recorder.rec
Restart=always
RestartSec=5
StartLimitIntervalSec=0
WatchdogSec=120            # rec.py sends sd_notify WATCHDOG=1 only while every feed advances
TimeoutStopSec=30          # SIGTERM: flush the open hour's Parquet, then exit
MemoryMax=3G
Nice=5
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
ReadWritePaths=/var/lib/deskd-recorder

[Install]
WantedBy=multi-user.target
```

The R2 upload runs as `deskd-recorder-upload.timer` (hourly, `Persistent=true`) so a crash in
the recorder never blocks uploads of finished hours, and an upload failure never stops
recording.

## 5. Logging

- The service logs to journald (stdout, one line per event, no request bodies, never a key or
  token). `journalctl -u deskd-recorder -f` to follow.
- Cap journald: `/etc/systemd/journald.conf.d/deskd.conf` with `SystemMaxUse=1G`.
- Data integrity is in the data, not the logs: each hourly file has a manifest row (rows, time
  range, sha256) and the `gaps` table records every disconnect and sequence gap.

## 6. Monitoring

| Check | How | Alert |
|---|---|---|
| Recorder alive and advancing | rec.py pings healthchecks.io every minute only while every feed advanced | healthchecks.io after 5 silent minutes (plan section 7) |
| Hourly upload | upload job pings a second healthchecks.io check | missed 2 hours |
| Disk | upload job refuses to delete local files not verified on R2; alerts above 70% | via the upload check's failure ping |
| Clock | chrony; rec.py stamps first-seen times, so offset matters | `chronyc tracking` offset > 100 ms logged as a gap reason |
| Reboots/updates | unattended-upgrades for security only, automatic reboot OFF | operator reboots with `tools/vm/update.sh --reboot` |

## 7. Credentials

| Secret | Scope | Where it lives |
|---|---|---|
| Kalshi production API key | **read-only** (create it in Kalshi with read access only; never the trading key) | `/etc/deskd-recorder/credentials/`, 0400 root, given to the service only through `LoadCredential` |
| Cloudflare R2 token | **write-only** to the recorder bucket; `archive/` locked against deletes | same |
| healthchecks.io ping URLs | not secret, but kept there too | same |

Rules: secrets are copied with `scp` from the Mac straight into place, never committed, never
in `recorder.env`, never passed on a command line (visible in `ps`), never printed in logs.
The Kalshi demo key (`~/.kalshi/demo.json`) stays on the Mac; it is not needed on the VM.
SSH: key-only login (verify `PasswordAuthentication no`), only port 22 open in the OCI
security list and the host firewall; the recorder listens on no port.

## 8. If you later decide to run deskd 24/7 (not recommended yet)

Needs, at least: a separate Kalshi subaccount or account for the VM so the Mac and the VM can
never trade one account at once (deskd's account lock is per machine); an arm64 Linux build
and a full test pass there; SSH-tunnel-only UI access (`ssh -L 8788:127.0.0.1:8788`); the
trading key in systemd credentials as above; `deskctl kill` reachable over SSH; the supervisor
restart with exit 75 (plan section 2) as a systemd unit; and latency measured from the VM's
region to Kalshi before trusting the 100 ms p90 target. That is a plan change for you to approve.

## 9. Order of work

1. **Inspect (read-only):** `ssh -i ~/.ssh/oracle_deskd ubuntu@64.181.211.165 'bash -s' < tools/vm/inspect.sh > vm-inventory.txt`
2. **Updates, controlled:** copy `tools/vm/update.sh` over; run it with no flag (shows the plan), then
   `--apply` (keeps every existing config file, removes nothing), then `--reboot` (refuses while a
   recorder or deskd process is active; reboots in one minute, cancellable).
3. **Base setup** (recorder's `setup.sh`, to be written on `desk/recorder`): user, directories,
   chrony, swap, journald cap, venv, security-only unattended upgrades without auto-reboot.
4. **Operator to-dos (from the handoff):** production read-only Kalshi key; R2 bucket + write-only
   token; healthchecks.io checks; the Pay As You Go check above.
5. **Recorder:** finish `recorder/` on `desk/recorder`, soak one hour on the Mac, then deploy with
   `deploy.sh` (git pull to `/opt`, `systemctl restart`), and the day-one test: an hour of
   `orderbook_delta` from the datacenter IP.
