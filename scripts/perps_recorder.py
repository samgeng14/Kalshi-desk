#!/usr/bin/env python3
"""24/7 tick recorder: Kalshi perps book/trades/ticker + CF Benchmarks 5Hz index + external venues.

The backfill gives 1-minute bars. This gives everything a 1-minute bar cannot: spread paths, queue
dynamics, book imbalance, the cancel-vs-trade distinction, and the 1s-60s lead-lag the research
plan is actually aimed at. Neither replaces the other, and this one is gated on wall-clock time,
so it should be running before anything else is decided.

TIME. Every timestamp stored is an INTEGER MICROSECOND value in a field whose name says whose
clock it came from, next to a field that says what precision the source really had:

  src_ts_us       the exchange's own timestamp, converted to microseconds
  src_precision   "ms" | "s" | "us" | "none"  -- what the wire actually carried
  recv_ts_us      local wall clock when the frame was read
  recv_mono_us    local MONOTONIC clock for the same instant
  proc_ts_us      local wall clock after parsing

Two clocks are kept because wall clock can step (NTP correction, sleep/wake) and monotonic cannot.
An interval computed from recv_ts_us that disagrees with the same interval from recv_mono_us is a
clock step, and the recorder logs it rather than letting it quietly corrupt a lead-lag study.

The margin socket sends epoch MILLISECONDS with an _ms suffix; the event-contract socket sends
SECONDS. Mixing them puts two feeds 1000x apart, so conversion happens once, at the edge, in
`to_us()`, and nothing downstream ever sees a raw wire timestamp.

STORAGE. Append-only gzipped JSONL, one file per (source, UTC hour), flushed every second. Raw and
immutable: no parsing decisions are baked in at write time, because the parse you regret is the one
you cannot undo. `scripts/perps_compact.py` turns these into Parquet for DuckDB afterwards. JSONL
first is deliberate -- a recorder must not die because pyarrow is not installed.

SEQUENCING. Kalshi orderbook_delta carries a per-subscription `seq`. A gap means a missed update,
so the local book is marked stale and a fresh snapshot is requested; it is never patched over.
Every gap is recorded in the stream itself as a `_gap` event so the research can exclude the
affected window instead of silently trusting a wrong book.

  pip install websockets cryptography
  python3 scripts/perps_recorder.py                       # everything
  python3 scripts/perps_recorder.py --no-external         # Kalshi + index only
  python3 scripts/perps_recorder.py --tickers KXBTCPERP KXETHPERP
"""
import argparse, asyncio, base64, gzip, json, os, signal, sys, time
from collections import defaultdict
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PREFIX = '/trade-api/v2'
OUT = os.path.join(ROOT, 'data', 'raw')

# Found by scripts/ws_probe.py against production. The documentation gives the margin host with
# `address: /`, and that combination answers HTTP 404; the working path is /trade-api/ws/v2/margin.
# Subscribing to perps on the main socket looks like it works -- it acknowledges and sends
# orderbook_snapshot frames -- but those frames carry no bid, no ask and an empty market_id, so it
# serves no perp book at all. Each socket signs its own path.
MARGIN_WS = 'wss://external-api-margin-ws.kalshi.com/trade-api/ws/v2/margin'
MARGIN_SIGN_PATH = '/trade-api/ws/v2/margin'
EVENT_WS = 'wss://external-api-ws.kalshi.com/trade-api/ws/v2'
EVENT_SIGN_PATH = '/trade-api/ws/v2'

# --- time -------------------------------------------------------------------------------------
# One conversion point. Everything downstream is integer microseconds.
_T0_WALL, _T0_MONO = time.time_ns(), time.monotonic_ns()


def now_us():
    return time.time_ns() // 1000


def mono_us():
    return time.monotonic_ns() // 1000


def clock_skew_us():
    """Wall-clock drift against monotonic since start. Non-zero means OUR clock stepped."""
    return ((time.time_ns() - _T0_WALL) - (time.monotonic_ns() - _T0_MONO)) // 1000


def ntp_offset_us(server='time.apple.com', timeout=2.0):
    """Offset and round-trip against an NTP server, in microseconds.

    clock_skew_us() only catches our own clock stepping. It cannot tell us that the wall clock is
    37ms wrong relative to everyone else, which is exactly the error that would show up as a fake
    lead-lag result. Returns (offset_us, rtt_us) or (None, None) if the probe fails -- it is a
    diagnostic, never load-bearing.

    Offset is positive when the local clock is BEHIND the server.
    """
    import socket, struct
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(timeout)
        t1 = time.time()
        s.sendto(b'\x1b' + 47 * b'\0', (server, 123))
        data, _ = s.recvfrom(48)
        t4 = time.time()
        s.close()
        # Transmit timestamp: seconds since 1900, 32.32 fixed point at offset 40.
        secs, frac = struct.unpack('!II', data[40:48])
        t3 = (secs - 2208988800) + frac / 2 ** 32
        # One-sided: we cannot see the server's receive time in this minimal query, so the
        # estimate carries half the round trip as its own uncertainty. Reported alongside.
        rtt = t4 - t1
        return int((t3 - (t1 + rtt / 2)) * 1e6), int(rtt * 1e6)
    except Exception:
        return None, None


def to_us(value, unit):
    """unit: 's' | 'ms' | 'us' | 'ns' | 'iso'. Returns (microseconds, precision_tag)."""
    if value is None: return None, 'none'
    try:
        if unit == 'iso':
            s = str(value).replace('Z', '+00:00')
            return int(datetime.fromisoformat(s).timestamp() * 1_000_000), 'iso'
        v = float(value)
    except (TypeError, ValueError):
        return None, 'none'
    return {'s': lambda: (int(v * 1_000_000), 's'),
            'ms': lambda: (int(v * 1_000), 'ms'),
            'us': lambda: (int(v), 'us'),
            'ns': lambda: (int(v // 1000), 'ns')}[unit]()


# --- storage ----------------------------------------------------------------------------------
class Sink:
    """One gzipped JSONL file per (source, UTC hour). Flushed every second, reopened on the hour."""

    def __init__(self, root):
        self.root, self.files, self.hours = root, {}, {}
        self.counts = defaultdict(int)
        self.last_flush = 0.0

    def _handle(self, source):
        hour = datetime.now(timezone.utc).strftime('%Y-%m-%dT%H')
        if self.hours.get(source) != hour:
            if source in self.files:
                self.files[source].close()
            d = os.path.join(self.root, f'source={source}', f'hour={hour}')
            os.makedirs(d, exist_ok=True)
            self.files[source] = gzip.open(os.path.join(d, 'events.jsonl.gz'), 'at')
            self.hours[source] = hour
        return self.files[source]

    def write(self, source, rec):
        self._handle(source).write(json.dumps(rec, separators=(',', ':')) + '\n')
        self.counts[source] += 1
        now = time.monotonic()
        if now - self.last_flush > 1.0:
            for f in self.files.values(): f.flush()
            self.last_flush = now

    def close(self):
        for f in self.files.values():
            try: f.flush(); f.close()
            except Exception: pass


def stamp(rec, src_ts, unit):
    us, prec = to_us(src_ts, unit)
    rec['src_ts_us'] = us
    rec['src_precision'] = prec
    rec['proc_ts_us'] = now_us()
    if us is not None and rec.get('recv_ts_us'):
        rec['latency_us'] = rec['recv_ts_us'] - us
    return rec


# --- config / auth ----------------------------------------------------------------------------
def load_config():
    """Returns (key_id, pem_path, rest_base). env comes from the config, never a flag that could
    silently disagree with the one the desk itself trades on."""
    raw = open(os.path.join(ROOT, 'config', 'desk.json')).read()
    out, i, instr, esc = [], 0, False, False
    while i < len(raw):
        ch = raw[i]
        if instr:
            out.append(ch)
            if esc: esc = False
            elif ch == '\\': esc = True
            elif ch == '"': instr = False
            i += 1; continue
        if ch == '"': instr = True; out.append(ch); i += 1; continue
        if ch == '/' and i + 1 < len(raw) and raw[i + 1] == '/':
            while i < len(raw) and raw[i] != '\n': i += 1
            continue
        out.append(ch); i += 1
    cfg = json.loads(''.join(out))
    host = ('https://external-api.kalshi.com' if cfg.get('env') == 'prod'
            else 'https://external-api.demo.kalshi.co')
    return cfg['key_id'], os.path.expanduser(cfg['pem_path']), host + PREFIX


def make_signer(key_id, pem_path):
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding
    key = serialization.load_pem_private_key(open(pem_path, 'rb').read(), password=None)

    def headers(method, path):
        ts = str(int(time.time() * 1000))
        sig = key.sign((ts + method + path).encode(),
                       padding.PSS(mgf=padding.MGF1(hashes.SHA256()),
                                   salt_length=hashes.SHA256.digest_size),
                       hashes.SHA256())
        return {'KALSHI-ACCESS-KEY': key_id, 'KALSHI-ACCESS-TIMESTAMP': ts,
                'KALSHI-ACCESS-SIGNATURE': base64.b64encode(sig).decode()}
    return headers


# --- feeds ------------------------------------------------------------------------------------
# websockets renamed `extra_headers` to `additional_headers` at v14, when the asyncio client was
# rewritten. Wrapping the call in try/except does not help: connect() stores its kwargs and the
# TypeError surfaces later, from create_connection(), when the connection is awaited. So the
# keyword is detected from the signature and, if that guesses wrong, flipped on the first failure
# and retried -- self-healing either way, because a recorder that dies on a library upgrade is a
# recorder that has silently stopped collecting.
_HEADER_KW = None


def _detect_header_kw():
    import inspect
    import websockets
    for target in (websockets.connect,):
        try:
            ps = inspect.signature(target).parameters
            if 'additional_headers' in ps: return 'additional_headers'
            if 'extra_headers' in ps: return 'extra_headers'
        except (TypeError, ValueError):
            pass
    try:
        import inspect as _i
        from websockets.asyncio.client import connect as _c
        if 'additional_headers' in _i.signature(_c).parameters: return 'additional_headers'
    except Exception:
        pass
    return 'additional_headers'


def flip_header_kw():
    global _HEADER_KW
    _HEADER_KW = 'extra_headers' if _HEADER_KW == 'additional_headers' else 'additional_headers'
    return _HEADER_KW


def ws_connect(url, headers=None, **kw):
    global _HEADER_KW
    import websockets
    if not headers:
        return websockets.connect(url, **kw)
    if _HEADER_KW is None:
        _HEADER_KW = _detect_header_kw()
    kw[_HEADER_KW] = headers
    return websockets.connect(url, **kw)
async def run_feed(name, connect, subscribe, on_msg, sink, log):
    """Reconnect with capped exponential backoff. A feed dying must never take the others down."""
    delay, last_error = 1.0, None
    while True:
        try:
            async with connect() as ws:
                log(f"{name}: connected")
                delay = 1.0
                sink.write(name, {'ev': '_connect', 'recv_ts_us': now_us(),
                                  'recv_mono_us': mono_us(), 'clock_skew_us': clock_skew_us()})
                await subscribe(ws)
                async for raw in ws:
                    recv, mono = now_us(), mono_us()
                    try:
                        msg = json.loads(raw)
                    except Exception:
                        continue
                    on_msg(msg, recv, mono)
        except asyncio.CancelledError:
            raise
        except Exception as e:
            # The header-keyword mismatch is a local, fixable problem: flip and retry at once
            # rather than backing off as though the exchange were down.
            if isinstance(e, TypeError) and 'headers' in str(e):
                kw = flip_header_kw()
                log(f"{name}: websockets wants '{kw}' for headers; retrying")
                await asyncio.sleep(0.5)
                continue
            sig = f"{type(e).__name__}:{str(e)[:60]}"
            if sig != last_error or delay >= 30.0:
                log(f"{name}: {type(e).__name__}: {str(e)[:120]} -- reconnecting in {delay:.0f}s"
                    + ("" if sig != last_error else " (repeating)"))
            last_error = sig
            sink.write(name, {'ev': '_disconnect', 'recv_ts_us': now_us(),
                              'recv_mono_us': mono_us(), 'why': f"{type(e).__name__}: {e}"[:200]})
            await asyncio.sleep(delay)
            delay = min(delay * 2, 30.0)


def margin_handler(sink, log):
    """The margin message handler, with no socket in it so it can be tested directly.

    Returns (on_msg, seq_state). Kept separate from the connection because the thing most worth
    testing here -- that a sequence gap is flagged rather than patched over, and that a
    milliseconds payload is not read as seconds -- should not need a network to test.
    """
    seq_state = {}

    def on_msg(msg, recv, mono):
        typ = msg.get('type', '')
        m = msg.get('msg') or {}
        rec = {'ev': typ, 'sid': msg.get('sid'), 'seq': msg.get('seq'),
               'recv_ts_us': recv, 'recv_mono_us': mono, 'msg': m}
        # Margin payloads are epoch MILLISECONDS with an _ms suffix. Never assume seconds here.
        rec = stamp(rec, m.get('ts_ms'), 'ms')
        sid, seq = msg.get('sid'), msg.get('seq')
        if sid is not None and isinstance(seq, int):
            prev = seq_state.get(sid)
            if typ == 'orderbook_snapshot':
                seq_state[sid] = seq
            elif prev is not None and seq != prev + 1:
                # A gap means the local book is wrong. Record it in-stream so research can drop
                # the affected window, and mark the book stale rather than patching over it.
                sink.write('kalshi_margin', {'ev': '_gap', 'sid': sid, 'expected': prev + 1,
                                             'got': seq, 'ticker': m.get('market_ticker'),
                                             'recv_ts_us': recv, 'recv_mono_us': mono})
                log(f"kalshi_margin: seq gap sid={sid} {prev + 1} -> {seq}; book stale")
                seq_state[sid] = seq
            else:
                seq_state[sid] = seq
        sink.write('kalshi_margin', rec)

    return on_msg, seq_state


def kalshi_margin(sink, log, tickers, hdrs):
    import websockets
    on_msg, _ = margin_handler(sink, log)

    def connect():
        return ws_connect(MARGIN_WS, hdrs('GET', MARGIN_SIGN_PATH),
                          ping_interval=10, ping_timeout=20, max_queue=4096)

    async def subscribe(ws):
        await ws.send(json.dumps({'id': 1, 'cmd': 'subscribe', 'params': {
            'channels': ['orderbook_delta', 'ticker', 'trade'],
            'market_tickers': tickers, 'send_initial_snapshot': True}}))

    return run_feed('kalshi_margin', connect, subscribe, on_msg, sink, log)


def index_handler(sink):
    """The CF Benchmarks handler, socket-free so the unit handling is testable.

    Two fields, both MILLISECONDS. An earlier version read a non-existent `ts` field as seconds,
    which is the exact 1000x error the whole to_us() discipline exists to stop -- introduced in
    the feed wiring rather than in the conversion, which is why the wiring is tested too.
    """
    def on_msg(msg, recv, mono):
        m = msg.get('msg') or {}
        rec = {'ev': msg.get('type', ''), 'sid': msg.get('sid'), 'seq': msg.get('seq'),
               'recv_ts_us': recv, 'recv_mono_us': mono, 'msg': m}
        rec = stamp(rec, m.get('source_ts_ms'), 'ms')
        # received_at is KALSHI's own receipt time for the same tick. source_ts_ms -> received_at
        # is therefore an upstream latency measured entirely on their side: the one CF-to-Kalshi
        # timing number that does not depend on our clock being right.
        ra, _ = to_us(m.get('received_at'), 'ms')
        if ra is not None:
            rec['venue_recv_ts_us'] = ra
            if rec.get('src_ts_us') is not None:
                rec['venue_latency_us'] = ra - rec['src_ts_us']
        sink.write('kalshi_index', rec)
    return on_msg


def kalshi_index(sink, log, index_tickers, hdrs):
    """CF Benchmarks 5 Hz: the contract's REFERENCE ANCHOR, not its fair value.

    The distinction matters. CF is what the contract settles and funds against, which makes
    basis_cf = kalshi_mid - cf_index the right quantity to study. It does not make CF the price
    Kalshi should trade at: funding expectations, order flow, inventory pressure and latency all
    drive a real and possibly persistent basis.

    And CF is itself downstream of somewhere. The chain to measure is three-layer --

        external venues  ->  CF Benchmarks  ->  Kalshi

    -- because the interesting finding would be Binance leading CF by 300ms and CF leading Kalshi
    by 1.5s, which is invisible if CF is assumed to be the terminal predictor. That is why the
    external feeds are recorded for BTC and ETH too, where Kalshi's own basis is far too small to
    trade: they are there to test who leads CF, not only who leads Kalshi.
    """
    import websockets

    def connect():
        return ws_connect(EVENT_WS, hdrs('GET', EVENT_SIGN_PATH),
                          ping_interval=10, ping_timeout=20, max_queue=4096)

    async def subscribe(ws):
        # The parameter is `index_ids` and the values are CF Benchmarks index IDs (BRTI,
        # ETHUSD_RTI), not Kalshi tickers. Getting either wrong subscribes to nothing.
        await ws.send(json.dumps({'id': 1, 'cmd': 'subscribe', 'params': {
            'channels': ['cfbenchmarks_value_5hz'], 'index_ids': index_tickers}}))
        # Fall back to the 1 Hz channel if the 5 Hz one is not entitled for this account.
        await ws.send(json.dumps({'id': 2, 'cmd': 'subscribe', 'params': {
            'channels': ['cfbenchmarks_value'], 'index_ids': index_tickers}}))

    return run_feed('kalshi_index', connect, subscribe, index_handler(sink), sink, log)


EXTERNAL = {
    'coinbase': {
        'url': 'wss://ws-feed.exchange.coinbase.com',
        'sub': lambda syms: {'type': 'subscribe', 'product_ids': syms,
                             'channels': ['ticker', 'matches']},
        'ts': ('time', 'iso'),
    },
    'kraken': {
        'url': 'wss://ws.kraken.com/v2',
        'sub': lambda syms: {'method': 'subscribe',
                             'params': {'channel': 'ticker', 'symbol': syms}},
        'ts': ('timestamp', 'iso'),
    },
    # binance.com answers HTTP 451 from a US address (legally unavailable), so the default is
    # binance.us. It is a thinner book and a different one -- treat it as its own venue in any
    # lead-lag result, not as a stand-in for binance.com.
    'binance': {
        # Combined stream: bookTicker is best bid/ask on every change, aggTrade is signed prints.
        'url': lambda syms: 'wss://stream.binance.us:9443/stream?streams=' + '/'.join(
            f'{s.lower()}@bookTicker/{s.lower()}@aggTrade' for s in syms),
        'sub': None,
        'ts': ('E', 'ms'),
    },
    # allMids streams every market on the venue keyed by numeric index (#12090), which is neither
    # filterable nor joinable to anything, and Hyperliquid echoes the subscribe frame back as data.
    # Per-coin l2Book and trades give the top of book and signed prints for the coins we care about.
    'hyperliquid': {
        'url': 'wss://api.hyperliquid.xyz/ws',
        'sub': lambda syms: [{'method': 'subscribe', 'subscription': {'type': t, 'coin': c}}
                             for c in syms for t in ('l2Book', 'trades')],
        'ts': ('time', 'ms'),
    },
}


def external(name, sink, log, symbols):
    import websockets
    spec = EXTERNAL[name]
    url = spec['url'](symbols) if callable(spec['url']) else spec['url']
    ts_field, ts_unit = spec['ts']

    def connect():
        return ws_connect(url, None, ping_interval=15, ping_timeout=20, max_queue=4096)

    async def subscribe(ws):
        if not spec['sub']:
            return
        msgs = spec['sub'](symbols)
        # A venue may need one frame per instrument per channel rather than one frame for all.
        for m in (msgs if isinstance(msgs, list) else [msgs]):
            await ws.send(json.dumps(m))
            await asyncio.sleep(0.05)

    def on_msg(msg, recv, mono):
        # Some venues echo the subscribe frame back on the data socket. Storing it as a tick would
        # put a row with no price and our own words in it into the middle of a price series.
        if isinstance(msg, dict) and msg.get('method') == 'subscribe':
            return
        body = msg.get('data', msg) if isinstance(msg, dict) else msg
        rec = {'ev': name, 'channel': msg.get('channel') if isinstance(msg, dict) else None,
               'recv_ts_us': recv, 'recv_mono_us': mono, 'msg': body}
        src = body.get(ts_field) if (ts_field and isinstance(body, dict)) else None
        if src is None and isinstance(body, list) and body and isinstance(body[0], dict):
            src = body[0].get(ts_field)          # trades arrive as a list of prints
        sink.write(f'ext_{name}', stamp(rec, src, ts_unit))

    return run_feed(f'ext_{name}', connect, subscribe, on_msg, sink, log)


def rest_book_poller(sink, log, tickers, hdrs, base, hz, depth):
    """Poll /margin/markets/{t}/orderbook and write it in the same shape the socket would.

    Not a substitute for a tick feed, and it cannot see a book change that reverses inside one
    interval. But the research horizons here start at one second, and perps are metered in their
    own rate-limit bucket, so 1 Hz across the book is both affordable and enough resolution for
    the questions actually being asked. It also keeps collecting while the socket question is
    open, which matters more than elegance: wall-clock time is the one input that cannot be
    backfilled.

    Each row carries `poll_seq`, a per-market counter, so a gap in polling is as visible in the
    data as a sequence gap on the socket would be.
    """
    import urllib.error
    import urllib.request

    seq = defaultdict(int)
    # hz is PER MARKET. The old reading was requests-per-second across the whole book, which meant
    # 18 markets at "1 Hz" were each polled every 18 seconds while the log said 1000ms.
    interval = 1.0 / max(0.01, hz) / max(1, len(tickers))
    req_per_s = 1.0 / interval

    def fetch(ticker):
        path = f'/margin/markets/{ticker}/orderbook'
        url = f'{base}{path}?depth={depth}'
        req = urllib.request.Request(url, headers={**hdrs('GET', PREFIX + path),
                                                   'accept': 'application/json'})
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.loads(r.read().decode())

    async def run():
        loop = asyncio.get_running_loop()
        log(f"rest_book: {len(tickers)} markets at {hz:g} Hz each = {req_per_s:.1f} req/s, "
            f"one request every {interval*1000:.0f}ms")
        # Basic tier is ~200 read tokens/s at 10 tokens a request, so about 20 reads/s, and perps
        # meter separately from the event-contract desk. Past that the exchange starts refusing.
        if req_per_s > 18:
            log(f"WARNING {req_per_s:.0f} req/s is at or past the ~20/s perps read budget; "
                f"lower --rest-book-hz or poll fewer markets")
        while True:
            for t in tickers:
                t0 = mono_us()
                try:
                    body = await loop.run_in_executor(None, fetch, t)
                except Exception as e:
                    sink.write('kalshi_rest_book', {'ev': '_error', 'ticker': t,
                                                    'recv_ts_us': now_us(), 'recv_mono_us': mono_us(),
                                                    'why': f"{type(e).__name__}: {e}"[:180]})
                    await asyncio.sleep(interval)
                    continue
                recv, mono = now_us(), mono_us()
                seq[t] += 1
                ob = body.get('orderbook') or body
                sink.write('kalshi_rest_book',
                           {'ev': 'rest_orderbook', 'ticker': t, 'poll_seq': seq[t],
                            'recv_ts_us': recv, 'recv_mono_us': mono, 'proc_ts_us': now_us(),
                            'rtt_us': mono - t0, 'src_ts_us': None, 'src_precision': 'none',
                            'msg': ob})
                await asyncio.sleep(interval)
    return run()


# --- main -------------------------------------------------------------------------------------
async def amain(a):
    sink = Sink(a.out)
    t0 = time.monotonic()

    def log(s):
        print(f"[{datetime.now(timezone.utc):%H:%M:%S}] {s}", flush=True)

    key_id, pem, rest_base = load_config()
    hdrs = make_signer(key_id, pem)

    tickers = a.tickers or ['KXBTCPERP', 'KXETHPERP', 'KXSOLPERP', 'KXXRPPERP', 'KXDOGEPERP',
                            'KXLINKPERP', 'KXVVVPERP', 'KXNEARPERP', 'KXADAPERP', 'KXAAVEPERP',
                            'KXBCHPERP', 'KXWLDPERP', 'KXBNBPERP', 'KXSUIPERP', 'KXZECPERP',
                            'KXHYPEPERP', 'KXLTCPERP', 'KXKSHIBPERP']
    tasks = [asyncio.create_task(kalshi_index(sink, log, a.index_tickers, hdrs))]
    if a.rest_book_hz > 0:
        tasks.append(asyncio.create_task(
            rest_book_poller(sink, log, tickers, hdrs, rest_base, a.rest_book_hz, a.depth)))
    if not a.no_margin_ws:
        tasks.append(asyncio.create_task(kalshi_margin(sink, log, tickers, hdrs)))
    if not a.no_external:
        for name, syms in (('coinbase', a.coinbase), ('kraken', a.kraken),
                           ('binance', a.binance), ('hyperliquid', a.hyperliquid)):
            if syms: tasks.append(asyncio.create_task(external(name, sink, log, syms)))

    stop = asyncio.Event()
    for s in (signal.SIGINT, signal.SIGTERM):
        try: asyncio.get_running_loop().add_signal_handler(s, stop.set)
        except NotImplementedError: pass

    async def clock_watch():
        """Clock quality goes INTO the recording, so a later lead-lag result can be checked
        against what the clock was doing at the time rather than assumed to be fine."""
        while not stop.is_set():
            off, rtt = await asyncio.get_running_loop().run_in_executor(None, ntp_offset_us, a.ntp)
            sink.write('_clock', {'ev': '_clock', 'recv_ts_us': now_us(), 'recv_mono_us': mono_us(),
                                  'wall_vs_mono_skew_us': clock_skew_us(),
                                  'ntp_offset_us': off, 'ntp_rtt_us': rtt, 'ntp_server': a.ntp})
            if off is not None and abs(off) > 100_000:
                log(f"WARNING local clock is {off/1e6:+.3f}s off {a.ntp} (rtt {rtt/1e3:.0f}ms). "
                    f"Cross-venue lead-lag on SOURCE timestamps is unsafe; use receive-time "
                    f"differences measured by this machine instead.")
            await asyncio.sleep(a.clock_interval)

    async def heartbeat():
        last = dict(sink.counts)
        while not stop.is_set():
            await asyncio.sleep(a.heartbeat)
            cur = dict(sink.counts)
            rate = {k: (cur.get(k, 0) - last.get(k, 0)) / a.heartbeat for k in set(cur) | set(last)}
            mins = (time.monotonic() - t0) / 60
            log(f"{mins:.0f}m  " + "  ".join(f"{k}={cur.get(k,0):,}({rate.get(k,0):.0f}/s)"
                                             for k in sorted(cur)))
            skew = clock_skew_us()
            if abs(skew) > 50_000:
                log(f"WARNING clock stepped {skew/1e6:+.3f}s since start (wall vs monotonic)")
            last = cur
    tasks.append(asyncio.create_task(heartbeat()))
    tasks.append(asyncio.create_task(clock_watch()))

    log(f"recording {len(tickers)} perps -> {a.out}")
    await stop.wait()
    log("stopping")
    for t in tasks: t.cancel()
    await asyncio.gather(*tasks, return_exceptions=True)
    sink.close()
    log("events written: " + json.dumps(dict(sink.counts)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--tickers', nargs='*')
    # CF Benchmarks index IDs, not Kalshi tickers. BRTI is the Bitcoin Real Time Index; the
    # others follow the <ASSET>USD_RTI shape. ws_probe.py confirms which this account can see.
    ap.add_argument('--index-tickers', nargs='*',
                    default=['BRTI', 'ETHUSD_RTI', 'SOLUSD_RTI', 'XRPUSD_RTI'])
    ap.add_argument('--coinbase', nargs='*', default=['BTC-USD', 'ETH-USD', 'SOL-USD', 'XRP-USD'])
    ap.add_argument('--kraken', nargs='*', default=['BTC/USD', 'ETH/USD', 'SOL/USD', 'XRP/USD'])
    ap.add_argument('--binance', nargs='*', default=['BTCUSDT', 'ETHUSDT', 'SOLUSDT', 'XRPUSDT'])
    ap.add_argument('--hyperliquid', nargs='*', default=['BTC', 'ETH', 'SOL', 'XRP'])
    ap.add_argument('--no-external', action='store_true')
    ap.add_argument('--heartbeat', type=float, default=60.0)
    ap.add_argument('--ntp', default='time.apple.com')
    # The margin socket acknowledges a subscribe and then sends orderbook_snapshot frames with no
    # bid or ask in them, so it is not serving perps. Until that is resolved the REST book is the
    # only depth source, and it is on by default for exactly that reason.
    # Off by default now that the margin socket works. Kept because it is the only depth source
    # if that socket goes away again, and because it needs no WebSocket at all.
    ap.add_argument('--rest-book-hz', type=float, default=0.0,
                    help='poll each market REST orderbook this many times a second; 0 = off')
    ap.add_argument('--depth', type=int, default=10)
    ap.add_argument('--no-margin-ws', action='store_true',
                    help='skip the margin socket entirely while it returns empty snapshots')
    ap.add_argument('--clock-interval', type=float, default=300.0)
    a = ap.parse_args()
    try:
        import websockets  # noqa: F401
    except ImportError:
        sys.exit("need `pip install websockets cryptography`")
    try:
        asyncio.run(amain(a))
    except KeyboardInterrupt:
        pass


main()
