/* ui/desk/js/core.js
 *
 * The page's shared machinery. Classic script; everything hangs off window.Desk.
 *
 *   Desk.storage    try/catch-wrapped localStorage (falls back to memory; never throws)
 *   Desk.num        contract strings <-> integers. INSIDE THE PAGE every price and every dollar
 *                   amount is an integer in u = $0.0001 ("0.6400" -> 6400, "$142.31" -> 1423100).
 *   Desk.grid       price grids (Kalshi price_ranges): levels, snapping, stepping by ticks
 *   Desk.fees       Kalshi and Polymarket US fee formulas, with the venues' rounding
 *   Desk.fmt        display strings: prices, sizes, money, P&L with U+2212, cents, times
 *   Desk.dom        setText/setHTML/setClass/setAttr, keyedRows (keyed row diffing), flash
 *   Desk.render     the requestAnimationFrame scheduler: mark(topic) -> one frame -> renderers
 *   Desk.tick       one 250 ms clock for countdowns and the stale check (no other timers)
 *   Desk.focus      the global focused market, the ladder slots, and panel linking ([data-mkt])
 *   Desk.keys       key bindings with scopes, the typing guard, HOTKEYS ON/OFF, the ? key sheet
 *   Desk.lat        click-to-paint and click-to-ack meters
 *   Desk.layout     wide (>= 1920 px) / narrow (< 1400 px) flags
 *   Desk.ui         density and colour-blind toggles
 *
 * Nothing here touches the DOM at load, so node can load it into a vm for the tests.
 */
(function (root) {
  'use strict';
  var Desk = root.Desk = root.Desk || {};
  var hasDoc = typeof document !== 'undefined';
  var perf = (typeof performance !== 'undefined' && performance.now) ? performance : { now: function () { return Date.now(); } };
  var MINUS = '\u2212';
  Desk.MINUS = MINUS;
  Desk.now = function () { return Date.now(); };
  Desk.pnow = function () { return perf.now(); };

  // ---------------------------------------------------------------------------------------------
  // Storage. Browser storage is a convenience, never a dependency: it throws in a private window
  // or with site data blocked, and an unhandled SecurityError would blank the screen. Kept from
  // ui/index.html (the safe wrapper), with JSON helpers. Keys are prefixed 'desk3.'.
  // ---------------------------------------------------------------------------------------------
  var mem = Object.create(null);
  Desk.storage = {
    get: function (k) {
      k = 'desk3.' + k;
      try { var v = root.localStorage.getItem(k); if (v !== null) return v; } catch (e) { /* blocked */ }
      return k in mem ? mem[k] : null;
    },
    set: function (k, v) {
      k = 'desk3.' + k; v = String(v); mem[k] = v;
      try { root.localStorage.setItem(k, v); } catch (e) { /* blocked or full */ }
    },
    getJSON: function (k, dflt) {
      var s = this.get(k);
      if (s === null) return dflt;
      try { return JSON.parse(s); } catch (e) { return dflt; }
    },
    setJSON: function (k, v) { try { this.set(k, JSON.stringify(v)); } catch (e) { /* cyclic */ } }
  };

  // ---------------------------------------------------------------------------------------------
  // Numbers. u = $0.0001. Integers end float drift: a ladder keyed by 0.1 + 0.2 never matches.
  // ---------------------------------------------------------------------------------------------
  var U = 10000;
  function parseU(s) {
    if (s === null || s === undefined || s === '') return null;
    if (typeof s === 'number') return Math.round(s * U);
    var neg = s.charAt(0) === '-';
    var t = neg ? s.slice(1) : s;
    var dot = t.indexOf('.');
    var whole = dot < 0 ? t : t.slice(0, dot);
    var frac = dot < 0 ? '' : t.slice(dot + 1);
    frac = (frac + '0000').slice(0, 4);
    var v = parseInt(whole || '0', 10) * U + parseInt(frac, 10);
    if (isNaN(v)) return null;
    return neg ? -v : v;
  }
  function strU(u) {
    if (u === null || u === undefined || isNaN(u)) return null;
    var r = u < 0 ? -Math.round(-u) : Math.round(u);      // half away from zero
    if (r === 0) return '0.0000';
    var neg = r < 0; if (neg) r = -r;
    var w = Math.floor(r / U), f = String(r % U);
    while (f.length < 4) f = '0' + f;
    return (neg ? '-' : '') + w + '.' + f;
  }
  Desk.num = {
    U: U,
    px: parseU,                 // "0.6400" -> 6400
    pxStr: strU,                // 6400 -> "0.6400"
    money: parseU,              // "-3.2100" -> -32100
    moneyStr: strU,             // -32100 -> "-3.2100"
    r2: function (x) { return Math.round(x * 100) / 100; },   // sizes: 2 decimals
    clamp: function (x, lo, hi) { return x < lo ? lo : (x > hi ? hi : x); }
  };

  // ---------------------------------------------------------------------------------------------
  // Grids. A market's valid prices come from its price_ranges ({start,end,step} in dollars).
  // Desk.grid.make(mkt.grid) is cached per market by the store (market.g).
  // ---------------------------------------------------------------------------------------------
  function makeGrid(ranges) {
    var rs = (ranges || []).map(function (r) {
      return { s: typeof r.start === 'number' ? r.start : parseU(r.start),
               e: typeof r.end === 'number' ? r.end : parseU(r.end),
               st: typeof r.step === 'number' ? r.step : parseU(r.step) };
    });
    if (!rs.length) rs = [{ s: 0, e: U, st: 100 }];
    var seen = Object.create(null), lv = [];
    rs.forEach(function (r) {
      for (var p = r.s; p <= r.e; p += r.st) if (p > 0 && p < U && !seen[p]) { seen[p] = 1; lv.push(p); }
    });
    lv.sort(function (a, b) { return a - b; });
    var minStep = Math.min.apply(null, rs.map(function (r) { return r.st; }));
    return { ranges: rs, levels: lv, minStep: minStep, dp: minStep % 100 === 0 ? 2 : (minStep % 10 === 0 ? 3 : 4) };
  }
  // index of the highest level <= u (or -1)
  function floorIdx(g, u) {
    var lv = g.levels, lo = 0, hi = lv.length - 1, ans = -1;
    while (lo <= hi) { var mid = (lo + hi) >> 1; if (lv[mid] <= u) { ans = mid; lo = mid + 1; } else hi = mid - 1; }
    return ans;
  }
  Desk.grid = {
    make: makeGrid,
    floorIdx: floorIdx,
    onGrid: function (g, u) { var i = floorIdx(g, u); return i >= 0 && g.levels[i] === u; },
    // dir -1: highest level <= u; +1: lowest level >= u; 0: nearest
    snap: function (g, u, dir) {
      var lv = g.levels, i = floorIdx(g, u);
      if (i >= 0 && lv[i] === u) return u;
      if (dir < 0) return i >= 0 ? lv[i] : null;
      if (dir > 0) return i + 1 < lv.length ? lv[i + 1] : null;
      var a = i >= 0 ? lv[i] : null, b = i + 1 < lv.length ? lv[i + 1] : null;
      if (a === null) return b; if (b === null) return a;
      return (u - a) <= (b - u) ? a : b;
    },
    // n ticks up (n > 0) or down (n < 0) from a level; clamps at the grid's ends
    step: function (g, u, n) {
      var i = floorIdx(g, u);
      if (i < 0) i = n > 0 ? -1 : 0;
      else if (g.levels[i] !== u && n < 0) i += 1;      // off-grid: floor is already one tick down
      var j = Math.max(0, Math.min(g.levels.length - 1, i + n));
      return g.levels[j];
    },
    // ticks between two levels (b - a), signed
    ticks: function (g, a, b) { return floorIdx(g, b) - floorIdx(g, a); },
    tickAt: function (g, u) {
      for (var k = 0; k < g.ranges.length; k++) { var r = g.ranges[k]; if (u > r.s && u <= r.e) return r.st; }
      return g.minStep;
    }
  };

  // ---------------------------------------------------------------------------------------------
  // Fees. All results in u (float allowed; the money string rounds to 4 dp).
  //  Kalshi:  model fee = rate * mult * C * p * (1-p) dollars; taker rate 0.07; maker rate 0.0175
  //           on QUAD_MAKER series, 0 on QUAD, everything 0 on FREE (the LIP 15-minute series).
  //           Rounding per Kalshi's "Fee Rounding" page for a non-direct (FCM) member: the trade
  //           fee is rounded up to $0.000001, the balance change is floored to the cent, the
  //           difference is a rounding fee kept in a per-order accumulator and rebated in whole
  //           cents, never making a fill's net fee negative. One fill = ceil to the cent.
  //  Polymarket US: taker 0.0695 * C * p * (1-p), maker rebate 0.0125 * C * p * (1-p); each
  //           rounded to the cent half-to-even; an order's taker fills never total more than the
  //           rounded cumulative exact fee. Rebates are per fill.
  // ---------------------------------------------------------------------------------------------
  var MU = 1e6; // micro-dollars
  function roundHE(x) {
    var f = Math.floor(x), d = x - f;
    if (Math.abs(d - 0.5) < 1e-9) return (f % 2 === 0) ? f : f + 1;
    return Math.round(x);
  }
  function kRate(liq, fee) {
    if (!fee || fee.type === 'FREE') return 0;
    if (liq === 'T') return 0.07 * (fee.mult || 1);
    return fee.type === 'QUAD_MAKER' ? 0.0175 * (fee.mult || 1) : 0;
  }
  Desk.fees = {
    roundHE: roundHE,
    // exact model fee in dollars (positive charge, negative rebate)
    exact: function (v, fee, pxU, qty, liq) {
      var p = pxU / U, q = qty * p * (1 - p);
      if (v === 'P') return liq === 'T' ? 0.0695 * q : -0.0125 * q;
      return kRate(liq, fee) * q;
    },
    newAcc: function () { return { acc: 0, charged: 0, cumExact: 0 }; },
    // one Kalshi fill. revenueU: signed cash flow of the fill before fees (buyer negative).
    kalshiFill: function (acc, revenueU, pxU, qty, liq, fee) {
      var model = kRate(liq, fee) * qty * (pxU / U) * (1 - pxU / U) * MU;
      var tradeFee = Math.ceil(model - 1e-7);
      var rev = Math.round(revenueU * 100);              // u -> micro-dollars
      var change = rev - tradeFee;
      var aligned = Math.floor(change / 10000 + 1e-9) * 10000;
      var rounding = change - aligned;
      acc.acc += rounding;
      var rebate = Math.floor(acc.acc / 10000 + 1e-9) * 10000;
      if (rebate > tradeFee + rounding) rebate = Math.floor((tradeFee + rounding) / 10000) * 10000;
      acc.acc -= rebate;
      var net = tradeFee + rounding - rebate;
      acc.charged += net;
      return net / 100;                                  // micro-dollars -> u
    },
    // one Polymarket US fill
    pmFill: function (acc, pxU, qty, liq) {
      var p = pxU / U, q = qty * p * (1 - p);
      if (liq !== 'T') return -roundHE(0.0125 * q * 100) * 100;      // cents -> u
      var exact = 0.0695 * q * 100;                                   // cents
      acc.cumExact += exact;
      var f = roundHE(exact);
      var cap = roundHE(acc.cumExact) - acc.charged;
      if (f > cap) f = cap;
      if (f < 0) f = 0;
      acc.charged += f;
      return f * 100;
    },
    // fee of one fresh fill (ticket line, lane edge maths): u
    estimate: function (v, fee, side, pxU, qty, liq) {
      var a = this.newAcc();
      if (v === 'P') return this.pmFill(a, pxU, qty, liq);
      var rev = side === 'SELL' ? -(U - pxU) * qty : -pxU * qty;
      return this.kalshiFill(a, rev, pxU, qty, liq, fee);
    },
    // exact taker fee per contract at a price (u, float): the break-even band half-width
    takerPer: function (v, fee, pxU) { return this.exact(v, fee, pxU, 1, 'T') * U; }
  };

  // ---------------------------------------------------------------------------------------------
  // Formatting. No $ inside cells; units go in column headers. Negatives use U+2212.
  // ---------------------------------------------------------------------------------------------
  function pad2(n) { return n < 10 ? '0' + n : '' + n; }
  function fixed(x, dp) {
    var s = Math.abs(x).toFixed(dp);
    if (+s === 0) return (0).toFixed(dp);
    return (x < 0 ? MINUS : '') + s;
  }
  var fmt = Desk.fmt = {
    MINUS: MINUS,
    // price u -> '0.64' (dp: 2 for whole cents, 3 for 0.1c, 4 finer; or forced)
    px: function (u, dp) {
      if (u === null || u === undefined || isNaN(u)) return '';
      if (!dp) dp = (u % 100 === 0) ? 2 : (u % 10 === 0 ? 3 : 4);
      return (u / U).toFixed(dp);
    },
    // price on a market's grid: always that grid's decimals so a ladder column lines up
    pxg: function (u, g) { return fmt.px(u, g ? g.dp : 2); },
    money: function (u, dp) { if (u === null || u === undefined || isNaN(u)) return ''; return fixed(u / U, dp === undefined ? 2 : dp); },
    pnl: function (u, dp) {
      if (u === null || u === undefined || isNaN(u)) return '';
      var s = fixed(u / U, dp === undefined ? 2 : dp);
      return (u > 0 && +Math.abs(u / U).toFixed(dp === undefined ? 2 : dp) !== 0) ? '+' + s : s;
    },
    // u per contract -> signed cents: 120 -> '+1.2'
    cents: function (u, dp) {
      if (u === null || u === undefined || isNaN(u)) return '';
      dp = dp === undefined ? 1 : dp;
      var s = fixed(u / 100, dp);
      return (u > 0 && +Math.abs(u / 100).toFixed(dp) !== 0) ? '+' + s : s;
    },
    size: function (n) {
      if (n === null || n === undefined || isNaN(n)) return '';
      var r = Math.round(n * 100) / 100;
      var s = Math.abs(r) % 1 === 0 ? String(Math.abs(r)) : String(Math.abs(r).toFixed(2)).replace(/0$/, '');
      return (r < 0 ? MINUS : '') + s;
    },
    ssize: function (n) {
      if (n === null || n === undefined || isNaN(n)) return '';
      var s = fmt.size(n);
      return n > 0 ? '+' + s : s;
    },
    int: function (n) { return (n === null || n === undefined || isNaN(n)) ? '' : (n < 0 ? MINUS + Math.abs(Math.round(n)) : String(Math.round(n))); },
    pct: function (x) { return (x === null || x === undefined || isNaN(x)) ? '' : Math.round(x * 100) + '%'; },
    ms: function (n) { return (n === null || n === undefined || isNaN(n)) ? '' : Math.round(n) + 'ms'; },
    time: function (ms) { if (!ms) return ''; var d = new Date(ms); return pad2(d.getHours()) + ':' + pad2(d.getMinutes()) + ':' + pad2(d.getSeconds()); },
    timeT: function (ms) { if (!ms) return ''; var d = new Date(ms); return fmt.time(ms) + '.' + Math.floor(d.getMilliseconds() / 100); },
    hm: function (ms) { if (!ms) return ''; var d = new Date(ms); return pad2(d.getHours()) + ':' + pad2(d.getMinutes()); },
    // duration: '2h14m', '14m03s', '12s'
    dur: function (ms) {
      if (ms === null || ms === undefined || isNaN(ms)) return '';
      var neg = ms < 0; ms = Math.abs(ms);
      var s = Math.floor(ms / 1000), h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), r = s % 60;
      var out = h ? h + 'h' + pad2(m) + 'm' : (m ? m + 'm' + pad2(r) + 's' : r + 's');
      return (neg ? MINUS : '') + out;
    },
    // countdown seconds, rounded up, never negative: '12s'
    secs: function (ms) { if (ms === null || ms === undefined) return ''; return Math.max(0, Math.ceil(ms / 1000)) + 's'; },
    mmss: function (ms) { var s = Math.max(0, Math.ceil(ms / 1000)); return Math.floor(s / 60) + ':' + pad2(s % 60); },
    // age of a value: '−41s'
    age: function (ms) { if (ms === null || ms === undefined) return ''; return MINUS + Math.max(0, Math.round(ms / 1000)) + 's'; },
    // time to a close: '14m', '3h05m', '2d'
    left: function (ms) {
      if (ms === null || ms === undefined) return '';
      if (ms <= 0) return '0s';
      var m = Math.floor(ms / 60000);
      if (m < 1) return Math.ceil(ms / 1000) + 's';
      if (m < 60) return m + 'm';
      if (m < 48 * 60) return Math.floor(m / 60) + 'h' + pad2(m % 60) + 'm';
      return Math.floor(m / 1440) + 'd';
    },
    sign: function (x) { return x > 0 ? 'up' : (x < 0 ? 'dn' : ''); }
  };

  // ---------------------------------------------------------------------------------------------
  // DOM helpers. Never write a value that has not changed; reuse rows instead of recreating them.
  // ---------------------------------------------------------------------------------------------
  var reduceMotion = false;
  if (hasDoc && root.matchMedia) {
    try {
      var rm = root.matchMedia('(prefers-reduced-motion: reduce)');
      reduceMotion = rm.matches;
      if (rm.addEventListener) rm.addEventListener('change', function (e) { reduceMotion = e.matches; });
    } catch (e) { /* old engine */ }
  }
  var FLASH = { up: 'rgba(63,185,80,.22)', dn: 'rgba(248,81,73,.22)', sz: 'rgba(230,232,235,.10)', warn: 'rgba(227,160,8,.35)' };
  function flash(el, kind) {
    if (!el || reduceMotion) return;
    var c = FLASH[kind] || FLASH.sz;
    if (el.animate) {
      // an inset shadow over the cell, not its background: the bid/ask tint and depth bar stay put
      try { el.animate([{ boxShadow: 'inset 0 0 0 64px ' + c }, { boxShadow: 'inset 0 0 0 64px transparent' }], { duration: kind === 'warn' ? 600 : 300, easing: 'linear' }); return; } catch (e) { /* fallthrough */ }
    }
    el.classList.remove('fl-' + kind); void el.offsetWidth; el.classList.add('fl-' + kind);
  }
  function numOf(s) { if (!s) return NaN; return parseFloat(String(s).replace(MINUS, '-').replace(/[^0-9.\-]/g, '')); }
  function setText(el, v) { if (!el) return false; v = (v === null || v === undefined) ? '' : String(v); if (el.textContent !== v) { el.textContent = v; return true; } return false; }
  function setHTML(el, v) { if (el && el.innerHTML !== v) el.innerHTML = v; }
  function setClass(el, name, on) { if (el && el.classList.contains(name) !== !!on) el.classList.toggle(name, !!on); }
  function setAttr(el, k, v) {
    if (!el) return;
    if (v === null || v === undefined || v === false) { if (el.hasAttribute(k)) el.removeAttribute(k); return; }
    v = String(v); if (el.getAttribute(k) !== v) el.setAttribute(k, v);
  }
  function setStyleVar(el, k, v) { if (el && el.style.getPropertyValue(k) !== String(v)) el.style.setProperty(k, String(v)); }

  /* keyedRows(container, items, keyFn, cellsFn, rowFn, opts)
   *   container  a <tbody> (rows are <tr>/<td>) or any element (opts.row/opts.cell tag names)
   *   items      array, in display order
   *   keyFn      item -> unique key (string/number); kept in row.dataset.k
   *   cellsFn    item -> array of cells; a cell is a string/number, or an object
   *              {t: text, cls: 'n up', fl: 'd'|'s', title: '...', attr: {k: v}, html: '...'}
   *              fl 'd' flashes green/red by the direction of a numeric change, 's' flashes grey
   *              on any change. No flash on a row's first paint, nor when a cell empties.
   *   rowFn      item -> class string, or {cls, attr} (attr e.g. {'data-mkt': 'K|TK'})
   * Returns the number of rows. Rows that stay keep their DOM nodes (focus, selection, scroll and
   * click targets survive); only changed text is written.
   */
  function keyedRows(tbody, items, keyFn, cellsFn, rowFn, opts) {
    if (!tbody) return 0;
    var rowTag = (opts && opts.row) || 'tr', cellTag = (opts && opts.cell) || 'td';
    var existing = new Map();
    var ch = tbody.children;
    for (var i = 0; i < ch.length; i++) if (ch[i].dataset && ch[i].dataset.k !== undefined) existing.set(ch[i].dataset.k, ch[i]);
    var prev = null;
    for (var n = 0; n < items.length; n++) {
      var item = items[n];
      var key = String(keyFn(item));
      var cells = cellsFn(item);
      var tr = existing.get(key);
      var old = !!tr;
      if (tr) existing.delete(key);
      else { tr = document.createElement(rowTag); tr.dataset.k = key; }
      while (tr.children.length < cells.length) tr.appendChild(document.createElement(cellTag));
      while (tr.children.length > cells.length) tr.removeChild(tr.lastChild);
      for (var c = 0; c < cells.length; c++) {
        var cell = cells[c], td = tr.children[c];
        var isObj = cell !== null && typeof cell === 'object';
        var text = isObj ? (cell.t === undefined || cell.t === null ? '' : cell.t) : (cell === undefined || cell === null ? '' : cell);
        var before = old ? td.textContent : null;
        var changed;
        if (isObj && cell.html !== undefined) { setHTML(td, cell.html); changed = before !== td.textContent; }
        else changed = setText(td, text);
        var cls = isObj ? (cell.cls || '') : '';
        if (td.className !== cls) td.className = cls;
        if (isObj) {
          if (cell.title !== undefined) setAttr(td, 'title', cell.title);
          if (cell.attr) for (var ak in cell.attr) setAttr(td, ak, cell.attr[ak]);
          if (cell.style) for (var sk in cell.style) setStyleVar(td, sk, cell.style[sk]);
          // a value that changed flashes; one that appeared or went away (a level pulled) does not
          if (old && changed && before !== '' && td.textContent !== '' && cell.fl) {
            if (cell.fl === 'd') { var a = numOf(before), b = numOf(td.textContent); flash(td, b > a ? 'up' : (b < a ? 'dn' : 'sz')); }
            else flash(td, cell.fl === 's' ? 'sz' : cell.fl);
          }
        }
      }
      if (rowFn) {
        var rf = rowFn(item);
        var rcls = (rf && typeof rf === 'object') ? (rf.cls || '') : (rf || '');
        if (tr.className !== rcls) tr.className = rcls;
        if (rf && typeof rf === 'object' && rf.attr) for (var rk in rf.attr) setAttr(tr, rk, rf.attr[rk]);
      }
      var want = prev ? prev.nextSibling : tbody.firstChild;
      if (tr !== want) tbody.insertBefore(tr, want);
      prev = tr;
    }
    existing.forEach(function (tr) { tr.remove(); });
    return items.length;
  }
  // One grey "none" row for an empty grid (spec: empty grid shows one #848D97 row 'none').
  function emptyRow(tbody, ncols, text) {
    return keyedRows(tbody, [0], function () { return '__none'; }, function () {
      return [{ t: text || 'none', cls: 'none', attr: { colspan: ncols } }];
    }, function () { return 'none'; });
  }
  var byId = function (id) { return hasDoc ? document.getElementById(id) : null; };
  Desk.dom = {
    $: byId,
    setText: setText, setHTML: setHTML, setClass: setClass, setAttr: setAttr, setVar: setStyleVar,
    keyedRows: keyedRows, emptyRow: emptyRow, flash: flash,
    esc: function (s) { return String(s === null || s === undefined ? '' : s).replace(/[&<>"]/g, function (ch) { return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[ch]; }); },
    // build a <table> with a <thead> from column specs [{h:'Px', w:44, cls:'n'}]; returns tbody
    table: function (host, cols, cls) {
      if (!host) return null;
      var t = document.createElement('table'); t.className = 'grid' + (cls ? ' ' + cls : '');
      var cg = document.createElement('colgroup');
      var th = document.createElement('thead'), tr = document.createElement('tr');
      cols.forEach(function (c) {
        var col = document.createElement('col'); if (c.w) col.style.width = c.w + 'px'; cg.appendChild(col);
        var h = document.createElement('th'); h.textContent = c.h; if (c.cls) h.className = c.cls; if (c.title) h.title = c.title; tr.appendChild(h);
      });
      th.appendChild(tr); t.appendChild(cg); t.appendChild(th);
      var tb = document.createElement('tbody'); t.appendChild(tb);
      host.textContent = ''; host.appendChild(t);
      return tb;
    }
  };

  // ---------------------------------------------------------------------------------------------
  // Latency meters. Rolling windows; p50 over the last N.
  // ---------------------------------------------------------------------------------------------
  function Roll(n) { this.n = n; this.v = []; this.last = null; }
  Roll.prototype.push = function (x) { this.last = x; this.v.push(x); if (this.v.length > this.n) this.v.shift(); };
  Roll.prototype.q = function (q) {
    if (!this.v.length) return null;
    var s = this.v.slice().sort(function (a, b) { return a - b; });
    return s[Math.min(s.length - 1, Math.floor(q * s.length))];
  };
  Roll.prototype.p50 = function () { return this.q(0.5); };
  Roll.prototype.p90 = function () { return this.q(0.9); };
  Desk.Roll = Roll;

  var lat = Desk.lat = {
    paint: new Roll(50),        // click -> pending painted (ms)
    ack: new Roll(20),          // click -> first acknowledged state received (ms)
    ackV: { K: new Roll(20), P: new Roll(20) },
    frame: new Roll(120),       // per-frame render time (ms)
    _pending: new Map(),        // cid -> {t0, painted, acked, v}
    _waiting: [],               // cids waiting for their first paint
    // t0 of an input event on the performance clock
    t0: function (ev) {
      var n = perf.now();
      if (ev && typeof ev.timeStamp === 'number' && ev.timeStamp > 0 && ev.timeStamp <= n + 1 && n - ev.timeStamp < 2000) return ev.timeStamp;
      return n;
    },
    // epoch ms of an input event (for t_click_ms)
    epoch: function (ev) {
      var origin = perf.timeOrigin || (Date.now() - perf.now());
      return Math.round(origin + lat.t0(ev));
    },
    click: function (cid, ev, v) {
      var rec = { t0: lat.t0(ev), painted: false, acked: false, v: v || 'K' };
      lat._pending.set(cid, rec); lat._waiting.push(cid);
      if (lat._pending.size > 200) { var first = lat._pending.keys().next().value; lat._pending.delete(first); }
      return rec;
    },
    // called by the scheduler at the end of each frame's render pass
    painted: function () {
      if (!lat._waiting.length) return;
      var n = perf.now();
      lat._waiting.forEach(function (cid) {
        var r = lat._pending.get(cid); if (!r || r.painted) return;
        r.painted = true; lat.paint.push(n - r.t0);
      });
      lat._waiting.length = 0;
    },
    acked: function (cid) {
      var r = lat._pending.get(cid);
      if (!r || r.acked) return null;
      r.acked = true;
      var d = perf.now() - r.t0;
      lat.ack.push(d); if (lat.ackV[r.v]) lat.ackV[r.v].push(d);
      return d;
    },
    has: function (cid) { return lat._pending.has(cid); }
  };

  // ---------------------------------------------------------------------------------------------
  // Render scheduler. Event handlers only write into the store and mark topics dirty; one
  // requestAnimationFrame pass then runs the renderers. A renderer bound to a tab that is hidden
  // is skipped and redrawn in full the next time its tab shows.
  //
  //   Desk.render.mark('book:K|TK')     mark a topic (and schedule a frame)
  //   Desk.render.add({id, tab, fn})    tab: 'manual' | 'systematic' | null (always)
  //                                     fn(d) with d.has(topic), d.any(prefix), d.all (full redraw)
  //   Desk.render.flush()               run a frame now (tests, boot)
  //
  // Topics the store marks: hdr, mkt, book, book:<key>, ord, ord:<key>, fill, pos, lane, risk, mo,
  // alert, feeds, stale, snapshot (with all of them). Others: clock (250 ms), focus, tab, hk,
  // layout, ui (density/colour), lat.
  // ---------------------------------------------------------------------------------------------
  var raf = (hasDoc && root.requestAnimationFrame) ? function (f) { root.requestAnimationFrame(f); } : function (f) { setTimeout(function () { f(perf.now()); }, 16); };
  var R = Desk.render = {
    tab: null,                         // the visible tab, set by header.js
    dirty: new Set(),
    renderers: [],
    scheduled: false,
    errors: 0,
    lastError: null,
    mark: function (t) { R.dirty.add(t); if (!R.scheduled) { R.scheduled = true; raf(R.frame); } },
    add: function (spec) {
      var r = { id: spec.id, tab: spec.tab || null, fn: spec.fn, missed: true };
      R.renderers.push(r);
      R.mark('__add');
      return r;
    },
    // force a full redraw of every renderer of a tab (or all with null)
    force: function (tab) { R.renderers.forEach(function (r) { if (!tab || r.tab === tab) r.missed = true; }); R.mark('force'); },
    frame: function () {
      R.scheduled = false;
      var t0 = perf.now();
      var set = R.dirty; R.dirty = new Set();
      var snapAll = set.has('snapshot');
      var d = {
        set: set, all: false,
        has: function (t) { return this.all || set.has(t); },
        any: function (p) {
          if (this.all) return true;
          var hit = false; set.forEach(function (t) { if (!hit && t.indexOf(p) === 0) hit = true; }); return hit;
        }
      };
      for (var i = 0; i < R.renderers.length; i++) {
        var r = R.renderers[i];
        if (r.tab && R.tab && r.tab !== R.tab) { if (set.size) r.missed = true; continue; }
        d.all = r.missed || snapAll;
        r.missed = false;
        try { r.fn(d); } catch (e) {
          R.errors++; R.lastError = (r.id || '?') + ': ' + (e && e.message);
          if (root.console) console.error('[desk render]', r.id, e);
        }
      }
      lat.painted();
      lat.frame.push(perf.now() - t0);
    },
    flush: function () { R.frame(); }
  };

  // ---------------------------------------------------------------------------------------------
  // One clock. 250 ms: countdowns, the stale check, the header clock. Nothing else polls.
  // ---------------------------------------------------------------------------------------------
  var tickFns = [], tickTimer = null;
  Desk.tick = {
    on: function (fn) { tickFns.push(fn); },
    run: function () { var now = Date.now(); tickFns.forEach(function (f) { try { f(now); } catch (e) { if (root.console) console.error('[desk tick]', e); } }); R.mark('clock'); },
    start: function () { if (!tickTimer) tickTimer = setInterval(Desk.tick.run, 250); },
    stop: function () { if (tickTimer) clearInterval(tickTimer); tickTimer = null; }
  };

  // ---------------------------------------------------------------------------------------------
  // Focus and panel linking. One global focused market (key 'v|tk'). Selecting a market anywhere
  // loads it into the focused ladder and the ticket. Rows link themselves by carrying
  // data-mkt="K|TK": a click on such a row (not on a button or input inside it) focuses it;
  // Shift+click loads the other ladder.
  //   Desk.focus.key           the focused market key or null
  //   Desk.focus.ladder        'A' | 'B' | 'C' | 'D': the focused ladder
  //   Desk.focus.ladders       {A: key|null, B: key|null, C: key|null}
  //   Desk.focus.set(key, {other: bool, ladder: 'B', from: 'm-watch'})
  //   Desk.focus.setLadder('B')
  //   Desk.focus.on(fn(key, info))
  // ---------------------------------------------------------------------------------------------
  var focusFns = [];
  var F = Desk.focus = {
    key: null,
    ladder: 'A',
    ladders: { A: null, B: null, C: null, D: null },
    from: null,
    on: function (fn) { focusFns.push(fn); },
    load: function () {
      var s = Desk.storage.getJSON('ladders', null);
      if (s && typeof s === 'object') ['A', 'B', 'C', 'D'].forEach(function (k) { if (typeof s[k] === 'string') F.ladders[k] = s[k]; });
      var l = Desk.storage.get('ladder'); if (l === 'A' || l === 'B' || l === 'C' || l === 'D') F.ladder = l;
      F.key = F.ladders[F.ladder];
    },
    otherLadder: function () {
      var L = Desk.layout.ladders();
      var i = L.indexOf(F.ladder);
      return L[(i + 1) % L.length] || 'A';
    },
    set: function (key, opts) {
      opts = opts || {};
      var slot = opts.ladder || (opts.other ? F.otherLadder() : F.ladder);
      F.ladders[slot] = key;
      if (!opts.other) F.key = key;
      F.from = opts.from || null;
      Desk.storage.setJSON('ladders', F.ladders);
      R.mark('focus');
      focusFns.forEach(function (fn) { try { fn(key, { ladder: slot, other: !!opts.other, from: F.from }); } catch (e) { if (root.console) console.error(e); } });
    },
    setLadder: function (slot) {
      if (F.ladder === slot) return;
      F.ladder = slot; F.key = F.ladders[slot];
      Desk.storage.set('ladder', slot);
      R.mark('focus');
      focusFns.forEach(function (fn) { try { fn(F.key, { ladder: slot, other: false, from: 'ladder' }); } catch (e) { if (root.console) console.error(e); } });
    },
    // document-level delegation for [data-mkt] rows; a ladder ([data-ladder]) that gets the
    // pointer or keyboard focus becomes the focused ladder
    bindClicks: function () {
      if (!hasDoc) return;
      var ladderOf = function (ev) { var t = ev.target; var l = t && t.closest ? t.closest('[data-ladder]') : null; if (l) F.setLadder(l.getAttribute('data-ladder')); };
      document.addEventListener('mousedown', ladderOf, true);
      document.addEventListener('focusin', ladderOf);
      document.addEventListener('click', function (ev) {
        var t = ev.target;
        if (!t || !t.closest) return;
        var row = t.closest('[data-mkt]');
        if (!row) return;
        var ctl = t.closest('button, input, select, a, [data-nolink]');
        if (ctl && row.contains(ctl) && ctl !== row) return;
        var panel = row.closest('.panel');
        F.set(row.getAttribute('data-mkt'), { other: ev.shiftKey, from: panel ? panel.id : null });
      });
    }
  };

  // ---------------------------------------------------------------------------------------------
  // Keys. Bindings carry a scope; the most specific active scope wins:
  //   element scope   the nearest [data-kscope] around document.activeElement ('ladder',
  //                   'ticket', 'watch', ...); a ladder panel is tabindex=0 data-kscope="ladder"
  //   tab scope       'manual' or 'systematic' while that tab shows
  //   'global'
  // Typing guard (kept from ui/index.html): while an input/select/textarea/contenteditable has
  // focus, only bindings with typing:true fire (e.g. Enter in the ticket); Esc leaves the field.
  // order:true bindings fire only while HOTKEYS are ON (OFF at every page load; H toggles).
  // Key names: a letter or digit ('b', '1') matched by physical key so Alt/Option works on a
  // Mac, with 'Shift+' / 'Alt+' prefixes; punctuation by character ('`', '?', '/', '+', '-');
  // named keys as KeyboardEvent.key ('Escape', 'Enter', 'Tab', 'F1', 'ArrowUp', 'PageDown'),
  // ' ' is 'Space'. Cmd/Ctrl combinations are never taken from the browser.
  //   Desk.keys.bind({key, scope, desc, fn(ev, ctx), order, typing, after: 'g'})
  //   Desk.keys.setHK(bool), Desk.keys.hk
  // ---------------------------------------------------------------------------------------------
  var K = Desk.keys = {
    hk: false,
    binds: [],
    prefix: null, prefixAt: 0,
    // {doc: true} lists a key on the sheet without binding it (a key handled elsewhere)
    bind: function (b) {
      if (!b || !b.key || (typeof b.fn !== 'function' && !b.doc)) throw new Error('Desk.keys.bind: key and fn required');
      b.scope = b.scope || 'global';
      b.combo = K.norm(b.key);
      K.binds.push(b);
      return b;
    },
    unbind: function (b) { var i = K.binds.indexOf(b); if (i >= 0) K.binds.splice(i, 1); },
    setHK: function (on) { K.hk = !!on; R.mark('hk'); },
    // canonical 'Alt+Shift+b' from a spec string
    norm: function (s) {
      var parts = String(s).split('+');
      var base = parts.pop();
      if (base === '' && parts.length && parts[parts.length - 1] === '') { parts.pop(); base = '+'; }   // 'Shift++'
      var alt = parts.indexOf('Alt') >= 0, shift = parts.indexOf('Shift') >= 0;
      if (base === ' ') base = 'Space';
      if (base.length === 1 && /[A-Za-z]/.test(base)) { if (base !== base.toLowerCase()) shift = true; base = base.toLowerCase(); }
      return (alt ? 'Alt+' : '') + (shift ? 'Shift+' : '') + base;
    },
    // canonical combo of a keyboard event
    combo: function (ev) {
      var code = ev.code || '', base, shiftCounts;
      if (/^Key[A-Z]$/.test(code)) { base = code.charAt(3).toLowerCase(); shiftCounts = true; }
      else if (/^Digit[0-9]$/.test(code)) { base = code.charAt(5); shiftCounts = true; }
      else if (ev.key === ' ' || code === 'Space') { base = 'Space'; shiftCounts = true; }
      else if (ev.key && ev.key.length > 1) { base = ev.key; shiftCounts = true; }
      else { base = ev.key || ''; shiftCounts = false; }
      // Option+key on a Mac produces a symbol; map by physical key above. Without a code
      // (old engines), fall back to the character.
      if (!code && ev.key && ev.key.length === 1 && /[A-Za-z]/.test(ev.key)) { base = ev.key.toLowerCase(); shiftCounts = true; }
      return (ev.altKey ? 'Alt+' : '') + (ev.shiftKey && shiftCounts ? 'Shift+' : '') + base;
    },
    typing: function (el) {
      if (!el) return false;
      var tag = el.tagName;
      return tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA' || !!el.isContentEditable;
    },
    elemScope: function () {
      if (!hasDoc) return null;
      var a = document.activeElement;
      var s = a && a.closest ? a.closest('[data-kscope]') : null;
      return s ? { name: s.getAttribute('data-kscope'), el: s } : null;
    },
    overlays: [],      // functions that close an open overlay; return true if they closed one
    handle: function (ev) {
      if (ev.metaKey || ev.ctrlKey) return false;
      var target = ev.target, typing = K.typing(target);
      var combo = K.combo(ev);
      if (combo === 'Escape' || combo === 'Shift+Escape') {
        for (var o = 0; o < K.overlays.length; o++) if (K.overlays[o]()) { ev.preventDefault(); return true; }
        if (typing) { target.blur(); ev.preventDefault(); return true; }
      }
      var es = K.elemScope();
      var tab = R.tab || K.splitTab();
      var pre = (K.prefix && Date.now() - K.prefixAt < 1000) ? K.prefix : null;
      K.prefix = null;
      var cands = [];
      for (var i = 0; i < K.binds.length; i++) {
        var b = K.binds[i];
        if (b.doc || b.combo !== combo) continue;
        if ((b.after || null) !== pre) continue;
        if (typing && !b.typing) continue;
        var rank;
        if (es && b.scope === es.name) rank = 0;
        else if (b.scope === tab) rank = 1;
        else if (b.scope === 'global') rank = 2;
        else continue;
        if (typing && b.typing && rank !== 0) continue;
        cands.push({ b: b, rank: rank });
      }
      if (!cands.length) {
        // a prefix key (e.g. 'g') arms itself for one second
        if (!typing && !ev.altKey && !ev.shiftKey && K.binds.some(function (b) { return b.after === combo; })) {
          K.prefix = combo; K.prefixAt = Date.now(); ev.preventDefault(); return true;
        }
        return false;
      }
      cands.sort(function (a, b) { return a.rank - b.rank; });
      for (var c = 0; c < cands.length; c++) {
        var bb = cands[c].b;
        if (bb.order && !K.hk) continue;
        ev.preventDefault();
        try { bb.fn(ev, { scope: es ? es.name : null, el: es ? es.el : null, tab: tab }); }
        catch (e) { if (root.console) console.error('[desk key]', bb.key, e); }
        return true;
      }
      return false;
    },
    attach: function () { if (hasDoc) document.addEventListener('keydown', K.handle); },
    // SPLIT shows both tabs: the tab scope is the half holding the keyboard focus (MANUAL when
    // nothing in either half has it)
    splitTab: function () {
      if (!hasDoc) return null;
      var a = document.activeElement, root2 = a && a.closest ? a.closest('.tabroot') : null;
      return root2 && root2.id === 'tab-systematic' ? 'systematic' : 'manual';
    },
    // the key sheet: rows of [key, scope, desc] from the bindings
    sheetRows: function () {
      var order = { global: 0, manual: 1, systematic: 2 };
      return K.binds.filter(function (b) { return b.desc; }).map(function (b) {
        return { key: (b.after ? b.after + ' ' : '') + b.key, scope: b.scope, desc: b.desc, order: !!b.order };
      }).sort(function (a, b) {
        var oa = order[a.scope] === undefined ? 3 : order[a.scope], ob = order[b.scope] === undefined ? 3 : order[b.scope];
        return oa - ob || (a.scope < b.scope ? -1 : a.scope > b.scope ? 1 : 0);
      });
    }
  };

  // ---------------------------------------------------------------------------------------------
  // Layout flags mirrored from CSS media queries.
  // ---------------------------------------------------------------------------------------------
  // Ladders on screen: 1 under 1400 px, 2 to 1919, 3 from 1920 (WIDE), 4 from 2400 (the most a
  // stream subscribes at FULL depth).
  // SPLIT (>= 2400 px, the operator's choice, remembered): SYSTEMATIC on the left half, MANUAL
  // on the right half, both live. Each half then lays out as a window under 1400 px: one ladder,
  // the narrow column sets. Desk.render.tab is null while split (every renderer draws).
  Desk.layout = {
    wide: false, narrow: false, splitOK: false, splitOn: false, split: false,
    // the ladder slots on screen now
    ladders: function () {
      var L = Desk.layout;
      return L.split || L.narrow ? ['A'] : (L.splitOK ? ['A', 'B', 'C', 'D'] : (L.wide ? ['A', 'B', 'C'] : ['A', 'B']));
    },
    // the systematic column set: d ~1512 px, w WIDE, n narrow (and each half of SPLIT)
    sysMode: function () { var L = Desk.layout; return L.split || L.narrow ? 'n' : (L.wide ? 'w' : 'd'); },
    upd: null,
    setSplit: function (on) {
      Desk.layout.splitOn = !!on;
      Desk.storage.set('split', on ? '1' : '0');
      if (Desk.layout.upd) Desk.layout.upd();
    },
    init: function () {
      if (!hasDoc || !root.matchMedia) return;
      var w = root.matchMedia('(min-width: 1920px)'), n = root.matchMedia('(max-width: 1399px)'), s = root.matchMedia('(min-width: 2400px)');
      Desk.layout.splitOn = Desk.storage.get('split') === '1';
      var upd = Desk.layout.upd = function () {
        var L = Desk.layout;
        L.wide = w.matches; L.narrow = n.matches; L.splitOK = !!s.matches; L.split = L.splitOK && L.splitOn;
        document.body.setAttribute('data-wide', w.matches ? '1' : '0');
        document.body.setAttribute('data-splitok', L.splitOK ? '1' : '0');
        document.body.setAttribute('data-split', L.split ? '1' : '0');
        R.tab = L.split ? null : (Desk.tabs && Desk.tabs.cur) || R.tab;
        if (L.ladders().indexOf(F.ladder) < 0) F.setLadder('A');
        R.mark('layout'); R.mark('tab'); R.force(null);
      };
      [w, n, s].forEach(function (m) { if (m.addEventListener) m.addEventListener('change', upd); else if (m.addListener) m.addListener(upd); });
      upd();
    }
  };

  // ---------------------------------------------------------------------------------------------
  // Density (d) and colour-blind palette (c), remembered.
  // ---------------------------------------------------------------------------------------------
  Desk.ui = {
    density: 'compact',
    cb: false,
    apply: function () {
      if (!hasDoc) return;
      document.documentElement.setAttribute('data-density', Desk.ui.density);
      document.documentElement.setAttribute('data-cb', Desk.ui.cb ? '1' : '0');
      R.mark('ui');
    },
    load: function () {
      var d = Desk.storage.get('density'); if (d === 'normal' || d === 'compact') Desk.ui.density = d;
      Desk.ui.cb = Desk.storage.get('cb') === '1';
      Desk.ui.apply();
    },
    cycleDensity: function () { Desk.ui.density = Desk.ui.density === 'compact' ? 'normal' : 'compact'; Desk.storage.set('density', Desk.ui.density); Desk.ui.apply(); },
    toggleCB: function () { Desk.ui.cb = !Desk.ui.cb; Desk.storage.set('cb', Desk.ui.cb ? '1' : '0'); Desk.ui.apply(); }
  };

  // Query string flags: Desk.qs.demo, Desk.qs.live, ...
  Desk.qs = (function () {
    var o = {};
    try {
      var s = (root.location && root.location.search) || '';
      s.replace(/^\?/, '').split('&').forEach(function (kv) { if (!kv) return; var p = kv.split('='); o[decodeURIComponent(p[0])] = p.length > 1 ? decodeURIComponent(p[1]) : '1'; });
    } catch (e) { /* no location */ }
    return o;
  })();
})(typeof window !== 'undefined' ? window : globalThis);
