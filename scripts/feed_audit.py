#!/usr/bin/env python3
"""Phase 1b -- audit the feed's SEMANTICS, not just its volume.

`live_data_audit.py` checks that the data arrived: counts, gaps, staleness, price alignment. It
passed on synthetic data. The synthetic data was generated to match what the code assumed the
feed looked like, so it could not have failed -- and five real properties of the Kalshi margin
feed went unnoticed until an actual tape was replayed, four of them silently:

  * `last_update_reason` is absent, so every trade and cancel feature read 0.0
  * `taker_side` names the taker, not the side consumed, so every flow sign could invert
  * `seq` counts the subscription, not the market, so per-market gap detection saw 41,799
    phantom gaps in 97,940 deltas and would have marked every book permanently stale
  * snapshots are empty and the book streams in from zero, so depth is wrong for minutes
  * the socket reconnects, restarting every sequence at 1

This file checks each of those AGAINST THE TAPE, and re-derives the ones that are conventions
rather than facts. It does not read the documentation and it does not trust this docstring: if
Kalshi changes `taker_side` tomorrow, the orientation test below reports the change rather than
the code silently inverting.

  python3 scripts/feed_audit.py --all
  python3 scripts/feed_audit.py --hours 2026-09-11T14 2026-09-11T15
"""
import argparse, glob, gzip, json, os, statistics, sys
from collections import Counter, defaultdict, deque

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Fail conditions, fixed before looking at any output.
MAX_SID_GAP_PCT = 0.50        # real frame loss on a subscription
MIN_TAKER_MARGIN = 2.0        # the orientation test must be at least this decisive (ratio)
MIN_TAKER_MATCHES = 200       # ...over at least this many exact-magnitude matches
MAX_CLOCK_SKEW_MS = 250.0     # |median latency|; beyond this, sub-second lead-lag is meaningless
MATCH_US = 300_000


def med(xs):
    return statistics.median(xs) if xs else None


def num(v):
    try:
        f = float(v)
    except (TypeError, ValueError):
        return None
    return None if (f != f or abs(f) >= 1e9) else f


class Audit:
    def __init__(self):
        self.rows = 0
        self.ev = Counter()
        self.msg_keys = Counter()
        self.reason_present = 0
        self.deltas = 0
        self.sid_seq = {}
        self.sid_gap = Counter()
        self.sid_dupe = Counter()
        self.sid_late = Counter()
        self.sid_n = Counter()
        self.connects = []
        self.snap_levels = Counter()
        self.snaps = 0
        self.latency_ms = []
        self.trades = 0
        self.taker_match = Counter()
        self.recent_trades = defaultdict(deque)
        self.recent_deltas = defaultdict(deque)
        self.first_us = None
        self.last_us = None
        self.per_mkt_apparent_gap = Counter()
        self.per_mkt_n = Counter()
        self.per_mkt_prev = {}
        self.warm = {}          # ticker -> [(age_s, nbid, nask)]
        self.bids = defaultdict(dict)
        self.asks = defaultdict(dict)
        self.truncated = []
        self.mkts = set()

    def feed(self, ev):
        self.rows += 1
        typ = ev.get('ev')
        self.ev[typ] += 1
        if typ == '_connect':
            self.connects.append(ev.get('recv_ts_us'))
            self.sid_seq.clear()
            self.per_mkt_prev.clear()
            self.bids.clear()
            self.asks.clear()
            self.recent_trades.clear()
            self.recent_deltas.clear()
            return
        m = ev.get('msg') or {}
        if not isinstance(m, dict):
            return
        for k in m:
            self.msg_keys[k] += 1
        ts = ev.get('src_ts_us') or ev.get('recv_ts_us')
        if ts:
            self.first_us = ts if self.first_us is None else min(self.first_us, ts)
            self.last_us = ts if self.last_us is None else max(self.last_us, ts)
        lat = ev.get('latency_us')
        if isinstance(lat, (int, float)) and len(self.latency_ms) < 400000:
            self.latency_ms.append(lat / 1000.0)

        sid, seq = ev.get('sid'), ev.get('seq')
        if sid is not None and isinstance(seq, int):
            self.sid_n[sid] += 1
            prev = self.sid_seq.get(sid)
            if prev is not None:
                if seq == prev:
                    self.sid_dupe[sid] += 1
                elif seq < prev:
                    self.sid_late[sid] += 1
                elif seq != prev + 1:
                    self.sid_gap[sid] += 1
            self.sid_seq[sid] = seq

        t = m.get('market_ticker')
        if t:
            self.mkts.add(t)
        if typ == 'orderbook_snapshot':
            self.snaps += 1
            self.snap_levels[len(m.get('bid') or []) + len(m.get('ask') or [])] += 1
        elif typ == 'orderbook_delta' and t:
            self.deltas += 1
            if m.get('last_update_reason'):
                self.reason_present += 1
            # The per-market counter, kept ONLY to show how badly it misreads this feed.
            if isinstance(seq, int):
                self.per_mkt_n[t] += 1
                p = self.per_mkt_prev.get(t)
                if p is not None and seq != p + 1:
                    self.per_mkt_apparent_gap[t] += 1
                self.per_mkt_prev[t] = seq
            px, d = num(m.get('price')), num(m.get('delta'))
            if px is None or d is None:
                return
            bk = self.bids[t] if m.get('side') == 'bid' else self.asks[t]
            q = bk.get(px, 0.0) + d
            if q > 1e-9:
                bk[px] = q
            else:
                bk.pop(px, None)
            if d < 0:
                self._on_negative_delta(t, px, ts, m.get('side'), -d)
            if self.connects and ts:
                age = (ts - self.connects[-1]) / 1e6
                w = self.warm.setdefault(t, [])
                if not w or age >= (w[-1][0] * 2 if w[-1][0] > 0 else 0.05):
                    if len(w) < 14:
                        w.append((round(age, 2), len(self.bids[t]), len(self.asks[t])))
        elif typ == 'trade' and t:
            self.trades += 1
            px, cnt = num(m.get('price')), num(m.get('count'))
            if px is None or cnt is None:
                return
            key = (t, round(px, 6))
            taker = m.get('taker_side')
            dq = self.recent_deltas.get(key)
            if dq and ts is not None:
                for i, (dts, dqty, dside) in enumerate(dq):
                    if abs(dts - ts) <= MATCH_US:
                        self._record(taker, dside, cnt, dqty)
                        del dq[i]
                        return
            q = self.recent_trades[key]
            q.append((ts, cnt, taker))
            while q and ts and ts - q[0][0] > MATCH_US:
                q.popleft()

    def _on_negative_delta(self, t, px, ts, side, qty):
        """Pair a negative delta with a trade print, to settle what `taker_side` refers to.

        Only EXACT magnitude matches are used for the verdict. A loose time match pairs plenty of
        unrelated cancels with nearby prints; requiring the size to agree to the contract is what
        makes the answer decisive rather than merely suggestive.
        """
        if ts is None:
            return
        key = (t, round(px, 6))
        q = self.recent_trades.get(key)
        if q:
            for i, (tts, tq, taker) in enumerate(q):
                if abs(tts - ts) <= MATCH_US:
                    self._record(taker, side, tq, qty)
                    del q[i]
                    return
        # No trade print yet. The book and trade channels are separate subscriptions, so the
        # delta routinely arrives FIRST. Buffering it here is what makes the match symmetric;
        # matching in one direction only halves the sample and skews which pairs survive.
        dq = self.recent_deltas[key]
        dq.append((ts, qty, side))
        while dq and ts - dq[0][0] > MATCH_US:
            dq.popleft()

    def _record(self, taker, side, tq, qty):
        self.taker_match[(taker, side, 'any')] += 1
        if tq is not None and abs(tq - qty) < 1e-6:
            self.taker_match[(taker, side, 'exact')] += 1


def iter_file(path, audit):
    try:
        with gzip.open(path, 'rt') as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    yield json.loads(line)
                except json.JSONDecodeError:
                    continue
    except (EOFError, OSError) as e:
        audit.truncated.append((path, str(e)))


def discover_hours(src='kalshi_margin'):
    base = os.path.join(ROOT, 'data', 'raw', f'source={src}')
    return sorted(os.path.basename(p).split('=', 1)[1]
                  for p in glob.glob(os.path.join(base, 'hour=*')))


def report(a, hours, out):
    fails, warns, notes = [], [], []
    L = []
    W = L.append

    # -- 1. reason field --
    reason_pct = 100.0 * a.reason_present / a.deltas if a.deltas else 0.0
    if a.deltas and reason_pct < 1.0:
        notes.append('no last_update_reason: trade/cancel classification MUST come from the '
                     'trade channel, not the delta')
    elif reason_pct < 99.0:
        warns.append(f'last_update_reason present on only {reason_pct:.1f}% of deltas -- a mixed '
                     f'feed, so both classification paths are live at once')

    # -- 2. taker_side orientation, re-derived --
    ex = {k[:2]: v for k, v in a.taker_match.items() if k[2] == 'exact'}
    bid_ask = ex.get(('bid', 'ask'), 0)
    bid_bid = ex.get(('bid', 'bid'), 0)
    ask_bid = ex.get(('ask', 'bid'), 0)
    ask_ask = ex.get(('ask', 'ask'), 0)
    total_ex = bid_ask + bid_bid + ask_bid + ask_ask
    taker_verdict = 'UNDETERMINED'
    if total_ex >= MIN_TAKER_MATCHES:
        r1 = bid_ask / max(bid_bid, 1)
        r2 = ask_bid / max(ask_ask, 1)
        if r1 >= MIN_TAKER_MARGIN and r2 >= MIN_TAKER_MARGIN:
            taker_verdict = "TAKER'S OWN SIDE (taker_side=bid is an aggressive BUYER)"
        elif (1 / max(r1, 1e-9)) >= MIN_TAKER_MARGIN and (1 / max(r2, 1e-9)) >= MIN_TAKER_MARGIN:
            taker_verdict = 'SIDE CONSUMED (taker_side=bid means the BID was hit)'
            fails.append('taker_side now names the side CONSUMED, the opposite of what the code '
                         'assumes -- every flow sign is inverted until this is changed')
        else:
            taker_verdict = 'AMBIGUOUS'
            fails.append(f'taker_side orientation is ambiguous ({bid_ask}/{bid_bid} and '
                         f'{ask_bid}/{ask_ask}); flow signs cannot be trusted')
    else:
        warns.append(f'only {total_ex} exact trade/delta matches: too few to re-derive '
                     f'taker_side, so the stored convention is being taken on faith')

    # -- 3. sequence continuity, both ways --
    tot_gap = sum(a.sid_gap.values())
    tot_seq = sum(a.sid_n.values())
    gap_pct = 100.0 * tot_gap / tot_seq if tot_seq else 0.0
    if gap_pct > MAX_SID_GAP_PCT:
        fails.append(f'{tot_gap:,} real sequence gaps ({gap_pct:.2f}% of frames) on the '
                     f'subscription counters, over the {MAX_SID_GAP_PCT}% limit')
    apparent = sum(a.per_mkt_apparent_gap.values())
    if apparent > tot_gap * 10 + 100:
        notes.append(f'per-market sequence checking would report {apparent:,} gaps where there '
                     f'are {tot_gap:,}: seq counts the SUBSCRIPTION, never the market')

    # -- 4. snapshots --
    empty_snaps = a.snap_levels.get(0, 0)
    if a.snaps:
        if empty_snaps == a.snaps:
            warns.append('EVERY snapshot on this tape is empty. The book has to be rebuilt from '
                         'streamed deltas, so depth is unusable for minutes after each connect')
        elif empty_snaps:
            warns.append(f'{empty_snaps} of {a.snaps} snapshots arrived with no levels. That is '
                         f'a race at subscribe time, not the protocol -- the rest carry full '
                         f'depth. Only the empty ones need a warm-up')
        notes.append('a replay must start at an hour containing a connect: an hour with no '
                     'snapshot in it reconstructs no book at all')

    # -- 5. reconnects and truncation --
    if len(a.connects) > 1:
        notes.append(f'{len(a.connects)} connects: sequences restart at 1 and every book empties '
                     f'at each one')
    for path, err in a.truncated:
        warns.append(f'truncated: {os.path.basename(os.path.dirname(path))} ({err})')

    # -- 6. clock --
    mlat = med(a.latency_ms)
    if mlat is not None and abs(mlat) > MAX_CLOCK_SKEW_MS:
        fails.append(f'median exchange-to-receive latency is {mlat:+.0f} ms; '
                     f'{"negative means the local clock is BEHIND" if mlat < 0 else "the local clock is AHEAD"}. '
                     f'No sub-second lead-lag result is interpretable until this is fixed')
    elif mlat is not None and mlat < 0:
        warns.append(f'median latency {mlat:+.1f} ms is negative: the local clock is behind the '
                     f'exchange, which biases every measured lead')

    verdict = 'FAIL' if fails else 'PASS'
    W(f'# Feed semantics audit\n')
    W(f'**VERDICT: {verdict}**\n')
    W(f'Hours: {", ".join(hours)}  ')
    span = (a.last_us - a.first_us) / 3600e6 if (a.first_us and a.last_us) else 0
    W(f'Rows: {a.rows:,} over {span:.2f} h across {len(a.mkts)} markets\n')

    if fails:
        W('## Failures\n')
        for f in fails:
            W(f'- {f}')
        W('')
    if warns:
        W('## Warnings\n')
        for x in warns:
            W(f'- {x}')
        W('')
    if notes:
        W('## Properties of this feed that the code must match\n')
        for x in notes:
            W(f'- {x}')
        W('')

    W('## Event mix\n')
    for k, v in a.ev.most_common():
        W(f'- `{k}`: {v:,}')
    W('')

    W('## Delta fields\n')
    W(f'`last_update_reason` present on **{a.reason_present:,} of {a.deltas:,}** deltas '
      f'({reason_pct:.2f}%).\n')
    W('Keys seen in message bodies, with counts:\n')
    for k, v in a.msg_keys.most_common(24):
        W(f'- `{k}`: {v:,}')
    W('')

    W('## taker_side orientation, re-derived from the tape\n')
    W(f'Exact magnitude matches between a trade print and a negative delta at the same price '
      f'within {MATCH_US//1000} ms.\n')
    W('| taker_side | consumed the ask | consumed the bid |')
    W('|---|---:|---:|')
    W(f'| `bid` | {bid_ask:,} | {bid_bid:,} |')
    W(f'| `ask` | {ask_ask:,} | {ask_bid:,} |')
    W(f'\n**{taker_verdict}**\n')

    W('## Sequence continuity\n')
    W('| sid | frames | gaps | duplicates | late |')
    W('|---:|---:|---:|---:|---:|')
    for sid in sorted(a.sid_n):
        W(f'| {sid} | {a.sid_n[sid]:,} | {a.sid_gap.get(sid,0):,} | '
          f'{a.sid_dupe.get(sid,0):,} | {a.sid_late.get(sid,0):,} |')
    W(f'\nChecked per MARKET instead, the same tape would show **{apparent:,}** gaps. '
      f'That number is an artifact of interleaving, not loss.\n')

    W('## Snapshots\n')
    W(f'{a.snaps} snapshots: **{empty_snaps} empty**, {a.snaps - empty_snaps} carrying levels.\n')
    if a.snap_levels:
        top = ', '.join(f'{k} levels x{v}' for k, v in a.snap_levels.most_common(6))
        W(f'Level counts seen: {top}\n')

    W('## Book warm-up after a connect\n')
    W('Levels present per side as the streamed book fills in, measured from the FIRST connect on '
      'this tape. Where a snapshot arrived full this does not apply and no warm-up is charged.\n')
    for t in sorted(a.warm)[:3]:
        W(f'\n`{t}`\n')
        W('| s since connect | bid levels | ask levels |')
        W('|---:|---:|---:|')
        for age, nb, na in a.warm[t]:
            W(f'| {age} | {nb} | {na} |')
    W('')

    W('## Clock\n')
    if mlat is None:
        W('- no latency field on this tape')
    else:
        lo = sorted(a.latency_ms)
        n = len(lo)
        W(f'- median exchange-to-receive latency: **{mlat:+.1f} ms** over {n:,} frames')
        W(f'- p05 {lo[int(.05*n)]:+.1f} ms, p95 {lo[int(.95*n)]:+.1f} ms')
        W('- a negative value is not negative latency; it is the local clock running behind')
    W('')

    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w') as f:
        f.write('\n'.join(L) + '\n')
    return verdict, fails, warns, notes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--hours', nargs='*')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--out', default=os.path.join(ROOT, 'reports', 'feed_audit.md'))
    args = ap.parse_args()
    hours = args.hours or (discover_hours() if args.all else [])
    if not hours:
        sys.exit('no hours selected: pass --all or --hours 2026-09-11T14')

    a = Audit()
    n = 0
    for h in hours:
        p = os.path.join(ROOT, 'data', 'raw', 'source=kalshi_margin', f'hour={h}',
                         'events.jsonl.gz')
        if not os.path.exists(p):
            continue
        for ev in iter_file(p, a):
            a.feed(ev)
            n += 1
            if args.limit and n >= args.limit:
                break
        if args.limit and n >= args.limit:
            break

    verdict, fails, warns, notes = report(a, hours, args.out)
    print(f'VERDICT: {verdict}')
    for f in fails:
        print(f'  FAIL  {f}')
    for x in warns:
        print(f'  WARN  {x}')
    for x in notes:
        print(f'  NOTE  {x}')
    print(f'wrote {os.path.relpath(args.out, ROOT)}')
    return 0 if verdict == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
