#!/usr/bin/env python3
"""Tiny read-only Kalshi query helper for research: kq.py <path> [k=v ...]. GET only."""
import base64, json, os, sys, time, urllib.parse, urllib.request
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding

BASE = os.environ.get("KALSHI_BASE", "https://external-api.kalshi.com/trade-api/v2")
KEY_ID = os.environ.get("KALSHI_KEY_ID", "")
PEM = os.path.expanduser(os.environ.get("KALSHI_PEM", "~/.kalshi/sam.pem"))


def headers(method, path):
    if not KEY_ID:
        return {"User-Agent": "kalshi-desk-research/0.1"}
    with open(PEM, "rb") as f:
        key = serialization.load_pem_private_key(f.read(), password=None)
    ts = str(int(time.time() * 1000))
    msg = (ts + method + "/trade-api/v2" + path).encode()
    sig = key.sign(msg, padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=padding.PSS.DIGEST_LENGTH), hashes.SHA256())
    return {"KALSHI-ACCESS-KEY": KEY_ID, "KALSHI-ACCESS-TIMESTAMP": ts,
            "KALSHI-ACCESS-SIGNATURE": base64.b64encode(sig).decode(), "User-Agent": "kalshi-desk-research/0.1"}


def get(path, **params):
    q = urllib.parse.urlencode({k: v for k, v in params.items() if v is not None})
    url = BASE + path + ("?" + q if q else "")
    req = urllib.request.Request(url, headers=headers("GET", path))
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


if __name__ == "__main__":
    path = sys.argv[1]
    params = dict(a.split("=", 1) for a in sys.argv[2:])
    print(json.dumps(get(path, **params), indent=1))
