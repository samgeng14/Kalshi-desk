// The v3 trading screen's MANUAL tab (ui/desk/js/manual.js), checked without a browser or any
// package:
//   1. the pure logic: ladder rows and buckets, the click and hotkey rules (post away from the
//      touch, IOC at or through it with the slippage cap), working-cell text, the break-even band,
//      foreign cids, ticket price/qty parsing, cost lines, the watchlist's groups
//   2. the whole page (index.html parsed into a small fake DOM, every script loaded in a vm, the
//      demo venues on a fake clock): ladder clicks, the fat-finger guard and its second click,
//      right-click cancel, drag re-price, hotkeys (off at load), the ticket with its OVERRIDE
//      second click, working / fills / positions with FLAT, takeover and release, read-only
//      Polymarket, stale and KILL lockouts, WIDE mode's third ladder, the 15-minute roll, and
//      zero render errors or contract violations throughout
//   node tests/test_ui_desk_v3_manual.cjs
'use strict';
const fs = require('fs'), path = require('path'), vm = require('vm');
const UI = path.join(__dirname, '..', 'ui', 'desk');
const JS = path.join(UI, 'js');

let checks = 0, fails = 0;
function check(cond, what, detail) {
  if (process.env.V) console.log('  .. ' + what);
  checks++;
  if (!cond) { fails++; console.log('FAIL: ' + what + (detail !== undefined ? '  ' + (typeof detail === 'string' ? detail : JSON.stringify(detail)) : '')); }
}
function eq(a, b, what) { check(a === b, what, { got: a, want: b }); }
const M = '−';

// ---------------------------------------------------------------------------------------------
// A small DOM: enough for the page's scripts (elements, attributes, classList, dataset, style,
// events with capture and bubbling, focus, closest with simple selectors)
// ---------------------------------------------------------------------------------------------
function makeDom() {
  const ids = new Map();
  function simpleMatch(e, sel) {
    const m = /^([a-zA-Z0-9]*)((?:\.[\w-]+)*)((?:\[[\w-]+(?:="[^"]*")?\])*)$/.exec(sel.trim());
    if (!m) throw new Error('fake DOM: unsupported selector ' + sel);
    if (m[1] && e.tagName !== m[1].toUpperCase()) return false;
    if (m[2]) for (const c of m[2].split('.').filter(Boolean)) if (!e.classList.contains(c)) return false;
    if (m[3]) for (const a of m[3].match(/\[[^\]]+\]/g)) {
      const [, k, v] = /^\[([\w-]+)(?:="([^"]*)")?\]$/.exec(a);
      if (!e.hasAttribute(k)) return false;
      if (v !== undefined && e.getAttribute(k) !== v) return false;
    }
    return true;
  }
  class ClassList {
    constructor(e) { this.e = e; }
    get set() { return new Set(String(this.e.className || '').split(/\s+/).filter(Boolean)); }
    contains(c) { return this.set.has(c); }
    add(c) { const s = this.set; s.add(c); this.e.className = [...s].join(' '); }
    remove(c) { const s = this.set; s.delete(c); this.e.className = [...s].join(' '); }
    toggle(c, on) { const want = on === undefined ? !this.contains(c) : !!on; if (want) this.add(c); else this.remove(c); return want; }
  }
  class Style {
    constructor() { this._p = {}; }
    setProperty(k, v) { this._p[k] = String(v); }
    getPropertyValue(k) { return this._p[k] || ''; }
  }
  class El {
    constructor(tag) {
      this.tagName = tag.toUpperCase(); this.nodeName = this.tagName; this._ch = []; this.parentNode = null;
      this._attrs = new Map(); this._text = ''; this._ls = []; this.style = new Style(); this.classList = new ClassList(this);
      this.value = ''; this.disabled = false; this._h = 0;
      const self = this;
      this.dataset = new Proxy({}, {
        get(t, k) { const v = self.getAttribute('data-' + String(k).replace(/[A-Z]/g, x => '-' + x.toLowerCase())); return v === null ? undefined : v; },
        set(t, k, v) { self.setAttribute('data-' + String(k).replace(/[A-Z]/g, x => '-' + x.toLowerCase()), v); return true; }
      });
    }
    get children() { return this._ch; }
    get firstChild() { return this._ch[0] || null; }
    get lastChild() { return this._ch[this._ch.length - 1] || null; }
    get nextSibling() { if (!this.parentNode) return null; const a = this.parentNode._ch; return a[a.indexOf(this) + 1] || null; }
    get textContent() { return this._ch.length ? this._ch.map(c => c.textContent).join('') : this._text; }
    set textContent(v) { this._ch.forEach(c => { c.parentNode = null; }); this._ch = []; this._text = String(v); }
    get innerHTML() { return this.textContent; }
    set innerHTML(v) { this.textContent = String(v).replace(/<[^>]*>/g, ''); }
    get className() { return this.getAttribute('class') || ''; }
    set className(v) { this.setAttribute('class', v); }
    get id() { return this.getAttribute('id') || ''; }
    get title() { return this.getAttribute('title') || ''; }
    get clientHeight() { return this._h; }
    get isContentEditable() { return false; }
    getAttribute(k) { return this._attrs.has(k) ? this._attrs.get(k) : null; }
    setAttribute(k, v) { this._attrs.set(k, String(v)); if (k === 'id') ids.set(String(v), this); }
    hasAttribute(k) { return this._attrs.has(k); }
    removeAttribute(k) { this._attrs.delete(k); }
    appendChild(c) { if (c.parentNode) c.parentNode.removeChild(c); c.parentNode = this; this._ch.push(c); return c; }
    insertBefore(c, ref) {
      if (c.parentNode) c.parentNode.removeChild(c);
      c.parentNode = this;
      const i = ref ? this._ch.indexOf(ref) : -1;
      if (i < 0) this._ch.push(c); else this._ch.splice(i, 0, c);
      return c;
    }
    removeChild(c) { const i = this._ch.indexOf(c); if (i >= 0) this._ch.splice(i, 1); c.parentNode = null; return c; }
    remove() { if (this.parentNode) this.parentNode.removeChild(this); }
    contains(x) { while (x) { if (x === this) return true; x = x.parentNode; } return false; }
    matches(sel) { return sel.split(',').some(s => simpleMatch(this, s)); }
    closest(sel) { let e = this; while (e && e.tagName) { if (e.matches(sel)) return e; e = e.parentNode; } return null; }
    addEventListener(type, fn, opts) { this._ls.push({ type, fn, capture: opts === true || !!(opts && opts.capture) }); }
    removeEventListener(type, fn) { this._ls = this._ls.filter(l => !(l.type === type && l.fn === fn)); }
    focus() { doc.activeElement = this; dispatch(this, 'focusin', {}); }
    blur() { if (doc.activeElement === this) doc.activeElement = doc.body; }
    select() {}
    animate() { return {}; }
    scrollIntoView() {}
    get offsetWidth() { return 0; }
  }
  const doc = new El('#document');
  doc.readyState = 'interactive'; doc.hidden = false;
  doc.createElement = t => new El(t);
  doc.getElementById = id => { const e = ids.get(id); return e && doc.contains(e) ? e : null; };
  function dispatch(target, type, props) {
    const ev = Object.assign({
      type, target, bubbles: true, defaultPrevented: false, timeStamp: performance.now(), button: 0,
      shiftKey: false, altKey: false, metaKey: false, ctrlKey: false,
      preventDefault() { this.defaultPrevented = true; }, stopPropagation() { this._stop = true; }
    }, props || {});
    const pathUp = [];
    for (let e = target; e; e = e.parentNode) pathUp.push(e);
    if (pathUp[pathUp.length - 1] !== doc) pathUp.push(doc);
    for (let i = pathUp.length - 1; i >= 0 && !ev._stop; i--) for (const l of pathUp[i]._ls.slice()) if (l.type === type && l.capture) { ev.currentTarget = pathUp[i]; l.fn(ev); }
    for (let i = 0; i < pathUp.length && !ev._stop; i++) for (const l of pathUp[i]._ls.slice()) if (l.type === type && !l.capture) { ev.currentTarget = pathUp[i]; l.fn(ev); }
    return ev;
  }
  // parse index.html (the page's own markup, not a copy)
  const html = fs.readFileSync(path.join(UI, 'index.html'), 'utf8');
  const VOID = new Set(['meta', 'link', 'input', 'br', 'col', 'img']);
  const root = new El('html'); doc.appendChild(root); doc.documentElement = root;
  let cur = root;
  const re = /<!--[\s\S]*?-->|<!doctype[^>]*>|<(\/?)([a-zA-Z0-9]+)([^>]*?)(\/?)>|([^<]+)/gi;
  let m;
  while ((m = re.exec(html))) {
    if (m[0].startsWith('<!')) continue;
    if (m[5] !== undefined) { const t = m[5]; if (t.trim() && !cur._ch.length && cur.tagName !== 'SCRIPT' && cur.tagName !== 'STYLE') cur._text += t.trim(); continue; }
    const tag = m[2].toLowerCase();
    if (m[1]) { if (cur.tagName === tag.toUpperCase() && cur.parentNode) cur = cur.parentNode; continue; }
    if (tag === 'html') { for (const a of m[3].matchAll(/([\w-]+)(?:="([^"]*)")?/g)) root.setAttribute(a[1], a[2] === undefined ? '' : a[2]); continue; }
    const e = new El(tag);
    for (const a of m[3].matchAll(/([\w-]+)(?:="([^"]*)")?/g)) e.setAttribute(a[1], a[2] === undefined ? '' : a[2]);
    cur.appendChild(e);
    if (tag === 'body') doc.body = e;
    if (!VOID.has(tag) && !m[4]) cur = e;
  }
  doc.activeElement = doc.body;
  return { doc, dispatch, El };
}

// ---------------------------------------------------------------------------------------------
// The page in a vm on a fake clock
// ---------------------------------------------------------------------------------------------
function bootPage(opts) {
  opts = opts || {};
  const { doc, dispatch } = makeDom();
  const clock = { t: opts.t0 || Date.UTC(2026, 9, 6, 23, 31, 0) };
  const mq = {};
  const matchMedia = q => {
    if (!mq[q]) mq[q] = { matches: q.indexOf('1920') >= 0 ? !!opts.wide : (q.indexOf('2400') >= 0 ? !!opts.xwide : false), ls: [], addEventListener(t, f) { this.ls.push(f); } };
    return mq[q];
  };
  const rafs = [];
  const errors = [];
  const mem = {};
  const ctx = {
    console: { log: console.log, warn: console.warn, error: (...a) => { errors.push(a.map(x => (x && x.stack) || String(x)).join(' ')); } },
    setTimeout, clearTimeout, setInterval, clearInterval, setImmediate, performance, Promise, Map, Set, WeakSet, Uint8Array,
    document: doc, location: { protocol: 'file:', search: opts.search || '' },
    localStorage: { getItem(k) { return k in mem ? mem[k] : null; }, setItem(k, v) { mem[k] = String(v); } },
    matchMedia, requestAnimationFrame: f => { rafs.push(f); }, confirm: () => true, crypto: require('crypto').webcrypto,
    AbortController, fetch: () => Promise.reject(new Error('no network in tests')), __clock: clock
  };
  ctx.window = ctx;
  ctx.addEventListener = () => {};
  vm.createContext(ctx);
  vm.runInContext('Date.now = function () { return __clock.t; };', ctx);
  const load = f => vm.runInContext(fs.readFileSync(path.join(JS, f), 'utf8'), ctx, { filename: f });
  ['contract.js', 'core.js', 'store.js', 'sim.js', 'transport.js', 'header.js', 'manual.js', 'systematic.js'].forEach(load);
  const D = ctx.Desk;
  let sim = null;
  const create = D.sim.create;
  D.sim.create = o => { sim = create(Object.assign({}, o, { now: () => clock.t, seed: opts.seed || 11, pmRO: !!opts.pmRO })); sim.start = () => {}; return sim; };
  load('main.js');
  D.tick.stop();
  let lastTick = clock.t;
  const frame = () => { const fs2 = rafs.splice(0); fs2.forEach(f => f(performance.now())); };
  const flush = async () => { await new Promise(r => setImmediate(r)); await new Promise(r => setTimeout(r, 0)); };
  async function run(ms, step) {
    step = step || 50;
    const end = clock.t + ms;
    do {
      clock.t = Math.min(end, clock.t + step);
      if (sim) sim.pump();
      if (clock.t - lastTick >= 250) { lastTick = clock.t; D.tick.run(); }
      await flush();
      frame();
    } while (clock.t < end);
  }
  const $ = id => doc.getElementById(id);
  const fire = (el, type, props) => dispatch(el, type, props);
  function focusable(el) { for (let e = el; e && e.tagName; e = e.parentNode) if (e.hasAttribute && (e.hasAttribute('tabindex') || /^(INPUT|BUTTON)$/.test(e.tagName))) return e; return null; }
  function click(el, props) {
    props = props || {};
    fire(el, 'mousedown', props);
    const f = focusable(el); if (f && f.tagName !== 'BUTTON') f.focus();
    fire(el, 'mouseup', props);
    return fire(el, 'click', props);
  }
  function key(code, k, extra) {
    const t = doc.activeElement || doc.body;
    return fire(t, 'keydown', Object.assign({ code, key: k }, extra || {}));
  }
  function setWide(on) {
    const q = mq['(min-width: 1920px)'];
    q.matches = on; q.ls.forEach(f => f());
  }
  // an external monitor: 2400 px and wider (WIDE as well)
  function setXWide(on) {
    const w = mq['(min-width: 1920px)'], x = mq['(min-width: 2400px)'];
    w.matches = on; x.matches = on; w.ls.forEach(f => f());
  }
  return { ctx, D, doc, $, fire, click, key, run, frame, flush, clock, errors, sim: () => sim, setWide, setXWide, mem };
}

// helpers on the rendered DOM
const rowsOf = tb => tb.children.filter(r => r.getAttribute('data-k') !== null || r.getAttribute('data-mkt') !== null || r.getAttribute('data-grp') !== null);
function ladRow(P, slot, px) {
  const tb = P.D.manual.state().ladders[slot].tb;
  return tb.children.find(r => r.getAttribute('data-k') === String(px)) || null;
}
const COL = { wb: 0, bq: 1, rf: 2, px: 3, aq: 4, ws: 5, lt: 6 };
function ladCell(P, slot, px, col) { const r = ladRow(P, slot, px); return r ? r.children[COL[col]] : null; }
function newest(P, pred) { let best = null; P.D.store.orders.forEach(o => { if ((!pred || pred(o)) && (!best || (o.local && best.local ? o.local.at >= best.local.at : true))) best = o; }); return best; }
function manOrders(P, key) { const a = []; P.D.store.orders.forEach(o => { if (o.own === 'MAN' && (!key || o.key === key)) a.push(o); }); return a; }

(async () => {
  // =============================================================================================
  // 1. pure logic
  // =============================================================================================
  {
    const ctx = { console, setTimeout, clearTimeout, setInterval, clearInterval, performance, localStorage: { getItem() { return null; }, setItem() {} } };
    ctx.window = ctx; vm.createContext(ctx);
    for (const f of ['contract.js', 'core.js', 'store.js', 'manual.js']) vm.runInContext(fs.readFileSync(path.join(JS, f), 'utf8'), ctx, { filename: f });
    const D = ctx.Desk, L = D.manual.logic, G = D.grid;
    check(typeof D.manual.init === 'function' && typeof D.manual.render === 'function', 'Desk.manual keeps the {init, render} contract');
    const lin = G.make([{ start: '0.0000', end: '1.0000', step: '0.0100' }]);
    const deci = G.make([{ start: '0.0000', end: '1.0000', step: '0.0010' }]);
    const tap = G.make([{ start: '0.0000', end: '0.1000', step: '0.0010' }, { start: '0.1000', end: '0.9000', step: '0.0100' }, { start: '0.9000', end: '1.0000', step: '0.0010' }]);
    eq(L.rowLevels(lin, 1).length, 99, 'a cent grid ladder has 99 rows');
    eq(L.rowLevels(deci, 10).length, 99, 'a 0.1c grid zoomed x10 has one row per cent');
    eq(L.rowLevels(deci, 5).length, 199, 'a 0.1c grid zoomed x5 has one row per half cent');
    eq(L.rowLevels(tap, 10).length, 99, 'a tapered grid zoomed x10 keeps every cent in the middle');
    const z = L.rowLevels(deci, 10);
    eq(L.floorRow(z, 6430), 6400, 'zoomed: a 0.643 bid shows on the 0.64 row');
    eq(L.ceilRow(z, 6430), 6500, 'zoomed: a 0.643 ask shows on the 0.65 row');
    eq(L.ceilRow(z, 6400), 6400, 'ceilRow of a row price is itself');
    eq(z[L.nearestIdx(z, 6460)], 6500, 'nearest row');
    const bk = L.bucket([[6430, 5], [6410, 2.5], [6300, 1]], z, false);
    eq(bk.get(6400), 7.5, 'bid sizes sum into their floor row');
    eq(bk.get(6300), 1, 'other rows keep their size');
    // the click rules (plan section 6): away from the touch posts, at or through it takes
    let it = L.clickIntent(lin, 6300, 6400, 'BUY', 6200, 1, 15);
    check(it.tif === 'POST' && it.px === 6200 && it.ttl_s === 15 && it.touch === 6300, 'BUY below the ask posts at the clicked price with the expiry', it);
    it = L.clickIntent(lin, 6300, 6400, 'BUY', 6400, 1, 15);
    check(it.tif === 'IOC' && it.px === 6500 && it.ttl_s === null && it.touch === 6400, 'BUY at the ask: IOC up to one tick more (the slippage cap)', it);
    it = L.clickIntent(lin, 6300, 6400, 'BUY', 6400, 0, 15);
    eq(it.px, 6400, 'cap 0: IOC exactly at the clicked price');
    it = L.clickIntent(lin, 6300, 6400, 'SELL', 6300, 2, 5);
    check(it.tif === 'IOC' && it.px === 6100 && it.touch === 6300, 'SELL at the bid: IOC down to two ticks less', it);
    it = L.clickIntent(lin, 6300, 6400, 'SELL', 6400, 1, 5);
    check(it.tif === 'POST' && it.px === 6400 && it.ttl_s === 5, 'SELL at the ask joins the offer', it);
    it = L.clickIntent(lin, 6300, 6400, 'SELL', 6350 + 50, 1, 60);
    eq(it.tif, 'POST', 'SELL above the bid posts');
    it = L.clickIntent(lin, null, null, 'BUY', 9900, 1, 15);
    eq(it.tif, 'POST', 'an empty book never takes');
    it = L.clickIntent(lin, 6300, 6400, 'BUY', 9900, 1, 15);
    eq(it.px, 9900, 'the cap clamps at the grid end');
    // hotkeys
    it = L.hotIntent('take', lin, 6300, 6400, 'BUY', 1, 15);
    check(it.tif === 'IOC' && it.px === 6500 && it.touch === 6400, 'B: IOC at the ask plus the cap', it);
    it = L.hotIntent('take', lin, 6300, 6400, 'SELL', 1, 15);
    check(it.tif === 'IOC' && it.px === 6200, 'S: IOC at the bid minus the cap', it);
    it = L.hotIntent('join', lin, 6300, 6400, 'BUY', 1, 60);
    check(it.tif === 'POST' && it.px === 6300 && it.ttl_s === 60, 'Shift+B joins the bid with the ladder expiry', it);
    it = L.hotIntent('join', lin, 6300, 6400, 'SELL', 1, 60);
    eq(it.px, 6400, 'Shift+S joins the offer');
    eq(L.hotIntent('improve', lin, 6300, 6400, 'BUY', 1, 15).err, 'SPREAD 1 TICK', 'Alt+B refuses a 1-tick spread');
    it = L.hotIntent('improve', lin, 6200, 6400, 'BUY', 1, 15);
    check(it.tif === 'POST' && it.px === 6300, 'Alt+B betters the bid by one tick on a 2-tick spread', it);
    it = L.hotIntent('improve', lin, 6200, 6400, 'SELL', 1, 15);
    eq(it.px, 6300, 'Alt+S betters the offer by one tick');
    eq(L.hotIntent('take', lin, 6300, null, 'BUY', 1, 15).err, 'NO ASK', 'B with no ask is refused');
    eq(L.hotIntent('join', lin, null, 6400, 'BUY', 1, 15).err, 'NO BID', 'Shift+B with no bid is refused');
    // working cell
    eq(L.woText(8, 0, 12, false), '8 12s', 'working cell: size and seconds left');
    eq(L.woText(8, 2, 12, true), '8(2)', 'working cell: partial fill in brackets');
    eq(L.woText(100, 0, 59, false), '100 59', 'working cell drops the s when tight');
    eq(L.woText(1000, 0, 59, false), '1000', 'working cell keeps the size when nothing else fits');
    eq(L.woText(5, 0, null, false), '5', 'IOC or pending: size only');
    // break-even band: Kalshi taker 0.07 p(1-p) = 1.75c at 0.50
    const QUAD = { type: 'QUAD', mult: 1 };
    check(L.inBand('K', QUAD, 5000, 5100) && !L.inBand('K', QUAD, 5000, 5200), 'break-even band is the taker fee around the model fair');
    check(!L.inBand('K', QUAD, null, 5000), 'no model fair, no band');
    check(L.foreign('X-abc123') && !L.foreign('Mabc-1') && !L.foreign('SM1-x-2'), 'unprefixed cids are foreign');
    check(L.isExt({ own: 'EXT', cid: 'Mabc-1' }) && L.isExt({ own: 'MAN', cid: 'X-1abc' }) && !L.isExt({ own: 'SYS', cid: 'SM1-x-2' }), 'own EXT (or a foreign cid) marks an order EXT');
    check(L.ttlOK(15) && L.ttlOK(300) && L.ttlOK(1800) && !L.ttlOK('START'), 'expiries follow the contract (5 s to 30 min; START is a market time)');
    {
      const T = Date.UTC(2026, 9, 6, 23, 0, 0);
      eq(L.startExp({ start_ms: T + 600500 }, T), T + 600000, 'START: the game start, cut to a whole second');
      eq(L.startExp({ start_ms: T + 3000 }, T), null, 'START: refused under 5 s before the start');
      eq(L.startExp({ start_ms: T - 60000 }, T), null, 'START: refused once the game started');
      eq(L.startExp({ start_ms: null }, T), null, 'START: refused without a start time');
      const sh = [{ id: 'S0', free: 300000 }, { id: 'S1', free: 120000 }, { id: 'S3', free: 50000 }];
      const mv = L.moveFor(sh, 'S3', 101234);
      check(mv && mv.from === 'S0' && mv.amt === 110000 && mv.full, 'MOVE: from the shard with the most free cash, the shortfall rounded up to the dollar', mv);
      const mv2 = L.moveFor([{ id: 'S0', free: 52345 }, { id: 'S3', free: 0 }], 'S3', 101234);
      check(mv2 && mv2.amt === 52300 && !mv2.full, 'MOVE: at most the source shard\'s free cash (to the cent)', mv2);
      eq(L.moveFor([{ id: 'S3', free: 0 }], 'S3', 5000), null, 'MOVE: nothing to move from');
    }
    eq(L.parsePx('0.62'), 6200, 'ticket price in dollars');
    eq(L.parsePx('62'), 6200, 'ticket price in cents');
    eq(L.parsePx('62.5'), 6250, 'ticket price in tenths of a cent');
    eq(L.parsePx('.625'), 6250, 'ticket price with a leading dot');
    eq(L.parsePx('1.5'), 150, 'a number above 1 reads as cents');
    eq(L.parsePx('abc'), null, 'garbage is no price');
    eq(L.parsePx('100'), null, 'a dollar is not a price');
    eq(L.parseQty('2.5', 'P'), 2, 'Polymarket sizes are whole');
    eq(L.parseQty('2.555', 'K'), 2.56, 'Kalshi sizes keep two decimals');
    eq(L.parseQty('-1', 'K'), 0, 'negative size is 0');
    const mk = { v: 'K', fee: QUAD };
    let ci = L.costInfo(mk, null, 'BUY', 6200, 10, 'T');
    check(ci.reserve === 62000 && ci.fee > 0 && ci.cost === 62000 + ci.fee && ci.maxl === 62000 && ci.pos === 10, 'cost of a fresh buy: price x qty plus fee; max loss is the cost', ci);
    ci = L.costInfo(mk, { pos: 10, avg: 5000 }, 'SELL', 6000, 4, 'T');
    check(ci.reserve === 0 && ci.pos === 6 && ci.maxl === 30000, 'a reducing sell ties up no cash; max loss of what stays', ci);
    ci = L.costInfo(mk, { pos: 3, avg: 5000 }, 'SELL', 6000, 5, 'M');
    check(ci.reserve === 2 * 4000 && ci.pos === -2 && ci.maxl === 2 * 4000, 'a sell through flat: NO cost on the excess', ci);
    ci = L.costInfo(mk, null, 'BUY', 5800, 70, 'M');
    check(ci.fee === 0 && ci.cost === 406000 && ci.need === 406000 + D.fees.estimate('K', QUAD, 'BUY', 5800, 70, 'T'), 'a post: no maker fee, but the cash check (need) holds the taker fee as the gateway does', ci);
    eq(L.c2(175), '1.75', 'fee per contract in cents');
    eq(L.c2(-31.25), M + '0.31', 'a rebate is negative with U+2212');
    eq(L.series('KXNATGAS15M-26OCT061930-30'), 'KXNATGAS15M', 'the 15-minute series of a ticker');
    // watchlist groups
    const mm = (v, tk, grp, kind, close, short, status) => ({ v, tk, key: v + '|' + tk, grp, kind, close_ms: close, short: short || tk, title: tk, status: status || 'OPEN', og: grp, oc: short });
    const ms = new Map([
      ['K|W1', mm('K', 'W1', 'WX NYC', 'WEATHER', 9, 'NYC 64-65')], ['K|G1', mm('K', 'G1', 'NCAAF', 'GAME', 5, 'A@B A')],
      ['P|g1', mm('P', 'g1', 'NCAAF', 'GAME', 5, 'A@B A')], ['K|N1', mm('K', 'N1', '15M', 'LIP15', 3, 'NATGAS UP')],
      ['K|C1', mm('K', 'C1', '15M', 'LIP15', 3, 'COPPER UP')], ['K|S1', mm('K', 'S1', '15M', 'LIP15', 2, 'OLD', 'SETTLED')]
    ]);
    let wl = L.watchList(ms, { venue: 'ALL' });
    eq(wl.map(x => x.grp || x.m.tk).join(','), '15M,N1,C1,NCAAF A@B,G1,g1,WX NYC,W1', 'watchlist: LIP first, then games (one group per game), then weather; settled markets gone; listing order kept');
    eq(wl[0].n, 2, 'group rows carry their count');
    wl = L.watchList(ms, { venue: 'P' });
    eq(wl.map(x => x.grp || x.m.tk).join(','), 'NCAAF A@B,g1', 'watchlist venue filter');
    wl = L.watchList(ms, { venue: 'ALL', collapsed: { '15M': true } });
    check(wl[0].collapsed && wl[1].grp === 'NCAAF A@B', 'a collapsed group hides its markets');
    wl = L.watchList(ms, { venue: 'ALL', q: 'copper', collapsed: { '15M': true } });
    eq(wl.map(x => x.grp || x.m.tk).join(','), '15M,C1', 'search finds markets inside collapsed groups');
    eq(L.pairOf(ms, ms.get('K|G1')), 'P|g1', 'the same outcome on the other venue');
    eq(L.watchLabel(mm('K', 'X', 'NCAAF', 'GAME', 1, 'SOUMIS@TROY TROY')), 'TROY', 'a game row reads as its YES team');
    eq(L.watchLabel(mm('P', 'x', 'NCAAF', 'GAME', 1, 'SOUMIS@TROY SOUMIS')), 'SOUMIS P', 'a Polymarket row carries P');
    eq(L.watchLabel(mm('K', 'X', 'WX NYC 10/06', 'WEATHER', 1, 'NYC 64-65')), '64-65', 'a weather row reads as its bracket');
    eq(L.watchLabel(mm('K', 'X', '15M', 'LIP15', 1, 'NATGAS 19:30 UP')), 'NATGAS', 'a 15-minute row reads as its series');
    eq(L.watchLabel(mm('K', 'X', 'OTHER', 'OTHER', 1, 'FED HOLD')), 'FED HOLD', 'anything else keeps its short label');
    eq(L.watchGroup(mm('K', 'X', 'NCAAF', 'GAME', 1, 'SOUMIS@TROY TROY')), 'NCAAF SOUMIS@TROY', 'a game is its own group');
    eq(L.pairOf(ms, ms.get('K|N1')), null, 'no pair for a Kalshi-only market');
  }

  // =============================================================================================
  // 2. the page on the demo venues
  // =============================================================================================
  const P = bootPage({});
  const { D, $, click, key, run, fire } = P;
  const st = D.store, MS = D.manual.state();
  await run(1500);
  check(!st.stale && st.seq > 0 && st.markets.size > 30, 'the page booted on the demo venues and is in step', { stale: st.stale, why: st.staleWhy, n: st.markets.size });
  eq(D.render.tab, 'manual', 'MANUAL is the tab at first load');
  const A = MS.ladders.A, B = MS.ladders.B;
  check(A && B && MS.ladders.C, 'three ladders built');
  const natKey = D.focus.ladders.A;
  check(natKey && /\|KXNATGAS15M-/.test(natKey), 'ladder A defaults to the natural gas 15-minute market', natKey);
  check(D.focus.ladders.B && st.market(D.focus.ladders.B).kind === 'GAME', 'ladder B defaults to a game', D.focus.ladders.B);
  eq(D.focus.key, natKey, 'the focused market is ladder A\'s');
  await run(300);
  eq(st.book(natKey).depth, 'FULL', 'the ladders\' books stream at full depth');
  // freeze natural gas: bids 0.63 x120, 0.62 x85, 0.61 x40 / asks 0.64 x80, 0.65 x210, 0.67 x12
  const nat = st.market(natKey);
  P.sim().freeze(natKey, { bids: [[6300, 120], [6200, 85], [6100, 40]], asks: [[6400, 80], [6500, 210], [6700, 12]] });
  await run(300);
  eq(st.book(natKey).bb, 6300, 'frozen book arrived (bid)');
  eq(st.book(natKey).ba, 6400, 'frozen book arrived (ask)');
  // ladder anatomy
  eq($('m-lad-a-cols').textContent, 'WRK BBIDREFPRICEASKWRK SLTQ', 'ladder columns WRK B | BID | REF | PRICE | ASK | WRK S | LTQ');
  eq(rowsOf(A.tb).length, 38, 'about 37 visible rows of 18 px (38 with the partial one)');
  const rowPx = rowsOf(A.tb).map(r => +r.getAttribute('data-k'));
  check(rowPx.every((p, i) => !i || p < rowPx[i - 1]) && rowPx.indexOf(6300) >= 0 && rowPx.indexOf(6400) >= 0, 'one row per price, high to low, with the touch in view');
  const mid = rowPx.indexOf(6300);
  check(Math.abs(mid - 19) <= 2, 'centred on the touch', mid);
  eq(ladCell(P, 'A', 6300, 'bq').textContent, '120', 'BID size at the best bid');
  check(ladCell(P, 'A', 6300, 'bq').className.indexOf('has') >= 0 && ladCell(P, 'A', 6300, 'bq').style.getPropertyValue('--d') === '0.57', 'depth bar width is size over the largest visible', ladCell(P, 'A', 6300, 'bq').style.getPropertyValue('--d'));
  eq(ladCell(P, 'A', 6500, 'aq').textContent, '210', 'ASK size');
  check(/\bbb\b/.test(ladCell(P, 'A', 6300, 'px').className) && /\bba\b/.test(ladCell(P, 'A', 6400, 'px').className), 'best bid and ask prices are marked');
  eq(ladCell(P, 'A', 6400, 'px').textContent, '0.64', 'prices on the grid\'s decimals');
  {
    const fair = st.book(natKey).ref.fair, asc = A.asc, fr = asc[D.manual.logic.nearestIdx(asc, fair)];
    const fRows = rowsOf(A.tb).filter(r => /F/.test(r.children[2].textContent)).map(r => +r.getAttribute('data-k'));
    check(rowPx.indexOf(fr) >= 0 ? (fRows.length === 1 && fRows[0] === fr && /mk-f/.test(ladCell(P, 'A', fr, 'rf').className)) : fRows.length === 0,
      'the model fair marker F sits on the row nearest the fair', { fair, fr, fRows });
    const be = rowsOf(A.tb).filter(r => /\bbe\b/.test(r.className)).map(r => +r.getAttribute('data-k'));
    check(be.every(p => D.manual.logic.inBand('K', nat.fee, fair, p)), 'break-even band rows are within the taker fee of the fair');
  }
  const head = $('m-lad-a-head');
  check(/^K/.test(head.children[0].textContent) && head.children[0].textContent.indexOf(nat.short) >= 0, 'header line 1: venue and market', head.children[0].textContent);
  check(/POS0UNRL/.test(head.children[1].textContent.replace(/\s/g, '')) && /S3/.test(head.children[1].textContent) && /FEET1\.\d\dM0\.00/.test(head.children[1].textContent.replace(/\s/g, '')), 'header line 2: position, shard, fees (15-minute series: takers pay, makers free)', head.children[1].textContent);
  check(/MID0\.635/.test(head.children[2].textContent.replace(/\s/g, '')) && /MODEL0\.\d\d±0\.02/.test(head.children[2].textContent.replace(/\s/g, '')), 'header line 3: mid one decimal finer, model with sigma', head.children[2].textContent);
  check(/chg/.test(head.children[2].textContent), 'header shows the time since the book last changed');
  eq(P.$('m-lad-a').getAttribute('data-focused'), '1', 'the focused ladder carries the outline flag');

  // ---- clicks ---------------------------------------------------------------------------------
  const before = st.orders.size;
  click(ladCell(P, 'A', 6200, 'bq'));
  let o = newest(P, x => x.own === 'MAN');
  check(o && st.orders.size === before + 1 && o.st === 'PEND' && o.tif === 'POST' && o.px === 6200 && o.side === 'BUY', 'click BID below the touch: a post-only buy, pending at once', o && { st: o.st, tif: o.tif, px: o.px });
  P.frame();
  check(/\bmy\b/.test(ladCell(P, 'A', 6200, 'wb').className) && /\bpend\b/.test(ladCell(P, 'A', 6200, 'wb').className), 'the next frame paints a pending WRK B cell', ladCell(P, 'A', 6200, 'wb').className);
  check(D.lat.paint.last !== null && D.lat.paint.last < 100, 'click-to-paint measured', D.lat.paint.last);
  const workRow = P.$('m-work-body').children[0].children[2].children.find(r => r.getAttribute('data-k') === o.cid);
  check(workRow && /o-PEND/.test(workRow.className) && workRow.children[9].textContent === 'PEND', 'WORKING shows the order PEND on the same frame');
  const cidPost = o.cid;
  await run(250);
  o = st.orders.get(cidPost);
  eq(o.st, 'WORK', 'acknowledged: WORK');
  check(D.lat.ack.p50() !== null, 'click-to-ack measured');
  check(/^1 1[456]s$/.test(ladCell(P, 'A', 6200, 'wb').textContent) && !/pend/.test(ladCell(P, 'A', 6200, 'wb').className), 'our order shows size and seconds left', ladCell(P, 'A', 6200, 'wb').textContent);
  check(/^UI \d/.test(P.$('sb-ui').textContent) && /^ACK \d/.test(P.$('sb-ack').textContent), 'status bar shows click-to-paint and click-to-ack', P.$('sb-ui').textContent + ' ' + P.$('sb-ack').textContent);
  check(D.manual.state().held(natKey) && st.market(natKey).man === true, 'a manual order marks the market MAN (mkt.man from the backend)');
  P.frame(); await run(100);
  check(/MAN/.test(head.children[0].textContent) && !/\boff\b/.test(head.children[0].children[5].className), 'the ladder header shows MAN with REL', head.children[0].textContent);

  // SELL at or through the bid: IOC down to the cap, fills
  click(ladCell(P, 'A', 6300, 'aq'));
  o = newest(P, x => x.own === 'MAN');
  check(o.tif === 'IOC' && o.side === 'SELL' && o.px === 6200 && o.touch === 6300, 'click ASK at the bid: an IOC sell, limit one tick through (cap 1)', { tif: o.tif, px: o.px, touch: o.touch });
  P.frame();
  check(/pend/.test(ladCell(P, 'A', 6300, 'ws').className), 'a pending IOC paints on the row clicked, not on its capped limit');
  await run(250);
  o = st.orders.get(o.cid);
  check(o.st === 'FILL' && o.avg === 6300, 'the IOC filled at the bid', { st: o.st, avg: o.avg });
  check(st.fills.some(f => f.cid === o.cid), 'the fill arrived');
  const fillRow = P.$('m-fills-body').children[0].children[2].children[0];
  check(fillRow && /SELL/.test(fillRow.children[2].textContent) && fillRow.children[5].textContent === 'T', 'FILLS shows it as a taker sell');
  eq(st.position(natKey).pos, -1, 'position short 1');
  // BUY at the ask takes
  click(ladCell(P, 'A', 6400, 'bq'));
  o = newest(P, x => x.own === 'MAN');
  check(o.tif === 'IOC' && o.px === 6500 && o.touch === 6400, 'click BID at the ask: IOC buy up to one tick more');
  await run(250);
  eq(st.orders.get(o.cid).st, 'FILL', 'filled');
  eq(st.position(natKey).pos, 0, 'flat again');
  // the fat-finger guard: 4 ticks through needs a second click (or Shift)
  let n0 = st.orders.size;
  const a0 = st.alerts.length;
  click(ladCell(P, 'A', 6700, 'bq'));
  eq(st.orders.size, n0, 'a buy 4 ticks through the ask is not sent');
  check(st.alerts.length > a0 && /PRICE_BAND/.test(st.alerts[0].msg) && st.alerts[0].lvl === 'WARN', 'the reason goes to the status bar', st.alerts[0]);
  P.frame();
  check(/\barm\b/.test(ladCell(P, 'A', 6700, 'bq').className), 'the refused cell stays armed (amber) for a second click');
  check(/PRICE_BAND/.test(P.$('sb-alert').textContent), 'status bar shows the guard');
  click(ladCell(P, 'A', 6700, 'bq'));
  eq(st.orders.size, n0 + 1, 'the second click on the armed cell sends it (override)');
  o = newest(P, x => x.own === 'MAN');
  await run(250);
  check(st.orders.get(o.cid).st !== 'REJ', 'the override was accepted by the venue', st.orders.get(o.cid).why);
  n0 = st.orders.size;
  click(ladCell(P, 'A', 6700, 'bq'), { shiftKey: true });
  eq(st.orders.size, n0 + 1, 'Shift+click overrides at once');
  await run(250);
  // flatten what the guard tests bought (via positions FLAT later); right-click cancel
  check(/\bmy\b/.test(ladCell(P, 'A', 6200, 'wb').className), 'our resting buy is still on 0.62');
  fire(ladCell(P, 'A', 6200, 'wb'), 'contextmenu', { button: 2 });
  check(st.orders.get(cidPost).local.cxlPend, 'right-click on our order: cancel in flight at once');
  P.frame();
  check(/cxlpend/.test(ladCell(P, 'A', 6200, 'wb').className), 'a cancel in flight is struck through');
  await run(250);
  check(st.orders.get(cidPost).st === 'CXL' && st.orders.get(cidPost).why === 'USER', 'cancelled');
  check(!/\bmy\b/.test(ladCell(P, 'A', 6200, 'wb').className), 'the cell clears');
  const wrow = P.$('m-work-body').children[0].children[2].children.find(r => r.getAttribute('data-k') === cidPost);
  check(wrow && /o-CXL/.test(wrow.className), 'WORKING keeps the CXL row for 3 s');
  await run(3500);
  check(!P.$('m-work-body').children[0].children[2].children.some(r => r.getAttribute('data-k') === cidPost), 'then drops it');
  // drag to re-price: cancel + a new post-only order
  click(ladCell(P, 'A', 6100, 'bq'));
  const cidDrag = newest(P, x => x.own === 'MAN').cid;
  await run(250);
  fire(ladCell(P, 'A', 6100, 'wb'), 'mousedown', { button: 0 });
  fire(ladCell(P, 'A', 6000, 'wb'), 'mousemove', { button: 0 });
  P.frame();
  check(/drag/.test(ladCell(P, 'A', 6000, 'wb').className), 'the drag target is outlined');
  n0 = st.orders.size;
  fire(ladCell(P, 'A', 6000, 'wb'), 'mouseup', { button: 0 });
  const nRe = newest(P, x => x.own === 'MAN');
  check(st.orders.get(cidDrag).local.cxlPend && st.orders.size === n0 + 1 && nRe.px === 6000 && nRe.tif === 'POST', 'drag re-prices as cancel + new post-only', { px: nRe.px, tif: nRe.tif });
  check(/queue place lost/.test(st.alerts[0].msg), 'the status bar says queue place is lost');
  await run(250);
  // PRICE copies to the ticket, never an order
  n0 = st.orders.size;
  click(ladCell(P, 'A', 6200, 'px'));
  eq(st.orders.size, n0, 'a click on PRICE never sends');
  eq(MS.ticket.px, 6200, 'it copies the price to the ticket');
  // footer: expiry, qty presets, CLR
  const foot = P.$('m-lad-a-foot');
  const fbtn = t => foot.children[0].children.concat(foot.children[1].children).find(b => b.textContent === t);
  click(fbtn('60s'));
  eq(A.cfg.ttl, 60, 'expiry 60 s selected');
  click(fbtn('5'));
  eq(A.cfg.qty, 5, 'size preset 5');
  click(ladCell(P, 'A', 6100, 'bq'));
  o = newest(P, x => x.own === 'MAN');
  check(o.qty === 5 && o.tif === 'POST' && o.exp_ms - P.clock.t > 55000 && o.exp_ms - P.clock.t <= 61000, 'posts carry the ladder size and expiry', { qty: o.qty, exp: o.exp_ms - P.clock.t });
  click(fbtn('5'), { shiftKey: true });
  eq(A.cfg.qty, 10, 'Shift+preset adds');
  click(fbtn('CLR'));
  eq(A.cfg.qty, 0, 'CLR zeroes the size');
  n0 = st.orders.size;
  click(ladCell(P, 'A', 6100, 'bq'));
  check(st.orders.size === n0 && /QTY 0/.test(st.alerts[0].msg), 'a zero size refuses clicks');
  click(fbtn('1')); click(fbtn('15s'));
  // scrolling: wheel, auto-recentre only when the pointer leaves, Space recentres
  fire(P.$('m-lad-a'), 'mouseenter', {});
  const top0 = A.top;
  fire(A.body.children[0].children[1].children[0].children[0], 'wheel', { deltaY: 0, wheelDeltaY: -120, deltaMode: 0 });
  P.frame();
  eq(A.top, top0 + 3, 'a wheel notch moves 3 rows');
  for (let i = 0; i < 10; i++) fire(A.body, 'wheel', { deltaY: 54, deltaMode: 0 });
  P.frame(); await run(300);
  eq(A.top, top0 + 33, 'pixel deltas move a row per 18 px; rows never move under the pointer', A.top);
  fire(P.$('m-lad-a'), 'mouseleave', {});
  await run(300);
  eq(A.top, top0, 'the touch out of view and the pointer gone: it recentres');
  A.el.focus();
  key('ArrowDown', 'ArrowDown'); P.frame();
  eq(A.top, top0 + 1, 'Down scrolls one row');
  key('Space', ' '); await run(100);
  eq(A.top, top0, 'Space recentres');
  key('PageDown', 'PageDown'); await run(300);
  check(A.top > top0 + 20, 'PgDn scrolls a page, and the auto-recentre holds off after a keyboard scroll', { top: A.top, top0 });
  await run(10500, 250);
  eq(A.top, top0, 'until the hold lapses');
  key('PageUp', 'PageUp'); P.frame();
  key('Space', ' '); await run(100);
  eq(A.top, top0, 'Space recentres at once');

  // ---- hotkeys --------------------------------------------------------------------------------
  check(!D.keys.hk && P.$('sb-hk').textContent === 'HOTKEYS OFF', 'hotkeys are off at load');
  n0 = st.orders.size;
  A.el.focus();
  key('KeyB', 'b');
  eq(st.orders.size, n0, 'B does nothing while hotkeys are off');
  key('KeyH', 'h'); P.frame();
  check(D.keys.hk && P.$('sb-hk').textContent === 'HOTKEYS ON', 'H turns hotkeys on (amber in the status bar)');
  check(/HK ON/.test(P.$('m-lad-a-foot').textContent), 'the ladder footer shows HK ON');
  key('KeyB', 'b');
  o = newest(P, x => x.own === 'MAN');
  check(st.orders.size === n0 + 1 && o.tif === 'IOC' && o.side === 'BUY' && o.px === 6500 && o.touch === 6400, 'B: IOC buy at the ask plus the cap', { tif: o.tif, px: o.px });
  await run(250);
  key('F2', 'F2');
  o = newest(P, x => x.own === 'MAN');
  check(o.tif === 'IOC' && o.side === 'SELL' && o.px === 6200, 'F2: IOC sell at the bid minus the cap');
  await run(250);
  key('KeyB', 'B', { shiftKey: true });
  o = newest(P, x => x.own === 'MAN');
  check(o.tif === 'POST' && o.side === 'BUY' && o.px === 6300, 'Shift+B joins the bid');
  key('KeyS', 'S', { shiftKey: true });
  o = newest(P, x => x.own === 'MAN');
  check(o.tif === 'POST' && o.side === 'SELL' && o.px === 6400, 'Shift+S joins the offer');
  n0 = st.orders.size;
  key('KeyB', '∫', { altKey: true });
  check(st.orders.size === n0 && /SPREAD 1 TICK/.test(st.alerts[0].msg), 'Option+B (types ∫) is read by key code; refused on a 1-tick spread');
  await run(250);
  key('Escape', 'Escape');
  check(manOrders(P, natKey).filter(x => !x.terminal).every(x => x.local && x.local.cxlPend), 'Esc cancels every manual order in this market');
  await run(300);
  eq(manOrders(P, natKey).filter(x => !x.terminal).length, 0, 'none left working there');
  P.sim().freeze(natKey, { bids: [[6200, 50]], asks: [[6400, 50]] });
  await run(300);
  key('KeyB', '∫', { altKey: true });
  o = newest(P, x => x.own === 'MAN');
  check(o.tif === 'POST' && o.px === 6300 && o.side === 'BUY', 'Alt+B betters the bid by a tick on a 2-tick spread', { px: o.px });
  key('F6', 'F6');
  o = newest(P, x => x.own === 'MAN');
  check(o.tif === 'POST' && o.px === 6300 && o.side === 'SELL', 'F6 betters the offer');
  key('Digit3', '3'); eq(A.cfg.qty, 10, '3 picks the third size preset');
  key('Equal', '+', { shiftKey: true }); eq(A.cfg.qty, 11, '+ steps the size up');
  key('Minus', '-'); key('Minus', '-'); eq(A.cfg.qty, 9, '- steps it down');
  key('Digit1', '1');
  await run(250);
  // Shift+Esc: every manual order on both venues
  const gameKey = D.focus.ladders.B;
  B.el.focus();
  P.frame();
  eq(D.focus.ladder, 'B', 'focusing ladder B makes it the focused ladder');
  eq(D.focus.key, gameKey, 'and its market the focused market');
  const gb = st.book(gameKey);
  const farBid = gb && gb.bb !== null ? D.grid.step(st.market(gameKey).g, gb.bb, -3) : 1000;
  D.manual.ticketPrice(gameKey, farBid);
  D.manual.tkSubmit('BUY', null);
  check(manOrders(P).filter(x => !x.terminal).length >= 2, 'manual orders on two markets');
  key('Escape', 'Escape', { shiftKey: true });
  check(manOrders(P).filter(x => !x.terminal).every(x => x.local && x.local.cxlPend), 'Shift+Esc cancels every manual order');
  await run(300);
  eq(manOrders(P).filter(x => !x.terminal).length, 0, 'no manual order left');
  key('Tab', 'Tab');
  eq(D.focus.ladder, 'A', 'Tab moves to the next ladder');
  key('KeyH', 'h');
  check(!D.keys.hk, 'H turns hotkeys off again');
  await run(45000, 250);       // ESPN runs ~41 s behind the play: give it a lag's worth of history

  // ---- watchlist ------------------------------------------------------------------------------
  const wtb = MS.watch.tb;
  const grps = wtb.children.filter(r => r.getAttribute('data-grp'));
  check(grps.length >= 3 && grps[0].getAttribute('data-grp') === '15M' && /^15M 7$/.test(grps[0].textContent), 'watchlist groups, 15-minute series first with their count', grps.map(r => r.textContent));
  const natRow = wtb.children.find(r => r.getAttribute('data-mkt') === natKey);
  check(natRow && natRow.children[1].textContent === '0.62' && natRow.children[2].textContent === '0.64', 'watchlist bid / ask', natRow && natRow.children.map(c => c.textContent));
  check(natRow && /\bsel\b/.test(natRow.className), 'the focused market is selected in the watchlist');
  check(natRow && /^\d+m$|^\d+s$/.test(natRow.children[6].textContent), 'T is the time to close for a 15-minute market', natRow && natRow.children[6].textContent);
  const gRow = wtb.children.find(r => r.getAttribute('data-mkt') === gameKey);
  check(gRow && /^(Q\d|OT) \d+:\d\d$/.test(gRow.children[6].textContent), 'T is the game clock for a live game', gRow && gRow.children[6].textContent);
  {
    const gm = st.market(gameKey), gg = D.manual.logic.watchGroup(gm);
    const gh = wtb.children.find(r => r.getAttribute('data-grp') === gg);
    check(gh && /^NCAAF [A-Z]+@[A-Z]+ \d+  \d+-\d+$/.test(gh.textContent), 'a live game is its own group row with the score', gh && gh.textContent);
    eq(gRow.children[0].textContent, gm.oc, 'inside it the row reads as the YES team');
  }
  check(wtb.children.some(r => r.getAttribute('data-mkt') && r.children[4].textContent !== ''), 'ESPN shows on live games');
  check(wtb.children.some(r => r.getAttribute('data-mkt') && /\b(fair|warn)\b/.test(r.children[3].className) && r.children[3].textContent !== ''), 'model fair shows in the watchlist');
  // amber rule: |mid - fair| beyond the taker break-even
  const amberOk = wtb.children.filter(r => r.getAttribute('data-mkt')).every(r => {
    const k = r.getAttribute('data-mkt'), b = st.book(k), m = st.market(k);
    if (!b || b.ref.fair === null || b.mid === null) return true;
    const want = Math.abs(b.mid - b.ref.fair) > D.fees.takerPer(m.v, m.fee, b.mid);
    return /\bwarn\b/.test(r.children[3].className) === want;
  });
  check(amberOk, 'Fair is amber exactly when |mid - fair| beats the taker break-even');
  // click loads into the focused ladder, Shift+click into the other
  const wxRow = wtb.children.find(r => r.getAttribute('data-mkt') && st.market(r.getAttribute('data-mkt')).kind === 'WEATHER');
  const wxKey = wxRow.getAttribute('data-mkt');
  click(wxRow.children[0], { shiftKey: true });
  check(D.focus.ladders.B === wxKey && D.focus.key === natKey, 'Shift+click loads the other ladder and keeps the focus');
  await run(300);
  eq(st.book(wxKey).depth, 'FULL', 'the newly loaded ladder subscribes at full depth');
  // keyboard: / search, arrows, Enter
  A.el.focus();
  key('Slash', '/');
  eq(P.doc.activeElement, MS.watch.el.q, '/ focuses the watchlist search');
  MS.watch.el.q.value = 'copper'; fire(MS.watch.el.q, 'input', {});
  await run(300);
  const vis = wtb.children.filter(r => r.getAttribute('data-mkt'));
  check(vis.length === 1 && /COPPER/.test(vis[0].children[0].textContent), 'search filters the watchlist', vis.map(r => r.children[0].textContent));
  n0 = st.orders.size;
  key('KeyB', 'b');
  eq(st.orders.size, n0, 'typing in the search is never a command');
  key('Enter', 'Enter');
  const copperKey = vis[0].getAttribute('data-mkt');
  check(D.focus.key === copperKey && D.focus.ladders.A === copperKey, 'Enter loads the match into the focused ladder');
  MS.watch.el.q.value = ''; fire(MS.watch.el.q, 'input', {});
  key('Escape', 'Escape');
  check(P.doc.activeElement !== MS.watch.el.q, 'Esc leaves the search');
  await run(300);
  P.$('m-watch').focus();
  key('ArrowDown', 'ArrowDown'); key('ArrowDown', 'ArrowDown'); key('Enter', 'Enter');
  check(D.focus.key && D.focus.key !== copperKey, 'Up/Down and Enter work with the list focused', D.focus.key);
  // group collapse
  click(grps[0].children[0]);
  await run(300);
  check(MS.cfg.watch.collapsed['15M'] && !wtb.children.some(r => r.getAttribute('data-mkt') && st.market(r.getAttribute('data-mkt')).grp === '15M'), 'a group row click collapses it');
  click(wtb.children.find(r => r.getAttribute('data-grp') === '15M').children[0]);
  // venue filter
  const vbP = MS.watch.el.vb.find(b => b.textContent === 'P');
  click(vbP); await run(300);
  check(wtb.children.filter(r => r.getAttribute('data-mkt')).every(r => r.getAttribute('data-mkt').charAt(0) === 'P'), 'the P filter shows Polymarket only');
  click(MS.watch.el.vb.find(b => b.textContent === 'ALL')); await run(300);
  D.focus.set(natKey); await run(300);

  // ---- ticket ---------------------------------------------------------------------------------
  const E = MS.ticket.el;
  eq(E.mkt.textContent, nat.short, 'the ticket follows the focused market');
  eq(MS.cfg.tk.type, 'P15', 'ticket defaults to POST 15 s');
  P.sim().freeze(natKey, { bids: [[6300, 120], [6200, 85]], asks: [[6400, 80], [6500, 210], [6700, 12]] });
  await run(300);
  check(/^BUY 1 @0\.63 15s$/.test(E.buy.textContent) && /^SELL 1 @0\.64 15s$/.test(E.sell.textContent), 'AUTO price: a post joins its own side', E.buy.textContent + ' | ' + E.sell.textContent);
  check(/auto/.test(E.px.className) && E.px.value === '0.63', 'the price field shows AUTO in grey');
  const tyBtn = t => E.types.find(b => b.textContent === t);
  check(!tyBtn('30m').disabled && tyBtn('START').disabled && !tyBtn('5m').disabled, '5m and 30m are there; START needs a game start time (a 15-minute window has none)');
  check(['IOC', 'FOK', '5s', '15s', '60s', '5m', '30m', 'START'].every(t => tyBtn(t)), 'IOC FOK POST 5s 15s 60s 5m 30m START, no GTC');
  click(tyBtn('IOC')); P.frame();
  check(/^BUY 1 ≤0\.65$/.test(E.buy.textContent), 'IOC shows how far it may fill (the cap)', E.buy.textContent);
  check(/cost/.test(E.costL.textContent) && E.cost.textContent === '0.67' && E.shL.textContent === 'S3 free' && E.shOk.textContent === 'OK', 'check line: cost (0.65 plus the 0.02 taker fee), shard free cash, OK', [E.cost.textContent, E.shL.textContent, E.sh.textContent, E.shOk.textContent]);
  // a short shard: MOVE moves the missing cash in one click
  {
    const s3 = () => st.hdr.cash.K.shards.find(x => x.id === 'S3').free;
    E.qty.value = '70'; fire(E.qty, 'input', {}); P.frame();
    check(/^SHORT /.test(E.shOk.textContent) && !/\boff\b/.test(E.move.className) && /^MOVE \d+ S\d>S3$/.test(E.move.textContent), 'a short shard offers MOVE from the richest shard', [E.shOk.textContent, E.move.textContent]);
    const free0 = s3();
    click(E.move); P.frame();
    check(E.move.textContent === 'MOVING' && E.move.disabled, 'MOVE paints pending at once');
    await run(400);
    check(s3() > free0 && E.shOk.textContent === 'OK' && /\boff\b/.test(E.move.className), 'after the move the shard covers the order', [s3(), free0, E.shOk.textContent]);
    E.qty.value = '1'; fire(E.qty, 'input', {}); P.frame();
  }
  // START: a game that has not started posts until its start
  {
    const gm = Array.from(st.markets.values()).find(m => m.kind === 'GAME' && m.v === 'K' && m.start_ms && m.start_ms > P.clock.t + 60000);
    check(!!gm, 'a Kalshi game that has not started yet', gm && gm.tk);
    D.focus.set(gm.key); await run(300);
    check(!tyBtn('START').disabled, 'START is live on a game before its start');
    click(tyBtn('START')); P.frame();
    n0 = st.orders.size;
    click(E.buy);
    o = newest(P, x => x.own === 'MAN');
    check(st.orders.size === n0 + 1 && o.tif === 'POST' && o.exp_ms === Math.floor(gm.start_ms / 1000) * 1000, 'START sends a post whose exchange expiry is the game start', { tif: o.tif, exp: o.exp_ms, start: gm.start_ms });
    await run(300);
    o = st.orders.get(o.cid);
    check(o.st === 'WORK' && o.exp_ms === Math.floor(gm.start_ms / 1000) * 1000, 'the venue keeps that expiry', { st: o.st, exp: o.exp_ms });
    D.act.cancel([o.cid]); await run(300);
    D.focus.set(natKey); await run(300);
    check(MS.cfg.tk.type === 'PSTART' && / 15s$/.test(E.buy.textContent) && !E.buy.disabled && /\bon\b/.test(tyBtn('15s').className),
      'START chosen on a market with no start acts as 15 s (the buttons stay live)', [E.buy.textContent, E.buy.disabled]);
    click(tyBtn('IOC')); P.frame();
  }
  check(E.eM.textContent !== '' && /c/.test(E.eM.textContent), 'edge vs model shown', E.eM.textContent);
  click(E.q.find(b => b.textContent === '5'));
  eq(MS.cfg.tk.qty, 5, 'ticket size preset');
  n0 = st.orders.size;
  click(E.buy);
  o = newest(P, x => x.own === 'MAN');
  check(st.orders.size === n0 + 1 && o.tif === 'IOC' && o.qty === 5 && o.px === 6500, 'BUY sends the IOC', { tif: o.tif, qty: o.qty, px: o.px });
  await run(250);
  // fixed price, the guard as a second click
  click(tyBtn('15s'));
  E.px.focus(); E.px.value = '0.70'; fire(E.px, 'input', {}); E.px.blur();
  P.frame();
  eq(MS.ticket.px, 7000, 'a typed price fixes the ticket price');
  check(/GUARD/.test(E.guard.textContent), 'the check line flags the guard before the click', E.guard.textContent);
  n0 = st.orders.size;
  click(E.buy); P.frame();
  check(st.orders.size === n0 && /^OVERRIDE BUY/.test(E.buy.textContent) && /confirm/.test(E.buy.className), 'the guard turns the button into OVERRIDE (no dialog)', E.buy.textContent);
  click(E.buy);
  eq(st.orders.size, n0 + 1, 'the second click sends it');
  await run(3500);
  check(!/OVERRIDE/.test(E.buy.textContent), 'OVERRIDE lapses');
  // price stepping and the fill buttons
  E.px.focus(); fire(E.px, 'keydown', { key: 'ArrowDown', code: 'ArrowDown' }); E.px.blur();
  eq(MS.ticket.px, 6900, 'Down in the price field steps a tick on the grid');
  click(E.fB); eq(MS.ticket.px, 6300, 'BID fills the bid');
  click(E.fA); eq(MS.ticket.px, 6400, 'ASK fills the ask');
  E.px.focus(); E.px.value = '0.635'; fire(E.px, 'input', {});
  check(MS.ticket.pxBad, 'an off-grid price is flagged');
  E.px.blur(); eq(MS.ticket.px, 6400, 'and snaps to the grid on blur');
  E.px.focus(); key('Escape', 'Escape');
  eq(MS.ticket.px, null, 'Esc in the ticket returns the price to AUTO');
  // Enter sends the selected side
  click(E.sS); eq(MS.cfg.tk.side, 'SELL', 'side toggle');
  E.qty.focus(); E.qty.value = '2'; fire(E.qty, 'input', {});
  n0 = st.orders.size;
  key('Enter', 'Enter');
  o = newest(P, x => x.own === 'MAN');
  check(st.orders.size === n0 + 1 && o.side === 'SELL' && o.qty === 2 && o.tif === 'POST' && o.px === 6400, 'Enter in the ticket sends the selected side', { side: o.side, qty: o.qty, px: o.px });
  E.qty.blur(); click(E.sB);
  await run(300);
  // the venue toggle loads the same outcome on the other venue
  const pairM = Array.from(st.markets.values()).find(m => m.v === 'K' && m.kind === 'GAME' && m.status === 'OPEN' && D.manual.logic.pairOf(st.markets, m));
  D.focus.set(pairM.key); await run(300);
  check(!E.vP.disabled && /\bon\b/.test(E.vK.className), 'a game with a Polymarket pair can switch venue');
  click(E.vP); await run(300);
  check(D.focus.key && D.focus.key.charAt(0) === 'P' && st.market(D.focus.key).oc === pairM.oc, 'PM loads the paired market', D.focus.key);
  D.focus.set(natKey); await run(300);

  // ---- working, fills, positions --------------------------------------------------------------
  const wtb2 = P.$('m-work-body').children[0].children[2];
  const live = manOrders(P).filter(x => !x.terminal);
  check(live.length >= 1, 'something is working');
  const r0 = wtb2.children.find(r => r.getAttribute('data-k') === live[0].cid);
  check(r0 && r0.children[9].textContent === 'WORK' && /^\d+s$/.test(r0.children[7].textContent) && r0.getAttribute('data-mkt') === live[0].key, 'WORKING: state, seconds to expiry, linked market', r0 && r0.children.map(c => c.textContent));
  click(r0.children[10]);
  check(st.orders.get(live[0].cid).local.cxlPend, 'x cancels the row');
  check(D.focus.key === natKey, 'the x does not re-focus the market');
  await run(300);
  // foreign order flag
  const ex = JSON.parse(JSON.stringify(D.contract.examples.ord));
  ex.cid = 'X-foreign-1'; ex.tk = nat.tk; ex.v = 'K'; ex.own = 'EXT'; ex.st = 'WORK'; ex.filled = 0; ex.left = 3; ex.qty = 3; ex.px = '0.5000'; ex.exp_ms = null; ex.tif = 'POST';
  st._handlers.ord(ex); await run(300);
  const xr = wtb2.children.find(r => r.getAttribute('data-k') === 'X-foreign-1');
  check(xr && /^EXT /.test(xr.children[1].textContent) && /\bext\b/.test(xr.children[1].className), 'a foreign working order is flagged EXT', xr && xr.children[1].textContent);
  check(!/\boff\b/.test(head.children[0].children[6].className), 'the ladder header flags EXT too');
  ex.st = 'CXL'; ex.left = 0; ex.why = 'USER'; st._handlers.ord(ex);
  // PART, EXP and REJ as the ladder and WORKING show them
  P.sim().freeze(natKey, { bids: [[6200, 10]], asks: [[6400, 10]] });
  await run(300);
  click(fbtn('5')); click(fbtn('5s'));
  click(ladCell(P, 'A', 6300, 'bq'));
  const cidPart = newest(P, x => x.own === 'MAN').cid;
  await run(250);
  P.sim().trade(natKey, 'SELL', 2);
  await run(250);
  o = st.orders.get(cidPart);
  check(o.st === 'PART' && o.filled === 2 && o.left === 3, 'a resting buy partly filled: PART', { st: o.st, filled: o.filled });
  eq(ladCell(P, 'A', 6300, 'wb').textContent, '3(2)', 'the ladder shows what is left and the filled amount in brackets');
  let pr2 = P.$('m-work-body').children[0].children[2].children.find(r => r.getAttribute('data-k') === cidPart);
  check(pr2 && pr2.children[9].textContent === 'PART' && pr2.children[5].textContent === '2' && pr2.children[6].textContent === '3', 'WORKING: PART, Fill 2, Left 3');
  await run(6000, 250);
  o = st.orders.get(cidPart);
  check(o.st === 'EXP' && o.why === 'EXPIRY', 'the 5 s post expired at the exchange: EXP', { st: o.st });
  pr2 = P.$('m-work-body').children[0].children[2].children.find(r => r.getAttribute('data-k') === cidPart);
  check(pr2 && /o-EXP/.test(pr2.className) && pr2.children[9].textContent === 'EXP', 'WORKING shows EXP grey for 3 s');
  click(fbtn('1')); click(fbtn('15s'));
  await run(3500);
  // a reject: more than the shard's cash
  D.manual.ticketPrice(natKey, 5000);
  E.qty.value = '100000'; fire(E.qty, 'input', {});
  click(E.buy);
  const cidRej = newest(P, x => x.own === 'MAN').cid;
  await run(300);
  o = st.orders.get(cidRej);
  check(o.st === 'REJ' && o.why === 'SHARD_CASH', 'an order beyond the shard\'s cash is rejected', { st: o.st, why: o.why });
  pr2 = P.$('m-work-body').children[0].children[2].children.find(r => r.getAttribute('data-k') === cidRej);
  check(pr2 && /o-REJ/.test(pr2.className) && pr2.children[9].textContent === 'SHARD_CASH' && /\bwhy\b/.test(pr2.children[9].className), 'WORKING shows the reject struck through with its reason');
  check(/SHARD_CASH/.test(P.$('sb-alert').textContent), 'and the status bar carries it');
  E.qty.value = '1'; fire(E.qty, 'input', {});
  await run(3500);
  check(!P.$('m-work-body').children[0].children[2].children.some(r => r.getAttribute('data-k') === cidRej), 'the reject leaves WORKING after 3 s');
  P.sim().freeze(natKey, { bids: [[6300, 120], [6200, 85]], asks: [[6400, 80], [6500, 210], [6700, 12]] });
  await run(300);
  // positions and FLAT
  click(ladCell(P, 'A', 6400, 'bq'));
  await run(300);
  click(ladCell(P, 'A', 6400, 'bq'));
  await run(300);
  const pos = st.position(natKey);
  check(pos && pos.pos > 0, 'long after two buys', pos && pos.pos);
  const ptb = P.$('m-pos-body').children[0].children[2];
  const pr = ptb.children.find(r => r.getAttribute('data-k') === natKey);
  check(pr && pr.children[3].textContent === '+' + pos.pos && pr.children[2].textContent === 'MAN' && pr.children[10].textContent === 'FLAT', 'POSITIONS row with FLAT', pr && pr.children.map(c => c.textContent));
  check(ptb.children.some(r => /total/.test(r.className) && r.children[0].textContent === 'TOTAL'), 'a TOTAL row');
  n0 = st.orders.size;
  const posted = [], post0 = D.transport.post;
  D.transport.post = (p, b) => { posted.push([p, b]); return post0(p, b); };
  click(pr.children[10]);
  D.transport.post = post0;
  o = newest(P, x => x.own === 'MAN');
  check(st.orders.size === n0 + 1 && o.side === 'SELL' && o.tif === 'IOC' && o.qty === pos.pos && o.px === 6200, 'FLAT sends an IOC for the whole position at the bid minus the cap', { side: o.side, qty: o.qty, px: o.px });
  check(posted.length === 1 && posted[0][0] === '/api/order' && posted[0][1].reduce_only === true, 'FLAT is reduce_only, so it can never flip the position', posted.map(x => x[1]));
  await run(400);
  eq(st.position(natKey).pos, 0, 'flat');
  await run(11000, 250);
  const mo = P.$('m-fills-body').children[0].children[2].children.find(r => r.children[7].textContent !== '');
  check(mo && /^[+−]?\d+\.\d$/.test(mo.children[7].textContent), 'MO10 fills in once matured (cents per contract, signed)', mo && mo.children[7].textContent);
  check(P.$('m-fills-body').children[0].children[2].children.every(r => r.getAttribute('data-mkt') === null || st.fills.find(f => f.fid === r.getAttribute('data-k')).own === 'MAN'), 'FILLS lists manual fills only');

  // ---- takeover and release -------------------------------------------------------------------
  const cuKey = Array.from(st.markets.values()).find(m => m.kind === 'LIP15' && m.status === 'OPEN' && /COPPER/.test(m.tk)).key;
  D.focus.set(cuKey, { ladder: 'B', other: true });
  D.act.start();
  await run(6000, 100);
  const sysThere = () => { let n = 0; st.orders.forEach(x => { if (x.key === cuKey && x.own === 'SYS' && !x.terminal) n++; }); return n; };
  check(sysThere() > 0, 'Systematic quotes copper once started');
  B.shownKey === cuKey || await run(300);
  const headB = P.$('m-lad-b-head');
  check(/SYS/.test(headB.children[0].children[4].textContent), 'the ladder shows SYS while Systematic quotes there', headB.children[0].textContent);
  check(rowsOf(B.tb).some(r => /\bsys\b/.test(r.children[0].className + r.children[5].className)), 'Systematic\'s orders show grey on the ladder');
  const cb = st.book(cuKey), cg = st.market(cuKey).g;
  click(ladCell(P, 'B', D.grid.step(cg, cb.bb, -5), 'bq'));
  await run(600);
  check(sysThere() === 0, 'a manual order cancels Systematic\'s orders there (TAKEOVER)');
  let sawTake = false; st.orders.forEach(x => { if (x.key === cuKey && x.why === 'TAKEOVER') sawTake = true; });
  check(sawTake, 'with why TAKEOVER');
  check(/MAN/.test(headB.children[0].children[4].textContent) && !/\boff\b/.test(headB.children[0].children[5].className), 'the market is MAN with a REL button');
  eq(st.market(cuKey).man, true, 'the backend reports it held (mkt.man)');
  await run(3000, 100);
  eq(sysThere(), 0, 'Systematic stays out while it is MAN');
  click(headB.children[0].children[5]);
  await run(300);
  check(!MS.held(cuKey) && st.market(cuKey).man === false, 'REL hands it back (mkt.man false)');
  await run(1000, 100);
  check(!MS.held(cuKey), 'and it stays released (the TAKEOVER orders still in the store do not re-take it)');
  D.act.stop();
  await run(500);

  // ---- stale and KILL: new orders off, cancels live ------------------------------------------
  P.sim().setFeed('KX', false);
  await run(1500);
  check(st.vstale.K === 'KX DOWN', 'Kalshi feed down');
  check(E.buy.disabled && E.blk.textContent === 'KX DOWN', 'the ticket disables with the reason', E.blk.textContent);
  check(/KX DOWN/.test(head.children[0].textContent), 'the ladder header shows the reason');
  n0 = st.orders.size;
  click(ladCell(P, 'A', 6200, 'bq'));
  check(st.orders.size === n0 && /KX DOWN/.test(st.alerts[0].msg), 'ladder clicks refuse while the feed is down');
  P.sim().setFeed('KX', true);
  await run(2500);
  check(!st.vstale.K && !E.buy.disabled, 'feed back: trading again', { vk: st.vstale.K });
  click(ladCell(P, 'A', 6100, 'bq'));
  const cidK = newest(P, x => x.own === 'MAN').cid;
  await run(300);
  D.act.kill();
  await run(300);
  check(E.buy.disabled && E.blk.textContent === 'KILLED', 'KILL: the ticket is locked');
  eq(st.orders.get(cidK).st, 'CXL', 'KILL cancelled the working order');
  n0 = st.orders.size;
  click(ladCell(P, 'A', 6200, 'bq'));
  check(st.orders.size === n0 && /KILLED/.test(st.alerts[0].msg), 'ladder clicks refuse during the lockout');
  await run(61000, 500);
  D.act.rearm();
  await run(500);
  check(!E.buy.disabled, 're-armed: the ticket is live again');

  // ---- WIDE: a third ladder -------------------------------------------------------------------
  P.setWide(true);
  await run(500);
  check(D.layout.ladders().length === 3 && D.focus.ladders.C, 'WIDE mode has a third ladder with a market');
  check(rowsOf(MS.ladders.C.tb).length === 38 && MS.ladders.C.shownKey === D.focus.ladders.C, 'ladder C renders its rows');
  eq(st.book(D.focus.ladders.C).depth, 'FULL', 'ladder C streams full depth');
  P.setWide(false);
  await run(300);

  // ---- 2400 px and wider: a fourth ladder, and SPLIT ------------------------------------------
  P.setXWide(true);
  await run(500);
  check(D.layout.ladders().length === 4 && D.focus.ladders.D && MS.ladders.D.shownKey === D.focus.ladders.D && rowsOf(MS.ladders.D.tb).length > 0,
    'from 2400 px a fourth ladder shows a market', D.focus.ladders);
  eq(P.sim().state.subs.ladders.length, 4, 'the stream carries all four ladders at full depth');
  eq(P.doc.body.getAttribute('data-splitok'), '1', 'the SPLIT button is offered');
  click(P.$('tab-split')); await run(300);
  check(D.layout.split && P.doc.body.getAttribute('data-split') === '1' && D.render.tab === null, 'SPLIT shows both tabs, every renderer live');
  check(D.layout.ladders().join() === 'A' && P.sim().state.subs.ladders.length === 1, 'each half lays out narrow: one ladder', D.layout.ladders());
  check(D.systematic._t.mode() === 'n', 'SYSTEMATIC uses its narrow columns in its half');
  st.lanes.get('M1').pendOn = null; D.render.mark('lane'); await run(300);
  check(P.$('s-lanes-body').children[0] && P.$('s-lanes-body').children[0].children[2].children.length > 0, 'the SYSTEMATIC half renders while MANUAL is live too');
  {
    A.el.focus();
    D.keys.setHK(true);
    const n1 = st.orders.size;
    key('KeyB', 'b');
    check(st.orders.size === n1 + 1, 'MANUAL hotkeys work in SPLIT (B with the ladder focused)');
    D.keys.setHK(false);
    D.act.cancelAll('MAN', null); await run(300);
  }
  eq(D.storage.get('split'), '1', 'SPLIT is remembered');
  click(P.$('tab-man')); await run(300);
  check(!D.layout.split && D.render.tab === 'manual' && P.doc.body.getAttribute('data-tab') === 'manual' && D.layout.ladders().length === 4, 'picking a tab leaves SPLIT');
  P.setXWide(false);
  await run(300);

  // ---- zoom on a 0.1c grid --------------------------------------------------------------------
  {
    const dk = Array.from(st.markets.values()).find(m => m.status === 'OPEN' && m.g.minStep === 10 && m.g.dp === 3 && m.g.levels.length > 900);
    check(!!dk, 'the demo lists a 0.1c-grid market');
    D.focus.set(dk.key, { ladder: 'B', other: true });
    await run(500);
    const zb = P.$('m-lad-b-head').children[0].children[8];
    check(!/\boff\b/.test(zb.className) && zb.textContent === 'x1', 'a 0.1c ladder offers zoom');
    click(zb); await run(300);
    eq(zb.textContent, 'x5', 'zoom x5');
    let px5 = rowsOf(B.tb).map(r => +r.getAttribute('data-k'));
    check(px5.length === 38 && px5.every(p => p % 50 === 0) && px5[0] - px5[1] === 50, 'x5 rows are half cents', px5.slice(0, 3));
    click(zb); await run(300);
    px5 = rowsOf(B.tb).map(r => +r.getAttribute('data-k'));
    check(px5.every(p => p % 100 === 0) && px5[0] - px5[1] === 100, 'x10 rows are whole cents');
    const shownBid = rowsOf(B.tb).reduce((t, r) => t + (+r.children[1].textContent || 0), 0);
    const b3 = st.book(dk.key), lo = Math.min.apply(null, px5), inWin = b3.bids.filter(l => l[0] >= lo).reduce((t, l) => t + l[1], 0);
    check(Math.abs(shownBid - inWin) < 0.05, 'zoomed rows add up the book\'s sizes', { shownBid, inWin });
    const bbRowEl = rowsOf(B.tb).find(r => /\bbb\b/.test(r.children[3].className));
    check(bbRowEl && +bbRowEl.getAttribute('data-k') === Math.floor(b3.bb / 100) * 100, 'the best bid marks its floor row', bbRowEl && bbRowEl.getAttribute('data-k'));
    click(zb); await run(300);
    eq(zb.textContent, 'x1', 'zoom cycles back to x1');
  }

  // ---- the 15-minute roll ---------------------------------------------------------------------
  D.focus.set(natKey, { ladder: 'A' });
  await run(300);
  const closeAt = st.market(natKey).close_ms;
  await run(closeAt - P.clock.t + 3000, 1000);
  const nk = D.focus.ladders.A;
  check(nk !== natKey && st.market(nk) && st.market(nk).status === 'OPEN' && D.manual.logic.series(st.market(nk).tk) === 'KXNATGAS15M', 'ladder A rolls to the next natural gas window', nk);
  await run(500);
  eq(A.shownKey, nk, 'and shows it');

  // ---- no errors anywhere ---------------------------------------------------------------------
  eq(D.render.errors, 0, 'no render errors' + (D.render.lastError ? ': ' + D.render.lastError : ''));
  eq(st.invalid, 0, 'no contract violations' + (st.lastInvalid ? ': ' + st.lastInvalid : ''));
  check(P.errors.length === 0, 'no console errors', P.errors.slice(0, 3));
  check(D.keys.sheetRows().some(r => r.scope === 'ladder' && /IOC at the ask/.test(r.desc) && r.order), 'the ? sheet lists the ladder hotkeys as HK keys');

  // =============================================================================================
  // 3. Polymarket read-only
  // =============================================================================================
  {
    const Q = bootPage({ pmRO: true, seed: 5 });
    await Q.run(1500);
    const s2 = Q.D.store;
    const pk = Array.from(s2.markets.values()).find(m => m.v === 'P' && m.status === 'OPEN').key;
    Q.D.focus.set(pk);
    await Q.run(500);
    eq(Q.$('m-lad-a').getAttribute('data-ro'), '1', 'a Polymarket ladder is read-only while the key fails');
    check(/READ-ONLY: PM key/.test(Q.$('m-lad-a-foot').textContent), 'its footer says READ-ONLY: PM key');
    const L2 = Q.D.manual.state().ladders.A;
    const r = rowsOf(L2.tb)[10];
    const n1 = s2.orders.size;
    Q.click(r.children[1]);
    check(s2.orders.size === n1 && /RO/.test(s2.alerts[0].msg), 'and refuses clicks');
    check(Q.D.manual.state().ticket.el.buy.disabled && Q.D.manual.state().ticket.el.blk.textContent === 'RO', 'the ticket too');
    eq(Q.D.render.errors, 0, 'no render errors (read-only)');
  }

  console.log(fails ? `${fails} of ${checks} checks FAILED` : `ui desk v3 manual: ${checks} checks, 0 failures`);
  process.exit(fails ? 1 : 0);
})().catch(e => { console.log('FAIL: test crashed: ' + (e && e.stack || e)); process.exit(1); });
