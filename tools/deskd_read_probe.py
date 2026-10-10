#!/usr/bin/env python3
"""Read-only venue verification through deskd's actual HTTP/SSE contract.

Only loopback is accepted. The sole POST changes this probe's book subscription; it never
places, cancels, amends, transfers, starts, stops or kills anything at an exchange.
The per-start X-Desk token stays in memory and is never written to the capture.
"""
import argparse
import json
import re
import shutil
import subprocess
import time
import urllib.parse
import urllib.request
import uuid
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--require-private", action="store_true",
                        help="also require a subscribed, reconciled private account feed")
    parser.add_argument("--require-fees", action="store_true",
                        help="also require fresh venue series fees for the live watchlist")
    args = parser.parse_args()
    url = urllib.parse.urlsplit(args.base)
    if url.scheme != "http" or url.hostname not in {"127.0.0.1", "localhost"} or not url.port or url.username or url.password:
        parser.error("--base must be an HTTP loopback deskd port")
    base = args.base.rstrip("/")

    def get(path):
        with urllib.request.urlopen(base + path, timeout=8) as response:
            return json.load(response)

    with urllib.request.urlopen(base + "/", timeout=8) as response:
        page = response.read().decode()
    match = re.search(r'<meta\s+name="desk-token"\s+content="([a-f0-9]+)"', page)
    if not match:
        raise RuntimeError("deskd page lacks its per-start token")
    token = match.group(1)
    sid = "probe-" + uuid.uuid4().hex[:16]
    capture = {"snapshots": [], "events": [], "responses": []}
    with urllib.request.urlopen(base + "/api/stream?sid=" + sid, timeout=8) as stream:
        while True:
            line = stream.readline().decode().strip()
            if line.startswith("data:"):
                capture["events"].append(json.loads(line[5:]))
                break
        end = time.monotonic() + 40
        while True:
            snapshot = get("/api/snapshot?sid=" + sid)
            feeds = ("KX", "KXP") if args.require_private else ("KX",)
            status = get("/ops/status") if args.require_fees else {}
            fee_state = status.get("rest", {})
            fees_ready = not args.require_fees or (fee_state.get("live", 0) > 0 and
                                                  fee_state.get("fee_ready_live") == fee_state["live"])
            if snapshot["markets"] and fees_ready and all(snapshot["feeds"][name]["up"] and
                                           not snapshot["feeds"][name]["gap"] for name in feeds):
                break
            if time.monotonic() >= end:
                raise RuntimeError("demo feeds/fees did not become ready: " + ", ".join(feeds))
            time.sleep(0.2)
        ticker = snapshot["markets"][0]["tk"]
        body = {"sid": sid, "ladders": [{"v": "K", "tk": ticker}], "tops": "ALL"}
        request = urllib.request.Request(base + "/api/subscribe", json.dumps(body).encode(),
            {"X-Desk": token, "Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(request, timeout=8) as response:
            capture["responses"].append({"key": "POST /api/subscribe", "status": response.status, "json": json.load(response)})
        snapshot = get("/api/snapshot?sid=" + sid)
        if not snapshot["books"] or not any(book["tk"] == ticker and book["depth"] == "FULL" for book in snapshot["books"]):
            raise RuntimeError("subscribed ladder is absent from the snapshot")
        capture["snapshots"].append(snapshot)
        for day in ("T", "T1"):
            reply = get("/api/universe?day=" + day + "&limit=20")
            capture["responses"].append({"key": "GET /api/universe", "status": 200, "json": reply})
    args.out.write_text(json.dumps(capture))
    node = shutil.which("node")
    if not node:
        raise RuntimeError("node is needed to validate contract 1.5")
    checker = Path(__file__).resolve().parents[1] / "tests/deskd/contract_check.cjs"
    subprocess.run([node, str(checker), str(args.out)], check=True)
    print(json.dumps({"mode": snapshot["hdr"]["mode"], "markets": len(snapshot["markets"]),
                      "books": len(snapshot["books"]), "feeds": snapshot["feeds"],
                      "clock": snapshot["hdr"]["lat"], "orders_sent": 0}, indent=2))
    if args.require_fees:
        print(json.dumps({"fee_ready_live": fee_state["fee_ready_live"], "live": fee_state["live"]}))


if __name__ == "__main__":
    main()
