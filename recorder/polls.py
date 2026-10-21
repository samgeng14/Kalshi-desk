"""The recorder's REST pollers (plan section 7, "by public REST"): thin books every few seconds,
trade backfill, the hourly incentive programs and the daily market listing. Each takes a Venue
(GETs under a rate limit) and the store, so its tests drive it with a fake venue and fixtures.

Every list here is explicit: the books and the trades are read for the markets the recorder
was told to record, never "all markets" (an empty list polls nothing).
"""
import asyncio
import json
import time
import urllib.parse

from . import kalshi
from .store import now_us


def short(e):
    s = str(e) or type(e).__name__
    return s if len(s) <= 200 else s[:200] + '...'


class RateLimiter:
    """At most `rate` requests a second on average, `burst` at once: a token bucket every caller
    awaits before its request. A caller takes its token at once and waits for it if it is owed
    (tokens below zero), so concurrent callers queue in order instead of racing. Kalshi limits
    reads per key; the recorder's budgets (REC_REST_RPS + REC_REST_BOOK_RPS) stay far under it."""

    def __init__(self, rate, burst=None, clock=time.monotonic):
        self.rate = float(rate)
        self.burst = float(burst if burst is not None else max(1.0, rate))
        self.tokens = self.burst
        self.clock = clock
        self.t = clock()

    def delay(self):
        """Take a token; the seconds to wait before using it."""
        now = self.clock()
        self.tokens = min(self.burst, self.tokens + (now - self.t) * self.rate)
        self.t = now
        self.tokens -= 1.0
        return 0.0 if self.tokens >= 0 else -self.tokens / self.rate

    async def take(self):
        d = self.delay()
        if d > 0:
            await asyncio.sleep(d)


class Venue:
    """Kalshi's REST GETs from asyncio: each one waits for the limiter, then runs in a thread."""

    def __init__(self, rest, limiter):
        self.rest = rest
        self.limiter = limiter

    async def get(self, route, params=None):
        await self.limiter.take()
        return await asyncio.to_thread(self.rest.get, route, params)

    async def pages(self, route, params, key, max_pages):
        """Each page's `key` list and the cursor after it ('' at the end; non-empty after the last
        page means max_pages cut the listing short). A failed page raises."""
        cursor = ''
        for _ in range(max_pages):
            q = dict(params)
            if cursor:
                q['cursor'] = cursor
            page = await self.get(route, q)
            items = page.get(key) if isinstance(page, dict) else None
            items = items if isinstance(items, list) else []
            cursor = kalshi.cursor_of(page) if items else ''   # an empty page ends it, cursor or not
            yield items, cursor
            if not cursor:
                return


class Outage:
    """A polled feed's failures in the gaps table: a poll_failed row when it starts failing (and
    again at most every every_s while it stays down), then one recovered row spanning the whole
    outage, so recorder/report.py counts every minute of it."""

    def __init__(self, gap, source, every_s=600):
        self.gap = gap
        self.source = source
        self.every_s = every_s
        self.since = None

    def failed(self, detail, recv_us):
        if self.since is None:
            self.since = recv_us
        self.gap(self.source, 'poll_failed', detail, from_us=self.since, to_us=recv_us,
                 key=('poll', self.source), every_s=self.every_s)

    def ok(self, recv_us):
        if self.since is not None:
            self.gap(self.source, 'recovered', 'down %.0f s' % ((recv_us - self.since) / 1e6),
                     from_us=self.since, to_us=recv_us)
            self.since = None


# ---- thin books ----------------------------------------------------------------------------------

TOP_KEYS = ('yes_bid', 'yes_bid_size', 'yes_ask', 'yes_ask_size', 'last_price', 'volume')
TOP_RAW = ('yes_bid_dollars', 'yes_bid_size_fp', 'yes_ask_dollars', 'yes_ask_size_fp', 'last_price_dollars',
           'volume_fp', 'yes_bid', 'yes_ask', 'last_price', 'volume', 'status')


class RestBooks:
    """Books by public REST for an explicit list of markets ({ticker: mode}), every period_s:

    - the top of book of every listed market, in batches (GET /markets?tickers=...): a few
      requests cover hundreds of thin markets, so each is seen every period_s;
    - then the whole book (GET /markets/{t}/orderbook) of the markets whose top moved in the
      last hot_s, the longest unread first, until the next top scan is due. In a live game the
      top moves and its depth is read every few seconds; a quiet pregame book is not re-read.

    A row is written only when a book differs from the last one recorded for it (recv_us, the
    first time it was seen; prev_us, the poll before). The limiter decides how fast; the list
    decides what: a market off the list is forgotten (its next row starts fresh)."""

    BATCH = 100   # tickers per GET /markets: ~4 kB of query, inside common URL limits (8 kB+)

    def __init__(self, venue, store, gap, listing, period_s=4.0, hot_s=300.0, clock=time.monotonic):
        self.venue = venue
        self.store = store
        self.listing = listing
        self.period_s = period_s
        self.hot_s = hot_s
        self.clock = clock
        self.last = {}       # (kind, ticker) -> [digest, the last poll's recv_us, polls that saw it]
        self.moved = {}      # ticker -> clock() when its top last changed
        self.depth_at = {}   # ticker -> clock() of its last depth read
        self.batch = self.BATCH
        self.rows = 0
        self.outage = Outage(gap, 'kalshi_rest_books')

    def record(self, kind, mode, ticker, digest, sent, recv, fields, raw):
        key = (kind, ticker)
        prev = self.last.get(key)
        if prev is not None and prev[0] == digest:
            prev[1] = recv
            prev[2] += 1
            return False
        row = {'recv_us': recv, 'sent_us': sent, 'prev_us': prev[1] if prev else None,
               'polls': prev[2] if prev else 0, 'kind': kind, 'mode': mode, 'ticker': ticker,
               'last_price': None, 'volume': None, 'raw': raw}
        row.update(fields)
        self.store.write('kalshi_rest_books', row)
        self.last[key] = [digest, recv, 1]
        self.rows += 1
        return True

    def forget(self, listed):
        for key in [k for k in self.last if k[1] not in listed]:
            del self.last[key]
        for d in (self.moved, self.depth_at):
            for t in [t for t in d if t not in listed]:
                del d[t]

    async def tops(self, listed):
        tickers = sorted(listed)
        for i in range(0, len(tickers), self.batch):
            chunk = tickers[i:i + self.batch]
            sent = now_us()
            try:
                page = await self.venue.get('/markets', {'tickers': ','.join(chunk), 'limit': len(chunk)})
            except kalshi.HttpError as e:
                # the venue documents no cap on tickers per request; if it has one, a refused batch
                # halves the batch for the cycles after (this one fails, as an outage row)
                if e.status in (400, 413, 414) and len(chunk) > 1:
                    self.batch = max(1, len(chunk) // 2)
                raise
            recv = now_us()
            for m in (page.get('markets') or []) if isinstance(page, dict) else []:
                t = m.get('ticker') if isinstance(m, dict) else None
                if t not in listed:
                    continue
                top = kalshi.top_of_book(m)
                raw = json.dumps({k: m[k] for k in TOP_RAW if k in m}, separators=(',', ':'), sort_keys=True)
                if self.record('top', listed[t], t, tuple(top[k] for k in TOP_KEYS), sent, recv, top, raw):
                    self.moved[t] = self.clock()

    async def depth(self, t, mode):
        sent = now_us()
        try:
            resp = await self.venue.get('/markets/%s/orderbook' % urllib.parse.quote(t, safe=''))
        except kalshi.HttpError as e:
            if e.status != 404:
                raise
            self.depth_at[t] = self.clock()   # gone from the venue: to the back, not retried first
            return
        recv = now_us()
        self.depth_at[t] = self.clock()
        levels = kalshi.book_levels(resp)
        if levels is None:
            return
        book = resp.get('orderbook_fp') if isinstance(resp.get('orderbook_fp'), dict) else resp.get('orderbook')
        raw = json.dumps(book, separators=(',', ':'), sort_keys=True)
        self.record('depth', mode, t, raw, sent, recv, kalshi.best_of(*levels), raw)

    async def cycle(self, deadline):
        listed = self.listing()
        self.forget(listed)
        if not listed:
            return
        await self.tops(listed)
        now = self.clock()
        hot = [t for t in listed if now - self.moved.get(t, float('-inf')) <= self.hot_s]
        hot.sort(key=lambda t: (self.depth_at.get(t, float('-inf')), t))
        for t in hot:
            if self.clock() >= deadline:
                break   # the next top scan is due; the rest come first next cycle
            await self.depth(t, listed[t])

    async def run(self, stop, sleep):
        while not stop.is_set():
            began = self.clock()
            try:
                await self.cycle(began + self.period_s)
                self.outage.ok(now_us())
            except Exception as e:
                self.outage.failed(short(e), now_us())
            await sleep(max(0.0, began + self.period_s - self.clock()))


# ---- trade backfill ------------------------------------------------------------------------------

def merge_windows(windows):
    """Overlapping (from_us, to_us, reason) windows as one, reasons joined, oldest first."""
    out = []
    for a, b, why in sorted(windows):
        if out and a <= out[-1][1]:
            pa, pb, pwhy = out[-1]
            whys = pwhy.split('+')
            out[-1] = (pa, max(pb, b), pwhy if why in whys else pwhy + '+' + why)
        else:
            out.append((a, b, why))
    return out


class TradeBackfill:
    """GET /markets/trades over windows the socket may have missed: a disconnect (from the last
    frame before it to the resubscription), a sequence gap on the trade subscription, a restart,
    and every hour. Read for each market recorded during the window (tickers_for), each print
    written once: overlapping windows and repeated hours return the same trade_ids.

    save(windows), when given, is called with every window queued or not yet read each time
    that changes, so a run killed before it read them (a watchdog kill, a redeploy mid-drain)
    leaves them for the next run's restore()."""

    PAD_US = 60_000_000          # either side of a window: the venue's and our clocks differ
    KEEP_US = 2 * 86400 * 1_000_000

    def __init__(self, venue, store, gap, tickers_for, max_pages=20, save=None):
        self.venue = venue
        self.store = store
        self.tickers_for = tickers_for
        self.max_pages = max_pages
        self.save = save
        self.windows = []
        self.reading = []        # the running drain's windows not finished yet
        self.seen = {}           # trade_id -> when it was recorded (us), forgotten KEEP_US later
        self.rows = 0
        self.outage = Outage(gap, 'kalshi_trades_rest')

    def add(self, from_us, to_us, reason):
        if from_us is None or to_us is None or to_us < from_us:
            return
        self.windows.append((from_us - self.PAD_US, to_us + self.PAD_US, reason))
        self.saved()

    def pending(self):
        """Every window queued or not yet read, merged."""
        return merge_windows(self.reading + self.windows)

    def saved(self):
        if self.save is not None:
            self.save(self.pending())

    def restore(self, windows, now):
        """Queue again the windows a previous run saved and never read (already padded); one
        that ended over KEEP_US ago, or is unreadable, is dropped. How many were queued."""
        n = 0
        for w in windows if isinstance(windows, list) else []:
            try:
                a, b, why = int(w[0]), int(w[1]), str(w[2])
            except (TypeError, ValueError, IndexError, KeyError):
                continue
            if a <= b and b >= now - self.KEEP_US:
                self.windows.append((a, b, why))
                n += 1
        return n

    def seed(self, rows):
        """Trade ids already recorded (the table's recent rows on disk), so a restart does not
        write the last hour's prints again."""
        for r in rows:
            tid = r.get('trade_id')
            if tid:
                self.seen[tid] = r.get('recv_us') or 0

    def keep(self, t, recv, reason):
        """The row for one print, or None if it was recorded already."""
        f = kalshi.trade_fields(t)
        tid = f['trade_id']
        if not tid or tid in self.seen:
            return None
        # remembered from when we recorded it, not from its created_time: no window reaches back
        # two days, and a venue clock far off cannot make an id be forgotten early
        self.seen[tid] = recv
        f.update(recv_us=recv, reason=reason, raw=json.dumps(t, separators=(',', ':')))
        return f

    def prune(self, now):
        for tid in [k for k, v in self.seen.items() if v < now - self.KEEP_US]:
            del self.seen[tid]

    async def window(self, a, b, reason):
        for t in sorted(self.tickers_for(a, b)):
            params = {'ticker': t, 'min_ts': a // 1_000_000, 'max_ts': b // 1_000_000 + 1, 'limit': 1000}
            async for items, _ in self.venue.pages('/markets/trades', params, 'trades', self.max_pages):
                recv = now_us()
                for tr in items:
                    row = self.keep(tr, recv, reason) if isinstance(tr, dict) else None
                    if row:
                        self.store.write('kalshi_trades_rest', row)
                        self.rows += 1

    async def drain(self):
        """Read every queued window; one that fails goes back on the queue (with the ones after
        it) and the failure is raised for the outage row."""
        todo, self.windows = merge_windows(self.windows), []
        for i, (a, b, reason) in enumerate(todo):
            self.reading = todo[i:]
            self.saved()             # the ones before are read
            try:
                await self.window(a, b, reason)
            except Exception:
                self.reading = []
                self.windows.extend(todo[i:])
                self.saved()
                raise
        self.reading = []
        self.saved()
        self.prune(now_us())


# ---- the hourly and daily sweeps -----------------------------------------------------------------

# A sweep never raises: it returns what it wrote, with the error that stopped it, so the caller
# can say which sweep_us is partial (its rows stay; research keeps the complete sweeps).

async def incentives_sweep(venue, store, types, status='active', max_pages=50):
    """Every page of GET /incentive_programs; the programs of the kept types written, all with one
    sweep_us. {sweep_us, rows, dropped, cut (max_pages reached), error}."""
    r = {'sweep_us': now_us(), 'rows': 0, 'dropped': 0, 'cut': False, 'error': None}
    try:
        async for items, cursor in venue.pages('/incentive_programs', {'status': status, 'limit': 1000},
                                               'incentive_programs', max_pages):
            recv = now_us()
            for p in items:
                if not kalshi.keep_incentive(p, types):
                    r['dropped'] += 1
                    continue
                row = kalshi.incentive_fields(p)
                row.update(recv_us=recv, sweep_us=r['sweep_us'], raw=json.dumps(p, separators=(',', ':')))
                store.write('kalshi_incentives', row)
                r['rows'] += 1
            r['cut'] = bool(cursor)
    except Exception as e:
        r['error'] = short(e)
    return r


DAILY_DROP = ('rules_primary', 'rules_secondary')   # long text, constant, already in kalshi_markets


def daily_row(m, recv, sweep):
    return {'recv_us': recv, 'sweep_us': sweep, 'ticker': kalshi.text_of(m, 'ticker'),
            'event_ticker': kalshi.text_of(m, 'event_ticker'), 'status': kalshi.text_of(m, 'status'),
            'open_time': kalshi.text_of(m, 'open_time'), 'close_time': kalshi.text_of(m, 'close_time'),
            'volume': kalshi.count(m, 'volume'), 'open_interest': kalshi.count(m, 'open_interest'),
            'yes_bid': kalshi.dollars(m, 'yes_bid'), 'yes_ask': kalshi.dollars(m, 'yes_ask'),
            'raw': json.dumps({k: v for k, v in m.items() if k not in DAILY_DROP}, separators=(',', ':'))}


async def markets_sweep(venue, store, params, max_pages=500):
    """Every page of GET /markets with params; each market a row, all with one sweep_us.
    {sweep_us, rows, cut, error}."""
    r = {'sweep_us': now_us(), 'rows': 0, 'cut': False, 'error': None}
    try:
        async for items, cursor in venue.pages('/markets', dict(params, limit=1000), 'markets', max_pages):
            recv = now_us()
            for m in items:
                if isinstance(m, dict):
                    store.write('kalshi_markets_daily', daily_row(m, recv, r['sweep_us']))
                    r['rows'] += 1
            r['cut'] = bool(cursor)
    except Exception as e:
        r['error'] = short(e)
    return r
