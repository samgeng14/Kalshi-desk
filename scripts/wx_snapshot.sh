#!/usr/bin/env bash
# A consistent copy of the desk's database for research to read.
#
# Research must not open the live prod.sqlite. A read-only SQLite connection in WAL mode still
# maps and writes <db>-shm, which was measured here: with the desk stopped, one read-only query
# moved the -shm mtime. Doing that from a different operating system across the bridge mount, while
# the desk writes the same file, is the documented way to get exactly the three errors prod.log
# recorded on 2026-09-12 - "database disk image is malformed", "file is not a database", "disk I/O
# error" - on a file that passes quick_check afterwards.
#
# Uses SQLite's backup API rather than cp, so the copy is consistent even while the desk writes.
set -uo pipefail
cd "$(dirname "$0")/.."
mkdir -p data/snapshots
python3 - <<'PY'
import sqlite3, os, time
t = time.time()
tmp = "data/snapshots/prod-latest.sqlite.tmp"
if os.path.exists(tmp):
    os.remove(tmp)
src = sqlite3.connect("file:data/prod.sqlite?mode=ro", uri=True)
dst = sqlite3.connect(tmp)
src.backup(dst)
dst.close(); src.close()
os.replace(tmp, "data/snapshots/prod-latest.sqlite")
print("snapshot %.0fs %.0f MB" % (time.time() - t, os.path.getsize("data/snapshots/prod-latest.sqlite") / 1e6))
PY
