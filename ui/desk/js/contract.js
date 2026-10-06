/* ui/desk/js/contract.js
 *
 * THE BACKEND CONTRACT. This file is the single statement of what the C++ desk (web worker)
 * serves to the trading screen, and what the screen sends back. The demo venues (sim.js) emit
 * exactly these shapes, the store (store.js) consumes exactly these shapes, and the node test
 * validates every simulated event against the schemas below. If the backend and this file
 * disagree, this file wins and the backend is wrong.
 *
 * Classic script, no modules: it registers window.Desk.contract.
 *
 * ---------------------------------------------------------------------------------------------
 * CONVENTIONS (apply to every message in both directions)
 * ---------------------------------------------------------------------------------------------
 * - JSON, UTF-8. Field names are lower_snake or short lower-case keys exactly as written here.
 *   Unknown fields are an error (the validator is strict), so additions go through this file.
 * - PRICES are strings in dollars with exactly 4 decimals, as Kalshi's *_dollars fields:
 *   "0.6400". Range "0.0000".."1.0000". Every price is in YES terms: on Kalshi the backend
 *   converts NO bids into YES asks (YES ask = 1 - best NO bid) before sending.
 * - MONEY (cash, P&L, fees, budgets, per-contract edges and markouts) is a string in dollars
 *   with exactly 4 decimals, signed when it can be negative: "-3.2100". Finer amounts are
 *   rounded half away from zero to 4 decimals.
 * - SIZES (contracts) are JSON numbers with at most 2 decimals (Kalshi trades in 0.01
 *   contracts). Counts are non-negative integers.
 * - TIMES are integer milliseconds since the Unix epoch, server clock, in fields ending _ms
 *   (and in the t{} block of an order). Durations are integer ms, except ttl_s (seconds).
 * - VENUE "K" = Kalshi, "P" = Polymarket US. A market is identified by (v, tk). The screen
 *   keys it as "v|tk", e.g. "K|KXNATGAS15M-26OCT061930-30".
 * - SIDE is in YES terms: "BUY" = buy YES, "SELL" = sell YES. On Kalshi a SELL with no long
 *   YES position is a BUY NO at (1 - px); positions net to one signed number per market.
 * - TIF: "IOC" = immediate-or-cancel (never rests; px is the slippage cap, i.e. the worst
 *   acceptable price), "FOK" = fill-or-kill (all or nothing, never rests), "POST" = post-only
 *   with an exchange-side expiry (ttl_s of 5, 15, 60 or 300 seconds). There is no GTC.
 *   A POST that would cross is cancelled by the exchange (st CXL, why POST_CROSS).
 * - ORDER STATES: PEND (sent, not acknowledged), WORK (resting, nothing filled), PART (resting,
 *   partly filled), and the terminal states FILL (fully filled), CXL (cancelled; filled may be
 *   > 0, e.g. an IOC that filled 6 of 10), REJ (rejected; filled is 0), EXP (exchange expiry
 *   reached; filled may be > 0). A terminal order never changes again.
 * - Every POST must carry the headers "Content-Type: application/json" and "X-Desk: 1" and is
 *   same-origin only. Errors use one shape: {"ok":false,"err":{"code":CODE,"msg":text}}.
 *
 * ---------------------------------------------------------------------------------------------
 * THE STREAM: GET /api/stream?sid=<sid>   (text/event-stream, one per browser tab)
 * ---------------------------------------------------------------------------------------------
 * The page makes a random stream id (sid) at boot and opens the stream with it. Each SSE
 * message is:
 *     event: <type>
 *     id: <seq>
 *     data: {"type":<type>,"seq":<seq>,"ts_ms":<server send time>,"data":{...}}
 * - seq is per stream: 1 for the first message, then +1 for EVERY message of any type
 *   (hb included), so a gap is always detectable, even on a quiet book.
 * - The page resyncs (GET /api/snapshot, then applies only messages with seq > snapshot.seq)
 *   when: seq != last + 1, hb.epoch changes (backend restarted), the stream reconnects, or no
 *   hb arrives for 3 s. While resyncing every number on screen greys out (the STALE rule).
 * - Staleness is socket liveness plus sequence continuity (hb.feeds), never the time since the
 *   last trade: a quiet book is not stale.
 * - Rates: hdr at most 4/s and only on change; book coalesced to at most 20/s per ticker;
 *   pos/risk at most 1/s on mark changes (immediately on fills); hb exactly 1/s.
 * - Event types: hdr, mkt, book, ord, fill, pos, lane, risk, mo, alert, hb (shapes below).
 *   "mkt" is the market universe (listings, rolls of the 15-minute windows, settlement).
 *
 * Exported API (window.Desk.contract):
 *   .version, .types, .enums, .errors, .envelope, .events, .endpoints, .snapshot
 *   .validate(schemaNode, value) -> [error strings]   (empty array = valid)
 *   .validateEvent(envelope)     -> [error strings]
 *   .validateRequest(endpointKey, body)  e.g. ('POST /api/order', {...})
 *   .validateResponse(endpointKey, httpStatus, json)
 *   .endpointFor(method, path) -> endpoint key or null ('/api/lane/M1' -> 'POST /api/lane/{id}')
 */
(function (root) {
  'use strict';
  var Desk = root.Desk = root.Desk || {};

  // ---------------------------------------------------------------------------------------------
  // Leaf types. A schema node is one of:
  //   'type'                a leaf type below; 'type?' also allows null
  //   {field: node}         a strict object: every listed field must be present, no others;
  //                         a key ending in '?' ("ttl_s?") may be absent
  //   ['enum', a, b, ...]   one of the literal values; ['enum?', ...] also allows null
  //   ['arr', node]         an array of node
  //   ['tup', n1, n2]       a fixed-length array
  //   ['map', node]         an object with any keys, every value a node
  //   ['or', n1, n2, ...]   the first node that validates
  //   ['nul', node]         node or null
  // ---------------------------------------------------------------------------------------------
  var RE_PRICE = /^[01]\.\d{4}$/;
  var RE_MONEY = /^-?\d{1,9}\.\d{4}$/;
  var RE_CID = /^[A-Za-z0-9_-]{6,40}$/;
  var RE_TK = /^[A-Za-z0-9._-]{1,96}$/;
  var RE_ID = /^[A-Za-z0-9._:|-]{1,64}$/;
  function isInt(v) { return typeof v === 'number' && isFinite(v) && Math.floor(v) === v; }
  function twoDp(v) { return Math.abs(v * 100 - Math.round(v * 100)) < 1e-6; }

  var types = {
    price: { doc: 'YES-terms price, dollars, 4 decimals, "0.0000".."1.0000"',
             ok: function (v) { return typeof v === 'string' && RE_PRICE.test(v) && +v <= 1; } },
    money: { doc: 'dollars, 4 decimals, may be negative: "142.3100", "-0.0300"',
             ok: function (v) { return typeof v === 'string' && RE_MONEY.test(v) && v !== '-0.0000'; } },
    size: { doc: 'contracts, number >= 0 with at most 2 decimals',
            ok: function (v) { return typeof v === 'number' && isFinite(v) && v >= 0 && twoDp(v); } },
    ssize: { doc: 'signed contracts (position: + long YES, - long NO), at most 2 decimals',
             ok: function (v) { return typeof v === 'number' && isFinite(v) && twoDp(v); } },
    count: { doc: 'integer >= 0', ok: function (v) { return isInt(v) && v >= 0; } },
    int: { doc: 'integer', ok: isInt },
    ms: { doc: 'integer ms since the Unix epoch (server clock)',
          ok: function (v) { return isInt(v) && v >= 1e12 && v < 1e14; } },
    num: { doc: 'finite number', ok: function (v) { return typeof v === 'number' && isFinite(v); } },
    str: { doc: 'string', ok: function (v) { return typeof v === 'string' && v.length <= 400; } },
    bool: { doc: 'true or false', ok: function (v) { return v === true || v === false; } },
    cid: { doc: 'client order id ^[A-Za-z0-9_-]{6,40}$, unique per day',
           ok: function (v) { return typeof v === 'string' && RE_CID.test(v); } },
    tk: { doc: 'venue ticker (Kalshi ticker or Polymarket US slug)',
          ok: function (v) { return typeof v === 'string' && RE_TK.test(v); } },
    id: { doc: 'short identifier ^[A-Za-z0-9._:|-]{1,64}$',
          ok: function (v) { return typeof v === 'string' && RE_ID.test(v); } }
  };

  // ---------------------------------------------------------------------------------------------
  // Enums
  // ---------------------------------------------------------------------------------------------
  var enums = {
    VENUE: ['K', 'P'],
    SIDE: ['BUY', 'SELL'],
    TIF: ['IOC', 'FOK', 'POST'],
    TTL_S: [5, 15, 60, 300],
    ST: ['PEND', 'WORK', 'PART', 'FILL', 'CXL', 'REJ', 'EXP'],
    TERMINAL: ['FILL', 'CXL', 'REJ', 'EXP'],
    LIQ: ['M', 'T'],                       // maker / taker
    OWN: ['MAN', 'SYS'],
    POS_OWN: ['MAN', 'SYS', 'MIX'],
    RUN: ['RUN', 'STOP', 'KILLED', 'HALTED'],
    LANE_STATE: ['ON', 'OFF', 'BLOCKED', 'CAPPED', 'OVERLAY'],
    LANE_STYLE: ['REST', 'IOC', 'MIX'],
    LANE_VENUE: ['K', 'P', 'KP'],
    KIND: ['LIP15', 'WEATHER', 'GAME', 'OTHER'],
    MKT_STATUS: ['OPEN', 'CLOSED', 'SETTLED'],
    RESULT: ['YES', 'NO'],
    FEE_TYPE: ['QUAD', 'QUAD_MAKER', 'FREE', 'PM'],
    DEPTH: ['FULL', 'TOP'],
    GAME_SRC: ['LIVE', 'ESPN'],
    LVL: ['INFO', 'WARN', 'CRIT'],
    MO_H: ['1s', '10s', '60s', 'SETTLE'],
    PREFLIGHT: ['KEY', 'CASH', 'SHARD', 'CLK', 'DISK', 'FEED'],
    FEED: ['KX', 'KXP', 'PM'],             // Kalshi market data, Kalshi private (orders/fills), Polymarket
    MODE: ['DEMO', 'LIVE'],
    // why an order is in its state. '' while PEND/WORK/PART/FILL.
    WHY: ['', 'USER', 'KILL', 'STOP', 'HALT', 'PULL', 'IOC_REST', 'FOK', 'POST_CROSS', 'STP', 'TAKEOVER',
          'MARKET_CLOSED', 'EXPIRY', 'BAD_REQUEST', 'OFF_GRID', 'BAD_QTY', 'UNKNOWN_MARKET',
          'DUP_CID', 'PRICE_BAND', 'KILLED', 'STALE', 'RO', 'NO_CASH', 'SHARD_CASH', 'SYS_CEILING',
          'HALTED', 'RATE', 'EXCHANGE', 'INTERNAL']
  };
  var WHY_DOC = {
    '': 'live order, or fully filled',
    USER: 'cancelled by the operator (x, right-click, Esc, cancel endpoint)',
    KILL: 'cancelled by KILL',
    STOP: 'cancelled because Systematic was stopped',
    HALT: 'cancelled because the daily stop halted Systematic',
    PULL: 'a lane pulled its own quote (repricing, inventory, news, window end)',
    IOC_REST: 'IOC remainder killed after matching (filled may be > 0)',
    FOK: 'FOK could not fill completely; nothing filled',
    POST_CROSS: 'post-only would have crossed; cancelled by the exchange',
    STP: 'self-trade prevention cancelled this order',
    TAKEOVER: 'a manual order took this market; Systematic orders there were cancelled',
    MARKET_CLOSED: 'the market closed or settled while the order rested',
    EXPIRY: 'the exchange-side expiry was reached (st EXP)'
  };

  // ---------------------------------------------------------------------------------------------
  // Errors (HTTP responses and REJ reasons share the codes)
  // ---------------------------------------------------------------------------------------------
  var errors = {
    BAD_REQUEST: { http: 400, doc: 'malformed body or a field outside this contract' },
    OFF_GRID: { http: 400, doc: 'price is not on the market\'s price grid (mkt.grid)' },
    BAD_QTY: { http: 400, doc: 'qty <= 0, more than 2 decimals, or not whole on Polymarket' },
    UNKNOWN_MARKET: { http: 404, doc: 'no such (v, tk)' },
    NOT_FOUND: { http: 404, doc: 'no such order (cancel/amend) or lane' },
    DUP_CID: { http: 409, doc: 'cid already used today; the original order is returned in ord' },
    MARKET_CLOSED: { http: 409, doc: 'market is not OPEN' },
    PRICE_BAND: { http: 409, doc: 'crossing order more than 2 ticks through the touch; resend with override:true to send anyway' },
    KILLED: { http: 409, doc: 'KILL lockout active; POST /api/rearm first' },
    STALE: { http: 409, doc: 'fail-closed: feed gap, private socket down or reconcile pending; cancels and KILL still work' },
    RO: { http: 409, doc: 'venue is read-only (key failing)' },
    NO_CASH: { http: 409, doc: 'not enough free cash on the venue' },
    SHARD_CASH: { http: 409, doc: 'not enough free cash on the market\'s Kalshi shard' },
    SYS_CEILING: { http: 409, doc: 'Systematic order would exceed its 70% ceiling (lanes only)' },
    HALTED: { http: 409, doc: 'Systematic halted by the daily stop (start refused)' },
    PREFLIGHT: { http: 409, doc: 'start refused: a preflight check fails (see preflight in the reply)' },
    BAD_STATE: { http: 409, doc: 'the action does not apply now (e.g. rearm before rearm_ms)' },
    RATE: { http: 429, doc: 'write token bucket empty; retry after the stated ms' },
    EXCHANGE: { http: 502, doc: 'the venue rejected or did not answer; msg carries its text' },
    INTERNAL: { http: 500, doc: 'backend bug; msg carries the text' }
  };
  var ERR_CODES = Object.keys(errors);

  // ---------------------------------------------------------------------------------------------
  // Shared shapes
  // ---------------------------------------------------------------------------------------------
  var MKT_REF = { v: ['enum'].concat(enums.VENUE), tk: 'tk' };
  var LEVEL = ['tup', 'price', 'size'];
  var PRINT = { px: 'price', qty: 'size', side: ['enum'].concat(enums.SIDE), ts_ms: 'ms' };
  var RUN_OBJ = {
    state: ['enum'].concat(enums.RUN),
    since_ms: 'ms?',          // when the current state began
    rearm_ms: 'ms?',          // KILLED only: earliest time POST /api/rearm is accepted
    why: 'str'                // '' or a short code/text: 'daily stop', 'operator'
  };
  var PREFLIGHT_ROW = { id: ['enum'].concat(enums.PREFLIGHT), ok: 'bool', why: 'str' };
  var FEED_OBJ = { up: 'bool', gap: 'bool', lag_ms: 'int?' };
  var ERR_OBJ = { code: ['enum'].concat(ERR_CODES), msg: 'str' };

  var S = {};
  S.hdr = {
    mode: ['enum'].concat(enums.MODE),
    cash: {
      K: { total: 'money', free: 'money', ro: 'bool',
           shards: ['arr', { id: 'id', cash: 'money', free: 'money' }] },
      P: { total: 'money', free: 'money', ro: 'bool' }
    },
    day: { total: 'money', sys: 'money', man: 'money', lip: 'money' },
    risk: 'money',
    stop: { left: 'money', limit: 'money', hit: 'bool' },
    sys: {
      K: { used: 'money', ceiling: 'money', eff: 'money' },
      P: { used: 'money', ceiling: 'money', eff: 'money' }
    },
    cost: { used: 'money', budget: 'money' },
    run: RUN_OBJ,
    preflight: ['arr', PREFLIGHT_ROW],
    man_working: 'count',
    lat: { kx_lag_ms: 'int?', pm_lag_ms: 'int?', gw_ack_ms: 'int?', clk_off_ms: 'int?' }
  };
  S.mkt = {
    v: ['enum'].concat(enums.VENUE),
    tk: 'tk',
    ev: 'tk',                       // event ticker / event slug
    title: 'str',                   // full title
    short: 'str',                   // watchlist label, <= 24 chars: 'NATGAS 19:30 UP', 'SOUMIS@TROY SOUMIS'
    grp: 'str',                     // watchlist group row: '15M', 'WX NYC 10/06', 'NCAAF'
    kind: ['enum'].concat(enums.KIND),
    og: 'id',                       // outcome group: markets that settle on the same real-world outcome
    oc: 'str',                      // the outcome this market's YES stands for inside og
    open_ms: 'ms',
    close_ms: 'ms',
    status: ['enum'].concat(enums.MKT_STATUS),
    result: ['enum?'].concat(enums.RESULT),
    grid: ['arr', { start: 'price', end: 'price', step: 'price' }],   // Kalshi price_ranges
    fee: { type: ['enum'].concat(enums.FEE_TYPE), mult: 'num' },
    shard: 'id?',                   // Kalshi cash shard ('S0'..'S3'); null on Polymarket
    lip: 'bool'                     // pays Kalshi's Liquidity Incentive Program
  };
  S.book = {
    v: ['enum'].concat(enums.VENUE),
    tk: 'tk',
    depth: ['enum'].concat(enums.DEPTH),   // FULL for ladder subscriptions, TOP (1 level a side) for watchlist
    bids: ['arr', LEVEL],                  // best first (descending price); includes our resting size
    asks: ['arr', LEVEL],                  // best first (ascending price); includes our resting size
    trades: ['arr', PRINT],                // prints since this ticker's previous book message, oldest first
    last: ['nul', PRINT],                  // newest print of the day
    vol: 'size',                           // contracts traded today
    ref: {
      fair: 'price?',                      // model fair (null = no model; never a placeholder)
      sd: 'price?',                        // model one-sigma, probability units
      espn: 'price?',                      // ESPN win probability for this YES
      espn_ts_ms: 'ms?',                   // time of the play ESPN's number reflects (age = now - this)
      game: ['nul', { per: 'str', clock: 'str', score: 'str', src: ['enum'].concat(enums.GAME_SRC) }]
    },
    ex_ms: 'ms'                            // exchange timestamp of the newest change (Kalshi sending_ts_ms)
  };
  S.ord = {
    cid: 'cid',
    oid: 'str?',                           // venue order id once acknowledged
    v: ['enum'].concat(enums.VENUE),
    tk: 'tk',
    side: ['enum'].concat(enums.SIDE),
    px: 'price',                           // limit (IOC/FOK: the slippage cap)
    qty: 'size',                           // original quantity
    filled: 'size',
    left: 'size',                          // resting quantity; 0 once terminal
    avg: 'price?',                         // average fill price
    tif: ['enum'].concat(enums.TIF),
    exp_ms: 'ms?',                         // POST: exchange-side expiry; null for IOC/FOK
    st: ['enum'].concat(enums.ST),
    why: ['enum'].concat(enums.WHY),
    own: ['enum'].concat(enums.OWN),
    lane: 'id?',                           // SYS orders: lane id
    piq: 'size?',                          // contracts ahead of us in the queue at our price
    fee: 'money',                          // net fees so far (negative = rebate)
    touch: 'price?',                       // the touch we crossed or joined, at click/signal time
    edge: 'money?',                        // per contract vs model fair at send, + = in our favour
    t: { click: 'ms?', recv: 'ms', sent: 'ms?', ack: 'ms?', done: 'ms?' }
  };
  S.fill = {
    fid: 'id',
    cid: 'cid',
    oid: 'str?',
    v: ['enum'].concat(enums.VENUE),
    tk: 'tk',
    side: ['enum'].concat(enums.SIDE),
    px: 'price',
    qty: 'size',
    liq: ['enum'].concat(enums.LIQ),
    fee: 'money',                          // this fill's net fee (negative = Polymarket maker rebate)
    ts_ms: 'ms',
    own: ['enum'].concat(enums.OWN),
    lane: 'id?',
    mid: 'price?',                         // venue mid at the fill
    fair: 'price?',                        // model fair at the fill
    real: 'money'                          // realized P&L this fill closed, net of its fee
  };
  S.pos = {
    v: ['enum'].concat(enums.VENUE),
    tk: 'tk',
    pos: 'ssize',                          // + long YES, - long NO; 0 = flat (row kept for the day)
    avg: 'price?',                         // YES-terms average entry of the open position
    own: ['enum?'].concat(enums.POS_OWN),
    mark: 'price?',                        // mid used for unrl
    unrl: 'money',
    real: 'money',                         // today, net of fees
    fees: 'money',                         // today
    maxl: 'money',                         // worst-case loss of the open position, >= 0
    if_yes: 'money',                       // P&L of the open position if YES settles
    if_no: 'money'                         // P&L of the open position if NO settles
  };
  S.lane = {
    id: 'id',                              // 'M1'
    name: 'str',                           // 'LIP-15'
    venue: ['enum'].concat(enums.LANE_VENUE),
    state: ['enum'].concat(enums.LANE_STATE),
    on: 'bool',                            // operator toggle (POST /api/lane/{id})
    style: ['enum'].concat(enums.LANE_STYLE),
    budget: 'money',
    used: 'money',                         // reserved worst case: working orders + open positions
    open: 'money',                         // worst case of open positions alone
    wrk: 'count',                          // working orders now
    sent: 'count',                         // orders sent today
    hit: 'count',                          // orders with any fill today (Fill% = hit / sent)
    fills: 'count',
    ctrs: 'size',                          // contracts filled today
    fees: 'money',
    real: 'money',
    unrl: 'money',
    day: 'money',                          // real + unrl + lip
    lip: 'money',                          // LIP reward accrued today (estimate until paid)
    cap_left: 'money',                     // budget - used, capped by the effective ceiling
    thr: 'money?',                         // IOC lanes: net edge per contract required to take
    last_ms: 'ms?',                        // last signal
    why: 'str'                             // raw reason for the state, '' when ON
  };
  S.risk = {
    groups: ['arr', {
      id: 'id',                            // = mkt.og
      name: 'str',
      worst: 'money',                      // P&L of the worst real-world outcome (negative = loss)
      best: 'money',
      members: ['arr', MKT_REF]
    }],
    shards: ['arr', { v: ['enum'].concat(enums.VENUE), id: 'id', cash: 'money', free: 'money' }]
  };
  S.mo = {
    fid: 'id',
    h: ['enum'].concat(enums.MO_H),
    mo: 'money',                           // per contract, + = mid moved our way (SELL sign-flipped)
    mid: 'price?'                          // mid (or settlement value) used
  };
  S.alert = {
    id: 'id',
    lvl: ['enum'].concat(enums.LVL),
    src: 'str',                            // 'GW', 'KX', 'PM', 'SYS', a lane id, 'KILL'
    msg: 'str',                            // raw, terse: 'PM key 401: positions unknown'
    ts_ms: 'ms'
  };
  S.hb = {
    epoch: 'id',                           // changes when the backend restarts -> resync
    sid: 'str',
    srv_ms: 'ms',
    feeds: { KX: FEED_OBJ, KXP: FEED_OBJ, PM: FEED_OBJ }
  };

  var SUBS = { ladders: ['arr', MKT_REF], tops: ['or', ['enum', 'ALL'], ['arr', MKT_REF]] };

  // ---------------------------------------------------------------------------------------------
  // Examples (each validates against its schema; the node test checks this)
  // ---------------------------------------------------------------------------------------------
  var T0 = 1791306000000;   // 2026-10-06 19:00:00 ET
  var EX = {};
  EX.hdr = {
    mode: 'LIVE',
    cash: { K: { total: '142.3100', free: '101.1000', ro: false,
                 shards: [{ id: 'S0', cash: '41.1000', free: '30.0000' }, { id: 'S1', cash: '36.8100', free: '36.8100' },
                          { id: 'S2', cash: '33.2000', free: '20.1000' }, { id: 'S3', cash: '31.2000', free: '14.1900' }] },
            P: { total: '98.7000', free: '98.7000', ro: true } },
    day: { total: '3.2100', sys: '1.1000', man: '2.1100', lip: '0.4000' },
    risk: '18.4000',
    stop: { left: '21.7900', limit: '25.0000', hit: false },
    sys: { K: { used: '41.2000', ceiling: '99.6200', eff: '99.6200' }, P: { used: '0.0000', ceiling: '69.0900', eff: '69.0900' } },
    cost: { used: '1.1200', budget: '4.8000' },
    run: { state: 'RUN', since_ms: T0 - 8040000, rearm_ms: null, why: '' },
    preflight: [{ id: 'KEY', ok: true, why: '' }, { id: 'CASH', ok: true, why: '' }, { id: 'SHARD', ok: true, why: '' },
                { id: 'CLK', ok: true, why: '' }, { id: 'DISK', ok: true, why: '' }, { id: 'FEED', ok: true, why: '' }],
    man_working: 3,
    lat: { kx_lag_ms: 34, pm_lag_ms: 210, gw_ack_ms: 52, clk_off_ms: 80 }
  };
  EX.mkt = {
    v: 'K', tk: 'KXNATGAS15M-26OCT061930-30', ev: 'KXNATGAS15M-26OCT061930',
    title: 'Natural Gas price up in next 15 mins?', short: 'NATGAS 19:30 UP', grp: '15M',
    kind: 'LIP15', og: 'KXNATGAS15M-26OCT061930', oc: 'UP',
    open_ms: T0 + 900000, close_ms: T0 + 1800000, status: 'OPEN', result: null,
    grid: [{ start: '0.0000', end: '1.0000', step: '0.0100' }],
    fee: { type: 'FREE', mult: 1 }, shard: 'S3', lip: true
  };
  EX.book = {
    v: 'K', tk: 'KXNATGAS15M-26OCT061930-30', depth: 'FULL',
    bids: [['0.6300', 120], ['0.6200', 85.5], ['0.6100', 40]],
    asks: [['0.6400', 80], ['0.6500', 210], ['0.6700', 12]],
    trades: [{ px: '0.6400', qty: 5, side: 'BUY', ts_ms: T0 + 1200500 }],
    last: { px: '0.6400', qty: 5, side: 'BUY', ts_ms: T0 + 1200500 },
    vol: 2124.58,
    ref: { fair: '0.6600', sd: '0.0400', espn: null, espn_ts_ms: null, game: null },
    ex_ms: T0 + 1200510
  };
  EX.book_game = {
    v: 'K', tk: 'KXNCAAFGAME-26OCT06SOUMISTROY-SOUMIS', depth: 'TOP',
    bids: [['0.4100', 22]], asks: [['0.4400', 9]], trades: [], last: null, vol: 310,
    ref: { fair: '0.4600', sd: '0.0400', espn: '0.5100', espn_ts_ms: T0 + 1159000,
           game: { per: 'Q4', clock: '3:12', score: '21-24', src: 'LIVE' } },
    ex_ms: T0 + 1200000
  };
  EX.ord = {
    cid: 'Mk3f9x2-17', oid: 'b8a1c0de-0001', v: 'K', tk: 'KXNATGAS15M-26OCT061930-30', side: 'BUY',
    px: '0.6300', qty: 10, filled: 2, left: 8, avg: '0.6300', tif: 'POST', exp_ms: T0 + 1215000,
    st: 'PART', why: '', own: 'MAN', lane: null, piq: 40, fee: '0.0000', touch: '0.6300', edge: '0.0300',
    t: { click: T0 + 1199950, recv: T0 + 1199952, sent: T0 + 1199953, ack: T0 + 1200001, done: null }
  };
  EX.fill = {
    fid: 'K-F-000231', cid: 'Mk3f9x2-17', oid: 'b8a1c0de-0001', v: 'K', tk: 'KXNATGAS15M-26OCT061930-30',
    side: 'BUY', px: '0.6300', qty: 2, liq: 'M', fee: '0.0000', ts_ms: T0 + 1203000, own: 'MAN', lane: null,
    mid: '0.6350', fair: '0.6600', real: '0.0000'
  };
  EX.pos = {
    v: 'K', tk: 'KXNATGAS15M-26OCT061930-30', pos: 12, avg: '0.5800', own: 'MAN', mark: '0.6500',
    unrl: '0.8400', real: '0.0000', fees: '0.0000', maxl: '6.9600', if_yes: '5.0400', if_no: '-6.9600'
  };
  EX.lane = {
    id: 'M1', name: 'LIP-15', venue: 'K', state: 'ON', on: true, style: 'REST', budget: '40.0000',
    used: '12.4000', open: '8.2000', wrk: 4, sent: 212, hit: 31, fills: 38, ctrs: 212, fees: '0.0000',
    real: '1.2300', unrl: '0.3100', day: '3.5400', lip: '2.0000', cap_left: '27.6000', thr: null,
    last_ms: T0 + 1200000, why: ''
  };
  EX.risk = {
    groups: [{ id: 'KXNATGAS15M-26OCT061930', name: 'NATGAS 19:30', worst: '-6.9600', best: '5.0400',
               members: [{ v: 'K', tk: 'KXNATGAS15M-26OCT061930-30' }] }],
    shards: [{ v: 'K', id: 'S0', cash: '41.1000', free: '30.0000' }, { v: 'P', id: 'PM', cash: '98.7000', free: '98.7000' }]
  };
  EX.mo = { fid: 'K-F-000231', h: '10s', mo: '0.0050', mid: '0.6350' };
  EX.alert = { id: 'a-912', lvl: 'WARN', src: 'PM', msg: 'PM key 401: positions unknown', ts_ms: T0 + 1200000 };
  EX.hb = { epoch: 'e-1791300000', sid: 'k3f9x2ab', srv_ms: T0 + 1200000,
            feeds: { KX: { up: true, gap: false, lag_ms: 34 }, KXP: { up: true, gap: false, lag_ms: 21 },
                     PM: { up: true, gap: false, lag_ms: 210 } } };

  // ---------------------------------------------------------------------------------------------
  // Events
  // ---------------------------------------------------------------------------------------------
  var events = {
    hdr: { doc: 'Header numbers. Full replacement. On change, at most 4/s.', schema: S.hdr, example: EX.hdr },
    mkt: { doc: 'One market\'s listing. Sent when a market is listed (15-minute roll, new game, new day), when its status/result changes, and for every market in the snapshot. Full replacement keyed by (v, tk).',
           schema: S.mkt, example: EX.mkt },
    book: { doc: 'One market\'s book in YES terms plus its live reference values. Full replacement of the subscribed depth (FULL: every level the backend holds, up to 100 a side; TOP: best level a side). Coalesced to at most 20/s per ticker. After POST /api/subscribe the backend sends one book for every newly subscribed market at once.',
            schema: S.book, example: EX.book },
    ord: { doc: 'One order\'s full current record, on every change (sent, ack, fill, cancel, expiry, reject), for MAN and SYS orders alike. Keyed by cid. Arrives on the stream even when the HTTP reply carried the same state.',
           schema: S.ord, example: EX.ord },
    fill: { doc: 'One execution (Kalshi private fill channel). Keyed by fid. The ord event with the new filled/avg comes first.',
            schema: S.fill, example: EX.fill },
    pos: { doc: 'One market\'s position, full replacement keyed by (v, tk). Immediately after fills; at most 1/s on mark changes. A flat position (pos 0) stays for the day with its real.',
           schema: S.pos, example: EX.pos },
    lane: { doc: 'One Systematic lane\'s row, full replacement keyed by id. On change, at most 2/s.', schema: S.lane, example: EX.lane },
    risk: { doc: 'Risk book: outcome groups (worst case across the group\'s real-world outcomes) and cash per shard. Full replacement, at most 1/s.',
            schema: S.risk, example: EX.risk },
    mo: { doc: 'A fill\'s markout matured at horizon h (1 s, 10 s, 60 s after the fill, or at settlement).', schema: S.mo, example: EX.mo },
    alert: { doc: 'One log line. The status bar shows the newest; the LOG drawer lists them.', schema: S.alert, example: EX.alert },
    hb: { doc: 'Heartbeat, exactly 1/s, carries the feed liveness the STALE rule uses.', schema: S.hb, example: EX.hb }
  };
  var envelope = {
    doc: 'SSE data line. SSE "event:" = type and "id:" = seq as well.',
    schema: { type: ['enum'].concat(Object.keys(events)), seq: 'count', ts_ms: 'ms', data: 'any' },
    example: { type: 'mo', seq: 1042, ts_ms: T0 + 1213000, data: EX.mo }
  };

  // ---------------------------------------------------------------------------------------------
  // Snapshot
  // ---------------------------------------------------------------------------------------------
  var SNAP = {
    epoch: 'id',
    sid: 'str',
    seq: 'count',          // last stream seq this snapshot includes; apply stream messages with seq > this
    srv_ms: 'ms',
    hdr: S.hdr,
    markets: ['arr', S.mkt],          // every OPEN market, plus those that closed or settled in the last 30 min
    books: ['arr', S.book],           // the stream's current subscriptions (FULL and TOP)
    orders: ['arr', S.ord],           // every non-terminal order, plus terminal ones from the last 2 min
    fills: ['arr', S.fill],           // today's fills, oldest first
    mos: ['arr', S.mo],               // markouts matured for those fills
    pos: ['arr', S.pos],
    lanes: ['arr', S.lane],
    risk: S.risk,
    alerts: ['arr', S.alert],         // the last 200, oldest first
    feeds: S.hb.feeds,
    subs: SUBS
  };

  // ---------------------------------------------------------------------------------------------
  // Endpoints
  // ---------------------------------------------------------------------------------------------
  var OK_ORD = { ok: ['enum', true], ord: S.ord };
  var ERR_RESP = { ok: ['enum', false], err: ERR_OBJ, 'ord?': ['nul', S.ord], 'preflight?': ['arr', PREFLIGHT_ROW], 'retry_ms?': 'count' };
  var CANCEL_RESP = { ok: ['enum', true], n: 'count', cids: ['arr', 'cid'] };
  var RUN_RESP = { ok: ['enum', true], run: RUN_OBJ, n: 'count' };

  var endpoints = {
    'GET /api/stream': {
      doc: 'Server-sent events, one per browser tab. Query sid=<random 8+ chars>. The first message is an hb. Messages: see events. On reconnect EventSource sends Last-Event-ID; the backend may resume after it or start a new seq at 1 with a new hb, and the page resyncs either way.',
      query: { sid: 'str' }
    },
    'GET /api/snapshot': {
      doc: 'Everything the screen shows, taken atomically against the sid\'s stream counter. Query sid=<sid>.',
      query: { sid: 'str' },
      response: SNAP
    },
    'GET /api/health': {
      doc: 'Cheap liveness probe; the page uses it to choose live vs demo at boot and every 5 s while the stream is down.',
      response: { ok: ['enum', true], epoch: 'id', srv_ms: 'ms' },
      example: { response: { ok: true, epoch: 'e-1791300000', srv_ms: T0 } }
    },
    'POST /api/subscribe': {
      doc: 'Names the books the sid\'s stream carries: up to 4 ladders at FULL depth and the watchlist at TOP ("ALL" = every OPEN market). Replaces the previous subscription. The backend then pushes one book for every newly subscribed market.',
      request: { sid: 'str', ladders: ['arr', MKT_REF], tops: ['or', ['enum', 'ALL'], ['arr', MKT_REF]] },
      response: { ok: ['enum', true], subs: SUBS },
      example: { request: { sid: 'k3f9x2ab', ladders: [{ v: 'K', tk: 'KXNATGAS15M-26OCT061930-30' }], tops: 'ALL' },
                 response: { ok: true, subs: { ladders: [{ v: 'K', tk: 'KXNATGAS15M-26OCT061930-30' }], tops: 'ALL' } } }
    },
    'POST /api/order': {
      doc: 'Place one order. The reply comes after the venue\'s acknowledgement and carries the order\'s state then (WORK, PART, FILL or CXL; IOC/FOK fills are in it). A refusal returns the error and, when the cid was valid, the REJ order. The gateway checks risk in memory: no pre-check request is ever needed. The price guard refuses a crossing order more than 2 ticks through the touch unless override is true. A manual order in a market Systematic is quoting cancels those Systematic orders (why TAKEOVER) and keeps the market manual until POST /api/release.',
      request: {
        cid: 'cid', v: ['enum'].concat(enums.VENUE), tk: 'tk', side: ['enum'].concat(enums.SIDE),
        px: 'price', qty: 'size', tif: ['enum'].concat(enums.TIF),
        ttl_s: ['enum?'].concat(enums.TTL_S),     // POST: required; IOC/FOK: null
        override: 'bool',                         // true = skip the 2-tick price guard
        touch: 'price?',                          // the touch shown at click (logging, slippage)
        t_click_ms: 'ms?'                         // browser click time, epoch ms (performance.timeOrigin + event.timeStamp)
      },
      response: OK_ORD,
      errors: ['BAD_REQUEST', 'OFF_GRID', 'BAD_QTY', 'UNKNOWN_MARKET', 'DUP_CID', 'MARKET_CLOSED', 'PRICE_BAND',
               'KILLED', 'STALE', 'RO', 'NO_CASH', 'SHARD_CASH', 'RATE', 'EXCHANGE', 'INTERNAL'],
      example: {
        request: { cid: 'Mk3f9x2-17', v: 'K', tk: 'KXNATGAS15M-26OCT061930-30', side: 'BUY', px: '0.6300', qty: 10,
                   tif: 'POST', ttl_s: 15, override: false, touch: '0.6300', t_click_ms: T0 + 1199950 },
        response: { ok: true, ord: EX.ord },
        error: { ok: false, err: { code: 'PRICE_BAND', msg: 'BUY 0.6900 is 5 ticks through ask 0.6400 (max 2)' },
                 ord: (function () { var o = JSON.parse(JSON.stringify(EX.ord)); o.st = 'REJ'; o.why = 'PRICE_BAND'; o.px = '0.6900';
                   o.tif = 'IOC'; o.exp_ms = null; o.filled = 0; o.left = 0; o.avg = null; o.oid = null; o.piq = null;
                   o.t.sent = null; o.t.ack = null; o.t.done = T0 + 1199953; return o; })() }
      }
    },
    'POST /api/cancel': {
      doc: 'Cancel by cid list, or every order matching a market filter (side and px narrow it to one ladder level: right-click on WRK). n counts the orders asked to cancel; each then gets an ord event with st CXL (or FILL/EXP if that won the race).',
      request: ['or',
        { cids: ['arr', 'cid'] },
        { v: ['enum'].concat(enums.VENUE), tk: 'tk', own: ['enum', 'MAN', 'SYS', 'ANY'],
          side: ['enum?'].concat(enums.SIDE), px: 'price?' }],
      response: CANCEL_RESP,
      errors: ['BAD_REQUEST', 'NOT_FOUND', 'EXCHANGE'],
      example: { request: { v: 'K', tk: 'KXNATGAS15M-26OCT061930-30', own: 'MAN', side: 'BUY', px: '0.6300' },
                 response: { ok: true, n: 1, cids: ['Mk3f9x2-17'] } }
    },
    'POST /api/cancel_all': {
      doc: 'Cancel every working order of an owner, on one venue or both (v null). Shift+Esc sends {own:"MAN", v:null}.',
      request: { own: ['enum', 'MAN', 'SYS', 'ANY'], v: ['enum?'].concat(enums.VENUE) },
      response: CANCEL_RESP,
      errors: ['BAD_REQUEST', 'EXCHANGE'],
      example: { request: { own: 'MAN', v: null }, response: { ok: true, n: 3, cids: ['Mk3f9x2-17', 'Mk3f9x2-18', 'Mk3f9x2-19'] } }
    },
    'POST /api/amend': {
      doc: 'Amend a resting (WORK/PART) order; BAD_STATE when it is PEND or terminal. A price change or a size increase loses queue place; a size decrease or a new ttl_s only (expiry refresh: the new expiry counts from now) keeps it. Fields left null are unchanged; qty is the new total quantity. A new price that would cross cancels the order (CXL POST_CROSS). The reply carries the order after the venue\'s acknowledgement.',
      request: { cid: 'cid', px: 'price?', qty: 'size?', ttl_s: ['enum?'].concat(enums.TTL_S) },
      response: OK_ORD,
      errors: ['BAD_REQUEST', 'NOT_FOUND', 'BAD_STATE', 'OFF_GRID', 'BAD_QTY', 'PRICE_BAND', 'KILLED', 'STALE', 'EXCHANGE'],
      example: { request: { cid: 'Mk3f9x2-17', px: '0.6200', qty: null, ttl_s: null }, response: { ok: true, ord: EX.ord } }
    },
    'POST /api/lane/{id}': {
      doc: 'Turn one lane on or off. Off cancels that lane\'s working orders.',
      request: { on: 'bool' },
      response: { ok: ['enum', true], lane: S.lane },
      errors: ['NOT_FOUND'],
      example: { request: { on: false }, response: { ok: true, lane: EX.lane } }
    },
    'POST /api/start': {
      doc: 'Start Systematic. Refused with PREFLIGHT (and the preflight rows) while any check fails, KILLED during the lockout, HALTED after the daily stop.',
      request: {},
      response: RUN_RESP,
      errors: ['PREFLIGHT', 'KILLED', 'HALTED', 'BAD_STATE'],
      example: { request: {}, response: { ok: true, run: { state: 'RUN', since_ms: T0, rearm_ms: null, why: '' }, n: 0 } }
    },
    'POST /api/stop': {
      doc: 'Stop Systematic: no new orders, its working orders cancelled (n). Manual untouched.',
      request: {},
      response: RUN_RESP,
      errors: [],
      example: { request: {}, response: { ok: true, run: { state: 'STOP', since_ms: T0, rearm_ms: null, why: 'operator' }, n: 4 } }
    },
    'POST /api/kill': {
      doc: 'KILL: cancel every order on both venues (cancel-all, never behind a lock), stop Systematic, refuse new orders until rearm_ms (60 s) and a POST /api/rearm. Idempotent.',
      request: {},
      response: RUN_RESP,
      errors: [],
      example: { request: {}, response: { ok: true, run: { state: 'KILLED', since_ms: T0, rearm_ms: T0 + 60000, why: 'operator' }, n: 7 } }
    },
    'POST /api/rearm': {
      doc: 'Leave the KILLED state after rearm_ms (to STOP; Systematic needs a Start). Refused with BAD_STATE before rearm_ms or when not KILLED.',
      request: {},
      response: RUN_RESP,
      errors: ['BAD_STATE'],
      example: { request: {}, response: { ok: true, run: { state: 'STOP', since_ms: T0 + 61000, rearm_ms: null, why: '' }, n: 0 } }
    },
    'POST /api/release': {
      doc: 'Hand a market the operator took over back to Systematic.',
      request: { v: ['enum'].concat(enums.VENUE), tk: 'tk' },
      response: { ok: ['enum', true] },
      errors: ['UNKNOWN_MARKET'],
      example: { request: { v: 'K', tk: 'KXNATGAS15M-26OCT061930-30' }, response: { ok: true } }
    }
  };
  // Every endpoint shares the error shape.
  Object.keys(endpoints).forEach(function (k) { if (endpoints[k].request !== undefined || k === 'GET /api/snapshot') endpoints[k].error = ERR_RESP; });

  // ---------------------------------------------------------------------------------------------
  // Validator
  // ---------------------------------------------------------------------------------------------
  function validate(node, value, path, errs) {
    errs = errs || [];
    path = path || '$';
    if (node === 'any') return errs;
    if (typeof node === 'string') {
      var nullable = node.charAt(node.length - 1) === '?';
      var tname = nullable ? node.slice(0, -1) : node;
      if (value === null && nullable) return errs;
      var t = types[tname];
      if (!t) { errs.push(path + ': schema bug, unknown type ' + tname); return errs; }
      if (!t.ok(value)) errs.push(path + ': expected ' + tname + (nullable ? '|null' : '') + ', got ' + short(value));
      return errs;
    }
    if (Array.isArray(node)) {
      var op = node[0];
      if (op === 'enum' || op === 'enum?') {
        if (op === 'enum?' && value === null) return errs;
        if (node.indexOf(value, 1) < 1) errs.push(path + ': expected one of ' + JSON.stringify(node.slice(1)) + ', got ' + short(value));
        return errs;
      }
      if (op === 'nul') { if (value === null) return errs; return validate(node[1], value, path, errs); }
      if (op === 'arr') {
        if (!Array.isArray(value)) { errs.push(path + ': expected array, got ' + short(value)); return errs; }
        for (var i = 0; i < value.length; i++) validate(node[1], value[i], path + '[' + i + ']', errs);
        return errs;
      }
      if (op === 'tup') {
        if (!Array.isArray(value) || value.length !== node.length - 1) { errs.push(path + ': expected ' + (node.length - 1) + '-tuple, got ' + short(value)); return errs; }
        for (var j = 1; j < node.length; j++) validate(node[j], value[j - 1], path + '[' + (j - 1) + ']', errs);
        return errs;
      }
      if (op === 'map') {
        if (!isObj(value)) { errs.push(path + ': expected object, got ' + short(value)); return errs; }
        Object.keys(value).forEach(function (k) { validate(node[1], value[k], path + '.' + k, errs); });
        return errs;
      }
      if (op === 'or') {
        var best = null;
        for (var a = 1; a < node.length; a++) {
          var e = validate(node[a], value, path, []);
          if (!e.length) return errs;
          if (!best || e.length < best.length) best = e;
        }
        Array.prototype.push.apply(errs, best);
        return errs;
      }
      errs.push(path + ': schema bug, unknown op ' + op);
      return errs;
    }
    if (isObj(node)) {
      if (!isObj(value)) { errs.push(path + ': expected object, got ' + short(value)); return errs; }
      var seen = {};
      Object.keys(node).forEach(function (rawk) {
        var opt = rawk.charAt(rawk.length - 1) === '?';
        var k = opt ? rawk.slice(0, -1) : rawk;
        seen[k] = true;
        if (!Object.prototype.hasOwnProperty.call(value, k)) {
          if (!opt) errs.push(path + '.' + k + ': missing');
          return;
        }
        validate(node[rawk], value[k], path + '.' + k, errs);
      });
      Object.keys(value).forEach(function (k) { if (!seen[k]) errs.push(path + '.' + k + ': not in the contract'); });
      return errs;
    }
    errs.push(path + ': schema bug, bad node');
    return errs;
  }
  function isObj(v) { return v !== null && typeof v === 'object' && !Array.isArray(v); }
  function short(v) { var s; try { s = JSON.stringify(v); } catch (e) { s = String(v); } return s === undefined ? 'undefined' : (s.length > 60 ? s.slice(0, 57) + '...' : s); }

  function validateEvent(env) {
    var errs = validate(envelope.schema, env, '$', []);
    if (errs.length) return errs;
    if (env.seq < 1) errs.push('$.seq: must be >= 1');
    return validate(events[env.type].schema, env.data, '$.data', errs);
  }
  function endpointFor(method, path) {
    var p = String(path).split('?')[0];
    if (/^\/api\/lane\/[A-Za-z0-9_-]+$/.test(p)) p = '/api/lane/{id}';
    var k = String(method).toUpperCase() + ' ' + p;
    return endpoints[k] ? k : null;
  }
  function validateRequest(key, body) {
    var ep = endpoints[key];
    if (!ep || ep.request === undefined) return ['no request schema for ' + key];
    return validate(ep.request, body, '$', []);
  }
  function validateResponse(key, status, json) {
    var ep = endpoints[key];
    if (!ep) return ['unknown endpoint ' + key];
    if (status >= 200 && status < 300) return validate(ep.response, json, '$', []);
    var errs = validate(ERR_RESP, json, '$', []);
    if (!errs.length && errors[json.err.code].http !== status)
      errs.push('$: http ' + status + ' does not match code ' + json.err.code + ' (' + errors[json.err.code].http + ')');
    return errs;
  }

  Desk.contract = {
    version: '1.0.0',
    types: types,
    enums: enums,
    why: WHY_DOC,
    errors: errors,
    envelope: envelope,
    events: events,
    endpoints: endpoints,
    snapshot: SNAP,
    schemas: S,
    examples: EX,
    validate: function (node, value) { return validate(node, value, '$', []); },
    validateEvent: validateEvent,
    validateRequest: validateRequest,
    validateResponse: validateResponse,
    endpointFor: endpointFor,
    terminal: function (st) { return st === 'FILL' || st === 'CXL' || st === 'REJ' || st === 'EXP'; }
  };
})(typeof window !== 'undefined' ? window : globalThis);
