// The v3 trading screen's foundation (ui/desk/js), checked without a browser or any package:
// contract.js, core.js, store.js and sim.js load into a node vm with minimal stubs.
//   - formatting: prices, sizes, money, P&L with U+2212, cents, times; grids; fees by formula
//   - safe storage survives a throwing localStorage; keys: combos, typing guard, HOTKEYS gate
//   - the store applies every event type, refuses contract violations, never resurrects a
//     terminal order, and resyncs from a snapshot on a sequence gap, an epoch change or silence
//   - the demo venues' matching rules: IOC never rests, post-only never crosses, expiry, queue
//     position, self-trade prevention, kill lockout, price band, fees and cash by formula
//   - every event and HTTP reply the sim produces validates against Desk.contract
//   node tests/test_ui_desk_v3.cjs
'use strict';
const fs = require('fs'), path = require('path'), vm = require('vm');
const JS = path.join(__dirname, '..', 'ui', 'desk', 'js');

let checks = 0, fails = 0;
function check(cond, what, detail) {
  checks++;
  if (!cond) { fails++; console.log('FAIL: ' + what + (detail !== undefined ? '  ' + (typeof detail === 'string' ? detail : JSON.stringify(detail)) : '')); }
}
function eq(a, b, what) { check(a === b, what, { got: a, want: b }); }

function makeCtx(opts) {
  opts = opts || {};
  const mem = {};
  const storage = opts.throwingStorage
    ? { getItem() { throw new Error('SecurityError'); }, setItem() { throw new Error('SecurityError'); } }
    : { getItem(k) { return k in mem ? mem[k] : null; }, setItem(k, v) { mem[k] = String(v); } };
  const ctx = { console, setTimeout, clearTimeout, setInterval, clearInterval, performance, localStorage: storage };
  ctx.window = ctx;
  vm.createContext(ctx);
  for (const f of ['contract.js', 'core.js', 'store.js', 'sim.js'])
    vm.runInContext(fs.readFileSync(path.join(JS, f), 'utf8'), ctx, { filename: f });
  ctx.Desk.render.scheduled = true;      // no animation frames in node
  return ctx;
}
const flushMicro = () => new Promise(r => setImmediate(r));

(async () => {
  const ctx = makeCtx();
  const D = ctx.Desk, C = D.contract, F = D.fmt, N = D.num, G = D.grid, M = '−';

  // ---- 1. formatting ---------------------------------------------------------------------
  eq(N.px('0.6400'), 6400, 'num.px parses a 4-decimal price');
  eq(N.pxStr(6400), '0.6400', 'num.pxStr writes 4 decimals');
  eq(N.money('-3.2100'), -32100, 'num.money parses a negative amount');
  eq(N.moneyStr(-32100), '-3.2100', 'num.moneyStr signs a negative amount');
  eq(N.moneyStr(-0.4), '0.0000', 'num.moneyStr never writes -0.0000');
  eq(N.moneyStr(12.5), '0.0013', 'num.moneyStr rounds half away from zero');
  eq(F.px(6400), '0.64', 'fmt.px whole cents: 2 decimals');
  eq(F.px(6450), '0.645', 'fmt.px tenth of a cent: 3 decimals');
  eq(F.px(6455), '0.6455', 'fmt.px finer: 4 decimals');
  eq(F.px(null), '', 'fmt.px of null is empty');
  eq(F.pnl(32100), '+3.21', 'fmt.pnl shows an explicit +');
  eq(F.pnl(-32100), M + '3.21', 'fmt.pnl uses U+2212 for negatives');
  eq(F.pnl(0), '0.00', 'fmt.pnl zero has no sign');
  eq(F.pnl(-40), '0.00', 'fmt.pnl never shows minus zero');
  eq(F.money(1423100), '142.31', 'fmt.money two decimals, no $');
  eq(F.money(-500), M + '0.05', 'fmt.money negative uses U+2212');
  eq(F.money(412000, 1), '41.2', 'fmt.money with 1 decimal');
  eq(F.cents(120), '+1.2', 'fmt.cents per contract, signed');
  eq(F.cents(-50), M + '0.5', 'fmt.cents negative uses U+2212');
  eq(F.size(10), '10', 'fmt.size whole');
  eq(F.size(10.5), '10.5', 'fmt.size half');
  eq(F.size(0.25), '0.25', 'fmt.size hundredths');
  eq(F.ssize(12), '+12', 'fmt.ssize long');
  eq(F.ssize(-3), M + '3', 'fmt.ssize short uses U+2212');
  eq(F.dur(2 * 3600e3 + 14 * 60e3 + 5000), '2h14m', 'fmt.dur hours');
  eq(F.dur(63000), '1m03s', 'fmt.dur minutes');
  eq(F.dur(12000), '12s', 'fmt.dur seconds');
  eq(F.secs(11200), '12s', 'fmt.secs rounds a countdown up');
  eq(F.secs(-500), '0s', 'fmt.secs never negative');
  eq(F.mmss(58000), '0:58', 'fmt.mmss kill countdown');
  eq(F.age(41400), M + '41s', 'fmt.age is a signed age with U+2212');
  eq(F.left(14 * 60000 + 5), '14m', 'fmt.left minutes to close');
  eq(F.left(3 * 3600e3 + 5 * 60e3), '3h05m', 'fmt.left hours to close');
  const T = Date.UTC(2026, 9, 6, 23, 7, 13, 456), dt = new Date(T), p2 = n => (n < 10 ? '0' : '') + n;
  eq(F.time(T), p2(dt.getHours()) + ':' + p2(dt.getMinutes()) + ':' + p2(dt.getSeconds()), 'fmt.time local HH:MM:SS');
  eq(F.timeT(T), F.time(T) + '.4', 'fmt.timeT adds tenths');
  check(!/-/.test(F.pnl(-1) + F.cents(-1) + F.money(-1) + F.ssize(-1)), 'no ASCII hyphen-minus in displayed negatives');

  // grids
  const lin = G.make([{ start: '0.0000', end: '1.0000', step: '0.0100' }]);
  const tap = G.make([{ start: '0.0000', end: '0.1000', step: '0.0010' }, { start: '0.1000', end: '0.9000', step: '0.0100' }, { start: '0.9000', end: '1.0000', step: '0.0010' }]);
  eq(lin.levels.length, 99, 'linear cent grid has 99 prices (0.01..0.99)');
  eq(tap.levels.length, 100 + 80 + 99, 'tapered grid: 0.001..0.100, 0.11..0.90, 0.901..0.999');
  eq(tap.dp, 3, 'tapered grid displays 3 decimals');
  eq(G.snap(lin, 6450, -1), 6400, 'snap down to the grid');
  eq(G.snap(lin, 6450, 1), 6500, 'snap up to the grid');
  eq(G.step(lin, 6400, 2), 6600, 'two ticks up on a cent grid');
  eq(G.step(tap, 9000, 1), 9010, 'one tick up past 0.90 on a tapered grid is 0.1c');
  eq(G.step(tap, 1000, 1), 1100, 'one tick up from 0.10 on a tapered grid is 1c');
  eq(G.ticks(lin, 6400, 6700), 3, 'ticks between two prices');
  check(G.onGrid(tap, 950) && !G.onGrid(lin, 950), 'onGrid follows the market grid');

  // fees by formula (u = $0.0001)
  const QUAD = { type: 'QUAD', mult: 1 }, QM = { type: 'QUAD_MAKER', mult: 1 }, FREE = { type: 'FREE', mult: 1 };
  eq(D.fees.estimate('K', QUAD, 'BUY', 5000, 10, 'T'), 1800, 'Kalshi taker 10 @ 0.50 = ceil(0.175) = 0.18');
  eq(D.fees.estimate('K', QUAD, 'BUY', 5000, 10, 'M'), 0, 'Kalshi maker on a QUAD series is free');
  eq(D.fees.estimate('K', QM, 'BUY', 5000, 10, 'M'), 500, 'Kalshi maker on QUAD_MAKER 10 @ 0.50 = ceil(0.04375) = 0.05');
  eq(D.fees.estimate('K', FREE, 'BUY', 5000, 10, 'T'), 0, 'fee-free series charge nothing');
  eq(D.fees.estimate('K', QUAD, 'SELL', 3000, 10, 'T'), 1500, 'Kalshi taker is symmetric: 10 @ 0.30 = ceil(0.147) = 0.15');
  eq(D.fees.estimate('P', null, 'BUY', 5000, 10, 'T'), 1700, 'Polymarket taker 10 @ 0.50 = round(0.17375) = 0.17');
  eq(D.fees.estimate('P', null, 'BUY', 5000, 10, 'M'), -300, 'Polymarket maker rebate 10 @ 0.50 = -0.03');
  eq(D.fees.estimate('P', null, 'BUY', 2500, 2, 'T'), 300, 'Polymarket taker 2 @ 0.25 = round(0.026) = 0.03');
  eq(D.fees.roundHE(2.5), 2, 'banker\'s rounding: 2.5 -> 2');
  eq(D.fees.roundHE(3.5), 4, 'banker\'s rounding: 3.5 -> 4');
  { const acc = D.fees.newAcc(); eq(D.fees.kalshiFill(acc, -550, 550, 1, 'T', QUAD), 50, 'Kalshi fee-rounding doc example: buy 1 @ 0.055 nets 0.005'); }
  { const acc = D.fees.newAcc(); const a = D.fees.pmFill(acc, 5000, 5, 'T'), b = D.fees.pmFill(acc, 5000, 5, 'T');
    check(a + b <= 1700, 'Polymarket taker fills never exceed the rounded cumulative fee', [a, b]); }

  // ---- 2. safe storage, keys, meters ------------------------------------------------------
  {
    const c2 = makeCtx({ throwingStorage: true });
    let threw = false;
    try { c2.Desk.storage.set('tab', 'systematic'); } catch (e) { threw = true; }
    check(!threw, 'storage.set never throws when localStorage throws');
    eq(c2.Desk.storage.get('tab'), 'systematic', 'storage falls back to memory');
    eq(c2.Desk.storage.getJSON('missing', 7), 7, 'storage.getJSON default');
  }
  {
    const K = D.keys;
    eq(K.combo({ code: 'KeyB', key: '∫', altKey: true }), 'Alt+b', 'Option+B on a Mac matches Alt+b by physical key');
    eq(K.combo({ code: 'KeyB', key: 'B', shiftKey: true }), 'Shift+b', 'Shift+B');
    eq(K.combo({ code: 'Slash', key: '?', shiftKey: true }), '?', '? is matched by character');
    eq(K.combo({ code: 'Backquote', key: '`' }), '`', 'backquote');
    eq(K.combo({ code: 'Escape', key: 'Escape', shiftKey: true }), 'Shift+Escape', 'Shift+Esc');
    eq(K.combo({ code: 'F1', key: 'F1' }), 'F1', 'F1');
    eq(K.combo({ code: 'Space', key: ' ' }), 'Space', 'Space');
    eq(K.norm('B'), 'Shift+b', 'norm: capital letter means Shift');
    eq(K.norm('Shift++'), 'Shift++', 'norm: plus key');
    D.render.tab = 'manual';
    let fired = [];
    K.bind({ key: 'q', scope: 'global', desc: 't', fn: () => fired.push('q') });
    K.bind({ key: 'b', scope: 'manual', order: true, desc: 'buy', fn: () => fired.push('b') });
    K.bind({ key: 'Enter', scope: 'ticket', typing: true, desc: 'send', fn: () => fired.push('enter') });
    const ev = (code, key, extra) => Object.assign({ code, key, target: { tagName: 'DIV' }, preventDefault() {} }, extra || {});
    K.handle(ev('KeyQ', 'q'));
    K.handle(ev('KeyQ', 'q', { target: { tagName: 'INPUT', blur() {} } }));
    eq(fired.join(','), 'q', 'typing guard: a key typed into a field is not a command');
    fired = [];
    K.handle(ev('KeyB', 'b'));
    eq(fired.length, 0, 'order keys do nothing while HOTKEYS are OFF (the default)');
    K.setHK(true); K.handle(ev('KeyB', 'b'));
    eq(fired.join(','), 'b', 'order keys work with HOTKEYS ON in their tab');
    D.render.tab = 'systematic'; fired = []; K.handle(ev('KeyB', 'b'));
    eq(fired.length, 0, 'a tab-scoped key is inert on the other tab');
    K.setHK(false);
    fired = []; K.handle(ev('KeyQ', 'q', { metaKey: true }));
    eq(fired.length, 0, 'Cmd combinations are left to the browser');
    check(K.sheetRows().some(r => r.key === 'b' && r.order), 'the key sheet lists bindings and marks order keys');
    const r = new D.Roll(5); [5, 1, 3, 9, 7, 2].forEach(x => r.push(x));
    eq(r.p50(), 3, 'rolling p50 over the last N');
  }

  // ---- 3. contract self-consistency --------------------------------------------------------
  for (const [type, e] of Object.entries(C.events)) {
    const errs = C.validate(e.schema, e.example);
    check(!errs.length, 'contract example validates: ' + type, errs);
  }
  check(!C.validate(C.events.book.schema, C.examples.book_game).length, 'contract example validates: book (game, TOP)');
  check(!C.validateEvent(C.envelope.example).length, 'contract envelope example validates');
  for (const [k, ep] of Object.entries(C.endpoints)) {
    if (!ep.example) continue;
    if (ep.example.request !== undefined) check(!C.validateRequest(k, ep.example.request).length, 'request example validates: ' + k, C.validateRequest(k, ep.example.request));
    if (ep.example.response) check(!C.validateResponse(k, 200, ep.example.response).length, 'response example validates: ' + k, C.validateResponse(k, 200, ep.example.response));
    if (ep.example.error) check(!C.validateResponse(k, 409, ep.example.error).length, 'error example validates: ' + k, C.validateResponse(k, 409, ep.example.error));
  }
  {
    const good = C.endpoints['POST /api/order'].example.request;
    check(C.validateRequest('POST /api/order', Object.assign({}, good, { px: '0.64' })).length > 0, 'a price without 4 decimals is refused');
    check(C.validateRequest('POST /api/order', Object.assign({}, good, { extra: 1 })).length > 0, 'an unknown field is refused');
    const miss = Object.assign({}, good); delete miss.qty;
    check(C.validateRequest('POST /api/order', miss).length > 0, 'a missing field is refused');
    check(C.validateRequest('POST /api/order', Object.assign({}, good, { qty: 1.234 })).length > 0, 'sizes carry at most 2 decimals');
    check(C.validateResponse('POST /api/order', 400, { ok: false, err: { code: 'PRICE_BAND', msg: 'x' } }).length > 0, 'an error code must match its HTTP status');
    eq(C.endpointFor('POST', '/api/lane/M1'), 'POST /api/lane/{id}', 'endpointFor resolves the lane path');
    eq(C.endpointFor('GET', '/api/snapshot?sid=abc'), 'GET /api/snapshot', 'endpointFor drops the query');
    for (const t of ['hdr', 'book', 'ord', 'fill', 'pos', 'lane', 'risk', 'mo', 'alert', 'hb'])
      check(!!C.events[t], 'the contract defines event type ' + t);
    for (const k of ['GET /api/stream', 'GET /api/snapshot', 'POST /api/subscribe', 'POST /api/order', 'POST /api/cancel', 'POST /api/cancel_all',
                     'POST /api/lane/{id}', 'POST /api/start', 'POST /api/stop', 'POST /api/kill'])
      check(!!C.endpoints[k], 'the contract defines ' + k);
    eq(C.enums.ST.join(','), 'PEND,WORK,PART,FILL,CXL,REJ,EXP', 'order states');
    eq(C.enums.TIF.join(','), 'IOC,FOK,POST', 'time in force');
    // the rules that tie fields together are checked, not left to prose
    const vr = b => C.validateRequest('POST /api/order', Object.assign({}, good, b));
    check(vr({ exp_ms: 1791306015000 }).length > 0, 'a POST with both ttl_s and exp_ms is refused');
    check(vr({ ttl_s: null }).length > 0, 'a POST with neither ttl_s nor exp_ms is refused');
    check(!vr({ ttl_s: null, exp_ms: 1791306015000 }).length, 'a POST with exp_ms alone is fine');
    check(vr({ tif: 'IOC' }).length > 0, 'an IOC carrying ttl_s is refused');
    check(!vr({ tif: 'IOC', ttl_s: null }).length, 'an IOC with neither is fine');
    check(vr({ reduce_only: true }).length > 0 && !vr({ tif: 'IOC', ttl_s: null, reduce_only: true }).length, 'reduce_only is IOC only');
    check(vr({ qty: 0 }).length > 0, 'qty 0 is refused');
    check(vr({ px: '0.0000' }).length > 0 && vr({ px: '1.0000' }).length > 0, 'an order price is strictly between 0 and 1');
    check(vr({ ttl_s: null, exp_ms: 1791306015500 }).length > 0, 'exp_ms is a whole second');
    check(C.enums.WHY.indexOf('LANE_OFF') >= 0 && C.enums.WHY.indexOf('FORBIDDEN') >= 0 && C.enums.WHY.indexOf('BAD_STATE') >= 0, 'WHY names a lane switched off, FORBIDDEN and BAD_STATE');
    check(Object.keys(C.endpoints).filter(k => k.indexOf('POST ') === 0).every(k => ['FORBIDDEN', 'BAD_REQUEST', 'INTERNAL'].every(c => C.endpoints[k].errors.indexOf(c) >= 0)),
      'every POST lists FORBIDDEN, BAD_REQUEST and INTERNAL');
    check(C.endpoints['POST /api/kill'].errors.indexOf('RATE') < 0 && C.endpoints['POST /api/cancel'].errors.indexOf('RATE') < 0, 'KILL and cancels are never rate-limited');
    check(C.endpoints['POST /api/amend'].errors.indexOf('PRICE_BAND') < 0, 'an amend has no price guard (a crossing amend is POST_CROSS)');
    check(C.schemas.ord.rev === 'count' && C.examples.ord.rev === 4, 'an order record carries its revision');
    check(C.schemas.hdr.man_working === undefined, 'hdr carries no field the page does not read');
  }
  // ---- 3b. grid colours: the meaning classes beat 'table.grid td' (a node check of the CSS) ----
  {
    const css = fs.readFileSync(path.join(JS, '..', 'desk.css'), 'utf8');
    const at = sel => css.indexOf(sel);
    const base = at('table.grid td {'), thin = at('table.grid td.thin');
    for (const c of ['up', 'dn', 'dim', 'bid', 'ask']) {
      const i = at('table.grid td.' + c + ' {');
      check(i > base && i < thin, 'desk.css: table.grid td.' + c + ' comes after table.grid td and before td.thin (same specificity, so order decides)', { base, i, thin });
    }
    check(at('table.grid tr.o-CXL > td') > thin && at('body.stale #tabs td') > thin, 'row states and STALE come later and still win');
  }

  if (process.env.V) console.log('section 4');
  // ---- 4. the store applies every event type -----------------------------------------------
  const st = D.store, EX = C.examples, T0 = 1791306000000;
  const KEY = 'K|KXNATGAS15M-26OCT061930-30';
  const baseSnap = () => ({
    epoch: EX.hb.epoch, sid: EX.hb.sid, seq: 100, srv_ms: T0, date: EX.hb.date, hdr: EX.hdr, markets: [EX.mkt], books: [EX.book],
    orders: [], fills: [], mos: [], pos: [], lanes: [], risk: { groups: [], shards: [] }, alerts: [], feeds: EX.hb.feeds,
    subs: { ladders: [], tops: 'ALL' }
  });
  check(!C.validate(C.snapshot, baseSnap()).length, 'test snapshot is a valid contract snapshot', C.validate(C.snapshot, baseSnap()));
  let snapCalls = 0, nextSnap = baseSnap();
  st.fetchSnapshot = () => { snapCalls++; return Promise.resolve(JSON.parse(JSON.stringify(nextSnap))); };
  st.begin();
  st.onTransport('open', false);
  check(st.stale, 'the store is stale until the first snapshot');
  st.resync('BOOT');
  await flushMicro();
  eq(st.seq, 100, 'snapshot sets the sequence');
  check(!st.resyncing && st.stale && st.staleWhy === 'SYNC', 'a snapshot ends the resync but not the stale state: it proves nothing about the stream', st.staleWhy);
  let seq = 100;
  const env = (type, data) => ({ type, seq: ++seq, ts_ms: T0 + seq, data: JSON.parse(JSON.stringify(data)) });
  const ap = (type, data) => st.apply(env(type, data));
  eq(ap('hb', EX.hb), 'ok', 'a stream hb newer than the snapshot applies');
  check(!st.stale, 'and only that ends the stale state', st.staleWhy);
  eq(ap('hdr', EX.hdr), 'ok', 'hdr applies');
  eq(st.hdr.cash.K.total, 1423100, 'hdr money becomes integers in u');
  eq(st.hdr.day.man, 21100, 'hdr day split');
  eq(st.hdr.cash.K.shards.length, 4, 'hdr shards');
  eq(ap('mkt', EX.mkt), 'ok', 'mkt applies');
  eq(st.market(KEY).g.levels.length, 99, 'mkt carries its grid');
  eq(ap('book', EX.book), 'ok', 'book applies');
  const b = st.book(KEY);
  check(b.bb === 6300 && b.ba === 6400 && b.mid === 6350 && b.bbSz === 120, 'book touch, sizes and mid', b);
  eq(b.prints.length, 1, 'book prints are kept for the LTQ column');
  eq(b.ref.fair, 6600, 'book ref fair in u');
  eq(ap('book', EX.book_game), 'ok', 'a TOP game book applies');
  eq(st.book('K|KXNCAAFGAME-26OCT06SOUMISTROY-SOUMIS').ref.game.clock, '3:12', 'game clock rides on the book');
  eq(ap('ord', EX.ord), 'ok', 'ord applies');
  const o = st.orders.get('Mk3f9x2-17');
  check(o.st === 'PART' && o.px === 6300 && o.left === 8 && o.edge === 300 && !o.terminal && o.rev === 4, 'ord fields', o);
  eq(st.working().length, 1, 'working lists non-terminal orders');
  // the HTTP reply and the stream race: rev decides, an older record is dropped
  {
    const older = JSON.parse(JSON.stringify(EX.ord)); older.st = 'WORK'; older.filled = 0; older.left = 10; older.avg = null; older.rev = 3;
    st._handlers.ord(older, null, false);
    check(st.orders.get('Mk3f9x2-17').st === 'PART' && st.orders.get('Mk3f9x2-17').left === 8, 'a reply with an older rev (WORK, left 10) does not undo the stream\'s PART (left 8)');
  }
  eq(ap('mo', { fid: 'K-F-000231', h: '1s', mo: '0.0020', mid: '0.6370' }), 'ok', 'a markout before its fill applies');
  eq(ap('fill', EX.fill), 'ok', 'fill applies');
  eq(st.fills[0].px, 6300, 'fill price in u');
  eq(st.fills[0].mo['1s'], 20, 'an early markout attaches when the fill arrives');
  eq(ap('mo', EX.mo), 'ok', 'mo applies');
  eq(st.fills[0].mo['10s'], 50, 'mo lands on its fill');
  eq(ap('fill', EX.fill), 'ok', 'a duplicate fill is accepted on the stream');
  eq(st.fills.length, 1, 'but not stored twice');
  eq(ap('pos', EX.pos), 'ok', 'pos applies');
  const P = st.position(KEY);
  check(P.pos === 12 && P.avg === 5800 && P.maxl === 69600 && P.if_no === -69600, 'pos fields', P);
  eq(ap('lane', EX.lane), 'ok', 'lane applies');
  check(st.lanes.get('M1').budget === 400000 && st.laneOrder[0] === 'M1', 'lane fields and order');
  eq(ap('risk', EX.risk), 'ok', 'risk applies');
  check(st.risk.groups[0].worst === -69600 && st.risk.groups[0].members[0] === KEY, 'risk groups reference positions by key');
  eq(ap('alert', EX.alert), 'ok', 'alert applies');
  eq(st.alerts[0].msg, 'PM key 401: positions unknown', 'the newest alert is first');
  eq(ap('hb', EX.hb), 'ok', 'hb applies');
  check(st.feeds.KX.lag_ms === 34 && !st.stale, 'hb feeds and liveness');
  eq(st.priceGuard(KEY, 'BUY', 6600), '', 'price guard: 2 ticks through the ask is allowed');
  check(/^PRICE_BAND/.test(st.priceGuard(KEY, 'BUY', 6700)), 'price guard: 3 ticks through the ask is refused');
  check(/^PRICE_BAND/.test(st.priceGuard(KEY, 'SELL', 6000)), 'price guard: 3 ticks through the bid is refused');
  eq(st.canTrade('K'), '', 'trading allowed while live');
  // terminal orders never change again
  const term = JSON.parse(JSON.stringify(EX.ord)); term.st = 'CXL'; term.why = 'USER'; term.left = 0; term.t.done = T0 + 1300000; term.rev = 5;
  ap('ord', term);
  const back = JSON.parse(JSON.stringify(EX.ord)); back.rev = 6;
  ap('ord', back);
  eq(st.orders.get('Mk3f9x2-17').st, 'CXL', 'a late non-terminal echo cannot resurrect a terminal order');
  eq(st.recent(3000).length, 1, 'recent terminal orders are listed for the 3 s display');
  // a contract violation is refused and nothing is applied
  const before = st.hdr.risk;
  const badHdr = JSON.parse(JSON.stringify(EX.hdr)); badHdr.risk = '18.4';
  eq(ap('hdr', badHdr), 'invalid', 'an event that breaks the contract is refused');
  check(st.hdr.risk === before && st.invalid === 1, 'and not applied');
  seq--;   // the refused event did not advance the stream
  // optimistic order: pending, then a reject from the HTTP reply
  const pend = st.pend({ cid: 'Mtestabc-1', v: 'K', tk: EX.mkt.tk, side: 'BUY', px: 6700, qty: 5, tif: 'IOC', ttl_s: null, touch: 6400, t_click_ms: T0 });
  check(pend.st === 'PEND' && pend.local.pend, 'pend inserts a local PEND order');
  st.reply('Mtestabc-1', 409, { ok: false, err: { code: 'PRICE_BAND', msg: 'BUY 0.6700 is 3 ticks through ask 0.6400 (max 2)' } });
  const rj = st.orders.get('Mtestabc-1');
  check(rj.st === 'REJ' && rj.why === 'PRICE_BAND' && rj.terminal && rj.local.prov, 'a pre-send refusal with no ord shows REJ with the reason, provisionally', rj);
  check(/^REJ PRICE_BAND/.test(st.alerts[0].msg), 'the reason goes to the status bar');
  {
    // the backend's own record of that cid replaces a provisional REJ (it was sent after all)
    const real = JSON.parse(JSON.stringify(EX.ord)); real.cid = 'Mtestabc-1'; real.st = 'WORK'; real.filled = 0; real.left = 5; real.qty = 5; real.avg = null; real.rev = 2;
    st._handlers.ord(real, null, false);
    check(st.orders.get('Mtestabc-1').st === 'WORK' && !st.orders.get('Mtestabc-1').terminal, 'a stream record of the cid replaces the provisional REJ');
    real.st = 'CXL'; real.why = 'USER'; real.left = 0; real.rev = 3; st._handlers.ord(real, null, false);
  }
  const calls0 = snapCalls;
  st.pend({ cid: 'Mtestabc-2', v: 'K', tk: EX.mkt.tk, side: 'BUY', px: 6300, qty: 1, tif: 'POST', ttl_s: 15, touch: 6300, t_click_ms: T0 });
  st.reply('Mtestabc-2', 0, { ok: false, err: { code: 'EXCHANGE', msg: 'desk unreachable' } });
  eq(st.orders.get('Mtestabc-2').st, 'PEND', 'no answer leaves the order PEND (state unknown), never REJ');
  check(st.resyncing && snapCalls === calls0 + 1, 'and asks for a snapshot, which decides whether it exists', { calls: snapCalls - calls0 });
  await flushMicro();
  st.pend({ cid: 'Mtestabc-3', v: 'K', tk: EX.mkt.tk, side: 'BUY', px: 6300, qty: 1, tif: 'POST', ttl_s: 15, touch: 6300, t_click_ms: T0 });
  st.reply('Mtestabc-3', 502, { ok: false, err: { code: 'EXCHANGE', msg: 'venue timeout' } });
  check(st.orders.get('Mtestabc-3') && st.orders.get('Mtestabc-3').st === 'PEND', 'a 502 EXCHANGE with no ord is not a reject: the order may rest at the venue, so it stays PEND');
  st.pend({ cid: 'Mtestabc-4', v: 'K', tk: EX.mkt.tk, side: 'BUY', px: 6300, qty: 1, tif: 'POST', ttl_s: 15, touch: 6300, t_click_ms: T0 });
  st.reply('Mtestabc-4', 500, { ok: false, err: { code: 'INTERNAL', msg: 'HTTP 500 with no JSON body' } });
  eq(st.orders.get('Mtestabc-4').st, 'PEND', 'a proxy 5xx leaves it PEND too');
  await flushMicro();
  // the snapshot does not list Mtestabc-3/4: they stay while young (in flight), then go
  seq = st.seq;
  ap('hb', EX.hb);

  if (process.env.V) console.log('section 5');
  // ---- 5. sequence gaps, epochs, silence ---------------------------------------------------
  {
    const k = st.seq;
    eq(st.apply({ type: 'hb', seq: k, ts_ms: T0, data: EX.hb }), 'dup', 'an old seq is ignored');
    nextSnap = baseSnap(); nextSnap.seq = k + 3;
    const calls = snapCalls;
    eq(st.apply({ type: 'hb', seq: k + 2, ts_ms: T0, data: EX.hb }), 'gap', 'a skipped seq is a gap');
    check(st.resyncing && st.stale && st.staleWhy === 'GAP', 'a gap greys the screen (STALE GAP)', st.staleWhy);
    eq(snapCalls, calls + 1, 'a gap requests one snapshot');
    eq(st.apply({ type: 'hb', seq: k + 3, ts_ms: T0, data: EX.hb }), 'buffered', 'messages during a resync are buffered');
    eq(st.apply({ type: 'hb', seq: k + 4, ts_ms: T0, data: EX.hb }), 'buffered', 'more buffered');
    eq(st.apply({ type: 'alert', seq: k + 5, ts_ms: T0, data: Object.assign({}, EX.alert, { id: 'a-913', msg: 'after the snapshot' }) }), 'buffered', 'and kept');
    await flushMicro();
    check(!st.resyncing && !st.stale, 'the snapshot plus a buffered hb newer than it end the resync and the stale state', st.staleWhy);
    eq(st.seq, k + 5, 'buffered messages newer than the snapshot are replayed in order');
    eq(st.alerts[0].msg, 'after the snapshot', 'the replayed message is applied');
    eq(st.gaps, 1, 'one gap counted');
    // a new epoch (backend restart) resyncs even when seq looks fine
    const restart = Object.assign({}, EX.hb, { epoch: 'e-restart' });
    nextSnap = baseSnap(); nextSnap.epoch = 'e-restart'; nextSnap.seq = 1;
    eq(st.apply({ type: 'hb', seq: 1, ts_ms: T0, data: restart }), 'epoch', 'a restarted backend (new epoch, seq from 1) forces a resync');
    await flushMicro();
    check(st.epoch === 'e-restart' && st.seq === 1 && !st.resyncing, 'resynced onto the new epoch', { epoch: st.epoch, seq: st.seq, r: st.resyncing });
    // a snapshot older than the buffered stream backs off and asks again, never a tight loop
    nextSnap = baseSnap(); nextSnap.epoch = 'e-restart'; nextSnap.seq = 1;
    const asked = snapCalls;
    eq(st.apply({ type: 'hb', seq: 3, ts_ms: T0, data: restart }), 'gap', 'gap on the new epoch');
    st.apply({ type: 'hb', seq: 4, ts_ms: T0, data: restart });
    await flushMicro(); await flushMicro();
    check(st.resyncing && snapCalls === asked + 1, 'an old snapshot does not end the resync and is not re-requested at once', { calls: snapCalls - asked });
    nextSnap = baseSnap(); nextSnap.epoch = 'e-restart'; nextSnap.seq = 3;
    await new Promise(r => setTimeout(r, 300)); await flushMicro();
    check(!st.resyncing && st.seq === 4, 'the next snapshot after the back-off heals it', { seq: st.seq, r: st.resyncing });
    // silence: no hb for 3 s is a dead stream, whatever the books are doing
    st.lastHb = Date.now() - 3500;
    st.tick(Date.now());
    check(st.stale && st.staleWhy === 'SRV DOWN', 'no hb for 3 s greys the screen (SRV DOWN)', st.staleWhy);
    eq(st.canTrade('K'), 'SRV DOWN', 'new orders disable with the reason');
    await flushMicro();
    st.lastHb = Date.now(); st.tick(Date.now());
    check(!st.stale, 'liveness returns with the heartbeat');
    // venue feed down: that venue only
    const down = JSON.parse(JSON.stringify(EX.hb)); down.epoch = 'e-restart'; down.feeds.KXP.up = false;
    st.apply({ type: 'hb', seq: st.seq + 1, ts_ms: T0, data: down });
    check(!st.stale && st.vstale.K === 'KX DOWN' && st.canTrade('K') === 'KX DOWN' && st.vstale.P === '', 'Kalshi private socket down blocks Kalshi orders only', { stale: st.staleWhy, v: st.vstale });
    eq(st.canTrade('P'), 'RO', 'a read-only venue (PM key failing) blocks its new orders');
  }

  // ---- 5b. the STALE rule fails closed; a bad snapshot is never applied -------------------------
  {
    const c5 = makeCtx(), S5 = c5.Desk.store;
    let calls5 = 0, snap5 = baseSnap();
    S5.fetchSnapshot = () => { calls5++; return Promise.resolve(JSON.parse(JSON.stringify(snap5))); };
    S5.begin(); S5.onTransport('open', false);
    await flushMicro();
    eq(calls5, 1, 'the first stream open takes the snapshot');
    let q = S5.seq;
    S5.apply({ type: 'hb', seq: ++q, ts_ms: T0, data: EX.hb });
    check(!S5.stale && S5.canTrade('K') === '', 'live after the snapshot and a fresh hb');
    // the stream goes silent while GET /api/snapshot still answers: never live again from a snapshot
    const base = Date.now();
    S5.lastHb = base - 3100;
    let allowed = 0;
    for (let i = 0; i < 40; i++) { S5.tick(base + i * 250); await flushMicro(); if (S5.canTrade('K') === '') allowed++; }
    check(allowed === 0 && calls5 === 1, 'a silent stream (snapshots still answering) blocks new orders on every tick and asks for no snapshot', { allowed, calls: calls5 - 1 });
    S5.resync('USER'); await flushMicro();
    check(S5.stale && S5.canTrade('K') !== '', 'a snapshot taken during the silence does not make it live', S5.staleWhy);
    // the socket in error: snapshots cannot make it live either
    S5.onTransport('error'); S5.lastHb = Date.now();
    S5.resync('USER'); await flushMicro();
    check(S5.stale && S5.staleWhy === 'SRV DOWN' && S5.canTrade('K') === 'SRV DOWN', 'with the EventSource in error the page stays SRV DOWN through a snapshot', S5.staleWhy);
    // it reopens: snapshot, then the first hb newer than it
    const c0 = calls5;
    S5.onTransport('open', true); await flushMicro();
    check(calls5 === c0 + 1 && S5.stale && S5.staleWhy === 'SYNC', 'a reopen resyncs and waits for a stream hb', { calls: calls5 - c0, why: S5.staleWhy });
    q = S5.seq; S5.apply({ type: 'hb', seq: ++q, ts_ms: T0, data: EX.hb });
    check(!S5.stale, 'the hb after the snapshot makes it live');
    // a snapshot that breaks the contract is refused, not applied; the store retries with back-off
    const hdr0 = S5.hdr, inv0 = S5.invalid;
    snap5 = JSON.parse(JSON.stringify(baseSnap())); delete snap5.hdr.lat; snap5.seq = 500;
    S5.resync('USER'); await flushMicro();
    check(S5.resyncing && S5.stale && S5.hdr === hdr0 && S5.seq !== 500 && S5.invalid === inv0 + 1 && /snapshot refused/.test(S5.alerts[0].msg),
      'a snapshot missing hdr.lat is refused: nothing replaced, STALE, a CRIT line', { r: S5.resyncing, why: S5.staleWhy, msg: S5.alerts[0] && S5.alerts[0].msg });
    snap5 = baseSnap(); snap5.seq = 500;
    await new Promise(r => setTimeout(r, 1150)); await flushMicro();
    check(!S5.resyncing && S5.seq === 500, 'the retry after 1 s takes the good snapshot', { r: S5.resyncing, seq: S5.seq });
    S5.apply({ type: 'hb', seq: 501, ts_ms: T0, data: EX.hb });
    // a snapshot that throws while applying is rolled back whole
    const mk0 = S5.markets, ord0 = S5.orders;
    S5.validate = false;
    snap5 = baseSnap(); snap5.seq = 600; snap5.markets = [EX.mkt, null];
    S5.resync('USER'); await flushMicro();
    check(S5.markets === mk0 && S5.orders === ord0 && S5.seq === 501 && S5.resyncing && S5.stale, 'a snapshot that throws half-way leaves the old maps in place and stays STALE', { seq: S5.seq });
    S5.validate = true;
    // settled markets are forgotten after 30 min unless a position or a ladder holds them
    const sm = JSON.parse(JSON.stringify(EX.mkt)); sm.tk = 'KXOLD-1'; sm.status = 'SETTLED'; sm.result = 'YES';
    const sm2 = JSON.parse(JSON.stringify(sm)); sm2.tk = 'KXOLD-2';
    S5._handlers.mkt(sm); S5._handlers.mkt(sm2);
    const pp = JSON.parse(JSON.stringify(EX.pos)); pp.tk = 'KXOLD-2'; pp.pos = 0; pp.avg = null; pp.mark = null; S5._handlers.pos(pp);
    const later = Date.now() + 31 * 60000;
    S5.prune(Date.now() + 60000);
    check(S5.markets.has('K|KXOLD-1'), 'a market settled a minute ago stays');
    S5.prune(later);
    check(!S5.markets.has('K|KXOLD-1') && !S5.books.has('K|KXOLD-1') && S5.markets.has('K|KXOLD-2'), 'after 30 min it goes, unless a position row holds it');
  }

  if (process.env.V) console.log('section 6');
  // ---- 6. the demo venues: matching rules ---------------------------------------------------
  let t = Date.UTC(2026, 9, 6, 23, 7, 13);
  const evs = [];
  const sim = D.sim.create({ now: () => t, emit: e => evs.push(e), seed: 7, sid: 'testsid1' });
  const replies = [];
  function run(ms) { const end = t + ms; while (t < end) { t = Math.min(end, Math.max(t + 1, Math.min(sim.nextDue(), t + 100))); sim.pump(); } }
  const HDRS = { 'Content-Type': 'application/json', 'X-Desk': sim.token };
  function call(method, p, body, ms, hdrs) {
    let out = null;
    sim.request(method, p, body, (s, j) => { out = { s, j }; replies.push([method, p, s, j]); }, hdrs === undefined ? HDRS : hdrs);
    run(ms === undefined ? 300 : ms);
    return out;
  }
  let cidN = 0;
  const order = (mk, side, pxu, qty, tif, ttl, extra) => call('POST', '/api/order', Object.assign({
    cid: 'Mtest' + (++cidN).toString().padStart(4, '0'), v: mk.v, tk: mk.tk, side, px: N.pxStr(pxu), qty, tif,
    ttl_s: tif === 'POST' ? (ttl || 15) : null, override: false, touch: null, t_click_ms: t }, extra || {}));
  const ordEvents = cid => evs.filter(e => e.type === 'ord' && e.data.cid === cid).map(e => e.data);
  const fillsOf = cid => evs.filter(e => e.type === 'fill' && e.data.cid === cid).map(e => e.data);

  run(1000);
  // the web worker's guards: Host, then the per-start X-Desk token on every POST
  check(C.hostOk('127.0.0.1:8788', 8788) && C.hostOk('localhost:8788', 8788) && C.hostOk('LOCALHOST:8788', 8788), 'Host 127.0.0.1:<port> and localhost:<port> pass');
  check(!C.hostOk('evil.example:8788', 8788) && !C.hostOk('127.0.0.1:8787', 8788) && !C.hostOk('127.0.0.1', 8788) && !C.hostOk('', 8788), 'any other Host is refused (DNS rebinding)');
  check(typeof sim.token === 'string' && sim.token.length >= 16, 'the sim has a per-start token (what the backend writes into the page)');
  let g1 = call('POST', '/api/kill', {}, 300, {});
  check(g1.s === 403 && g1.j.err.code === 'FORBIDDEN' && sim.state.run.state === 'STOP', 'a POST without the token is refused, KILL included', g1.j);
  g1 = call('POST', '/api/stop', {}, 300, { 'X-Desk': 'stale-token-from-before' });
  check(g1.s === 403 && g1.j.err.code === 'FORBIDDEN' && /stale/.test(g1.j.err.msg), 'a stale token is refused', g1.j);
  g1 = call('GET', '/api/health', undefined, 300, { Host: 'evil.example:8788' });
  check(g1.s === 403 && g1.j.err.code === 'FORBIDDEN', 'a GET with a foreign Host is refused', g1.j);
  g1 = call('GET', '/api/health', undefined, 300, { Host: '127.0.0.1:8788' });
  eq(g1.s, 200, 'a GET with Host 127.0.0.1:<port> is served');
  g1 = call('POST', '/api/stop', {}, 300, { 'X-Desk': sim.token, Host: 'localhost:8788' });
  eq(g1.s, 200, 'a POST with the token and a local Host is served');
  const ng = sim.markets().find(x => x.tk.startsWith('KXNATGAS15M'));
  const ngm = sim.freeze(ng.key, { bids: [[6300, 50], [6200, 40]], asks: [[6400, 30], [6500, 40]] });
  sim.cash()[ng.shard] += 2000000;        // room on the shard for the matching tests
  const shardRefused = order(ng, 'BUY', 6400, 1000, 'IOC');
  check(shardRefused.s === 409 && shardRefused.j.err.code === 'SHARD_CASH', 'an order larger than the shard\'s free cash is refused (SHARD_CASH)', shardRefused.j);
  // IOC fills against the book and never rests
  let r = order(ng, 'BUY', 6500, 10, 'IOC');
  const firstIoc = r.j.ord.cid;
  check(r && r.s === 200 && r.j.ord.st === 'FILL' && r.j.ord.filled === 10 && r.j.ord.avg === '0.6400', 'IOC BUY fills at the ask', r && r.j);
  check(ordEvents(r.j.ord.cid).every(d => d.st !== 'WORK' && d.st !== 'PART'), 'an IOC is never WORK or PART');
  check(fillsOf(r.j.ord.cid).every(f => f.liq === 'T'), 'IOC fills are taker fills');
  r = order(ng, 'BUY', 6400, 50, 'IOC');
  check(r.j.ord.st === 'CXL' && r.j.ord.why === 'IOC_REST' && r.j.ord.filled === 20 && r.j.ord.left === 0, 'IOC fills what is there and kills the rest', r.j.ord);
  check(!ngm.ours.some(x => x.cid === r.j.ord.cid), 'IOC never rests on the book');
  r = order(ng, 'BUY', 6500, 100, 'FOK');
  check(r.j.ord.st === 'CXL' && r.j.ord.why === 'FOK' && r.j.ord.filled === 0, 'FOK that cannot fill completely fills nothing', r.j.ord);
  // post-only never crosses
  r = order(ng, 'BUY', 6500, 5, 'POST', 15);
  check(r.j.ord.st === 'CXL' && r.j.ord.why === 'POST_CROSS' && r.j.ord.filled === 0, 'a POST at the ask is cancelled, not filled', r.j.ord);
  r = order(ng, 'SELL', 6300, 5, 'POST', 15);
  check(r.j.ord.st === 'CXL' && r.j.ord.why === 'POST_CROSS', 'a POST at the bid is cancelled', r.j.ord);
  // resting, queue position, fills when prints trade through
  r = order(ng, 'BUY', 6300, 10, 'POST', 60);
  const rest = r.j.ord;
  check(rest.st === 'WORK' && rest.piq === 50, 'a POST rests behind the queue already there', rest);
  check(rest.exp_ms !== null && rest.exp_ms % 1000 === 0 && rest.exp_ms >= rest.t.recv + 60000, 'POST carries a whole-second exchange expiry');
  sim.trade(ng.key, 'SELL', 30);
  let cur = sim.order(rest.cid);
  check(cur.st === 'WORK' && cur.ahead === 20, 'prints ahead of us shrink the queue', { st: cur.st, ahead: cur.ahead });
  sim.trade(ng.key, 'SELL', 25);
  cur = sim.order(rest.cid);
  check(cur.st === 'PART' && cur.filled === 5, 'prints through the queue fill us (PART)', { st: cur.st, filled: cur.filled });
  sim.trade(ng.key, 'SELL', 10);
  cur = sim.order(rest.cid);
  check(cur.st === 'FILL' && cur.filled === 10, 'and then FILL');
  check(fillsOf(rest.cid).length === 2 && fillsOf(rest.cid).every(f => f.liq === 'M' && f.px === '0.6300'), 'resting fills are maker fills at our price');
  const ordSeq = ordEvents(rest.cid).map(d => d.st).join('>');
  check(/^PEND>WORK>PART>FILL$/.test(ordSeq), 'state sequence PEND>WORK>PART>FILL', ordSeq);
  // expiry
  r = order(ng, 'BUY', 6200, 5, 'POST', 5);
  const ex = r.j.ord;
  run(4000);
  eq(sim.order(ex.cid).st, 'WORK', 'a POST is still working before its expiry');
  run(2500);
  check(sim.order(ex.cid).st === 'EXP' && sim.order(ex.cid).why === 'EXPIRY' && sim.order(ex.cid).left === 0, 'a POST expires at its deadline (EXP)');
  // cancel
  r = order(ng, 'BUY', 6100, 5, 'POST', 60);
  const cx = call('POST', '/api/cancel', { cids: [r.j.ord.cid] });
  check(cx.j.ok && cx.j.n === 1 && sim.order(r.j.ord.cid).st === 'CXL' && sim.order(r.j.ord.cid).why === 'USER', 'cancel by cid', cx.j);
  // amend: a price that crosses cancels (post-only never crosses)
  r = order(ng, 'BUY', 6100, 5, 'POST', 60);
  const am = call('POST', '/api/amend', { cid: r.j.ord.cid, px: '0.6600', qty: null, ttl_s: null });
  check(am.j.ok && am.j.ord.st === 'CXL' && am.j.ord.why === 'POST_CROSS', 'an amend into the ask cancels (POST_CROSS)', am.j);
  r = order(ng, 'BUY', 6100, 5, 'POST', 15);
  const am2 = call('POST', '/api/amend', { cid: r.j.ord.cid, px: null, qty: null, ttl_s: 60 });
  check(am2.j.ok && am2.j.ord.st === 'WORK' && am2.j.ord.exp_ms >= t + 59000, 'an expiry-only amend keeps the order working', am2.j.ord);
  call('POST', '/api/cancel', { cids: [r.j.ord.cid] });
  // price band and override
  ngm.asks.set(6400, 30);
  r = order(ng, 'BUY', 6700, 1, 'IOC');
  check(r.s === 409 && r.j.err.code === 'PRICE_BAND' && r.j.ord.st === 'REJ', 'a crossing order 3 ticks through the touch is refused', r.j);
  r = order(ng, 'BUY', 6700, 1, 'IOC', null, { override: true });
  check(r.s === 200 && r.j.ord.st === 'FILL', 'override sends it anyway', r.j.ord);
  // self-trade prevention: a manual taker cancels our own resting order ('maker')
  ngm.asks = new Map([[6400, 30], [6500, 40]]);
  r = order(ng, 'SELL', 6500, 5, 'POST', 60);
  const mine = r.j.ord;
  check(mine.st === 'WORK', 'our own resting sell');
  sim.order(mine.cid).restAt = 0;          // first in the queue at 0.65, ahead of the 40
  sim.order(mine.cid).ahead = 0;
  r = order(ng, 'BUY', 6500, 40, 'IOC');
  check(r.j.ord.filled === 40 && sim.order(mine.cid).st === 'CXL' && sim.order(mine.cid).why === 'STP', 'a manual taker cancels our own resting order (STP) and keeps matching', { taker: r.j.ord, resting: sim.order(mine.cid).st });
  // duplicate cid
  const dup = call('POST', '/api/order', { cid: 'Mtest0001', v: ng.v, tk: ng.tk, side: 'BUY', px: '0.6000', qty: 1, tif: 'IOC', ttl_s: null, override: false, touch: null, t_click_ms: null });
  check(dup.s === 409 && dup.j.err.code === 'DUP_CID' && dup.j.ord.cid === 'Mtest0001', 'a reused cid is refused with the original order');
  // bad requests
  const bad = call('POST', '/api/order', { cid: 'Mtest9999', v: ng.v, tk: ng.tk, side: 'BUY', px: '0.6000', qty: 1, tif: 'POST', ttl_s: null, override: false, touch: null, t_click_ms: null });
  check(bad.s === 400 && bad.j.err.code === 'BAD_REQUEST', 'POST without ttl_s is a bad request', bad.j);
  const off = call('POST', '/api/order', { cid: 'Mtest9998', v: ng.v, tk: ng.tk, side: 'BUY', px: '0.6050', qty: 1, tif: 'IOC', ttl_s: null, override: false, touch: null, t_click_ms: null });
  check(off.s === 400 && off.j.err.code === 'OFF_GRID', 'a price off the market grid is refused', off.j);

  // fees and cash by formula (Kalshi QUAD weather, Polymarket taker and maker)
  const wx = sim.markets().find(x => x.kind === 'WEATHER' && x.status === 'OPEN' && x.fee.type === 'QUAD');
  sim.freeze(wx.key, { bids: [[2900, 50]], asks: [[3000, 50]] });
  const cash0 = sim.cash()[wx.shard];
  r = order(wx, 'BUY', 3000, 10, 'IOC');
  const wf = fillsOf(r.j.ord.cid)[0];
  const kExpect = Math.ceil(0.07 * 10 * 0.3 * 0.7 * 100 - 1e-9) / 100;
  eq(wf.fee, kExpect.toFixed(4), 'Kalshi taker fee = ceil to the cent of 0.07 C p (1-p)');
  eq(Math.round(cash0 - sim.cash()[wx.shard]), Math.round((3.00 + kExpect) * 10000), 'shard cash falls by cost plus fee');
  eq(sim.position(wx.key).pos, 10, 'the position nets to +10');
  r = order(wx, 'SELL', 2900, 4, 'IOC');
  const wf2 = fillsOf(r.j.ord.cid)[0];
  eq(sim.position(wx.key).pos, 6, 'a sell reduces the long');
  eq(wf2.real, N.moneyStr((2900 - 3000) * 4 - N.money(wf2.fee)), 'realized P&L on the reduction, net of its fee');
  const pm = sim.markets().find(x => x.v === 'P' && x.status === 'OPEN' && x.grid[0].step === '0.0100');
  sim.freeze(pm.key, { bids: [[4900, 50]], asks: [[5000, 50]] });
  r = order(pm, 'BUY', 5000, 10, 'IOC');
  const pf = fillsOf(r.j.ord.cid)[0];
  const pExpect = D.fees.roundHE(0.0695 * 10 * 0.5 * 0.5 * 100) / 100;
  eq(pf.fee, pExpect.toFixed(4), 'Polymarket taker fee = banker\'s round of 0.0695 C p (1-p)');
  r = order(pm, 'SELL', 5100, 10, 'POST', 60);
  sim.trade(pm.key, 'BUY', 60);
  const pmf = fillsOf(r.j.ord.cid)[0];
  check(pmf && pmf.liq === 'M' && pmf.fee === N.moneyStr(-D.fees.roundHE(0.0125 * 10 * 0.51 * 0.49 * 100) * 100), 'Polymarket maker fill earns the rebate', pmf);
  const ngFill = fillsOf(firstIoc)[0];
  eq(ngFill.fee, (Math.ceil(0.07 * 10 * 0.64 * 0.36 * 100 - 1e-9) / 100).toFixed(4), 'the LIP 15-minute series charge takers the quadratic fee (makers pay nothing)');
  check(sim.markets().filter(x => x.kind === 'LIP15').every(x => x.fee.type === 'QUAD') && sim.markets().filter(x => x.kind === 'GAME' && x.v === 'K' && /^KXNCAAFGAME/.test(x.tk)).some(x => x.fee.type === 'QUAD_MAKER'),
    'fee types: 15-minute series QUAD, FBS games QUAD_MAKER');
  r = order(pm, 'BUY', 5000, 1.5, 'IOC');
  check(r.s === 400 && r.j.err.code === 'BAD_QTY', 'Polymarket takes whole contracts only');

  // reduce_only (IOC only): never increases the position, qty cut to it
  {
    sim.freeze(wx.key, { bids: [[2900, 50]], asks: [[3000, 50]] });
    const p0 = sim.position(wx.key).pos;
    r = order(wx, 'BUY', 3000, 1, 'IOC', null, { reduce_only: true });
    check(r.s === 409 && r.j.err.code === 'REDUCE' && r.j.ord.st === 'REJ' && r.j.ord.why === 'REDUCE', 'reduce_only on the increasing side is refused (REDUCE)', r.j);
    r = order(wx, 'SELL', 2900, p0 + 5, 'IOC', null, { reduce_only: true });
    check(r.s === 200 && r.j.ord.qty === p0 && r.j.ord.filled === p0 && sim.position(wx.key).pos === 0, 'reduce_only cuts the qty to the position and flattens it', { ord: r.j.ord, pos: sim.position(wx.key).pos });
    r = order(wx, 'SELL', 2900, 1, 'IOC', null, { reduce_only: true });
    check(r.s === 409 && r.j.err.code === 'REDUCE', 'reduce_only on a flat position is refused', r.j);
    r = order(wx, 'SELL', 3100, 1, 'POST', 15, { reduce_only: true });
    check(r.s === 400 && r.j.err.code === 'BAD_REQUEST', 'reduce_only is IOC only', r.j);
  }
  // an absolute expiry (the ticket's START): exactly one of ttl_s and exp_ms on a POST
  {
    const e1 = Math.ceil((t + 20000) / 1000) * 1000;
    r = order(ng, 'BUY', 6000, 1, 'POST', null, { ttl_s: null, exp_ms: e1 });
    check(r.s === 200 && r.j.ord.st === 'WORK' && r.j.ord.exp_ms === e1, 'a POST with exp_ms rests until that time', r.j.ord);
    const ce = r.j.ord.cid;
    run(e1 - t + 200);
    check(sim.order(ce).st === 'EXP' && sim.order(ce).why === 'EXPIRY', 'and expires there');
    r = order(ng, 'BUY', 6000, 1, 'POST', 15, { exp_ms: e1 + 60000 });
    check(r.s === 400 && r.j.err.code === 'BAD_REQUEST', 'ttl_s and exp_ms together are a bad request');
    r = order(ng, 'BUY', 6000, 1, 'POST', null, { ttl_s: null, exp_ms: Math.ceil((t + 2000) / 1000) * 1000 });
    check(r.s === 400 && r.j.err.code === 'BAD_REQUEST', 'an expiry under 5 s ahead is a bad request');
    r = order(ng, 'BUY', 6000, 1, 'IOC', null, { exp_ms: e1 + 60000 });
    check(r.s === 400 && r.j.err.code === 'BAD_REQUEST', 'an IOC carries no expiry');
    // until close: exp_ms = the market's close, however far (a T+1 weather market closes > 24 h out)
    const far = sim.markets().filter(x => x.kind === 'WEATHER' && x.status === 'OPEN').sort((a, b) => b.close_ms - a.close_ms)[0];
    sim.freeze(far.key, { bids: [[2000, 30]], asks: [[2600, 30]] });
    const fc = Math.floor(far.close_ms / 1000) * 1000;
    check(fc - t > 86400000, 'the demo lists a market closing more than 24 h ahead', (fc - t) / 3600e3);
    r = order(far, 'BUY', 2100, 1, 'POST', null, { ttl_s: null, exp_ms: fc });
    check(r.s === 200 && r.j.ord.st === 'WORK' && r.j.ord.exp_ms === fc, 'a post until the close rests with the close as its venue expiry', r.j);
    D.contract.validateResponse('POST /api/order', r.s, r.j).forEach(e => check(false, 'until-close reply: ' + e));
    r = order(far, 'BUY', 2100, 1, 'POST', null, { ttl_s: null, exp_ms: fc + 1000 });
    check(r.s === 400 && r.j.err.code === 'BAD_REQUEST' && /close/.test(r.j.err.msg), 'an expiry past the market\'s close is a bad request', r.j);
    {
      // on a sim of its own (its clock runs to a 15-minute close)
      let tc = t;
      const simC = D.sim.create({ now: () => tc, emit: () => {}, seed: 21, sid: 'closes01' });
      const runC = ms => { const end = tc + ms; while (tc < end) { tc = Math.min(end, Math.max(tc + 1, Math.min(simC.nextDue(), tc + 100))); simC.pump(); } };
      runC(500);
      const lk = simC.markets().find(x => x.kind === 'LIP15' && x.status === 'OPEN' && x.close_ms - tc > 20000);
      simC.freeze(lk.key, { bids: [[5000, 40]], asks: [[5200, 40]] });
      const lc = Math.floor(lk.close_ms / 1000) * 1000;
      let rc = null;
      simC.request('POST', '/api/order', { cid: 'Mclose0001', v: 'K', tk: lk.tk, side: 'BUY', px: '0.4800', qty: 1, tif: 'POST', ttl_s: null, exp_ms: lc, override: false, touch: null, t_click_ms: null },
        (st2, j2) => { rc = { s: st2, j: j2 }; }, { 'X-Desk': simC.token });
      runC(300);
      check(rc && rc.s === 200 && rc.j.ord.st === 'WORK', 'a 15-minute window takes an until-close post', rc && rc.j);
      runC(lk.close_ms - tc + 500);
      check(simC.order('Mclose0001').st === 'EXP' && simC.order('Mclose0001').why === 'EXPIRY', 'it ends EXP at the close', { st: simC.order('Mclose0001').st, why: simC.order('Mclose0001').why });
    }
    const game = sim.markets().find(x => x.kind === 'GAME');
    check(sim.markets().every(x => x.kind === 'GAME' ? x.game && typeof x.game.start === 'number' : true), 'games carry their start');
    const gev = evs.filter(e => e.type === 'mkt').map(e => e.data).concat(sim.snapshot().markets).find(x => x.tk === game.tk);
    check(gev && gev.start_ms === game.game.start && sim.snapshot().markets.filter(x => x.kind !== 'GAME').every(x => x.start_ms === null), 'mkt.start_ms is the game start (null for other markets)');
  }
  // moving cash between Kalshi shards
  {
    const c0 = Object.assign({}, sim.cash());
    r = call('POST', '/api/transfer', { from: 'S0', to: 'S1', amt: '5.0000' });
    check(r.s === 200 && sim.cash().S0 === c0.S0 - 50000 && sim.cash().S1 === c0.S1 + 50000 && r.j.shards.length === 2, 'a transfer moves free cash between shards', r.j);
    r = call('POST', '/api/transfer', { from: 'S0', to: 'S1', amt: '99999.0000' });
    check(r.s === 409 && r.j.err.code === 'NO_CASH', 'more than the free cash is refused', r.j);
    r = call('POST', '/api/transfer', { from: 'S1', to: 'S1', amt: '1.0000' });
    check(r.s === 400 && r.j.err.code === 'BAD_REQUEST', 'a shard to itself is a bad request', r.j);
  }
  // a manual order takes a market over (mkt.man) until release
  {
    const lk = sim.markets().find(x => x.kind === 'LIP15' && x.status === 'OPEN' && !x.frozen && x.key !== ng.key);
    const nMkt = evs.length;
    r = order(lk, 'BUY', 100, 1, 'POST', 60);
    const mk1 = evs.slice(nMkt).filter(e => e.type === 'mkt' && e.data.tk === lk.tk).map(e => e.data);
    check(mk1.length === 1 && mk1[0].man === true, 'a manual order emits the market held (man true)', mk1);
    call('POST', '/api/cancel', { cids: [r.j.ord.cid] });
    const nRel = evs.length;
    r = call('POST', '/api/release', { v: lk.v, tk: lk.tk });
    const mk2 = evs.slice(nRel).filter(e => e.type === 'mkt' && e.data.tk === lk.tk).map(e => e.data);
    check(r.s === 200 && mk2.length === 1 && mk2[0].man === false, 'release hands it back (man false)', mk2);
  }
  // an order placed outside the desk (own EXT)
  const extCid = sim.external(wx.key, 'BUY', 2000, 3);
  run(300);
  {
    const xe = ordEvents(extCid);
    check(extCid && xe.length && xe[xe.length - 1].own === 'EXT' && xe[xe.length - 1].st === 'WORK' && xe[xe.length - 1].exp_ms === null, 'an EXT order shows on the stream, resting without an expiry', xe[xe.length - 1]);
    call('POST', '/api/cancel_all', { own: 'MAN', v: null });
    call('POST', '/api/stop', {});
    eq(sim.order(extCid).st, 'WORK', 'Stop and cancel-all MAN leave an EXT order alone');
  }

  // kill lockout and re-arm
  r = order(ng, 'BUY', 6000, 5, 'POST', 60);
  const k1 = call('POST', '/api/kill', {});
  check(k1.j.ok && k1.j.run.state === 'KILLED' && k1.j.run.rearm_ms === k1.j.run.since_ms + 60000, 'KILL stops everything for 60 s', k1.j);
  check(sim.order(r.j.ord.cid).st === 'CXL' && sim.order(r.j.ord.cid).why === 'KILL', 'KILL cancels working orders');
  check(sim.order(extCid).st === 'CXL' && sim.order(extCid).why === 'KILL', 'KILL cancels EXT orders too (everything on the account)');
  const pfK = sim.snapshot().hdr.preflight;
  check(pfK.map(x => x.id).join(' ') === 'KEY CASH SHARD CLK DISK FEED PRIV GROUPS' && pfK.find(x => x.id === 'GROUPS').ok === false, 'preflight carries PRIV and GROUPS; KILL leaves the order groups triggered', pfK);
  r = order(ng, 'BUY', 6000, 5, 'IOC');
  check(r.s === 409 && r.j.err.code === 'KILLED', 'new orders are refused during the lockout');
  const s1 = call('POST', '/api/start', {});
  check(s1.s === 409 && s1.j.err.code === 'KILLED', 'start is refused during the lockout');
  const re1 = call('POST', '/api/rearm', {});
  check(re1.s === 409 && re1.j.err.code === 'BAD_STATE', 're-arm is refused before 60 s');
  run(60000);
  const re2 = call('POST', '/api/rearm', {}, 50);
  check(re2.s === 200 && re2.j.run.state === 'STOP', 're-arm after 60 s returns to STOP');
  const sg = call('POST', '/api/start', {}, 50);
  check(sg.s === 409 && sg.j.err.code === 'PREFLIGHT' && /GROUPS/.test(sg.j.err.msg), 'start waits for the order groups to reset after a re-arm', sg.j);
  run(1500);
  check(sim.snapshot().hdr.preflight.find(x => x.id === 'GROUPS').ok, 'the groups reset about a second later');
  r = order(ng, 'BUY', 6000, 5, 'IOC');
  eq(r.s, 200, 'orders are accepted again after re-arm');

  if (process.env.V) console.log('section 7');
  // ---- 7. a long run: every event and reply validates against the contract ------------------
  call('POST', '/api/subscribe', { sid: 'testsid1', ladders: [{ v: ng.v, tk: ng.tk }, { v: pm.v, tk: pm.tk }], tops: 'ALL' });
  {
    const nL = evs.length;
    let laneReply = null;
    sim.request('POST', '/api/lane/X1', { on: true }, (s2, j2) => { laneReply = { s: s2, j: j2 }; replies.push(['POST', '/api/lane/X1', s2, j2]); }, HDRS);
    check(!evs.slice(nL).some(e => e.type === 'lane') && laneReply === null && !sim.state.laneById.X1.on, 'a lane toggle is applied by the strategy worker a few ms later, not inside the click');
    run(300);
    check(laneReply && laneReply.s === 200 && laneReply.j.lane.on === true && evs.slice(nL).some(e => e.type === 'lane' && e.data.id === 'X1' && e.data.on), 'then the lane echo and the reply arrive', laneReply);
  }
  const st1 = call('POST', '/api/start', {});
  check(st1.s === 200 && st1.j.run.state === 'RUN', 'start runs the lanes', st1.j);
  run(10 * 60000);
  call('POST', '/api/lane/M1', { on: false });
  call('POST', '/api/cancel_all', { own: 'ANY', v: null });
  run(5 * 60000);
  call('POST', '/api/lane/M1', { on: true });
  sim.setFeed('KXP', false);
  const stale = order(sim.markets().find(x => x.kind === 'WEATHER' && x.status === 'OPEN' && !x.frozen), 'BUY', 100, 1, 'IOC');
  check(stale.s === 409 && stale.j.err.code === 'STALE', 'fail closed: Kalshi orders refused while its private socket is down');
  sim.setFeed('KXP', true);
  sim.dropNext();
  run(32 * 60000);                // window rolls, a game goes FINAL and settles
  call('POST', '/api/stop', {});
  run(2000);

  let bad2 = 0, gaps = 0;
  const seen = {};
  evs.forEach((e, i) => {
    seen[e.type] = (seen[e.type] || 0) + 1;
    const errs = C.validateEvent(e);
    if (errs.length && bad2++ < 5) console.log('  invalid event', e.type, e.seq, errs.slice(0, 2));
    if (i && e.seq !== evs[i - 1].seq + 1) gaps++;
  });
  eq(bad2, 0, 'every event the sim emitted validates against the contract (' + evs.length + ' events)');
  eq(gaps, 1, 'the sim emitted a contiguous sequence (one deliberate drop)');
  for (const tp of Object.keys(C.events)) check(seen[tp] > 0, 'the sim emitted ' + tp + ' events', seen);
  let badR = 0;
  replies.forEach(([meth, p, s, j]) => {
    const errs = C.validateResponse(C.endpointFor(meth, p), s, j);
    if (errs.length && badR++ < 5) console.log('  invalid reply', meth, p, s, errs.slice(0, 2));
  });
  eq(badR, 0, 'every HTTP reply the sim gave validates against the contract (' + replies.length + ' replies)');
  const snap = sim.snapshot();
  check(!C.validate(C.snapshot, snap).length, 'the sim snapshot validates', C.validate(C.snapshot, snap).slice(0, 3));
  eq(snap.seq, sim.state.seq, 'the snapshot carries the stream position');
  const ordSt = {}; evs.filter(e => e.type === 'ord').forEach(e => { ordSt[e.data.st] = 1; });
  check(['PEND', 'WORK', 'PART', 'FILL', 'CXL', 'REJ', 'EXP'].every(s => ordSt[s]), 'every order state occurred', Object.keys(ordSt));
  check(evs.some(e => e.type === 'mkt' && e.data.status === 'SETTLED' && e.data.kind === 'LIP15'), 'a 15-minute window rolled and settled');
  check(evs.some(e => e.type === 'mkt' && e.data.status === 'SETTLED' && e.data.kind === 'GAME'), 'a game finished and settled');
  check(evs.some(e => e.type === 'mo' && e.data.h === 'SETTLE'), 'settlement markouts were emitted');
  check(evs.filter(e => e.type === 'fill' && e.data.own === 'SYS').length > 20, 'the lanes traded while running');
  const hbs = evs.filter(e => e.type === 'hb');
  check(hbs.every((e, i) => !i || e.data.srv_ms - hbs[i - 1].data.srv_ms >= 900), 'heartbeats at most 1/s');
  const hdrTimes = evs.filter(e => e.type === 'hdr').map(e => e.ts_ms);
  check(hdrTimes.every((x, i) => !i || x - hdrTimes[i - 1] >= 250), 'hdr at most 4/s');
  const lastBook = {}; let tooFast = 0;
  evs.filter(e => e.type === 'book').forEach(e => { const k = e.data.v + '|' + e.data.tk; if (lastBook[k] && e.ts_ms - lastBook[k] < 50) tooFast++; lastBook[k] = e.ts_ms; });
  eq(tooFast, 0, 'book messages coalesced to at most 20/s per ticker');
  check(evs.filter(e => e.type === 'book' && e.data.depth === 'FULL').length > 0, 'ladder subscriptions stream FULL books');
  check(evs.every(e => e.type !== 'ord' || e.data.tif === 'POST' || (e.data.st !== 'WORK' && e.data.st !== 'PART')), 'no IOC or FOK ever rested in the whole run');
  const cross = evs.filter(e => e.type === 'fill' && e.data.liq === 'T').map(e => sim.order(e.data.cid)).filter(o => o && o.tif === 'POST');
  eq(cross.length, 0, 'no POST order ever took liquidity in the whole run');
  // order revisions: up on every change, the same state carries the same rev in a reply
  {
    const last = {}; let back = 0, same = 0, mism = 0;
    evs.filter(e => e.type === 'ord').forEach(e => {
      const d = e.data, k = d.cid, sig = d.st + '|' + d.filled + '|' + d.left;
      if (last[k] && d.rev < last[k].rev) back++;
      if (last[k] && d.rev === last[k].rev && sig !== last[k].sig) same++;
      last[k] = { rev: d.rev, sig };
    });
    eq(back + same, 0, 'every order\'s rev rises on the stream, and a change always carries a new rev');
    replies.forEach(([meth, p, s2, j]) => {
      if (!j || !j.ord) return;
      const evRev = evs.filter(e => e.type === 'ord' && e.data.cid === j.ord.cid && e.data.rev === j.ord.rev);
      if (evRev.length && evRev[0].data.st !== j.ord.st) mism++;
    });
    eq(mism, 0, 'a reply and a stream event with the same rev carry the same state');
  }
  check(evs.some(e => e.type === 'ord' && e.data.why === 'LANE_OFF' && e.data.lane === 'M1'), 'switching a lane off cancels its orders with why LANE_OFF');
  {
    const fids = new Set(snap.fills.map(f => f.fid));
    check(snap.fills.length <= 5000 && snap.mos.every(m => fids.has(m.fid)), 'the snapshot carries at most 5000 fills and only their markouts', { fills: snap.fills.length, mos: snap.mos.length });
    const bs = call('POST', '/api/subscribe', { sid: 'someone-else', ladders: [], tops: 'ALL' });
    check(bs.s === 409 && bs.j.err.code === 'BAD_STATE', 'a subscribe for a sid with no open stream is refused BAD_STATE', bs);
    let gs = null; sim.request('GET', '/api/snapshot?sid=nobody', undefined, (s2, j) => { gs = { s: s2, j }; }); run(50);
    check(gs && gs.s === 409 && gs.j.err.code === 'BAD_STATE', 'so is a snapshot', gs);
  }
  // the browser loop wakes for a reply due before its next world tick (real timers)
  {
    const simR = D.sim.create({ seed: 9, sid: 'wake0001', emit: () => {} });
    simR.start();
    await new Promise(r => setTimeout(r, 300));
    const lags = [];
    for (let i = 0; i < 8; i++) {
      const t0 = Date.now();
      await new Promise(res => simR.request('GET', '/api/health', undefined, () => { lags.push(Date.now() - t0); res(); }));
      await new Promise(r => setTimeout(r, 37));
    }
    simR.stop();
    lags.sort((a, b) => a - b);
    check(lags[4] <= 15, 'a reply due in 1 ms arrives in about that, not at the next 100 ms tick (p50 ms)', lags);
  }

  if (process.env.V) console.log('section 8');
  // ---- 8. the store follows the sim end to end, through a dropped message --------------------
  {
    const c3 = makeCtx();
    const D3 = c3.Desk, st3 = D3.store;
    let t3 = Date.UTC(2026, 9, 6, 23, 30, 0);
    const sim3 = D3.sim.create({ now: () => t3, seed: 3, sid: 'endtoend', emit: e => st3.apply(e) });
    st3.fetchSnapshot = () => new Promise(res => sim3.request('GET', '/api/snapshot?sid=endtoend', undefined, (s, j) => res(j)));
    st3.begin(); st3.onTransport('open', false); st3.resync('BOOT');
    const run3 = async ms => { const end = t3 + ms; while (t3 < end) { t3 = Math.min(end, t3 + 50); sim3.pump(); await flushMicro(); } };
    await run3(1000);
    check(!st3.resyncing && st3.seq === sim3.state.seq, 'the store is in step with the stream after boot', { seq: st3.seq, sim: sim3.state.seq });
    sim3.request('POST', '/api/start', {}, () => {}, { 'X-Desk': sim3.token });
    await run3(30000);
    sim3.dropNext();
    await run3(20000);
    check(st3.gaps >= 1 && !st3.resyncing && st3.seq === sim3.state.seq, 'a dropped message is detected and healed by a snapshot', { gaps: st3.gaps, seq: st3.seq, sim: sim3.state.seq });
    eq(st3.invalid, 0, 'the store saw no contract violations');
    const live = Array.from(sim3.state.live).length;
    eq(st3.working().length, live, 'the store\'s working orders match the venue\'s');
    const sp = Array.from(sim3.state.pos.values()).filter(x => x.pos !== 0).length;
    eq(Array.from(st3.pos.values()).filter(x => x.pos !== 0).length, sp, 'the store\'s open positions match the venue\'s');
    check(st3.hdr && st3.hdr.mode === 'DEMO' && st3.lanes.size === 5, 'header and lanes arrived');
  }

  if (process.env.V) console.log('section 9');
  // ---- 9. live transport: the X-Desk token from the page, refreshed after a restart -----------
  {
    let srvToken = 'tok-B', healthOk = false;
    const calls = [];
    const page = tok => '<!doctype html><html><head><meta charset="utf-8">\n<meta name="desk-token" content="' + tok + '">\n</head></html>';
    const resp = (status, body) => ({ status, text: () => Promise.resolve(typeof body === 'string' ? body : JSON.stringify(body)) });
    const fetchMock = (url, init) => {
      calls.push({ url, method: init.method, xdesk: init.headers && init.headers['X-Desk'], cache: init.cache });
      if (init.method === 'GET' && url === '/desk/') return Promise.resolve(resp(200, page(srvToken)));
      if (init.method === 'GET' && url === '/api/health') return Promise.resolve(healthOk ? resp(200, { ok: true, epoch: 'e-2', srv_ms: Date.now() }) : resp(503, ''));
      if (init.method === 'POST') {
        if (init.headers['X-Desk'] !== srvToken) return Promise.resolve(resp(403, { ok: false, err: { code: 'FORBIDDEN', msg: 'stale X-Desk token' } }));
        return Promise.resolve(resp(200, { ok: true, run: { state: 'KILLED', since_ms: Date.now(), rearm_ms: Date.now() + 60000, why: 'operator' }, n: 0 }));
      }
      return Promise.resolve(resp(404, { ok: false, err: { code: 'NOT_FOUND', msg: url } }));
    };
    const esList = [];
    function FakeES(url) { this.url = url; this.ls = {}; this.readyState = 1; esList.push(this); }
    FakeES.prototype.addEventListener = function (t, f) { this.ls[t] = f; };
    FakeES.prototype.close = function () {};
    const meta = { getAttribute: k => (k === 'content' ? 'tok-A' : null) };
    const c5 = { console, setTimeout, clearTimeout, setInterval, clearInterval, performance, Promise, Uint8Array, AbortController,
                 localStorage: { getItem() { return null; }, setItem() {} }, fetch: fetchMock, EventSource: FakeES,
                 location: { protocol: 'http:', search: '?live=1', pathname: '/desk/' },
                 document: { querySelector: sel => (sel === 'meta[name="desk-token"]' ? meta : null) } };
    c5.window = c5; vm.createContext(c5);
    for (const f of ['contract.js', 'core.js', 'store.js', 'sim.js', 'transport.js']) vm.runInContext(fs.readFileSync(path.join(JS, f), 'utf8'), c5, { filename: f });
    const T5 = c5.Desk.transport;
    eq(await T5.choose(), 'live', '?live=1 picks the live transport');
    eq(c5.Desk.token.value, 'tok-A', 'the token is read from <meta name="desk-token"> at boot');
    const kr = await T5.post('/api/kill', {});
    check(kr.status === 200 && calls.length === 3 && calls[0].xdesk === 'tok-A' && calls[1].method === 'GET' && calls[1].url === '/desk/' && calls[1].cache === 'no-store' && calls[2].xdesk === 'tok-B',
      'a POST refused FORBIDDEN re-reads the token from the page and is retried once with it', calls);
    eq(c5.Desk.token.value, 'tok-B', 'the new token is kept');
    // a backend restart (new hb.epoch) re-reads the token before any POST
    const got = [];
    T5.connect(e => got.push(e), () => {});
    const es = esList[esList.length - 1];
    const hb = ep => ({ data: JSON.stringify({ type: 'hb', seq: 1, ts_ms: Date.now(), data: { epoch: ep, sid: T5.sid, srv_ms: Date.now(), feeds: {} } }) });
    srvToken = 'tok-C';
    const n0 = calls.length;
    es.ls.hb(hb('e-1')); es.ls.hb(hb('e-1'));
    eq(calls.length, n0, 'the same epoch fetches nothing');
    es.ls.hb(hb('e-2'));
    await flushMicro(); await flushMicro();
    check(calls.length === n0 + 1 && calls[n0].method === 'GET' && c5.Desk.token.value === 'tok-C' && got.length === 3, 'a new hb.epoch re-reads the token', { calls: calls.slice(n0), tok: c5.Desk.token.value });
    const posts = calls.filter(x => x.method === 'POST');
    check(posts.every(x => typeof x.xdesk === 'string' && x.xdesk.length), 'every POST carried X-Desk');
    check(calls.filter(x => x.method === 'GET').every(x => x.xdesk === undefined), 'GETs carry no token');
    // the watchdog: a stream silent for 5 s is closed and opened again
    const nEs = esList.length;
    T5.impl.lastMsg = Date.now() - 6000;
    await new Promise(r => setTimeout(r, 1100));
    check(esList.length === nEs + 1, 'a stream with no message for 5 s is reopened', { before: nEs, after: esList.length });
    // while the stream is down, GET /api/health every 5 s; an answer reopens the stream at once
    healthOk = true;
    esList[esList.length - 1].readyState = 0;
    T5.impl.lastProbe = 0;
    const nEs2 = esList.length;
    await new Promise(r => setTimeout(r, 1100)); await flushMicro();
    check(calls.some(x => x.url === '/api/health') && esList.length === nEs2 + 1, 'a down stream probes /api/health and reopens when it answers', { es: esList.length - nEs2 });
    T5.impl.close();
  }
  // live vs demo: never switched by a health probe; a page the backend served is always live
  {
    const mk = (protocol, search, tok) => {
      const c6 = { console, setTimeout, clearTimeout, setInterval, clearInterval, performance, Promise, Uint8Array, AbortController,
                   localStorage: { getItem() { return null; }, setItem() {} }, fetch: () => Promise.reject(new Error('no backend')),
                   location: { protocol, search, pathname: '/' },
                   document: { querySelector: sel => (sel === 'meta[name="desk-token"]' ? { getAttribute: () => tok } : null) } };
      c6.window = c6; vm.createContext(c6);
      for (const f of ['contract.js', 'core.js', 'store.js', 'sim.js', 'transport.js']) vm.runInContext(fs.readFileSync(path.join(JS, f), 'utf8'), c6, { filename: f });
      return c6.Desk.transport.choose();
    };
    eq(await mk('file:', '', ''), 'demo', 'file:// with no token is demo');
    eq(await mk('http:', '?demo=1', ''), 'demo', '?demo=1 with no token is demo');
    eq(await mk('http:', '', ''), 'live', 'http with no token and no ?demo=1 is live (a backend that is down shows SRV DOWN, never the simulation)');
    eq(await mk('http:', '?demo=1', 'tok-X'), 'live', 'a page the backend served (a token) is live even with ?demo=1');
  }

  if (process.env.V) console.log('section 11');
  // ---- 11. THE UNIVERSE: everything settling today and tomorrow, paged, never streamed ---------
  {
    let tu = Date.UTC(2026, 9, 6, 17, 30, 0);          // 13:30 ET
    const ue = [];
    const su = D.sim.create({ now: () => tu, emit: e => ue.push(e), seed: 7, sid: 'universe' });
    const runU = ms => { const end = tu + ms; while (tu < end) { tu = Math.min(end, Math.max(tu + 1, Math.min(su.nextDue(), tu + 100))); su.pump(); } };
    const H = { 'X-Desk': su.token };
    const get = q => { let o = null; su.request('GET', '/api/universe?' + q, undefined, (st2, j) => { o = { s: st2, j }; }, H); runU(30); return o; };
    const post = (p2, b) => { let o = null; su.request('POST', p2, b, (st2, j) => { o = { s: st2, j }; }, H); runU(60); return o; };
    runU(1000);
    const listed = su.universe().length + su.markets().length;
    check(listed > 2500 && listed < 8000, 'the demo lists a few thousand markets for today and tomorrow', listed);
    const live0 = su.markets().length;
    check(live0 < 80, 'of which only the desk\'s own are live (mkt events, books)', live0);
    const rT = get('day=T&limit=200'), rT1 = get('day=T1&limit=200');
    for (const r of [rT, rT1]) D.contract.validateResponse('GET /api/universe', r.s, r.j).forEach(e => check(false, 'universe reply: ' + e));
    check(rT.s === 200 && rT.j.total > 1000 && rT1.j.total > 800, 'T and T+1 each list well over a thousand markets', [rT.j.total, rT1.j.total]);
    check(rT.j.date === '2026-10-06' && rT1.j.date === '2026-10-07', 'T is today\'s ET date, T+1 tomorrow\'s', [rT.j.date, rT1.j.date]);
    check(C.enums.CAT.every(c => rT.j.cats[c] > 0), 'every category has markets today', rT.j.cats);
    eq(rT.j.groups.reduce((t, g) => t + g.n, 0), rT.j.total, 'the groups add up to the total');
    eq(rT.j.groups.map(g => g.cat).join(','), rT.j.groups.map(g => g.cat).slice().sort((a, b) => C.enums.CAT.indexOf(a) - C.enums.CAT.indexOf(b)).join(','), 'groups come category by category (SPORTS WEATHER FIN ECON OTHER)');
    const leagues = new Set(rT.j.groups.filter(g => g.cat === 'SPORTS').map(g => g.gk.split(' ')[0]));
    check(['MLB', 'NHL', 'NBA', 'NCAAF', 'UCL'].every(l => leagues.has(l)), 'sports group by league and game', [...leagues]);
    check(rT1.j.groups.some(g => /^ATP /.test(g.gk)) && !leagues.has('ATP'), 'Shanghai\'s tennis plays overnight ET: today\'s matches are closed at 13:30, tomorrow\'s are listed');
    check(rT.j.groups.some(g => g.gk === '15M') && rT.j.groups.some(g => / 1H /.test(g.gk)), 'FIN lists the 15-minute windows and the hourly events', rT.j.groups.filter(g => g.cat === 'FIN').map(g => g.gk).slice(0, 6));
    check(rT.j.groups.some(g => /^GAS /.test(g.gk)) && rT.j.groups.some(g => /^RATES /.test(g.gk)), 'ECON lists daily gas and rates');
    // Polymarket US: mainly sports and weather
    const pm = get('day=T&v=P&limit=200');
    check(pm.j.total > 50 && pm.j.cats.SPORTS + pm.j.cats.WEATHER === pm.j.total, 'Polymarket US lists sports and weather', pm.j.cats);
    // paging: the second page continues the first, in the same order
    const p0 = get('day=T&sort=vol&offset=0&limit=50'), p1 = get('day=T&sort=vol&offset=50&limit=50'), pAll = get('day=T&sort=vol&offset=0&limit=100');
    eq(p0.j.rows.concat(p1.j.rows).map(r => r.tk).join(), pAll.j.rows.map(r => r.tk).join(), 'pages line up: offset 50 continues offset 0');
    // sorts
    const inGroups = (rows, f) => { let ok = true; for (let i = 1; i < rows.length; i++) if (rows[i].gk === rows[i - 1].gk && !f(rows[i - 1], rows[i])) ok = false; return ok; };
    check(inGroups(pAll.j.rows, (a, b) => a.vol >= b.vol), 'sort=vol: inside a group the busiest first');
    const cl = get('day=T&sort=close&limit=200');
    check(inGroups(cl.j.rows, (a, b) => a.close_ms <= b.close_ms), 'sort=close: inside a group the soonest first');
    const sp = get('day=T&cat=WEATHER&sort=sprd&limit=200'), spv = r => (r.sprd === null ? Infinity : +r.sprd);
    check(inGroups(sp.j.rows, (a, b) => spv(a) <= spv(b)), 'sort=sprd: tightest first, a one-sided book last');
    const ed = get('day=T&cat=WEATHER&sort=edge&limit=200'), edv = r => (r.edge === null ? -Infinity : +r.edge);
    check(ed.j.rows.some(r => r.edge !== null) && inGroups(ed.j.rows, (a, b) => edv(a) >= edv(b)), 'sort=edge: the best take after the fee first (rows with a model fair)');
    // filters
    const wx = get('day=T&cat=WEATHER&limit=200');
    check(wx.j.rows.every(r => r.cat === 'WEATHER') && wx.j.total === rT.j.cats.WEATHER, 'cat filters, and the cats counts say how many it will find');
    const qd = get('day=T&q=den&limit=200');
    check(qd.j.total > 0 && qd.j.rows.every(r => (r.tk + r.title + r.short + r.gk).toLowerCase().indexOf('den') >= 0), 'q searches tickers, titles and groups');
    for (const bad of ['day=X', 'day=T&limit=500', 'day=T&sort=price', 'day=T&cat=CRYPTO', 'cat=FIN']) {
      const b = get(bad);
      check(b.s === 400 && b.j.err.code === 'BAD_REQUEST', 'a query outside the contract is refused: ' + bad, b.j);
    }
    // pin a listing row: it joins the live set (mkt with pin true, then its book)
    const row = wx.j.rows.find(r => !r.live && r.v === 'K');
    const n0 = ue.length;
    const w = post('/api/watch', { v: row.v, tk: row.tk, on: true });
    check(w.s === 200 && w.j.ok && w.j.pin === true, 'POST /api/watch pins it', w.j);
    D.contract.validateResponse('POST /api/watch', w.s, w.j).forEach(e => check(false, 'watch reply: ' + e));
    const mk = ue.slice(n0).filter(e => e.type === 'mkt' && e.data.tk === row.tk);
    check(mk.length === 1 && mk[0].data.pin === true && mk[0].data.cat === 'WEATHER' && mk[0].data.date === '2026-10-06', 'a mkt event lists it, pinned, with its category and date', mk.map(e => e.data));
    runU(500);
    check(ue.slice(n0).some(e => e.type === 'book' && e.data.tk === row.tk), 'and its book streams (TOP: every live market\'s top)');
    check(get('day=T&q=' + row.tk).j.rows[0].live && get('day=T&q=' + row.tk).j.rows[0].pin, 'the listing now says live and pinned');
    const uw = post('/api/watch', { v: row.v, tk: row.tk, on: false });
    check(uw.j.pin === false && su.market(row.v + '|' + row.tk).pin === false && ue.filter(e => e.type === 'mkt' && e.data.tk === row.tk).pop().data.pin === false, 'unpinning sends mkt.pin false');
    const unk = post('/api/watch', { v: 'K', tk: 'KXNOSUCH-1', on: true });
    check(unk.s === 404 && unk.j.err.code === 'UNKNOWN_MARKET', 'pinning an unknown market is refused', unk.j);
    // a ladder on a listing row brings it live without pinning it
    const lr = get('day=T1&cat=SPORTS&limit=50').j.rows.find(r => !r.live && r.v === 'K');
    const sub = post('/api/subscribe', { sid: 'universe', ladders: [{ v: lr.v, tk: lr.tk }], tops: 'ALL' });
    runU(300);
    check(sub.j.ok && su.market(lr.v + '|' + lr.tk) && su.market(lr.v + '|' + lr.tk).pin === false && ue.some(e => e.type === 'book' && e.data.tk === lr.tk && e.data.depth === 'FULL'),
      'a ladder subscription on a listing row brings it live at full depth, unpinned', sub.j);
    // hb and the snapshot carry today's ET date; every event and the snapshot stay in contract
    check(ue.filter(e => e.type === 'hb').every(e => e.data.date === '2026-10-06'), 'hb.date is today (ET)');
    eq(su.snapshot().date, '2026-10-06', 'snapshot.date too');
    eq(ue.filter(e => D.contract.validateEvent(e).length).length, 0, 'every event stays in contract');
    // only live markets stream: book events name live markets only
    const booked = new Set(ue.filter(e => e.type === 'book').map(e => e.data.v + '|' + e.data.tk));
    check([...booked].every(k => su.market(k)), 'no listing row streams a book (only the live set does)');
    // an exclusive event settles one way: two brackets of one weather event, pinned, to the close
    const ev = wx.j.rows.find(r => !r.live && /^KXHIGH/.test(r.tk) && wx.j.rows.filter(x => x.tk.split('-').slice(0, 2).join('-') === r.tk.split('-').slice(0, 2).join('-') && !x.live).length >= 3);
    const evk = ev.tk.split('-').slice(0, 2).join('-'), sibs = wx.j.rows.filter(x => x.tk.indexOf(evk + '-') === 0 && !x.live);
    sibs.forEach(r => post('/api/watch', { v: r.v, tk: r.tk, on: true }));
    runU(sibs[0].close_ms - tu + 2000);
    const res = sibs.map(r => su.market('K|' + r.tk)).map(m => m.status + ':' + m.result);
    check(res.every(x => /^SETTLED:/.test(x)) && res.filter(x => /YES$/.test(x)).length <= 1, 'pinned brackets of one event settle at the close, at most one YES', res);
  }

  if (process.env.V) console.log('section 10');
  // ---- 10. the palette: near-black surfaces, ink that still reads on every one -----------------
  {
    const css = fs.readFileSync(path.join(JS, '..', 'desk.css'), 'utf8');
    const root = /:root\s*\{([\s\S]*?)\n\}/.exec(css)[1];
    const tok = {};
    for (const m of root.matchAll(/--([\w-]+):\s*(#[0-9A-Fa-f]{6})/g)) tok[m[1]] = m[2];
    const lum = hex => {
      const c = [1, 3, 5].map(i => parseInt(hex.slice(i, i + 2), 16) / 255).map(x => (x <= 0.03928 ? x / 12.92 : Math.pow((x + 0.055) / 1.055, 2.4)));
      return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
    };
    const ratio = (a, b) => { const x = lum(tok[a]), y = lum(tok[b]); return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05); };
    check(lum(tok.bg0) < 0.001, 'the gutters, header and status bar are near-black (#000)', tok.bg0);
    check(lum(tok.bg1) < 0.004 && lum(tok.bg1) > lum(tok.bg0), 'panels sit one step above the black', tok.bg1);
    check(lum(tok.bg0) < lum(tok.bg1) && lum(tok.bg1) < lum(tok.bg2) && lum(tok.bg2) < lum(tok.bg3), 'surfaces step up bg0 < bg1 < bg2 < bg3');
    check(ratio('line', 'bg1') >= 1.25 && ratio('line', 'bg0') >= 1.3, 'hairlines still separate panels on black', [ratio('line', 'bg1').toFixed(2), ratio('line', 'bg0').toFixed(2)]);
    check(/\.tabroot \{[^}]*background: var\(--line\)/.test(css), 'the 1 px gaps between panels are drawn in the hairline colour');
    const bad = [];
    ['tx0', 'tx1', 'tx2'].forEach(t => ['bg0', 'bg1', 'bg2', 'bg3', 'sel'].forEach(b => { if (ratio(t, b) < 4.5) bad.push(t + '/' + b + ' ' + ratio(t, b).toFixed(2)); }));
    ['bid-tx', 'ask-tx', 'up', 'dn', 'warn', 'fair', 'espn'].forEach(t => ['bg0', 'bg1', 'bg2'].forEach(b => { if (ratio(t, b) < 4.5) bad.push(t + '/' + b + ' ' + ratio(t, b).toFixed(2)); }));
    eq(bad.join(', '), '', 'every ink keeps at least 4.5:1 on every surface it sits on');
  }

  console.log(fails ? `${fails} of ${checks} checks FAILED` : `ui desk v3: ${checks} checks, 0 failures`);
  process.exit(fails ? 1 : 0);
})().catch(e => { console.log('FAIL: test crashed: ' + (e && e.stack || e)); process.exit(1); });
