// The v3 screen's SYSTEMATIC tab (ui/desk/js/systematic.js), checked without a browser or any
// package. Two parts:
//   1. the pure maths in a plain vm: P(daily stop) against redesign v3's table, reservations,
//      contract-weighted markouts, the amber "realized well under predicted" rule, equity,
//      derived preflight rows, the St text of finished orders
//   2. the whole page booted in a small fake DOM (index.html parsed into a tree, events that
//      bubble, a fake clock driving the demo venues): the strip, START/STOP with a pending paint
//      on the next frame, preflight failures disabling START with the reason, the lanes grid
//      (toggle, kill, filter, amber), working and fills filtered to Systematic, the risk book
//      grouped by outcome with the cash footer, markouts, row linking to the focused market,
//      WIDE and narrow column sets, KILL, the stale rule, and no render errors.
//   node tests/test_ui_desk_v3_systematic.cjs
'use strict';
const fs = require('fs'), path = require('path'), vm = require('vm');
const DESK = path.join(__dirname, '..', 'ui', 'desk');
const JS = path.join(DESK, 'js');

let checks = 0, fails = 0;
function check(cond, what, detail) {
  checks++;
  if (!cond) { fails++; console.log('FAIL: ' + what + (detail !== undefined ? '  ' + (typeof detail === 'string' ? detail : JSON.stringify(detail)) : '')); }
}
function eq(a, b, what) { check(a === b, what, { got: a, want: b }); }
function near(a, b, tol, what) { check(typeof a === 'number' && Math.abs(a - b) <= tol, what, { got: a, want: b, tol }); }
const M = '\u2212';

// ---------------------------------------------------------------------------------------------
// A small DOM: enough of Element/Document for the desk's code (keyedRows, closest, dataset,
// classList, style, disabled, bubbling click/keydown events with capture).
// ---------------------------------------------------------------------------------------------
const VOID = new Set(['meta', 'link', 'input', 'br', 'img', 'col', 'hr']);
class ClassList {
  constructor(e) { this.e = e; }
  get set() { return new Set((this.e._cls || '').split(/\s+/).filter(Boolean)); }
  contains(c) { return this.set.has(c); }
  add(c) { const s = this.set; s.add(c); this.e._cls = [...s].join(' '); }
  remove(c) { const s = this.set; s.delete(c); this.e._cls = [...s].join(' '); }
  toggle(c, on) { const want = on === undefined ? !this.contains(c) : !!on; if (want) this.add(c); else this.remove(c); return want; }
}
function dataKey(k) { return 'data-' + k.replace(/[A-Z]/g, m => '-' + m.toLowerCase()); }
class Node_ {
  constructor(tag, doc) {
    this.tagName = tag.toUpperCase(); this.ownerDocument = doc;
    this._children = []; this.parentNode = null; this._attrs = {}; this._cls = ''; this._text = ''; this._ls = [];
    this.classList = new ClassList(this);
    const self = this;
    this.style = { _p: {}, setProperty(k, v) { this._p[k] = String(v); }, getPropertyValue(k) { return this._p[k] || ''; } };
    this.dataset = new Proxy({}, {
      get(t, k) { const v = self._attrs[dataKey(String(k))]; return v === undefined ? undefined : v; },
      set(t, k, v) { self._attrs[dataKey(String(k))] = String(v); return true; },
      has(t, k) { return dataKey(String(k)) in self._attrs; }
    });
  }
  get id() { return this._attrs.id || ''; } set id(v) { this._attrs.id = String(v); }
  get className() { return this._cls; } set className(v) { this._cls = String(v); }
  get title() { return this._attrs.title || ''; } set title(v) { this._attrs.title = String(v); }
  get disabled() { return 'disabled' in this._attrs; } set disabled(v) { if (v) this._attrs.disabled = ''; else delete this._attrs.disabled; }
  get children() { return this._children; }
  get firstChild() { return this._children[0] || null; }
  get lastChild() { return this._children[this._children.length - 1] || null; }
  get nextSibling() { if (!this.parentNode) return null; const a = this.parentNode._children; return a[a.indexOf(this) + 1] || null; }
  get textContent() { return this._children.length ? this._children.map(c => c.textContent).join('') : this._text; }
  set textContent(v) { this._children.forEach(c => { c.parentNode = null; }); this._children = []; this._text = String(v); }
  get innerHTML() { return this._text; }
  set innerHTML(v) { this.ownerDocument.innerHTMLWrites++; this._children = []; this._text = String(v).replace(/<[^>]*>/g, ''); }
  appendChild(c) { return this.insertBefore(c, null); }
  insertBefore(c, ref) {
    if (c.parentNode) c.parentNode.removeChild(c);
    c.parentNode = this; this._text = '';
    const i = ref ? this._children.indexOf(ref) : -1;
    if (i < 0) this._children.push(c); else this._children.splice(i, 0, c);
    return c;
  }
  removeChild(c) { const i = this._children.indexOf(c); if (i >= 0) this._children.splice(i, 1); c.parentNode = null; return c; }
  remove() { if (this.parentNode) this.parentNode.removeChild(this); }
  getAttribute(k) { if (k === 'class') return this._cls; return k in this._attrs ? this._attrs[k] : null; }
  setAttribute(k, v) { if (k === 'class') this._cls = String(v); else this._attrs[k] = String(v); }
  removeAttribute(k) { if (k === 'class') this._cls = ''; else delete this._attrs[k]; }
  hasAttribute(k) { return k === 'class' ? !!this._cls : k in this._attrs; }
  addEventListener(t, fn, cap) { this._ls.push({ t, fn, cap: !!(cap && (cap === true || cap.capture)) }); }
  removeEventListener(t, fn) { this._ls = this._ls.filter(l => !(l.t === t && l.fn === fn)); }
  matches(sel) { return sel.split(',').some(s => simpleMatch(this, s.trim())); }
  closest(sel) { let e = this; while (e && e.tagName) { if (e.matches(sel)) return e; e = e.parentNode; } return null; }
  contains(x) { while (x) { if (x === this) return true; x = x.parentNode; } return false; }
  animate() { return {}; } get offsetWidth() { return 0; } blur() {} focus() {} get isContentEditable() { return false; }
  walk(fn) { fn(this); this._children.forEach(c => c.walk(fn)); }
}
function simpleMatch(e, s) {
  const m = /^([a-zA-Z0-9]*)((?:\.[\w-]+|\[[^\]]+\])*)$/.exec(s);
  if (!m) throw new Error('fake DOM: unsupported selector ' + s);
  if (m[1] && e.tagName !== m[1].toUpperCase()) return false;
  const parts = m[2].match(/\.[\w-]+|\[[^\]]+\]/g) || [];
  return parts.every(p => {
    if (p[0] === '.') return e.classList.contains(p.slice(1));
    const a = /^\[([\w-]+)(?:="([^"]*)")?\]$/.exec(p);
    if (!a) throw new Error('fake DOM: unsupported attribute selector ' + p);
    return a[2] === undefined ? e.hasAttribute(a[1]) : e.getAttribute(a[1]) === a[2];
  });
}
function makeDocument(html) {
  const doc = { innerHTMLWrites: 0, readyState: 'interactive', _ls: [] };
  const htmlEl = new Node_('html', doc), body = new Node_('body', doc);
  htmlEl.appendChild(body);
  const bm = /<body([^>]*)>([\s\S]*)<\/body>/.exec(html);
  for (const a of bm[1].matchAll(/([\w-]+)="([^"]*)"/g)) body.setAttribute(a[1], a[2]);
  const stack = [body];
  const re = /<!--[\s\S]*?-->|<\/(\w+)\s*>|<(\w+)((?:\s+[\w-]+(?:="[^"]*")?)*)\s*\/?>|([^<]+)/g;
  let m;
  while ((m = re.exec(bm[2]))) {
    if (m[0].startsWith('<!--')) continue;
    if (m[1]) { const top = stack[stack.length - 1]; if (top.tagName === m[1].toUpperCase()) stack.pop(); else throw new Error('fake DOM: mismatched </' + m[1] + '>'); continue; }
    if (m[2]) {
      const e = new Node_(m[2], doc);
      for (const a of (m[3] || '').matchAll(/([\w-]+)(?:="([^"]*)")?/g)) e.setAttribute(a[1], a[2] === undefined ? '' : a[2]);
      stack[stack.length - 1].appendChild(e);
      if (!VOID.has(m[2].toLowerCase()) && !m[0].endsWith('/>')) stack.push(e);
      continue;
    }
    if (m[4] && m[4].trim()) { const top = stack[stack.length - 1]; if (!top._children.length) top._text += m[4].trim(); }
  }
  if (stack.length !== 1) throw new Error('fake DOM: unclosed ' + stack[stack.length - 1].tagName);
  Object.assign(doc, {
    documentElement: htmlEl, body, activeElement: body,
    createElement: t => new Node_(t, doc),
    getElementById: id => { let hit = null; htmlEl.walk(e => { if (!hit && e._attrs.id === id) hit = e; }); return hit; },
    addEventListener: (t, fn, cap) => doc._ls.push({ t, fn, cap: !!cap }),
    removeEventListener: () => {}
  });
  return doc;
}
// dispatch with a capture phase (document down) and a bubble phase (target up)
function fire(doc, target, type, init) {
  const ev = Object.assign({ type, target, bubbles: true, shiftKey: false, altKey: false, metaKey: false, ctrlKey: false,
    timeStamp: performance.now(), defaultPrevented: false, _stop: false,
    preventDefault() { this.defaultPrevented = true; }, stopPropagation() { this._stop = true; } }, init || {});
  const pathUp = []; for (let e = target; e; e = e.parentNode) pathUp.push(e);
  const call = (node, cap) => { const ls = node._ls.filter(l => l.t === type && l.cap === cap); for (const l of ls) { ev.currentTarget = node; l.fn(ev); } };
  call(doc, true);
  for (let i = pathUp.length - 1; i >= 0 && !ev._stop; i--) call(pathUp[i], true);
  for (let i = 0; i < pathUp.length && !ev._stop; i++) call(pathUp[i], false);
  if (!ev._stop) call(doc, false);
  return ev;
}
const KEYCODE = k => (/^[a-z]$/i.test(k) ? 'Key' + k.toUpperCase() : /^[0-9]$/.test(k) ? 'Digit' + k : k === 'Escape' ? 'Escape' : '');

// ---------------------------------------------------------------------------------------------
// Boot the page: every script in index.html order, demo mode from file://, a fake clock.
// ---------------------------------------------------------------------------------------------
function bootPage(opts) {
  opts = opts || {};
  const html = fs.readFileSync(path.join(DESK, 'index.html'), 'utf8');
  const doc = makeDocument(html);
  const errors = [];
  const media = {};
  let vw = opts.width || 1512;
  const mq = q => {
    if (!media[q]) {
      const o = { q, ls: [], addEventListener(t, f) { this.ls.push(f); }, addListener(f) { this.ls.push(f); } };
      Object.defineProperty(o, 'matches', { get() {
        let m;
        if ((m = /min-width:\s*(\d+)px/.exec(q))) return vw >= +m[1];
        if ((m = /max-width:\s*(\d+)px/.exec(q))) return vw <= +m[1];
        return false;
      } });
      media[q] = o;
    }
    return media[q];
  };
  const clock = { t: opts.t0 || Date.UTC(2026, 9, 6, 17, 30, 0) };
  const store = Object.assign({}, opts.storage || {});
  const ctx = {
    console: { log: console.log, warn: console.warn, info() {}, debug() {}, error: (...a) => { errors.push(a.map(x => (x && x.stack) || String(x)).join(' ')); } },
    setTimeout, clearTimeout, setInterval, clearInterval, performance, document: doc, Promise, Uint8Array, Map, Set, JSON, Math,
    location: { protocol: 'file:', search: opts.search || '?seed=11' },
    localStorage: { getItem: k => (k in store ? store[k] : null), setItem: (k, v) => { store[k] = String(v); } },
    matchMedia: mq, requestAnimationFrame: () => {}, confirm: () => true, crypto: require('crypto').webcrypto,
    AbortController, fetch: () => Promise.reject(new Error('no network in the test')), __now: () => clock.t
  };
  ctx.window = ctx;
  vm.createContext(ctx);
  vm.runInContext('Date.now = function () { return __now(); };', ctx);
  const order = [...html.matchAll(/<script defer src="js\/([\w.]+)"><\/script>/g)].map(x => x[1]);
  for (const f of order) vm.runInContext(fs.readFileSync(path.join(JS, f), 'utf8'), ctx, { filename: f });
  const D = ctx.Desk;
  D.tick.stop();                                  // the test drives the 250 ms clock itself
  const page = {
    ctx, D, doc, errors, clock, media,
    $: id => doc.getElementById(id),
    sim: () => D.transport.sim(),
    flush: () => D.render.flush(),
    async settle() { for (let i = 0; i < 3; i++) { await new Promise(r => setTimeout(r, 0)); await new Promise(r => setImmediate(r)); } },
    // advance the fake clock: pump the venues, tick 4/s, let promises settle, paint once a second
    async advance(ms, paintEvery) {
      paintEvery = paintEvery || 1000;
      const end = clock.t + ms; let nextTick = clock.t + 250, nextPaint = clock.t + paintEvery;
      while (clock.t < end) {
        clock.t = Math.min(end, clock.t + 50);
        const s = page.sim(); if (s) s.pump();
        if (clock.t >= nextTick) { nextTick += 250; D.tick.run(); }
        await new Promise(r => setImmediate(r));
        if (clock.t >= nextPaint) { nextPaint += paintEvery; page.flush(); }
      }
      await page.settle();
      page.flush();
    },
    click(el, init) { return fire(doc, el, 'click', init); },
    key(k, init) { return fire(doc, doc.activeElement, 'keydown', Object.assign({ key: k, code: KEYCODE(k) }, init || {})); },
    setWidth(w) { vw = w; Object.values(media).forEach(o => o.ls.forEach(f => f({ matches: o.matches }))); },
    text: el => (el ? el.textContent : null),
    rows: id => { const b = doc.getElementById(id); const tb = b && b._children[0] && b._children[0]._children.find(c => c.tagName === 'TBODY'); return tb ? tb._children : []; },
    heads: id => { const b = doc.getElementById(id); const th = b && b._children[0] && b._children[0]._children.find(c => c.tagName === 'THEAD'); return th ? th._children[0]._children.map(c => c.textContent) : []; },
    cell: (row, id, gridId) => { const h = page.heads(gridId); return row._children[h.indexOf(id)]; }
  };
  return page;
}

(async () => {
  // ===========================================================================================
  // 1. Pure maths
  // ===========================================================================================
  {
    const ctx = { console, setTimeout, clearTimeout, setInterval, clearInterval, performance, localStorage: { getItem() { return null; }, setItem() {} } };
    ctx.window = ctx; vm.createContext(ctx);
    for (const f of ['contract.js', 'core.js', 'store.js', 'systematic.js']) vm.runInContext(fs.readFileSync(path.join(JS, f), 'utf8'), ctx, { filename: f });
    const D = ctx.Desk, T = D.systematic._t, st = D.store;
    check(D.systematic && typeof D.systematic.init === 'function' && typeof D.systematic.render === 'function', 'systematic.js registers {init, render}');
    D.render.scheduled = true;
    // normal CDF and the daily-stop chance against redesign v3's table (zero edge, whole day)
    near(T.ncdf(0), 0.5, 1e-7, 'ncdf(0) = 0.5');
    near(T.ncdf(-1.96), 0.0249979, 1e-6, 'ncdf(-1.96)');
    near(T.ncdf(1.25) + T.ncdf(-1.25), 1, 1e-9, 'ncdf is symmetric');
    const pct = x => Math.round(x * 100);
    eq(pct(T.pStop(250000, 150000, 1)), 10, 'P(stop) $25 stop at $15 SD = 10% (redesign v3 table)');
    eq(pct(T.pStop(250000, 200000, 1)), 21, 'P(stop) $25 stop at $20 SD = 21%');
    eq(pct(T.pStop(250000, 250000, 1)), 32, 'P(stop) $25 stop at $25 SD = 32%');
    eq(pct(T.pStop(300000, 200000, 1)), 13, 'P(stop) $30 stop at $20 SD = 13%');
    eq(pct(T.pStop(400000, 250000, 1)), 11, 'P(stop) $40 stop at $25 SD = 11%');
    eq(T.pStop(0, 200000, 1), 1, 'P(stop) is 1 once the stop is used up');
    check(T.pStop(250000, 200000, 0.25) < T.pStop(250000, 200000, 1), 'P(stop) falls as the day runs out');
    eq(T.pStop(250000, 200000, 0), 0, 'P(stop) at the end of the day is 0');
    const noon = new Date(2026, 9, 6, 12, 0, 0).getTime();
    near(T.dayLeft(noon), 0.5, 1e-9, 'dayLeft at local noon is a half');
    // reservations: worst case of a working order; a reducing order reserves nothing
    eq(T.reserve('BUY', 6000, 10, 0), 60000, 'a buy of 10 at 0.60 reserves 6.00');
    eq(T.reserve('SELL', 6000, 10, 0), 40000, 'a sell of 10 at 0.60 (buy NO at 0.40) reserves 4.00');
    eq(T.reserve('SELL', 6000, 10, 10), 0, 'a sell that only closes a long reserves nothing');
    eq(T.reserve('BUY', 6000, 10, -4), 36000, 'a buy that closes 4 short reserves the other 6');
    // markout and edge statistics, contract-weighted (u per contract)
    const a = T.newAcc();
    T.addFill(a, { qty: 10, side: 'BUY', px: 6000, fee: 0, mid: 6050, fair: 6200, mo: { '1s': 50, '10s': 100, '60s': 200 } });
    T.addFill(a, { qty: 30, side: 'SELL', px: 6000, fee: 3000, mid: 5950, fair: 5900, mo: { '10s': -100, '60s': -100 } });
    const s = T.stats(a);
    eq(s.n, 2, 'stats count fills'); eq(s.ctr, 40, 'stats count contracts');
    near(s.m10, (10 * 100 + 30 * -100) / 40, 1e-9, 'MO10 is contract-weighted');
    near(s.m1, 50, 1e-9, 'MO1 averages only the fills where it matured');
    near(s.pred, (10 * 200 + 30 * (100 - 100)) / 40, 1e-9, 'Pred = (fair - px) x side - fee per contract');
    near(s.rlzd, (10 * 200 + 30 * (-100 - 100)) / 40, 1e-9, 'Rlzd = MO60 - fee per contract');
    near(s.sprd, (10 * 50 + 30 * 50) / 40, 1e-9, 'spread paid is + when we traded better than the mid');
    near(s.adv, 0.5, 1e-9, 'Adv% = share of fills with a negative 60 s markout');
    near(s.feePer, 3000 / 40, 1e-9, 'fee per contract');
    // the amber rule: realized well under predicted over >= 30 fills with both
    const good = T.newAcc(), bad = T.newAcc(), few = T.newAcc();
    for (let i = 0; i < 40; i++) {
      T.addFill(good, { qty: 5, side: 'BUY', px: 5000, fee: 0, mid: 5000, fair: 5200, mo: { '60s': 150 } });
      T.addFill(bad, { qty: 5, side: 'BUY', px: 5000, fee: 0, mid: 5000, fair: 5200, mo: { '60s': -100 } });
      if (i < 20) T.addFill(few, { qty: 5, side: 'BUY', px: 5000, fee: 0, mid: 5000, fair: 5200, mo: { '60s': -100 } });
    }
    check(!T.stats(good).warn, 'realized slightly under predicted (2.0c vs 1.5c) is not amber');
    check(T.stats(bad).warn, 'realized far under predicted (2.0c vs -1.0c) turns the lane amber');
    check(!T.stats(few).warn, 'fewer than 30 fills never turn a lane amber');
    // the store-driven pieces: lane rows with the amber flag, preflight rows
    const C = D.contract;
    st._handlers.hdr(C.examples.hdr);
    st._handlers.lane(C.examples.lane);
    st._handlers.lane(Object.assign({}, C.examples.lane, { id: 'M2', name: 'BAND', lip: '0.0000', day: '1.5400' }));
    for (let i = 0; i < 40; i++) {
      st._handlers.fill(Object.assign({}, C.examples.fill, { fid: 'F' + i, own: 'SYS', lane: 'M1', side: 'BUY', px: '0.6000', fair: '0.6200', qty: 5 }));
      st._handlers.mo({ fid: 'F' + i, h: '60s', mo: '-0.0100', mid: '0.5900' });
      st._handlers.fill(Object.assign({}, C.examples.fill, { fid: 'G' + i, own: 'SYS', lane: 'M2', side: 'BUY', px: '0.6000', fair: '0.6200', qty: 5 }));
      st._handlers.mo({ fid: 'G' + i, h: '60s', mo: '0.0180', mid: '0.6180' });
    }
    st._handlers.fill(Object.assign({}, C.examples.fill, { fid: 'MAN1', own: 'MAN', lane: null }));
    T.recompute();
    const lr = T.laneRows();
    eq(lr.map(r => r.label).join(','), 'M1 LIP-15,M2 BAND,TOTAL', 'one row per lane plus TOTAL');
    check(lr[0].warn && !lr[1].warn, 'the lane with realized -1.0c against predicted +2.0c is amber; the one at +1.8c is not', lr.map(r => r.warn));
    eq(lr[0].pnl, lr[0].real + lr[0].unrl, 'lane P&L = real + unrl (LIP kept in its own column)');
    eq(lr[2].lip, 20000 + 0, 'TOTAL sums LIP (M1 2.00 + M2 0.00)');
    check(lr[0].lip === 20000 && lr[1].lip === 0, 'LIP stays per lane');
    eq(T.agg().all.n, 80, 'markouts count Systematic fills only (the manual fill is left out)');
    eq(T.preflightRows(st.hdr).map(r => r.id + ':' + r.ok).join(' '), 'KEY:true CASH:true SHARD:true CLK:true DISK:true FEED:true PRIV:true GROUPS:true',
      'preflight shows the plan set, PRIV and GROUPS from the backend (contract 1.1)');
    // an older backend without PRIV and GROUPS rows: PRIV derived from the feed, GROUPS unknown
    const old = Object.assign({}, st.hdr, { preflight: st.hdr.preflight.filter(r => r.id !== 'PRIV' && r.id !== 'GROUPS') });
    eq(T.preflightRows(old).map(r => r.id + ':' + r.ok).join(' '), 'KEY:true CASH:true SHARD:true CLK:true DISK:true FEED:true PRIV:null GROUPS:null',
      'without the rows: PRIV unknown before any heartbeat, GROUPS unknown');
    st.feeds = { KX: { up: true, gap: false, lag_ms: 1 }, KXP: { up: false, gap: false, lag_ms: null }, PM: { up: true, gap: false, lag_ms: 1 } };
    const priv = T.preflightRows(old).find(r => r.id === 'PRIV');
    check(priv.ok === false && priv.why === 'KXP down', 'PRIV derives from the private feed when the backend sends no PRIV row', priv);
    eq(T.preflightRows(st.hdr).find(r => r.id === 'PRIV').ok, true, 'a backend PRIV row wins over the derived one');
    {   // START and a HALT: the equity floor's ends with a Start (gw checks the equity), the daily stop's does not
      const keep = [st.stale, st.staleWhy, st.vstale.K];
      st.stale = false; st.staleWhy = ''; st.vstale.K = '';
      const halted = why => Object.assign({}, st.hdr, { run: Object.assign({}, st.hdr.run, { state: 'HALTED', why }) });
      check(!T.startGate(halted("equity floor: the account's equity $120.00 is under $150.00 (Start again once it is back over)")).block,
        'a floor HALT leaves START live: gw decides whether the equity is back over the floor');
      check(T.startGate(halted('daily stop')).block, 'a daily-stop HALT keeps START off until ET midnight');
      [st.stale, st.staleWhy, st.vstale.K] = keep;
    }
    // St text of finished orders
    eq(T.stText({ terminal: true, st: 'CXL', tif: 'IOC', why: 'IOC_REST', filled: 0, qty: 10 }), 'IOC 0/10', 'an IOC whose rest was killed reads IOC 0/10');
    eq(T.stText({ terminal: true, st: 'REJ', tif: 'IOC', why: 'PRICE_BAND', filled: 0, qty: 1 }), 'REJ PRICE_BAND', 'a reject carries its reason');
    eq(T.stText({ terminal: true, st: 'EXP', tif: 'POST', why: 'EXPIRY', filled: 2, qty: 5 }), 'EXP 2/5', 'an expiry shows what filled');
    eq(T.stText({ terminal: false, st: 'WORK' }), 'WORK', 'live states read as themselves');
    // equity: cash plus positions marked at the bid
    st.books.clear(); st.pos.clear(); st.markets.clear();
    st._handlers.mkt(C.examples.mkt); st._handlers.book(C.examples.book, null, true); st._handlers.pos(C.examples.pos);
    eq(T.equity(), 1423100 + 987000 + 12 * 6300, 'equity = both venues\' cash + 12 long marked at the 0.63 bid');
    // reservations share a position's reducing room across that side's orders, best price first
    const key = 'K|' + C.examples.pos.tk, mk = (cid, side, px, left) => ({ cid, key, side, px, left, terminal: false });
    st.orders.clear();
    st.orders.set('a', mk('a', 'SELL', 6200, 10)); st.orders.set('b', mk('b', 'SELL', 6000, 10));
    st.orders.set('c', mk('c', 'BUY', 5500, 4));
    eq(T.reservations().get(key), 8 * (10000 - 6200) + 4 * 5500,
      'with a +12 long and two sells of 10, the 0.60 sell reduces 10 and the 0.62 sell reduces 2, reserving 8 x 0.38; the buy reserves 4 x 0.55');
    st.orders.clear();
  }

  // ===========================================================================================
  // 2. The page in a fake DOM, demo venues on a fake clock
  // ===========================================================================================
  const P = bootPage();
  const { D, $ } = P;
  await P.settle();
  P.sim().stop();                                  // drive the venues from the fake clock only
  await P.advance(2000);
  check(D.transport.mode === 'demo', 'file:// boots in demo mode');
  check(!D.store.stale && D.store.hdr && D.store.lanes.size === 5, 'snapshot applied: header and 5 lanes', { stale: D.store.staleWhy, lanes: D.store.lanes.size });
  D.tabs.set('systematic');
  P.flush();
  eq(P.doc.body.getAttribute('data-tab'), 'systematic', 'the SYSTEMATIC tab shows');

  // ---- control strip
  const start = $('s-start'), stop = $('s-stop');
  check(start && stop && start.textContent === 'START' && stop.textContent === 'STOP', 'START and STOP are in the strip');
  eq(P.text($('s-run')), 'STOPPED', 'state text STOPPED');
  const tags = () => $('s-pf')._children.map(e => e.textContent + ':' + e.className);
  eq(tags().map(x => x.split(':')[0]).join(' '), 'KEY CASH SHARD CLK DISK FEED PRIV GROUPS', 'preflight tags in the plan order (plus DISK)');
  check(tags().every(x => x.endsWith(':ok')), 'passing tags are green (the backend reports PRIV and GROUPS)', tags());
  eq(start.disabled, false, 'START is enabled when stopped and every check passes');
  eq(stop.disabled, true, 'STOP is disabled with nothing to stop');
  const plan = P.text($('s-plan'));
  check(/^budget 70% K \d+\.\d\d P \d+\.\d\d \| used \d+\.\d\d \| cost \d+\.\d\d\/4\.80 \| P\(stop\) \d+% \| loss 25\.00\/25\.00$/.test(plan),
    'plan line: budget 70% K P | used | cost used/budget | P(stop) | loss (the daily loss stop); the floor is on the risk summary', plan);
  const ceilK = D.store.hdr.sys.K.ceiling;
  check(plan.indexOf(' K ' + D.fmt.money(ceilK)) > 0, 'plan shows Kalshi\'s 70% ceiling', [plan, ceilK]);
  check(P.doc.innerHTMLWrites === 0, 'nothing writes innerHTML');

  // ---- lanes grid
  let lanes = P.rows('s-lanes-body');
  eq(lanes.length, 6, 'lanes grid: 5 lanes + TOTAL');
  eq(lanes[5].className, 'total', 'the last row is TOTAL, bold');
  const LH = P.heads('s-lanes-body');
  ['Lane', 'State', 'On', 'Budget', 'Used', 'Wrk', 'Fills', 'Ctrs', 'Pred\u00a2', 'Rlzd\u00a2', 'MO10\u00a2', 'MO60\u00a2', 'Fees', 'P&L', 'LIP', 'Day', 'Last', 'Reason'].forEach(h =>
    check(LH.indexOf(h) >= 0, 'lanes grid has a ' + h + ' column at 1512 px', LH));
  check(LH.indexOf('Style') < 0 && LH.indexOf('Unrl') < 0 && LH.indexOf('Thr\u00a2') < 0, 'WIDE-only lane columns are hidden at 1512 px', LH);
  eq(LH.indexOf('On'), LH.indexOf('State') + 1, 'the lane toggle sits next to State');
  const laneRow = id => P.rows('s-lanes-body').find(r => r.getAttribute('data-lane') === id);
  const tgOf = id => laneRow(id)._children.find(c => /\btg\b/.test(c.className));
  const klOf = id => laneRow(id)._children.find(c => /\bkl\b/.test(c.className));
  eq(tgOf('M1').textContent, 'ON', 'M1 toggle reads ON');
  eq(tgOf('X1').textContent, 'OFF', 'X1 (the ESPN experiment) starts OFF');
  eq(P.cell(laneRow('M1'), 'State', 's-lanes-body').textContent, 'IDLE', 'a lane switched on reads IDLE while Systematic is stopped (not OFF)');
  eq(P.cell(laneRow('X1'), 'State', 's-lanes-body').textContent, 'OFF', 'a lane switched off reads OFF');
  check(klOf('M1').className === 'kl' && klOf('X1').className === 'kl dis', 'the lane KILL is grey at rest (red only with live orders)', [klOf('M1').className, klOf('X1').className]);
  eq(P.text($('s-lanes-tools')), 'ON 4/5', 'lanes title counts lanes switched on');

  // ---- START: pending paints on the next frame, before the venue answers
  D.lat.paint.v.length = 0; D.lat.paint.last = null;
  P.click(start);
  P.flush();
  eq(P.text($('s-run')), 'STARTING', 'START paints STARTING on the next frame');
  check(start.disabled && start.className.indexOf('pend') >= 0, 'START shows pending and is disabled while in flight');
  check(D.lat.paint.last !== null && D.lat.paint.last < 50, 'the click-to-paint meter recorded the START click', D.lat.paint.last);
  await P.advance(1000);
  check(/^RUNNING \d\d:\d\d:\d\d \d+s$/.test(P.text($('s-run'))), 'state text RUNNING hh:mm:ss with the run time', P.text($('s-run')));
  eq(start.disabled, true, 'START is disabled while running');
  eq(stop.disabled, false, 'STOP is live while running');
  eq(P.text($('tab-sys-st')), 'RUN', 'the header tab switch reads RUN');

  // ---- let the lanes trade
  await P.advance(4 * 60000);
  const S = D.store;
  const sysFills = S.fills.filter(f => f.own === 'SYS');
  check(sysFills.length > 5, 'the lanes traded', sysFills.length);
  lanes = P.rows('s-lanes-body');
  check(/^\d+$/.test(P.cell(laneRow('M1'), 'Fills', 's-lanes-body').textContent) && +P.cell(laneRow('M1'), 'Fills', 's-lanes-body').textContent === S.lanes.get('M1').fills,
    'lane Fills column shows the lane\'s fills');
  eq(P.cell(laneRow('M1'), 'State', 's-lanes-body').textContent, S.lanes.get('M1').state, 'lane State follows the backend while running');
  {
    const busyLane = ['M1', 'M2'].find(id => S.working(o => o.own === 'SYS' && o.lane === id).length);
    if (busyLane) check(/\blive\b/.test(klOf(busyLane).className), 'a lane with live orders shows its KILL red', klOf(busyLane).className);
  }
  const tot = lanes[5];
  const sumFills = [...S.lanes.values()].reduce((x, l) => x + l.fills, 0);
  eq(P.cell(tot, 'Fills', 's-lanes-body').textContent, String(sumFills), 'TOTAL sums the lanes\' fills');
  // a manual order never shows in the Systematic blotters
  const wx = [...S.markets.values()].find(m => m.kind === 'WEATHER' && m.status === 'OPEN' && S.book(m.key) && S.book(m.key).bb !== null);
  const manCid = D.act.order({ v: wx.v, tk: wx.tk, side: 'BUY', px: D.grid.step(wx.g, S.book(wx.key).bb, -3), qty: 1, tif: 'POST', ttl_s: 60, touch: S.book(wx.key).bb }, { timeStamp: performance.now() });
  await P.advance(1000);
  check(manCid && S.orders.get(manCid) && !S.orders.get(manCid).terminal, 'a manual order is working', manCid && S.orders.get(manCid) && S.orders.get(manCid).st);
  // working: systematic only, every live SYS order listed
  const workRows = () => P.rows('s-work-body').filter(r => r.className !== 'none');
  const liveSys = S.working(o => o.own === 'SYS');
  const shownCids = new Set(workRows().map(r => r.dataset.k));
  check(!shownCids.has(manCid), 'manual orders stay out of the Systematic WORKING grid');
  check(liveSys.every(o => shownCids.has(o.cid)), 'every live Systematic order is in WORKING', { live: liveSys.length, shown: shownCids.size });
  check(workRows().every(r => S.orders.get(r.dataset.k).own === 'SYS'), 'WORKING lists only Systematic orders');
  check(workRows().every(r => r.getAttribute('data-mkt') && r.getAttribute('data-v')), 'working rows carry data-mkt and data-v');
  const WH = P.heads('s-work-body');
  eq(WH.join('|'), 'Time|Lane|Market|Side|Px|Qty|Left|PIQ|TTL|Edge\u00a2|St|', 'WORKING columns at 1512 px follow the spec (cents marked)');
  check(workRows().every(r => { const o = S.orders.get(r.dataset.k); return !o.terminal || !(o.st === 'FILL' || o.st === 'EXP' || (o.st === 'CXL' && (o.why === 'PULL' || o.why === 'MARKET_CLOSED'))); }),
    'WORKING drops routine endings at once (requote pulls, expiries, fills)');
  // fills: systematic only, newest first
  const fillRows = () => P.rows('s-fills-body').filter(r => r.className !== 'none' && r.className.indexOf('none') < 0 && r.className !== 'vpad');
  // FILLS is a virtual list (2026-10-06 browser pass: 200 grid rows relaid out on every fill): all
  // the fills scroll, only the rows in view and a few around them exist
  const FV = D.systematic._t.grids.fills.V, nSysF = S.fills.filter(f => f.own === 'SYS').length;
  check(FV && FV.n === Math.min(D.systematic.cfg.fillsMax, nSysF), 'FILLS lists the Systematic fills (a virtual list over all of them)', [FV && FV.n, nSysF]);
  check(fillRows().length === Math.min(FV.b - FV.a, nSysF) && fillRows().length <= 64, 'only the rows in view (and a few around them) exist in the DOM', fillRows().length);
  check(fillRows().every(r => S.fillIdx.get(r.dataset.k).own === 'SYS'), 'FILLS lists only Systematic fills');
  if (nSysF > 80) {
    const host = $('s-fills-body'), sysF = S.fills.filter(f => f.own === 'SYS');
    host.scrollTop = 40 * D.dom.rowPx(); fire(P.doc, host, 'scroll'); P.flush();
    check(fillRows()[0].dataset.k === sysF[32].fid && fillRows().some(r => r.dataset.k === sysF[40].fid), 'scrolling FILLS draws the fills further down (the window moves 8 rows at a time)', [fillRows()[0].dataset.k, sysF[32].fid]);
    const a0 = FV.a; host.scrollTop = 43 * D.dom.rowPx(); fire(P.doc, host, 'scroll'); P.flush();
    eq(FV.a, a0, 'a scroll inside the drawn window redraws nothing');
    host.scrollTop = 0; fire(P.doc, host, 'scroll'); P.flush();
    eq(fillRows()[0].dataset.k, sysF[0].fid, 'and back at the top, the newest fill first');
  } else check(true, 'too few fills to scroll FILLS (skipped)');
  eq(P.heads('s-fills-body').join('|'), 'Time|Lane|Market|Side|Px|Qty|Liq|Fee|Fair|Edge\u00a2|MO10\u00a2|MO60\u00a2|Real', 'FILLS columns at 1512 px follow the spec (cents marked)');
  const f0 = S.fillIdx.get(fillRows()[0].dataset.k), f1 = S.fillIdx.get(fillRows()[fillRows().length - 1].dataset.k);
  check(f0.ts_ms >= f1.ts_ms, 'FILLS newest first');
  const matured = fillRows().map(r => S.fillIdx.get(r.dataset.k)).find(f => f.mo['10s'] !== undefined);
  if (matured) {
    const r = fillRows().find(x => x.dataset.k === matured.fid);
    eq(P.cell(r, 'MO10\u00a2', 's-fills-body').textContent, D.fmt.cents(matured.mo['10s']), 'a matured MO10 shows signed cents');
  }
  // ACTIVITY: the lanes' tape (sent, filled, cancelled, expired, exits), newest first
  {
    const arows = () => P.rows('s-act-body').filter(r => /^a-/.test(r.className));
    eq(P.heads('s-act-body').join('|'), 'Time|Lane|Ev|Side|Qty|Market|Px|Info', 'SYSTEMATIC has an ACTIVITY tape with the lane');
    // 2026-10-06 review: under ALL the lanes' requotes (a sent and a cancel, ~600 a minute) were a
    // waterfall; the tape opens on FILLS (fills, exits) and ALL is one click away
    const kindBtn = t => P.$('s-act-tools')._children.find(b => b.textContent === t);
    check(/\bon\b/.test(kindBtn('FILLS').className) && arows().length > 0 && arows().every(r => /^(FILL|PART|EXIT|HALT)$/.test(r._children[2].textContent)),
      'the lanes\' tape opens on FILLS: fills and exits only', arows().slice(0, 5).map(r => r._children[2].textContent));
    fire(P.doc, kindBtn('ALL'), 'pointerdown', { button: 0, pointerType: 'mouse' }); P.click(kindBtn('ALL')); P.flush();
    eq(D.storage.get('act.SYS'), 'ALL', 'ALL is one click away, and remembered');
    const evs = new Set(arows().map(r => r._children[2].textContent));
    check(arows().length > 20 && evs.has('SUB') && (evs.has('FILL') || evs.has('PART')) && evs.has('CXL'), 'it shows the lanes\' orders sent, filled and cancelled', [...evs]);
    check(arows().every(r => /^[A-Z]\d$/.test(r._children[1].textContent) || r._children[2].textContent === 'EXIT'), 'every order row names its lane');
    check(arows().every(r => r.getAttribute('data-mkt')), 'every row links to its market');
    const subInfos = arows().filter(r => r._children[2].textContent === 'SUB').map(r => r._children[7].textContent);
    check(subInfos.length && subInfos.every(t => /^(IOC|FOK|POST (5s|15s|60s|5m|30m|CL|ST))$/.test(t)), 'a sent lane order says what it rests for (the venue rounds an expiry up to the second: still POST 15s)', [...new Set(subInfos)]);
    const ts = arows().map(r => r._children[0].textContent);
    check(ts.every((x, i) => !i || x <= ts[i - 1]), 'newest first');
    const fb = $('s-act-tools')._children.find(b => b.textContent === 'FILLS');
    P.click(fb); P.flush();
    check(arows().length && arows().every(r => /^(FILL|PART|EXIT)$/.test(r._children[2].textContent)), 'FILLS keeps fills and exits');
    P.click($('s-act-tools')._children.find(b => b.textContent === 'ORDERS')); P.flush();
    check(arows().length && arows().every(r => /^(SUB|CXL|EXP|REJ)$/.test(r._children[2].textContent)), 'ORDERS keeps the order events');
    P.click($('s-act-tools')._children.find(b => b.textContent === 'ALL')); P.flush();
    D.systematic.setFilter('M1'); await P.advance(300);
    check(arows().length && arows().every(r => r._children[1].textContent === 'M1' || r._children[2].textContent === 'EXIT'), 'the lane filter narrows the tape too');
    D.systematic.setFilter(null); await P.advance(300);
    eq(D.activity.unseen.SYS, 0, 'nothing counts unseen while SYSTEMATIC is on screen');
  }
  // markouts: one row per lane, ALL, then recent fills
  const moRows = P.rows('s-mo-body');
  eq(moRows.slice(0, 6).map(r => r._children[0].textContent.split(' ')[0]).join(','), 'M1,M2,S1,C1,X1,ALL', 'MARKOUTS: one row per lane, then ALL');
  eq(P.heads('s-mo-body').join('|'), 'Lane|n|+1s|+10s|+60s|Settle|t60|Adv%|SprdPd', 'MARKOUTS columns follow the spec');
  const allRow = moRows[5];
  eq(allRow._children[1].textContent, String(S.fills.filter(f => f.own === 'SYS').length), 'ALL n counts Systematic fills');
  check(allRow._children[1].className.indexOf('thin') >= 0 === (S.fills.filter(f => f.own === 'SYS').length < 30), 'n under 30 shows grey (thin)');
  check(moRows.some(r => r.className === 'grp' && r.textContent.indexOf('RECENT') === 0), 'recent fills follow the lane rows');
  // risk book: groups by outcome, members, total; cash footer
  const riskRows = P.rows('s-risk-body');
  const grpRows = riskRows.filter(r => r.className === 'grp');
  const multi = S.risk.groups.filter(g => g.members.length > 1), single = S.risk.groups.filter(g => g.members.length === 1);
  eq(grpRows.length, multi.length, 'a group header only for an outcome with two or more markets');
  eq(riskRows.filter(r => /^solo/.test(r.className)).length, single.length, 'a one-market outcome is one row');
  if (S.risk.groups.length) {
    check(riskRows[riskRows.length - 1].className === 'total', 'the risk book ends with TOTAL');
    const one = riskRows.find(r => /^(solo|mem)/.test(r.className));
    const p = S.position(one.getAttribute('data-mkt'));
    eq(P.cell(one, 'Pos', 's-risk-body').textContent, D.fmt.ssize(p.pos), 'a market row shows its signed position');
    if (single.length) {
      const g = single[0], row = riskRows.find(r => /^solo/.test(r.className) && r.getAttribute('data-mkt') === g.members[0]);
      const mm = S.market(g.members[0]);
      check(row && P.cell(row, 'Outcome / Market', 's-risk-body').textContent === (mm ? mm.short : g.name) && P.cell(row, 'MaxL', 's-risk-body').textContent === D.fmt.money(Math.max(0, -g.worst)),
        'it carries the market\'s label and the group\'s worst case', row && row._children.map(c => c.textContent));
    }
    if (multi.length) eq(P.cell(grpRows[0], 'MaxL', 's-risk-body').textContent, D.fmt.money(Math.max(0, -multi[0].worst)), 'a group row shows its worst-case loss');
  }
  eq(P.heads('s-risk-body').join('|'), 'Outcome / Market|V|Own|Pos|Avg|Mark|Unrl|IfYES|IfNO|MaxL', 'RISK BOOK columns follow the spec');
  const foot = P.text($('s-risk-foot'));
  check(/^K S0 \d+\.\d\d S1 \d+\.\d\d S2 \d+\.\d\d S3 \d+\.\d\d \| PM \d+\.\d\d$/.test(foot), 'cash footer: K S0..S3 | PM', foot);
  const sum = P.text($('s-risk-sum'));
  check(/^risk \d+\.\d\d open \d+\.\d\d rsv \d+\.\d\d \| loss \d+\.\d\d\/25\.00 \| floor 150\.00 eq \d+\.\d\d$/.test(sum), 'risk summary: risk, open, reserved, loss, floor', sum);
  const rsvShown = +sum.split(' rsv ')[1].split(' ')[0];
  let rsvWant = 0; D.systematic._t.reservations().forEach(u => { rsvWant += u; });
  let rsvNaive = 0; S.orders.forEach(o => { if (!o.terminal && o.left > 0) rsvNaive += o.side === 'BUY' ? o.left * o.px : o.left * (10000 - o.px); });
  near(rsvShown, Math.round(rsvWant / 100) / 100, 0.011, 'the summary shows the reservations of every working order (both owners)');
  check(rsvWant <= rsvNaive && rsvWant > 0, 'reservations never exceed the orders\' full worst case', { rsvWant, rsvNaive });

  if (process.env.V) console.log('  info: sys fills', sysFills.length, 'risk groups', S.risk.groups.length, 'matured', !!matured, 'live sys', liveSys.length, 'plan', plan);
  // ---- linking: a fill row click focuses its market; Shift+click loads the other ladder
  const fr = fillRows()[0];
  P.click(fr._children[3]);
  eq(D.focus.key, fr.getAttribute('data-mkt'), 'clicking a fill row sets the focused market');
  P.flush();
  check(/\bsel\b/.test(fillRows()[0].className), 'the clicked row carries the selection bar');
  check(fillRows().slice(1).filter(r => r.getAttribute('data-mkt') === D.focus.key).every(r => /\bfmk\b/.test(r.className) && !/\bsel\b/.test(r.className)), 'the market\'s other rows a tint, never a second bar');
  const other = D.focus.otherLadder();
  const wr = workRows().find(r => r.getAttribute('data-mkt') !== D.focus.key) || workRows()[0];
  if (wr) {
    P.click(wr._children[3], { shiftKey: true });
    eq(D.focus.ladders[other], wr.getAttribute('data-mkt'), 'Shift+click on a working row loads the other ladder');
  }
  const mem2 = P.rows('s-risk-body').find(r => /^(mem|solo)/.test(r.className));
  if (mem2) { P.click(mem2._children[0]); eq(D.focus.key, mem2.getAttribute('data-mkt'), 'clicking a risk book row sets the focused market'); }
  const moFill = P.rows('s-mo-body').find(r => r.getAttribute('data-mkt'));
  if (moFill) { P.click(moFill._children[0]); eq(D.focus.key, moFill.getAttribute('data-mkt'), 'clicking a recent markout row sets the focused market'); }

  // ---- lane filter: click a lane row, the blotters narrow; again clears
  const busy = ['M1', 'M2', 'S1', 'C1'].find(id => S.fills.some(f => f.own === 'SYS' && f.lane === id)) || 'M1';
  P.click(laneRow(busy)._children[0]);
  P.flush();
  eq(D.systematic.filter, busy, 'clicking a lane row filters to that lane');
  check(laneRow(busy).className.indexOf('sel') >= 0, 'the filtered lane row is selected');
  check(fillRows().length > 0 && fillRows().every(r => S.fillIdx.get(r.dataset.k).lane === busy), 'FILLS narrows to the lane');
  check(workRows().every(r => S.orders.get(r.dataset.k).lane === busy), 'WORKING narrows to the lane');
  check(P.rows('s-mo-body').filter(r => r.getAttribute('data-mkt')).every(r => S.fillIdx.get(r.dataset.k.slice(2)).lane === busy), 'recent markouts narrow to the lane');
  eq(P.text($('s-work-tools')).indexOf(busy + ' x'), 0, 'an amber filter tag shows in the WORKING title');
  const laneKey = D.systematic._t.laneMarket(busy);
  eq(D.focus.key, laneKey, 'a lane row click also focuses the lane\'s newest market');
  P.click(laneRow(busy)._children[0]);
  P.flush();
  eq(D.systematic.filter, null, 'clicking the lane row again clears the filter');
  P.key('2'); P.flush();
  eq(D.systematic.filter, S.laneOrder[1], 'key 2 filters to the second lane');
  P.key('Escape'); P.flush();
  eq(D.systematic.filter, null, 'Esc clears the lane filter');
  P.key('1'); P.flush();
  const tagBtn = $('s-fills-tools')._children.find(e => e.getAttribute('data-act') === 'fclear');
  P.click(tagBtn); P.flush();
  eq(D.systematic.filter, null, 'the filter tag in a title clears the filter');

  // ---- cancel one systematic order with x: CXL-PEND paints at once
  const live1 = workRows().find(r => { const o = S.orders.get(r.dataset.k); return !o.terminal && o.tif === 'POST'; });
  if (process.env.V) console.log('  info: live1', !!live1, 'wr', !!wr, 'mem2', !!mem2, 'moFill', !!moFill, 'busy', busy);
  if (live1) {
    const cid = live1.dataset.k;
    const focusBefore = D.focus.key;
    P.click(live1._children[live1._children.length - 1]);
    P.flush();
    check(S.orders.get(cid).terminal || S.orders.get(cid).local.cxlPend, 'x marks the order cancel-pending at once');
    eq(D.focus.key, focusBefore, 'the x cell does not change the focused market');
    const row = workRows().find(r => r.dataset.k === cid);
    check(!row || row.className.indexOf('cxlpend') >= 0 || S.orders.get(cid).terminal, 'the row paints cxlpend', row && row.className);
    await P.advance(500);
    check(S.orders.get(cid) === undefined || S.orders.get(cid).terminal, 'the venue confirmed the cancel');
  } else check(true, 'no resting systematic order to cancel at this moment (skipped)');

  // ---- lane toggle: optimistic, then confirmed (the strategy worker answers a few ms later)
  P.click(tgOf('M2'));
  P.flush();
  check(tgOf('M2').textContent === 'OFF' && tgOf('M2').className.indexOf('pend') >= 0 && S.lanes.get('M2').on === true,
    'the M2 toggle shows OFF, pending, on the next frame, before the backend answers', tgOf('M2').className);
  await P.advance(500);
  check(tgOf('M2').textContent === 'OFF' && tgOf('M2').className.indexOf('pend') < 0 && S.lanes.get('M2').on === false, 'the backend agreed: M2 off');
  P.click(tgOf('M2'));
  await P.advance(500);
  eq(S.lanes.get('M2').on, true, 'M2 back on');

  // ---- lane kill: off and its working orders cancel-pending in the same frame
  await P.advance(3000);
  const killId = ['M1', 'M2', 'S1', 'C1'].find(id => S.working(o => o.own === 'SYS' && o.lane === id).length) || 'M1';
  const before = S.working(o => o.own === 'SYS' && o.lane === killId).map(o => o.cid);
  P.click(klOf(killId));
  P.flush();
  eq(tgOf(killId).textContent, 'OFF', 'lane KILL turns the lane off at once');
  check(before.every(c => S.orders.get(c).terminal || S.orders.get(c).local.cxlPend), 'lane KILL marks the lane\'s working orders cancel-pending', before.length);
  await P.advance(1000);
  eq(S.working(o => o.own === 'SYS' && o.lane === killId).length, 0, 'the killed lane has no working orders');
  eq(klOf(killId).className, 'kl dis', 'the lane KILL greys once nothing is live');
  P.click(tgOf(killId));
  await P.advance(500);

  // ---- STOP: pending, then STOPPED; Systematic orders cancelled
  P.click(stop);
  P.flush();
  eq(P.text($('s-run')), 'STOPPING', 'STOP paints STOPPING on the next frame');
  await P.advance(1000);
  eq(P.text($('s-run')), 'STOPPED', 'STOPPED after the venue answers');
  eq(S.working(o => o.own === 'SYS').length, 0, 'STOP cancelled Systematic\'s orders');
  check(S.working(o => o.own === 'MAN').length >= 1, 'STOP left the manual order alone');

  // ---- preflight failure disables START with the reason
  P.sim().setFeed('KXP', false);
  await P.advance(1500);
  check(start.disabled, 'START is disabled while a check fails');
  const why = P.text($('s-why'));
  check(/PRIV KXP down/.test(why) && !/FEED/.test(why), 'the failing check and its reason show next to the tags (the private socket is PRIV, not FEED)', why);
  check(tags().find(x => x.startsWith('PRIV')).endsWith(':bad') && tags().find(x => x.startsWith('FEED')).endsWith(':ok'), 'the failing tag turns red', tags());
  P.click(start); P.flush();
  eq(P.text($('s-run')), 'STOPPED', 'a click on the disabled START does nothing');
  P.sim().setFeed('KXP', true);
  await P.advance(1500);
  check(!start.disabled && P.text($('s-why')) === '', 'START comes back when the checks pass');

  // ---- the stale rule: a silent stream (no heartbeat for 3 s) disables START with the reason;
  // a dropped message is detected as a gap and healed from a snapshot
  P.clock.t += 3500;                               // the venues say nothing for 3.5 s
  D.tick.run();
  P.flush();
  check(S.stale && start.disabled && /^STALE SRV DOWN/.test(P.text($('s-why'))), 'a silent stream greys out and disables START with STALE SRV DOWN', { stale: S.staleWhy, why: P.text($('s-why')) });
  eq(P.doc.body.className.indexOf('stale') >= 0, true, 'the body carries the stale class (numbers grey out)');
  eq(stop.disabled, false, 'STOP stays live while stale (fail closed, cancels always go)');
  await P.advance(3000);
  check(!S.stale && !start.disabled, 'the stream heals and START is live again', { stale: S.staleWhy });
  const gaps0 = S.gaps;
  P.sim().dropNext();
  await P.advance(3000);
  check(S.gaps > gaps0 && !S.stale && !start.disabled, 'a dropped message is detected as a gap and healed', { gaps: S.gaps, stale: S.staleWhy });

  // ---- KILL from the header: pending at once, KILLED with the re-arm countdown once confirmed;
  // a KILL with no answer says KILL FAIL, un-strikes the orders and retries by itself
  {
    const post0 = D.transport.post;
    let tries = 0;
    D.transport.post = (p, b) => (p === '/api/kill' ? (tries++, Promise.resolve({ status: 0, json: { ok: false, err: { code: 'EXCHANGE', msg: 'desk unreachable' } } })) : post0(p, b));
    const wk = S.working().map(o => o.cid);
    P.click($('kill'));
    P.flush();
    eq($('kill').textContent, 'KILL ...', 'KILL paints pending at once (not KILLED before the backend says so)');
    check(S.canTrade('K') === 'KILLED', 'new orders are refused from the click on');
    await P.settle(); P.flush();
    check($('kill').textContent === 'KILL FAIL' && /fail/.test($('kill').className) && wk.every(c => !S.orders.get(c) || S.orders.get(c).terminal || !S.orders.get(c).local || !S.orders.get(c).local.cxlPend),
      'no answer: KILL FAIL, and the orders it did not cancel are no longer struck through', $('kill').textContent);
    check(/KILL not confirmed/.test(S.alerts[0].msg) && S.alerts[0].lvl === 'CRIT', 'a CRIT line says so');
    check(S.canTrade('K') === 'KILLED', 'new orders stay refused while KILL retries');
    D.transport.post = post0;
    await new Promise(r => setTimeout(r, 1100));     // the retry runs on its own 1 s timer
    await P.advance(200);
    check(tries === 1 && D.act.killing === null, 'the automatic retry got through', { tries, killing: !!D.act.killing });
  }
  check(/^KILLED re-arm 0:\d\d$/.test(P.text($('s-run'))) || /^KILLED re-arm 1:00$/.test(P.text($('s-run'))), 'KILL shows KILLED re-arm m:ss', P.text($('s-run')));
  check(/^KILLED \d+s$/.test($('kill').textContent), 'the button counts the lockout down in seconds', $('kill').textContent);
  eq(P.text($('tab-sys-st')), 'KILL', 'the tab says KILL');
  eq(start.disabled, true, 'START is disabled during the kill lockout');
  await P.advance(61000, 5000);
  eq(P.text($('s-run')), 'KILLED RE-ARM', 'after 60 s the strip asks for a re-arm');
  P.click($('kill'));                              // RE-ARM: no dialog, the label asks for a second click
  P.flush();
  eq($('kill').textContent, 'CONFIRM', 'the first RE-ARM click turns the button to CONFIRM');
  await P.advance(3500);
  eq($('kill').textContent, 'RE-ARM', 'CONFIRM lapses after 3 s');
  P.click($('kill')); P.flush();
  P.click($('kill')); P.flush();
  eq(P.text($('s-run')), 'KILLED RE-ARM', 'nothing changes before the backend answers');
  await P.advance(200);
  eq(P.text($('s-run')), 'STOPPED', 're-armed: STOPPED');
  check(start.disabled && /GROUPS/.test(P.text($('s-why'))), 'START waits for the order groups to reset', P.text($('s-why')));
  await P.advance(1500);
  eq(start.disabled, false, 'START is live once the groups reset');

  // ---- markets Manual holds (mkt.man) show in the LANES title
  {
    const heldN = () => Array.from(S.markets.values()).filter(m => m.man && m.status === 'OPEN').length;
    const n0 = heldN();
    const lk = Array.from(S.markets.values()).find(m => m.kind === 'LIP15' && m.status === 'OPEN' && !m.man);
    const g = lk.g, bk = S.book(lk.key);
    D.act.order({ v: lk.v, tk: lk.tk, side: 'BUY', px: g.levels[0], qty: 1, tif: 'POST', ttl_s: 60, override: false, touch: bk ? bk.bb : null }, null);
    await P.advance(500);
    check(S.market(lk.key).man === true && P.text($('s-held')) === 'MAN ' + (n0 + 1) && ($('s-held').getAttribute('title') || '').indexOf(lk.short) >= 0, 'a market Manual took over counts in MAN n in the LANES title, named on its title', P.text($('s-held')));
    D.act.cancelAll('MAN', null);
    D.act.release(lk.v, lk.tk);
    await P.advance(500);
    eq(P.text($('s-held')), n0 ? 'MAN ' + n0 : '', 'released: it leaves the count');
  }

  // ---- WIDE: more columns; narrow: fewer
  P.setWidth(1920);
  P.flush();
  check(D.layout.wide && D.systematic._t.mode() === 'w', 'WIDE mode at 1920 px');
  const LW = P.heads('s-lanes-body');
  ['V', 'Style', 'Open', 'Unrl', 'Day', 'Last', 'Thr\u00a2'].forEach(h => check(LW.indexOf(h) >= 0, 'WIDE adds the lane column ' + h, LW));
  check(LW[LW.length - 1] === '' && LW[LW.length - 2] === '' && P.heads('s-work-body')[P.heads('s-work-body').length - 1] === '', 'WIDE caps Market and Reason: a blank filler column takes the slack', LW.slice(-3));
  check(P.heads('s-work-body').indexOf('Touch') >= 0 && P.heads('s-work-body').indexOf('Ack') >= 0, 'WIDE adds Touch and Ack to WORKING');
  check(P.heads('s-fills-body').indexOf('Mid') >= 0 && P.heads('s-fills-body').indexOf('Settle\u00a2') >= 0, 'WIDE adds Mid and Settle to FILLS');
  check(P.heads('s-risk-body').indexOf('Rsv') >= 0, 'WIDE adds reservations to the RISK BOOK');
  check(P.heads('s-mo-body').indexOf('Net60') >= 0, 'WIDE adds Net60 to MARKOUTS');
  check(/S0 \d+\.\d\d\/\d+\.\d\d/.test(P.text($('s-risk-foot'))), 'WIDE shows free cash per shard in the footer', P.text($('s-risk-foot')));
  eq(P.rows('s-lanes-body').length, 6, 'the lanes grid keeps its rows across a layout change');
  // 2400 px and wider (not SPLIT): an always-open LOG beside the blotters
  P.setWidth(2560);
  P.flush();
  check(P.rows('s-log-body').length > 0 && P.heads('s-log-body').join('|') === 'Time|Lvl|Source|Message', 'from 2400 px SYSTEMATIC shows the LOG', P.rows('s-log-body').length);
  P.setWidth(1280);
  P.flush();
  const LN = P.heads('s-lanes-body');
  check(D.layout.narrow && LN.indexOf('Ctrs') < 0 && LN.indexOf('Fees') >= 0 && LN.indexOf('LIP') >= 0 && LN.indexOf('P&L') >= 0, 'narrow keeps budget, fees, P&L and LIP and drops the rest', LN);
  check(P.heads('s-work-body').indexOf('PIQ') < 0 && P.heads('s-fills-body').indexOf('Fee') < 0, 'narrow drops PIQ and Fee');
  P.setWidth(1512);
  P.flush();
  eq(P.heads('s-lanes-body').join('|'), LH.join('|'), 'back to the 1512 px column set');

  // ---- Polymarket key failing: P rows read '?' in the risk book, RO in the footer and plan
  const pmk = [...S.markets.values()].find(m => m.v === 'P' && m.status === 'OPEN' && S.book(m.key) && S.book(m.key).ba !== null);
  if (pmk) {
    D.act.order({ v: 'P', tk: pmk.tk, side: 'BUY', px: D.grid.step(pmk.g, S.book(pmk.key).ba, 1), qty: 2, tif: 'IOC', touch: S.book(pmk.key).ba, override: false }, { timeStamp: performance.now() });
    await P.advance(1500);
    P.sim().state.pmRO = true;
    await P.advance(2500);
    const prow = P.rows('s-risk-body').find(r => r.getAttribute('data-v') === 'P');
    if (process.env.V) console.log('  info: pm position', S.position(pmk.key) && S.position(pmk.key).pos, 'prow', !!prow);
    check(S.hdr.cash.P.ro, 'the venue reports the PM key failing');
    if (prow) eq(P.cell(prow, 'Pos', 's-risk-body').textContent, '?', 'a Polymarket row reads ? while the key fails');
    else check(S.position(pmk.key) === null || !S.position(pmk.key).pos, 'no PM position to show (IOC did not fill)');
    check(/\| PM \d+\.\d\d RO$/.test(P.text($('s-risk-foot'))), 'the cash footer tags PM RO', P.text($('s-risk-foot')));
    check(/ RO \|/.test(P.text($('s-plan'))), 'the plan line tags P RO', P.text($('s-plan')));
  }

  // ---- the read-only settings drawer: p (or SET in the status bar)
  P.key('p'); P.flush();
  check(P.doc.body.className.indexOf('set') >= 0 && P.rows('set-lanes').length === 5 && P.rows('set-fees').length > 0 && P.rows('set-limits').length > 5,
    'p opens the settings drawer: lanes, fees, limits', [P.rows('set-lanes').length, P.rows('set-fees').length, P.rows('set-limits').length]);
  check(P.rows('set-lanes').find(r => r.dataset.k === 'S1')._children[5].textContent === '2.0', 'it shows a lane\'s threshold in cents');
  P.key('Escape'); P.flush();
  check(P.doc.body.className.indexOf('set') < 0, 'Esc closes it');

  // ---- instant controls: pressed at pointerdown; STOP, cancels and the lane filter act there,
  // START and the lane toggle keep the click
  {
    const pd = el => fire(P.doc, el, 'pointerdown', { button: 0, pointerType: 'mouse' });
    const pu = el => fire(P.doc, el, 'pointerup', { button: 0 });
    D.tabs.set('systematic'); P.flush();
    pd(start); P.flush();
    check(start.hasAttribute('data-prs') && P.text($('s-run')) === 'STOPPED', 'START shows pressed at pointerdown and does not start yet', [start.className, P.text($('s-run'))]);
    // 2026-10-06 review: the strip's next paint rewrote START's className and dropped the press
    D.render.mark('hdr'); D.render.mark('clock'); D.render.mark('sys'); P.flush(); P.flush();
    check(start.hasAttribute('data-prs'), 'START stays pressed through the frames that repaint the strip, until pointerup', start.className);
    pu(start);
    check(!start.hasAttribute('data-prs'), 'pointerup clears it');
    P.click(start); P.flush();
    eq(P.text($('s-run')), 'STARTING', 'START acts on the click');
    await P.advance(1500);
    const cell0 = laneRow('M1')._children[0];
    pd(cell0); P.flush();
    eq(D.systematic.filter, 'M1', 'a lane row filters at pointerdown');
    P.click(cell0); P.flush();
    eq(D.systematic.filter, 'M1', 'the click that follows does not toggle it back');
    const tg = tgOf('M2'), on0 = D.store.lanes.get('M2').on;
    pd(tg); P.flush();
    check(D.store.lanes.get('M2').pendOn === null && tgOf('M2').textContent === (on0 ? 'ON' : 'OFF'), 'the lane toggle does nothing at pointerdown');
    D.render.mark('lane'); D.render.mark('hdr'); D.render.mark('ord'); D.render.mark('clock'); P.flush(); P.flush();
    check(tgOf('M2') === tg && tg.hasAttribute('data-prs'), 'the lane ON cell stays pressed while the lanes grid repaints', tg.className);
    pu(tg);
    pd(stop); P.flush();
    eq(P.text($('s-run')), 'STOPPING', 'STOP acts at pointerdown (it only takes risk off)');
    P.click(stop);
    await P.advance(1500);
    eq(P.text($('s-run')), 'STOPPED', 'stopped once');
    // the dev check over the tab's controls
    D.perf.set(true);
    const ctl = [P.$('tab-man'), P.$('tab-sys'), laneRow('M2')._children[0], laneRow('M2')._children[0], P.$('s-cxlall'), P.$('sb-set'), P.$('set-close')];
    for (const c of ctl) { pd(c); pu(c); P.click(c); P.flush(); }
    D.systematic.setFilter(null); P.flush();
    const rep = D.perf.report();
    check(rep.some(r => r.ctl === '#tab-sys') && rep.some(r => /s-lanes:/.test(r.ctl)), 'the dev check times the tab\'s controls', rep.map(r => r.ctl));
    const over = rep.filter(r => r.h > 16.7 || r.p > 16.7);
    eq(over.map(r => r.ctl + ' ' + r.h + '/' + r.p).join(', '), '', 'every control inside one frame here (handler and the frame it caused)');
    D.perf.set(false);
  }

  // ---- FLASH SUBTLE on a busy Systematic tab (2026-10-06 browser pass: ~100 cells a second
  // flashed with the lanes quoting): only fills, exits, rejects and realized P&L flash; the
  // quoting churn (sent / working / cancelled), unrealized P&L and the risk book's marks only
  // with FULL
  {
    const proto = Object.getPrototypeOf(P.$('kill')), anim0 = proto.animate, flush0 = D.render.flush;
    const tapeOf0 = tr => { const k = tr && tr.dataset.k; const e = k && D.activity.lists.SYS.find(x => 'a' + x.id === k); return e ? e.type : null; };
    // where a flashed cell sits, read right after the frame that flashed it (a new row's cells
    // flash before the row is inserted; a tape row may scroll out of the window later)
    const where = el => { const tr = el.parentNode; let host = tr && tr.parentNode; while (host && !/-body$/.test(host.id)) host = host.parentNode; const hid = host ? host.id : '';
      return { tr, host: hid, col: tr ? tr._children.indexOf(el) : -1, ev: hid === 's-act-body' ? tapeOf0(tr) : null }; };
    let pend = [], fl = [];
    proto.animate = function () { pend.push(this); return {}; };
    D.render.flush = function () { flush0(); while (pend.length) fl.push(where(pend.shift())); };
    D.tabs.set('systematic'); D.ui.setFlash('subtle');
    D.act.start(); await P.advance(5000, 250);
    const id0 = D.activity.n, f0n = S.fills.length;
    fl = [];
    await P.advance(30000, 250);
    const got = fl, newEv = D.activity.lists.SYS.filter(e => e.id > id0);
    const tapeOf = w => w.ev;
    const tapeTypes = got.filter(w => w.host === 's-act-body').map(tapeOf);
    const churn = newEv.filter(e => e.type === 'SUB' || e.type === 'CXL').length;
    check(churn > 20 && !tapeTypes.some(t => t === 'SUB' || t === 'CXL' || t === 'EXP'), 'SUBTLE: the tape\'s quoting churn (sent, cancelled) never flashes', { churn, flashed: [...new Set(tapeTypes)] });
    const newFills = newEv.filter(e => e.type === 'FILL' || e.type === 'PART').length;
    check(!newFills || tapeTypes.some(t => t === 'FILL' || t === 'PART'), 'a fill on the tape does', { newFills });
    const WHs = P.heads('s-work-body'), stCol = WHs.indexOf('St');
    const workSt = got.filter(w => w.host === 's-work-body' && w.col === stCol).map(w => S.orders.get(w.tr.dataset.k)).filter(Boolean);
    check(workSt.every(o => o.filled > 0 || o.st === 'REJ'), 'WORKING flashes a lane order only when it fills or is rejected', workSt.map(o => o.st + '/' + o.filled).slice(0, 5));
    const unrlCol = P.heads('s-risk-body').indexOf('Unrl');
    eq(got.filter(w => w.host === 's-risk-body' && w.col === unrlCol).length, 0, 'the risk book\'s unrealized P&L (marks) does not flash');
    const LHs = P.heads('s-lanes-body'), pnlCols = ['P&L', 'Day'].map(h => LHs.indexOf(h)).filter(i => i >= 0);
    const laneFl = got.filter(w => w.host === 's-lanes-body' && pnlCols.indexOf(w.col) >= 0).length, sysNews = newEv.filter(e => e.type !== 'SUB' && e.type !== 'CXL' && e.type !== 'EXP').length;
    check(laneFl <= 2 * sysNews + 4, 'a lane\'s P&L and Day flash when its realized part moves (fills, exits), not on every mark', { laneFl, sysNews });
    const perSec = got.length / 30;
    check(perSec < 15, 'SUBTLE stays calm: under 15 cells a second with the lanes trading', { perSec: Math.round(perSec * 10) / 10, fills: S.fills.length - f0n });
    D.ui.setFlash('full'); fl = [];
    await P.advance(10000, 250);
    const got2 = fl;
    check(got2.filter(w => w.host === 's-act-body').map(tapeOf).some(t => t === 'SUB' || t === 'CXL'), 'FULL: the tape\'s new rows all flash');
    check(got2.some(w => w.host === 's-work-body' && w.col === stCol && (S.orders.get(w.tr.dataset.k) || { filled: 1 }).filled === 0), 'FULL: every lane order state change flashes');
    check(got2.length > got.length / 3, 'FULL flashes far more than SUBTLE', { full10s: got2.length, subtle30s: got.length });
    D.ui.setFlash('subtle');
    D.act.stop(); await P.advance(1500);
    proto.animate = anim0; D.render.flush = flush0;
  }

  // ---- switching tabs (2026-10-06 review: SYSTEMATIC with the lanes running took 12-19 ms of
  // script, style and layout before its paint, every panel redrawn in full): the hidden tab keeps
  // what it missed and catches up about once a second in a task of its own, with no flashes; the
  // frame that shows it draws only the rest
  {
    const proto = Object.getPrototypeOf(P.$('kill')), anim0 = proto.animate;
    let bgFlashes = 0;
    // a flash counts only once SYSTEMATIC is hidden: the lanes' cells flash legitimately while it shows
    proto.animate = function () { let e = this; while (e && e.id !== 'tab-systematic') e = e.parentNode; if (e && D.tabs.cur !== 'systematic') bgFlashes++; return {}; };
    D.tabs.set('systematic'); P.flush();
    D.act.start(); await P.advance(2000, 250);
    D.tabs.set('manual'); P.flush();
    const runs0 = D.render.bgRuns;
    await P.advance(6000, 250);
    check(D.render.bgRuns - runs0 >= 4, 'while MANUAL shows, the SYSTEMATIC panels catch up about once a second', D.render.bgRuns - runs0);
    eq(bgFlashes, 0, 'and flash nothing nobody can see');
    const lr = laneRow('M1'), fillsCell = P.cell(lr, 'Fills', 's-lanes-body');
    D.render.bg();
    eq(fillsCell.textContent, String(S.lanes.get('M1').fills), 'the hidden lanes grid is current (M1 fills)');
    const sysR = D.render.renderers.filter(r => r.tab === 'systematic'), seen = [];
    const fns = sysR.map(r => r.fn);
    sysR.forEach((r, i) => { r.fn = d => { seen.push(r.id + ':' + (d.all ? 'ALL' : 'part')); return fns[i](d); }; });
    D.tabs.set('systematic'); P.flush();
    check(seen.length > 0 && seen.every(x => /:part$/.test(x)), 'the switch frame draws what changed since the last catch-up, never every panel in full', seen);
    sysR.forEach((r, i) => { r.fn = fns[i]; });
    D.act.stop(); await P.advance(1500);
    proto.animate = anim0;
  }

  // ---- the page stayed healthy
  eq(D.render.errors, 0, 'no render errors' + (D.render.lastError ? ' (' + D.render.lastError + ')' : ''));
  eq(S.invalid, 0, 'the store saw no contract violations');
  eq(P.errors.length, 0, 'nothing logged to console.error' + (P.errors.length ? ': ' + P.errors[0].slice(0, 200) : ''));
  eq(P.doc.innerHTMLWrites, 0, 'still no innerHTML writes');
  const sheet = D.keys.sheetRows().filter(r => r.scope === 'systematic').map(r => r.key);
  check(sheet.indexOf('Escape') >= 0 && sheet.indexOf('1-9') >= 0, 'the key sheet lists the tab\'s keys', sheet);

  // a remembered filter for a lane the backend no longer has is dropped
  {
    const P2 = bootPage({ search: '?seed=5', storage: { 'desk3.sys.lane': 'ZZ9', 'desk3.tab': 'systematic' } });
    await P2.settle(); P2.sim().stop();
    await P2.advance(1500);
    P2.D.tabs.set('systematic'); P2.flush();
    check(P2.D.systematic.filter === null || P2.D.systematic.filter === undefined, 'an unknown remembered lane filter is cleared');
    eq(P2.D.render.errors, 0, 'a second boot renders without errors');
  }

  console.log(fails ? `${fails} of ${checks} checks FAILED` : `ui desk v3 systematic: ${checks} checks, 0 failures`);
  process.exit(fails ? 1 : 0);
})().catch(e => { console.log('FAIL: test crashed: ' + (e && e.stack || e)); process.exit(1); });
