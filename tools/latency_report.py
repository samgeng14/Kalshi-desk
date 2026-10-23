#!/usr/bin/env python3
"""Where an order's time goes: p50/p90/p99 per segment of deskd's order timelines.

deskd's store writes every ended order's timeline to data/ops.sqlite (table `orders`, plan
section 9: "every order logs its timeline"). The segments, in milliseconds:

  click > recv     the page's click to deskd's web thread (both clocks are this machine's)
  recv > gw        web to gw's admission
  gw > sent        waiting in the sender's queue, signing, writing
  sent > ack       the venue's round trip, wall clock (sent when written, ack when answered)
  wire: write      the sender's start to the request written (a new connection counts here)
  wire: venue      written to the reply's first byte: the venue's processing and the network
  wire: read       first byte to the last
  ack > priv       the private feed's first word on the order after the ack (negative: before)
  ack > fill       a fill after the ack (a take's fill is usually in the ack already)
  click > ack      the whole click
  cx > cx_ack      a cancel's round trip

Read-only. Usage:
  python3 tools/latency_report.py                      # data/ops.sqlite, the last 24 hours
  python3 tools/latency_report.py --hours 2 --own SYS  # Systematic's, the last 2 hours
  python3 tools/latency_report.py --db PATH --json
  python3 tools/latency_report.py --self-test
"""
import argparse
import json
import math
import os
import sqlite3
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# (label, SQL for the value in ms); NULL when the order has no such segment
SEGMENTS = [
    ("click > recv", "recv_ms - click_ms"),
    ("recv > gw", "gw_ms - recv_ms"),
    ("gw > sent", "sent_ms - gw_ms"),
    ("sent > ack", "ack_ms - sent_ms"),
    ("wire: write", "w_ms"),
    ("wire: venue", "fb_ms"),
    ("wire: read", "rd_ms"),
    ("ack > priv", "priv_ms - ack_ms"),
    ("ack > fill", "CASE WHEN fill_ms > ack_ms THEN fill_ms - ack_ms END"),
    ("click > ack", "ack_ms - click_ms"),
    ("cx > cx_ack", "cx_ack_ms - cx_ms"),
]


def pct(values, q):
    """Nearest rank: the smallest value with at least q of them at or below it."""
    if not values:
        return None
    v = sorted(values)
    return v[max(0, math.ceil(q * len(v)) - 1)]


def report(con, hours=24.0, own=None, lane=None, now_ms=None):
    now_ms = now_ms if now_ms is not None else int(time.time() * 1000)
    where = ["done_ms >= ?"]
    args = [now_ms - int(hours * 3600 * 1000)]
    if own:
        where.append("own = ?")
        args.append(own)
    if lane:
        where.append("lane = ?")
        args.append(lane)
    w = " AND ".join(where)
    out = {"orders": 0, "states": {}, "segments": []}
    out["orders"], first, last = con.execute(f"SELECT count(*), min(done_ms), max(done_ms) FROM orders WHERE {w}", args).fetchone()
    out["first_ms"], out["last_ms"] = first, last
    for st, n in con.execute(f"SELECT st, count(*) FROM orders WHERE {w} GROUP BY st ORDER BY 2 DESC", args):
        out["states"][st] = n
    for label, expr in SEGMENTS:
        vals = [r[0] for r in con.execute(f"SELECT {expr} FROM orders WHERE {w} AND ({expr}) IS NOT NULL", args)]
        out["segments"].append({
            "segment": label, "n": len(vals),
            "p50": pct(vals, 0.5), "p90": pct(vals, 0.9), "p99": pct(vals, 0.99),
            "max": max(vals) if vals else None,
        })
    return out


def text(r, hours):
    def f(v):
        return "-" if v is None else (f"{v:.1f}" if isinstance(v, float) and not v.is_integer() else f"{v:.0f}")
    lines = []
    span = ""
    if r["orders"]:
        t0 = time.strftime("%Y-%m-%d %H:%M", time.localtime(r["first_ms"] / 1000))
        t1 = time.strftime("%H:%M", time.localtime(r["last_ms"] / 1000))
        span = f", ended {t0} to {t1}"
    lines.append(f"{r['orders']} orders in the last {hours:g} h{span}")
    if r["states"]:
        lines.append("  " + "  ".join(f"{k} {v}" for k, v in r["states"].items()))
    lines.append(f"{'segment (ms)':<14}{'n':>6}{'p50':>9}{'p90':>9}{'p99':>9}{'max':>9}")
    for s in r["segments"]:
        lines.append(f"{s['segment']:<14}{s['n']:>6}{f(s['p50']):>9}{f(s['p90']):>9}{f(s['p99']):>9}{f(s['max']):>9}")
    return "\n".join(lines)


def open_ro(path):
    if not os.path.exists(path):
        sys.exit(f"latency_report: no {path} (deskd writes it once an order ends)")
    return sqlite3.connect(f"file:{path}?mode=ro", uri=True)


SCHEMA = """CREATE TABLE orders(cid TEXT PRIMARY KEY, own TEXT, lane TEXT, st TEXT, click_ms INTEGER,
  recv_ms INTEGER, gw_ms INTEGER, sent_ms INTEGER, ack_ms INTEGER, priv_ms INTEGER, fill_ms INTEGER,
  cx_ms INTEGER, cx_ack_ms INTEGER, done_ms INTEGER, w_ms REAL, fb_ms REAL, rd_ms REAL, json TEXT)"""


def self_test():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "ops.sqlite")
        con = sqlite3.connect(path)
        con.execute(SCHEMA)
        now = 1_791_000_000_000
        # 100 Manual orders: sent > ack is 1..100 ms; the clicks have no stamp on every other one
        for i in range(1, 101):
            click = now - 60_000 if i % 2 else None
            con.execute("INSERT INTO orders VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                        (f"M{i}", "MAN", None, "FILL" if i <= 70 else "CXL", click, now - 59_990, now - 59_989,
                         now - 59_980, now - 59_980 + i, now - 59_980 + i - 2, None,
                         None if i <= 70 else now - 50_000, None if i <= 70 else now - 49_990,
                         now - 40_000, 0.5, i - 1.5, 0.2, "{}"))
        # one Systematic order, and one that ended two days ago
        con.execute("INSERT INTO orders(cid,own,lane,st,sent_ms,ack_ms,done_ms,json) VALUES('S1','SYS','M1','CXL',?,?,?,'{}')",
                    (now - 1000, now - 990, now - 900))
        con.execute("INSERT INTO orders(cid,own,st,sent_ms,ack_ms,done_ms,json) VALUES('Mold','MAN','FILL',1,999,?, '{}')",
                    (now - 48 * 3600 * 1000,))
        con.commit()
        con.close()
        r = report(open_ro(path), hours=24, now_ms=now)
        seg = {s["segment"]: s for s in r["segments"]}
        assert r["orders"] == 101, r["orders"]
        assert r["states"] == {"FILL": 70, "CXL": 31}, r["states"]
        sa = seg["sent > ack"]
        # 1..100 and the Systematic order's 10: 101 values; nearest rank
        assert sa["n"] == 101 and sa["p50"] == 50 and sa["p90"] == 90 and sa["p99"] == 99 and sa["max"] == 100, sa
        assert seg["click > recv"]["n"] == 50 and seg["click > recv"]["p50"] == 10, seg["click > recv"]
        assert seg["ack > priv"]["p50"] == -2, seg["ack > priv"]
        assert seg["cx > cx_ack"]["n"] == 30 and seg["cx > cx_ack"]["p99"] == 10, seg["cx > cx_ack"]
        assert seg["ack > fill"]["n"] == 0 and seg["ack > fill"]["p50"] is None
        assert abs(seg["wire: venue"]["p50"] - 48.5) < 1e-9, seg["wire: venue"]
        sys_only = report(open_ro(path), hours=24, own="SYS", now_ms=now)
        assert sys_only["orders"] == 1 and sys_only["segments"][3]["p50"] == 10
        assert report(open_ro(path), hours=100, now_ms=now)["orders"] == 102
        out = text(r, 24)
        assert "101 orders" in out and "sent > ack" in out, out
        assert pct([], 0.5) is None and pct([7], 0.99) == 7 and pct([1, 2, 3, 4], 0.5) == 2
    print("latency_report self-test: ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--db", default=os.path.join(ROOT, "data", "ops.sqlite"))
    ap.add_argument("--hours", type=float, default=24.0)
    ap.add_argument("--own", choices=["MAN", "SYS"])
    ap.add_argument("--lane")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    r = report(open_ro(a.db), hours=a.hours, own=a.own, lane=a.lane)
    print(json.dumps(r, indent=1) if a.json else text(r, a.hours))


if __name__ == "__main__":
    main()
