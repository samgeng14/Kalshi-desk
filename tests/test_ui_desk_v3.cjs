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
  }

  if (process.env.V) console.log('section 4');
  // ---- 4. the store applies every event type -----------------------------------------------
  const st = D.store, EX = C.examples, T0 = 1791306000000;
  const KEY = 'K|KXNATGAS15M-26OCT061930-30';
  const baseSnap = () => ({
    epoch: EX.hb.epoch, sid: EX.hb.sid, seq: 100, srv_ms: T0, hdr: EX.hdr, markets: [EX.mkt], books: [EX.book],
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
  check(!st.resyncing && !st.stale, 'snapshot ends the resync and the stale state', st.staleWhy);
  let seq = 100;
  const env = (type, data) => ({ type, seq: ++seq, ts_ms: T0 + seq, data: JSON.parse(JSON.stringify(data)) });
  const ap = (type, data) => st.apply(env(type, data));
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
  check(o.st === 'PART' && o.px === 6300 && o.left === 8 && o.edge === 300 && !o.terminal, 'ord fields', o);
  eq(st.workingAt(KEY).BUY.get(6300).left, 8, 'workingAt aggregates our orders by level');
  eq(st.working().length, 1, 'working lists non-terminal orders');
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
  const m = st.markouts();
  check(m.n === 1 && m.m10 === 50 && m.n60 === 0, 'markout stats over fills', m);
  eq(st.priceGuard(KEY, 'BUY', 6600), '', 'price guard: 2 ticks through the ask is allowed');
  check(/^PRICE_BAND/.test(st.priceGuard(KEY, 'BUY', 6700)), 'price guard: 3 ticks through the ask is refused');
  check(/^PRICE_BAND/.test(st.priceGuard(KEY, 'SELL', 6000)), 'price guard: 3 ticks through the bid is refused');
  eq(st.canTrade('K'), '', 'trading allowed while live');
  // terminal orders never change again
  const term = JSON.parse(JSON.stringify(EX.ord)); term.st = 'CXL'; term.why = 'USER'; term.left = 0; term.t.done = T0 + 1300000;
  ap('ord', term);
  const back = JSON.parse(JSON.stringify(EX.ord));
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
  check(rj.st === 'REJ' && rj.why === 'PRICE_BAND' && rj.terminal, 'a refused order shows REJ with the reason', rj);
  check(/^REJ PRICE_BAND/.test(st.alerts[0].msg), 'the reason goes to the status bar');
  st.pend({ cid: 'Mtestabc-2', v: 'K', tk: EX.mkt.tk, side: 'BUY', px: 6300, qty: 1, tif: 'POST', ttl_s: 15, touch: 6300, t_click_ms: T0 });
  st.reply('Mtestabc-2', 0, { ok: false, err: { code: 'EXCHANGE', msg: 'desk unreachable' } });
  eq(st.orders.get('Mtestabc-2').st, 'PEND', 'no answer leaves the order PEND (state unknown), never REJ');

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
    eq(st.apply({ type: 'alert', seq: k + 4, ts_ms: T0, data: Object.assign({}, EX.alert, { id: 'a-913', msg: 'after the snapshot' }) }), 'buffered', 'and kept');
    await flushMicro();
    check(!st.resyncing && !st.stale, 'the snapshot ends the resync');
    eq(st.seq, k + 4, 'buffered messages newer than the snapshot are replayed in order');
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

  if (process.env.V) console.log('section 6');
  // ---- 6. the demo venues: matching rules ---------------------------------------------------
  let t = Date.UTC(2026, 9, 6, 23, 7, 13);
  const evs = [];
  const sim = D.sim.create({ now: () => t, emit: e => evs.push(e), seed: 7, sid: 'testsid1' });
  const replies = [];
  function run(ms) { const end = t + ms; while (t < end) { t = Math.min(end, Math.max(t + 1, Math.min(sim.nextDue(), t + 100))); sim.pump(); } }
  function call(method, p, body, ms) {
    let out = null;
    sim.request(method, p, body, (s, j) => { out = { s, j }; replies.push([method, p, s, j]); });
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
  eq(ngFill.fee, '0.0000', 'the LIP 15-minute series are fee-free');
  r = order(pm, 'BUY', 5000, 1.5, 'IOC');
  check(r.s === 400 && r.j.err.code === 'BAD_QTY', 'Polymarket takes whole contracts only');

  // kill lockout and re-arm
  r = order(ng, 'BUY', 6000, 5, 'POST', 60);
  const k1 = call('POST', '/api/kill', {});
  check(k1.j.ok && k1.j.run.state === 'KILLED' && k1.j.run.rearm_ms === k1.j.run.since_ms + 60000, 'KILL stops everything for 60 s', k1.j);
  check(sim.order(r.j.ord.cid).st === 'CXL' && sim.order(r.j.ord.cid).why === 'KILL', 'KILL cancels working orders');
  r = order(ng, 'BUY', 6000, 5, 'IOC');
  check(r.s === 409 && r.j.err.code === 'KILLED', 'new orders are refused during the lockout');
  const s1 = call('POST', '/api/start', {});
  check(s1.s === 409 && s1.j.err.code === 'KILLED', 'start is refused during the lockout');
  const re1 = call('POST', '/api/rearm', {});
  check(re1.s === 409 && re1.j.err.code === 'BAD_STATE', 're-arm is refused before 60 s');
  run(60000);
  const re2 = call('POST', '/api/rearm', {});
  check(re2.s === 200 && re2.j.run.state === 'STOP', 're-arm after 60 s returns to STOP');
  r = order(ng, 'BUY', 6000, 5, 'IOC');
  eq(r.s, 200, 'orders are accepted again after re-arm');

  if (process.env.V) console.log('section 7');
  // ---- 7. a long run: every event and reply validates against the contract ------------------
  call('POST', '/api/subscribe', { sid: 'testsid1', ladders: [{ v: ng.v, tk: ng.tk }, { v: pm.v, tk: pm.tk }], tops: 'ALL' });
  call('POST', '/api/lane/X1', { on: true });
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
    sim3.request('POST', '/api/start', {}, () => {});
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

  console.log(fails ? `${fails} of ${checks} checks FAILED` : `ui desk v3: ${checks} checks, 0 failures`);
  process.exit(fails ? 1 : 0);
})().catch(e => { console.log('FAIL: test crashed: ' + (e && e.stack || e)); process.exit(1); });
