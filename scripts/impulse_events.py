#!/usr/bin/env python3
"""Build BTC/ETH impulse events with executable targets. No model, no fitting.

The question this exists to answer, before any model is allowed near it:

    When the major external crypto venues make a large synchronised move and Kalshi has not
    fully incorporated it, is the remaining 2m/5m/10m Kalshi move large enough to trade after
    executable spread and fees?

Everything here is built so that the answer can be NO cheaply.

TWO TARGET CLASSES, kept apart on purpose.

  MIDPOINT, research only. `signed_mid_ret_h`. Tells you whether the price moved. Says nothing
  about whether you could have had it.

  TAKER-EXECUTABLE, primary. A long enters at the ASK and exits at the BID; a short does the
  reverse. Both legs are real recorded quotes and the round trip pays two taker fees. No
  profitability claim in this project may be made on a midpoint exit again.

EVENTS, NOT SAMPLES. A single price move sampled every 100 ms is one observation, not six
hundred. Events carry a cooldown and every suppressed duplicate is counted, so the number
discarded is visible rather than implied. `gap_since_prev_s` is recorded on every event so a
study can demand a 2m, 5m or 10m separation without a second pass.

NOTHING AFTER t_signal ENTERS A FEATURE. The feature block is captured at the trigger instant and
never revisited. Targets are computed afterwards from a recorded quote series, which is the only
part of this file that looks forward, and it is the part that may not touch features.

  python3 scripts/impulse_events.py --all
  python3 scripts/impulse_events.py --hours 2026-09-11T14 2026-09-11T15 --threshold 5
"""
import argparse, bisect, csv, gzip, json, math, os, statistics, sys
from collections import defaultdict, deque

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from execution_sim import iter_events, discover_hours, TAKER_BPS, MAKER_BPS
from feature_engine import FeatureEngine, num, check_epoch

MARKETS = ['KXBTCPERP', 'KXETHPERP']
HORIZONS_S = [120, 300, 600]
TRIGGER_WINDOWS = {'1s': 1_000_000, '5s': 5_000_000, '30s': 30_000_000, '1m': 60_000_000}
# Emission cooldown: two minutes, the SHORTEST horizon studied. Emitting at the longest horizon
# instead caps the sample at six events per market per hour and throws away everything the 2m
# analysis could have used; every event carries `gap_since_prev_s`, so a 5m or 10m study filters
# down to a properly separated subset without a second replay. Separation is enforced where the
# inference happens, not where the events are made.
COOLDOWN_US = 120_000_000
EXT_VENUES = ['coinbase', 'binance', 'hyperliquid', 'kraken']


class Quotes:
    """Best bid and ask over time, per market. The only forward-looking structure here."""

    def __init__(self):
        self.ts = []
        self.bid = []
        self.ask = []

    def add(self, t, b, a):
        if self.ts and self.ts[-1] == t:
            self.bid[-1], self.ask[-1] = b, a
            return
        if self.ts and self.bid[-1] == b and self.ask[-1] == a:
            return
        self.ts.append(t)
        self.bid.append(b)
        self.ask.append(a)

    def at(self, t, max_stale_us=30_000_000):
        """The quote in force at t, or None if the nearest one is too old to stand in for it."""
        if not self.ts:
            return None
        i = bisect.bisect_right(self.ts, t) - 1
        if i < 0 or t - self.ts[i] > max_stale_us:
            return None
        return self.bid[i], self.ask[i]

    def span(self, t0, t1):
        """Every quote in [t0, t1], for the excursion measures."""
        i = bisect.bisect_left(self.ts, t0)
        j = bisect.bisect_right(self.ts, t1)
        return list(zip(self.ts[i:j], self.bid[i:j], self.ask[i:j]))


def composite(engine, market, now_us, span_us):
    """Mean external return across the venues that have enough history, plus their dispersion.

    A venue with less history than the window returns None and is left out rather than being
    given a shorter return under the same name -- the reason the feature engine refuses to reach
    for the oldest point it has.
    """
    rets = []
    for venue in EXT_VENUES:
        key = next((k for k in engine.ext_map.get(market, []) if k.startswith(venue)), None)
        fd = engine.ext.get(key) if key else None
        if fd is None or fd.price is None:
            continue
        age = fd.age_us(now_us)
        if age is None or age > 15_000_000:
            continue
        r = fd.ret_bps(now_us, span_us)
        if r is not None:
            rets.append((venue, r))
    if not rets:
        return None, None, 0, {}
    vals = [r for _, r in rets]
    mean = sum(vals) / len(vals)
    disp = statistics.pstdev(vals) if len(vals) > 1 else 0.0
    return mean, disp, len(vals), dict(rets)


class Builder:
    def __init__(self, markets, threshold_bps, cooldown_us=COOLDOWN_US):
        self.eng = FeatureEngine()
        self.markets = set(markets)
        # Only these markets are reconstructed, and no flow windows are maintained: this builder
        # reads the book and the external feeds and nothing else.
        self.eng.only_markets = set(markets)
        self.eng.track_flow = False
        self.threshold = threshold_bps
        self.cooldown_us = cooldown_us
        self.quotes = defaultdict(Quotes)
        # Parallel lists, for the same reason Feed keeps them: this is bisected on every book
        # update and a deque of pairs forces an O(n) key rebuild per call.
        self.k_ts = defaultdict(list)
        self.k_px = defaultdict(list)
        self.last_event = {}
        self.events = []
        self.suppressed = defaultdict(int)
        self.grid_last = {}
        self.n_events_seen = 0
        # What the external composite actually does, so "a large synchronised move" is set from
        # the data instead of from a round number. Sampled, not exhaustive: the point is the
        # shape of the distribution, and every book update would just be the same shape slower.
        self.mag_hist = defaultdict(lambda: defaultdict(int))
        self.mag_n = defaultdict(int)

    def feed(self, src, ev):
        try:
            self.eng.on_event(src, ev)
        except Exception:
            self.suppressed['engine_error'] += 1
            return
        m = ev.get('msg') or {}
        t = m.get('market_ticker')
        if not t or t not in self.markets or src != 'kalshi_margin':
            return
        bk = self.eng.books.get(t)
        if bk is None or not bk.usable():
            return
        ts = check_epoch(ev.get('src_ts_us'), 'src') or ev.get('recv_ts_us')
        if not ts:
            return
        self.quotes[t].add(ts, bk.best_bid, bk.best_ask)
        mid = bk.mid
        ks, kp = self.k_ts[t], self.k_px[t]
        if not kp or kp[-1] != mid:
            ks.append(ts)
            kp.append(mid)
        self._maybe_event(t, bk, ts)

    def _kalshi_ret(self, t, now_us, span_us):
        ks, kp = self.k_ts[t], self.k_px[t]
        if not ks:
            return None
        target = now_us - span_us
        if ks[0] > target:
            return None
        i = bisect.bisect_left(ks, target)
        if i >= len(ks):
            return None
        base = kp[i]
        return (kp[-1] - base) / base * 1e4 if base else None

    def _maybe_event(self, t, bk, ts):
        # Warm-up and staleness are the engine's job; an event built on a book it would have
        # suppressed is an event built on nothing.
        if self.eng.connect_us and (ts - self.eng.connect_us) < self.eng.warmup_us \
                and bk.empty_snapshot:
            return
        fired_window, fired_mag, fired_sign = None, 0.0, 0
        comps = {}
        for name, span in TRIGGER_WINDOWS.items():
            mean, disp, n, per = composite(self.eng, t, ts, span)
            comps[name] = (mean, disp, n, per)
            if mean is None or n < 2:
                continue
            self.mag_n[name] += 1
            if self.mag_n[name] % 97 == 0:
                self.mag_hist[name][min(int(abs(mean)), 200)] += 1
            if abs(mean) >= self.threshold and abs(mean) > abs(fired_mag):
                fired_window, fired_mag = name, mean
                fired_sign = 1 if mean > 0 else -1
        if fired_window is None:
            return
        self.n_events_seen += 1
        prev = self.last_event.get(t)
        gap = (ts - prev) if prev else None
        if prev is not None and (ts - prev) < self.cooldown_us:
            self.suppressed['cooldown'] += 1
            return
        self.last_event[t] = ts
        self.events.append(self._capture(t, bk, ts, fired_window, fired_mag, fired_sign,
                                         comps, gap))

    def _capture(self, t, bk, ts, window, mag, sign, comps, gap):
        """Pre-entry state only. Nothing here may be knowable after ts."""
        cs = self.eng.contract_size.get(t)
        mid = bk.mid
        under = (mid / cs) if (cs and mid) else None
        r = {
            'market': t, 'signal_us': ts, 'entry_us': ts,
            'trigger_window': window, 'impulse_bps': mag, 'impulse_sign': sign,
            'gap_since_prev_s': (gap / 1e6) if gap else None,
            'bid': bk.best_bid, 'ask': bk.best_ask, 'mid': mid,
            'spread_bps': bk.spread_bps,
            'microprice_minus_mid_bps': ((bk.microprice - mid) / mid * 1e4) if mid else None,
            'n_bid_levels': len(bk.bids), 'n_ask_levels': len(bk.asks),
        }
        for n_ in (1, 5, 10):
            r[f'L{n_}_OBI'] = bk.obi(n_)
            r[f'depth_{n_}_bid'] = bk.depth(n_, 'bid')
            r[f'depth_{n_}_ask'] = bk.depth(n_, 'ask')
        for name, (mean, disp, n, per) in comps.items():
            r[f'ext_ret_{name}_bps'] = mean
            r[f'ext_disp_{name}_bps'] = disp
            r[f'ext_n_venues_{name}'] = n
            for v in EXT_VENUES:
                r[f'ext_{v}_{name}_bps'] = per.get(v)
            # Agreement: how many venues moved the same way as the composite.
            if per:
                same = sum(1 for x in per.values() if mean and (x > 0) == (mean > 0))
                r[f'ext_agree_{name}'] = same / len(per)
            kr = self._kalshi_ret(t, ts, TRIGGER_WINDOWS[name])
            r[f'kalshi_ret_{name}_bps'] = kr
            # THE lag feature: how much of the external move Kalshi has not yet taken.
            r[f'lag_{name}_bps'] = ((mean - kr) if (mean is not None and kr is not None) else None)
        idx = self.eng.cf_map.get(t)
        cf = self.eng.cf.get(idx) if idx else None
        if cf and cf.price and under and (cf.age_us(ts) or 0) < 10_000_000:
            r['cf_basis_bps'] = (under / cf.price - 1) * 1e4
            for name, span in TRIGGER_WINDOWS.items():
                r[f'cf_ret_{name}_bps'] = cf.ret_bps(ts, span)
        else:
            r['cf_basis_bps'] = None
            for name in TRIGGER_WINDOWS:
                r[f'cf_ret_{name}_bps'] = None
        kp = self.k_px[t]
        if len(kp) > 30:
            recent = kp[-400:]
            rets = [(b - a) / a * 1e4 for a, b in zip(recent, recent[1:]) if a]
            r['kalshi_vol_bps'] = statistics.pstdev(rets) if len(rets) > 2 else None
        else:
            r['kalshi_vol_bps'] = None
        return r


# ---- targets, computed AFTER the replay, never touching the feature block -----------------------
def attach_targets(events, quotes, fee_bps=TAKER_BPS):
    """Executable and midpoint targets for every horizon, plus the excursions.

    A long enters at the ask and leaves at the bid. A short enters at the bid and leaves at the
    ask. Both are recorded quotes. The midpoint target is computed alongside and labelled as
    research-only, because the two answer different questions and the last lane was lost by
    letting one stand in for the other.
    """
    kept = 0
    for e in events:
        q = quotes.get(e['market'])
        if q is None:
            continue
        t0 = e['entry_us']
        entry_long = e['ask']
        entry_short = e['bid']
        for hs in HORIZONS_S:
            h_us = hs * 1_000_000
            fut = q.at(t0 + h_us)
            if fut is None or not entry_long or not entry_short:
                e[f'mid_ret_{hs}s'] = None
                e[f'taker_long_net_{hs}s'] = None
                e[f'taker_short_net_{hs}s'] = None
                continue
            fb, fa = fut
            mid0, mid1 = (e['bid'] + e['ask']) / 2, (fb + fa) / 2
            e[f'mid_ret_{hs}s'] = (mid1 / mid0 - 1) * 1e4 if mid0 else None
            # long: pay the ask now, hit the bid later
            gl = (fb / entry_long - 1) * 1e4
            # short: hit the bid now, pay the ask later
            gs = (entry_short / fa - 1) * 1e4
            e[f'taker_long_gross_{hs}s'] = gl
            e[f'taker_short_gross_{hs}s'] = gs
            e[f'taker_long_net_{hs}s'] = gl - 2 * fee_bps
            e[f'taker_short_net_{hs}s'] = gs - 2 * fee_bps
            # Signed by the impulse: what a trade that follows the external move would have made.
            sgn = e['impulse_sign']
            e[f'signed_mid_ret_{hs}s'] = (sgn * e[f'mid_ret_{hs}s']
                                          if e[f'mid_ret_{hs}s'] is not None else None)
            e[f'signed_taker_net_{hs}s'] = (e[f'taker_long_net_{hs}s'] if sgn > 0
                                            else e[f'taker_short_net_{hs}s'])
            e[f'signed_taker_gross_{hs}s'] = (gl if sgn > 0 else gs)
            # Excursions on executable prices, over the holding interval.
            path = q.span(t0, t0 + h_us)
            if path:
                if sgn > 0:
                    vals = [(b / entry_long - 1) * 1e4 for _, b, _ in path]
                else:
                    vals = [(entry_short / a - 1) * 1e4 for _, _, a in path]
                e[f'mfe_{hs}s'] = max(vals)
                e[f'mae_{hs}s'] = min(vals)
                tmfe = path[vals.index(max(vals))][0]
                tmae = path[vals.index(min(vals))][0]
                e[f'time_to_mfe_s_{hs}s'] = (tmfe - t0) / 1e6
                e[f'time_to_mae_s_{hs}s'] = (tmae - t0) / 1e6
        kept += 1
    return kept


def short_path(events, quotes):
    """Return after 1s..1m, so the decay shape is visible without a second replay."""
    SHORT = [1, 5, 10, 30, 60]
    for e in events:
        q = quotes.get(e['market'])
        if q is None:
            continue
        sgn, t0 = e['impulse_sign'], e['entry_us']
        el, es = e['ask'], e['bid']
        for s in SHORT:
            fut = q.at(t0 + s * 1_000_000)
            if fut is None or not el or not es:
                e[f'signed_taker_gross_{s}s'] = None
                continue
            fb, fa = fut
            e[f'signed_taker_gross_{s}s'] = ((fb / el - 1) * 1e4 if sgn > 0
                                             else (es / fa - 1) * 1e4)


def write_events(events, path):
    if not events:
        return 0, []
    cols = sorted({k for e in events for k in e})
    front = ['market', 'signal_us', 'entry_us', 'trigger_window', 'impulse_bps', 'impulse_sign',
             'gap_since_prev_s', 'bid', 'ask', 'mid', 'spread_bps']
    cols = front + [c for c in cols if c not in front]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction='ignore')
        w.writeheader()
        for e in sorted(events, key=lambda x: x['entry_us']):
            w.writerow(e)
    return len(events), cols


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--hours', nargs='*')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--markets', nargs='*', default=MARKETS)
    ap.add_argument('--threshold', type=float, default=5.0,
                    help='external composite move in bps that fires an event')
    ap.add_argument('--cooldown-s', type=float, default=120.0)
    ap.add_argument('--out', default=os.path.join(ROOT, 'reports', 'impulse_events.csv'))
    args = ap.parse_args()
    hours = args.hours or (discover_hours() if args.all else [])
    if not hours:
        sys.exit('no hours selected: pass --all')

    b = Builder(args.markets, args.threshold, int(args.cooldown_s * 1e6))
    n = 0
    for src, ev in iter_events(hours):
        b.feed(src, ev)
        n += 1
        if n % 4_000_000 == 0:
            print(f'  {n:,} events, {len(b.events):,} impulses', file=sys.stderr)

    attach_targets(b.events, b.quotes)
    short_path(b.events, b.quotes)
    cnt, cols = write_events(b.events, args.out)

    span_h = 0.0
    if b.events:
        ts = [e['entry_us'] for e in b.events]
        span_h = (max(ts) - min(ts)) / 3.6e9
    print(f'replayed {n:,} frames over {len(hours)} hours')
    print(f'impulse conditions seen: {b.n_events_seen:,}')
    for k, v in sorted(b.suppressed.items(), key=lambda kv: -kv[1]):
        print(f'  suppressed {k}: {v:,}')
    print(f'events emitted: {cnt:,} across {span_h:.2f} h of event span')
    from collections import Counter
    for m, c in Counter(e['market'] for e in b.events).most_common():
        usable = sum(1 for e in b.events if e.get('signed_taker_net_600s') is not None)
        print(f'  {m}: {c} events')
    print(f'  with a usable 10m executable target: '
          f'{sum(1 for e in b.events if e.get("signed_taker_net_600s") is not None):,}')
    print()
    print('external composite |move| distribution, by trigger window (sampled 1 in 97):')
    for name in TRIGGER_WINDOWS:
        h = b.mag_hist.get(name)
        if not h:
            continue
        tot = sum(h.values())
        cum, pcts = 0, {}
        for mag in sorted(h):
            cum += h[mag]
            for q in (50, 90, 95, 99):
                if q not in pcts and cum / tot >= q / 100.0:
                    pcts[q] = mag
        print(f'  {name:>3s}: n={tot:,}  p50={pcts.get(50,0)}bp  p90={pcts.get(90,0)}bp  '
              f'p95={pcts.get(95,0)}bp  p99={pcts.get(99,0)}bp')
    print()
    print(f'wrote {os.path.relpath(args.out, ROOT)} ({len(cols)} columns)')


if __name__ == '__main__':
    main()
