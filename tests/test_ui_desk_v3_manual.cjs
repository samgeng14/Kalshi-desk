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
  const mem = Object.assign({}, opts.storage || {});
  const ctx = {
    console: { log: console.log, warn: console.warn, debug() {}, error: (...a) => { errors.push(a.map(x => (x && x.stack) || String(x)).join(' ')); } },
    setTimeout, clearTimeout, setInterval, clearInterval, setImmediate, performance, Promise, Map, Set, WeakSet, Uint8Array,
    document: doc, location: { protocol: 'file:', search: opts.search || '' },
    localStorage: { getItem(k) { return k in mem ? mem[k] : null; }, setItem(k, v) { mem[k] = String(v); } },
    matchMedia, requestAnimationFrame: f => { rafs.push(f); }, confirm: () => true, crypto: require('crypto').webcrypto,
    AbortController, fetch: () => Promise.reject(new Error('no network in tests')), __clock: clock
  };
  if (opts.Notification) ctx.Notification = opts.Notification;
  if (opts.AudioContext) ctx.AudioContext = opts.AudioContext;
  ctx.window = ctx;
  ctx.addEventListener = () => {};
  vm.createContext(ctx);
  vm.runInContext('Date.now = function () { return __clock.t; };', ctx);
  const load = f => vm.runInContext(fs.readFileSync(path.join(JS, f), 'utf8'), ctx, { filename: f });
  ['contract.js', 'core.js', 'store.js', 'sim.js', 'transport.js', 'universe.js', 'activity.js', 'header.js', 'manual.js', 'browse.js', 'systematic.js'].forEach(load);
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
function Desk_step(D, m, px, n) { return D.grid.step(m.g, px, n); }
function manOrders(P, key) { const a = []; P.D.store.orders.forEach(o => { if (o.own === 'MAN' && (!key || o.key === key)) a.push(o); }); return a; }

(async () => {
  // =============================================================================================
  // 1. pure logic
  // =============================================================================================
  {
    const ctx = { console, setTimeout, clearTimeout, setInterval, clearInterval, performance, localStorage: { getItem() { return null; }, setItem() {} } };
    ctx.window = ctx; vm.createContext(ctx);
    for (const f of ['contract.js', 'core.js', 'store.js', 'universe.js', 'manual.js']) vm.runInContext(fs.readFileSync(path.join(JS, f), 'utf8'), ctx, { filename: f });
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
    eq(L.woText(8, 0, 840, false), '8 14m', 'a long expiry (until close) counts in minutes');
    eq(L.woText(1, 0, 402, false), '1 ' + D.fmt.left(402000), 'in whole minutes, as the ticket and WORKING count time to close (6m42s: 6m, not 7m)');
    eq(L.woText(8, 0, 100, false), '8 1m', 'from 100 s');
    eq(L.woText(8, 0, 3 * 3600 + 300, false), '8 3h', 'and in hours past 100 minutes');
    eq(L.woText(1000, 0, 840, false), '1000', 'it still never overflows the cell');
    {
      const T = Date.UTC(2026, 9, 6, 23, 0, 0);
      eq(L.closeExp({ status: 'OPEN', close_ms: T + 3600500 }, T), T + 3600000, 'CLOSE: the market close, cut to a whole second');
      eq(L.closeExp({ status: 'OPEN', close_ms: T + 4000 }, T), null, 'CLOSE: refused under 5 s before the close');
      eq(L.closeExp({ status: 'CLOSED', close_ms: T + 600000 }, T), null, 'CLOSE: only on an open market');
      // 2026-10-06 review: the backend refuses an expiry under 5 s ahead at receipt; the page asks
      // 6 s, so a click 5.0-5.9 s before the close is refused here (CLOSING), never REJ BAD_REQUEST
      eq(L.closeExp({ status: 'OPEN', close_ms: T + 5500 }, T), null, 'CLOSE: refused under 6 s before the close');
      eq(L.closeExp({ status: 'OPEN', close_ms: T + 6000 }, T), T + 6000, 'CLOSE: taken from 6 s');
      eq(L.startExp({ start_ms: T + 5900 }, T), null, 'START: the same 6 s margin');
      eq(L.expShort(12000), '12s', 'a short expiry counts in seconds');
      eq(L.expShort(3 * 3600e3 + 5 * 60e3), '3h05m', 'a long one reads as time to close');
    }
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
    ci = L.costInfo(mk, { pos: 12, avg: 5000 }, 'SELL', 6000, 10, 'T', [{ side: 'SELL', px: 5900, left: 10 }]);
    check(ci.reserve === 8 * 4000, 'a sell next to a working sell: the +12 long covers 10 + 2, the other 8 reserve 1 - px (the gateway\'s rule)', ci);
    ci = L.costInfo(mk, null, 'BUY', 5800, 70, 'M');
    check(ci.fee === 0 && ci.cost === 406000 && ci.need === 406000 + D.fees.estimate('K', QUAD, 'BUY', 5800, 70, 'T'), 'a post: no maker fee, but the cash check (need) holds the taker fee as the gateway does', ci);
    eq(L.c2(175), '1.75', 'fee per contract in cents');
    eq([12, -16.4, 1.6, -0.5, 0.04, 9.96, -100].map(L.posShort).join(' '), '+12 ' + M + '16 +1.6 ' + M + '0.5 +.04 +10 ' + M + '100', 'watchlist positions fit 4 characters');
    eq(L.c2(-31.25), M + '0.31', 'a rebate is negative with U+2212');
    eq(L.series('KXNATGAS15M-26OCT061930-30'), 'KXNATGAS15M', 'the 15-minute series of a ticker');
    // watchlist order (THE UNIVERSE): category (SPORTS WEATHER FIN ECON OTHER), each category's
    // groups by their best row under the sort, rows by the sort, ties by ticker
    const UV = D.universe;
    const mm = (v, tk, grp, kind, close, short, status, extra) => Object.assign({ v, tk, key: v + '|' + tk, grp, kind, close_ms: close, short: short || tk, title: tk, status: status || 'OPEN', og: grp, oc: short }, extra || {});
    const ms = new Map([
      ['K|W1', mm('K', 'W1', 'WX NYC', 'WEATHER', 9, 'NYC 64-65')], ['K|G1', mm('K', 'G1', 'NCAAF', 'GAME', 5, 'A@B A')],
      ['P|g1', mm('P', 'g1', 'NCAAF', 'GAME', 5, 'A@B A')], ['K|N1', mm('K', 'N1', '15M', 'LIP15', 3, 'NATGAS UP')],
      ['K|C1', mm('K', 'C1', '15M', 'LIP15', 3, 'COPPER UP')], ['K|S1', mm('K', 'S1', '15M', 'LIP15', 2, 'OLD', 'SETTLED')]
    ]);
    const rowsOfMs = f => Array.from(ms.values()).filter(m => m.status !== 'SETTLED' && (!f || f(m))).map(m => UV.liveRow(m, null));
    let o = UV.order(rowsOfMs(), 'close');
    let wl = UV.layout(o.groups, {}).map(x => x.t === 'cat' ? x.cat : (x.t === 'grp' ? x.gk : o.rows[x.i].tk));
    eq(wl.join(','), 'SPORTS,NCAAF A@B,G1,g1,WEATHER,WX NYC,W1,FIN,15M,C1,N1', 'watchlist: sports, weather, fin; a game is its own group; settled markets gone; ties by ticker');
    eq(UV.layout(o.groups, {})[0].n, 2, 'category rows carry their count');
    eq(o.cats.FIN, 2, 'and the counts per category');
    o = UV.order(rowsOfMs(m => m.v === 'P'), 'close');
    eq(UV.layout(o.groups, {}).map(x => x.t === 'row' ? o.rows[x.i].tk : (x.gk || x.cat)).join(','), 'SPORTS,NCAAF A@B,g1', 'watchlist venue filter');
    o = UV.order(rowsOfMs(), 'close');
    wl = UV.layout(o.groups, { '15M': true });
    check(wl.find(x => x.gk === '15M').collapsed && !wl.some(x => x.t === 'row' && o.rows[x.i].grp === '15M'), 'a collapsed group hides its markets');
    wl = UV.layout(o.groups, { 'c:SPORTS': true });
    check(wl[0].collapsed && wl[1].cat === 'WEATHER', 'a collapsed category hides its groups');
    wl = UV.layout(o.groups, { '15M': true }, { q: 'copper' });
    check(!wl.find(x => x.gk === '15M').collapsed, 'a search shows inside collapsed groups');
    const vr = [{ tk: 'a', gk: 'G', cat: 'FIN', vol: 5, bid: 4000, ask: 4600, close_ms: 9, edge: null }, { tk: 'b', gk: 'G', cat: 'FIN', vol: 50, bid: 4000, ask: 4100, close_ms: 7, edge: 120 },
                { tk: 'c', gk: 'H', cat: 'FIN', vol: 20, bid: null, ask: 4100, close_ms: 1, edge: -50 }];
    eq(UV.order(vr, 'vol').rows.map(r => r.tk).join(''), 'bac', 'sort by volume: the busiest group first, its busiest row first');
    eq(UV.order(vr, 'sprd').rows.map(r => r.tk).join(''), 'bac', 'sort by spread: tightest first, a one-sided book last');
    eq(UV.order(vr, 'close').rows.map(r => r.tk).join(''), 'cba', 'sort by time to close');
    eq(UV.order(vr, 'edge').rows.map(r => r.tk).join(''), 'bac', 'sort by edge: the best take first, no model last');
    eq(UV.catOf({ kind: 'LIP15' }) + UV.catOf({ kind: 'GAME' }) + UV.catOf({ cat: 'ECON', kind: 'OTHER' }), 'FINSPORTSECON', 'a market\'s category (mkt.cat, or from its kind)');
    eq(L.pairOf(ms, ms.get('K|G1')), 'P|g1', 'the same outcome on the other venue');
    eq(L.watchLabel(mm('K', 'X', 'NCAAF', 'GAME', 1, 'SOUMIS@TROY TROY')), 'TROY', 'a game row reads as its YES team');
    eq(L.watchLabel(mm('P', 'x', 'NCAAF', 'GAME', 1, 'SOUMIS@TROY SOUMIS')), 'SOUMIS P', 'a Polymarket row carries P');
    eq(L.watchLabel(mm('K', 'X', 'WX NYC 10/06', 'WEATHER', 1, 'NYC 64-65')), '64-65', 'a weather row reads as its bracket');
    eq(L.watchLabel(mm('K', 'X', '15M', 'LIP15', 1, 'NATGAS 19:30 UP')), 'NATGAS', 'a 15-minute row reads as its series');
    eq(L.watchLabel(mm('K', 'X', 'OTHER', 'OTHER', 1, 'FED HOLD')), 'FED HOLD', 'anything else keeps its short label');
    eq(L.watchGroup(mm('K', 'X', 'NCAAF', 'GAME', 1, 'SOUMIS@TROY TROY')), 'NCAAF SOUMIS@TROY', 'a game is its own group');
    eq(L.pairOf(ms, ms.get('K|N1')), null, 'no pair for a Kalshi-only market');
    // the layout is cached on the listing's shape: a redraw reuses it
    {
      const cache = {}, gr = [{ gk: 'G', cat: 'FIN', n: 3 }, { gk: 'H', cat: 'FIN', n: 2 }];
      const l1 = UV.layoutOf(cache, gr, 's1', {}, ''), items1 = l1.items;
      UV.layoutOf(cache, gr, 's1', {}, ''); UV.layoutOf(cache, gr, 's1', { H: false }, '');
      check(cache.builds === 1 && cache.items === items1 && l1.rowItems.join() === '2,3,4,6,7', 'the layout is built once per shape: redraws reuse it', { builds: cache.builds, rows: l1.rowItems.join() });
      UV.layoutOf(cache, gr, 's1', { H: true }, '');
      check(cache.builds === 2 && cache.rowItems.join() === '2,3,4', 'a collapsed group rebuilds it');
      UV.layoutOf(cache, gr, 's2', { H: true }, '');
      eq(cache.builds, 3, 'and so does a new shape');
    }
    // one paged view on a transport whose replies the test releases one by one
    {
      const reqs = [];
      D.transport = { get: p => new Promise(res => reqs.push({ p, res })) };
      const tick = () => new Promise(r => setImmediate(r));
      const grp = (n1, n2) => [{ gk: 'G', cat: 'FIN', n: n1 }, { gk: 'H', cat: 'FIN', n: n2 }];
      const rowsFor = (off, n, tag) => Array.from({ length: n }, (_, i) => ({ v: 'K', tk: tag + (off + i), ev: 'E', title: 't', short: tag + (off + i), cat: 'FIN', grp: 'G', gk: 'G', kind: 'OTHER',
        date: '2026-10-06', close_ms: 1791306000000, bid: '0.4000', ask: '0.4200', sprd: '0.0200', vol: 1, fee: { type: 'QUAD', mult: 1 }, fair: null, edge: null, pin: false, live: false }));
      const answer = (rq, tag, grev, groups, total) => {
        const off = +/offset=(\d+)/.exec(rq.p)[1];
        rq.res({ status: 200, json: { ok: true, day: 'T', date: '2026-10-06', total, offset: off, rows: rowsFor(off, Math.min(100, total - off), tag), groups, grev, cats: { SPORTS: 0, WEATHER: 0, FIN: total, ECON: 0, OTHER: 0 }, ts_ms: 1 } });
      };
      let changes = 0;
      const V = UV.create({ onChange: () => { changes++; } });
      V.set({ day: 'T', cat: 'FIN', v: '', q: '', sort: 'vol' });
      eq(reqs.length, 1, 'a new query asks for its first page');
      answer(reqs.shift(), 'a', 'g1', grp(150, 50), 200); await tick();
      check(V.res && V.res.total === 200 && V.row(0).tk === 'a0' && !V.waiting(), 'its first page lands');
      V.want(90, 130);
      check(reqs.length === 1 && /offset=100/.test(reqs[0].p) && /grev=g1/.test(reqs[0].p), 'the next page is asked with the shape the page holds (grev)', reqs.map(r => r.p));
      answer(reqs.shift(), 'a', 'g1', null, 200); await tick();
      check(V.row(120).tk === 'a120' && V.res.groups.length === 2, 'a reply without groups (same grev) keeps the groups the page holds');
      // typing: the old result stays drawn until the new query's first page lands
      V.set({ q: 'x' });
      check(V.waiting() && V.res && V.row(0).tk === 'a0', 'a new search keeps the previous result drawn (waiting)');
      const tStart = Date.now(); while (Date.now() - tStart < 170) await tick();
      V.want(0, 100);
      check(reqs.length === 1 && /q=x/.test(reqs[0].p) && !/grev=/.test(reqs[0].p), 'once the typing rests, the new query\'s first page is asked (no grev: a new result)', reqs.map(r => r.p));
      check(V.row(0).tk === 'a0', 'still the old rows while it is out');
      answer(reqs.shift(), 'x', 'gx', grp(30, 0).slice(0, 1), 30); await tick();
      check(!V.waiting() && V.res.total === 30 && V.row(0).tk === 'x0' && V.row(100) === null, 'its first page swaps the list in at once');
      // the listing changes shape on a refresh: the pages on screen are swapped in together
      V.set({ q: '' }); while (Date.now() - tStart < 400) await tick(); V.want(0, 100);
      answer(reqs.shift(), 'a', 'g1', grp(150, 50), 200); await tick();
      V.want(90, 130); answer(reqs.shift(), 'a', 'g1', null, 200); await tick();
      V.lastAt = 0; V.tick(Date.now());
      check(reqs.length === 2 && reqs.every(r => /grev=g1/.test(r.p)), 'every 3 s the pages on screen are asked again', reqs.map(r => r.p));
      const r0 = reqs.shift(), r1 = reqs.shift();
      answer(r0, 'b', 'g2', grp(149, 50), 199); await tick();
      check(V.res.grev === 'g1' && V.row(0).tk === 'a0' && V.row(120).tk === 'a120' && V.nx && V.nx.res.grev === 'g2', 'a page of a new shape waits: the old pages stay drawn, none goes blank', { grev: V.res.grev, r120: V.row(120) && V.row(120).tk });
      answer(r1, 'b', 'g2', grp(149, 50), 199); await tick();
      check(V.res.grev === 'g2' && V.row(0).tk === 'b0' && V.row(120).tk === 'b120' && !V.nx, 'once every page on screen has the new shape, they swap in together');
      delete D.transport;
    }
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
  {
    // a snapshot whose subs differ from the ladders on screen makes the page subscribe again
    const want = Desk => Desk.layout.ladders().map(sl => Desk.focus.ladders[sl]).filter(Boolean).length;
    eq(D.subs.check(JSON.parse(JSON.stringify(P.sim().state.subs))), false, 'subs that match the ladders on screen send nothing');
    P.sim().state.subs = { ladders: [], tops: 'ALL' };          // the backend lost them (a restart, a sid reset)
    eq(D.subs.check({ ladders: [], tops: 'ALL' }), true, 'a snapshot without them is noticed');
    await run(300);
    eq(P.sim().state.subs.ladders.length, want(D), 'and the subscription is sent again');
  }
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
  click(ladCell(P, 'A', 6700, 'bq'), { detail: 2 });
  eq(st.orders.size, n0, 'a double-click\'s second click never confirms the guard');
  await run(100);
  click(ladCell(P, 'A', 6700, 'bq'));
  eq(st.orders.size, n0, 'nor does a separate click under 0.35 s after the arming one');
  await run(300);
  click(ladCell(P, 'A', 6700, 'bq'));
  eq(st.orders.size, n0 + 1, 'a separate second click on the armed cell sends it (override)');
  o = newest(P, x => x.own === 'MAN');
  await run(250);
  check(st.orders.get(o.cid).st !== 'REJ', 'the override was accepted by the venue', st.orders.get(o.cid).why);
  // the backend's touch is not ours: its PRICE_BAND refusal arms the cell for a second click
  {
    const lb = st.book(natKey), ba0 = lb.ba;
    lb.ba = 6600;                                   // our book says 0.66 (stale); the venue's ask is 0.64
    n0 = st.orders.size;
    click(ladCell(P, 'A', 6700, 'bq'));
    lb.ba = ba0;
    eq(st.orders.size, n0 + 1, 'a buy 1 tick through our (stale) ask passes our guard and is sent');
    const cb = newest(P, x => x.own === 'MAN');
    await run(250);
    check(st.orders.get(cb.cid).st === 'REJ' && st.orders.get(cb.cid).why === 'PRICE_BAND', 'the venue refuses it PRICE_BAND', st.orders.get(cb.cid).why);
    P.frame();
    check(/\barm\b/.test(ladCell(P, 'A', 6700, 'bq').className), 'the refusal arms the cell (amber)');
    await run(400);
    click(ladCell(P, 'A', 6700, 'bq'));
    const ov = newest(P, x => x.own === 'MAN');
    await run(250);
    check(ov.cid !== cb.cid && st.orders.get(ov.cid).st !== 'REJ', 'the next separate click sends it with override', st.orders.get(ov.cid) && st.orders.get(ov.cid).why);
  }
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
  check(st.orders.get(cidDrag).local.cxlPend && st.orders.size === n0, 'drag: the cancel goes first; nothing new until it is confirmed');
  await run(250);
  const nRe = newest(P, x => x.own === 'MAN');
  check(st.orders.get(cidDrag).st === 'CXL' && st.orders.size === n0 + 1 && nRe.px === 6000 && nRe.tif === 'POST' && nRe.qty === st.orders.get(cidDrag).qty - st.orders.get(cidDrag).filled,
    'then a new post-only order for what the cancel took off', { px: nRe.px, tif: nRe.tif, qty: nRe.qty });
  check(/queue place lost/.test(st.alerts[0].msg), 'the status bar says queue place is lost');
  // a fill that races the cancel is not sent again
  {
    click(ladCell(P, 'A', 6100, 'bq'));
    const cr = newest(P, x => x.own === 'MAN').cid;
    await run(250);
    fire(ladCell(P, 'A', 6100, 'wb'), 'mousedown', { button: 0 });
    fire(ladCell(P, 'A', 5900, 'wb'), 'mousemove', { button: 0 });
    const n1 = st.orders.size;
    fire(ladCell(P, 'A', 5900, 'wb'), 'mouseup', { button: 0 });
    // the market trades our 0.61 bid before the cancel lands at the venue
    P.sim().freeze(natKey, { bids: [[6000, 50]], asks: [[6400, 80]] });
    P.sim().trade(natKey, 'SELL', 5);
    await run(300);
    check(st.orders.get(cr).st === 'FILL' && st.orders.size === n1, 'the order filled before its cancel: no re-price is sent', { st: st.orders.get(cr).st, n: st.orders.size - n1 });
    P.sim().freeze(natKey, { bids: [[6300, 120], [6200, 85], [6100, 40]], asks: [[6400, 80], [6500, 210], [6700, 12]] });
    await run(300);
    const ext = JSON.parse(JSON.stringify(D.contract.examples.ord));
    ext.cid = 'X-drag-ext1'; ext.tk = nat.tk; ext.v = 'K'; ext.own = 'EXT'; ext.st = 'WORK'; ext.filled = 0; ext.left = 2; ext.qty = 2; ext.px = '0.5800'; ext.exp_ms = null; ext.rev = 1;
    st._handlers.ord(ext); P.frame();
    fire(ladCell(P, 'A', 5800, 'wb'), 'mousedown', { button: 0 });
    check(!A.drag, 'an EXT order (placed outside the desk) cannot be dragged');
    fire(ladCell(P, 'A', 5800, 'wb'), 'mouseup', { button: 0 });
    ext.st = 'CXL'; ext.left = 0; ext.why = 'USER'; ext.rev = 2; st._handlers.ord(ext);
  }
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
  {
    click(fbtn('CL'));
    eq(A.cfg.ttl, 'CLOSE', 'the ladder offers CL: post until the market closes');
    click(ladCell(P, 'A', 6000, 'bq'));
    const oc = newest(P, x => x.own === 'MAN');
    check(oc.tif === 'POST' && oc.exp_ms === Math.floor(nat.close_ms / 1000) * 1000, 'a CL click posts with the market close as its expiry', { exp: oc.exp_ms, close: nat.close_ms });
    await run(300);
    check(/^5 \d+m$/.test(ladCell(P, 'A', 6000, 'wb').textContent), 'the ladder counts it down to the close in minutes', ladCell(P, 'A', 6000, 'wb').textContent);
    D.act.cancel([oc.cid]); await run(300);
    click(fbtn('60s'));
  }
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

  // ---- watchlist (THE UNIVERSE: T / T+1, categories, star, virtual rows) ---------------------
  const wtb = MS.watch.tb, WE = MS.watch.el;
  const wrows = () => wtb.children.filter(r => r.getAttribute('data-mkt'));
  const grps = wtb.children.filter(r => r.getAttribute('data-grp'));
  eq(MS.cfg.watch.scope + ' ' + MS.cfg.watch.day, 'watch T', 'the watchlist opens on the starred markets settling today (T)');
  check(grps.length >= 4 && grps[0].getAttribute('data-grp') === 'c:SPORTS' && /^SPORTS \d+$/.test(grps[0].textContent) && grps.some(r => /^15M 7$/.test(r.textContent)),
    'category rows (SPORTS first) with their counts, then groups; the 15-minute series is one group', grps.map(r => r.textContent));
  const natRow = wtb.children.find(r => r.getAttribute('data-mkt') === natKey);
  check(natRow && natRow.children[2].textContent === '0.62' && natRow.children[3].textContent === '0.64', 'watchlist bid / ask', natRow && natRow.children.map(c => c.textContent));
  check(natRow && natRow.children[0].textContent === '★' && /\bon\b/.test(natRow.children[0].className), 'a desk market starts pinned (the star, white)');
  check(natRow && /\bsel\b/.test(natRow.className), 'the focused market is selected in the watchlist');
  check(natRow && /^\d+m$|^\d+s$/.test(natRow.children[7].textContent), 'T is the time to close for a 15-minute market', natRow && natRow.children[7].textContent);
  const gRow = wtb.children.find(r => r.getAttribute('data-mkt') === gameKey);
  check(gRow && /^(Q\d|OT) \d+:\d\d$/.test(gRow.children[7].textContent), 'T is the game clock for a live game', gRow && gRow.children[7].textContent);
  {
    const gm = st.market(gameKey), gg = D.manual.logic.watchGroup(gm);
    const gh = wtb.children.find(r => r.getAttribute('data-grp') === gg);
    check(gh && /^NCAAF [A-Z]+@[A-Z]+ \d+  \d+-\d+$/.test(gh.textContent), 'a live game is its own group row with the score', gh && gh.textContent);
    eq(gRow.children[1].textContent, gm.oc, 'inside it the row reads as the YES team');
  }
  check(wrows().some(r => r.children[5].textContent !== ''), 'ESPN shows on live games');
  check(wrows().some(r => /\b(fair|warn)\b/.test(r.children[4].className) && r.children[4].textContent !== ''), 'model fair shows in the watchlist');
  // amber rule: a taker at the touch beats the model after the fee (never the mid)
  let amberN = 0;
  const amberOk = wrows().every(r => {
    const k = r.getAttribute('data-mkt'), b = st.book(k), m = st.market(k);
    if (!b || b.ref.fair === null) return true;
    const f = b.ref.fair, buy = b.ba !== null ? f - b.ba - D.fees.takerPer(m.v, m.fee, b.ba) : -1, sell = b.bb !== null ? b.bb - f - D.fees.takerPer(m.v, m.fee, b.bb) : -1;
    const want = buy > 0 || sell > 0;
    if (want) amberN++;
    return /\bwarn\b/.test(r.children[4].className) === want;
  });
  check(amberOk, 'Fair is amber exactly when a take at the touch clears the taker fee', amberN);
  {
    const L2 = D.manual.logic, QUAD = { type: 'QUAD', mult: 1 };
    eq(L2.takeEdge('K', QUAD, 6100, 6100, 6700) && L2.takeEdge('K', QUAD, 6100, 6100, 6700).e < 0, true, 'ILST-like: bid 0.61 / ask 0.67, fair 0.61: no take clears the fee (not amber)');
    const te = L2.takeEdge('K', QUAD, 7000, 6100, 6700);
    check(te.side === 'BUY' && te.e > 0, 'fair 0.70 over an ask of 0.67: buying at the ask clears the fee', te);
  }
  // click loads into the focused ladder, Shift+click into the other
  const wxRow = wrows().find(r => st.market(r.getAttribute('data-mkt')).kind === 'WEATHER');
  const wxKey = wxRow.getAttribute('data-mkt');
  click(wxRow.children[1], { shiftKey: true });
  check(D.focus.ladders.B === wxKey && D.focus.key === natKey, 'Shift+click loads the other ladder and keeps the focus');
  await run(300);
  eq(st.book(wxKey).depth, 'FULL', 'the newly loaded ladder subscribes at full depth');
  // keyboard: / search, arrows, Enter
  A.el.focus();
  key('Slash', '/');
  eq(P.doc.activeElement, WE.q, '/ focuses the watchlist search');
  WE.q.value = 'copper'; fire(WE.q, 'input', {});
  await run(300);
  const vis = wrows();
  check(vis.length === 1 && /COPPER/.test(vis[0].children[1].textContent), 'search filters the watchlist', vis.map(r => r.children[1].textContent));
  n0 = st.orders.size;
  key('KeyB', 'b');
  eq(st.orders.size, n0, 'typing in the search is never a command');
  key('Enter', 'Enter');
  const copperKey = vis[0].getAttribute('data-mkt');
  check(D.focus.key === copperKey && D.focus.ladders.A === copperKey, 'Enter loads the match into the focused ladder');
  WE.q.value = ''; fire(WE.q, 'input', {});
  key('Escape', 'Escape');
  check(P.doc.activeElement !== WE.q, 'Esc leaves the search');
  await run(300);
  P.$('m-watch').focus();
  key('ArrowDown', 'ArrowDown'); key('ArrowDown', 'ArrowDown'); key('Enter', 'Enter');
  check(D.focus.key && D.focus.key !== copperKey, 'Up/Down and Enter work with the list focused', D.focus.key);
  // group and category collapse
  click(wtb.children.find(r => r.getAttribute('data-grp') === '15M').children[0]);
  await run(300);
  check(MS.cfg.watch.collapsed['15M'] && !wrows().some(r => st.market(r.getAttribute('data-mkt')).grp === '15M'), 'a group row click collapses it');
  click(wtb.children.find(r => r.getAttribute('data-grp') === '15M').children[0]);
  click(wtb.children.find(r => r.getAttribute('data-grp') === 'c:WEATHER').children[0]); await run(300);
  check(!wrows().some(r => st.market(r.getAttribute('data-mkt')).kind === 'WEATHER') && wtb.children.some(r => r.getAttribute('data-grp') === 'c:WEATHER'), 'a category row click collapses the category to its header');
  click(wtb.children.find(r => r.getAttribute('data-grp') === 'c:WEATHER').children[0]); await run(300);
  // venue: K+P, K, P
  eq(WE.ven.textContent, 'K+P', 'venue starts on both');
  click(WE.ven); click(WE.ven); await run(300);
  check(WE.ven.textContent === 'P' && wrows().length && wrows().every(r => r.getAttribute('data-mkt').charAt(0) === 'P'), 'the P filter shows Polymarket only');
  click(WE.ven); await run(300);
  // categories
  const chip = c => WE.cats.find(b => b.getAttribute('data-v') === c);
  click(chip('WEATHER')); await run(300);
  check(wrows().length && wrows().every(r => st.market(r.getAttribute('data-mkt')).cat === 'WEATHER') && /\bon\b/.test(chip('WEATHER').className), 'a category chip shows that category only');
  click(chip('')); await run(300);
  // sort: CLOSE, VOL, SPRD, EDGE
  click(WE.sort); await run(300);
  eq(MS.cfg.watch.sort + ' ' + WE.sort.textContent, 'vol VOL', 'the sort cycles to volume');
  {
    const ks = wrows().map(r => r.getAttribute('data-mkt')), fin = ks.filter(k => st.market(k).grp === '15M').map(k => st.book(k).vol);
    check(fin.every((v, i) => !i || v <= fin[i - 1]), 'inside a group the busiest market comes first', fin);
  }
  click(WE.sort); click(WE.sort); click(WE.sort); await run(300);
  eq(MS.cfg.watch.sort, 'close', 'and round to time to close');
  // T / T+1
  click(WE.day[1]); await run(300);
  check(MS.cfg.watch.day === 'T1' && wrows().length >= 1 && wrows().every(r => st.market(r.getAttribute('data-mkt')).date > st.today), 'T+1 shows the markets settling tomorrow only', wrows().map(r => r.getAttribute('data-mkt') + ' ' + st.market(r.getAttribute('data-mkt')).date));
  click(WE.day[0]); await run(300);
  check(wrows().every(r => st.market(r.getAttribute('data-mkt')).date <= st.today), 'T shows today\'s');
  // the star: unpinning a market nothing holds takes it off the watchlist; the backend agrees
  {
    const wk = wrows().map(r => r.getAttribute('data-mkt')).find(k => st.market(k).kind === 'WEATHER' && !Object.values(D.focus.ladders).includes(k) && !(st.position(k) && st.position(k).pos));
    const star = wrows().find(r => r.getAttribute('data-mkt') === wk).children[0];
    fire(star, 'pointerdown', { button: 0 }); fire(star, 'click', {}); P.frame();
    check(!wrows().some(r => r.getAttribute('data-mkt') === wk), 'unpinning takes it off at once');
    await run(300);
    check(P.sim().market(wk).pin === false && st.market(wk).pin === false && !st.pinPend.has(wk), 'the backend unpinned it (mkt.pin false)');
    click(chip('')); click(WE.scope[1]);
    WE.q.value = st.market(wk).tk; fire(WE.q, 'input', {}); await run(600);
    const back = wrows().find(r => r.getAttribute('data-mkt') === wk);
    check(back && back.children[0].textContent === '☆', 'ALL still lists it, unstarred', back && back.children[0].textContent);
    click(back.children[0]); await run(300);
    check(st.market(wk).pin === true, 'a star in ALL pins it again');
    WE.q.value = ''; fire(WE.q, 'input', {});
    click(WE.scope[0]); await run(300);
    check(wrows().some(r => r.getAttribute('data-mkt') === wk), 'and it is back on the watchlist');
  }
  // ALL: the day's whole universe, paged and virtual
  {
    click(WE.scope[1]); await run(600);
    const total = +WE.n.textContent;
    // (this test's clock is late in the ET day: many of today's games and hours have closed)
    check(MS.cfg.watch.scope === 'all' && total > 500, 'ALL lists the day\'s whole universe (both venues, every category)', WE.n.textContent);
    const drawn = wtb.children.filter(r => !/\bvpad\b/.test(r.className)).length;
    check(drawn <= 70, 'only the rows in view exist (virtual)', drawn);
    const pads = wtb.children.filter(r => /\bvpad\b/.test(r.className)).map(r => parseInt(r.children[0].style.getPropertyValue('height'), 10));
    const items = MS.watch.items.length;
    check(Math.abs(pads.reduce((x, y) => x + y, 0) + (drawn) * 18 - items * 18) < 1, 'the spacer rows keep the whole list\'s height', { pads, drawn, items });
    const gets0 = MS.watch.uni.gets;
    MS.watch.body.scrollTop = 18 * 500; fire(MS.watch.body, 'scroll', {}); await run(600);
    check(MS.watch.uni.gets > gets0 && wrows().length > 10 && wrows().every(r => r.children[1].textContent !== '…'), 'scrolling far down asks for the pages there and draws them', { gets: MS.watch.uni.gets - gets0 });
    check(wtb.children.filter(r => !/\bvpad\b/.test(r.className)).length <= 70, 'still only the rows in view');
    // a listing row (not live): load it into a ladder, its book arrives
    const uniRow = wrows().find(r => /\buni\b/.test(r.className) && r.getAttribute('data-mkt').charAt(0) === 'K');
    check(!!uniRow, 'ALL shows listing rows that are not live', wrows().slice(0, 3).map(r => r.className));
    const uk = uniRow.getAttribute('data-mkt');
    eq(st.market(uk), null, 'a listing row is not in the store');
    click(uniRow.children[1], { shiftKey: true }); await run(600);
    check(st.market(uk) && st.book(uk) && st.book(uk).depth === 'FULL' && D.focus.ladders.B === uk && rowsOf(B.tb).length > 5, 'loaded into a ladder it goes live: its market and full book arrive', { m: !!st.market(uk), b: st.book(uk) && st.book(uk).depth });
    eq(st.market(uk).pin, false, 'a ladder brings it live without pinning it');
    // T+1 in ALL
    click(WE.day[1]); await run(600);
    check(+WE.n.textContent > 500 && MS.watch.uni.res.date > st.today, 'T+1 in ALL lists tomorrow\'s universe', WE.n.textContent);
    click(WE.day[0]); click(WE.scope[0]); await run(300);
    MS.watch.body.scrollTop = 0; fire(MS.watch.body, 'scroll', {});
    D.focus.set(natKey, { ladder: 'A' }); D.focus.set(gameKey, { ladder: 'B', other: true }); await run(300);
  }
  // THE MARKET BROWSER: u (or + on the watchlist)
  {
    const BR = D.browse.state(), BE = BR.el;
    A.el.focus(); key('KeyU', 'u');
    check(D.browse.isOpen && P.doc.activeElement !== BE.q, 'the handler only shows the browser: its search takes focus after the frame (a focus there would force a layout in the handler)');
    await run(600);
    check(D.browse.isOpen && /\bbrowse\b/.test(P.doc.body.className) && P.doc.activeElement === BE.q, 'u opens the market browser with its search focused');
    const btb = BR.V.tb, brows = () => btb.children.filter(r => r.getAttribute('data-mkt'));
    check(+BE.n.textContent.split(' ')[0] > 500 && brows().length > 10 && btb.children.filter(r => !/\bvpad\b/.test(r.className)).length <= 70, 'it lists the day\'s universe, virtual', BE.n.textContent);
    check(BE.cats.some(b => /^SPORTS \d+$/.test(b.textContent)) && BE.cats.some(b => /^WEATHER \d+$/.test(b.textContent)), 'the category chips carry their counts', BE.cats.map(b => b.textContent));
    BE.q.value = 'denver'; fire(BE.q, 'input', {}); await run(800);
    BE.q.value = 'den'; fire(BE.q, 'input', {}); await run(800);
    const dr = brows().find(r => /^DEN /.test(r.children[2].textContent) && r.children[0].textContent === '☆');
    check(!!dr, 'a search narrows it (the server matches tickers, titles and groups)', brows().map(r => r.children[2].textContent).slice(0, 5));
    const dk = dr.getAttribute('data-mkt');
    fire(dr.children[0], 'pointerdown', { button: 0 }); P.frame();
    check(dr.children[0].textContent === '★', 'its star pins at once');
    await run(400);
    check(st.market(dk) && st.market(dk).pin === true && st.book(dk), 'pinned: the market joins the live set with its book');
    key('ArrowDown', 'ArrowDown'); key('Enter', 'Enter'); await run(400);
    check(!D.browse.isOpen && D.focus.key && D.focus.key === BR.cursor, 'Enter loads the cursor\'s market into the focused ladder and closes the browser', [D.focus.key, BR.cursor]);
    click(WE.browse); await run(300);
    check(D.browse.isOpen, '+ on the watchlist opens it too');
    key('Escape', 'Escape');
    check(!D.browse.isOpen, 'Esc closes it');
    click(WE.scope[0]); await run(300);
    check(wrows().some(r => r.getAttribute('data-mkt') === dk), 'the market pinned in the browser is on the watchlist');
    D.focus.set(natKey, { ladder: 'A' }); await run(300);
  }
  // ---- virtual lists: the drawn window moves 8 rows at a time, so most scroll frames redraw
  // nothing (2026-10-06 browser pass: every 2-row scroll step relaid out ~50 rows, ~5 ms)
  {
    const host = P.doc.createElement('div'); P.doc.body.appendChild(host);
    let calls = 0;
    const V = D.vlist(host, [{ h: 'A' }, { h: 'B', w: 40 }], 'grid', { rowH: 18, headH: 20, h: 360, onScroll: () => { calls++; } });
    const items = Array.from({ length: 1000 }, (_, i) => i);
    const drawAll = () => V.draw(items, String, i => [String(i), ''], () => '');
    const firstKey = () => { const r = V.tb.children.find(x => !/\bvpad\b/.test(x.className)); return r ? r.getAttribute('data-k') : null; };
    const scrollTo = row => { host.scrollTop = row * 18; fire(host, 'scroll', {}); };
    drawAll();
    eq(V.a + '-' + V.b + ' ' + firstKey(), '0-45 0', 'a virtual list draws the rows in view and a margin: 45 of 1,000');
    scrollTo(5);
    eq(calls, 0, 'scrolling inside the drawn window asks for no redraw (the browser just scrolls)');
    scrollTo(20);
    eq(calls, 1, 'leaving it asks for one');
    drawAll();
    eq(V.a + ' ' + firstKey(), '8 8', 'and the window moves by whole chunks of 8 rows, 8 rows above the view');
    scrollTo(23);
    eq(calls, 1, 'the next rows scroll free again');
    scrollTo(995); drawAll();
    check(V.b === 1000 && V.a <= 1000 - 21, 'at the end it draws the end, never an empty window', [V.a, V.b]);
    const pads = V.tb.children.filter(r => /\bvpad\b/.test(r.className)).map(r => parseInt(r.children[0].style.getPropertyValue('height'), 10));
    eq(pads.reduce((x, y) => x + y, 0) + (V.b - V.a) * 18, 1000 * 18, 'the spacers keep the whole list\'s height');
    const nScroll = () => host._ls.filter(l => l.type === 'scroll').length;
    const tb0 = V.tb;
    V.retable([{ h: 'A' }, { h: 'B', w: 40 }, { h: 'C', w: 40 }], 'grid');
    check(V.tb !== tb0 && host.contains(V.tb) && !host.contains(tb0) && nScroll() === 1, 'a new column set swaps the table, not the listeners', nScroll());
    host.remove();
    // the watchlist's wider column set (from 420 px) reuses its list
    const n0 = MS.watch.body._ls.filter(l => l.type === 'scroll').length;
    MS.watch.wide = true; D.render.mark('mwatch'); await run(300);
    MS.watch.wide = false; D.render.mark('mwatch'); await run(300);
    check(n0 === 1 && MS.watch.body._ls.filter(l => l.type === 'scroll').length === 1 && MS.watch.body.contains(MS.watch.tb), 'the watchlist widening and narrowing keeps one scroll listener', [n0, MS.watch.body._ls.filter(l => l.type === 'scroll').length]);
  }
  // ---- ticket ---------------------------------------------------------------------------------
  const E = MS.ticket.el;
  eq(E.mkt.textContent, nat.short, 'the ticket follows the focused market');
  eq(MS.cfg.tk.type, 'IOC', 'ticket defaults to IOC: a click takes the touch');
  MS.cfg.tk.type = 'P15'; D.render.mark('mtk');   // the checks below start from a 15 s post
  P.sim().freeze(natKey, { bids: [[6300, 120], [6200, 85]], asks: [[6400, 80], [6500, 210], [6700, 12]] });
  await run(300);
  check(/^BUY 1 @0\.63 15s$/.test(E.buy.textContent) && /^SELL 1 @0\.64 15s$/.test(E.sell.textContent), 'AUTO price: a post joins its own side', E.buy.textContent + ' | ' + E.sell.textContent);
  check(/auto/.test(E.px.className) && E.px.value === '0.63', 'the price field shows AUTO in grey');
  const tyBtn = t => E.types.find(b => b.textContent === t);
  check(!tyBtn('30m').disabled && tyBtn('START').disabled && !tyBtn('5m').disabled, '5m and 30m are there; START needs a game start time (a 15-minute window has none)');
  check(['IOC', 'FOK', '5s', '15s', '60s', '5m', '30m', 'START', 'CLOSE'].every(t => tyBtn(t)), 'IOC FOK POST 5s 15s 60s 5m 30m START CLOSE, no open-ended GTC');
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
  // CLOSE: an order that rests until the market closes (Kalshi expiration_ts / PM GTD at the close)
  {
    check(!!tyBtn('CLOSE') && !tyBtn('CLOSE').disabled, 'the ticket offers CLOSE (until close) next to START');
    const wxm = Array.from(st.markets.values()).find(m => m.kind === 'WEATHER' && m.status === 'OPEN' && m.close_ms - P.clock.t > 3 * 3600e3 && st.book(m.key) && st.book(m.key).bb !== null);
    D.focus.set(wxm.key); await run(300);
    click(tyBtn('CLOSE')); P.frame();
    check(/@\d\.\d+ CLOSE \d+h\d\dm$/.test(E.buy.textContent), 'the button says CLOSE and the time to close', E.buy.textContent);
    check(/until the market closes at/.test(tyBtn('CLOSE').getAttribute('title')), 'CLOSE carries the close time on its title');
    D.manual.ticketPrice(wxm.key, D.grid.step(wxm.g, st.book(wxm.key).bb, -2)); P.frame();
    n0 = st.orders.size;
    click(E.buy);
    o = newest(P, x => x.own === 'MAN');
    const closeS = Math.floor(wxm.close_ms / 1000) * 1000;
    check(st.orders.size === n0 + 1 && o.tif === 'POST' && o.exp_ms === closeS, 'CLOSE sends a post whose exchange expiry is the market close', { exp: o.exp_ms, close: wxm.close_ms });
    await run(300);
    o = st.orders.get(o.cid);
    check(o.st === 'WORK' && o.exp_ms === closeS, 'the venue rests it until the close (more than 24 h is allowed when the close is)', { st: o.st, why: o.why });
    const wr = P.$('m-work-body').children[0].children[2].children.find(r => r.getAttribute('data-k') === o.cid);
    check(wr && /^\d+h\d\dm$/.test(wr.children[7].textContent) && /until close/.test(wr.children[7].getAttribute('title')), 'WORKING counts it down as the time to close', wr && wr.children[7].textContent);
    D.act.cancel([o.cid]); await run(300);
    D.focus.set(natKey); await run(300);
    click(tyBtn('IOC')); P.frame();
  }
  check(E.eM.textContent !== '' && /c/.test(E.eM.textContent), 'edge vs model shown', E.eM.textContent);
  click(E.q.find(b => b.textContent === '5'));
  eq(MS.cfg.tk.qty, 5, 'ticket size preset');
  n0 = st.orders.size;
  click(E.buy);
  o = newest(P, x => x.own === 'MAN');
  check(st.orders.size === n0 + 1 && o.tif === 'IOC' && o.qty === 5 && o.px === 6500, 'BUY sends the IOC', { tif: o.tif, qty: o.qty, px: o.px });
  P.frame();
  check(/^BUY 5 @0\.65 sent/.test(E.last.textContent), 'the result line says it went out', E.last.textContent);
  await run(250);
  check(/^BUY 5 (filled|@0\.65 (filled|not filled))/.test(E.last.textContent) && st.orders.get(o.cid).terminal, 'and then what became of it',
    [E.last.textContent, st.orders.get(o.cid).st]);
  // fixed price, the guard as a second click
  click(tyBtn('15s'));
  E.px.focus(); E.px.value = '0.70'; fire(E.px, 'input', {}); E.px.blur();
  P.frame();
  eq(MS.ticket.px, 7000, 'a typed price fixes the ticket price');
  check(/GUARD/.test(E.guard.textContent), 'the check line flags the guard before the click', E.guard.textContent);
  n0 = st.orders.size;
  click(E.buy); P.frame();
  check(st.orders.size === n0 && /^OVERRIDE BUY/.test(E.buy.textContent) && /confirm/.test(E.buy.className), 'the guard turns the button into OVERRIDE (no dialog)', E.buy.textContent);
  click(E.buy, { detail: 2 });
  eq(st.orders.size, n0, 'a double-click does not confirm OVERRIDE');
  await run(400);
  click(E.buy);
  eq(st.orders.size, n0 + 1, 'the second, separate click sends it');
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
  click(r0.children[1]);
  P.frame();
  check(/\bsel\b/.test(r0.className) && wtb2.children.filter(r => r !== r0 && r.getAttribute('data-mkt') === r0.getAttribute('data-mkt')).every(r => /\bfmk\b/.test(r.className) && !/\bsel\b/.test(r.className)),
    'the clicked row carries the selection bar; the market\'s other rows only a tint', r0.className);
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
  P.frame();
  check(P.$('kill').textContent === 'KILL ...' && st.canTrade('K') === 'KILLED' && st.orders.get(cidK).local.cxlPend,
    'KILL paints pending at once: the button, new orders refused, working orders struck through', P.$('kill').textContent);
  await run(300);
  check(/^KILLED \d+s$/.test(P.$('kill').textContent), 'KILLED with the lockout once the backend confirms', P.$('kill').textContent);
  check(E.buy.disabled && E.blk.textContent === 'KILLED', 'KILL: the ticket is locked');
  check(/^cannot send: KILLED: KILL is on; press RE-ARM/.test(E.last.textContent), 'and its result line says how to unlock it', E.last.textContent);
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

  // ---- flashes: a changed size flashes, a pulled level (the cell empties) does not -------------
  {
    const tb = P.doc.createElement('tbody');
    const draw = v => D.dom.keyedRows(tb, [1], String, () => [{ t: v, cls: 'bq', fl: 's' }]);
    draw('5');
    const td = tb.children[0].children[0];
    let n = 0; td.animate = () => { n++; return {}; };
    draw('7');
    eq(n, 1, 'a size that changes flashes');
    draw('');
    eq(n, 1, 'a level that goes away does not flash (no grey box on an empty cell)');
  }
  // ---- a P&L keyed on its realized part: no flash when only the mark moves; green up, red down
  {
    const tb = P.doc.createElement('tbody');
    const draw = (t, fk) => D.dom.keyedRows(tb, [1], String, () => [{ t, cls: 'n', fl: 'd', flg: 1500, fk }]);
    draw('+1.00', '100');
    const td = tb.children[0].children[0], got = [];
    td.animate = kf => { got.push(kf[0].boxShadow); return {}; };
    draw('+1.40', '100');
    eq(got.length, 0, 'a P&L keyed on its realized part does not flash when only the mark moves');
    draw('+2.40', '150');
    check(got.length === 1 && /63,185,80/.test(got[0]), 'it flashes green when the realized part rises', got);
    td._flT = 0; draw('+0.40', '20');
    check(got.length === 2 && /248,81,73/.test(got[1]), 'and red when it falls', got);
  }
  // ---- FLASH: calm by default (SUBTLE), routine book updates only with FULL, nothing with OFF ---
  {
    const proto = P.doc.createElement('td').constructor.prototype, anim0 = proto.animate;
    let fl = [];
    proto.animate = function (kf, o) { fl.push({ el: this, ms: o && o.duration, c: kf && kf[0] && kf[0].boxShadow }); return { cancel() {} }; };
    const onLad = () => fl.filter(x => A.tb.contains(x.el));
    const reset = () => { fl = []; rowsOf(A.tb).forEach(r => r.children.forEach(c => { c._flT = 0; })); };
    eq(D.ui.flash, 'subtle', 'FLASH defaults to SUBTLE');
    eq(P.doc.documentElement.getAttribute('data-flash'), 'subtle', 'and says so on <html> (the CSS fallback durations)');
    D.focus.set(natKey, { ladder: 'A' });
    P.sim().freeze(natKey, { bids: [[6300, 120], [6200, 85]], asks: [[6400, 80], [6500, 210]] }); await run(300);
    reset();
    P.sim().freeze(natKey, { bids: [[6300, 140], [6200, 60]], asks: [[6400, 95], [6500, 180]] }); await run(300);
    eq(ladCell(P, 'A', 6300, 'bq').textContent, '140', 'the new sizes are drawn');
    eq(onLad().length, 0, 'SUBTLE: a routine size change on the ladder does not flash (the depth bar shows size)');
    reset();
    P.sim().trade(natKey, 'BUY', 3); await run(300);
    const lastPx = onLad().filter(x => /\bpx\b/.test(x.el.className));
    check(lastPx.length === 1 && /\blast\b/.test(lastPx[0].el.className), 'a print gives the last-trade price row a short highlight', onLad().map(x => x.el.className));
    check(lastPx.length && lastPx[0].ms >= 400 && lastPx[0].ms <= 600, 'SUBTLE fades over 400-600 ms', lastPx[0] && lastPx[0].ms);
    check(!onLad().some(x => /\b(bq|aq|lt)\b/.test(x.el.className)), 'and nothing else on the ladder flashes for it');
    reset();
    click(ladCell(P, 'A', 6100, 'bq'));
    const myCid = newest(P, x => x.own === 'MAN').cid;
    P.frame();
    check(onLad().some(x => x.el === ladCell(P, 'A', 6100, 'wb')), 'our own order flashes its cell when it is sent');
    reset();
    await run(300);
    check(onLad().some(x => x.el === ladCell(P, 'A', 6100, 'wb')), 'and again when the venue acknowledges it (a state change)');
    reset();
    await run(2000, 250);
    check(!onLad().some(x => x.el === ladCell(P, 'A', 6100, 'wb')), 'the countdown ticking is not a state change: no flash');
    D.act.cancel([myCid]); await run(300);
    D.ui.setFlash('full'); reset();
    eq(D.storage.get('flash'), 'full', 'the FLASH choice is saved (safe storage)');
    P.sim().freeze(natKey, { bids: [[6300, 100], [6200, 70]], asks: [[6400, 60], [6500, 150]] }); await run(300);
    check(onLad().some(x => /\bbq\b/.test(x.el.className)) && onLad().every(x => x.ms === 300), 'FULL: size changes flash again, 300 ms', onLad().map(x => x.ms));
    D.ui.setFlash('off'); reset();
    P.sim().freeze(natKey, { bids: [[6300, 90], [6200, 75]], asks: [[6400, 66], [6500, 140]] }); P.sim().trade(natKey, 'SELL', 2); await run(300);
    eq(fl.length, 0, 'OFF: nothing flashes anywhere');
    key('KeyF', 'f');
    eq(D.ui.flash, 'subtle', 'f cycles FLASH (OFF to SUBTLE)');
    // POSITIONS: unrealized P&L moves with every mark, so it flashes only with FULL (2026-10-06
    // browser pass: twenty Systematic positions twinkled ~9 cells a second in SUBTLE)
    {
      P.sim().freeze(natKey, { bids: [[6300, 120]], asks: [[6400, 300]] }); await run(300);
      D.act.order({ v: 'K', tk: nat.tk, side: 'BUY', px: 6400, qty: 2, tif: 'IOC', touch: 6400 }, null); await run(800);
      const ptbF = P.$('m-pos-body').children[0].children[2];
      const unrlOf = () => { const r = ptbF.children.find(x => x.getAttribute('data-k') === natKey); return r ? r.children[7] : null; };
      check(!!unrlOf() && st.position(natKey).pos >= 2, 'a position to mark', st.position(natKey));
      const u0 = unrlOf().textContent;
      fl = [];
      P.sim().freeze(natKey, { bids: [[6600, 120]], asks: [[6700, 300]] }); await run(1200);
      P.sim().freeze(natKey, { bids: [[6100, 120]], asks: [[6200, 300]] }); await run(1200);
      check(unrlOf().textContent !== u0 && !fl.some(x => x.el === unrlOf()), 'SUBTLE: unrealized P&L in POSITIONS does not flash when the mark moves', [u0, unrlOf().textContent]);
      D.ui.setFlash('full'); fl = [];
      P.sim().freeze(natKey, { bids: [[6600, 120]], asks: [[6700, 300]] }); await run(1200);
      check(fl.some(x => x.el === unrlOf()), 'FULL: it does');
      D.ui.setFlash('subtle');
      D.act.order({ v: 'K', tk: nat.tk, side: 'SELL', px: 6600, qty: st.position(natKey).pos, tif: 'IOC', touch: 6600 }, null); await run(800);
      eq(st.position(natKey).pos, 0, 'flat again');
    }
    // a busy book repaints the ladder at most about 10 times a second
    const bk = JSON.parse(JSON.stringify(D.contract.examples.book));
    bk.tk = nat.tk; bk.trades = []; bk.last = null;
    const p0 = A.paints;
    for (let i = 0; i < 50; i++) {
      bk.bids[0][1] = 100 + i; bk.ex_ms = P.clock.t;
      st._handlers.book(JSON.parse(JSON.stringify(bk)));
      P.clock.t += 20; P.frame();
    }
    const n10 = A.paints - p0;
    check(n10 >= 8 && n10 <= 11, 'a book updating 50 times a second repaints its ladder about 10 times a second', n10);
    click(ladCell(P, 'A', 6100, 'bq'));
    const pp = A.paints; P.frame();
    eq(A.paints, pp + 1, 'a click still paints on the very next frame');
    D.act.cancelAll('MAN', null);
    proto.animate = anim0;
    P.sim().freeze(natKey, { bids: [[6300, 120], [6200, 85], [6100, 40]], asks: [[6400, 80], [6500, 210], [6700, 12]] });
    await run(500);
  }
  // ---- instant controls: pressed at pointerdown, safe controls act there, orders keep the click --
  {
    const press = (el, props) => fire(el, 'pointerdown', Object.assign({ button: 0, pointerType: 'mouse' }, props || {}));
    const up = el => fire(el, 'pointerup', { button: 0 });
    const foot = P.$('m-lad-a-foot');
    const fb = t => foot.children[0].children.concat(foot.children[1].children).find(b => b.textContent === t);
    D.focus.set(natKey, { ladder: 'A' }); await run(300);
    click(fb('1'));
    const paints0 = A.paints;
    press(fb('CLR'));
    check(fb('CLR').hasAttribute('data-prs'), 'a pressed control shows pressed in the same event');
    eq(A.cfg.qty, 0, 'a footer setting acts at pointerdown, before the click');
    up(fb('CLR')); fire(fb('CLR'), 'click', {});
    check(!fb('CLR').hasAttribute('data-prs'), 'pointerup clears the pressed state');
    press(fb('5'), { shiftKey: true }); fire(fb('5'), 'click', { shiftKey: true });
    eq(A.cfg.qty, 5, 'the click after a pointerdown that acted is swallowed (Shift+5 adds once, not twice)');
    P.frame();
    check(/\bon\b/.test(fb('5').className) && A.paints === paints0, 'the footer repaints alone: the 38 rows are not redrawn for a setting', { paints: A.paints - paints0 });
    fire(fb('15s'), 'click', {});
    eq(A.cfg.ttl, 15, 'a keyboard or scripted click (no pointerdown) still acts');
    // an order cell: pressed at pointerdown, sent only on the click
    const n1 = st.orders.size, cell = ladCell(P, 'A', 6000, 'bq');
    press(cell);
    check(st.orders.size === n1 && cell.hasAttribute('data-prs'), 'an order cell shows pressed at pointerdown but sends nothing yet');
    up(cell); fire(cell, 'click', {});
    const sent = newest(P, x => x.own === 'MAN');
    check(st.orders.size === n1 + 1 && sent.px === 6000 && sent.st === 'PEND', 'the click sends it, PEND in the store at once');
    check(!P.sim().order(sent.cid), 'the simulated venue has not seen it inside the handler (its work runs after the paint)');
    P.frame();
    check(/\bpend\b/.test(ladCell(P, 'A', 6000, 'wb').className), 'the next frame paints it pending');
    await run(300);
    check(P.sim().order(sent.cid) && st.orders.get(sent.cid).st === 'WORK', 'then the venue takes it and the ack arrives');
    D.act.cancel([sent.cid]); await run(300);
    // a press dragged off an order cell never sends
    const n2 = st.orders.size;
    press(ladCell(P, 'A', 5900, 'bq')); up(P.$('m-lad-a-head'));
    eq(st.orders.size, n2, 'a press released elsewhere sends nothing');
    // PRICE copies to the ticket at pointerdown
    press(ladCell(P, 'A', 6100, 'px'));
    eq(MS.ticket.px, 6100, 'a PRICE press copies the price to the ticket at once');
    fire(ladCell(P, 'A', 6100, 'px'), 'click', {});
    // the tab switch acts at pointerdown
    press(P.$('tab-sys'));
    eq(D.render.tab, 'systematic', 'the tab switch acts at pointerdown');
    fire(P.$('tab-sys'), 'click', {});
    press(P.$('tab-man')); fire(P.$('tab-man'), 'click', {});
    eq(D.render.tab, 'manual', 'and back');
    // the ticket: settings at pointerdown, BUY / SELL on the click
    const E2 = MS.ticket.el, n3 = st.orders.size;
    press(E2.types.find(b => b.textContent === '60s'));
    eq(MS.cfg.tk.type, 'P60', 'a ticket order type acts at pointerdown');
    press(E2.buy);
    eq(st.orders.size, n3, 'BUY does not send at pointerdown');
    fire(E2.buy, 'click', {});
    eq(st.orders.size, n3 + 1, 'it sends on the click');
    click(E2.types.find(b => b.textContent === '15s'));
    await run(300);
    // a working row's x cancels at pointerdown
    const live1 = manOrders(P).find(x => !x.terminal);
    const xr = P.$('m-work-body').children[0].children[2].children.find(r => r.getAttribute('data-k') === live1.cid);
    press(xr.children[10]);
    check(st.orders.get(live1.cid).local.cxlPend, 'a working order\'s x cancels at pointerdown');
    fire(xr.children[10], 'click', {});
    await run(300);
    // the dev check: handler and paint time per control
    D.perf.set(true);
    const ctls = [fb('1'), fb('5'), fb('60s'), fb('15s'), P.$('tab-sys'), P.$('tab-man'), E2.types.find(b => b.textContent === 'IOC'),
                  E2.types.find(b => b.textContent === '15s'), MS.watch.el.cats[2], MS.watch.el.cats[0], MS.watch.el.day[1], MS.watch.el.day[0], P.$('sb-keys'), P.$('keysheet-close'), P.$('sb-set'), P.$('set-close')];
    for (const c of ctls) { press(c); up(c); fire(c, 'click', {}); P.frame(); }
    click(ladCell(P, 'A', 5800, 'bq')); P.frame();
    A.el.focus(); key('Space', ' '); P.frame();
    const rep = D.perf.report();
    check(rep.length >= 10 && rep.some(r => r.ctl === '#tab-sys') && rep.some(r => /m-lad-a:q$/.test(r.ctl)) && rep.some(r => /m-lad-a:bq$/.test(r.ctl)) && rep.some(r => r.ctl === 'key:Space'),
      'the dev check records handler and paint time per control', rep.map(r => r.ctl));
    const over = rep.filter(r => r.h > 16.7 || r.p > 16.7);
    eq(over.map(r => r.ctl + ' ' + r.h + '/' + r.p).join(', '), '', 'every control: handler and frame inside one frame (16.7 ms) here');
    check(P.errors.length === 0, 'and the dev check logs no SLOW control', P.errors.slice(0, 2));
    D.perf.set(false);
    D.act.cancelAll('MAN', null); await run(300);
  }

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
  P.$('tab-systematic').focus();
  eq(D.keys.splitTab(), 'systematic', 'in SPLIT the half holding the focus gives the tab scope');
  A.el.focus();
  eq(D.keys.splitTab(), 'manual', 'and the MANUAL half when a ladder has it');
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
    check(!/\boff\b/.test(zb.className) && zb.textContent === 'x10', 'a 0.1c ladder offers zoom and opens at x10 (whole-cent rows)', zb.textContent);
    let px5 = rowsOf(B.tb).map(r => +r.getAttribute('data-k'));
    check(px5.every(p => p % 100 === 0) && px5[0] - px5[1] === 100, 'x10 rows are whole cents');
    click(zb); await run(300);
    eq(zb.textContent, 'x1', 'zoom x1: every 0.1c level');
    px5 = rowsOf(B.tb).map(r => +r.getAttribute('data-k'));
    check(px5[0] - px5[1] === 10, 'x1 rows are tenths of a cent', px5.slice(0, 3));
    click(zb); await run(300);
    eq(zb.textContent, 'x5', 'zoom x5');
    px5 = rowsOf(B.tb).map(r => +r.getAttribute('data-k'));
    check(px5.length === 38 && px5.every(p => p % 50 === 0) && px5[0] - px5[1] === 50, 'x5 rows are half cents', px5.slice(0, 3));
    // the choice belongs to the market, not the ladder slot
    D.focus.set(dk.key, { ladder: 'A' }); await run(300);
    eq(P.$('m-lad-a-head').children[0].children[8].textContent, 'x5', 'the same market in another ladder keeps its zoom');
    D.focus.set(natKey, { ladder: 'A' }); await run(300);
    click(zb); await run(300);
    eq(zb.textContent, 'x10', 'x10 again');
    px5 = rowsOf(B.tb).map(r => +r.getAttribute('data-k'));
    const shownBid = rowsOf(B.tb).reduce((t, r) => t + (+(r.children[1].getAttribute('title') || 0)), 0);
    check(rowsOf(B.tb).every(r => /^(\d+|<1|)$/.test(r.children[1].textContent)), 'ladder sizes read as whole contracts (the exact size on the title)');
    const b3 = st.book(dk.key), lo = Math.min.apply(null, px5), inWin = b3.bids.filter(l => l[0] >= lo).reduce((t, l) => t + l[1], 0);
    check(Math.abs(shownBid - inWin) < 0.05, 'zoomed rows add up the book\'s sizes', { shownBid, inWin });
    const bbRowEl = rowsOf(B.tb).find(r => /\bbb\b/.test(r.children[3].className));
    check(bbRowEl && +bbRowEl.getAttribute('data-k') === Math.floor(b3.bb / 100) * 100, 'the best bid marks its floor row', bbRowEl && bbRowEl.getAttribute('data-k'));
  }

  // ---- the 15-minute roll ---------------------------------------------------------------------
  D.focus.set(natKey, { ladder: 'A' });
  await run(300);
  const closeAt = st.market(natKey).close_ms;
  await run(closeAt - P.clock.t - 200, 1000);
  let nk = D.focus.ladders.A;
  for (let i = 0; i < 80 && nk === natKey; i++) { await run(50, 50); nk = D.focus.ladders.A; }
  check(nk !== natKey && st.market(nk) && st.market(nk).status === 'OPEN' && D.manual.logic.series(st.market(nk).tk) === 'KXNATGAS15M', 'ladder A rolls to the next natural gas window', nk);
  {
    // the row under the pointer is now another market's: clicks wait half a second
    P.frame();
    const row = rowsOf(A.tb)[5], n1 = st.orders.size;
    click(row.children[1]);
    check(st.orders.size === n1 && /^ROLLED/.test(st.alerts[0].msg), 'a click just after the roll is refused (ROLLED)', st.alerts[0].msg);
    await run(600);
  }
  await run(500);
  eq(A.shownKey, nk, 'and shows it');

  // ---- ACTIVITY: the operator's own tape, and the header's count of what he has not seen ------
  {
    D.tabs.set('manual');
    const ak = D.focus.ladders.A;                    // the natural gas window open now (natKey rolled)
    P.sim().freeze(ak, { bids: [[6300, 120], [6200, 85]], asks: [[6400, 80], [6500, 210]] }); await run(300);
    const atb = P.$('m-act-body').children[0].children[2], arows = () => atb.children.filter(r => /^a-/.test(r.className));
    const top = () => arows()[0], ev = r => r.children[1].textContent;
    eq(P.$('m-act').children[0].textContent.indexOf('ACTIVITY'), 0, 'MANUAL has an ACTIVITY tape');
    click(ladCell(P, 'A', 6200, 'bq'));
    const ca = newest(P, x => x.own === 'MAN');
    P.frame();
    check(top() && ev(top()) === 'SUB' && top().children[2].textContent === 'BUY' && /side-BUY/.test(top().children[2].className) && top().getAttribute('data-mkt') === ak,
      'an order shows on the tape at the click: SUB, side in its colour, linked to its market', top() && top().textContent);
    eq(top().children[6].textContent, 'POST 15s', 'a sent post says what it rests for');
    await run(300);
    D.act.cancel([ca.cid]); await run(300);
    check(ev(top()) === 'CXL' && /USER/.test(top().children[6].textContent) && /a-CXL/.test(top().className), 'a cancel shows CXL with its reason (grey)', top().textContent);
    {
      const ttlBtn = v => P.$('m-lad-a-foot').children[1].children.find(b => b.getAttribute('data-v') === v);
      click(ttlBtn('CLOSE')); await run(600);           // (clear of the ladder's roll hold)
      click(ladCell(P, 'A', 6100, 'bq'));
      const cc = newest(P, x => x.own === 'MAN');
      P.frame();
      check(ev(top()) === 'SUB' && top().children[6].textContent === 'POST CL' && cc.exp_ms === Math.floor(st.market(ak).close_ms / 1000) * 1000, 'an until-close post says POST CL on the tape', top().textContent + ' | ' + st.alerts.slice(0, 2).map(a => a.msg).join(' / '));
      await run(300); D.act.cancel([cc.cid]); await run(300);
      click(ttlBtn('15')); P.frame();
    }
    click(ladCell(P, 'A', 6400, 'bq')); await run(400);
    check(arows().slice(0, 3).some(r => ev(r) === 'FILL' && /\bT\b/.test(r.children[6].textContent)), 'an IOC that fills shows FILL with its liquidity (T)', arows().slice(0, 3).map(r => r.textContent));
    const pr = P.$('m-pos-body').children[0].children[2].children.find(r => r.getAttribute('data-k') === ak);
    click(pr.children[10]); await run(400);
    const ex = arows().find(r => ev(r) === 'EXIT');
    check(ex && /^[+−]?\d+\.\d\d$/.test(ex.children[6].textContent) && ex.getAttribute('data-mkt') === ak, 'flattening shows EXIT with the market\'s P&L', ex && ex.textContent);
    const fb = P.$('m-act-tools').children.find(b => b.textContent === 'FILLS');
    click(fb); await run(300);
    check(arows().length && arows().every(r => /^(FILL|PART|EXIT)$/.test(ev(r))), 'FILLS shows fills and exits only');
    click(P.$('m-act-tools').children.find(b => b.textContent === 'ALL')); await run(300);
    click(ex.children[4]); P.frame();
    eq(D.focus.key, ak, 'a tape row focuses its market');
    // the counter: SYS fills while MANUAL shows count as not seen; ACT shows them
    eq(D.activity.unseen.MAN, 0, 'the operator\'s own events count as seen while MANUAL is on screen');
    D.activity.seen('SYS'); await run(300);
    eq(P.$('h-act').textContent, '0', 'ACT reads 0 with nothing unseen');
    const id0 = D.activity.n;
    D.act.start(); await run(60000, 250);
    const un = +P.$('h-act').textContent, fresh = D.activity.lists.SYS.filter(e => e.id > id0);
    const sysHot = fresh.filter(e => e.type === 'EXIT' || e.type === 'REJ' || e.type === 'HALT').length;
    check(un > 0 && /\bwarn\b/.test(P.$('hc-act').className) && D.activity.unseen.SYS === un, 'Systematic\'s exits, while MANUAL shows, count in the header\'s ACT (amber)', { un, cls: P.$('hc-act').className });
    check(fresh.some(e => e.type === 'SUB') && fresh.some(e => e.type === 'FILL') && un === sysHot,
      'only the SYS alert types count (exits, rejects, a halt by default): not every quote, not every lane fill', { un, sysHot, fills: fresh.filter(e => e.type === 'FILL').length, all: fresh.length });
    fire(P.$('hc-act'), 'pointerdown', { button: 0 }); fire(P.$('hc-act'), 'click', {}); await run(300);
    check(D.render.tab === 'systematic' && P.$('h-act').textContent === '0', 'ACT shows the tab they happened on and counts them seen');
    // the tape is a virtual list: a minute of busy lanes is hundreds of events, a few dozen rows
    D.render.force('systematic'); P.frame();
    const allBtn = P.$('s-act-tools').children.find(b => b.textContent === 'ALL');
    fire(allBtn, 'pointerdown', { button: 0 }); fire(allBtn, 'click', {}); P.frame();
    const stb = P.$('s-act-body').children[0].children[2], sDrawn = stb.children.filter(r => /^a-/.test(r.className)).length;
    check(D.activity.lists.SYS.length > 200 && sDrawn > 10 && sDrawn <= 64, 'the tape keeps every event and draws only the rows in view', { events: D.activity.lists.SYS.length, drawn: sDrawn });
    const sPads = stb.children.filter(r => /\bvpad\b/.test(r.className)).map(r => parseInt(r.children[0].style.getPropertyValue('height'), 10) || 0);
    eq(sPads.reduce((x, y) => x + y, 0) + sDrawn * D.dom.rowPx(), D.activity.lists.SYS.length * D.dom.rowPx(), 'its spacers keep the whole tape\'s height (it scrolls back through all of it)');
    D.act.stop(); D.tabs.set('manual'); await run(500);
  }

  // ---- 2026-10-06 review: presses that survive repaints, Enter in the browser and the watchlist,
  // calm ladders, the amber cue in FLASH OFF, DAY on its realized part, the live set shrinking ---
  {
    D.tabs.set('manual'); await run(500);
    const press = el => fire(el, 'pointerdown', { button: 0, pointerType: 'mouse' });
    const up = el => fire(el, 'pointerup', { button: 0 });
    const prs = el => !!el && el.hasAttribute('data-prs');
    // (1) a press stays drawn until pointerup, whatever repaints its control
    D.focus.setLadder('A'); await run(300);
    const bKey = D.focus.ladders.B;
    P.sim().freeze(bKey, { bids: [[4000, 10], [3900, 20]], asks: [[4200, 10], [4300, 5]] }); await run(400);
    const bCell = ladCell(P, 'B', 4000, 'bq');
    check(!!bCell && /\bhas\b/.test(bCell.className), 'ladder B shows its bid', bCell && bCell.className);
    press(bCell); fire(bCell, 'mousedown', { button: 0 });
    eq(D.focus.ladder, 'B', 'the press\'s mousedown makes ladder B the focused one (an urgent repaint of it)');
    P.frame();
    ['book:' + bKey, 'clock', 'hdr', 'focus', 'pos', 'pos:' + bKey, 'ord:' + bKey].forEach(t => D.render.mark(t)); P.frame();
    B.dirty = true; D.render.mark('mlad'); P.frame();
    check(prs(bCell) && B.tb.contains(bCell) && /\bbq\b/.test(bCell.className), 'a BID cell pressed on an unfocused ladder stays pressed through its repaints (2026-10-06: it read pressed only until the next frame)', bCell.className);
    up(bCell);
    check(!prs(bCell), 'pointerup clears it');
    const aCell = rowsOf(A.tb).map(r => r.children[1]).find(td => /\bhas\b/.test(td.className));
    press(aCell); D.render.mark('book:' + D.focus.ladders.A); A.dirty = true; D.render.mark('mlad'); P.frame(); P.frame();
    check(prs(aCell), 'on the focused ladder too, through a book repaint');
    up(aCell);
    const E2 = MS.ticket.el;
    check(!E2.buy.disabled, 'the ticket can send');
    press(E2.buy);
    ['mtk', 'clock', 'hdr', 'book:' + D.focus.key, 'pos'].forEach(t => D.render.mark(t)); P.frame(); P.frame();
    check(prs(E2.buy) && /\bbuy\b/.test(E2.buy.className), 'ticket BUY stays pressed while the ticket repaints (its className is written every clock tick)', E2.buy.className);
    up(E2.buy);
    const kb = $('kill');
    press(kb); D.render.mark('hdr'); D.render.mark('clock'); P.frame(); P.frame();
    check(prs(kb), 'KILL stays pressed through the header\'s repaints');
    up(kb);
    check(!prs(kb) && st.hdr.run.state !== 'KILLED', 'and a press released without its click kills nothing');
    // (2) the market browser: Enter loads what is drawn now, never a stale cursor
    const BR = D.browse.state(), BE = BR.el, brows = () => BR.V.tb.children.filter(r => r.getAttribute('data-mkt'));
    D.focus.setLadder('A'); A.el.focus(); key('KeyU', 'u'); await run(600);
    BE.q.value = ''; fire(BE.q, 'input', {}); await run(600);
    key('ArrowDown', 'ArrowDown'); key('ArrowDown', 'ArrowDown');
    const x = BR.cursor;
    key('Enter', 'Enter'); await run(300);
    check(x && D.focus.key === x && !D.browse.isOpen, 'u, ArrowDown twice, Enter: the cursor\'s market', x);
    A.el.focus(); key('KeyU', 'u'); await run(600);
    BE.q.value = 'okc'; fire(BE.q, 'input', {}); P.frame();
    check(BR.cursor === null, 'typing clears the cursor (a row of the old results)');
    check(brows().length > 10 && /\bwait\b/.test(BR.body.className), 'typing keeps the previous results drawn, dimmed, until the search lands', brows().length);
    key('Enter', 'Enter');
    check(D.browse.isOpen && !!BR.enterQ, 'an Enter before the results land waits for them');
    await run(800);
    const hit = BR.rowAt(BR.rowItems[0]);
    check(!D.browse.isOpen && !BR.enterQ && hit && D.focus.key === hit.key && D.focus.key !== x && /okc/i.test(hit.tk + hit.title + hit.short + hit.gk),
      'u, type, Enter loads the first hit of the search (2026-10-06: it loaded the old cursor\'s market)', [D.focus.key, x]);
    A.el.focus(); key('KeyU', 'u'); await run(600);
    BE.q.value = ''; fire(BE.q, 'input', {}); await run(600);
    key('ArrowDown', 'ArrowDown'); key('ArrowDown', 'ArrowDown');
    const y = BR.cursor, chipW = BE.cats.find(b => b.getAttribute('data-v') === 'WEATHER');
    fire(chipW, 'pointerdown', { button: 0 }); fire(chipW, 'click', {});
    check(BR.cursor === null, 'a filter change clears the cursor too');
    BE.q.focus(); key('Enter', 'Enter'); await run(800);
    check(!D.browse.isOpen && D.focus.key !== y && st.market(D.focus.key) && st.market(D.focus.key).cat === 'WEATHER', 'Enter after a filter change loads the first row of the new list', [D.focus.key, y]);
    A.el.focus(); key('KeyU', 'u'); await run(600);
    const chipAll = BE.cats.find(b => b.getAttribute('data-v') === '');
    fire(chipAll, 'pointerdown', { button: 0 }); fire(chipAll, 'click', {});
    key('Escape', 'Escape'); await run(300);
    // (3) the watchlist's ALL: typing never blanks it; a quick Enter loads the first hit
    {
      const WE2 = MS.watch.el, wr = () => MS.watch.tb.children.filter(r => r.getAttribute('data-mkt'));
      click(WE2.scope[1]); await run(800);
      WE2.q.focus();
      let blank = 0;
      for (const t of ['n', 'ny', 'nyc']) { WE2.q.value = t; fire(WE2.q, 'input', {}); P.frame(); if (!wr().length) blank++; }
      check(blank === 0 && /\bwait\b/.test(MS.watch.body.className), 'typing n, ny, nyc in ALL keeps a list drawn (dimmed) every frame (2026-10-06: it read "loading")', { blank });
      key('Enter', 'Enter');
      check(!!MS.watch.enterQ, 'an Enter right after typing waits for the search');
      await run(800);
      const f0 = MS.watch.rowAtItem(MS.watch.rowItems[0]);
      check(!MS.watch.enterQ && f0 && D.focus.key === f0.key && /nyc/i.test(f0.tk + f0.title + f0.short + f0.gk) && !/\bwait\b/.test(MS.watch.body.className),
        'and loads its first hit when it lands', [D.focus.key, f0 && f0.key]);
      const builds = MS.watch.uni.lay.builds;
      for (let i = 0; i < 4; i++) { D.render.mark('clock'); MS.watch.dirty = true; await run(300); }
      eq(MS.watch.uni.lay.builds, builds, 'redraws reuse the cached layout (one build per listing shape)');
      WE2.q.value = ''; fire(WE2.q, 'input', {}); click(WE2.scope[0]); await run(600);
    }
    // (4) a ladder repaints for its own market: the lanes' header and position traffic elsewhere
    // repaints its head at most
    {
      const quiet = P.sim().universe().find(r => r.cat === 'ECON' && r.v === 'K' && r.open_ms <= P.clock.t && r.close_ms > P.clock.t + 3600000 && r.bid !== null && r.ask !== null);
      D.focus.set(quiet.key, { ladder: 'A' }); await run(800);
      check(st.market(quiet.key) && st.book(quiet.key), 'a quiet listing market on ladder A, live');
      P.sim().freeze(quiet.key, { bids: [[4000, 10]], asks: [[4200, 10]] });
      D.act.start(); await run(3000, 50);
      const p0 = A.paints, h0 = A.heads, s0 = D.store.applied;
      await run(10000, 50);
      const rowsPs = (A.paints - p0) / 10, headPs = (A.heads - h0) / 10;
      check(D.store.applied - s0 > 100 && rowsPs < 1.5, 'with the lanes busy elsewhere, a ladder whose own book is quiet hardly repaints its rows (2026-10-06: 8 a second)', { rowsPs, headPs, events: D.store.applied - s0 });
      check(headPs >= 2 && headPs <= 12, 'its head still follows the clock', headPs);
      D.act.stop(); await run(1500);
      // (5) moved off the ladder, nothing else holding it, the market leaves the live set
      D.focus.set(natKey, { ladder: 'A' }); await run(2500);
      check(!st.market(quiet.key) && !st.book(quiet.key) && !P.sim().market(quiet.key), 'a listing market that no ladder, pin, order or position holds leaves the live set: the store forgets it and its book', [!!st.market(quiet.key), !!st.book(quiet.key)]);
    }
    // (6) the amber cue on a refused click shows in FLASH OFF; under Reduce Motion it is static
    {
      const fb = t => P.$('m-lad-a-foot').children[0].children.concat(P.$('m-lad-a-foot').children[1].children).find(b => b.textContent === t);
      D.focus.set(D.focus.ladders.A, { ladder: 'A' }); await run(300);
      const proto = Object.getPrototypeOf(A.tb), anim0 = proto.animate, animated = [];
      proto.animate = function () { animated.push(this); return {}; };
      D.ui.setFlash('off'); click(fb('CLR')); await run(700);
      const cell = rowsOf(A.tb)[6].children[1], n0 = st.orders.size;
      click(cell);
      check(st.orders.size === n0 && animated.indexOf(cell) >= 0, 'FLASH OFF: a refused click (QTY 0) still flashes amber at the cell (2026-10-06: OFF removed it)');
      D.dom.setReduceMotion(true); animated.length = 0;
      const cell2 = rowsOf(A.tb)[7].children[1];
      click(cell2); P.frame(); P.frame();
      check(!animated.length && cell2.hasAttribute('data-warn'), 'under Reduce Motion it is a static amber outline (no animation), kept through the repaint', cell2.className);
      await new Promise(r => setTimeout(r, 700));
      check(!cell2.hasAttribute('data-warn'), 'gone after 600 ms');
      D.dom.setReduceMotion(false); D.ui.setFlash('subtle'); proto.animate = anim0;
      click(fb('1')); await run(300);
    }
    // (7) DAY flashes when its realized part moves, not on marks
    {
      const hc = $('hc-day'), proto = Object.getPrototypeOf(hc), anim0 = proto.animate;
      let n = 0; proto.animate = function () { if (this === hc) n++; return {}; };
      const mv = Array.from(st.markets.values()).find(m => m.kind === 'LIP15' && m.status === 'OPEN' && m.close_ms - P.clock.t > 120000 && st.book(m.key) && st.book(m.key).ba !== null);
      const ba = st.book(mv.key).ba;
      D.act.order({ v: mv.v, tk: mv.tk, side: 'BUY', px: Desk_step(D, mv, ba, 2), qty: 20, tif: 'IOC', touch: ba }, null);
      await run(2500);
      check(st.position(mv.key) && st.position(mv.key).pos > 0, 'a position held on a moving market');
      n = 0;
      const seen = new Set();
      for (let i = 0; i < 80; i++) { await run(250, 250); seen.add(Math.round(st.hdr.day.total / 100)); }
      check(seen.size >= 2, 'its marks move the day\'s total', seen.size);
      eq(n, 0, 'SUBTLE: DAY does not flash on marks (2026-10-06: 13 flashes in 20 s with the lanes running)');
      // the flash gap is real time (performance.now(), 1.5 s) and these loops run 20 s of sim time
      // in about a second of it: wait out the fill's flash first, or FULL may have nothing to show
      await new Promise(r => setTimeout(r, 1600));
      D.ui.setFlash('full'); n = 0;
      for (let i = 0; i < 80; i++) await run(250, 250);
      check(n > 0, 'FULL: it does', n);
      D.ui.setFlash('subtle');
      await new Promise(r => setTimeout(r, 1600));      // the cell's quiet time after the FULL flashes
      n = 0;
      D.manual.flatten(mv.key, null); await run(1500);
      check(st.position(mv.key).pos === 0 && n >= 1, 'a fill that realizes P&L flashes it in SUBTLE', { pos: st.position(mv.key).pos, n });
      proto.animate = anim0;
    }
  }

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

  // =============================================================================================
  // 4. desktop alerts: permission from a click, filters, the 4 s rate limit, the sound
  // =============================================================================================
  {
    class Note { constructor(title, o) { Note.list.push({ title, body: o.body, tag: o.tag }); } close() {} }
    Note.list = []; Note.permission = 'default'; Note.asked = 0;
    Note.requestPermission = cb => { Note.asked++; Note.permission = 'granted'; if (cb) cb('granted'); return Promise.resolve('granted'); };
    let tones = 0;
    class Audio { constructor() { this.state = 'running'; this.currentTime = 0; this.destination = {}; }
      createOscillator() { return { frequency: { value: 0 }, connect() {}, start() { tones++; }, stop() {} }; }
      createGain() { return { gain: { setValueAtTime() {}, exponentialRampToValueAtTime() {} }, connect() {} }; } }
    const Q = bootPage({ seed: 13, Notification: Note, AudioContext: Audio });
    await Q.run(1500);
    const A = Q.D.activity, su = Q.D.header.su;
    check(!A.cfg.on && !A.cfg.sound && A.cfg.when === 'hidden' && A.cfg.MAN.join() === 'FILL,PART,EXIT' && A.cfg.SYS.join() === 'EXIT,REJ,HALT',
      'alerts start off, only while the page is hidden: MAN every fill (FILL, PART) and exit; SYS exits, rejects and a halt', [A.cfg.MAN.join(), A.cfg.SYS.join()]);
    Q.key('KeyP', 'p'); Q.frame();
    // TEST says what it did (2026-10-06: nothing showed with alerts blocked and the sound off)
    const testBtn = m => su.test.btns.find(b => b.textContent === m);
    Q.click(testBtn('SYS')); Q.frame();
    eq(su.testSt.textContent, 'off: turn Desktop or Sound on', 'TEST with Desktop and Sound off says so');
    const onBtn = su.desk.btns.find(b => b.textContent === 'ON');
    Q.fire(onBtn, 'pointerdown', { button: 0 });
    eq(Note.asked, 0, 'the permission is never asked on pointerdown');
    Q.fire(onBtn, 'click', {}); await Q.run(100);
    check(Note.asked === 1 && A.cfg.on && /allowed/.test(su.deskSt.textContent), 'Desktop ON asks for the permission from the click, and shows it allowed', su.deskSt.textContent);
    Q.click(su.sound.btns.find(b => b.textContent === 'ON'));
    Q.click(su.when.btns.find(b => b.textContent === 'ALWAYS'));
    check(A.cfg.sound && A.cfg.when === 'always' && Q.D.storage.getJSON('alerts').on === true, 'sound and ALWAYS are saved');
    const nl0 = Note.list.length;
    Q.click(testBtn('MAN')); Q.frame();
    check(Note.list.length === nl0 + 1 && /^shown, sound played$/.test(su.testSt.textContent) && /\bok\b/.test(su.testSt.className), 'TEST shows a test alert and says shown', su.testSt.textContent);
    Note.permission = 'denied';
    Q.click(testBtn('MAN')); Q.frame();
    check(Note.list.length === nl0 + 1 && /^needs the served page \(127\.0\.0\.1\), sound played$/.test(su.testSt.textContent) && /\bwarn\b/.test(su.testSt.className),
      'with the permission refused on a page opened from a file, TEST says it needs the served page', su.testSt.textContent);
    check(/needs the served page/.test(su.deskSt.textContent), 'and so does the Desktop row');
    Note.permission = 'granted'; A.sent.MAN = 0; Note.list.length = 0; tones = 0;
    Q.key('Escape', 'Escape');
    const st2 = Q.D.store, nk = Q.D.focus.ladders.A;
    Q.sim().freeze(nk, { bids: [[6300, 120]], asks: [[6400, 300]] }); await Q.run(300);
    const buy = () => Q.D.act.order({ v: 'K', tk: st2.market(nk).tk, side: 'BUY', px: 6400, qty: 1, tif: 'IOC', touch: 6400 }, null);
    buy(); await Q.run(400);
    check(Note.list.length === 1 && Note.list[0].title === 'MAN FILL' && /BUY 1 .*@0\.64/.test(Note.list[0].body) && Note.list[0].tag === 'desk-MAN', 'a fill raises one desktop alert: MAN FILL, what and where', Note.list);
    check(tones === 1, 'with one short tone');
    eq(A.list('MAN', 'ALL').filter(e => e.type === 'SUB').length >= 1 && Note.list.every(n => !/^MAN SUB/.test(n.title)), true, 'a sent order is on the tape but raises no alert (not in the filter)');
    buy(); buy(); await Q.run(1000);
    eq(Note.list.length, 1, 'more fills inside 4 s wait (one alert per mode per 4 s)');
    await Q.run(3500, 250);
    await new Promise(r => setTimeout(r, 4100));       // the batch flushes on a real 4 s timer
    check(Note.list.length === 2 && /^MAN: 2 fills$/.test(Note.list[1].title) && Note.list[1].body.split('\n').length === 2, 'then one alert for the batch: MAN: 2 fills', Note.list.slice(1));
    Q.D.activity.toggleType('MAN', 'SUB');
    Q.D.activity.sent.MAN = 0;
    buy(); await Q.run(400);
    check(Note.list.some(n => /SUB/.test(n.title) || /SUB/.test(n.body)), 'a type switched on in the filter alerts too');
    Q.D.activity.toggleType('MAN', 'SUB');
    // a manual order filled in part alerts (PART is in MAN's default filter: 2026-10-06 review)
    {
      Q.sim().freeze(nk, { bids: [[6200, 50]], asks: [[6400, 300]] }); await Q.run(300);
      Q.D.act.order({ v: 'K', tk: st2.market(nk).tk, side: 'BUY', px: 6300, qty: 10, tif: 'POST', ttl_s: 60, touch: 6200 }, null);
      await Q.run(400);
      await new Promise(r => setTimeout(r, 4100)); A.sent.MAN = 0;
      const nn = Note.list.length;
      Q.sim().trade(nk, 'SELL', 4); await Q.run(400);
      const part = A.list('MAN', 'FILLS')[0];
      check(part && part.type === 'PART' && part.qty === 4, 'a resting manual order filled 4 of 10 is a PART on the tape', part);
      check(Note.list.length === nn + 1 && /^MAN PART$/.test(Note.list[nn].title) && /PART BUY 4 /.test(Note.list[nn].body), 'and raises an alert', Note.list.slice(nn));
      Q.D.act.cancelAll('MAN', null); await Q.run(400);
    }
    // SYS: exits, rejects and a halt, as a digest every 30 s
    {
      eq(A.RATE.SYS + '/' + A.RATE.MAN, '30000/4000', 'one SYS alert per 30 s (a digest), one MAN alert per 4 s');
      eq(A.digest('SYS', [{ type: 'EXIT', real: 5000 }, { type: 'EXIT', real: 7000 }, { type: 'REJ' }]), 'SYS: 2 exits, 1 reject, real +1.20', 'the digest counts what happened and the exits\' realized P&L');
      const id0 = A.n, nn = Note.list.length;
      const h = JSON.parse(JSON.stringify(Q.D.contract.examples.hdr)); h.mode = 'DEMO'; h.run = { state: 'RUN', since_ms: Q.clock.t, rearm_ms: null, why: '' };
      Q.D.store._handlers.hdr(h);
      h.run = { state: 'HALTED', since_ms: Q.clock.t, rearm_ms: null, why: 'daily stop' }; A.sent.SYS = Date.now();
      Q.D.store._handlers.hdr(h);
      const ht = A.lists.SYS.filter(e => e.id > id0);
      check(ht.length === 1 && ht[0].type === 'HALT' && /daily stop/.test(A.line(ht[0])), 'Systematic halting by the daily stop is on its tape: HALT', ht);
      check(Note.list.length === nn + 1 && /^SYS HALT$/.test(Note.list[nn].title), 'and alerts at once, whatever the 30 s digest holds', Note.list.slice(nn));
      Q.D.store._handlers.hdr(h);
      eq(A.lists.SYS.filter(e => e.id > id0).length, 1, 'once');
    }
    eq(Q.D.render.errors, 0, 'no render errors (alerts)');
  }
  // a saved SOUND ON: Safari starts the audio only from a gesture, so the drawer asks for a tap
  // and the first pointer release or key starts it (2026-10-06 review)
  {
    let resumes = 0;
    class Audio2 { constructor() { this.state = 'suspended'; this.currentTime = 0; this.destination = {}; Audio2.made++; }
      resume() { resumes++; this.state = 'running'; if (this.onstatechange) this.onstatechange(); return Promise.resolve(); }
      createOscillator() { return { frequency: { value: 0 }, connect() {}, start() {}, stop() {} }; }
      createGain() { return { gain: { setValueAtTime() {}, exponentialRampToValueAtTime() {} }, connect() {} }; } }
    Audio2.made = 0;
    const R2 = bootPage({ seed: 17, AudioContext: Audio2, storage: { 'desk3.alerts': JSON.stringify({ v: 2, on: false, sound: true, when: 'hidden', SYS: ['EXIT'], MAN: ['FILL', 'PART', 'EXIT'] }) } });
    await R2.run(1000);
    R2.D.header.openSet(true); R2.frame();            // (a key would be the gesture that starts it)
    const su2 = R2.D.header.su;
    check(R2.D.activity.cfg.sound && Audio2.made === 0 && su2.soundSt.textContent === 'tap to enable', 'a saved SOUND ON waits for a gesture: the drawer says tap to enable', su2.soundSt.textContent);
    R2.fire(R2.$('m-ticket'), 'pointerup', { button: 0 }); await R2.run(100);
    check(Audio2.made === 1 && resumes === 1 && su2.soundSt.textContent === '', 'the first pointer release starts the audio', { made: Audio2.made, resumes, st: su2.soundSt.textContent });
    R2.fire(R2.$('m-ticket'), 'pointerup', { button: 0 });
    eq(resumes, 1, 'and only the first');
    const old = bootPage({ seed: 18, storage: { 'desk3.alerts': JSON.stringify({ on: true, sound: false, when: 'hidden', SYS: ['FILL', 'EXIT'], MAN: ['FILL', 'EXIT'] }) } });
    await old.run(300);
    check(old.D.activity.cfg.on && old.D.activity.cfg.SYS.join() === 'EXIT,REJ,HALT' && old.D.activity.cfg.MAN.join() === 'FILL,PART,EXIT', 'filters saved with the old defaults take the new ones (Desktop ON kept)', old.D.activity.cfg);
  }

  console.log(fails ? `${fails} of ${checks} checks FAILED` : `ui desk v3 manual: ${checks} checks, 0 failures`);
  process.exit(fails ? 1 : 0);
})().catch(e => { console.log('FAIL: test crashed: ' + (e && e.stack || e)); process.exit(1); });
