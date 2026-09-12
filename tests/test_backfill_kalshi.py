"""Offline test for scripts/backfill_kalshi_history.py.

The network is mocked, so this checks the parts that were actually wrong rather than the parts
that need an exchange:
  - Kalshi renamed fields (volume -> volume_fp, last_price -> last_price_dollars). Reading a name
    that no longer exists returns None, and treating that as zero records a live market as dead.
  - Trade ids are not documented as unique across markets. A global primary key silently dropped
    every trade whose id repeated in another market; the key is (ticker, trade_id).
  - Re-running must resume, not duplicate.

Run: python3 tests/test_backfill_kalshi.py
"""

import importlib.util, os, sqlite3, sys, json, time
spec = importlib.util.spec_from_file_location("bf", os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),"scripts","backfill_kalshi_history.py"))
bf = importlib.util.module_from_spec(spec); spec.loader.exec_module(bf)
import urllib.parse
def fake_get(url, tries=6):
    if '/series?' in url: return {"series":[{"ticker":"KXAAA"},{"ticker":"KXBBB"}]}
    if '/markets?' in url:
        if 'cursor=' in url: return {"markets":[], "cursor":None}
        s = urllib.parse.parse_qs(urllib.parse.urlparse(url).query)['series_ticker'][0]
        return {"markets":[
          {"ticker":f"{s}-1","event_ticker":"E1","title":"t","result":"yes","close_time":"2026-09-01T12:00:00Z",
           "open_time":"2026-09-01T11:00:00Z","expiration_time":"2026-09-01T18:00:00Z",
           "volume_fp":"1234","open_interest_fp":"99","last_price_dollars":"0.97","strike_type":"greater","floor_strike":"100"},
          {"ticker":f"{s}-2","event_ticker":"E1","title":"t2","result":"no","close_time":"2026-09-01T12:00:00Z",
           "volume":"5","open_interest":"1","last_price":"0.02"}], "cursor":None}
    if '/markets/trades' in url:
        if 'cursor=' in url: return {"trades":[], "cursor":None}
        # deliberately reuses trade ids across markets, which is the case the old key dropped
        return {"trades":[
          {"trade_id":"a","created_time":"2026-09-01T11:30:00Z","yes_price_dollars":"0.55","no_price_dollars":"0.45","count_fp":"3","taker_side":"yes"},
          {"trade_id":"b","created_time":"2026-09-01T11:45:00Z","yes_price":"0.61","no_price":"0.39","count":"2","taker_side":"no"}], "cursor":None}
    return {}
bf.get = fake_get
import tempfile
db=os.path.join(tempfile.mkdtemp(),"research.sqlite")
if os.path.exists(db): os.remove(db)
sys.argv=["x","--db",db,"--discover","Crypto","--max-markets","10"]; bf.main()
c=sqlite3.connect(db); ok=True; fails=[]
def chk(name, cond):
    global ok
    if not cond: ok=False; fails.append(name)
    print(f"  {'ok  ' if cond else 'FAIL'} {name}")
m=list(c.execute("select ticker,series_ticker,result,volume,open_interest,last_price from hist_markets order by ticker"))
print()
chk("4 markets across 2 series", len(m)==4)
chk("new *_fp field names read", m[0][3]==1234.0 and m[0][4]==99.0)
chk("legacy field names read",   m[1][3]==5.0)
chk("dollar prices read",        abs(m[0][5]-0.97)<1e-9)
t=list(c.execute("select ticker,trade_id,ts,yes_price from hist_trades order by ticker,trade_id"))
chk("trade ids reused across markets are all kept", len(t)==8)
chk("prices in (0,1), both shapes", all(0<x[3]<1 for x in t))
chk("iso timestamps parsed",       all(x[2] and x[2]>1_700_000_000 for x in t))
n=c.execute("select count(*) from hist_fetch").fetchone()[0]; c.close()
sys.argv=["x","--db",db,"--series","KXAAA","--max-markets","10"]; bf.main()
c=sqlite3.connect(db)
print()
chk("re-run adds no duplicate trades", c.execute("select count(*) from hist_trades").fetchone()[0]==8)
chk("re-run skips already-fetched",    c.execute("select count(*) from hist_fetch").fetchone()[0]==n)
print()
print("PASS" if ok else "FAILED: "+", ".join(fails))
sys.exit(0 if ok else 1)

