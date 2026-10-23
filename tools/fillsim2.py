#!/usr/bin/env python3
"""Where should LIP-15 rest, and for how long? A queue-position replay over the recorder's books
and prints (plan section 5a: "Before week 3, fillsim2.py tests 'rest 1-2 ticks behind' and age
caps on natural gas; rest behind or join only where simulated markout plus expected LIP dollars
per contract is positive").

The books are lip_join.py's: deskd's frames and units (md/kalshi_md.cpp), the same sequence,
connection and gap rules. The prints are kalshi_ws's trade frames (yes_price_dollars, count_fp,
taker_side, as md/kalshi_md.cpp reads them).

Orders, all virtual (none changes the book or another order): at each whole multiple of --every
seconds (default 10), on each market of the series open then for at least the longest cap after
it (kalshi_markets open_time to close_time; without a market row, its LIP program's window), with
a live book: one order of --size contracts (default 10, the plan's markout leg) on each side, the
YES bid and the YES ask, per placement:
  join      at the touch (the best bid; the best ask)
  improve   one tick better (skipped where it would lock or cross the other side: post-only)
  behind1   one tick behind the touch
  behind2   two ticks behind
A tick is the market's price grid step there (its kalshi_markets row's price_ranges, as
strat/maker.cpp grid_step reads them), 1c without one. Each order is followed 60 s; the age caps
5, 15 and 60 s read the same order cut at that age.

The queue (the rules chosen):
  - placed, an order joins the back of the visible queue at its price: the level's size is ahead
    of it, nothing behind it.
  - size added at its price joins behind it.
  - size removed at its price is an execution as far as a print accounts for it, else a cancel.
    A first pass over the frames pairs each print with the removals at its price and side
    received within 1 s of it, before or after: one of exactly the print's count first, then the
    nearest in time. A removal's unpaired part is a cancel, applied as it is received, to the
    queue as it stands then. Cancels come from the BACK of the queue first: those behind the
    order go before any ahead of it, so it moves up only once nobody is behind it.
    --cancel-front assumes the other: cancels come from the front, ahead of it first.
  - a print at its price fills the queue in order: the size ahead of it first, then the order,
    then those behind. A print through its price (below a bid, above an ask) fills it up to the
    print's count: that price traded only once the order's level was used up. The part of a print
    paired with a removal received before the order was placed is not counted for it: its queue
    was measured without that size.
  - an order arriving on the other side at or through its price with no print (an ask added at
    or below a resting bid, a bid at or above a resting ask) fills it at its own price, up to the
    size that arrived: no real order stood at that price (the real book would have crossed), so
    the arrival would have traded with ours first. What a partial fill leaves rests on, and earns.
  - a dropped book (a gap, a disconnect) ends its orders where they stand; each counts for the
    caps it had reached.

Per placement and age cap, the two sides together: orders; the share that filled at all and in
full within the cap; the mean age at the first fill; contracts filled; the 10 s and 60 s
markouts per contract (the mid N seconds after each fill less the price for a bid, the price less
the mid for an ask, in cents, + in our favour, contract-weighted, as deskd's strat/markouts.h; a
horizon without a two-sided book, or at or past the market's close, is not marked); the expected
LIP dollars per contract; and net60 = the 60 s markout - the maker fee + LIP, in cents a contract.

The LIP estimate (an estimate, kept simple): each second an order rests, deskd's share rule
(strat/lip.h) on the book with the order in it: a side scores the size from its best price
outward until the program's target_size, each contract weighted discount^(cents from the best
price), the order's own contracts last in their level; the second pays only when both sides reach
the target; it pays period_reward / the program's seconds, half to each side, each side's half by
size-weighted share. The order earns its share of its side's half. A market with no program of
its own recorded takes its series' latest recorded terms over its open to close (the hourly sweep
misses most 15-minute windows' own programs). LIP $ per contract: every order's LIP dollars
within the cap, filled or not, over the contracts filled within it, which is what the plan adds
to the markout.

The maker fee: the plan's LIP-15 series are fee-free (section 1's "seven fee-free non-crypto
series", section 5a's heading); for any other series 0 is ASSUMED, and the report says so, unless
--maker-fee gives one.

Input: the recorder's data directory (as lip_join.py): kalshi_ws, kalshi_markets,
kalshi_incentives, gaps. Read-only.

Usage:
  python3 tools/fillsim2.py --data DIR [--series KXNATGAS15M] [--size 10] [--every 10]
                            [--cancel-front] [--maker-fee CENTS] [--json]
  python3 tools/fillsim2.py --self-test
"""
import argparse
import heapq
import json
import os
import re
import sys
import tempfile
from collections import Counter, deque
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import lip_join as lj   # noqa: E402  (the reader and the book replay)
from lip_join import CENT, CT, UNIT   # noqa: E402

S = 1_000_000
PLACEMENTS = (('join', 0), ('improve', 1), ('behind1', -1), ('behind2', -2))   # ticks toward the other side
CAPS = (5, 15, 60)
HORIZONS = (10, 60)
MATCH_US = 1_000_000   # a print pairs with the removals at its price received this close to it
# The plan's LIP-15 lane runs on fee-free series: section 1's "seven fee-free non-crypto series",
# named in docs/edge_results.json (the self-test checks this copy against it).
PLAN_FEE_FREE = ('KXEURUSD15M', 'KXGBPUSD15M', 'KXUSDJPY15M', 'KXNATGAS15M', 'KXPLATINUM15M', 'KXPALLADIUM15M',
                 'KXCOPPER15M')


def maker_fee(series, override=None):
    """(cents a contract, where it came from)."""
    if override is not None:
        return float(override), 'given by --maker-fee'
    if series in PLAN_FEE_FREE:
        return 0.0, 'the plan: a fee-free LIP-15 series'
    return 0.0, 'ASSUMED: the plan states no maker fee for %s' % series


# ---- the price grid -------------------------------------------------------------------------
def grid_of(raw):
    """A market's REST row's price_ranges as [(start, end, step)] in 1/10,000 $; [] without."""
    try:
        m = json.loads(raw or '{}')
    except ValueError:
        return []
    out = []
    for r in (m.get('price_ranges') or []) if isinstance(m, dict) else []:
        if not isinstance(r, dict):
            return []
        a, b, st = lj.px_of(r.get('start')), lj.px_of(r.get('end')), lj.px_of(r.get('step'))
        if a is None or b is None or not st:
            return []
        out.append((a, b, st))
    return out


def step_at(p, grid):
    """strat/maker.cpp grid_step: the step of the range holding p; 1c without one."""
    for a, b, st in grid:
        if a <= p < b:
            return st
    if grid and p == grid[-1][1]:
        return grid[-1][2]
    return CENT


def move(p, ticks, grid):
    """p moved by ticks grid steps: up when ticks > 0, down when below."""
    for _ in range(abs(ticks)):
        p = p + step_at(p, grid) if ticks > 0 else p - step_at(p - 1, grid)
    return p


# ---- deskd's LIP share ----------------------------------------------------------------------
def lip_side(levels, bid, target, discount, our_px=None, ours=0):
    """strat/lip.cpp score() for one side: levels [(px, count)] best first, the book's own; ours
    contracts at our_px, last in their level. (score, our score, the side reaches the target)."""
    total = mine = 0.0
    left, best, i, n, todo = target, None, 0, len(levels), ours > 0
    while left > 0:
        px, q = levels[i] if i < n else (None, 0)
        if todo and (px is None or (our_px > px if bid else our_px < px)):
            px, q, m, todo = our_px, 0, ours, False   # a level of our own
        elif todo and px == our_px:
            m, todo, i = ours, False, i + 1
        elif px is None:
            break
        else:
            m, i = 0, i + 1
        if best is None:
            best = px
        counted = min(q + m, left)
        w = discount ** (abs(px - best) / CENT)
        total += counted * w
        mine += max(0, counted - q) * w
        left -= counted
    return total, mine, left <= 0


def markout(bid, px, mid2):
    """Cents a contract in our favour; mid2 is twice the mid (best bid + best ask)."""
    return ((mid2 / 2 - px) if bid else (px - mid2 / 2)) / CENT


# ---- the replay -----------------------------------------------------------------------------
def frame_key(r):
    return (r.get('recv_us'), r.get('conn'), r.get('sid'), r.get('seq'))


def pair_prints(data, keep):
    """The first pass over the frames: each print paired with the removals at its price, on the
    side it hit, received less than MATCH_US before or after it: one of exactly the print's count
    first, then the nearest in time; prints in the order received. (executed: a removal's frame ->
    the count prints account for; paired: a print's frame -> [(the removal's recv_us, count)])."""
    removals, waiting, executed, paired = {}, deque(), {}, {}

    def settle(t, key, level, q):
        cands = [r for r in removals.get(level, ()) if r[2] and abs(r[0] - t) < MATCH_US]
        cands.sort(key=lambda r: (r[2] != q, abs(r[0] - t)))
        for r in cands:
            if not q:
                break
            x = min(q, r[2])
            r[2] -= x
            q -= x
            executed[r[1]] = executed.get(r[1], 0) + x
            paired.setdefault(key, []).append((r[0], x))

    for r in lj.rows(data, 'kalshi_ws'):
        t, typ = r.get('recv_us'), r.get('type')
        if not isinstance(t, int) or isinstance(t, bool):
            continue
        while waiting and waiting[0][0] + MATCH_US <= t:   # every removal it could pair with is in
            settle(*waiting.popleft())
        tk = r.get('ticker')
        if typ not in ('orderbook_delta', 'trade') or not tk or not keep(tk):
            continue
        try:
            msg = json.loads(r.get('raw') or '{}').get('msg')
        except (ValueError, AttributeError):
            continue
        if not isinstance(msg, dict):
            continue
        if typ == 'trade':
            px, q, side = lj.px_of(msg.get('yes_price_dollars')), lj.qty_of(msg.get('count_fp')), msg.get('taker_side')
            if px is not None and q and q > 0 and side in ('yes', 'no'):
                waiting.append((t, frame_key(r), (tk, side == 'no', px), q))
            continue
        side, px, d = msg.get('side'), lj.px_of(msg.get('price_dollars')), lj.qty_of(msg.get('delta_fp'), True)
        if side in ('yes', 'no') and px is not None and d is not None and d < 0:
            level = removals.setdefault((tk, side == 'yes', px if side == 'yes' else UNIT - px), deque())
            while level and level[0][0] < t - 3 * MATCH_US:
                level.popleft()
            level.append([t, frame_key(r), -d])
    while waiting:
        settle(*waiting.popleft())
    return executed, paired


class Order:
    __slots__ = ('place', 'bid', 'tk', 'px', 't0', 'size', 'left', 'ahead', 'behind', 'fills', 'lip', 'lost')

    def __init__(self, place, bid, tk, px, t0, size, ahead):
        self.place, self.bid, self.tk, self.px, self.t0 = place, bid, tk, px, t0
        self.size = self.left = size
        self.ahead, self.behind = ahead, 0
        self.fills = []                 # [t_us, count, mo10, mo60]; a markout None until marked
        self.lip = [0.0] * len(CAPS)    # LIP dollars within each cap
        self.lost = None                # age (us) at which a dropped book ended it


class Replay(lj.Replay):
    now = 0
    cur = None   # the frame being applied (frame_key)

    def row(self, r):
        t = r.get('recv_us')
        if isinstance(t, int):
            self.now = t
        self.cur = frame_key(r)
        super().row(r)


class Sim:
    def __init__(self, data, series, size=10, every=10, front=False):
        self.data, self.series, self.size, self.every, self.front = data, series, size * CT, every * S, front
        self.markets = lj.load_markets(data)
        self.terms = lj.Terms(lj.load_programs(data), self.markets)
        self.mine = mine = lambda tk: lj.series_of(tk, self.markets) == series   # noqa: E731
        self.rp = Replay(keep=mine)
        self.rp.on_drop, self.rp.on_level, self.rp.on_trade = self.dropped, self.level, self.trade
        self.wins, self.lip_terms, self.lip_src = [], {}, Counter()
        for tk in sorted(set(self.markets) | set(self.terms.own)):
            if not mine(tk):
                continue
            m, own = self.markets.get(tk), self.terms.own.get(tk) or []
            if m and m['open_us'] is not None and m['close_us'] is not None and m['close_us'] > m['open_us']:
                self.wins.append((m['open_us'], m['close_us'], tk))
            else:
                self.wins.extend((p['start_us'], p['end_us'], tk) for p in own)
            terms = []
            for p in own:
                tgt = p['target'] or self.terms.target(tk, series)[0]
                terms.append((p['start_us'], p['end_us'], p, tgt))
            latest = self.terms.latest.get(series)
            if not terms and latest is not None and latest['target'] and m and m['open_us'] is not None \
                    and m['close_us'] is not None:
                terms.append((m['open_us'], m['close_us'], latest, latest['target']))
                self.lip_src['series'] += 1
            elif terms:
                self.lip_src['own'] += 1
            else:
                self.lip_src['none'] += 1
            self.lip_terms[tk] = terms
        self.wins.sort()
        self.grids = {}
        self.live = {}          # ticker -> its live orders
        self.orders = []        # every order placed
        self.executed, self.paired = {}, {}   # pair_prints()
        self.timers = []
        self.ntimer = 0
        self.active, self.nxt = {}, 0
        self.placed_markets = set()
        self.skipped = Counter()
        self.span = (None, None)

    # -- placing and ending orders
    def grid(self, tk):
        g = self.grids.get(tk)
        if g is None:
            m = self.markets.get(tk)
            g = self.grids[tk] = grid_of(m['raw']) if m else []
        return g

    def place(self, tk, s):
        book = self.rp.books.get(tk)
        if book is None:
            return
        self.placed_markets.add(tk)
        for bid in (True, False):
            touch, other = (book.best_bid(), book.best_ask()) if bid else (book.best_ask(), book.best_bid())
            for name, k in PLACEMENTS:
                px = move(touch, k if bid else -k, self.grid(tk)) if touch is not None else None
                if px is None or px <= 0 or px >= UNIT or (other is not None and (px >= other if bid else px <= other)):
                    self.skipped[name] += 1
                    continue
                o = Order(name, bid, tk, px, s, self.size, (book.bids if bid else book.asks).get(px, 0))
                self.orders.append(o)
                self.live.setdefault(tk, []).append(o)

    def finish(self, o):
        self.live[o.tk].remove(o)

    def dropped(self, tk):
        for o in list(self.live.get(tk, ())):
            o.lost = max(0, self.rp.now - o.t0)
            self.finish(o)

    # -- the queue
    def cancel(self, o, c):
        if self.front:
            a = min(o.ahead, c)
            o.ahead -= a
            o.behind -= min(o.behind, c - a)
        else:
            b = min(o.behind, c)
            o.behind -= b
            o.ahead -= min(o.ahead, c - b)

    def level(self, tk, bid, px, before, after, t):
        live = self.live.get(tk)
        if not live:
            return
        if after > before:
            d = after - before
            for o in list(live):
                if o.bid == bid:
                    if o.px == px:
                        o.behind += d   # joins behind it
                elif (px <= o.px) if o.bid else (px >= o.px):
                    self.fill(o, min(o.left, d), t)   # the other side at or through it, no print
            return
        c = before - after - self.executed.get(self.rp.cur, 0)   # what no print accounts for
        if c > 0:
            for o in live:
                if o.bid == bid and o.px == px:
                    self.cancel(o, c)   # against the queue as it stands now

    def trade(self, tk, p, q, taker_yes, t):
        bid = not taker_yes   # a taker selling YES hits the bids
        live = self.live.get(tk)
        if not live:
            return
        pre = self.paired.get(self.rp.cur, ())
        for o in list(live):
            if o.bid != bid:
                continue
            v = q - sum(x for tr, x in pre if tr <= o.t0)   # removed before the order was placed: not in its queue
            if v <= 0:
                continue
            if o.px == p:
                a = min(o.ahead, v)
                o.ahead -= a
                f = min(o.left, v - a)
                o.behind -= min(o.behind, v - a - f)
            elif p < o.px if bid else p > o.px:
                f = min(o.left, v)
                o.ahead = 0
            else:
                continue
            if f:
                self.fill(o, f, t)

    def fill(self, o, f, t):
        o.left -= f
        rec = [t, f, None, None]
        o.fills.append(rec)
        for hi, h in enumerate(HORIZONS):
            self.ntimer += 1
            heapq.heappush(self.timers, (t + h * S, self.ntimer, lambda o=o, rec=rec, hi=hi, h=h: self.mark(o, rec, hi, h)))
        if not o.left:
            self.finish(o)

    def mark(self, o, rec, hi, h):
        m = self.markets.get(o.tk)
        if m and m['close_us'] is not None and rec[0] + h * S >= m['close_us']:
            return   # past the close: not a market any more
        book = self.rp.books.get(o.tk)
        if book is None or not book.bids or not book.asks:
            return
        rec[2 + hi] = markout(o.bid, o.px, book.best_bid() + book.best_ask())

    # -- each second
    def terms_at(self, tk, s):
        for a, b, p, tgt in self.lip_terms.get(tk, ()):
            if a <= s < b and p['reward'] > 0:
                return p['reward'] / 10000.0 / ((p['end_us'] - p['start_us']) / S), tgt, p['discount_bps'] / 10000.0
        return None

    def second(self, s):
        cap = CAPS[-1] * S
        for orders in self.live.values():
            for o in [o for o in orders if s - o.t0 >= cap]:
                self.finish(o)
        if s % self.every == 0:
            while self.nxt < len(self.wins) and self.wins[self.nxt][0] <= s:
                self.active[self.nxt] = self.wins[self.nxt]
                self.nxt += 1
            for k in [k for k, w in self.active.items() if w[1] <= s]:
                del self.active[k]
            for a, b, tk in self.active.values():
                if s + cap <= b:
                    self.place(tk, s)
        for tk, orders in self.live.items():
            if not orders:
                continue
            terms = self.terms_at(tk, s)
            if terms is None:
                continue
            per_s, target, disc = terms
            book = self.rp.books[tk]
            sides = {True: sorted(book.bids.items(), reverse=True), False: sorted(book.asks.items())}
            reach = {k: sum(q for _, q in v) >= target for k, v in sides.items()}
            for o in orders:
                if not reach[not o.bid]:
                    continue
                total, ours, reaches = lip_side(sides[o.bid], o.bid, target, disc, o.px, o.left)
                if not reaches or total <= 0:
                    continue
                usd = per_s * 0.5 * ours / total
                for ci, c in enumerate(CAPS):
                    if s - o.t0 < c * S:
                        o.lip[ci] += usd

    def run(self):
        self.executed, self.paired = pair_prints(self.data, self.mine)
        self.span = lj.drive(self.data, self.rp, S, self.second, lj.down_instants(self.data), self.timers)
        end = self.span[1]
        for orders in self.live.values():
            for o in orders:
                o.lost = max(0, end - o.t0)   # the data ended first
        return self

    # -- the results
    def results(self, fee):
        rows = []
        have_lip = any(self.lip_terms.values())
        for name, _ in PLACEMENTS:
            for ci, c in enumerate(CAPS):
                cap = c * S
                orders = [o for o in self.orders if o.place == name and (o.lost is None or o.lost >= cap)]
                firsts, full, cts, lip = [], 0, 0, 0.0
                mo = {h: [0.0, 0] for h in HORIZONS}
                for o in orders:
                    fills = [f for f in o.fills if f[0] - o.t0 < cap]
                    if fills:
                        firsts.append((fills[0][0] - o.t0) / S)
                    got = sum(f[1] for f in fills)
                    full += got >= o.size
                    cts += got
                    lip += o.lip[ci]
                    for f in fills:
                        for hi, h in enumerate(HORIZONS):
                            if f[2 + hi] is not None:
                                mo[h][0] += f[2 + hi] * f[1]
                                mo[h][1] += f[1]
                n = len(orders)
                mo10, mo60 = [(mo[h][0] / mo[h][1] if mo[h][1] else None) for h in HORIZONS]
                per_ct = (lip / (cts / CT) if cts else None) if have_lip else None
                net = None if mo60 is None else mo60 - fee + (100 * per_ct if per_ct is not None else 0.0)
                rows.append({'placement': name, 'cap_s': c, 'orders': n, 'fill': len(firsts) / n if n else 0.0,
                             'full': full / n if n else 0.0,
                             'avg_s': sum(firsts) / len(firsts) if firsts else None, 'contracts': cts / CT,
                             'mo10': mo10, 'mo60': mo60, 'marked10': mo[10][1] / CT, 'marked60': mo[60][1] / CT,
                             'lip_usd': lip if have_lip else None, 'lip_per_ct': per_ct, 'net60': net})
        return rows


def simulate(data, series='KXNATGAS15M', size=10, every=10, front=False, fee=None):
    sim = Sim(data, series, size, every, front).run()
    fee_c, fee_why = maker_fee(series, fee)
    first, last = sim.span
    return {'series': series, 'from': lj.iso(first) if first is not None else None,
            'to': lj.iso(last) if last is not None else None, 'markets': len(sim.placed_markets),
            'size': size, 'every_s': every, 'cancels': 'front' if front else 'back', 'maker_fee': fee_c,
            'maker_fee_from': fee_why, 'lip_terms': dict(sim.lip_src), 'book_gaps': sim.rp.gaps,
            'trade_gaps': sim.rp.trade_gaps, 'bad': sum(sim.rp.bad.values()),
            'cut_short': sum(1 for o in sim.orders if o.lost is not None and o.lost < CAPS[-1] * S),
            'skipped': dict(sim.skipped), 'rows': sim.results(fee_c)}


def report(res):
    def f(v, fmt):
        return '-' if v is None else fmt % v
    lip = res['lip_terms']
    lines = ['fillsim2: %s, %d market(s), %s to %s UTC; %d-contract orders every %d s on the YES bid and the YES ask' % (
        res['series'], res['markets'], res['from'] or '-', res['to'] or '-', res['size'], res['every_s']),
        'queue: cancels come from the %s of the queue first%s; a print pairs with the removals at its price '
        'within 1 s (its own count first, then the nearest); the other side arriving at or through an order '
        'fills it' % (res['cancels'], ' (--cancel-front)' if res['cancels'] == 'front' else
                      ' (--cancel-front: from the front)'),
        'maker fee: %.2fc a contract (%s)' % (res['maker_fee'], res['maker_fee_from']),
        'LIP (an estimate, deskd strat/lip.h each second): %d market(s) on their own program, %d on their series\' '
        'latest terms, %d without' % (lip.get('own', 0), lip.get('series', 0), lip.get('none', 0)),
        'book sequence gaps %d, trade sequence gaps %d (prints lost), bad frames %d; orders cut short by a dropped book '
        'or the data\'s end %d; placements skipped (lock, cross or off the price range): %s' % (
            res['book_gaps'], res['trade_gaps'], res['bad'], res['cut_short'],
            ', '.join('%s %d' % kv for kv in sorted(res['skipped'].items())) or 'none'),
        '%-9s %4s %7s %6s %6s %6s %8s %7s %7s %9s %7s' % ('placement', 'cap', 'orders', 'fill%', 'full%', 'avg_s',
                                                         'cts', 'mo10c', 'mo60c', 'LIP$/ct', 'net60c')]
    if not res['markets']:
        lines.append('(no market of %s had a recorded window and a live book to place on)' % res['series'])
    for r in res['rows']:
        lines.append('%-9s %3ds %7d %6.1f %6.1f %6s %8.1f %7s %7s %9s %7s' % (
            r['placement'], r['cap_s'], r['orders'], 100 * r['fill'], 100 * r['full'], f(r['avg_s'], '%.1f'),
            r['contracts'], f(r['mo10'], '%+.2f'), f(r['mo60'], '%+.2f'), f(r['lip_per_ct'], '%.4f'),
            f(r['net60'], '%+.2f')))
    return '\n'.join(lines)


def run(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--data', help="the recorder's data directory")
    ap.add_argument('--series', default='KXNATGAS15M')
    ap.add_argument('--size', type=int, default=10, help='contracts an order (default 10)')
    ap.add_argument('--every', type=int, default=10, help='seconds between placements (default 10)')
    ap.add_argument('--cancel-front', action='store_true', help='cancels come from the front of the queue first')
    ap.add_argument('--maker-fee', type=float, help='cents a contract, over the plan (or the assumed 0)')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.data or not os.path.isdir(a.data):
        print('error: --data DIR (the recorder\'s data directory) is required', file=sys.stderr)
        return 2
    if a.size <= 0 or a.every <= 0:
        print('error: --size and --every must be above 0', file=sys.stderr)
        return 2
    res = simulate(a.data, a.series, a.size, a.every, a.cancel_front, a.maker_fee)
    print(json.dumps(res, indent=1) if a.json else report(res))
    if res['maker_fee_from'].startswith('ASSUMED'):
        print('note: the maker fee of %s was assumed 0' % a.series, file=sys.stderr)
    return 0


# ---- self-test ------------------------------------------------------------------------------
_T0 = int(datetime(2026, 10, 20, 14, 50, tzinfo=timezone.utc).timestamp()) * S
_M = 'KXNATGAS15M-26OCT201505-05'


def _window(d):
    """A natural-gas window from 14:50 UTC (_T0) to 15:05, its program $20, target 10, discount
    0.5: (the spool, seconds from _T0 -> recv_us, the next book sequence number)."""
    sp = lj.Spool(d)
    sp.market(_T0 - 60 * S, 'KXNATGAS15M', _M, _T0, _T0 + 900 * S,
              {'price_ranges': [{'start': '0.0000', 'end': '1.0000', 'step': '0.0100'}]})
    sp.program(_T0 - 60 * S, _M, 'liquidity', _T0, _T0 + 900 * S, 10, 5000, 200000)
    seq = iter(range(1, 10000))
    return sp, (lambda sec: _T0 + int(round(sec * S))), (lambda: next(seq))


def _crossed(d):
    """Bids 50c x300, asks 52c x300. At 3 s an ask of 4 comes in at 51c, the improve bid's price,
    with no print; at 4 s the 50c bids go, a bid comes in at 47c and an ask of 50 at 49c: at or
    through the join bid (50c), behind1 (49c) and the improve bid's last 6 (51c), again no print."""
    sp, at, n = _window(d)
    sp.snapshot(at(-5), 1, 1, n(), _M, [(0.50, 300)], [(0.48, 300)])
    sp.delta(at(3), 1, 1, n(), _M, 'no', 0.49, 4)
    sp.delta(at(4), 1, 1, n(), _M, 'yes', 0.50, -300)
    sp.delta(at(4.01), 1, 1, n(), _M, 'yes', 0.47, 100)
    sp.delta(at(4.02), 1, 1, n(), _M, 'no', 0.51, 50)
    sp.delta(at(130), 1, 1, n(), _M, 'yes', 0.46, 1)


def _requeue(d, add_s):
    """Bids 50c x30: the join bid has 30 ahead, nobody behind. At 2 s 10 are cancelled at 50c and
    re-posted add_s later; 25 sell at 50c at 8 s."""
    sp, at, n = _window(d)
    sp.snapshot(at(-5), 1, 1, n(), _M, [(0.50, 30)], [(0.48, 100)])
    sp.delta(at(2.0), 1, 1, n(), _M, 'yes', 0.50, -10)
    sp.delta(at(2.0 + add_s), 1, 1, n(), _M, 'yes', 0.50, 10)
    sp.trade(at(8.0), 1, 2, 1, _M, 0.50, 25, 'no')
    sp.delta(at(8.01), 1, 1, n(), _M, 'yes', 0.50, -25)
    sp.delta(at(130), 1, 1, n(), _M, 'yes', 0.40, 1)


def _early_cancel(d, delta_first):
    """Bids 50c x30. 5 cancelled and re-posted at 50c 0.4 s before the placement; 20 join behind the
    join bid 0.2 s after it; 30 sell at 50c at 0.3 s, the print and its removal 10 ms apart (the
    removal first when delta_first); 10 sell at 5 s."""
    sp, at, n = _window(d)
    sp.snapshot(at(-5), 1, 1, n(), _M, [(0.50, 30)], [(0.48, 100)])
    sp.delta(at(-0.4), 1, 1, n(), _M, 'yes', 0.50, -5)
    sp.delta(at(-0.39), 1, 1, n(), _M, 'yes', 0.50, 5)
    sp.delta(at(0.2), 1, 1, n(), _M, 'yes', 0.50, 20)
    if delta_first:
        sp.delta(at(0.29), 1, 1, n(), _M, 'yes', 0.50, -30)
        sp.trade(at(0.3), 1, 2, 1, _M, 0.50, 30, 'no')
    else:
        sp.trade(at(0.3), 1, 2, 1, _M, 0.50, 30, 'no')
        sp.delta(at(0.31), 1, 1, n(), _M, 'yes', 0.50, -30)
    sp.trade(at(5.0), 1, 2, 2, _M, 0.50, 10, 'no')
    sp.delta(at(5.01), 1, 1, n(), _M, 'yes', 0.50, -10)
    sp.delta(at(130), 1, 1, n(), _M, 'yes', 0.40, 1)


def self_test():
    fails = []

    def check(name, ok, detail=''):
        print('%s %s%s' % ('OK  ' if ok else 'FAIL', name, (': ' + str(detail)) if detail else ''))
        if not ok:
            fails.append(name)

    def near(a, b, tol=1e-6):
        return a is not None and abs(a - b) <= tol

    # the plan's fee-free series are docs/edge_results.json's LIP-15 markets
    named = []
    try:
        with open(os.path.join(ROOT, 'docs', 'edge_results.json'), encoding='utf-8') as fh:
            for e in json.load(fh)['hunt:financial']['candidates']:
                if isinstance(e, dict) and str(e.get('name', '')).startswith('LIP-15'):
                    named = re.findall(r'KX[A-Z0-9]+', e.get('markets', '').split('LIP terms')[0])
    except (OSError, ValueError, KeyError, TypeError):
        pass
    check("the fee-free series are the plan's seven (docs/edge_results.json)", sorted(named) == sorted(PLAN_FEE_FREE),
          named)
    check('maker fee: 0 by the plan for natural gas, 0 assumed elsewhere, --maker-fee over both',
          maker_fee('KXNATGAS15M') == (0.0, 'the plan: a fee-free LIP-15 series')
          and maker_fee('KXGOLD15M')[1].startswith('ASSUMED') and maker_fee('KXNATGAS15M', 1.5)[0] == 1.5)
    # the grid: 1c without one; a tapered one's steps (0.1c under 10c and over 90c)
    tapered = [(0, 1000, 10), (1000, 9000, 100), (9000, 10000, 10)]
    check('ticks: grid_step and its moves', move(5000, 1, []) == 5100 and move(5000, -2, []) == 4800
          and move(1000, -1, tapered) == 990 and move(990, 1, tapered) == 1000 and move(8900, 2, tapered) == 9010
          and grid_of(json.dumps({'price_ranges': [{'start': '0.0000', 'end': '1.0000', 'step': '0.0100'}]}))
          == [(0, 10000, 100)])
    # deskd's LIP score by hand: target 10, discount 0.5, ours 4 alone at 51c, 100 at 52c: 4 + 6 x 0.5
    tot, ours, reaches = lip_side([(5200, 10000)], False, 1000, 0.5, 5100, 400)
    check('lip_side: 4 of ours and 6 of 100 a cent away at half weight: share 4/7', reaches and near(tot, 700)
          and near(ours, 400), (tot, ours, reaches))
    tot, ours, reaches = lip_side([(5000, 3000)], True, 1000, 0.5, 5000, 1000)
    check('lip_side: ours last in a level the target cuts: no share', reaches and near(tot, 1000) and ours == 0)
    check('markout signs: a bid bought at 50c with the mid at 49.5c is -0.5c, an ask sold there +0.5c',
          near(markout(True, 5000, 9900), -0.5) and near(markout(False, 5000, 9900), 0.5))

    # One natural-gas window from t0 = 14:50 UTC, the program $20, target 10, discount 0.5; one
    # placement instant (t0, with --every 600). The book at t0 (YES prices): bids 50c 30, 49c 40,
    # 48c 50; asks 52c 100, 53c 100. The bids: join 50c (30 ahead), improve 51c, behind1 49c (40
    # ahead), behind2 48c (50 ahead); the asks: join 52c, improve 51c, behind1 53c, behind2 54c.
    t0 = int(datetime(2026, 10, 20, 14, 50, tzinfo=timezone.utc).timestamp()) * S
    M = 'KXNATGAS15M-26OCT201505-05'
    with tempfile.TemporaryDirectory() as d:
        sp = lj.Spool(d)
        sp.market(t0 - 60 * S, 'KXNATGAS15M', M, t0, t0 + 900 * S,
                  {'price_ranges': [{'start': '0.0000', 'end': '1.0000', 'step': '0.0100'}]})
        sp.program(t0 - 60 * S, M, 'liquidity', t0, t0 + 900 * S, 10, 5000, 200000)

        def at(sec):
            return t0 + int(round(sec * S))
        seq = iter(range(1, 100))
        sp.snapshot(at(-5), 1, 1, next(seq), M, [(0.50, 30), (0.49, 40), (0.48, 50)], [(0.48, 100), (0.47, 100)])
        sp.delta(at(2.5), 1, 1, next(seq), M, 'yes', 0.50, -10)    # two cancels ahead of the 50c bid (nobody
        sp.delta(at(3.5), 1, 1, next(seq), M, 'yes', 0.50, -10)    # behind it): 30 -> 10 ahead
        sp.delta(at(4.5), 1, 1, next(seq), M, 'yes', 0.50, 20)     # 20 join behind it
        sp.delta(at(5.5), 1, 1, next(seq), M, 'yes', 0.50, -5)     # a cancel: from behind (back first)
        sp.trade(at(6.5), 1, 2, 1, M, 0.50, 15, 'no')              # 15 sold at 50c: 10 ahead, then 5 of ours
        sp.delta(at(6.7), 1, 1, next(seq), M, 'yes', 0.50, -15)    # that print's removal
        sp.delta(at(11.5), 1, 1, next(seq), M, 'yes', 0.50, -10)   # the bid falls to 48c, an ask comes in at 51c
        sp.delta(at(11.51), 1, 1, next(seq), M, 'yes', 0.49, -40)
        sp.delta(at(11.52), 1, 1, next(seq), M, 'no', 0.49, 50)
        sp.trade(at(30.5), 1, 2, 2, M, 0.51, 6, 'yes')             # 6 bought at 51c: the improve ask's
        sp.delta(at(30.7), 1, 1, next(seq), M, 'no', 0.49, -6)
        sp.trade(at(40.0), 1, 2, 4, M, 0.48, 8, 'no')              # trade seq 4 after 2: a trade gap
        sp.delta(at(40.2), 1, 1, next(seq), M, 'yes', 0.48, -8)
        sp.delta(at(50.5), 1, 1, next(seq), M, 'yes', 0.49, 30)    # mid 50.5c from here
        sp.delta(at(50.52), 1, 1, next(seq), M, 'no', 0.49, -44)
        sp.delta(at(130), 1, 1, next(seq), M, 'no', 0.47, 1)       # the data's last frame
        res = simulate(d, 'KXNATGAS15M', 10, 600)
        rows = {(r['placement'], r['cap_s']): r for r in res['rows']}
        j5, j15, j60 = rows[('join', 5)], rows[('join', 15)], rows[('join', 60)]
        # the 50c bid: 2 cancels take it from 30 ahead to 10, the 5-lot cancel at 5.5 s comes from
        # behind, so the 15 sold at 6.5 s reach it after 10: 5 filled. At 40 s 8 sold at 48c, through
        # it: its last 5. Mids: 49.5c at 16.5 s and 50 s, 50.5c at 66.5 s and 100 s.
        check('the 5 s cap expires first: no join order filled within 5 s', j5['orders'] == 2 and j5['fill'] == 0)
        check('join, 15 s: the print reaches the bid after the 2 cancels ahead: 5 at 6.5 s',
              (j15['fill'], j15['full'], j15['avg_s'], j15['contracts']) == (0.5, 0.0, 6.5, 5.0), j15)
        check('join, 15 s: markouts -0.5c (10 s) and +0.5c (60 s)', near(j15['mo10'], -0.5) and near(j15['mo60'], 0.5),
              (j15['mo10'], j15['mo60']))
        check('join, 60 s: the print through at 40 s fills the rest', (j60['full'], j60['contracts']) == (0.5, 10.0)
              and near(j60['mo10'], -0.5) and near(j60['mo60'], 0.5), j60)
        # LIP: the join bid scores only once it is alone at 50c (12-39 s): 500 of 625 (48c's 500 at a
        # quarter), 0.8 x $20/900/2 a second
        check('join LIP $/ct: 3 s (15 s cap) and 28 s (60 s cap) at 0.8 of a side, over 5 and 10 contracts',
              near(j15['lip_per_ct'], 3 * 0.8 * 20 / 900 / 2 / 5) and near(j60['lip_per_ct'], 28 * 0.8 * 20 / 900 / 2 / 10),
              (j15['lip_per_ct'], j60['lip_per_ct']))
        i15, i60 = rows[('improve', 15)], rows[('improve', 60)]
        # the 51c bid fills 10 at 6.5 s through the 50c print; the 51c ask 6 at 30.5 s. The bid's
        # markouts -1.5c and -0.5c, the ask's +1.5c and +0.5c (the mid fell after it sold).
        check('improve, 15 s: the bid filled through: 10 at 6.5 s, -1.5c and -0.5c',
              (i15['fill'], i15['contracts'], i15['avg_s']) == (0.5, 10.0, 6.5) and near(i15['mo10'], -1.5)
              and near(i15['mo60'], -0.5), i15)
        check('improve, 60 s: both filled, 16 contracts, a markout sign per side: (10 x -1.5 + 6 x 1.5) / 16',
              (i60['fill'], i60['full'], i60['avg_s'], i60['contracts']) == (1.0, 0.5, 18.5, 16.0)
              and near(i60['mo10'], -0.375) and near(i60['mo60'], -0.125), i60)
        lip_bid = 7 * 20 / 900 / 2                                     # alone at 51c, 0-6 s
        lip_ask15 = 12 * 20 / 900 / 2                                  # alone at 51c, 0-11 s
        lip_ask60 = lip_ask15 + 9 * (4 / 7) * 20 / 900 / 2           # 4 left alone again, 51-59 s
        check('improve LIP $/ct: (bid + ask) over 10 and 16 contracts', near(i15['lip_per_ct'], (lip_bid + lip_ask15) / 10)
              and near(i60['lip_per_ct'], (lip_bid + lip_ask60) / 16), (i15['lip_per_ct'], i60['lip_per_ct']))
        check('net60 = the 60 s markout - the fee (0) + LIP in cents', near(i60['net60'], -0.125 + 100 * i60['lip_per_ct']))
        b15, b60 = rows[('behind1', 15)], rows[('behind1', 60)]
        check('behind1: the 49c bid fills only once 48c trades through it: 8 at 40 s, +0.5c and +1.5c',
              b15['fill'] == 0 and (b60['fill'], b60['avg_s'], b60['contracts']) == (0.5, 40.0, 8.0)
              and near(b60['mo10'], 0.5) and near(b60['mo60'], 1.5), b60)
        # behind1's LIP: alone at 49c from 12 s (1 of the side), 200 left from 40 s (200 of 200 + 800 x 0.5),
        # cut off at 51 s by 30 joining at 49c
        check('behind1 LIP $/ct: 28 s at 1 and 11 s at 1/3 of a side, over 8 contracts',
              near(b60['lip_per_ct'], (28 + 11 / 3) * 20 / 900 / 2 / 8), b60['lip_per_ct'])
        check('behind2: nothing reaches 48c with 50 ahead', all(rows[('behind2', c)]['contracts'] == 0 for c in CAPS))
        check('the trade gap is counted, the fee is the plan\'s', res['trade_gaps'] == 1 and res['book_gaps'] == 0
              and res['maker_fee'] == 0.0 and res['maker_fee_from'].startswith('the plan'),
              (res['trade_gaps'], res['book_gaps'], res['maker_fee'], res['maker_fee_from']))
        front = {(r['placement'], r['cap_s']): r for r in simulate(d, 'KXNATGAS15M', 10, 600, front=True)['rows']}
        check('--cancel-front: the 5.5 s cancel comes from ahead, so the print fills all 10 at 6.5 s',
              (front[('join', 15)]['contracts'], front[('join', 15)]['full']) == (10.0, 0.5), front[('join', 15)])
        check('the CLI runs', run(['--data', d, '--every', '600']) == 0)

    def rows_of(build, *args):
        with tempfile.TemporaryDirectory() as d:
            build(d, *args)
            return {(r['placement'], r['cap_s']): r for r in simulate(d, 'KXNATGAS15M', 10, 600)['rows']}

    # the other side arriving at or through a resting order, with no print, fills it at its price
    c = rows_of(_crossed)
    ci, cj, cb = c[('improve', 5)], c[('join', 5)], c[('behind1', 5)]
    check('crossed, no print: the improve bid fills 4 at 3 s, its last 6 at 4.02 s, at 51c; the mid then 48c',
          (ci['fill'], ci['full'], ci['avg_s'], ci['contracts']) == (0.5, 0.5, 3.0, 10.0) and near(ci['mo10'], -3.0)
          and near(ci['mo60'], -3.0), ci)
    check('crossed, no print: the join bid (50c) and behind1 (49c, at its price) fill 10 at 4.02 s',
          cj['contracts'] == 10.0 and near(cj['avg_s'], 4.02) and near(cj['mo10'], -2.0)
          and cb['contracts'] == 10.0 and near(cb['mo10'], -1.0) and c[('behind2', 5)]['contracts'] == 0, (cj, cb))
    # LIP stops when the bid is filled, and its last 6 earn after the partial fill: the bid 1 a second
    # for 0-2 s and 0.75 at 3 s (6 of 6 + 4 x 0.5 at 50c); the ask 1 for 0-2 s and 0.6 at 3 s (6 of the
    # 10 counted, 4 others ahead of ours at 51c); nothing at 4 s (no bids), then the 49c ask is first
    check('crossed: LIP stops once the improve bid is filled; 7.35 side-seconds in all, at every cap',
          all(near(c[('improve', cap)]['lip_usd'], 7.35 * 20 / 900 / 2) for cap in CAPS),
          [c[('improve', cap)]['lip_usd'] for cap in CAPS])
    # a cancel is settled against the queue as it was when it came: nobody was behind, so it moves
    # the order up, and the re-post goes behind it, however soon after it comes
    for add_s in (0.1, 1.2):
        r = rows_of(_requeue, add_s)[('join', 15)]
        check('a cancel at 2 s with the re-post %.1f s later: 30 -> 20 ahead, 25 sold fill 5' % add_s,
              r['contracts'] == 5.0 and r['avg_s'] == 8.0, r)
    # a print pairs with its own removal (the same count, 10 ms away), not with a cancel from
    # before the placement: all 30 ahead trade, so the next 10 sold are the order's
    for delta_first in (False, True):
        r = rows_of(_early_cancel, delta_first)[('join', 15)]
        check('a cancel 0.4 s before the placement, a print of 30 and its removal (%s first): the next 10 fill it'
              % ('removal' if delta_first else 'print'), (r['contracts'], r['full'], r['avg_s']) == (10.0, 0.5, 5.0), r)
    print('fillsim2 self-test: %s' % ('ok' if not fails else 'FAILED (%d)' % len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(run(sys.argv[1:]))
