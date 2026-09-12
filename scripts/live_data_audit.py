#!/usr/bin/env python3
"""Phase 1: is the recorded data trustworthy enough to do research on?

Runs on the raw JSONL, not on features, because the question is whether the inputs are sound.
Emits reports/live_data_audit.md with an explicit VERDICT: PASS or VERDICT: FAIL on the first
page. The pipeline reads that line and will not unlock later stages without a PASS.

Bad periods are marked invalid, never silently repaired. A quietly patched hour is worse than a
missing one because nothing downstream can tell it was patched.

  python3 scripts/live_data_audit.py
  python3 scripts/live_data_audit.py --min-hours 4     # for a dry run before 24h exists
"""
import argparse, glob, gzip, json, math, os, statistics, sys
from collections import defaultdict, Counter
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RAW = os.path.join(ROOT, 'data', 'raw')
REPORTS = os.path.join(ROOT, 'reports')

# A feed silent for longer than this is stale. Same limits the feature engine enforces.
STALE_US = {'kalshi': 5_000_000, 'cf': 10_000_000, 'external': 15_000_000}
# Fail conditions. Chosen before looking at the data.
MAX_GAP_PCT = 1.0          # sequence gaps as a share of deltas
MAX_STALE_PCT = 20.0       # share of the sample with a stale feed
MAX_BASIS_BPS = 200.0      # a persistent basis larger than this means a scaling error


def pct(xs, q):
    if not xs: return float('nan')
    s = sorted(xs); return s[min(len(s) - 1, max(0, int(round(q * (len(s) - 1)))))]


def describe(xs):
    if not xs:
        return dict(n=0, missing=100.0, mean=None, std=None, p01=None, p05=None, med=None,
                    p95=None, p99=None, lo=None, hi=None)
    return dict(n=len(xs), missing=0.0, mean=statistics.mean(xs),
                std=statistics.pstdev(xs) if len(xs) > 1 else 0.0,
                p01=pct(xs, .01), p05=pct(xs, .05), med=pct(xs, .5),
                p95=pct(xs, .95), p99=pct(xs, .99), lo=min(xs), hi=max(xs))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--raw', default=RAW)
    ap.add_argument('--min-hours', type=int, default=24)
    ap.add_argument('--out', default=os.path.join(REPORTS, 'live_data_audit.md'))
    ap.add_argument('--csv', default=os.path.join(REPORTS, 'live_data_audit.csv'))
    a = ap.parse_args()

    files = sorted(glob.glob(os.path.join(a.raw, 'source=*', 'hour=*', '*.jsonl.gz')))
    if not files:
        sys.exit("no recorder data")
    hours = sorted({os.path.basename(os.path.dirname(f)).split('=', 1)[1] for f in files})
    if len(hours) < a.min_hours:
        print(f"only {len(hours)}h of data, need {a.min_hours}h. Pass --min-hours to override "
              f"for a dry run; the verdict is then advisory.")

    # -- pass 1: counts, sequencing, latency, staleness ----------------------------------------
    per_mkt = defaultdict(lambda: defaultdict(int))
    lat = defaultdict(list)                 # source -> receive latency us
    last_seq, gaps, dupes, ooo = {}, defaultdict(int), defaultdict(int), defaultdict(int)
    seen = defaultdict(set)
    feed_last = {}                          # feed key -> last recv_ts_us
    stale_us = defaultdict(int)
    prices = defaultdict(list)              # for the basis check
    ev_total = Counter()
    hour_counts = defaultdict(Counter)
    recv_lo = recv_hi = None      # the true span of the sample, for every rate and share below

    for path in files:
        src = os.path.basename(os.path.dirname(os.path.dirname(path))).split('=', 1)[1]
        hour = os.path.basename(os.path.dirname(path)).split('=', 1)[1]
        with gzip.open(path, 'rt') as fh:
            for line in fh:
                try:
                    e = json.loads(line)
                except Exception:
                    ev_total['unparseable'] += 1
                    continue
                ev_total[src] += 1
                hour_counts[hour][src] += 1
                typ = e.get('ev', '')
                recv, srcts = e.get('recv_ts_us'), e.get('src_ts_us')
                if recv:
                    recv_lo = recv if recv_lo is None else min(recv_lo, recv)
                    recv_hi = recv if recv_hi is None else max(recv_hi, recv)
                if recv and srcts:
                    d = recv - srcts
                    if -60_000_000 < d < 60_000_000:
                        lat[src].append(d)
                if src == 'kalshi_margin':
                    m = e.get('msg') or {}
                    t = m.get('market_ticker') or '?'
                    per_mkt[t][typ] += 1
                    if typ == '_gap':
                        gaps[t] += 1
                    seq = e.get('seq')
                    if isinstance(seq, int):
                        if typ == 'orderbook_snapshot':
                            last_seq[t] = seq; seen[t] = {seq}
                        elif typ == 'orderbook_delta':
                            if seq in seen[t]:
                                dupes[t] += 1
                            elif t in last_seq and seq <= last_seq[t]:
                                ooo[t] += 1
                            elif t in last_seq and seq != last_seq[t] + 1:
                                gaps[t] += 1
                                last_seq[t] = seq
                            else:
                                last_seq[t] = seq
                            seen[t].add(seq)
                            if len(seen[t]) > 8192:
                                seen[t] = set(sorted(seen[t])[-4096:])
                    if typ in ('orderbook_snapshot', 'orderbook_delta'):
                        key = f'kalshi:{t}'
                        if key in feed_last and recv:
                            d = recv - feed_last[key]
                            if d > STALE_US['kalshi']:
                                stale_us[key] += d
                        if recv: feed_last[key] = recv
                elif src == 'kalshi_index':
                    m = e.get('msg') or {}
                    idx = m.get('index_id')
                    if idx:
                        key = f'cf:{idx}'
                        if key in feed_last and recv:
                            d = recv - feed_last[key]
                            if d > STALE_US['cf']: stale_us[key] += d
                        if recv: feed_last[key] = recv
                        try: prices[key].append((recv, float(m.get('value_usd'))))
                        except (TypeError, ValueError): pass
                elif src.startswith('ext_'):
                    m = e.get('msg') or {}
                    if isinstance(m, dict):
                        sym = m.get('product_id') or m.get('symbol') or m.get('s') or m.get('coin')
                        px = m.get('price') or m.get('last') or m.get('c') or m.get('b')
                        if sym:
                            key = f'{src[4:]}:{sym}'
                            if key in feed_last and recv:
                                d = recv - feed_last[key]
                                if d > STALE_US['external']: stale_us[key] += d
                            if recv: feed_last[key] = recv
                            try: prices[key].append((recv, float(px)))
                            except (TypeError, ValueError): pass

    span_us = max(1, (recv_hi - recv_lo) if (recv_hi and recv_lo) else 1)

    # -- price alignment: the check that catches a contract_size error -------------------------
    CS = {'KXBTCPERP': 0.0001, 'KXETHPERP': 0.001}
    CF_OF = {'KXBTCPERP': 'cf:BRTI', 'KXETHPERP': 'cf:ETHUSD_RTI'}
    EXT_OF = {'KXBTCPERP': ['coinbase:BTC-USD', 'binance:BTCUSDT'],
              'KXETHPERP': ['coinbase:ETH-USD', 'binance:ETHUSDT']}
    align = {}
    for t, cs in CS.items():
        cfk = CF_OF[t]
        cfp = [p for _, p in prices.get(cfk, [])]
        row = {'cf_n': len(cfp), 'cf_median': (statistics.median(cfp) if cfp else None)}
        for ek in EXT_OF[t]:
            ep = [p for _, p in prices.get(ek, [])]
            row[ek] = statistics.median(ep) if ep else None
            if cfp and ep:
                row[f'{ek}_vs_cf_bps'] = (row[ek] / row['cf_median'] - 1) * 1e4
        align[t] = row

    # -- verdict --------------------------------------------------------------------------------
    fails, warns = [], []
    total_deltas = sum(per_mkt[t].get('orderbook_delta', 0) for t in per_mkt)
    total_gaps = sum(gaps.values())
    gap_pct = total_gaps / total_deltas * 100 if total_deltas else 0.0
    if gap_pct > MAX_GAP_PCT:
        fails.append(f"sequence gaps are {gap_pct:.2f}% of deltas, over the {MAX_GAP_PCT}% limit")
    worst_stale = max(((k, min(100.0, v / span_us * 100)) for k, v in stale_us.items()),
                      key=lambda p: p[1], default=(None, 0.0))
    if worst_stale[1] > MAX_STALE_PCT:
        fails.append(f"{worst_stale[0]} was stale for {worst_stale[1]:.0f}% of the sample")
    for t, row in align.items():
        for k, v in row.items():
            if k.endswith('_vs_cf_bps') and v is not None and abs(v) > MAX_BASIS_BPS:
                fails.append(f"{t}: {k} is {v:+.0f} bps, which is a scaling error, not a basis")
    if len(hours) < a.min_hours:
        warns.append(f"only {len(hours)}h of data against a {a.min_hours}h requirement; "
                     f"this verdict is advisory")
    if ev_total.get('unparseable'):
        warns.append(f"{ev_total['unparseable']} unparseable lines")
    verdict = 'FAIL' if fails else 'PASS'

    os.makedirs(REPORTS, exist_ok=True)
    with open(a.out, 'w') as f:
        w = f.write
        w(f"# Live data quality audit\n\n**VERDICT: {verdict}**\n\n")
        w(f"Generated {datetime.now(timezone.utc):%Y-%m-%d %H:%M} UTC over {len(hours)} hours "
          f"({hours[0]} to {hours[-1]}), {sum(ev_total.values()):,} events.\n\n")
        if fails:
            w("## Failures\n\nLater stages stay locked until these are resolved. Bad periods are "
              "marked invalid, never silently repaired.\n\n")
            for x in fails: w(f"- {x}\n")
            w("\n")
        if warns:
            w("## Warnings\n\n")
            for x in warns: w(f"- {x}\n")
            w("\n")
        w("## Events by source\n\n| source | events | receive latency med / p95 |\n|---|---|---|\n")
        for s in sorted(ev_total):
            l = lat.get(s, [])
            lt = (f"{pct(l,.5)/1000:.0f} / {pct(l,.95)/1000:.0f} ms" if l else "-")
            w(f"| {s} | {ev_total[s]:,} | {lt} |\n")
        w("\n## Kalshi book integrity, per market\n\n")
        w("| market | snapshots | deltas | gaps | gap % | duplicates | out of order |\n"
          "|---|---|---|---|---|---|---|\n")
        for t in sorted(per_mkt):
            d = per_mkt[t].get('orderbook_delta', 0)
            g = gaps.get(t, 0)
            w(f"| `{t}` | {per_mkt[t].get('orderbook_snapshot',0):,} | {d:,} | {g} | "
              f"{(g/d*100 if d else 0):.3f}% | {dupes.get(t,0)} | {ooo.get(t,0)} |\n")
        w("\n## Feed staleness\n\n| feed | stale time | share of sample |\n|---|---|---|\n")
        for k in sorted(set(list(feed_last) + list(stale_us))):
            s = stale_us.get(k, 0)
            share = s / span_us * 100
            w(f"| {k} | {s/1e6:,.0f}s | {min(share, 100.0):.1f}%"
              + ("  **accounting error, over 100%**" if share > 100.5 else "") + " |\n")
        w("\n## Price alignment\n\n")
        w("The check that catches a `contract_size` error: an external venue and the CF index "
          "quote the same underlying and must agree to within a few basis points.\n\n")
        for t, row in align.items():
            w(f"**{t}** (contract_size {CS[t]})\n\n")
            w(f"- CF median: {row['cf_median']}\n")
            for k, v in row.items():
                if k.endswith('_vs_cf_bps'):
                    flag = "  **<- IMPOSSIBLE**" if abs(v) > MAX_BASIS_BPS else ""
                    w(f"- {k}: {v:+.1f} bps{flag}\n")
            w("\n")
        w("\n## Hourly coverage\n\n| hour | " + " | ".join(sorted(ev_total)) + " |\n")
        w("|" + "---|" * (1 + len(ev_total)) + "\n")
        for h in hours:
            w(f"| {h} | " + " | ".join(f"{hour_counts[h].get(s,0):,}" for s in sorted(ev_total)) + " |\n")
    print(f"VERDICT: {verdict}")
    for x in fails: print("  FAIL:", x)
    for x in warns: print("  warn:", x)
    print(f"wrote {a.out}")
    return 0 if verdict == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
