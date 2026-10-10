#!/usr/bin/env python3
"""Archive the old desk's SQLite database to zstd Parquet on Cloudflare R2 (plan section 7).

    python3 tools/archive_legacy.py plan    --db data/prod.sqlite
    python3 tools/archive_legacy.py convert --db data/prod.sqlite --out ~/kalshi-archive/prod
    python3 tools/archive_legacy.py upload  --out ~/kalshi-archive/prod --env ~/.r2/kalshi-desk-archive.env

Needs: python3 -m pip install pyarrow boto3   (in a venv; no DuckDB extension download).

Safety:
  - the database is opened read-only (SQLite URI mode=ro); nothing in it is changed
  - it prints table names, counts, sizes and checksums only, never a row's contents
  - it deletes nothing, locally or on R2; deleting the local copy after 30 days is the operator's call
  - convert refuses to finish a table whose Parquet row count differs from the source count
  - upload skips objects already on R2 with the same size and sha256, so it can be re-run after a
    failure, and writes manifest.json last, so a manifest on R2 means every file it lists is there

Layout: <out>/<table>/day=YYYY-MM-DD/part-0.parquet (UTC day of the table's time column;
day=unknown for rows without a usable time; tables without a time column go to day=all), and
<out>/manifest.json with rows, bytes, sha256 and time range per file. On R2 the same paths sit
under --prefix (default archive/legacy/prod/).

Not covered: re-parsing 2026-09-14..17 from data/raw (those archive rows have zero prices); that
needs the raw format and is a separate step.
"""
import argparse
import datetime as dt
import hashlib
import json
import os
import shutil
import sqlite3
import subprocess
import sys
import time

TIME_COLUMNS = ("ts", "ts_ms", "time_ms", "created_ts", "created_ms", "ts_s", "time", "timestamp",
                "created", "updated_ts", "updated_ms")
BATCH = 100_000


def log(msg):
    print(f"{dt.datetime.now(dt.timezone.utc):%H:%M:%SZ} {msg}", flush=True)


def connect(db):
    if not os.path.isfile(db):
        sys.exit(f"no database at {db}")
    return sqlite3.connect(f"file:{os.path.abspath(db)}?mode=ro", uri=True)


def qi(name):
    return '"' + name.replace('"', '""') + '"'


def tables(con):
    rows = con.execute("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' "
                       "ORDER BY name").fetchall()
    return [r[0] for r in rows]


def columns(con, table):
    return [r[1] for r in con.execute(f"PRAGMA table_info({qi(table)})").fetchall()]


def observed_types(con, table, cols):
    """One scan: how many values of each storage class every column holds."""
    parts = []
    for c in cols:
        for k in ("integer", "real", "text", "blob"):
            parts.append(f"sum(typeof({qi(c)})='{k}')")
    row = con.execute(f"SELECT count(*), {', '.join(parts)} FROM {qi(table)}").fetchone()
    total, rest = row[0], row[1:]
    out = {}
    for i, c in enumerate(cols):
        n = [v or 0 for v in rest[i * 4:(i + 1) * 4]]
        out[c] = dict(zip(("integer", "real", "text", "blob"), n))
    return total, out


def arrow_type(counts):
    import pyarrow as pa
    present = {k for k, v in counts.items() if v}
    if not present:
        return pa.string(), "null"
    if present == {"integer"}:
        return pa.int64(), "int64"
    if present <= {"integer", "real"}:
        return pa.float64(), "float64"
    if present == {"blob"}:
        return pa.binary(), "binary"
    return pa.string(), "string"   # text, or a mix: kept exactly as text


def coerce(v, kind):
    if v is None:
        return None
    if kind == "float64":
        return float(v)
    if kind == "string":
        if isinstance(v, bytes):
            return v.hex()
        return v if isinstance(v, str) else repr(v) if isinstance(v, float) else str(v)
    return v


def epoch_unit(v):
    a = abs(v)
    if a >= 1e17:
        return 1e9
    if a >= 1e14:
        return 1e6
    if a >= 1e11:
        return 1e3
    return 1.0


def day_of(v):
    """UTC day of a time value: epoch s/ms/us/ns (by magnitude) or an ISO date string."""
    if v is None:
        return "unknown", None
    if isinstance(v, (int, float)):
        if v <= 0:
            return "unknown", None
        ms = int(v / epoch_unit(v) * 1000)
        try:
            return dt.datetime.fromtimestamp(ms / 1000, dt.timezone.utc).strftime("%Y-%m-%d"), ms
        except (OverflowError, OSError, ValueError):
            return "unknown", None
    if isinstance(v, str) and len(v) >= 10:
        try:
            d = dt.date.fromisoformat(v[:10])
            ms = int(dt.datetime(d.year, d.month, d.day, tzinfo=dt.timezone.utc).timestamp() * 1000)
            return d.isoformat(), ms
        except ValueError:
            pass
    return "unknown", None


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def old_desk_running():
    try:
        return subprocess.run(["pgrep", "-x", "kalshi-desk"], capture_output=True).returncode == 0
    except FileNotFoundError:
        return False


def cmd_plan(a):
    con = connect(a.db)
    size = os.path.getsize(a.db)
    log(f"{a.db}: {size / 1e9:.2f} GB")
    for t in tables(con):
        cols = columns(con, t)
        n = con.execute(f"SELECT count(*) FROM {qi(t)}").fetchone()[0]
        tc = next((c for c in TIME_COLUMNS if c in cols), None)
        log(f"  {t}: {n:,} rows, {len(cols)} columns, split by {tc or '(none: one file)'}")
    if old_desk_running():
        log("WARNING: kalshi-desk is running; stop it before convert")


def cmd_convert(a):
    import pyarrow as pa
    import pyarrow.parquet as pq
    if old_desk_running() and not a.allow_running:
        sys.exit("kalshi-desk is running: stop it first (the plan converts with the old desk stopped)")
    con = connect(a.db)
    out = os.path.expanduser(a.out)
    os.makedirs(out, exist_ok=True)
    free = shutil.disk_usage(out).free
    if free < os.path.getsize(a.db) * 0.6:
        sys.exit(f"only {free / 1e9:.1f} GB free at {out}; need about 60% of the database size to be safe")
    manifest_path = os.path.join(out, "manifest.json")
    manifest = {"source": os.path.basename(a.db), "source_bytes": os.path.getsize(a.db),
                "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"), "tables": {}}
    if os.path.exists(manifest_path):
        with open(manifest_path) as f:
            manifest["tables"] = json.load(f).get("tables", {})
    wanted = set(a.table or [])
    for t in tables(con):
        if wanted and t not in wanted:
            continue
        if t in manifest["tables"] and manifest["tables"][t].get("verified") and not a.redo:
            log(f"{t}: already converted and verified, skipped (--redo to convert again)")
            continue
        t0 = time.time()
        cols = columns(con, t)
        total, seen = observed_types(con, t, cols)
        kinds, fields = {}, []
        for c in cols:
            typ, kinds[c] = arrow_type(seen[c])
            fields.append(pa.field(c, typ))
        schema = pa.schema(fields)
        tc = next((c for c in TIME_COLUMNS if c in cols), None)
        tdir = os.path.join(out, t)
        if os.path.isdir(tdir):
            for root, _, files in os.walk(tdir):   # only our own partial output from an earlier run
                for fn in files:
                    if fn.endswith(".parquet"):
                        os.remove(os.path.join(root, fn))
        writers, stats = {}, {}
        cur = con.execute(f"SELECT {', '.join(qi(c) for c in cols)} FROM {qi(t)}")
        ti = cols.index(tc) if tc else None
        done = 0
        while True:
            rows = cur.fetchmany(BATCH)
            if not rows:
                break
            groups = {}
            for r in rows:
                if ti is None:
                    day, ms = "all", None
                else:
                    day, ms = day_of(r[ti])
                groups.setdefault(day, []).append(r)
                s = stats.setdefault(day, {"rows": 0, "min_ms": None, "max_ms": None})
                s["rows"] += 1
                if ms is not None:
                    s["min_ms"] = ms if s["min_ms"] is None else min(s["min_ms"], ms)
                    s["max_ms"] = ms if s["max_ms"] is None else max(s["max_ms"], ms)
            for day, rs in groups.items():
                arrays = [pa.array([coerce(r[i], kinds[c]) for r in rs], type=schema.field(i).type)
                          for i, c in enumerate(cols)]
                batch = pa.record_batch(arrays, schema=schema)
                if day not in writers:
                    d = os.path.join(tdir, f"day={day}")
                    os.makedirs(d, exist_ok=True)
                    writers[day] = pq.ParquetWriter(os.path.join(d, "part-0.parquet"), schema,
                                                    compression="zstd", compression_level=6)
                writers[day].write_batch(batch)
            done += len(rows)
            if done % (BATCH * 20) == 0:
                log(f"  {t}: {done:,} / {total:,} rows")
        for w in writers.values():
            w.close()
        if not writers:   # an empty table still gets one file, so the schema is archived
            d = os.path.join(tdir, "day=all")
            os.makedirs(d, exist_ok=True)
            pq.write_table(schema.empty_table(), os.path.join(d, "part-0.parquet"), compression="zstd")
            stats["all"] = {"rows": 0, "min_ms": None, "max_ms": None}
        files, written = [], 0
        for day in sorted(stats):
            p = os.path.join(tdir, f"day={day}", "part-0.parquet")
            n = pq.read_metadata(p).num_rows
            if n != stats[day]["rows"]:
                sys.exit(f"{t} day={day}: wrote {n} rows, expected {stats[day]['rows']}; stopped")
            written += n
            files.append({"path": os.path.relpath(p, out), "day": day, "rows": n, "bytes": os.path.getsize(p),
                          "sha256": sha256(p), "min_ms": stats[day]["min_ms"], "max_ms": stats[day]["max_ms"]})
        if written != total:
            sys.exit(f"{t}: Parquet holds {written} rows, the database {total}; stopped")
        manifest["tables"][t] = {"rows": total, "time_column": tc, "verified": True,
                                 "columns": {c: kinds[c] for c in cols},
                                 "mixed_columns": [c for c in cols if sum(1 for v in seen[c].values() if v) > 1],
                                 "files": files}
        with open(manifest_path + ".tmp", "w") as f:
            json.dump(manifest, f, indent=1)
        os.replace(manifest_path + ".tmp", manifest_path)
        size = sum(x["bytes"] for x in files)
        log(f"{t}: {total:,} rows -> {len(files)} files, {size / 1e6:.1f} MB, verified ({time.time() - t0:.0f} s)")
    total_bytes = sum(x["bytes"] for tb in manifest["tables"].values() for x in tb["files"])
    log(f"done: {len(manifest['tables'])} tables, {total_bytes / 1e9:.2f} GB Parquet "
        f"from {manifest['source_bytes'] / 1e9:.2f} GB SQLite; manifest at {manifest_path}")


def read_env(path):
    env = {}
    with open(os.path.expanduser(path)) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                env[k.strip()] = v.strip()
    missing = [k for k in ("R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY", "R2_ENDPOINT", "R2_BUCKET") if not env.get(k)]
    if missing:
        sys.exit(f"{path} lacks {', '.join(missing)}")
    mode = os.stat(os.path.expanduser(path)).st_mode & 0o077
    if mode:
        log(f"WARNING: {path} is readable by others; run chmod 600 on it")
    return env


def cmd_upload(a):
    import boto3
    from botocore.config import Config
    from botocore.exceptions import ClientError
    out = os.path.expanduser(a.out)
    with open(os.path.join(out, "manifest.json")) as f:
        manifest = json.load(f)
    env = read_env(a.env)
    s3 = boto3.client("s3", endpoint_url=env["R2_ENDPOINT"], aws_access_key_id=env["R2_ACCESS_KEY_ID"],
                      aws_secret_access_key=env["R2_SECRET_ACCESS_KEY"], region_name="auto",
                      config=Config(retries={"max_attempts": 5, "mode": "standard"}))
    bucket, prefix = env["R2_BUCKET"], a.prefix.strip("/") + "/"
    try:
        s3.head_bucket(Bucket=bucket)
    except ClientError as e:
        sys.exit(f"cannot reach bucket {bucket}: {e.response.get('Error', {}).get('Code', e)} "
                 "(check the endpoint, keys and bucket name)")
    sent = skipped = 0
    for t, tb in manifest["tables"].items():
        if not tb.get("verified"):
            sys.exit(f"{t} is not verified in the manifest; run convert again")
        for x in tb["files"]:
            p = os.path.join(out, x["path"])
            if os.path.getsize(p) != x["bytes"] or (a.recheck and sha256(p) != x["sha256"]):
                sys.exit(f"{x['path']} changed since convert; stopped")
            key = prefix + x["path"]
            try:
                h = s3.head_object(Bucket=bucket, Key=key)
                if h["ContentLength"] == x["bytes"] and h.get("Metadata", {}).get("sha256") == x["sha256"]:
                    skipped += 1
                    continue
            except ClientError as e:
                if e.response.get("Error", {}).get("Code") not in ("404", "NoSuchKey", "NotFound"):
                    raise
            s3.upload_file(p, bucket, key, ExtraArgs={"Metadata": {"sha256": x["sha256"], "rows": str(x["rows"])},
                                                      "ContentType": "application/vnd.apache.parquet"})
            h = s3.head_object(Bucket=bucket, Key=key)
            if h["ContentLength"] != x["bytes"]:
                sys.exit(f"{key}: R2 holds {h['ContentLength']} bytes, expected {x['bytes']}; stopped")
            sent += 1
            log(f"  {key} ({x['bytes'] / 1e6:.1f} MB)")
    s3.upload_file(os.path.join(out, "manifest.json"), bucket, prefix + "manifest.json",
                   ExtraArgs={"ContentType": "application/json"})
    log(f"done: {sent} uploaded, {skipped} already there; manifest at s3://{bucket}/{prefix}manifest.json")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("plan", help="list tables, row counts and how each will be split")
    p.add_argument("--db", required=True)
    c = sub.add_parser("convert", help="write verified zstd Parquet and manifest.json")
    c.add_argument("--db", required=True)
    c.add_argument("--out", required=True)
    c.add_argument("--table", action="append", help="only this table (repeatable)")
    c.add_argument("--redo", action="store_true", help="convert tables already verified again")
    c.add_argument("--allow-running", action="store_true", help=argparse.SUPPRESS)
    u = sub.add_parser("upload", help="upload the converted files to R2, then the manifest")
    u.add_argument("--out", required=True)
    u.add_argument("--env", default="~/.r2/kalshi-desk-archive.env")
    u.add_argument("--prefix", default="archive/legacy/prod")
    u.add_argument("--recheck", action="store_true", help="re-hash every file before upload")
    a = ap.parse_args()
    {"plan": cmd_plan, "convert": cmd_convert, "upload": cmd_upload}[a.cmd](a)


if __name__ == "__main__":
    main()
