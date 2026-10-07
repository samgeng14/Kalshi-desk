/* ui/desk/js/browse.js
 *
 * THE MARKET BROWSER (u, or + on the watchlist): the day's whole universe (contract 1.4) in a
 * wide overlay over the watchlist and the first two ladders, to find markets and pin them.
 *   T / T+1, venue (K+P / K / P), sort (CLOSE VOL SPRD EDGE), search (/), categories with their
 *   counts; category and group headers collapse; virtual rows (Desk.vlist): only the rows in
 *   view exist, the pages they need are asked for (Desk.universe), a page on screen is asked
 *   again every 3 s.
 *   A row: pointerdown loads the market into the focused ladder (Shift: the other ladder); the
 *   click that ends it closes the browser (never on pointerdown: the press must not land on the
 *   ladder underneath). The star pins / unpins at once (POST /api/watch); * does it from the
 *   keyboard. Up / Down move, Enter loads and closes, Shift+Enter loads the other ladder and
 *   stays, Esc closes.
 *   Enter loads the cursor's market only when the cursor is a row of the results drawn now; a
 *   search or a filter change clears the cursor, so Enter then loads the first hit. While a new
 *   search is still on its way (the previous results stay drawn, dimmed), an Enter waits and
 *   loads the first hit when it lands.
 *
 *   Desk.browse.open(on?), .close(), .isOpen, .state()
 */
(function (root) {
  'use strict';
  var Desk = root.Desk = root.Desk || {};
  var hasDoc = typeof document !== 'undefined';
  var STAR = '★', NOSTAR = '☆';
  var SORT_CYCLE = ['close', 'vol', 'sprd', 'edge'], SORT_LBL = { close: 'CLOSE', vol: 'VOL', sprd: 'SPRD', edge: 'EDGE' };
  var VENUES = ['', 'K', 'P'], VEN_LBL = { '': 'K+P', K: 'K', P: 'P' };
  var FEE_LBL = { QUAD: 'T', QUAD_MAKER: 'T+M', FREE: 'FREE', PM: 'PM' };
  var ENTER_WAIT_MS = 3000;            // an Enter queued behind a search is dropped after this long
  var B = { open: false, built: false, q: { day: 'T', cat: '', v: '', q: '', sort: 'vol' }, collapsed: {}, cursor: null, cursorI: -1,
            items: [], rowItems: [], el: {}, uni: null, V: null, pinOf: {}, dirty: true, lastAt: 0, enterQ: null };

  function $(id) { return hasDoc ? document.getElementById(id) : null; }
  function el(tag, cls, text, attrs) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined && text !== null) e.textContent = text;
    if (attrs) for (var k in attrs) e.setAttribute(k, attrs[k]);
    return e;
  }
  function btn(text, cls, attrs) { var b = el('button', cls, text, attrs); b.setAttribute('type', 'button'); return b; }
  function mark() { B.dirty = true; Desk.render.mark('mbrowse'); }
  // a new search or filter: the cursor was a row of the old results
  function reset() { B.cursor = null; B.cursorI = -1; B.V.top = 0; if (B.body) B.body.scrollTop = 0; }
  function btnOf(ev) { var b = ev.target && ev.target.closest ? ev.target.closest('button') : null; return b && !b.disabled ? b : null; }

  function build() {
    var panel = $('m-browse');
    if (!panel || B.built) return;
    B.built = true;
    var E = B.el, U = Desk.universe;
    var title = el('div', 'ptitle'), name = el('span', 'pname', 'MARKET BROWSER'), tools = el('span', 'ptools');
    E.day = [btn('T', 'bd', { 'data-a': 'day', 'data-v': 'T', title: 'settles today' }), btn('T+1', 'bd', { 'data-a': 'day', 'data-v': 'T1', title: 'settles tomorrow' })];
    E.ven = btn('K+P', 'bv', { 'data-a': 'venue', title: 'venue: both / Kalshi / Polymarket US' });
    E.sort = btn('VOL', 'bo', { 'data-a': 'sort', title: 'sort: time to close / volume / spread / edge' });
    E.q = el('input', 'srch', null, { type: 'text', placeholder: 'search /', autocomplete: 'off', spellcheck: 'false' });
    E.n = el('span', 'pinfo n');
    E.x = btn('x', '', { 'data-a': 'close', title: 'close (Esc)' });
    [E.day[0], E.day[1], E.ven, E.sort, E.q, E.n, E.x].forEach(function (x) { tools.appendChild(x); });
    title.appendChild(name); title.appendChild(tools);
    var bar = el('div', 'wbar');
    E.cats = [['', 'ALL']].concat(U.CATS.map(function (c) { return [c, c]; })).map(function (x) { return btn(x[1], 'bc', { 'data-a': 'cat', 'data-v': x[0] }); });
    E.cats.forEach(function (b) { bar.appendChild(b); });
    var body = el('div', 'pbody');
    panel.textContent = '';
    panel.appendChild(title); panel.appendChild(bar); panel.appendChild(body);
    B.body = body;
    // columns: the star, venue, the market and its title, when it closes, the touch, the spread,
    // the day's volume, the model's fair and take edge (where the desk has a model), the fee type
    B.V = Desk.vlist(body, [{ h: STAR, w: 18, cls: 'pin' }, { h: 'V', w: 18 }, { h: 'Market', w: 168 }, { h: 'Title' }, { h: 'Close', w: 44, cls: 'tcl' },
                            { h: 'T', w: 52, cls: 'tcl' }, { h: 'Bid', w: 40, cls: 'n' }, { h: 'Ask', w: 40, cls: 'n' }, { h: 'Sprd¢', w: 46, cls: 'n' },
                            { h: 'Vol', w: 58, cls: 'n' }, { h: 'Fair', w: 40, cls: 'n fair' }, { h: 'Edge¢', w: 50, cls: 'n' }, { h: 'Fee', w: 82 }],
                     'browse', { rowH: 18, headH: 20, onScroll: mark });
    B.uni = U.create({ onChange: mark });
    var tool = function (ev) {
      var b = btnOf(ev); if (!b) return;
      var a = b.getAttribute('data-a'), v = b.getAttribute('data-v'), q = B.q;
      if (a === 'day') q.day = v;
      else if (a === 'cat') q.cat = v;
      else if (a === 'venue') q.v = VENUES[(VENUES.indexOf(q.v) + 1) % VENUES.length];
      else if (a === 'sort') q.sort = SORT_CYCLE[(SORT_CYCLE.indexOf(q.sort) + 1) % SORT_CYCLE.length];
      else if (a === 'close') return;                      // closes on the click (below)
      else return;
      reset();
      mark();
    };
    Desk.dom.onPress(title, tool, function (ev) { var b = btnOf(ev); return b && b.getAttribute('data-a') !== 'close' ? b : null; });
    Desk.dom.onPress(bar, tool, btnOf);
    E.x.addEventListener('click', function () { close(); });
    E.q.addEventListener('input', function () { B.q.q = E.q.value; reset(); mark(); });
    // rows: the star and the headers act at pointerdown; a market row loads at pointerdown (the
    // document's row link) and the click that ends the press closes the browser
    var rowAt = function (ev) { var tr = ev.target && ev.target.closest ? ev.target.closest('tr') : null; return tr && B.V && B.V.tb.contains(tr) ? tr : null; };
    Desk.dom.onPress(body, function (ev) {
      var tr = rowAt(ev); if (!tr) return;
      var k = tr.getAttribute('data-mkt'), star = ev.target.closest('[data-a]');
      if (star && star.getAttribute('data-a') === 'pin' && k) { pin(k); return; }
      var g = tr.getAttribute('data-grp');
      if (g) { if (B.collapsed[g]) delete B.collapsed[g]; else B.collapsed[g] = true; mark(); return; }
      if (k) { B.cursor = k; B.cursorI = -1; mark(); }
    }, rowAt);
    body.addEventListener('click', function (ev) {
      var tr = rowAt(ev); if (!tr) return;
      var star = ev.target.closest ? ev.target.closest('[data-a]') : null;
      if (tr.getAttribute('data-mkt') && !star && !ev.shiftKey) close();
    });
    panel.setAttribute('data-kscope', 'browse');
    panel.setAttribute('tabindex', '0');
  }
  function pin(key) {
    var st = Desk.store, sp = st.split(key), on = !st.pinned(key, B.pinOf[key]);
    Desk.act.watch(sp.v, sp.tk, on);
    mark();
  }
  function open(on) {
    if (on === undefined) on = !B.open;
    if (!on) { close(); return; }
    build();
    if (!B.built) return;
    // opens on the watchlist's day
    var ms = Desk.manual && Desk.manual.state ? Desk.manual.state().cfg : null;
    if (!B.open && ms && ms.watch) B.q.day = ms.watch.day;
    B.open = true;
    Desk.dom.setClass(document.body, 'browse', true);
    // the search takes the keys once the frame that shows the browser is painted: focusing an
    // element that was display:none a moment ago forces a layout, which would run inside the
    // handler (7-11 ms at pointerdown in Chrome); later, the layout is already done (and the
    // press's own default focus change is over)
    B.focusQ = true;
    mark();
    var focusQ = function () { if (B.open && B.focusQ) { B.focusQ = false; if (B.el.q && B.el.q.focus) B.el.q.focus(); } };
    if (Desk.later) Desk.later(focusQ); else focusQ();
  }
  function close() {
    if (!B.open) return false;
    B.open = false;
    B.focusQ = false;
    // blur first: blurring after the class change would restyle the page inside the handler
    var a = document.activeElement;
    if (a && a.blur && B.el.q === a) a.blur();
    Desk.dom.setClass(document.body, 'browse', false);
    return true;
  }
  function move(n) {
    var rows = B.rowItems;
    if (!rows.length) return;
    var at = rows.indexOf(B.cursorI), j = at < 0 ? (n > 0 ? 0 : rows.length - 1) : Math.max(0, Math.min(rows.length - 1, at + n));
    B.cursorI = rows[j];
    var r = B.rowAt(B.cursorI);
    B.cursor = r ? r.key : null;
    if (B.V) B.V.reveal(B.cursorI);
    mark();
  }
  // the cursor, when it is a row of the results drawn now (an arrow put it there, or a press on
  // a row in view); its item index is found in the drawn window when a press set it
  function cursorKey() {
    if (!B.cursor || !B.rowAt) return null;
    var r = B.cursorI >= 0 ? B.rowAt(B.cursorI) : null;
    if (r && r.key === B.cursor) return B.cursor;
    for (var i = B.V ? B.V.a : 0; B.V && i < B.V.b; i++) { var x = B.rowAt(i); if (x && x.key === B.cursor) { B.cursorI = i; return B.cursor; } }
    return null;
  }
  // the first hit of the results drawn, or null while its page is out
  function firstKey() { var r = B.rowItems.length && B.rowAt ? B.rowAt(B.rowItems[0]) : null; return r ? r.key : null; }
  function go(k, other) {
    B.enterQ = null;
    Desk.focus.set(k, { other: !!other, from: 'm-browse' });
    if (!other) close();
  }
  // the query as the controls set it, applied now (the render applies it on the next frame; an
  // Enter in between must not read the previous list)
  function syncQuery() { if (B.uni) { var q = B.q; B.uni.set({ day: q.day, cat: q.cat, v: q.v, q: q.q, sort: q.sort }); } }
  function load(other) {
    syncQuery();
    // waiting: the search is out, or its results have landed but are not drawn yet
    var waiting = !B.uni || B.uni.waiting() || B.uni.res !== B.drawn;
    var k = waiting ? null : (cursorKey() || firstKey());
    if (k) { go(k, other); return; }
    // the search is still on its way (or its first page): load its first hit when it lands
    if (waiting || (B.uni.res && B.uni.res.total > 0)) { B.enterQ = { other: !!other, at: Date.now() }; mark(); }
  }
  // a queued Enter, once the current query's first rows are drawn
  function runEnterQ() {
    var e = B.enterQ;
    if (!e) return;
    if (Date.now() - e.at > ENTER_WAIT_MS) { B.enterQ = null; return; }
    if (B.uni.waiting() || B.uni.res !== B.drawn) return;
    var k = firstKey();
    if (k) go(k, e.other);
    else if (B.uni.res && B.uni.res.total === 0) B.enterQ = null;      // nothing matched: nothing to load
  }

  function render(d) {
    if (!B.open || !B.built) return;
    var now = Date.now();
    if (d.has('clock')) { B.uni.tick(now); if (now - B.lastAt >= 1000) B.dirty = true; }
    if (!(B.dirty || d.all || d.has('mbrowse') || d.has('mkt') || d.has('uni'))) return;
    B.dirty = false; B.lastAt = now;
    var st = Desk.store, U = Desk.universe, F = Desk.fmt, D = Desk.dom, E = B.el, q = B.q;
    B.uni.set({ day: q.day, cat: q.cat, v: q.v, q: q.q, sort: q.sort });
    if (B.uni.pending()) B.dirty = true;                   // a search waiting for the typing to rest
    else if (B.uni.waiting()) B.uni.want(0, U.PAGE);
    // the previous results stay drawn, dimmed, until the new query's first page lands
    D.setClass(B.body, 'wait', !!B.uni.res && B.uni.waiting());
    E.day.forEach(function (b) { D.setClass(b, 'on', b.getAttribute('data-v') === q.day); });
    D.setAttr(E.day[0], 'title', 'settles today' + (st.today ? ' ' + st.today : ''));
    D.setText(E.ven, VEN_LBL[q.v]); D.setClass(E.ven, 'on', !!q.v);
    D.setText(E.sort, SORT_LBL[q.sort]);
    var res = B.uni.res;
    E.cats.forEach(function (b) {
      var c = b.getAttribute('data-v'), n = res && res.cats ? (c ? res.cats[c] || 0 : Object.keys(res.cats).reduce(function (t, k) { return t + res.cats[k]; }, 0)) : null;
      D.setText(b, (c || 'ALL') + (n === null ? '' : ' ' + n)); D.setClass(b, 'on', c === q.cat);
    });
    D.setText(E.n, res ? res.total + ' rows' : '…');
    // the layout is cached on the listing's shape: a redraw does not rebuild one item per row
    var lay = B.uni.layout(B.collapsed, q.q), items = lay.items;
    B.drawn = res;
    // a market in the live set shows its own book; any other row is the listing's
    var rowAtI = function (i) { var r = B.uni.row(i); if (!r) return null; var m = st.liveMarket(r.key); return m && m.status === 'OPEN' ? U.liveRow(m, st.book(r.key)) : r; };
    B.items = items; B.rowItems = lay.rowItems;
    B.rowAt = function (k) { var it = items[k]; return it && it.t === 'row' ? rowAtI(it.i) : null; };
    if (!items.length) { D.emptyRow(B.V.tb, 13, res ? 'none' : 'loading'); runEnterQ(); return; }
    var nowS = now + (st.srvOffset || 0);
    B.pinOf = {};
    B.V.draw(items, function (x) {
      if (x.t === 'cat') return 'c:' + x.cat;
      if (x.t === 'grp') return 'g:' + x.gk;
      var r = rowAtI(x.i); return r ? r.key : '__u' + x.i;
    }, function (x) {
      if (x.t === 'cat') return [{ t: (x.collapsed ? '+ ' : '') + x.cat + ' ' + x.n, cls: 'ch', attr: { colspan: 13 } }];
      if (x.t === 'grp') return [{ t: (x.collapsed ? '+ ' : '') + x.gk + ' ' + x.n, attr: { colspan: 13 } }];
      var r = rowAtI(x.i);
      if (!r) return [{ t: '' }, { t: '' }, { t: '…', cls: 't dim' }, '', '', '', '', '', '', '', '', '', ''];
      var pinned = st.pinned(r.key, r.pin), m = st.market(r.key), g = m ? m.g : null;
      B.pinOf[r.key] = r.pin;
      var px = function (u) { return u === null || u === undefined ? '' : (g ? F.pxg(u, g) : F.px(u, 2)); };
      var sp = r.bid !== null && r.ask !== null && r.bid !== undefined && r.ask !== undefined ? r.ask - r.bid : null;
      return [
        { t: pinned ? STAR : NOSTAR, cls: 'pin' + (pinned ? ' on' : ''), attr: { 'data-a': 'pin', 'data-nolink': '1' }, title: pinned ? 'unpin' : 'pin to the watchlist' },
        { t: r.v, cls: 't dim' },
        { t: r.short, cls: 't', title: r.tk },
        { t: r.title, cls: 't lbl', title: r.title },
        { t: F.hm(r.close_ms), cls: 'tcl dim', title: F.time(r.close_ms) },
        { t: F.left(r.close_ms - nowS), cls: 'tcl' },
        { t: px(r.bid), cls: 'n bid' },
        { t: px(r.ask), cls: 'n ask' },
        { t: sp === null ? '' : (sp / 100).toFixed(sp % 100 ? 1 : 0), cls: 'n' },
        { t: F.whole(r.vol), cls: 'n dim', title: F.size(r.vol) },
        { t: r.fair === null || r.fair === undefined ? '' : (r.fair / 10000).toFixed(2), cls: 'n fair' },
        { t: r.edge === null || r.edge === undefined ? '' : F.cents(r.edge), cls: 'n ' + (r.edge > 0 ? 'warn' : 'dim') },
        // the fee in a word: T takers pay, T+M both pay, FREE, PM (taker fee, maker rebate)
        { t: (FEE_LBL[r.fee.type] || r.fee.type) + (r.fee.mult !== 1 ? ' x' + r.fee.mult : ''), cls: 't dim', title: r.fee.type + ' x' + r.fee.mult }
      ];
    }, function (x) {
      if (x.t === 'cat') return { cls: 'cat', attr: { 'data-grp': 'c:' + x.cat, 'data-mkt': null, 'data-v': null } };
      if (x.t === 'grp') return { cls: 'grp', attr: { 'data-grp': x.gk, 'data-mkt': null, 'data-v': null } };
      var r = rowAtI(x.i);
      if (!r) return { cls: 'ph', attr: { 'data-mkt': null, 'data-v': null, 'data-grp': null } };
      return { cls: (r.key === B.cursor ? 'sel' : '') + (r.live ? '' : ' uni'), attr: { 'data-mkt': r.key, 'data-v': r.v, 'data-grp': null } };
    });
    var a = Infinity, b = -1;
    for (var vi = B.V.a; vi < B.V.b; vi++) { var it = items[vi]; if (it && it.t === 'row') { if (it.i < a) a = it.i; if (it.i > b) b = it.i; } }
    if (b >= 0) B.uni.want(a, b + 1);
    runEnterQ();
  }

  function init() {
    if (!hasDoc || !$('m-browse')) return;
    var K = Desk.keys;
    K.overlays.push(function () { return close(); });
    K.bind({ key: 'ArrowUp', scope: 'browse', typing: true, desc: 'browser: previous market', fn: function () { move(-1); } });
    K.bind({ key: 'ArrowDown', scope: 'browse', typing: true, desc: 'browser: next market', fn: function () { move(1); } });
    K.bind({ key: 'PageUp', scope: 'browse', typing: true, fn: function () { move(-20); } });
    K.bind({ key: 'PageDown', scope: 'browse', typing: true, fn: function () { move(20); } });
    K.bind({ key: 'Enter', scope: 'browse', typing: true, desc: 'browser: load into the focused ladder and close', fn: function () { load(false); } });
    K.bind({ key: 'Shift+Enter', scope: 'browse', typing: true, desc: 'browser: load into the other ladder, stay open', fn: function () { load(true); } });
    K.bind({ key: 'Shift+8', scope: 'browse', desc: 'browser: * pins / unpins the market under the cursor', fn: function () { if (B.cursor) pin(B.cursor); } });
    K.bind({ key: '*', scope: 'browse', fn: function () { if (B.cursor) pin(B.cursor); } });
    K.bind({ key: '/', scope: 'browse', fn: function () { if (B.el.q) { B.el.q.focus(); if (B.el.q.select) B.el.q.select(); } } });
    Desk.render.add({ id: 'm-browse', tab: 'manual', fn: render });
    // its DOM is built after the first paint, so the first u / + only shows it (no build in the handler)
    if (Desk.later) Desk.later(build); else build();
  }

  Desk.browse = {
    init: init, open: open, close: close,
    get isOpen() { return B.open; },
    state: function () { return B; }
  };
})(typeof window !== 'undefined' ? window : globalThis);
