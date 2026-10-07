/* ui/desk/js/universe.js
 *
 * THE UNIVERSE on the page (contract 1.4): everything that settles today (T) and tomorrow (T1),
 * a few thousand markets on a demo day, ~84,000 on a real Kalshi day. Nothing here streams it:
 * the backend pages it (GET /api/universe) and the page asks only for the pages it draws.
 *
 *   Desk.universe.create({onChange})     one paged view (the watchlist's ALL scope, the browser)
 *     .set({day, cat, v, q, sort})       a new query (a text change waits 150 ms for the typing)
 *     .res                               {total, groups [{gk, cat, n}], cats, date, grev, sig} |
 *                                        null. A new query keeps the previous result (and its
 *                                        pages) drawn until the new query's first page lands, then
 *                                        swaps: typing never blanks the list.
 *     .waiting()                         true while what is drawn is not the current query's
 *                                        (typing, or its first page in flight): an Enter waits
 *     .row(i)                            the row at offset i, or null while its page is out
 *     .want(a, b)                        ask for the pages covering offsets a .. b-1
 *     .tick(now)                         every 3 s while drawn: ask again for the pages on screen
 *                                        (a row is a snapshot of the venue's listing). When the
 *                                        listing changed shape (a market listed or closed), the
 *                                        pages on screen are asked again for the new shape and
 *                                        swapped in together; the old ones stay drawn till then.
 *     .layout(collapsed, q)              the cached layout of .res: {items, rowItems}, rebuilt only
 *                                        when the listing's shape (grev), the collapsed headers or
 *                                        the search's presence change
 *   Desk.universe.layout(groups, collapsed, opts) -> items: [{t: 'cat', cat, n}, {t: 'grp', gk,
 *                                        cat, n}, {t: 'row', i}] (a collapsed category or group
 *                                        keeps its header only; opts.q shows everything)
 *   Desk.universe.layoutOf(cache, groups, sig, collapsed, q) the same through a cache object
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
  // the group a market shows under: the contract's gk (mkt.gk, contract 1.5); for an older
  // backend, grp, or for a game grp + its matchup taken from short ('AWAY@HOME ...')
  function gkOf(m) {
    if (m.gk) return m.gk;
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
  // the layout through a cache: rebuilt only when the listing's shape (sig), the collapsed headers
  // or the search's presence change; a redraw (every book tick, every clock second) reuses it, so
  // a day of 84,000 rows is not one new object per row per frame
  function layoutOf(cache, groups, sig, collapsed, q) {
    var cs = Object.keys(collapsed || {}).filter(function (k) { return collapsed[k]; }).sort().join('\u0001');
    var key = sig + '\u0002' + cs + '\u0002' + (q ? 1 : 0);
    if (cache.key === key && cache.items) return cache;
    var items = layout(groups, collapsed, { q: q }), rowItems = [];
    for (var i = 0; i < items.length; i++) if (items[i].t === 'row') rowItems.push(i);
    cache.key = key; cache.items = items; cache.rowItems = rowItems; cache.builds = (cache.builds || 0) + 1;
    return cache;
  }

  // ---------------------------------------------------------------------------------------------
  // one paged view
  // ---------------------------------------------------------------------------------------------
  function create(opts) {
    opts = opts || {};
    var V = {
      q: { day: 'T', cat: '', v: '', q: '', sort: 'close' },
      res: null, pages: new Map(), nx: null, inflight: new Set(), shown: [], lastAt: 0, typeAt: 0, gen: 0, gets: 0, lay: {},
      sig: function () { var q = V.q; return [q.day, q.cat, q.v, q.q.toLowerCase(), q.sort].join('|'); },
      set: function (patch) {
        var before = V.sig(), typing = patch.q !== undefined && patch.q !== V.q.q;
        Object.keys(patch).forEach(function (k) { V.q[k] = patch[k]; });
        if (V.sig() === before) return;
        // the result on screen stays (dimmed by the owner) until the new query's first page lands;
        // replies to the old query are dropped by their generation
        V.inflight = new Set(); V.nx = null; V.gen++;
        // typing: ask once the text has rested TYPE_MS (the page's clock; a timer only prompts a redraw)
        if (typing) { V.typeAt = Date.now(); setTimeout(changed, TYPE_MS + 10); }
        else { V.typeAt = 0; fetch(0); }
        changed();
      },
      pending: function () { return !!V.typeAt && Date.now() - V.typeAt < TYPE_MS; },
      waiting: function () { return V.pending() || !V.res || V.res.sig !== V.sig(); },
      row: function (i) {
        var p = V.pages.get(Math.floor(i / PAGE));
        return p ? (p.rows[i % PAGE] || null) : null;
      },
      want: function (a, b) {
        if (V.pending()) return;
        V.typeAt = 0;
        // a new query: only its first page, which swaps the list in (the scroll is at the top)
        if (!V.res || V.res.sig !== V.sig()) { fetch(0); return; }
        var p0 = Math.floor(Math.max(0, a) / PAGE), p1 = Math.floor(Math.max(0, b - 1) / PAGE);
        p1 = Math.min(p1, Math.max(0, Math.floor((V.res.total - 1) / PAGE)));
        V.shown = [];
        for (var p = p0; p <= p1; p++) { V.shown.push(p); if (!V.pages.has(p)) fetch(p); }
      },
      tick: function (now) {
        if (now - V.lastAt < REFRESH_MS) return;
        V.lastAt = now;
        if (V.waiting()) { if (!V.pending()) fetch(0); return; }
        (V.shown.length ? V.shown : [0]).forEach(function (p) { fetch(p, true); });
      },
      layout: function (collapsed, q) { return V.res ? layoutOf(V.lay, V.res.groups, V.res.sig + '\u0003' + V.res.grev, collapsed, q) : { items: [], rowItems: [] }; }
    };
    function changed() { if (opts.onChange) opts.onChange(); }
    // grev: the listing's shape the page holds for this query; the backend then leaves groups out
    // of its reply when they have not changed (contract 1.5)
    function path(p, grev) {
      var q = V.q, e = encodeURIComponent;
      return '/api/universe?day=' + q.day + '&cat=' + e(q.cat) + '&v=' + e(q.v) + '&q=' + e(q.q) + '&sort=' + q.sort + '&offset=' + (p * PAGE) + '&limit=' + PAGE +
        (grev ? '&grev=' + e(grev) : '');
    }
    function fetch(p, again) {
      if (V.inflight.has(p) || (!again && V.inflight.size >= MAX_INFLIGHT) || !Desk.transport || !Desk.transport.get) return;
      var gen = V.gen, sig = V.sig(), held = V.res && V.res.sig === sig ? V.res : null;
      V.inflight.add(p); V.gets++;
      Desk.transport.get(path(p, held ? held.grev : '')).then(function (r) {
        if (gen !== V.gen) return;                       // a newer query replaced this one
        V.inflight.delete(p);
        var j = r && r.json;
        if (!(r.status === 200 && j && j.ok)) { if (!again && Desk.store) Desk.store.localAlert('WARN', 'UNI', 'universe page ' + p + ': ' + (j && j.err ? j.err.code + ' ' + j.err.msg : 'HTTP ' + (r && r.status))); return; }
        var cur = V.res && V.res.sig === sig ? V.res : null;
        var groups = j.groups;
        // a backend without grev: the shape is its groups list
        var grev = j.grev || (groups ? groups.map(function (g) { return g.gk + ':' + g.n; }).join(',') : '');
        if (!groups) {
          // groups left out: the shape is the one this page holds (grev matched); anything else
          // means the reply cannot be placed, so ask again in full
          var base = cur && cur.grev === grev ? cur : (V.nx && V.nx.res.grev === grev ? V.nx.res : null);
          if (!base) { if (cur) cur.grev = ''; return; }
          groups = base.groups;
        }
        var res = { total: j.total, groups: groups, cats: j.cats, date: j.date, grev: grev, sig: sig, at: Date.now() };
        var page = { rows: j.rows.map(fromWire), at: Date.now() };
        if (!cur) {
          // the first page of a new query: the list swaps at once
          V.res = res; V.pages = new Map([[p, page]]); V.nx = null;
        } else if (cur.grev === res.grev) {
          V.res = res; V.pages.set(p, page);
        } else {
          // the listing changed shape (a market listed or closed): rows from before no longer
          // line up with the new groups, so the pages on screen are asked again for the new shape
          // and swapped in together; until then the old ones stay drawn (no placeholder flicker)
          if (!V.nx || V.nx.res.grev !== res.grev) V.nx = { res: res, pages: new Map() };
          V.nx.pages.set(p, page);
          var need = V.shown.length ? V.shown : [0];
          if (need.every(function (q) { return V.nx.pages.has(q); })) { V.res = V.nx.res; V.pages = V.nx.pages; V.nx = null; }
          else { need.forEach(function (q) { if (!V.nx.pages.has(q)) fetch(q, true); }); return; }
        }
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
    create: create, layout: layout, layoutOf: layoutOf, order: order, liveRow: liveRow, fromWire: fromWire, takeEdge: takeEdge, catOf: catOf, gkOf: gkOf,
    CATS: CATS, CAT_SHORT: CAT_SHORT, SORTS: SORTS, PAGE: PAGE
  };
  Desk.vlist = vlist;
  void hasDoc;
})(typeof window !== 'undefined' ? window : globalThis);
