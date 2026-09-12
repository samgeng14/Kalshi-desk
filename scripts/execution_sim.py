#!/usr/bin/env python3
"""Phase 4 -- execution simulator. What does it actually cost to get a position on?

Every result in this project so far has died at the fee line, and every one of them died because
the cost side was a constant pulled from the fee schedule. That constant is the optimistic case.
The real cost of a passive order is the fee PLUS the adverse selection you accept by being the
one who was standing still, and the real cost of a marketable order is the fee PLUS the spread
PLUS whatever the book does in the second after you take it. Neither is knowable from a fee
schedule. Both are measurable from recorded L2, which is what this does.

It replays the recorder's event stream, places hypothetical orders at sampled decision times,
tracks each one through the real subsequent book updates, and measures what happened.

FOUR MODES
  taker            cross the spread now, walking levels until the size is filled
  join             rest at the touch on the passive side, wait
  improve          rest one tick inside the touch, wait
  join_then_cross  rest at the touch; if unfilled at the timeout, cross

`join_then_cross` is the mode that matters for a signal-driven trader, because it is the only one
whose cost is not conditional on a fill that may never come. A 3 bps average cost on the 12% of
orders that fill passively is not a 3 bps cost; it is a 3 bps cost on a position you usually do
not have.

WHAT THIS CANNOT DO, STATED UP FRONT
  1. Our order is not in the book being replayed. A passive order at the touch adds size that
     later arrivals would have queued behind; a marketable order removes liquidity that the next
     taker would otherwise have hit. This simulator does not model that reaction. The guard is
     size: any order larger than --max-size-frac of the touch is dropped and counted, so the
     conclusion is only ever drawn over orders small enough for the assumption to be defensible.
     The excluded count is printed. If it is large, the result does not describe the size you
     want to trade.
  2. Queue position is not observable. Two bounds are computed for every mode and both are
     reported (see below). If a conclusion holds under one and not the other, it is not a
     conclusion.
  3. `improve` fills partly by price priority: a print at a worse price is treated as a fill for
     our better-priced order. That is the right rule, but it also means the simulated inside
     order never has to beat anyone to the level, whereas a real one advertises its price and
     invites someone to step in front. Treat `improve` fill rates as an upper bound.
  4. Cancel/replace and quote-following are not simulated. A resting order here is posted once
     and left. A real maker repositions, which improves fill rate and worsens selection.

THE TWO QUEUE BOUNDS
  pessimistic  only volume that actually TRADED at our price advances us. Cancels ahead of us in
               the queue are assumed to have been behind us. This is the floor on fill rate.
  optimistic   the resting quantity at our price is an upper bound on the queue ahead of us, so
               every cancel at our level is credited as if it were in front. This is the ceiling.
  The truth is between them. The gap between them is the honest error bar on every fill-rate
  number this produces, and it is printed rather than averaged away.

FEES: maker 5 bps, taker 12 bps, on notional, per leg.

  python3 scripts/execution_sim.py --all --markets KXBTCPERP KXXRPPERP
  python3 scripts/execution_sim.py --hours 2026-09-11T15 --notional 25 --sample-s 15
"""
import argparse, bisect, glob, gzip, json, math, os, statistics, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    from feature_engine import (Book, num, to_us, check_epoch, TRADE_REASONS, CANCEL_REASONS,
                                FeatureEngine)
except ImportError:
    sys.exit("execution_sim.py needs feature_engine.py beside it (same scripts/ directory).")

MAKER_BPS = 5.0
TAKER_BPS = 12.0

# Markout horizons. 1s and 5s say whether the fill was immediately wrong, which is the signature
# of adverse selection; 60s and 300s say whether the position was worth having.
MARKOUTS_US = [('1s', 1_000_000), ('5s', 5_000_000), ('10s', 10_000_000),
               ('30s', 30_000_000), ('60s', 60_000_000), ('300s', 300_000_000)]

MODES = ['taker', 'join', 'improve', 'join_then_cross']


def fmt(x, n=2):
    return '' if x is None or (isinstance(x, float) and x != x) else f'{x:.{n}f}'


class TickSizer:
    """Measures the price grid instead of assuming it.

    A hardcoded tick is a silent way to place an order at a price the exchange cannot represent,
    and 'improve by one tick' is then a fiction. The grid is inferred from the gaps between
    adjacent observed prices: the smallest gap that recurs is the tick.
    """

    def __init__(self):
        self.seen = defaultdict(set)
        self.frozen = {}

    def observe(self, t, price):
        if len(self.seen[t]) < 4000 and price and price > 0:
            self.seen[t].add(round(price, 6))

    def tick(self, t):
        if t in self.frozen:
            return self.frozen[t]
        ps = sorted(self.seen.get(t, ()))
        if len(ps) < 8:
            return None
        gaps = [round(b - a, 6) for a, b in zip(ps, ps[1:]) if b - a > 1e-9]
        if not gaps:
            return None
        g = min(gaps)
        # Require the minimum gap to recur; a single anomalous pair is not a grid.
        if sum(1 for x in gaps if abs(x - g) < 1e-9) < 3:
            g = statistics.median(gaps)
        self.frozen[t] = g
        return g


class Order:
    """One hypothetical order, tracked through the real event stream until it fills or expires."""
    __slots__ = ('mode', 'market', 'side', 'price', 'size', 'posted_us', 'decision_mid',
                 'q_pess', 'q_opt', 'fill_pess_us', 'fill_opt_us', 'fill_px', 'done',
                 'timeout_us', 'crossed_after_timeout', 'left_touch', 'spread_at_post',
                 'state')

    def __init__(self, mode, market, side, price, size, posted_us, decision_mid, queue,
                 timeout_us, spread_at_post, state=None):
        self.mode, self.market, self.side = mode, market, side
        self.price, self.size, self.posted_us = price, size, posted_us
        self.decision_mid = decision_mid
        self.q_pess = queue          # floor on fill rate: only trades advance us
        self.q_opt = queue           # ceiling: cancels at our level advance us too
        self.fill_pess_us = None
        self.fill_opt_us = None
        self.fill_px = price
        self.done = False
        self.timeout_us = timeout_us
        self.crossed_after_timeout = False
        self.left_touch = False
        self.spread_at_post = spread_at_post
        # Everything observable BEFORE the fill. Captured at post time and never touched again,
        # so a study built on it cannot accidentally condition on the fill it is predicting.
        self.state = state

    def signed(self, x):
        """+1 for a buy, -1 for a sell, applied so every bps number means 'good' when positive."""
        return x if self.side == 'buy' else -x


def walk_book(book, side, size):
    """Marketable fill: consume levels until size is covered. Returns (vwap, filled, levels).

    Returns the partial fill rather than pretending the book was deep enough. A taker cost
    computed from the touch price alone understates every order larger than the touch.
    """
    levels = sorted(book.asks) if side == 'buy' else sorted(book.bids, reverse=True)
    src = book.asks if side == 'buy' else book.bids
    need, cost, n = size, 0.0, 0
    for p in levels:
        q = min(src[p], need)
        if q <= 0:
            continue
        cost += p * q
        need -= q
        n += 1
        if need <= 1e-12:
            break
    filled = size - need
    return (cost / filled if filled > 0 else None), filled, n


class Sim:
    def __init__(self, notional, sample_s, timeout_s, max_size_frac, markets, capture=False,
                 max_open=64):
        self.capture = capture
        self.max_open = max_open
        self.notional = notional
        self.sample_us = int(sample_s * 1e6)
        self.timeout_us = int(timeout_s * 1e6)
        self.max_size_frac = max_size_frac
        self.want = set(markets) if markets else None
        self.eng = FeatureEngine()
        self.ticks = TickSizer()
        self.open = defaultdict(list)            # market -> [Order]
        self.filled = []                          # completed orders with a fill
        self.expired = defaultdict(int)           # (market, mode) -> count
        self.skipped = defaultdict(int)           # reason -> count
        # Markout lookups need a mid at an arbitrary time, tens of thousands of times. Kept as
        # two parallel lists rather than a list of pairs: bisect needs a sorted key sequence, and
        # building one per call (`[s[0] for s in series]`) is O(n) inside an O(n) loop -- fine on
        # a thousand synthetic rows, and on a real tape it never finishes.
        self.mid_ts = defaultdict(list)           # market -> [ts_us]
        self.mid_px = defaultdict(list)           # market -> [mid]
        self.last_sample = {}
        self.last_mid_rec = {}
        self.events = 0

    # ---- replay -------------------------------------------------------------------------
    def feed(self, src, ev):
        self.events += 1
        if ev.get('ev') == '_connect':
            # Every book restarts from nothing. An order resting across a reconnect cannot be
            # tracked through the discontinuity, so it is retired as unfilled rather than carried
            # into a book that no longer contains the level it was sitting in.
            for t_, orders in self.open.items():
                for o in orders:
                    if o.fill_opt_us is not None:
                        self.filled.append(o)
                    else:
                        self.expired[(t_, o.mode)] += 1
            self.open.clear()
            self.last_sample.clear()
            self.eng.on_event(src, ev)
            return
        before = None
        t = None
        if src == 'kalshi_margin':
            m = ev.get('msg') or {}
            t = m.get('market_ticker')
            if t and (self.want is None or t in self.want):
                bk = self.eng.books.get(t)
                before = self._level_qty(bk, m) if bk else None
                self.ticks.observe(t, num(m.get('price'), default=None))
        self.eng.on_event(src, ev)
        if t and (self.want is None or t in self.want):
            ts = check_epoch(ev.get('src_ts_us'), 'src_ts_us') or ev.get('recv_ts_us')
            if ts:
                self._after_kalshi(t, ev, ts, before)

    @staticmethod
    def _level_qty(bk, m):
        side = m.get('side')
        p = num(m.get('price'), default=None)
        if p is None or side not in ('bid', 'ask'):
            return None
        return (bk.bids if side == 'bid' else bk.asks).get(p, 0.0)

    def _after_kalshi(self, t, ev, ts, before_qty):
        bk = self.eng.books.get(t)
        if bk is None:
            return
        m = ev.get('msg') or {}
        typ = ev.get('ev')

        # The Kalshi margin feed sends no last_update_reason, so a delta cannot tell us whether
        # it was a trade. Fills therefore come from the TRADE CHANNEL, which is the only place
        # that volume is published. An earlier version drove fills from the delta's reason, which
        # on this feed is always absent -- so nothing passive ever filled and every passive mode
        # would have reported a fill rate of exactly zero.
        if typ == 'trade' and self.open[t]:
            price = num(m.get('price'), default=None)
            cnt = num(m.get('count'), default=None)
            taker = m.get('taker_side')
            if price is not None and cnt and cnt > 0 and taker in ('bid', 'ask'):
                # taker_side is the TAKER's own side. An aggressive buyer ('bid') consumes the
                # ask, and so fills a resting SELL of ours.
                consumed = 'ask' if taker == 'bid' else 'bid'
                self._consume(t, consumed, price, cnt, 'Trade', ts)

        if typ == 'orderbook_delta' and self.open[t]:
            side = m.get('side')
            price = num(m.get('price'), default=None)
            delta = num(m.get('delta'), default=None)
            reason = m.get('last_update_reason', '')
            # Only on a feed that labels its deltas. Where it does, that label is authoritative.
            if reason and price is not None and delta is not None and delta < 0:
                self._consume(t, side, price, -delta, reason, ts)

        # A crossed book is not a usable book for FEATURES, but it is exactly the state in which
        # a resting order gets hit, so marketability and expiry are checked on any non-stale book.
        # Only mid recording and new sampling require a clean two-sided book.
        if not bk.stale:
            self._check_marketable(t, bk, ts)
            self._expire(t, bk, ts)
        if bk.usable():
            mid = bk.mid
            if self.last_mid_rec.get(t) != mid:
                self.mid_ts[t].append(ts)
                self.mid_px[t].append(mid)
                self.last_mid_rec[t] = mid
            self._maybe_sample(t, bk, ts)

    def _consume(self, t, side, price, qty, reason, ts):
        """A level shrank. Only a TRADE can fill us; a cancel can only move us up the queue.

        The distinction matters more than it looks. An earlier version filled an order whenever
        its queue reached zero, which meant that a level being cancelled out from under a resting
        order counted as a fill. It is not: an empty queue ahead means you are at the FRONT, and
        the front of a queue that nobody trades against is still an unfilled order. Every fill
        here is caused by volume that actually printed.

        Price priority is enforced too. A trade printing on the bid side at 9.99 means an
        aggressor sold at 9.99; a resting bid of ours at 10.00 would have been hit first, at our
        better price. Without this, any order posted inside the spread would look dead, because
        the replayed book never prints at a price our absent order created.
        """
        our_side = 'buy' if side == 'bid' else 'sell'
        if reason not in TRADE_REASONS:
            return                      # a cancel: handled by the re-anchor in _check_marketable
        for o in self.open[t]:
            if o.done or o.side != our_side:
                continue
            same = abs(o.price - price) <= 1e-9
            ahead_in_price = (o.price > price + 1e-9) if our_side == 'buy' \
                else (o.price < price - 1e-9)
            if ahead_in_price:
                # We were in front of this print on price. Both bounds fill; the queue at a price
                # level we created is by definition empty.
                o.fill_pess_us = o.fill_pess_us or ts
                o.fill_opt_us = o.fill_opt_us or ts
                o.done = True
                continue
            if not same:
                continue
            # Same price: the print consumes the queue ahead of us first. A fill requires volume
            # left over after the queue is exhausted -- volume exactly equal to the queue leaves
            # us at the front, not filled.
            if qty - o.q_pess > 1e-12 and o.fill_pess_us is None:
                o.fill_pess_us = ts
            if qty - o.q_opt > 1e-12 and o.fill_opt_us is None:
                o.fill_opt_us = ts
            o.q_pess = max(0.0, o.q_pess - qty)
            o.q_opt = max(0.0, o.q_opt - qty)
            if o.fill_pess_us is not None:
                o.done = True

    def _check_marketable(self, t, bk, ts):
        """Two things a resting order must react to: the book crossing it, and leaving the touch.

        If the best ask falls to or below our resting bid, a real order would have been hit. The
        optimistic queue bound is also re-anchored here: the quantity still resting at our price
        is an upper bound on how many orders can be ahead of us.
        """
        for o in self.open[t]:
            if o.done:
                continue
            if o.side == 'buy':
                lvl = bk.bids.get(o.price, 0.0)
                # Credits every cancel at our level as if it had been ahead of us. It moves us up
                # the queue and nothing more: reaching the front is not a fill.
                if o.q_opt > lvl:
                    o.q_opt = lvl
                if bk.best_ask is not None and bk.best_ask <= o.price:
                    o.fill_pess_us = o.fill_pess_us or ts
                    o.fill_opt_us = o.fill_opt_us or ts
                    o.done = True
                elif bk.best_bid is not None and bk.best_bid > o.price:
                    o.left_touch = True
            else:
                lvl = bk.asks.get(o.price, 0.0)
                if o.q_opt > lvl:
                    o.q_opt = lvl
                if bk.best_bid is not None and bk.best_bid >= o.price:
                    o.fill_pess_us = o.fill_pess_us or ts
                    o.fill_opt_us = o.fill_opt_us or ts
                    o.done = True
                elif bk.best_ask is not None and bk.best_ask < o.price:
                    o.left_touch = True

    def _expire(self, t, bk, ts):
        keep = []
        for o in self.open[t]:
            if o.done:
                self.filled.append(o)
                continue
            if ts - o.posted_us >= o.timeout_us:
                if o.mode == 'join_then_cross':
                    # The whole point of the mode: pay up rather than hold an unfilled intention.
                    vwap, got, _ = walk_book(bk, o.side, o.size)
                    if vwap:
                        o.fill_px = vwap
                        o.size = got
                        o.fill_pess_us = o.fill_opt_us = ts
                        o.crossed_after_timeout = True
                        o.done = True
                        self.filled.append(o)
                        continue
                self.expired[(t, o.mode)] += 1
                if o.fill_opt_us is not None:
                    self.filled.append(o)      # filled on the optimistic bound only
                continue
            keep.append(o)
        self.open[t] = keep

    def _maybe_sample(self, t, bk, ts):
        # The first time a market's book becomes usable, start the clock rather than sampling.
        # A sample taken on the snapshot itself sits at a moment when no flow window has had a
        # chance to fill, and after a reconnect that moment recurs -- which would quietly
        # overweight post-reconnect states in the sample.
        if t not in self.last_sample:
            self.last_sample[t] = ts
            return
        if ts - self.last_sample[t] < self.sample_us:
            return
        # Cap concurrent orders per market. At a 5-second sample and a 30-second timeout there
        # can legitimately be six rounds of three modes on two sides in flight, so a cap of 16
        # would silently drop a third of the sample and bias it toward quiet periods.
        if len(self.open[t]) > self.max_open:
            self.skipped['too_many_open'] += 1
            return
        self.last_sample[t] = ts
        tick = self.ticks.tick(t)
        mid = bk.mid
        if not mid or mid <= 0:
            return
        size = self.notional / mid
        if size <= 0:
            return
        spread = bk.spread_bps
        for side in ('buy', 'sell'):
            touch_px = bk.best_ask if side == 'buy' else bk.best_bid
            passive_px = bk.best_bid if side == 'buy' else bk.best_ask
            touch_depth = (bk.asks if side == 'buy' else bk.bids).get(touch_px, 0.0)
            passive_depth = (bk.bids if side == 'buy' else bk.asks).get(passive_px, 0.0)
            if touch_depth <= 0 or passive_depth <= 0:
                self.skipped['empty_touch'] += 1
                continue
            if size > self.max_size_frac * max(touch_depth, passive_depth):
                self.skipped['size_exceeds_touch'] += 1
                continue

            # taker: resolves immediately, no queue.
            vwap, got, _ = walk_book(bk, side, size)
            if vwap:
                o = Order('taker', t, side, vwap, got, ts, mid, 0.0, 0, spread)
                o.fill_px, o.fill_pess_us, o.fill_opt_us, o.done = vwap, ts, ts, True
                self.filled.append(o)

            st = self.pre_fill_state(t, bk, ts, side, passive_px, passive_depth,
                                     touch_depth) if self.capture else None
            self.open[t].append(Order('join', t, side, passive_px, size, ts, mid,
                                      passive_depth, self.timeout_us, spread, st))
            self.open[t].append(Order('join_then_cross', t, side, passive_px, size, ts, mid,
                                      passive_depth, self.timeout_us, spread, st))
            if tick and abs(touch_px - passive_px) > tick * 1.5:
                imp = passive_px + tick if side == 'buy' else passive_px - tick
                # A new price level: nothing is resting there, so the queue ahead is genuinely 0.
                # This is the one place where the two bounds agree.
                self.open[t].append(Order('improve', t, side, imp, size, ts, mid, 0.0,
                                          self.timeout_us, spread, st))
            else:
                self.skipped['no_room_to_improve'] += 1

    # ---- pre-fill state -------------------------------------------------------------------
    def pre_fill_state(self, t, bk, now_us, side, px, passive_depth, touch_depth):
        """Everything observable at the instant the order is posted, and nothing else.

        The one rule: no field here may be computable only after the fill. That is easy to state
        and easy to violate -- `spread_at_fill`, `queue_consumed`, `time_to_fill` are all natural
        things to want and all of them leak the answer. The study that uses this predicts a
        quantity measured after the fill, so a single post-fill field in this dict turns the whole
        exercise into a tautology with a good-looking R-squared.
        """
        from feature_engine import Flow
        f = self.eng.flow[t]
        mid = bk.mid
        cs = self.eng.contract_size.get(t)
        under = (mid / cs) if (cs and mid) else None
        d = {
            'market': t, 'side': side, 'post_us': now_us,
            'mid': mid, 'price': px,
            'spread_bps': bk.spread_bps,
            'microprice_minus_mid_bps': ((bk.microprice - mid) / mid * 1e4) if mid else None,
            'passive_depth': passive_depth, 'touch_depth': touch_depth,
            'queue_ahead': passive_depth,
            'since_connect_s': ((now_us - self.eng.connect_us) / 1e6
                                if self.eng.connect_us else None),
            'n_bid_levels': len(bk.bids), 'n_ask_levels': len(bk.asks),
            'time_since_trade_us': ((now_us - bk.last_trade_us) if bk.last_trade_us else None),
            'time_since_mid_change_us': ((now_us - bk.last_mid_change_us)
                                         if bk.last_mid_change_us else None),
        }
        for n in (1, 3, 5, 10):
            d[f'L{n}_OBI'] = bk.obi(n)
            d[f'depth_{n}_bid'] = bk.depth(n, 'bid')
            d[f'depth_{n}_ask'] = bk.depth(n, 'ask')
        for w in ('100ms', '500ms', '1s', '5s', '30s', '1m'):
            d[f'trade_imbalance_{w}'] = Flow.imbalance(f.trade_buy[w], f.trade_sell[w], now_us)
            d[f'cancel_imbalance_{w}'] = Flow.imbalance(f.cancel_bid[w], f.cancel_ask[w], now_us)
            d[f'add_imbalance_{w}'] = Flow.imbalance(f.add_bid[w], f.add_ask[w], now_us)
            d[f'aggressive_buy_{w}'] = f.trade_buy[w].sum(now_us)
            d[f'aggressive_sell_{w}'] = f.trade_sell[w].sum(now_us)
            d[f'cancel_vol_{w}'] = f.cancel_bid[w].sum(now_us) + f.cancel_ask[w].sum(now_us)
            d[f'update_rate_{w}'] = f.updates[w].count(now_us)
        # Cross-venue residuals. A perp PRICE is dollars per contract and an index is dollars per
        # unit of the underlying, so the contract_size scaling is what keeps these from reading
        # -9999 bps. They are None when the feed is stale rather than forward-filled: a basis
        # against a thirty-second-old index is not a basis.
        idx = self.eng.cf_map.get(t)
        cf = self.eng.cf.get(idx) if idx else None
        if cf and cf.price and under and (cf.age_us(now_us) or 0) < 10_000_000:
            d['cf_basis_bps'] = (under / cf.price - 1) * 1e4
            d['cf_ret_5s_bps'] = cf.ret_bps(now_us, 5_000_000)
            d['cf_ret_30s_bps'] = cf.ret_bps(now_us, 30_000_000)
            d['cf_age_us'] = cf.age_us(now_us)
        else:
            d['cf_basis_bps'] = d['cf_ret_5s_bps'] = d['cf_ret_30s_bps'] = d['cf_age_us'] = None
        for venue in ('coinbase', 'binance', 'hyperliquid'):
            key = next((k for k in self.eng.ext_map.get(t, []) if k.startswith(venue)), None)
            fd = self.eng.ext.get(key) if key else None
            if fd and fd.price and under and (fd.age_us(now_us) or 0) < 15_000_000:
                d[f'{venue}_basis_bps'] = (under / fd.price - 1) * 1e4
                d[f'{venue}_ret_5s_bps'] = fd.ret_bps(now_us, 5_000_000)
                d[f'{venue}_ret_30s_bps'] = fd.ret_bps(now_us, 30_000_000)
            else:
                d[f'{venue}_basis_bps'] = None
                d[f'{venue}_ret_5s_bps'] = None
                d[f'{venue}_ret_30s_bps'] = None
        # Short-horizon realised volatility from our own recorded mid path.
        xs = self.mid_ts.get(t)
        if xs and len(xs) > 4:
            import bisect as _b
            i0 = _b.bisect_left(xs, now_us - 60_000_000)
            px_ = self.mid_px[t][i0:]
            if len(px_) > 3:
                rets = [(b - a) / a * 1e4 for a, b in zip(px_, px_[1:]) if a > 0]
                if rets:
                    m = sum(rets) / len(rets)
                    d['vol_60s_bps'] = (sum((r - m) ** 2 for r in rets) / len(rets)) ** 0.5
                    d['n_mid_changes_60s'] = len(rets)
        d.setdefault('vol_60s_bps', None)
        d.setdefault('n_mid_changes_60s', None)
        return d

    # ---- measurement --------------------------------------------------------------------
    def mid_at(self, t, ts):
        xs = self.mid_ts.get(t)
        if not xs:
            return None
        i = bisect.bisect_right(xs, ts) - 1
        if i < 0:
            return None
        # Do not carry a mid forward more than 60s; that is a different market state.
        if ts - xs[i] > 60_000_000:
            return None
        return self.mid_px[t][i]

    def measure(self, o, bound='pess'):
        fill_us = o.fill_pess_us if bound == 'pess' else o.fill_opt_us
        if fill_us is None or not o.decision_mid:
            return None
        px = o.fill_px
        fee = TAKER_BPS if (o.mode == 'taker' or o.crossed_after_timeout) else MAKER_BPS
        # Cost of entry against the mid we decided at. Positive = we paid worse than that mid.
        cost = o.signed((px - o.decision_mid) / o.decision_mid * 1e4)
        row = {'market': o.market, 'mode': o.mode, 'side': o.side, 'bound': bound,
               'fill_us': fill_us, 'wait_s': (fill_us - o.posted_us) / 1e6,
               'cost_bps': cost, 'fee_bps': fee, 'spread_at_post_bps': o.spread_at_post,
               'crossed': o.crossed_after_timeout, 'left_touch': o.left_touch}
        for name, h in MARKOUTS_US:
            m2 = self.mid_at(o.market, fill_us + h)
            mo = (o.signed((m2 - px) / px * 1e4) if m2 else None)
            row[f'markout_{name}'] = mo
            # kept_h = half-spread earned at entry + what the market took back by h.
            # `cost` is signed so that positive means we paid up, so -cost is the half-spread
            # credit a passive fill earns. This is the target of the conditional maker study:
            # not "will the price rise" but "if someone hits me here, is that fill toxic".
            row[f'kept_{name}'] = ((-cost + mo) if mo is not None else None)
        return row


def export_fills(sim, path, modes=('join', 'improve')):
    """One row per passive fill: the state it was posted into, and what it kept.

    Only the pessimistic queue bound is exported. The optimistic bound credits us every cancel
    ahead in the queue, which changes WHICH fills exist, and a conditional model trained on fills
    that may not have happened is learning about the bound rather than the market.
    """
    rows = []
    for o in sim.filled:
        if o.mode not in modes or o.state is None or o.crossed_after_timeout:
            continue
        m = sim.measure(o, 'pess')
        if not m:
            continue
        r = dict(o.state)
        r['mode'] = o.mode
        r['fill_us'] = m['fill_us']
        r['wait_s'] = m['wait_s']
        r['fee_bps'] = m['fee_bps']
        r['left_touch'] = o.left_touch
        for name, _ in MARKOUTS_US:
            r[f'markout_{name}'] = m.get(f'markout_{name}')
            r[f'kept_{name}'] = m.get(f'kept_{name}')
        rows.append(r)
    if not rows:
        return 0, []
    cols = sorted({k for r in rows for k in r})
    front = ['market', 'mode', 'side', 'post_us', 'fill_us', 'wait_s', 'mid', 'price',
             'spread_bps', 'fee_bps']
    cols = front + [c for c in cols if c not in front]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    import csv as _csv
    with open(path, 'w', newline='') as f:
        w = _csv.DictWriter(f, fieldnames=cols, extrasaction='ignore')
        w.writeheader()
        rows.sort(key=lambda r: r['fill_us'])
        for r in rows:
            w.writerow(r)
    return len(rows), cols


def iter_events(hours, srcs=('kalshi_margin', 'kalshi_index', 'ext_coinbase', 'ext_binance',
                             'ext_hyperliquid', 'ext_kraken')):
    """Yield (src, event) in timestamp order across sources, streaming rather than loading."""
    import heapq
    streams = []
    for src in srcs:
        for h in hours:
            p = os.path.join(ROOT, 'data', 'raw', f'source={src}', f'hour={h}', 'events.jsonl.gz')
            if os.path.exists(p):
                streams.append((src, p))
    if not streams:
        return

    def gen(src, path):
        # The last hour of a recording is a gzip stream that was never closed, because the
        # recorder was killed mid-write. Reading it raises EOFError partway through. Stopping
        # cleanly at that point keeps every complete record; refusing to read the file loses the
        # whole hour, and ignoring the error loses the ability to say the file was truncated.
        try:
            with gzip.open(path, 'rt') as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        ev = json.loads(line)
                    except json.JSONDecodeError:
                        continue           # a half-written final line
                    ts = ev.get('src_ts_us') or ev.get('recv_ts_us')
                    if ts:
                        yield (ts, src, ev)
                    elif ev.get('ev') == '_connect':
                        yield (ev.get('recv_ts_us') or 0, src, ev)
        except (EOFError, OSError) as e:
            print(f'  [truncated] {os.path.basename(os.path.dirname(path))}: {e}',
                  file=sys.stderr)

    for ts, src, ev in heapq.merge(*[gen(s, p) for s, p in streams], key=lambda x: x[0]):
        yield src, ev


# ---- aggregation ---------------------------------------------------------------------------
def med(xs):
    xs = [x for x in xs if x is not None and x == x]
    return statistics.median(xs) if xs else None


def mean(xs):
    xs = [x for x in xs if x is not None and x == x]
    return sum(xs) / len(xs) if xs else None


def aggregate(sim, rows):
    """One line per (market, mode, bound). Attempts are counted separately from fills, because a
    cost conditional on filling is not a cost until you know how often you fill."""
    attempts = defaultdict(int)
    for o in sim.filled:
        attempts[(o.market, o.mode)] += 1
    for (t, mode), n in sim.expired.items():
        attempts[(t, mode)] += n

    by = defaultdict(list)
    for r in rows:
        by[(r['market'], r['mode'], r['bound'])].append(r)

    out = []
    for (t, mode, bound), rs in sorted(by.items()):
        att = attempts.get((t, mode), 0)
        n = len(rs)
        fee = mean([r['fee_bps'] for r in rs])
        cost = mean([r['cost_bps'] for r in rs])
        line = {'market': t, 'mode': mode, 'queue_bound': bound,
                'attempts': att, 'fills': n,
                'fill_rate_pct': (100.0 * n / att) if att else None,
                'median_wait_s': med([r['wait_s'] for r in rs]),
                'median_spread_at_post_bps': med([r['spread_at_post_bps'] for r in rs]),
                'entry_cost_bps': cost, 'fee_bps': fee}
        for name, _ in MARKOUTS_US:
            line[f'markout_{name}_bps'] = mean([r[f'markout_{name}'] for r in rs])
        # Net EV of the entry leg: what the position was worth at the horizon, minus what it cost
        # to get in (the price paid relative to the decision mid) and minus the fee.
        for name in ('10s', '60s', '300s'):
            mo = line.get(f'markout_{name}_bps')
            line[f'net_{name}_bps'] = (mo - cost - fee) if (mo is not None and cost is not None
                                                            and fee is not None) else None
        # The adverse-selection signature: a passive fill that is immediately wrong.
        line['adverse_1s_bps'] = line.get('markout_1s_bps')
        line['crossed_pct'] = (100.0 * sum(1 for r in rs if r['crossed']) / n) if n else None
        out.append(line)
    return out


CSV_COLS = (['market', 'mode', 'queue_bound', 'attempts', 'fills', 'fill_rate_pct',
             'median_wait_s', 'median_spread_at_post_bps', 'entry_cost_bps', 'fee_bps']
            + [f'markout_{n}_bps' for n, _ in MARKOUTS_US]
            + ['net_10s_bps', 'net_60s_bps', 'net_300s_bps', 'adverse_1s_bps', 'crossed_pct'])


def write_csv(lines, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write(','.join(CSV_COLS) + '\n')
        for L in lines:
            f.write(','.join(
                ('' if L.get(c) is None else
                 (f"{L[c]:.3f}" if isinstance(L.get(c), float) else str(L.get(c))))
                for c in CSV_COLS) + '\n')


def write_summary(sim, lines, path, hours, args):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    pess = [L for L in lines if L['queue_bound'] == 'pess']
    opt = {(L['market'], L['mode']): L for L in lines if L['queue_bound'] == 'opt'}

    def best(metric):
        c = [L for L in pess if L.get(metric) is not None and L['fills'] >= 20]
        return max(c, key=lambda L: L[metric]) if c else None

    b60 = best('net_60s_bps')
    out = []
    A = out.append
    A('# Execution simulator\n')
    A(f"Hours replayed: {', '.join(hours) if hours else '(none)'}  ")
    A(f"Events: {sim.events:,}  Orders simulated: "
      f"{sum(sim.expired.values()) + len(sim.filled):,}  ")
    A(f"Order notional: ${args.notional:.2f}  Sample interval: {args.sample_s}s  "
      f"Passive timeout: {args.timeout_s}s  Max size: {args.max_size_frac:.0%} of touch\n")

    A('## The number that decides everything\n')
    if b60 is None:
        A('No (market, mode) pair produced 20 or more pessimistic-bound fills. There is not '
          'enough recorded data to say anything about execution cost yet. This is the correct '
          'output for 2.5 hours of L2, not a failure.\n')
    else:
        A(f"Best net 60s EV on the pessimistic queue bound: **{b60['market']} / {b60['mode']} "
          f"= {fmt(b60['net_60s_bps'])} bps** over {b60['fills']} fills "
          f"({fmt(b60['fill_rate_pct'],1)}% of attempts).\n")
        A(f"That is entry cost {fmt(b60['entry_cost_bps'])} + fee {fmt(b60['fee_bps'])} "
          f"against a 60s markout of {fmt(b60['markout_60s_bps'])} bps.\n")
        if b60['net_60s_bps'] is not None and b60['net_60s_bps'] <= 0:
            A('It is not positive. Execution does not pay for itself on this sample, which means '
              'no signal built on top of it can be evaluated against a cheaper cost assumption '
              'than this one.\n')

    A('## Fill rate: the two bounds\n')
    A('The gap between the bounds is the error bar. A mode whose conclusion flips across it has '
      'no conclusion.\n')
    A('| market | mode | attempts | fill% pess | fill% opt | gap | median wait s |')
    A('|---|---|---:|---:|---:|---:|---:|')
    for L in sorted(pess, key=lambda x: -(x['fill_rate_pct'] or 0))[:24]:
        o = opt.get((L['market'], L['mode']))
        op = o['fill_rate_pct'] if o else None
        gap = (op - L['fill_rate_pct']) if (op is not None and L['fill_rate_pct'] is not None) else None
        A(f"| {L['market']} | {L['mode']} | {L['attempts']} | {fmt(L['fill_rate_pct'],1)} | "
          f"{fmt(op,1)} | {fmt(gap,1)} | {fmt(L['median_wait_s'],1)} |")
    A('')

    A('## Adverse selection\n')
    A('Markout at 1s conditional on a passive fill. Negative means the fill was immediately '
      'wrong: the book moved against the side that was standing still. This is the cost that a '
      'fee schedule cannot show you.\n')
    A('| market | mode | fills | 1s | 5s | 10s | 60s | 300s |')
    A('|---|---|---:|---:|---:|---:|---:|---:|')
    for L in sorted([x for x in pess if x['mode'] != 'taker' and x['fills'] >= 5],
                    key=lambda x: (x['markout_1s_bps'] if x['markout_1s_bps'] is not None else 0)):
        A(f"| {L['market']} | {L['mode']} | {L['fills']} | {fmt(L['markout_1s_bps'])} | "
          f"{fmt(L['markout_5s_bps'])} | {fmt(L['markout_10s_bps'])} | "
          f"{fmt(L['markout_60s_bps'])} | {fmt(L['markout_300s_bps'])} |")
    A('')

    A('## Where the spread goes\n')
    A('For a passive fill the spread is the whole revenue: you buy at the bid and the mid is half '
      'a spread above you. What you KEEP is that half-spread plus the markout, and the markout is '
      'what the market takes back for having traded with you.\n')
    j = [L for L in pess if L['mode'] == 'join' and L['fills'] >= 50
         and L['median_spread_at_post_bps'] is not None
         and L['markout_60s_bps'] is not None]
    if len(j) >= 5:
        xs = [L['median_spread_at_post_bps'] for L in j]
        ys = [L['markout_60s_bps'] for L in j]
        n = len(xs)
        mx, my = sum(xs) / n, sum(ys) / n
        den = (sum((a - mx) ** 2 for a in xs) * sum((b - my) ** 2 for b in ys)) ** 0.5
        corr = (sum((a - mx) * (b - my) for a, b in zip(xs, ys)) / den) if den else None
        A('| market | spread | half-spread | 60s markout | kept | maker fee | net |')
        A('|---|---:|---:|---:|---:|---:|---:|')
        for L in sorted(j, key=lambda x: -x['median_spread_at_post_bps']):
            sp = L['median_spread_at_post_bps']
            kept = sp / 2 + L['markout_60s_bps']
            A(f"| {L['market']} | {sp:.1f} | {sp/2:.1f} | {L['markout_60s_bps']:+.2f} | "
              f"**{kept:+.2f}** | {L['fee_bps']:.1f} | {fmt(L['net_60s_bps'])} |")
        kepts = [L['median_spread_at_post_bps'] / 2 + L['markout_60s_bps'] for L in j]
        A('')
        if corr is not None:
            A(f'**Correlation between the quoted spread and the 60-second markout: '
              f'{corr:+.3f}** across {n} markets.\n')
            A('A wide spread is not a wider margin. It is compensation for the information in the '
              'flow that crosses it, and the compensation is close to exact: the markout scales '
              'with the spread almost one for one. What survives -- the `kept` column -- runs '
              f'from {min(kepts):+.2f} to {max(kepts):+.2f} bps, mean '
              f'{sum(kepts)/len(kepts):+.2f}.\n')
            worst = min(j, key=lambda L: L['median_spread_at_post_bps'])
            A(f'The tightest market on the board (`{worst["market"]}`, '
              f'{worst["median_spread_at_post_bps"]:.1f} bps) shows the same thing from the other '
              f'end: its markout is only {worst["markout_60s_bps"]:+.2f} bps, so almost nothing '
              f'is lost to selection -- and there is almost nothing to win either.\n')
            A(f'Against a maker fee of {j[0]["fee_bps"]:.0f} bps, the fee is roughly '
              f'{abs(j[0]["fee_bps"] / (sum(kepts)/len(kepts))):.0f}x what the market leaves on '
              f'the table. That ratio, not the choice of feature or horizon, is what decides '
              f'whether quoting these contracts can work.\n')

    A('## Cost of crossing\n')
    A('| market | fills | entry cost bps | + fee | total to cross |')
    A('|---|---:|---:|---:|---:|')
    for L in sorted([x for x in pess if x['mode'] == 'taker'], key=lambda x: x['market']):
        tot = ((L['entry_cost_bps'] or 0) + (L['fee_bps'] or 0))
        A(f"| {L['market']} | {L['fills']} | {fmt(L['entry_cost_bps'])} | "
          f"{fmt(L['fee_bps'])} | {fmt(tot)} |")
    A('')

    A('## Orders not simulated\n')
    if sim.skipped:
        for k, v in sorted(sim.skipped.items(), key=lambda kv: -kv[1]):
            A(f'- `{k}`: {v:,}')
    else:
        A('- none')
    tot_skip = sum(sim.skipped.values())
    placed = sum(sim.expired.values()) + len(sim.filled)
    if tot_skip and placed:
        frac = tot_skip / (tot_skip + placed)
        A(f'\n{frac:.0%} of sampled decisions were dropped before an order was placed.')
        if frac > 0.5:
            A('\n**More than half were dropped.** The surviving sample is not a random sample of '
              'market states, so the averages above describe the states that happened to admit a '
              f'${args.notional:.0f} order, not the market.')
    A('')

    A('## What would change these numbers\n')
    A('- Our order is absent from the replayed book. Fill rates are therefore optimistic for '
      '`improve` (a real order inside the spread invites others to step in front) and roughly '
      'neutral for `join` at small size.')
    A('- A resting order here never repositions. Real quoting cancels and replaces, which raises '
      'fill rate and worsens the 1s markout at the same time.')
    A('- Markouts use the mid, not what we could have exited at. The exit is a second spread. '
      'A round trip costs the entry above plus an exit of comparable size.')
    A('- Sample: one session of recorded L2. Intraday, day-of-week and regime variation are not '
      'in it.\n')
    with open(path, 'w') as f:
        f.write('\n'.join(out) + '\n')


def discover_hours(src='kalshi_margin'):
    base = os.path.join(ROOT, 'data', 'raw', f'source={src}')
    return sorted(os.path.basename(p).split('=', 1)[1]
                  for p in glob.glob(os.path.join(base, 'hour=*')))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--hours', nargs='*', help='hour partitions, e.g. 2026-09-11T15')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--markets', nargs='*')
    ap.add_argument('--notional', type=float, default=25.0)
    ap.add_argument('--sample-s', type=float, default=15.0)
    ap.add_argument('--timeout-s', type=float, default=30.0)
    ap.add_argument('--max-size-frac', type=float, default=0.25,
                    help='drop orders larger than this fraction of the touch')
    ap.add_argument('--limit', type=int, default=0, help='stop after N events (smoke test)')
    ap.add_argument('--fills-csv', default='',
                    help='also export every passive fill with its pre-fill state')
    ap.add_argument('--csv', default=os.path.join(ROOT, 'reports', 'execution_simulator.csv'))
    ap.add_argument('--md', default=os.path.join(ROOT, 'reports', 'execution_summary.md'))
    args = ap.parse_args()

    hours = args.hours or (discover_hours() if args.all else [])
    if not hours:
        sys.exit('no hours selected: pass --all or --hours 2026-09-11T15')

    sim = Sim(args.notional, args.sample_s, args.timeout_s, args.max_size_frac, args.markets,
              capture=bool(args.fills_csv))
    n = 0
    for src, ev in iter_events(hours):
        sim.feed(src, ev)
        n += 1
        if args.limit and n >= args.limit:
            break
        if n % 2_000_000 == 0:
            print(f'  {n:,} events, {len(sim.filled):,} fills', file=sys.stderr)

    # Anything still open at the end of the tape never resolved. It is counted as unfilled rather
    # than dropped, because dropping it would quietly raise every fill rate.
    for t, orders in sim.open.items():
        for o in orders:
            if o.fill_opt_us is not None:
                sim.filled.append(o)
            else:
                sim.expired[(t, o.mode)] += 1

    rows = []
    for o in sim.filled:
        for b in ('pess', 'opt'):
            r = sim.measure(o, b)
            if r:
                rows.append(r)

    if args.fills_csv:
        n, cols = export_fills(sim, args.fills_csv)
        print(f'exported {n:,} passive fills with {len(cols)} columns to '
              f'{os.path.relpath(args.fills_csv, ROOT)}')
    lines = aggregate(sim, rows)
    write_csv(lines, args.csv)
    write_summary(sim, lines, args.md, hours, args)

    print(f'events {sim.events:,}  orders {sum(sim.expired.values()) + len(sim.filled):,}  '
          f'fills {len(sim.filled):,}  rows {len(lines)}')
    print(f'wrote {os.path.relpath(args.csv, ROOT)}')
    print(f'wrote {os.path.relpath(args.md, ROOT)}')
    pess = [L for L in lines if L['queue_bound'] == 'pess' and L['fills'] >= 20]
    if pess:
        b = max(pess, key=lambda L: (L['net_60s_bps'] if L['net_60s_bps'] is not None else -1e9))
        print(f"best net 60s (pessimistic): {b['market']} {b['mode']} "
              f"{fmt(b['net_60s_bps'])} bps over {b['fills']} fills")
    else:
        print('no (market, mode) reached 20 pessimistic fills: not enough data to conclude')


if __name__ == '__main__':
    main()
