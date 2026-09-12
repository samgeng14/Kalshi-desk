#!/usr/bin/env python3
"""Streaming feature engine for Kalshi perpetuals: L2 book, flow, and cross-venue state.

Replays the recorder's JSONL.gz and emits one feature row per market per snapshot interval, for
two research lanes that share a book but ask different questions:

  A. BTC/ETH IMPULSE -- 2m, 5m, 10m directional, weighted to 10m. The 10s/30s/1m horizons are
     excluded by arithmetic, not preference: a perfect oracle on the top decile of 1-minute BTC
     moves loses 3.2 bps after fees (research/impulse_viability.md).
  B. FILL-AWARE MAKER -- queue state and flow for markets whose spread can clear 10 bps
     (research/maker_viability.md).

THE RULE THIS FILE EXISTS TO ENFORCE: a feature is never computed through a book sequence gap,
never from a forward-filled stale feed, and never from a timestamp whose unit is in doubt. Every
row carries the age of every input it used, so a later study can drop rows rather than discover
afterwards that it trusted a stale book.

Time: integer microseconds everywhere, converted once at the edge. The margin socket sends
milliseconds (`ts_ms`), the CF socket sends milliseconds (`source_ts_ms`), and the event-contract
sockets send seconds. Mixing them puts feeds 1000x apart, so `to_us` takes the unit as a required
argument and there is no inference anywhere.

  python3 scripts/feature_engine.py --hours 2026-09-11T14 --out data/features
  python3 scripts/feature_engine.py --all --markets KXBTCPERP KXETHPERP
"""
import argparse, bisect, glob, gzip, json, math, os, sys
from collections import defaultdict, deque

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Kalshi encodes "no order on this side" as INT64_MAX, which arrives as 922337203685477.6 after
# the 10^4 scaling. It reached the research path once already and reported a 3e14 bps edge.
SENTINEL = 1e9
# A feed older than this is stale and the row is dropped rather than computed from a stale price.
MAX_AGE_US = {'kalshi': 5_000_000, 'cf': 10_000_000, 'external': 15_000_000}


def num(v, default=0.0):
    """A number, or the default. Rejects the no-order sentinel and anything non-finite."""
    try:
        f = float(v)
    except (TypeError, ValueError):
        return default
    if f != f or abs(f) >= SENTINEL:
        return default
    return f


def to_us(value, unit):
    """unit is required and never inferred: 's' | 'ms' | 'us' | 'ns'."""
    if value is None:
        return None
    try:
        v = float(value)
    except (TypeError, ValueError):
        return None
    if v != v or abs(v) >= 1e19:
        return None
    mult = {'s': 1_000_000, 'ms': 1_000, 'us': 1, 'ns': 0.001}
    if unit not in mult:
        raise ValueError(f"unknown time unit {unit!r}; the unit is never guessed")
    return int(v * mult[unit])


class UnitMismatch(Exception):
    """Raised when a timestamp is wildly outside the plausible epoch range in microseconds.

    This fails loudly on purpose. A seconds value read as microseconds lands in 1970 and a
    milliseconds value read as microseconds lands in 1970 too; both look like small numbers and
    both silently destroy any lead-lag result computed from them.
    """


# Microseconds for 2020-01-01 and 2100-01-01. Anything outside is a unit error, not a timestamp.
_LO, _HI = 1_577_836_800_000_000, 4_102_444_800_000_000


def check_epoch(us, what):
    if us is None:
        return None
    if not (_LO < us < _HI):
        raise UnitMismatch(
            f"{what}: {us} microseconds is {us/1e6:.0f}s since epoch, outside 2020-2100. "
            f"This is a unit mismatch, not a clock problem.")
    return us


# ---- order book ------------------------------------------------------------------------------
class Book:
    """One market's L2 book, rebuilt from a snapshot and deltas.

    `seq` is per subscription and monotonic. A gap means updates were missed, so the book is
    WRONG, not merely behind: it is marked stale and every feature derived from it is suppressed
    until a fresh snapshot arrives. Patching over a gap is how a study ends up measuring a book
    that never existed.
    """

    def __init__(self, ticker=''):
        # Default so a defaultdict can construct one; the engine sets the ticker on first event.
        self.ticker = ticker
        self.bids = {}          # price -> quantity
        self.asks = {}
        self.seq = None
        self.stale = True       # until the first snapshot
        self.gap_count = 0
        self.last_update_us = None
        self.last_trade_us = None
        self.last_mid = None
        self.last_mid_change_us = None
        self.built_from_us = None   # when this book last started from nothing (connect/snapshot)
        self.empty_snapshot = True  # True until a snapshot arrives WITH levels in it

    def snapshot(self, bids, asks, seq, ts_us):
        """Apply a snapshot. It is usually full, and sometimes EMPTY -- the difference matters.

        Of 36 snapshots in the recorded tape, the 18 from the second connect carry real depth
        (BTC: 245 bid levels, 108 ask). The 18 from the FIRST connect of the session carry
        {market_ticker, market_id} and nothing else. That is a race at subscribe time, not the
        protocol: the venue answered before it had a book to send.

        The consequence is specific rather than general. After a full snapshot the book is
        complete immediately and needs no warm-up. After an EMPTY one it has to be rebuilt from
        streamed deltas, and while top of book converges in about a tenth of a second, depth
        keeps filling for minutes (BTC: 31 levels a side at 7s, 61 by the end of the hour) -- so
        an L5 or L10 imbalance computed early is a number about our subscription, not the market.
        `empty_snapshot` records which case this book is in, so the warm-up gate applies only
        where it is earned.
        """
        self.bids = {p: q for p, q in ((num(a), num(b)) for a, b in bids) if p > 0 and q > 0}
        self.asks = {p: q for p, q in ((num(a), num(b)) for a, b in asks) if p > 0 and q > 0}
        self.seq = seq
        self.stale = False
        self.empty_snapshot = not (self.bids or self.asks)
        self.built_from_us = ts_us
        self.last_update_us = ts_us
        return True

    def delta(self, price, delta, side, seq, ts_us):
        """Returns True if applied, False if the book is stale and the update cannot be trusted.

        Gap detection is NOT done here. `seq` on the Kalshi margin feed is per SUBSCRIPTION, not
        per market: three markets interleaved on one socket produce seq 101, 102, 103, so each
        market's own sequence looks like it is jumping by 3 every time. Checking it per market
        flagged 41,799 gaps in 97,940 real BTC deltas -- and there were none. The engine checks
        continuity per sid and marks every market on that subscription stale together, which is
        also the truth: a dropped frame on a shared socket could have belonged to any of them.
        """
        if isinstance(seq, int):
            self.seq = seq
        if self.stale:
            return False
        p, d = num(price), num(delta, default=None)
        if p <= 0 or d is None:
            return True
        side_map = self.bids if side == 'bid' else self.asks
        q = side_map.get(p, 0.0) + d
        if q > 1e-12:
            side_map[p] = q
        else:
            side_map.pop(p, None)
        self.last_update_us = ts_us
        return True

    # -- derived state --
    @property
    def best_bid(self):
        return max(self.bids) if self.bids else None

    @property
    def best_ask(self):
        return min(self.asks) if self.asks else None

    @property
    def crossed(self):
        b, a = self.best_bid, self.best_ask
        return b is not None and a is not None and b >= a

    def usable(self):
        """A book is usable when it is not stale, has both sides, and is not crossed.

        A crossed book is not a tradable state; it is a book mid-update or a feed error, and a
        midpoint computed from one is meaningless.
        """
        return (not self.stale) and self.best_bid is not None and self.best_ask is not None \
            and not self.crossed

    @property
    def mid(self):
        return 0.5 * (self.best_bid + self.best_ask) if self.usable() else None

    @property
    def spread_bps(self):
        m = self.mid
        return (self.best_ask - self.best_bid) / m * 1e4 if m else None

    @property
    def microprice(self):
        """Size-weighted touch. Weighted TOWARD the thin side, because that is the side that
        gives way first: a large bid and a small ask implies the next move is up."""
        if not self.usable():
            return None
        bq, aq = self.bids.get(self.best_bid, 0.0), self.asks.get(self.best_ask, 0.0)
        tot = bq + aq
        if tot <= 0:
            return self.mid
        return (self.best_bid * aq + self.best_ask * bq) / tot

    def depth(self, levels, side):
        book = self.bids if side == 'bid' else self.asks
        if not book:
            return 0.0
        prices = sorted(book, reverse=(side == 'bid'))[:levels]
        return sum(book[p] for p in prices)

    def obi(self, levels):
        """Order book imbalance over the top N levels, in [-1, +1]. Positive = bid-heavy."""
        b, a = self.depth(levels, 'bid'), self.depth(levels, 'ask')
        tot = b + a
        return (b - a) / tot if tot > 0 else 0.0

    def queue_ahead(self, price, side):
        """Size resting at a price, which is what a new order at that price queues behind."""
        return (self.bids if side == 'bid' else self.asks).get(num(price), 0.0)


# ---- rolling windows -------------------------------------------------------------------------
class Window:
    """Sum of values within the last `span` microseconds.

    Evicts on WRITE as well as on read. Evicting only on read is correct arithmetic and a memory
    leak: a consumer that feeds the engine without ever calling `row()` -- the execution
    simulator does exactly that, because it needs the book and not the flow -- never triggers an
    eviction, so all 56 windows per market keep every entry forever. On a 15-million-event tape
    that is roughly 100 million retained tuples, and the process is killed by the OOM reaper
    partway through with no error and no output.

    Evicting on write costs one amortised comparison per append and removes the failure mode
    entirely, regardless of how the structure is used.
    """

    def __init__(self, span_us):
        self.span = span_us
        self.q = deque()
        self.total = 0.0

    def add(self, ts_us, value):
        self.q.append((ts_us, value))
        self.total += value
        cutoff = ts_us - self.span
        while self.q and self.q[0][0] < cutoff:
            self.total -= self.q.popleft()[1]

    def sum(self, now_us):
        cutoff = now_us - self.span
        while self.q and self.q[0][0] < cutoff:
            self.total -= self.q.popleft()[1]
        return self.total

    def count(self, now_us):
        self.sum(now_us)
        return len(self.q)


# `last_update_reason` separates a trade from a cancel on the venues that publish it. The KALSHI
# MARGIN SOCKET DOES NOT. Every orderbook_delta on that feed carries only
# {market_ticker, price, delta, side, ts_ms} -- checked against 6,017,346 real deltas, of which
# zero had the field. Classifying from it there silently zeroes every trade and cancel feature,
# which is worse than having none.
#
# The margin feed publishes trades on a SEPARATE channel (sid 3) instead:
#   {trade_id, market_ticker, price, count, taker_side, ts_ms}
# `taker_side` is the TAKER's own side, not the side consumed. Established empirically rather than
# assumed: matching each trade to negative deltas at the same price within 300ms gives
# taker_side=bid -> ask consumed 3340 vs bid 594, and taker_side=ask -> bid consumed 2677 vs ask
# 410, on exact magnitude matches. So taker_side='bid' is an aggressive BUYER lifting the offer.
#
# These sets are kept for feeds that DO publish a reason (the event-contract sockets do).
TRADE_REASONS = {'Trade'}
CANCEL_REASONS = {'MarginCancel', 'SelfTradeCancel', 'ExpiryCancel', 'PostOnlyCrossCancel'}
# 'Decrease' and 'Amend' are ambiguous: a decrease may be a cancel or a fill. They are counted
# separately rather than folded into either, so a study can choose.
AMBIGUOUS_REASONS = {'Decrease', 'Amend'}

WINDOWS_US = {'100ms': 100_000, '500ms': 500_000, '1s': 1_000_000, '2s': 2_000_000,
              '5s': 5_000_000, '10s': 10_000_000, '30s': 30_000_000, '1m': 60_000_000}


class Flow:
    """Signed flow by classification, over every window, for one market."""

    def __init__(self):
        mk = lambda: {k: Window(v) for k, v in WINDOWS_US.items()}
        self.trade_buy, self.trade_sell = mk(), mk()
        self.cancel_bid, self.cancel_ask = mk(), mk()
        self.add_bid, self.add_ask = mk(), mk()
        self.updates = mk()

    def on_delta(self, ts_us, side, delta, reason):
        for w in self.updates.values():
            w.add(ts_us, 1.0)
        mag = abs(delta)
        if delta > 0:
            tgt = self.add_bid if side == 'bid' else self.add_ask
        elif reason in TRADE_REASONS:
            # Liquidity removed by a trade. A trade that hits the BID is a seller being aggressive.
            tgt = self.trade_sell if side == 'bid' else self.trade_buy
        elif reason in CANCEL_REASONS:
            tgt = self.cancel_bid if side == 'bid' else self.cancel_ask
        else:
            return
        for w in tgt.values():
            w.add(ts_us, mag)

    @staticmethod
    def imbalance(a, b, now):
        x, y = a.sum(now), b.sum(now)
        tot = x + y
        return (x - y) / tot if tot > 0 else 0.0


class Feed:
    """One external or index price, with its own age so staleness is visible, never assumed.

    History is kept as two parallel lists rather than a deque of pairs. `ret_bps` bisects it, and
    bisect needs a sorted key sequence: building one per call (`[h[0] for h in self.hist]`) is
    O(n) inside what is already a hot loop. With four venues, four windows and a million book
    updates that is sixteen million rebuilds of a twenty-thousand element list, and the job never
    finishes. This is the third place in this project the same pattern has appeared, so it is
    fixed here at the source rather than at the call site again.
    """

    MAXLEN = 20000

    def __init__(self, name=''):
        # Defaulted for the same reason as Book: a defaultdict constructs with no arguments.
        self.name = name
        self.last_us = None
        self.price = None
        self.hist_ts = []
        self.hist_px = []

    @property
    def hist(self):
        """Pairs, for the handful of callers that want them. Not used on any hot path."""
        return list(zip(self.hist_ts, self.hist_px))

    def update(self, ts_us, price):
        p = num(price)
        if p <= 0 or ts_us is None:
            return
        self.last_us, self.price = ts_us, p
        self.hist_ts.append(ts_us)
        self.hist_px.append(p)
        if len(self.hist_ts) > self.MAXLEN * 2:
            del self.hist_ts[:self.MAXLEN]
            del self.hist_px[:self.MAXLEN]

    def age_us(self, now_us):
        return None if self.last_us is None else now_us - self.last_us

    def ret_bps(self, now_us, span_us):
        """Return over the last `span`, or None if there is no observation that old.

        Deliberately returns None rather than reaching for the oldest available point: a
        '5-second return' computed from a price 90 seconds old is not a 5-second return.
        """
        if not self.hist_ts or self.price is None:
            return None
        target = now_us - span_us
        # No observation at or before the target means we do not have `span` of history. Returning
        # the oldest point instead would label a 5-second return as a 1-hour one.
        if self.hist_ts[0] > target:
            return None
        i = bisect.bisect_left(self.hist_ts, target)
        if i >= len(self.hist_ts):
            return None
        base = self.hist_px[i]
        return (self.price - base) / base * 1e4 if base > 0 else None


# ---- the engine ------------------------------------------------------------------------------
class FeatureEngine:
    """Consumes recorder events, emits feature rows.

    Emission is explicit: `row(ticker, now_us)` returns a dict or None. It returns None whenever
    the row would be built on something untrustworthy -- a stale or gapped book, a crossed book,
    a CF or external feed older than its limit -- and the caller counts the reason. Suppressing
    the row is the point; a study can only drop what it knows about.
    """

    def __init__(self, cf_map=None, ext_map=None, contract_size=None):
        # A perp PRICE is dollars per contract; an index is dollars per unit of the underlying.
        # BTC marks at 7.90 with contract_size 0.0001, so the underlying is 79,000. Comparing the
        # two without this scaling makes every basis -9999 bps. Populated from /margin/markets.
        self.contract_size = contract_size or {
            'KXBTCPERP': 0.0001, 'KXETHPERP': 0.001, 'KXSOLPERP': 0.1, 'KXXRPPERP': 1.0,
            'KXADAPERP': 1.0, 'KXWLDPERP': 1.0, 'KXAAVEPERP': 0.01, 'KXBNBPERP': 0.001,
            'KXVVVPERP': 0.1, 'KXKSHIBPERP': 1000.0, 'KXNEARPERP': 1.0, 'KXZECPERP': 0.01,
            'KXLINKPERP': 1.0, 'KXLTCPERP': 0.1, 'KXSUIPERP': 10.0, 'KXBCHPERP': 0.01,
            'KXDOGEPERP': 100.0, 'KXHYPEPERP': 0.1,
        }
        self.books = defaultdict(Book)
        self.flow = defaultdict(Flow)
        self.cf = defaultdict(Feed)          # index_id -> Feed
        self.ext = defaultdict(Feed)         # "venue:symbol" -> Feed
        self.cf_map = cf_map or {'KXBTCPERP': 'BRTI', 'KXETHPERP': 'ETHUSD_RTI',
                                 'KXSOLPERP': 'SOLUSD_RTI', 'KXXRPPERP': 'XRPUSD_RTI'}
        self.ext_map = ext_map or {
            'KXBTCPERP': ['coinbase:BTC-USD', 'binance:BTCUSDT', 'hyperliquid:BTC', 'kraken:BTC/USD'],
            'KXETHPERP': ['coinbase:ETH-USD', 'binance:ETHUSDT', 'hyperliquid:ETH', 'kraken:ETH/USD'],
            'KXSOLPERP': ['coinbase:SOL-USD', 'binance:SOLUSDT', 'hyperliquid:SOL'],
            'KXXRPPERP': ['coinbase:XRP-USD', 'binance:XRPUSDT', 'hyperliquid:XRP'],
        }
        self.suppressed = defaultdict(int)
        self.events = defaultdict(int)
        self.seen_seq = defaultdict(set)     # (ticker) -> seqs, for duplicate detection
        # A consumer that only cares about two markets should not pay to maintain eighteen. Book
        # and flow work is skipped for anything outside `only_markets`; sequence continuity is
        # still judged for every frame, because a gap on the shared socket invalidates the books
        # we DO care about regardless of which market carried the frame.
        self.only_markets = None
        # 56 rolling windows per market is most of the cost of a replay, and a consumer that
        # never reads flow should not pay it.
        self.track_flow = True
        self.sid_seq = {}                    # sid -> last seq, the ONLY place continuity is judged
        self.connect_us = None               # when the socket last (re)connected
        # After a reconnect the book is rebuilt from zero, so nothing is trustworthy for a while.
        self.warmup_us = 60_000_000
        # Trade prints, kept briefly so an unmatched negative delta can be called a cancel.
        self.recent_trades = defaultdict(deque)   # (ticker, price) -> deque[(ts_us, qty)]
        self.trade_match = defaultdict(int)

    # -- ingest --
    def on_event(self, src, ev):
        self.events[src] += 1
        recv = check_epoch(ev.get('recv_ts_us'), f"{src} recv_ts_us")
        typ = ev.get('ev')
        if typ == '_connect':
            # A reconnect restarts that socket's sequences at 1 and empties the books it feeds.
            # Without this the seq drops from ~13 million to 1, reads as a gap, and every market
            # stays stale for the rest of the tape.
            #
            # SCOPED TO THE SOURCE. Every recorded socket writes its own `_connect`, and an
            # earlier version treated all of them alike: a Coinbase reconnect wiped the Kalshi
            # order books. On a multi-source replay that fired eleven times in 400,000 events and
            # left every book permanently stale, so the run produced zero orders and no error.
            # A reconnect on a price feed invalidates that feed's history and nothing else.
            self.events[f'reconnect_{src}'] += 1
            if src == 'kalshi_margin':
                self.sid_seq.clear()
                self.connect_us = recv
                for bk in self.books.values():
                    bk.bids, bk.asks = {}, {}
                    bk.stale = True
                    bk.seq = None
                    bk.built_from_us = None
                    bk.empty_snapshot = True
                self.recent_trades.clear()
            elif src == 'kalshi_index':
                for fd in self.cf.values():
                    fd.hist_ts.clear()
                    fd.hist_px.clear()
                    fd.last_us = fd.price = None
            elif src.startswith('ext_'):
                venue = src[4:]
                for k, fd in self.ext.items():
                    if k.startswith(venue + ':'):
                        fd.hist_ts.clear()
                        fd.hist_px.clear()
                        fd.last_us = fd.price = None
            return
        if src == 'kalshi_margin':
            self._kalshi(ev, recv)
        elif src == 'kalshi_index':
            self._cf(ev, recv)
        elif src.startswith('ext_'):
            self._ext(src[4:], ev, recv)

    def _kalshi(self, ev, recv):
        typ, m = ev.get('ev'), ev.get('msg') or {}
        sid, seq_ = ev.get('sid'), ev.get('seq')
        # Continuity is a property of the SUBSCRIPTION. A gap on sid 1 means a frame was lost
        # that could have belonged to any market on it, so they all go stale together.
        if sid is not None and isinstance(seq_, int):
            prev = self.sid_seq.get(sid)
            if prev is not None:
                if seq_ == prev:
                    self.events['duplicate_seq'] += 1
                    return                      # a replayed frame: never applied twice
                if seq_ < prev:
                    self.events['out_of_order'] += 1
                    return                      # a late frame: never rewinds the book
                if seq_ != prev + 1:
                    # A real gap. This feed never re-snapshots mid-stream, so the books on this
                    # subscription cannot be repaired -- they stay stale until the next connect.
                    # That is the honest state, and it is why the recorder should force a
                    # reconnect on a gap rather than keep streaming into a book it knows is wrong.
                    self.events['sid_gap'] += 1
                    for bk_ in self.books.values():
                        bk_.stale = True
                        bk_.gap_count += 1
            self.sid_seq[sid] = seq_
        t = m.get('market_ticker')
        if not t:
            return
        if self.only_markets is not None and t not in self.only_markets:
            self.events['skipped_market'] += 1
            return
        bk = self.books[t]
        if bk.ticker != t:
            bk.ticker = t
        if typ == 'trade':
            self._trade(t, bk, m, ev, recv)
            return
        # The engine prefers the exchange's own timestamp and falls back to our receive time,
        # recording which was used so a study can exclude the fallback if it matters.
        ts = check_epoch(ev.get('src_ts_us'), 'kalshi src_ts_us') or recv
        seq = ev.get('seq')
        if typ == '_gap':
            bk.stale = True
            bk.gap_count += 1
            return
        if typ == 'orderbook_snapshot':
            bk.snapshot(m.get('bid') or [], m.get('ask') or [], seq, ts)
            self.seen_seq[t] = {seq}
            return
        if typ == 'orderbook_delta':
            d = num(m.get('delta'), default=None)
            px = num(m.get('price'), default=None)
            applied = bk.delta(m.get('price'), m.get('delta'), m.get('side'), seq, ts)
            if applied and d is not None and self.track_flow:
                reason = m.get('last_update_reason', '')
                if reason:
                    # A feed that publishes reasons: use them, they are authoritative.
                    self.flow[t].on_delta(ts, m.get('side'), d, reason)
                    if reason in TRADE_REASONS:
                        bk.last_trade_us = ts
                elif d > 0:
                    self.flow[t].on_delta(ts, m.get('side'), d, '')        # an add
                elif px is not None and not self._matched_trade(t, px, ts, -d):
                    # Negative, and no trade printed at this price in the matching window, so it
                    # is a cancel. Trades themselves are counted from the trade channel, never
                    # from the delta, so that no volume is counted twice.
                    self.flow[t].on_delta(ts, m.get('side'), d, 'MarginCancel')
                    self.trade_match['delta_called_cancel'] += 1
            mid = bk.mid
            if mid is not None and (bk.last_mid is None or abs(mid - bk.last_mid) > 1e-12):
                bk.last_mid, bk.last_mid_change_us = mid, ts
            return

    # The window for calling a negative delta a cancel. Trade prints and book deltas arrive on
    # different subscriptions with different latency, so the pairing is time-boxed, not exact.
    TRADE_MATCH_US = 300_000

    def _matched_trade(self, t, price, ts, qty):
        q = self.recent_trades.get((t, round(price, 6)))
        if not q:
            return False
        while q and ts - q[0][0] > self.TRADE_MATCH_US:
            q.popleft()
        for i, (tts, tq) in enumerate(q):
            if abs(tts - ts) <= self.TRADE_MATCH_US:
                del q[i]
                self.trade_match['delta_matched_to_trade'] += 1
                return True
        return False

    def _trade(self, t, bk, m, ev, recv):
        """A print on the trade channel. This is the ONLY source of trade volume on this feed.

        `taker_side` is the taker's own side: 'bid' is an aggressive buyer lifting the offer.
        """
        ts = check_epoch(ev.get('src_ts_us'), 'kalshi trade src_ts_us') or recv
        px, cnt = num(m.get('price'), default=None), num(m.get('count'), default=None)
        if px is None or cnt is None or cnt <= 0:
            return
        taker = m.get('taker_side')
        bk.last_trade_us = ts
        if self.track_flow:
            f = self.flow[t]
            for w in f.updates.values():
                w.add(ts, 1.0)
            tgt = f.trade_buy if taker == 'bid' else f.trade_sell
            for w in tgt.values():
                w.add(ts, cnt)
        q = self.recent_trades[(t, round(px, 6))]
        q.append((ts, cnt))
        while q and ts - q[0][0] > self.TRADE_MATCH_US:
            q.popleft()
        self.trade_match['trades'] += 1

    def _cf(self, ev, recv):
        m = ev.get('msg') or {}
        idx = m.get('index_id')
        if not idx:
            return
        ts = check_epoch(ev.get('src_ts_us'), 'cf src_ts_us') or recv
        self.cf[idx].update(ts, m.get('value_usd'))

    def _ext(self, venue, ev, recv):
        m = ev.get('msg') or {}
        if not isinstance(m, dict):
            return
        sym = m.get('product_id') or m.get('symbol') or m.get('s') or m.get('coin')
        px = (m.get('price') or m.get('last') or m.get('c') or m.get('b') or
              (m.get('bid') if isinstance(m.get('bid'), (int, float, str)) else None))
        if sym is None or px is None:
            return
        ts = check_epoch(ev.get('src_ts_us'), f'{venue} src_ts_us') or recv
        self.ext[f'{venue}:{sym}'].update(ts, px)

    # -- emit --
    def row(self, t, now_us, lane='impulse'):
        bk = self.books.get(t)
        if bk is None:
            self.suppressed['no_book'] += 1
            return None
        if bk.stale:
            self.suppressed['book_stale_or_gapped'] += 1
            return None
        if not bk.usable():
            self.suppressed['crossed_or_one_sided'] += 1
            return None
        age = now_us - (bk.last_update_us or 0)
        if age > MAX_AGE_US['kalshi']:
            self.suppressed['kalshi_stale'] += 1
            return None
        # Warm-up. After a connect the book is rebuilt from an empty snapshot by streamed deltas.
        # Top of book is right within a tenth of a second; DEPTH is not, and an OBI computed over
        # five levels when only two have arrived is a number about our subscription, not the
        # market. Rows are suppressed until the book has had time to fill.
        since_connect = (now_us - self.connect_us) if self.connect_us else None
        # Warm-up is charged only to books that had to be REBUILT. A snapshot that arrived with
        # depth in it is complete on arrival; making it wait 60 seconds discards good rows for
        # no reason. A snapshot that arrived empty has to stream its depth back in, and until it
        # has, every depth-derived feature describes the subscription rather than the market.
        if bk.empty_snapshot and since_connect is not None and since_connect < self.warmup_us:
            self.suppressed['book_warming_up'] += 1
            return None
        f, mid = self.flow[t], bk.mid
        # The underlying-equivalent price, which is what an index or an external venue quotes.
        cs = self.contract_size.get(t)
        underlying = (mid / cs) if cs else None
        r = {'market': t, 'feature_ts_us': now_us, 'book_seq': bk.seq,
             'book_gap_count': bk.gap_count, 'kalshi_age_us': age,
             'since_connect_s': (since_connect / 1e6) if since_connect is not None else None,
             'n_bid_levels': len(bk.bids), 'n_ask_levels': len(bk.asks),
             'mid': mid, 'microprice': bk.microprice, 'spread_bps': bk.spread_bps,
             'microprice_minus_mid_bps': (bk.microprice - mid) / mid * 1e4,
             'bid_depth_1': bk.depth(1, 'bid'), 'ask_depth_1': bk.depth(1, 'ask'),
             'bid_depth_5': bk.depth(5, 'bid'), 'ask_depth_5': bk.depth(5, 'ask'),
             'bid_depth_10': bk.depth(10, 'bid'), 'ask_depth_10': bk.depth(10, 'ask'),
             'time_since_trade_us': (now_us - bk.last_trade_us) if bk.last_trade_us else None,
             'time_since_mid_change_us': ((now_us - bk.last_mid_change_us)
                                          if bk.last_mid_change_us else None)}
        for n in (1, 3, 5, 10):
            r[f'L{n}_OBI'] = bk.obi(n)
        for w in WINDOWS_US:
            r[f'trade_imbalance_{w}'] = Flow.imbalance(f.trade_buy[w], f.trade_sell[w], now_us)
            r[f'cancel_imbalance_{w}'] = Flow.imbalance(f.cancel_bid[w], f.cancel_ask[w], now_us)
            r[f'add_imbalance_{w}'] = Flow.imbalance(f.add_bid[w], f.add_ask[w], now_us)
            r[f'aggressive_buy_{w}'] = f.trade_buy[w].sum(now_us)
            r[f'aggressive_sell_{w}'] = f.trade_sell[w].sum(now_us)
            r[f'cancel_bid_{w}'] = f.cancel_bid[w].sum(now_us)
            r[f'cancel_ask_{w}'] = f.cancel_ask[w].sum(now_us)
            r[f'book_update_rate_{w}'] = f.updates[w].count(now_us) / (WINDOWS_US[w] / 1e6)

        # CF: the contract's reference anchor. Stale CF suppresses the row rather than
        # forward-filling, because a basis against a 30-second-old index is not a basis.
        idx = self.cf_map.get(t)
        cf = self.cf.get(idx) if idx else None
        cf_age = cf.age_us(now_us) if cf else None
        if cf is not None and cf.price:
            if cf_age is not None and cf_age > MAX_AGE_US['cf']:
                self.suppressed['cf_stale'] += 1
                return None
            r['cf_age_us'] = cf_age
            r['cf_index'] = cf.price
            r['contract_size'] = cs
            r['underlying_px'] = underlying
            r['cf_basis_bps'] = ((underlying / cf.price - 1) * 1e4) if underlying else None
            for w, span in (('1s', 1_000_000), ('5s', 5_000_000), ('30s', 30_000_000),
                            ('1m', 60_000_000), ('2m', 120_000_000)):
                r[f'cf_return_{w}'] = cf.ret_bps(now_us, span)
        else:
            r['cf_age_us'] = None
            r['cf_basis_bps'] = None

        # External venues, each with its own age. A venue past its limit contributes nothing
        # rather than a stale price; the composite says how many venues were actually fresh.
        fresh, comp = [], []
        for key in self.ext_map.get(t, []):
            fd = self.ext.get(key)
            a = fd.age_us(now_us) if fd else None
            r[f'{key}_age_us'] = a
            if fd is None or fd.price is None or a is None or a > MAX_AGE_US['external']:
                r[f'{key}_basis_bps'] = None
                continue
            fresh.append(key)
            r[f'{key}_basis_bps'] = ((underlying / fd.price - 1) * 1e4) if underlying else None
            for w, span in (('1s', 1_000_000), ('5s', 5_000_000), ('30s', 30_000_000),
                            ('1m', 60_000_000), ('2m', 120_000_000)):
                v = fd.ret_bps(now_us, span)
                r[f'{key}_return_{w}'] = v
                if w == '5s' and v is not None:
                    comp.append(v)
        r['external_venues_fresh'] = len(fresh)
        r['external_composite_return_5s'] = (sum(comp) / len(comp)) if comp else None
        r['cross_venue_dispersion_5s'] = (
            (max(comp) - min(comp)) if len(comp) > 1 else None)
        r['cross_venue_agreement'] = (
            (sum(1 for v in comp if v > 0) / len(comp)) if comp else None)
        r['lane'] = lane
        return r


# ---- replay ----------------------------------------------------------------------------------
def replay(raw_dir, hours=None, markets=None, interval_us=1_000_000, limit=None):
    """Walk the recorder's files in timestamp order and yield feature rows.

    Files are per (source, hour). Events are merged on receive time so one market's book and the
    external feeds advance together, which is the only way a cross-venue feature means anything.
    """
    eng = FeatureEngine()
    paths = []
    for src_dir in sorted(glob.glob(os.path.join(raw_dir, 'source=*'))):
        src = os.path.basename(src_dir).split('=', 1)[1]
        for hd in sorted(glob.glob(os.path.join(src_dir, 'hour=*'))):
            hour = os.path.basename(hd).split('=', 1)[1]
            if hours and hour not in hours:
                continue
            for f in sorted(glob.glob(os.path.join(hd, '*.jsonl.gz'))):
                paths.append((src, f))
    if not paths:
        return

    def events():
        for src, path in paths:
            with gzip.open(path, 'rt') as fh:
                for line in fh:
                    try:
                        yield src, json.loads(line)
                    except Exception:
                        eng.events['unparseable'] += 1

    buf = sorted(events(), key=lambda p: p[1].get('recv_ts_us') or 0)
    next_emit, emitted = None, 0
    for src, ev in buf:
        ts = ev.get('recv_ts_us')
        if not ts:
            continue
        eng.on_event(src, ev)
        if next_emit is None:
            next_emit = ts + interval_us
        while ts >= next_emit:
            for t in sorted(eng.books):
                if markets and t not in markets:
                    continue
                row = eng.row(t, next_emit)
                if row:
                    emitted += 1
                    yield row
            next_emit += interval_us
            if limit and emitted >= limit:
                return
    yield from ()
    replay.engine = eng


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--raw', default=os.path.join(ROOT, 'data', 'raw'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'data', 'features'))
    ap.add_argument('--hours', nargs='*')
    ap.add_argument('--markets', nargs='*')
    ap.add_argument('--interval-ms', type=int, default=1000)
    ap.add_argument('--limit', type=int)
    ap.add_argument('--dry-run', action='store_true', help='count rows, write nothing')
    a = ap.parse_args()

    eng_rows, by_market = 0, defaultdict(list)
    for r in replay(a.raw, a.hours, set(a.markets) if a.markets else None,
                    a.interval_ms * 1000, a.limit):
        eng_rows += 1
        by_market[r['market']].append(r)
    print(f"{eng_rows:,} feature rows across {len(by_market)} markets")
    for t in sorted(by_market):
        print(f"  {t:14s} {len(by_market[t]):,}")
    if a.dry_run or not eng_rows:
        return
    try:
        import pyarrow as pa, pyarrow.parquet as pq
    except ImportError:
        sys.exit("need `pip install pyarrow` to write Parquet; --dry-run works without it")
    import datetime as dt
    for t, rows in by_market.items():
        # Partition by date and market, as the research plan specifies.
        for day in sorted({dt.datetime.utcfromtimestamp(r['feature_ts_us'] / 1e6).strftime('%Y-%m-%d')
                           for r in rows}):
            sel = [r for r in rows
                   if dt.datetime.utcfromtimestamp(r['feature_ts_us'] / 1e6).strftime('%Y-%m-%d') == day]
            d = os.path.join(a.out, f'date={day}', f'market={t}')
            os.makedirs(d, exist_ok=True)
            keys = sorted({k for r in sel for k in r})
            tbl = pa.table({k: [r.get(k) for r in sel] for k in keys})
            pq.write_table(tbl, os.path.join(d, 'features.parquet'))
            print(f"  wrote {len(sel):,} rows -> {d}")


if __name__ == '__main__':
    main()
