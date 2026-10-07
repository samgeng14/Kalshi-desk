/* ui/desk/js/universe.js
 *
 * THE UNIVERSE on the page (contract 1.4): everything that settles today (T) and tomorrow (T1),
 * a few thousand markets on a demo day, ~84,000 on a real Kalshi day. Nothing here streams it:
 * the backend pages it (GET /api/universe) and the page asks only for the pages it draws.
 *
 *   Desk.universe.create({onChange})     one paged view (the watchlist's ALL scope, the browser)
 *     .set({day, cat, v, q, sort})       a new query (a text change waits 150 ms for the typing)
 *     .res                               {total, groups [{gk, cat, n}], cats, date, sig} | null
 *     .row(i)                            the row at offset i, or null while its page is out
 *     .want(a, b)                        ask for the pages covering offsets a .. b-1
 *     .tick(now)                         every 3 s while drawn: ask again for the pages on screen
 *                                        (a row is a snapshot of the venue's listing)
 *   Desk.universe.layout(groups, collapsed, opts) -> items: [{t: 'cat', cat, n}, {t: 'grp', gk,
 *                                        cat, n}, {t: 'row', i}] (a collapsed category or group
 *                                        keeps its header only; opts.q shows everything)
 *   Desk.universe.order(rows, sort)     the backend's order (category, group by its best row, the
 *                                        rows), for the live set the page holds itself
 *   Desk.universe.liveRow(m, b, pos)    a live market as a row
 *   Desk.vlist(host, cols, cls, opts)    a virtual table: only the rows in view (plus 8 above and
 *                                        below) exist in the DOM; two spacer rows keep the scroll
 *                                        height, so thousands of rows cost what forty do. The
 *                                        drawn window moves 8 rows at a time: scrolling inside it
 *                                        redraws nothing. .retable(cols) swaps the column set.
 *                                        (The watchlist, the browser, both ACTIVITY tapes and
 *                                        Systematic's FILLS.)
 *
 * Row shape (page side, prices in u): {key, v, tk, ev, title, short, cat, grp, gk, kind, date,
 * close_ms, bid, ask, sprd, vol, fee, fair, edge, pin, live}.
 */
(function (root) {
  'use strict';
  var Desk = root.Desk = root.Desk || {};
  var hasDoc = typeof document !== 'undefined';
  var U = 10000, PAGE = 100, MAX_INFLIGHT = 3, REFRESH_MS = 3000, TYPE_MS = 150;
  var CATS = (Desk.contract && Desk.contract.enums.CAT) || ['SPORTS', 'WEATHER', 'FIN', 'ECON', 'OTHER'];
  var CAT_IX = {}; CATS.forEach(function (c, i) { CAT_IX[c] = i; });
  var CAT_SHORT = { SPORTS: 'SPT', WEATHER: 'WX', FIN: 'FIN', ECON: 'ECO', OTHER: 'OTH' };
  var SORTS = (Desk.contract && Desk.contract.enums.UNI_SORT) || ['vol', 'sprd', 'close', 'edge'];

  // the take edge after the taker fee, per contract (u): the better of buying the ask and selling
  // the bid against the model fair; null without a fair or a touch
  function takeEdge(v, fee, fair, bid, ask) {
    if (fair === null || fair === undefined) return null;
    var best = null;
    if (ask !== null && ask !== undefined) best = fair - ask - Desk.fees.takerPer(v, fee, ask);
    if (bid !== null && bid !== undefined) { var e = bid - fair - Desk.fees.takerPer(v, fee, bid); if (best === null || e > best) best = e; }
    return best;
  }
  function catOf(m) {
    if (m.cat) return m.cat;
    return m.kind === 'GAME' ? 'SPORTS' : (m.kind === 'WEATHER' ? 'WEATHER' : (m.kind === 'LIP15' ? 'FIN' : 'OTHER'));
  }
  function gkOf(m) {
    var mu = String(m.short).split(' ')[0];
    return m.kind === 'GAME' && mu.indexOf('@') > 0 ? m.grp + ' ' + mu : m.grp;
  }
  // a live market (the store's) as a row; b its book or null
  function liveRow(m, b) {
    var bid = b ? b.bb : null, ask = b ? b.ba : null, fair = b && b.ref ? b.ref.fair : null;
    return {
      key: m.key, v: m.v, tk: m.tk, ev: m.ev, title: m.title, short: m.short, cat: catOf(m), grp: m.grp, gk: gkOf(m), kind: m.kind,
      date: m.date, close_ms: m.close_ms, bid: bid, ask: ask, sprd: bid !== null && ask !== null ? ask - bid : null, vol: b ? b.vol : 0,
      fee: m.fee, fair: fair, edge: takeEdge(m.v, m.fee, fair, bid, ask), pin: !!m.pin, live: true, m: m
    };
  }
  // a row of GET /api/universe, converted at the door (prices to u)
  function fromWire(r) {
    var px = Desk.num.px;
    return {
      key: r.v + '|' + r.tk, v: r.v, tk: r.tk, ev: r.ev, title: r.title, short: r.short, cat: r.cat, grp: r.grp, gk: r.gk, kind: r.kind,
      date: r.date, close_ms: r.close_ms, bid: px(r.bid), ask: px(r.ask), sprd: px(r.sprd), vol: r.vol, fee: r.fee,
      fair: px(r.fair), edge: Desk.num.money(r.edge), pin: r.pin, live: r.live
    };
  }
  var SORTK = {
    vol: function (r) { return -(r.vol || 0); },
    sprd: function (r) { return r.bid !== null && r.ask !== null ? r.ask - r.bid : Infinity; },
    close: function (r) { return r.close_ms; },
    edge: function (r) { return r.edge === null || r.edge === undefined ? Infinity : -r.edge; }
  };
  function cmp(a, b) { return a < b ? -1 : (a > b ? 1 : 0); }
  // the backend's order: category, then each category's groups by their best row, then rows by
  // the key (ties by ticker); returns {rows, groups [{gk, cat, n}], cats {CAT: n}}
  function order(rows, sort) {
    var kf = SORTK[sort] || SORTK.close, gs = new Map(), cats = {};
    CATS.forEach(function (c) { cats[c] = 0; });
    rows.forEach(function (r) {
      var k = kf(r), g = gs.get(r.gk);
      cats[r.cat] = (cats[r.cat] || 0) + 1;
      if (!g) { g = { gk: r.gk, cat: r.cat, rows: [], best: Infinity }; gs.set(r.gk, g); }
      g.rows.push({ r: r, k: k }); if (k < g.best) g.best = k;
    });
    var gl = Array.from(gs.values()).sort(function (a, b) {
      return (CAT_IX[a.cat] === undefined ? 9 : CAT_IX[a.cat]) - (CAT_IX[b.cat] === undefined ? 9 : CAT_IX[b.cat]) ||
        (a.best === b.best ? 0 : (a.best < b.best ? -1 : 1)) || cmp(a.gk, b.gk);
    });
    var out = [];
    gl.forEach(function (g) {
      g.rows.sort(function (a, b) { return (a.k === b.k ? 0 : (a.k < b.k ? -1 : 1)) || cmp(a.r.tk, b.r.tk); });
      g.rows.forEach(function (x) { out.push(x.r); });
    });
    return { rows: out, groups: gl.map(function (g) { return { gk: g.gk, cat: g.cat, n: g.rows.length }; }), cats: cats };
  }
  // the list drawn: category and group headers, rows by their offset in the ordered result
  function layout(groups, collapsed, opts) {
    collapsed = collapsed || {}; opts = opts || {};
    var items = [], cat = null, off = 0, open = !!opts.q, catN = {};
    groups.forEach(function (g) { catN[g.cat] = (catN[g.cat] || 0) + g.n; });
    groups.forEach(function (g) {
      if (g.cat !== cat) { cat = g.cat; items.push({ t: 'cat', cat: cat, n: catN[cat], collapsed: !open && !!collapsed['c:' + cat] }); }
      if (!open && collapsed['c:' + g.cat]) { off += g.n; return; }
      var col = !open && !!collapsed[g.gk];
      items.push({ t: 'grp', gk: g.gk, cat: g.cat, n: g.n, collapsed: col });
      if (!col) for (var i = 0; i < g.n; i++) items.push({ t: 'row', i: off + i });
      off += g.n;
    });
    return items;
  }

  // ---------------------------------------------------------------------------------------------
  // one paged view
  // ---------------------------------------------------------------------------------------------
  function create(opts) {
    opts = opts || {};
    var V = {
      q: { day: 'T', cat: '', v: '', q: '', sort: 'close' },
      res: null, pages: new Map(), inflight: new Set(), shown: [], lastAt: 0, typeAt: 0, gen: 0, gets: 0,
      sig: function () { var q = V.q; return [q.day, q.cat, q.v, q.q.toLowerCase(), q.sort].join('|'); },
      set: function (patch) {
        var before = V.sig(), typing = patch.q !== undefined && patch.q !== V.q.q;
        Object.keys(patch).forEach(function (k) { V.q[k] = patch[k]; });
        if (V.sig() === before) return;
        V.res = null; V.pages = new Map(); V.inflight = new Set(); V.gen++;
        // typing: ask once the text has rested TYPE_MS (the page's clock; a timer only prompts a redraw)
        if (typing) { V.typeAt = Date.now(); setTimeout(changed, TYPE_MS + 10); }
        else { V.typeAt = 0; V.want(0, PAGE); }
        changed();
      },
      pending: function () { return !!V.typeAt && Date.now() - V.typeAt < TYPE_MS; },
      row: function (i) {
        var p = V.pages.get(Math.floor(i / PAGE));
        return p ? (p.rows[i % PAGE] || null) : null;
      },
      want: function (a, b) {
        if (V.pending()) return;
        V.typeAt = 0;
        var p0 = Math.floor(Math.max(0, a) / PAGE), p1 = Math.floor(Math.max(0, b - 1) / PAGE);
        if (V.res) p1 = Math.min(p1, Math.max(0, Math.floor((V.res.total - 1) / PAGE)));
        V.shown = [];
        for (var p = p0; p <= p1; p++) { V.shown.push(p); if (!V.pages.has(p)) fetch(p); }
      },
      tick: function (now) {
        if (now - V.lastAt < REFRESH_MS) return;
        V.lastAt = now;
        (V.shown.length ? V.shown : [0]).forEach(function (p) { fetch(p, true); });
      }
    };
    function changed() { if (opts.onChange) opts.onChange(); }
    function path(p) {
      var q = V.q, e = encodeURIComponent;
      return '/api/universe?day=' + q.day + '&cat=' + e(q.cat) + '&v=' + e(q.v) + '&q=' + e(q.q) + '&sort=' + q.sort + '&offset=' + (p * PAGE) + '&limit=' + PAGE;
    }
    function fetch(p, again) {
      if (V.inflight.has(p) || (!again && V.inflight.size >= MAX_INFLIGHT) || !Desk.transport || !Desk.transport.get) return;
      var gen = V.gen, sig = V.sig();
      V.inflight.add(p); V.gets++;
      Desk.transport.get(path(p)).then(function (r) {
        if (gen !== V.gen) return;                       // a newer query replaced this one
        V.inflight.delete(p);
        var j = r && r.json;
        if (!(r.status === 200 && j && j.ok)) { if (!again && Desk.store) Desk.store.localAlert('WARN', 'UNI', 'universe page ' + p + ': ' + (j && j.err ? j.err.code + ' ' + j.err.msg : 'HTTP ' + (r && r.status))); return; }
        var gsig = j.groups.map(function (g) { return g.gk + ':' + g.n; }).join(',');
        if (V.res && V.res.gsig !== gsig) V.pages = new Map();     // the listing changed shape: pages from before no longer line up
        V.res = { total: j.total, groups: j.groups, cats: j.cats, date: j.date, gsig: gsig, sig: sig, at: Date.now() };
        V.pages.set(p, { rows: j.rows.map(fromWire), at: Date.now() });
        changed();
      }, function () { if (gen === V.gen) V.inflight.delete(p); });
    }
    return V;
  }

  // ---------------------------------------------------------------------------------------------
  // the virtual table
  // ---------------------------------------------------------------------------------------------
  // OVER rows above and below the view are drawn too; the drawn window moves in steps of CHUNK
  // rows, so most scroll frames only scroll (the browser moves the rows already there: no script,
  // no table layout) and a redraw comes once every CHUNK rows
  var OVER = 8, CHUNK = 8;
  // opts: rowH (px, or a function returning it, e.g. Desk.dom.rowPx), headH, h (until measured),
  // onScroll (called when the window to draw changed: the owner redraws on its next frame)
  function vlist(host, cols, cls, opts) {
    opts = opts || {};
    var L = { host: host, rowH: typeof opts.rowH === 'number' ? opts.rowH : 18, rowPx: typeof opts.rowH === 'function' ? opts.rowH : null,
              headH: opts.headH || 20, top: 0, h: opts.h || 600, n: 0, a: 0, b: 0, ncol: cols.length, onScroll: opts.onScroll || null };
    L.tb = Desk.dom.table(host, cols, cls);
    // a scroll that stays inside the drawn window needs nothing from us
    L.moved = function () { var w = L.range(L.n); return w.a !== L.a || w.b !== L.b; };
    if (host && host.addEventListener) {
      host.addEventListener('scroll', function () { L.top = host.scrollTop || 0; if (L.onScroll && L.moved()) L.onScroll(); }, { passive: true });
      if (root.ResizeObserver) {
        try {
          new root.ResizeObserver(function (en) { var h = en && en[0] && en[0].contentRect ? en[0].contentRect.height : 0; if (h > 0 && h !== L.h) { L.h = h; if (L.onScroll) L.onScroll(); } }).observe(host);
        } catch (e) { /* the default height */ }
      }
    }
    // a new column set on the same host (a wider watchlist, another layout): a new table, the
    // same scroll and resize listeners
    L.retable = function (cols2, cls2) { L.tb = Desk.dom.table(host, cols2, cls2); L.ncol = cols2.length; L.a = 0; L.b = 0; return L.tb; };
    // the item range to draw for n items: [a, b), a on a CHUNK boundary
    L.range = function (n) {
      var rh = L.rowH, vis = Math.ceil(L.h / rh) + 1;
      var first = Math.max(0, Math.floor(L.top / rh));
      var a = Math.floor(Math.max(0, first - OVER) / CHUNK) * CHUNK;
      // a list that shrank under a scrolled view: draw its end, never an empty window
      a = Math.max(0, Math.min(a, n - vis - OVER));
      var b = Math.min(n, a + vis + 2 * OVER + CHUNK);
      return { a: a, b: b };
    };
    L.draw = function (items, keyFn, cellsFn, rowFn, kopts) {
      if (L.rowPx) { var rp = L.rowPx(); if (rp > 0) L.rowH = rp; }
      var n = items.length, w = L.range(n), list = [];
      L.n = n; L.a = w.a; L.b = w.b;
      list.push({ pad: 't', h: w.a * L.rowH });
      for (var i = w.a; i < w.b; i++) list.push(items[i]);
      list.push({ pad: 'b', h: (n - w.b) * L.rowH });
      var nc = L.ncol;
      return Desk.dom.keyedRows(L.tb, list, function (x) { return x.pad ? '__pad' + x.pad : keyFn(x); }, function (x) {
        return x.pad ? [{ t: '', cls: 'vpad', attr: { colspan: nc }, style: { height: x.h + 'px' } }] : cellsFn(x);
      }, function (x) { return x.pad ? { cls: 'vpad' } : rowFn(x); }, kopts);
    };
    // bring item i into view (a scroll write, never a layout read)
    L.reveal = function (i) {
      var y = L.headH + i * L.rowH;
      if (y - L.headH < L.top) L.top = Math.max(0, y - L.headH);
      else if (y + L.rowH > L.top + L.h) L.top = y + L.rowH - L.h;
      else return false;
      if (host) host.scrollTop = L.top;
      return true;
    };
    return L;
  }

  Desk.universe = {
    create: create, layout: layout, order: order, liveRow: liveRow, fromWire: fromWire, takeEdge: takeEdge, catOf: catOf, gkOf: gkOf,
    CATS: CATS, CAT_SHORT: CAT_SHORT, SORTS: SORTS, PAGE: PAGE
  };
  Desk.vlist = vlist;
  void hasDoc;
})(typeof window !== 'undefined' ? window : globalThis);
