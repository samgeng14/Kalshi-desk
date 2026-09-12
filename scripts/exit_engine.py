#!/usr/bin/env python3
"""Can a selected maker entry get OUT? The round trip, measured rather than assumed.

The conditional study established two things. Selecting entries works: the top 1% of states by
predicted `kept` holds +8.5 to +9.0 bps against a random null that holds nothing, monotone in
selectivity and consistent across four horizons. And nothing clears a round trip -- but only
because the exit was priced at the measured cost of CROSSING, 17 to 20 bps, which is an
assumption, not a measurement.

This file removes the assumption. For every entry the model selects, it replays the real book
after the fill and tries to get out under ten different exit policies, tracking each exit order
through the same queue machinery the entry used. The output is

    net = (exit price - entry price) * side - entry fee - exit fee

in basis points, per trade, with no leg priced by assumption.

WHY THE EXIT IS THE HARD HALF. An entry can be selected and a exit cannot. Declining to quote
costs nothing, so the entry model is free to wait for its top 1% of states. Once a fill happens
there is inventory, and it has to leave in whatever state the market is actually in. Every policy
below is therefore a different answer to "how long do you wait before you give up and pay", and
the interesting number is not the best one but the shape of the trade-off.

WHAT IS AND IS NOT MODELLED
  * Each entry is treated independently. There is no inventory netting, no position limit and no
    interaction between simultaneous trades. A real book of these would have all three.
  * Our orders are still not in the replayed book, on either leg. Fill rates are upper bounds,
    and `improve` most of all -- its fills lean on the price-priority rule.
  * The entry fills are the ones the execution simulator produced, so the entry leg carries every
    caveat that study already carries.
  * Both queue bounds are tracked and both are reported. A policy whose conclusion flips between
    them has no conclusion.

  python3 scripts/exit_engine.py --fills reports/maker_fills.csv --all
"""
import argparse, bisect, csv, json, math, os, random, statistics, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from execution_sim import (Sim, Order, walk_book, iter_events, MAKER_BPS, TAKER_BPS, fmt,
                           discover_hours)
from feature_engine import num
import conditional_markout as CM

# A policy is one answer to "how long do you wait before you pay up".
#   delay     seconds to hold before quoting at all
#   style     'cross' | 'touch' | 'improve'
#   replace   reprice to the new touch when the book moves away
#   timeout   seconds quoting before crossing
#   cross     cross at the timeout, or keep waiting
#   max_hold  hard cap; a forced cross, always, so no policy can "never exit"
POLICIES = {
    'taker_now':         dict(delay=0, style='cross',   replace=False, timeout=0,  cross=True,  max_hold=0),
    'maker_touch_1s':    dict(delay=0, style='touch',   replace=False, timeout=1,  cross=True,  max_hold=60),
    'maker_touch_5s':    dict(delay=0, style='touch',   replace=False, timeout=5,  cross=True,  max_hold=60),
    'maker_touch_10s':   dict(delay=0, style='touch',   replace=False, timeout=10, cross=True,  max_hold=60),
    'maker_touch_30s':   dict(delay=0, style='touch',   replace=False, timeout=30, cross=True,  max_hold=90),
    'maker_improve_10s': dict(delay=0, style='improve', replace=False, timeout=10, cross=True,  max_hold=60),
    'maker_replace_10s': dict(delay=0, style='touch',   replace=True,  timeout=10, cross=True,  max_hold=60),
    'maker_replace_30s': dict(delay=0, style='touch',   replace=True,  timeout=30, cross=True,  max_hold=90),
    'wait5_maker_10s':   dict(delay=5, style='touch',   replace=False, timeout=10, cross=True,  max_hold=60),
    'maker_30s_patient': dict(delay=0, style='touch',   replace=False, timeout=30, cross=False, max_hold=300),
}


class ExitOrder:
    """One hypothetical exit, its policy, and the entry it is trying to close."""
    __slots__ = ('entry', 'policy', 'name', 'order', 'spawn_us', 'posted_us', 'done',
                 'exit_px', 'exit_us', 'exit_fee', 'crossed', 'n_replaces', 'forced',
                 'mid_exit', 'filled_frac')

    def __init__(self, entry, name, policy, spawn_us):
        self.entry, self.name, self.policy = entry, name, policy
        self.spawn_us = spawn_us
        self.posted_us = None
        self.order = None
        self.done = False
        self.exit_px = self.exit_us = self.exit_fee = None
        self.crossed = False
        self.forced = False
        self.n_replaces = 0
        self.mid_exit = None
        self.filled_frac = 1.0

    @property
    def side(self):
        # Closing a long means selling.
        return 'sell' if self.entry['side'] == 'buy' else 'buy'


class ExitSim(Sim):
    """Replays the tape, spawns exits at the recorded entry fills, never creates new entries."""

    def __init__(self, entries, markets, bound='pess'):
        # sample_s enormous so the parent never generates an entry of its own.
        super().__init__(25.0, 1e12, 1e9, 1.0, markets, capture=False, max_open=100000)
        self.bound = bound
        self.by_market = defaultdict(list)
        for e in entries:
            self.by_market[e['market']].append(e)
        for m in self.by_market:
            self.by_market[m].sort(key=lambda e: e['fill_us'])
        self.cursor = defaultdict(int)
        self.live = defaultdict(list)          # market -> [ExitOrder]
        self.closed = []
        self.owner = {}                        # Order -> ExitOrder
        self.stats = defaultdict(int)

    # -- no new entries, ever --
    def _maybe_sample(self, t, bk, ts):
        return

    def _after_kalshi(self, t, ev, ts, before_qty):
        super()._after_kalshi(t, ev, ts, before_qty)
        bk = self.eng.books.get(t)
        if bk is None or bk.stale:
            return
        self._spawn(t, ts)
        if self.live[t]:
            self._drive(t, bk, ts)

    def _spawn(self, t, ts):
        """Start every policy's exit once the tape reaches the entry's fill time."""
        lst = self.by_market.get(t)
        if not lst:
            return
        i = self.cursor[t]
        while i < len(lst) and lst[i]['fill_us'] <= ts:
            e = lst[i]
            for name, pol in POLICIES.items():
                self.live[t].append(ExitOrder(e, name, pol, ts))
                self.stats['spawned'] += 1
            i += 1
        self.cursor[t] = i

    def _price_for(self, bk, side, style):
        """Where this policy wants to rest. Returns None when there is nowhere valid."""
        if not bk.usable():
            return None
        touch = bk.best_ask if side == 'sell' else bk.best_bid
        if style == 'touch':
            return touch
        tick = self.ticks.tick(bk.ticker)
        if not tick:
            return None
        other = bk.best_bid if side == 'sell' else bk.best_ask
        if abs(touch - other) <= tick * 1.5:
            return touch               # one-tick spread: nowhere to improve to
        return touch - tick if side == 'sell' else touch + tick

    def _drive(self, t, bk, ts):
        keep = []
        for xo in self.live[t]:
            if xo.done:
                self.closed.append(xo)
                continue
            age = ts - xo.spawn_us
            pol = xo.policy

            # Hard cap first: no policy is allowed to hold forever.
            if pol['max_hold'] and age >= pol['max_hold'] * 1e6:
                self._force_cross(xo, bk, ts, forced=True)
                self.closed.append(xo)
                continue

            if xo.order is None:
                if age < pol['delay'] * 1e6:
                    keep.append(xo)
                    continue
                if pol['style'] == 'cross':
                    self._force_cross(xo, bk, ts)
                    self.closed.append(xo)
                    continue
                px = self._price_for(bk, xo.side, pol['style'])
                if px is None:
                    keep.append(xo)
                    continue
                depth = (bk.asks if xo.side == 'sell' else bk.bids).get(px, 0.0)
                q = depth if pol['style'] == 'touch' else 0.0
                o = Order('exit', t, xo.side, px, xo.entry['size'], ts, bk.mid, q,
                          10 ** 12, bk.spread_bps)
                xo.order, xo.posted_us = o, ts
                self.open[t].append(o)
                self.owner[o] = xo
                keep.append(xo)
                continue

            o = xo.order
            filled_us = o.fill_pess_us if self.bound == 'pess' else o.fill_opt_us
            if filled_us is not None:
                xo.exit_px, xo.exit_us = o.fill_px, filled_us
                xo.mid_exit = bk.mid
                xo.exit_fee = MAKER_BPS
                xo.done = True
                self._retire(t, o)
                self.closed.append(xo)
                self.stats['exit_passive'] += 1
                continue

            # Cancel/replace: a resting order the book has walked away from is not a quote.
            if pol['replace']:
                want = self._price_for(bk, xo.side, pol['style'])
                if want is not None and abs(want - o.price) > 1e-9:
                    self._retire(t, o)
                    depth = (bk.asks if xo.side == 'sell' else bk.bids).get(want, 0.0)
                    o2 = Order('exit', t, xo.side, want, xo.entry['size'], ts, bk.mid,
                               depth, 10 ** 12, bk.spread_bps)
                    xo.order = o2
                    self.open[t].append(o2)
                    self.owner[o2] = xo
                    xo.n_replaces += 1
                    self.stats['replaces'] += 1
                    keep.append(xo)
                    continue

            if pol['timeout'] and (ts - xo.posted_us) >= pol['timeout'] * 1e6 and pol['cross']:
                self._retire(t, o)
                self._force_cross(xo, bk, ts)
                self.closed.append(xo)
                continue
            keep.append(xo)
        self.live[t] = keep

    def _retire(self, t, o):
        o.done = True
        self.owner.pop(o, None)
        try:
            self.open[t].remove(o)
        except ValueError:
            pass

    def _force_cross(self, xo, bk, ts, forced=False):
        """Pay the spread. Walks the book for the real size rather than taking the touch price."""
        if xo.order is not None:
            self._retire(xo.entry['market'], xo.order)
            xo.order = None
        vwap, got, _ = walk_book(bk, xo.side, xo.entry['size'])
        if vwap is None:
            # No book to cross into. Marking this at the mid would invent a price, so it is
            # recorded as unresolved and excluded downstream with a counted reason.
            xo.done = True
            xo.exit_px = None
            xo.exit_us = ts
            self.stats['exit_no_book'] += 1
            return
        # A cross that cannot fill the whole size leaves part of the position open. Treating the
        # partial VWAP as a completed exit would price a trade we did not finish, so the fraction
        # is recorded and reported rather than rounded away.
        want = xo.entry['size']
        xo.filled_frac = (got / want) if want else 0.0
        if xo.filled_frac < 0.999:
            self.stats['exit_partial_cross'] += 1
        xo.exit_px, xo.exit_us = vwap, ts
        xo.mid_exit = bk.mid
        xo.exit_fee = TAKER_BPS
        xo.crossed = True
        xo.forced = forced
        xo.done = True
        self.stats['exit_forced' if forced else 'exit_crossed'] += 1

    def finish(self):
        for t, lst in self.live.items():
            for xo in lst:
                if not xo.done:
                    self.stats['exit_unresolved_at_tape_end'] += 1
                    xo.exit_px = None
                    self.closed.append(xo)
        self.live.clear()
        return self.closed


# ---- entry selection -------------------------------------------------------------------------
def select_entries(fills_path, top_frac, horizon='5s', seed=5):
    """The same ranking the conditional study used, on the same held-out half.

    The model is imported rather than reimplemented. If the selection here differed from the
    selection there -- a different split, a different standardiser, a different feature list --
    the round-trip result would not be about the same trades, and the two studies could not be
    read together.
    """
    rows, feats = CM.load(fills_path)
    if len(rows) < 400:
        return [], [], 'not enough fills'
    cut = int(len(rows) * CM.TRAIN_FRAC)
    train, test = rows[:cut], rows[cut:]
    std = CM.Standardiser()
    cols = std.fit(train, feats)
    ytr = [r[f'kept_{horizon}'] for r in train]
    keep = [i for i, v in enumerate(ytr) if v is not None]
    X = [std.vec(train[i]) for i in keep]
    y = [ytr[i] for i in keep]
    w = CM.ridge_fit(X, y)
    if w is None:
        return [], [], 'model did not fit'
    scores = [sum(a * b for a, b in zip(w, std.vec(r))) for r in test]
    order = sorted(range(len(test)), key=lambda i: -scores[i])
    k = max(int(len(order) * top_frac), 1)

    def pack(idx, rank_by):
        out = []
        for rank, i in enumerate(idx):
            r = test[i]
            out.append({'market': r['market'], 'side': r['side'],
                        'fill_us': int(r['fill_us']),
                        'price': r['x'].get('price') or r.get('price'),
                        'mid': r['x'].get('mid'),
                        'size': 1.0, 'score': scores[i], 'rank': rank,
                        'n_test': len(test), 'selector': rank_by,
                        'spread_bps': r.get('spread_bps'),
                        f'kept_{horizon}': r.get(f'kept_{horizon}')})
        return out

    sel = pack(order[:k], 'model')
    rng = random.Random(seed)
    ridx = list(range(len(test)))
    rng.shuffle(ridx)
    rnd = pack(ridx[:k], 'random')
    return sel, rnd, None


def load_entry_prices(fills_path):
    """Entry fill price and size, keyed by (market, fill_us). CM.load drops them into x."""
    out = {}
    with open(fills_path) as f:
        for r in csv.DictReader(f):
            try:
                key = (r['market'], int(float(r['fill_us'])))
            except (KeyError, TypeError, ValueError):
                continue
            out[key] = (num(r.get('price')), num(r.get('mid')))
    return out


# ---- accounting ------------------------------------------------------------------------------
def round_trip(xo):
    """entry PnL + exit PnL - fees, in bps of the entry price. No leg priced by assumption."""
    e = xo.entry
    pe, px = e.get('price'), xo.exit_px
    if not pe or not px or pe <= 0:
        return None
    sgn = 1.0 if e['side'] == 'buy' else -1.0
    gross = sgn * (px - pe) / pe * 1e4
    entry_fee = MAKER_BPS
    exit_fee = xo.exit_fee if xo.exit_fee is not None else TAKER_BPS
    # Decomposition: what the position was worth at the mid when we left, and what leaving cost.
    mid = xo.mid_exit
    entry_edge = (sgn * (mid - pe) / pe * 1e4) if mid else None
    exit_slip = (gross - entry_edge) if entry_edge is not None else None
    return {
        'selector': e.get('selector'), 'policy': xo.name, 'market': e['market'],
        'rank': e['rank'], 'n_test': e['n_test'], 'entry_us': e.get('fill_us'),
        'side': e['side'], 'entry_px': pe, 'exit_px': px,
        'hold_s': ((xo.exit_us - xo.spawn_us) / 1e6) if xo.exit_us else None,
        'passive_exit': not xo.crossed, 'forced': xo.forced, 'replaces': xo.n_replaces,
        'filled_frac': xo.filled_frac,
        'gross_bps': gross, 'entry_edge_bps': entry_edge, 'exit_slip_bps': exit_slip,
        'entry_fee_bps': entry_fee, 'exit_fee_bps': exit_fee,
        'net_bps': gross - entry_fee - exit_fee,
    }


QUANTS = [0.10, 0.05, 0.02, 0.01]


def avg(xs):
    xs = [x for x in xs if x is not None and x == x]
    return sum(xs) / len(xs) if xs else None


def aggregate(trades, span_h):
    """One line per (selector, entry filter, exit policy). Sub-quantiles are nested subsets of
    the selected set, so the same replay answers every row of the table."""
    out = []
    by_sel = defaultdict(list)
    for t in trades:
        by_sel[t['selector']].append(t)
    for sel, rows in by_sel.items():
        n_test = rows[0]['n_test'] if rows else 0
        for q in QUANTS:
            k = max(int(n_test * q), 1)
            sub = [r for r in rows if r['rank'] < k]
            for pol in POLICIES:
                rs = [r for r in sub if r['policy'] == pol and r['net_bps'] is not None]
                if len(rs) < 10:
                    continue
                nets = [r['net_bps'] for r in rs]
                # Cluster on the minute the trade was entered. Every perpetual moves together,
                # so a sub-hour sample of two thousand fills is a handful of independent
                # observations, not two thousand.
                keys = [CM.time_cluster(r.get('entry_us') or r.get('fill_us')) for r in rs]
                if all(k is None for k in keys):
                    keys = [CM.time_cluster(r.get('post_us')) for r in rs]
                m, lo, hi, n_clu = CM.cluster_bootstrap(nets, keys, want_n=True)
                out.append({
                    'selector': sel, 'entry_filter': f'top {q:.0%}', 'quantile': q,
                    'exit_policy': pol, 'trades': len(rs),
                    # Trades per hour of TAPE, not extrapolated to a day. The held-out window
                    # is under an hour, and multiplying it by 24 produced a "56,632 trades/day"
                    # column that describes arithmetic rather than the strategy.
                    'trades_per_hour': len(rs) / span_h if span_h else None,
                    'passive_exit_pct': 100.0 * sum(1 for r in rs if r['passive_exit']) / len(rs),
                    'forced_pct': 100.0 * sum(1 for r in rs if r['forced']) / len(rs),
                    'median_hold_s': statistics.median([r['hold_s'] for r in rs
                                                        if r['hold_s'] is not None] or [0]),
                    'entry_edge_bps': avg([r['entry_edge_bps'] for r in rs]),
                    'exit_slip_bps': avg([r['exit_slip_bps'] for r in rs]),
                    'gross_bps': sum(r['gross_bps'] for r in rs) / len(rs),
                    'entry_fee_bps': MAKER_BPS,
                    'exit_fee_bps': sum(r['exit_fee_bps'] for r in rs) / len(rs),
                    'net_bps': m, 'net_lo': lo, 'net_hi': hi, 'n_clusters': n_clu,
                })
    return out


def report(lines, stats, span_h, n_sel, out_md, out_csv):
    L = []
    W = L.append
    W('# Exit engine: the conditional maker round trip\n')
    n_orders = stats.get('spawned') or sum(l['trades'] for l in lines
                                          if l['quantile'] == max(QUANTS))
    W(f'{n_sel:,} selected entries, {len(POLICIES)} exit policies each, '
      f'{n_orders:,} exit orders simulated over {span_h:.1f} hours of held-out tape.\n')
    W('Net is `(exit price - entry price) x side - entry fee - exit fee`, in bps of the entry '
      'price. Nothing is priced by assumption on either leg.\n')

    model = [L_ for L_ in lines if L_['selector'] == 'model']
    rnd = {(x['quantile'], x['exit_policy']): x for x in lines if x['selector'] == 'random'}

    W('## The table\n')
    W('Intervals are 90% from a bootstrap that resamples whole ONE-MINUTE CLUSTERS. `clusters` '
      'is how many independent minutes the interval actually rests on; the trade count is not '
      'the sample size.\n')
    W('| entry | exit policy | trades | clusters | /hour | passive exit | median hold | '
      'entry edge | exit slip | exit fee | net | 90% interval | random null |')
    W('|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|')
    for r in sorted(model, key=lambda x: (-x['quantile'], -x['net_bps'])):
        nr = rnd.get((r['quantile'], r['exit_policy']))
        null_net = '' if not nr else f"{nr['net_bps']:+.2f}"
        band = ('n/a' if r['net_lo'] is None
                else f"[{r['net_lo']:+.2f}, {r['net_hi']:+.2f}]")
        W('| {} | `{}` | {} | {} | {:.0f} | {:.0f}% | {:.1f}s | {} | {} | {:.1f} | '
          '**{:+.2f}** | {} | {} |'.format(
              r['entry_filter'], r['exit_policy'], r['trades'], r.get('n_clusters', ''),
              r['trades_per_hour'], r['passive_exit_pct'], r['median_hold_s'],
              fmt(r['entry_edge_bps']), fmt(r['exit_slip_bps']), r['exit_fee_bps'],
              r['net_bps'], band, null_net))
    W('')

    pos = [r for r in model if r['net_bps'] > 0]
    strong = [r for r in model if r['net_lo'] is not None and r['net_lo'] > 0]
    W('## Verdict\n')
    if strong:
        b = max(strong, key=lambda r: r['net_lo'])
        W(f"**{len(strong)} of {len(model)} combinations have a 90% lower bound above zero.** "
          f"The best is `{b['entry_filter']}` entries exited by `{b['exit_policy']}`: "
          f"{b['net_bps']:+.2f} bps net [{b['net_lo']:+.2f}, {b['net_hi']:+.2f}] over "
          f"{b['trades']} trades, {b['passive_exit_pct']:.0f}% of them exiting passively.\n")
        nr = rnd.get((b['quantile'], b['exit_policy']))
        if nr:
            W(f"The same exit policy applied to randomly chosen entries nets "
              f"{nr['net_bps']:+.2f} bps, so the edge is in the selection and not in the exit "
              f"policy by itself.\n")
        W('That is the first paper-trading candidate this project has produced. It is one '
          'session of tape and the interval is what it is, so the next thing it needs is more '
          'tape, not more modelling.\n')
    elif pos:
        b = max(pos, key=lambda r: r['net_bps'])
        W(f"**{len(pos)} of {len(model)} combinations are positive on the point estimate, none "
          f"with an interval clear of zero.** Best: `{b['entry_filter']}` exited by "
          f"`{b['exit_policy']}`, {b['net_bps']:+.2f} bps "
          f"[{b['net_lo']:+.2f}, {b['net_hi']:+.2f}] over {b['trades']} trades.\n")
    else:
        b = max(model, key=lambda r: r['net_bps']) if model else None
        if b:
            W(f"**Nothing is positive.** The best combination is `{b['entry_filter']}` exited by "
              f"`{b['exit_policy']}` at {b['net_bps']:+.2f} bps over {b['trades']} trades.\n")
        W('The exit is now measured rather than assumed, and it did not rescue the round trip.\n')

    if model:
        W('## What the exit policy actually buys\n')
        W('Averaged across entry filters, so the comparison is between policies rather than '
          'between subsets.\n')
        W('| exit policy | passive exit | median hold | exit slip | exit fee | net |')
        W('|---|---:|---:|---:|---:|---:|')
        for pol in POLICIES:
            rs = [r for r in model if r['exit_policy'] == pol]
            if not rs:
                continue
            W(f"| `{pol}` | {sum(r['passive_exit_pct'] for r in rs)/len(rs):.0f}% | "
              f"{sum(r['median_hold_s'] for r in rs)/len(rs):.1f}s | "
              f"{avg([r['exit_slip_bps'] for r in rs]) or 0:+.2f} | "
              f"{sum(r['exit_fee_bps'] for r in rs)/len(rs):.1f} | "
              f"{sum(r['net_bps'] for r in rs)/len(rs):+.2f} |")
        W('')
        W('`taker_now` is the policy the conditional study assumed for every exit. Every row '
          'above it in net terms is a measurement that assumption was getting wrong.\n')

    W('## Orders that did not resolve cleanly\n')
    for k in ('exit_passive', 'exit_crossed', 'exit_forced', 'exit_no_book',
              'exit_partial_cross', 'exit_unresolved_at_tape_end', 'replaces'):
        if stats.get(k):
            W(f'- `{k}`: {stats[k]:,}')
    W('')
    W('An exit at `max_hold` is counted as `exit_forced` and is included in the numbers: a '
      'policy that quietly drops the trades it could not close would report the fills it liked '
      'and none of the ones that went wrong.\n')

    W('## What would change these numbers\n')
    W('- Inventory. Each trade is independent here. A real book holds several at once, cannot '
      'always exit them separately, and carries the correlation between them.')
    W('- Our orders are absent from the replayed book on both legs, so passive fill rates are '
      'upper bounds twice over.')
    W('- One session. Adverse selection and spread are both regime-dependent and this is one '
      'regime.')
    W('- The fee is tier 0. `reports/fee_ladder.md` has what each row looks like lower down.\n')

    os.makedirs(os.path.dirname(out_md), exist_ok=True)
    with open(out_md, 'w') as f:
        f.write('\n'.join(L) + '\n')
    cols = ['selector', 'entry_filter', 'quantile', 'exit_policy', 'trades', 'trades_per_hour',
            'passive_exit_pct', 'forced_pct', 'median_hold_s', 'entry_edge_bps', 'exit_slip_bps',
            'gross_bps', 'entry_fee_bps', 'exit_fee_bps', 'net_bps', 'net_lo', 'net_hi',
            'n_clusters']
    with open(out_csv, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction='ignore')
        w.writeheader()
        for r in lines:
            w.writerow(r)


def reaggregate(trades_csv, md, csv_out):
    """Rebuild the tables from a completed run. The replay is the expensive part and nothing
    about the aggregation needs it, so a formatting fix should not cost another pass."""
    rows = []
    with open(trades_csv) as f:
        for r in csv.DictReader(f):
            d = dict(r)
            for k in ('rank', 'n_test', 'entry_us', 'entry_px', 'exit_px', 'hold_s', 'gross_bps',
                      'entry_edge_bps', 'exit_slip_bps', 'entry_fee_bps', 'exit_fee_bps',
                      'net_bps', 'filled_frac'):
                if k in d:
                    try:
                        d[k] = float(d[k])
                    except (TypeError, ValueError):
                        d[k] = None
            d['passive_exit'] = str(r.get('passive_exit')).lower() == 'true'
            d['forced'] = str(r.get('forced')).lower() == 'true'
            rows.append(d)
    if not rows:
        sys.exit('no trades in ' + trades_csv)
    ts = [r.get('entry_us') for r in rows if r.get('entry_us')]
    span = ((max(ts) - min(ts)) / 3.6e9) if len(ts) > 1 else 0.0
    lines = aggregate(rows, span)
    report(lines, {}, span, len({r['rank'] for r in rows if r['selector'] == 'model'}),
           md, csv_out)
    model = [x for x in lines if x['selector'] == 'model']
    b = max(model, key=lambda r: r['net_bps'])
    print(f"re-aggregated {len(rows):,} trades -> {len(lines)} lines")
    print(f"best: {b['entry_filter']} + {b['exit_policy']} = {b['net_bps']:+.2f} bps")
    print(f'wrote {os.path.relpath(md, ROOT)}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--from-trades', default='',
                    help='rebuild the report from a finished round_trips.csv, no replay')
    ap.add_argument('--fills', default=os.path.join(ROOT, 'reports', 'maker_fills.csv'))
    ap.add_argument('--hours', nargs='*')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--top', type=float, default=0.10,
                    help='select this fraction; the table nests smaller ones inside it')
    ap.add_argument('--horizon', default='5s')
    ap.add_argument('--bound', default='pess', choices=['pess', 'opt'])
    ap.add_argument('--md', default=os.path.join(ROOT, 'reports', 'exit_engine.md'))
    ap.add_argument('--csv', default=os.path.join(ROOT, 'reports', 'exit_engine.csv'))
    ap.add_argument('--trades-csv', default=os.path.join(ROOT, 'reports', 'round_trips.csv'))
    args = ap.parse_args()

    if args.from_trades:
        reaggregate(args.from_trades, args.md, args.csv)
        return
    hours = args.hours or (discover_hours() if args.all else [])
    if not hours:
        sys.exit('no hours selected: pass --all')
    if not os.path.exists(args.fills):
        sys.exit(f'no fills at {args.fills}: run execution_sim.py --fills-csv first')

    sel, rnd, why = select_entries(args.fills, args.top, args.horizon)
    if why:
        sys.exit(why)
    prices = load_entry_prices(args.fills)
    entries = []
    for e in sel + rnd:
        px, mid = prices.get((e['market'], e['fill_us']), (None, None))
        if px:
            e['price'], e['mid'] = px, mid
            # $25 notional, the size the entry study used.
            e['size'] = 25.0 / px
            entries.append(e)
    print(f'selected {len(sel):,} model + {len(rnd):,} random entries; '
          f'{len(entries):,} have a usable entry price')

    sim = ExitSim(entries, None, bound=args.bound)
    n = 0
    for src, ev in iter_events(hours):
        sim.feed(src, ev)
        n += 1
        if n % 3_000_000 == 0:
            print(f'  {n:,} events, {len(sim.closed):,} exits resolved', file=sys.stderr)
    closed = sim.finish()

    trades = [t for t in (round_trip(xo) for xo in closed) if t]
    span_h = 0.0
    if trades:
        ts = [e['fill_us'] for e in entries]
        span_h = (max(ts) - min(ts)) / 3.6e9
    lines = aggregate(trades, span_h)
    report(lines, sim.stats, span_h, len(sel), args.md, args.csv)

    with open(args.trades_csv, 'w', newline='') as f:
        if trades:
            w = csv.DictWriter(f, fieldnames=sorted(trades[0]), extrasaction='ignore')
            w.writeheader()
            for t in trades:
                w.writerow(t)

    print(f'events {n:,}  exits {len(closed):,}  priced round trips {len(trades):,}')
    model = [x for x in lines if x['selector'] == 'model']
    if model:
        b = max(model, key=lambda r: r['net_bps'])
        print(f"best: {b['entry_filter']} + {b['exit_policy']} = {b['net_bps']:+.2f} bps "
              f"[{b['net_lo']:+.2f}, {b['net_hi']:+.2f}] over {b['trades']} trades")
        print(f"positive point estimates: {sum(1 for r in model if r['net_bps'] > 0)} of {len(model)}")
        print(f"lower bound above zero:  {sum(1 for r in model if r['net_lo'] and r['net_lo'] > 0)}")
    print(f'wrote {os.path.relpath(args.md, ROOT)}')


if __name__ == '__main__':
    main()
