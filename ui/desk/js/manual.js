/* ui/desk/js/manual.js
 *
 * The MANUAL tab: watchlist, price ladders A/B (C from 1920 px, D from 2400), the ticket, and the manual
 * working orders, fills and positions grids. Everything links through Desk.focus: a market picked
 * anywhere loads into the focused ladder and the ticket.
 *
 * LADDER (fixed price axis, YES terms): WRK B | BID | REF | PRICE | ASK | WRK S | LTQ.
 *   - One row per price on the market's grid (or every 5th/10th 0.1c price when zoomed); the axis
 *     never moves under the pointer. Wheel scrolls, Space recentres; it recentres by itself only
 *     when the touch leaves the window and the pointer is not over the ladder.
 *   - Left-click the bid side (BID or WRK B) buys, the ask side (ASK or WRK S) sells:
 *       away from the touch      POST (post-only) at that price, the ladder's expiry (5/15/60 s)
 *       at or through the touch  IOC at that price plus the slippage cap ("fills up to"), under
 *                                the fat-finger guard: more than 2 ticks through the touch is
 *                                refused (amber flash, reason in the status bar) unless the click
 *                                is a Shift+click or a second click on the same cell within 3 s.
 *   - Right-click a level's working orders cancels them. Dragging our working orders to another
 *     price re-prices them as cancel + a new post-only order (queue place is lost; the status bar
 *     says so). A click on PRICE copies it to the ticket; it never sends.
 *   - Our orders show size and seconds left ('8 12s'), a partial fill '8(2)'; a sent order not
 *     yet acknowledged is grey and dotted (PEND); a cancel in flight is struck through.
 *   - REF markers: F model fair, E ESPN, A our average entry. Break-even band: prices within the
 *     taker fee of the model fair.
 * TICKET: venue (paired market on the other venue), side, qty, price (AUTO follows the touch until
 *   typed or stepped), IOC / FOK / POST 5s 15s 60s 5m 30m START (15 s default, no GTC; START = an
 *   exchange-side expiry at the game's start_ms, off when the market has none or it has passed),
 *   cost / max loss / fee / shard line (a short Kalshi shard offers MOVE: one click moves the
 *   missing cash from the shard with the most free cash), edge vs model and ESPN, the guard as a
 *   second click (the button turns OVERRIDE for 3 s).
 * HOTKEYS: off at load (H toggles, foundation). In a focused ladder: B/F1 S/F2 take (IOC at the
 *   touch plus the cap), Shift+B/F3 Shift+S/F4 join, Alt+B/F5 Alt+S/F6 improve one tick (spread of
 *   2+ ticks), Esc cancel this market, 1-6 size presets, + / - step size. Shift+Esc cancels every
 *   manual order. Always on: Space recentre, arrows / PgUp / PgDn scroll, Tab next ladder, / search.
 *
 * Exports (window.Desk.manual):
 *   init(), render(d)        the foundation's tab contract (panels render through their own
 *                            Desk.render renderers registered in init, so one failing panel cannot
 *                            blank the others)
 *   logic                    pure helpers, no DOM (the node tests drive these)
 *   state()                  the tab's runtime state, for tests and debugging
 *
 * Held markets: a manual order makes its market MAN (the backend cancels Systematic's orders
 * there, why TAKEOVER) until POST /api/release; the backend reports it as mkt.man. Between our send
 * and the mkt echo the ladder shows MAN from the send itself (at most 3 s). Foreign orders are
 * own EXT (placed outside the desk); they show amber as EXT.
 */
(function (root) {
  'use strict';
  var Desk = root.Desk = root.Desk || {};
  var hasDoc = typeof document !== 'undefined';
  var U = 10000;
  var MINUS = '\u2212';
  var ROW_PX = 18;
  var SLOTS = ['A', 'B', 'C', 'D'];
  var PRESETS = [1, 5, 10, 25, 50, 100];
  var LAD_TTLS = [5, 15, 60];
  var CAPS = [0, 1, 2];
  var ZOOMS = [1, 5, 10];
  var ARM_MS = 3000;                 // a guarded click or ticket button stays armed this long
  var TERM_SHOW_MS = 3000;           // CXL / REJ / FILL rows stay in WORKING this long
  var LTQ_MS = 3000;                 // a print shows in LTQ this long
  var KEY_HOLD_MS = 10000;           // a keyboard scroll holds off the auto-recentre this long
  var HELD_MS = 3000;                // our send shows the market MAN until the mkt echo, at most this long
  // ticket order types; ttl 'START' = until the game starts
  var TK_TYPES = [
    { id: 'IOC', tif: 'IOC', ttl: null, label: 'IOC' }, { id: 'FOK', tif: 'FOK', ttl: null, label: 'FOK' },
    { id: 'P5', tif: 'POST', ttl: 5, label: '5s' }, { id: 'P15', tif: 'POST', ttl: 15, label: '15s' },
    { id: 'P60', tif: 'POST', ttl: 60, label: '60s' }, { id: 'P300', tif: 'POST', ttl: 300, label: '5m' },
    { id: 'P1800', tif: 'POST', ttl: 1800, label: '30m' }, { id: 'PSTART', tif: 'POST', ttl: 'START', label: 'START' }
  ];
  var KIND_ORDER = { LIP15: 0, GAME: 1, WEATHER: 2, OTHER: 3 };

  // ---------------------------------------------------------------------------------------------
  // Pure logic (no DOM; the node tests call these directly)
  // ---------------------------------------------------------------------------------------------
  function floorIdx(a, p) {
    var lo = 0, hi = a.length - 1, ans = -1;
    while (lo <= hi) { var mid = (lo + hi) >> 1; if (a[mid] <= p) { ans = mid; lo = mid + 1; } else hi = mid - 1; }
    return ans;
  }
  var logic = {
    // the ladder's row prices, ascending: every grid level, or every k-th 0.1c step when zoomed
    rowLevels: function (g, k) {
      if (!k || k <= 1) return g.levels;
      var q = g.minStep * k, out = [];
      for (var i = 0; i < g.levels.length; i++) if (g.levels[i] % q === 0) out.push(g.levels[i]);
      return out.length > 1 ? out : g.levels;
    },
    floorIdx: floorIdx,
    floorRow: function (a, p) { var i = floorIdx(a, p); return i >= 0 ? a[i] : null; },
    ceilRow: function (a, p) { var i = floorIdx(a, p); if (i >= 0 && a[i] === p) return p; return i + 1 < a.length ? a[i + 1] : null; },
    nearestIdx: function (a, p) {
      var i = floorIdx(a, p);
      if (i < 0) return 0;
      if (i + 1 >= a.length) return i;
      return (p - a[i]) <= (a[i + 1] - p) ? i : i + 1;
    },
    // book levels [[px, size]] summed into rows: bids floor (a 0.643 bid shows on 0.64), asks ceil
    bucket: function (levels, asc, up) {
      var out = new Map();
      if (!levels) return out;
      for (var i = 0; i < levels.length; i++) {
        var r = up ? logic.ceilRow(asc, levels[i][0]) : logic.floorRow(asc, levels[i][0]);
        if (r === null) continue;
        out.set(r, Math.round(((out.get(r) || 0) + levels[i][1]) * 100) / 100);
      }
      return out;
    },
    // a ladder click: BUY at p (bid side) or SELL at p (ask side)
    //   away from the touch: POST at p with the expiry; at or through it: IOC at p plus cap ticks
    clickIntent: function (g, bb, ba, side, p, cap, ttl) {
      var G = Desk.grid;
      if (side === 'BUY') {
        if (ba !== null && ba !== undefined && p >= ba) return { tif: 'IOC', px: G.step(g, p, cap || 0), ttl_s: null, touch: ba, take: true };
        return { tif: 'POST', px: p, ttl_s: ttl, touch: bb === undefined ? null : bb, take: false };
      }
      if (bb !== null && bb !== undefined && p <= bb) return { tif: 'IOC', px: G.step(g, p, -(cap || 0)), ttl_s: null, touch: bb, take: true };
      return { tif: 'POST', px: p, ttl_s: ttl, touch: ba === undefined ? null : ba, take: false };
    },
    // hotkey orders: take (IOC at the touch plus cap), join (POST at our side's touch),
    // improve (POST one tick inside, only with a spread of 2+ ticks)
    hotIntent: function (kind, g, bb, ba, side, cap, ttl) {
      var G = Desk.grid, buy = side === 'BUY';
      if (kind === 'take') {
        var t = buy ? ba : bb;
        if (t === null || t === undefined) return { err: buy ? 'NO ASK' : 'NO BID' };
        return { tif: 'IOC', px: G.step(g, t, buy ? (cap || 0) : -(cap || 0)), ttl_s: null, touch: t, take: true };
      }
      if (kind === 'join') {
        var j = buy ? bb : ba;
        if (j === null || j === undefined) return { err: buy ? 'NO BID' : 'NO ASK' };
        return { tif: 'POST', px: j, ttl_s: ttl, touch: j, take: false };
      }
      if (bb === null || ba === null || bb === undefined || ba === undefined) return { err: 'ONE-SIDED BOOK' };
      if (G.ticks(g, bb, ba) < 2) return { err: 'SPREAD 1 TICK' };
      return { tif: 'POST', px: G.step(g, buy ? bb : ba, buy ? 1 : -1), ttl_s: ttl, touch: buy ? bb : ba, take: false };
    },
    // our working cell: '8 12s', '8(2)' when partly filled, shorter when it would not fit 6 chars
    woText: function (left, filled, secs, part) {
      var F = Desk.fmt, s = F.size(left);
      if (part && filled > 0) return s + '(' + F.size(filled) + ')';
      if (secs === null || secs === undefined) return s;
      var t = s + ' ' + secs + 's';
      if (t.length > 6) t = s + ' ' + secs;
      if (t.length > 6) t = s;
      return t;
    },
    // break-even band: neither side pays after the taker fee
    inBand: function (v, fee, fair, p) {
      if (fair === null || fair === undefined) return false;
      return Math.abs(p - fair) <= Desk.fees.takerPer(v, fee, p) + 1e-9;
    },
    // client order ids are prefixed M (manual) or S<lane> (systematic); anything else is foreign
    foreign: function (cid) { return !/^[MS]/.test(String(cid || '')); },
    // an order this desk did not place: own EXT (contract 1.1), or a cid that is neither M nor S
    isExt: function (o) { return !!o && (o.own === 'EXT' || logic.foreign(o.cid)); },
    ttlOK: function (ttl) { var e = Desk.contract && Desk.contract.enums.TTL_S; return !!e && e.indexOf(ttl) >= 0; },
    // START: an expiry at the game's start, a whole second, 5 s to 24 h ahead; null when not possible
    startExp: function (m, nowS) {
      if (!m || !m.start_ms) return null;
      var e = Math.floor(m.start_ms / 1000) * 1000;
      return e - nowS >= 5000 && e - nowS <= 86400000 ? e : null;
    },
    // MOVE for a short Kalshi shard: the shard with the most free cash and the amount to move
    // (the shortfall rounded up to the dollar, at most that shard's free cash); null when none
    moveFor: function (shards, to, need) {
      if (!shards || !to || !(need > 0)) return null;
      var best = null;
      shards.forEach(function (x) { if (x.id !== to && x.free > 0 && (!best || x.free > best.free)) best = x; });
      if (!best) return null;
      var amt = Math.min(Math.ceil(need / U) * U, Math.floor(best.free / 100) * 100);
      return amt > 0 ? { from: best.id, amt: amt, full: amt >= need } : null;
    },
    // a reference value (model, ESPN) on the market's decimals, not snapped to the grid
    fmtRef: function (u, g) { if (u === null || u === undefined || isNaN(u)) return ''; return (u / U).toFixed(g ? g.dp : 2); },
    // the cash an order ties up (reducing orders tie up nothing), its fee and the position after
    costInfo: function (m, pos, side, px, qty, liq) {
      var p = pos ? pos.pos : 0, avg = pos && pos.avg !== null && pos.avg !== undefined ? pos.avg : 0;
      var reserve = side === 'BUY' ? Math.max(0, qty - Math.max(0, -p)) * px : Math.max(0, qty - Math.max(0, p)) * (U - px);
      var fee = Desk.fees.estimate(m.v, m.fee, side, px, qty, liq);
      var np, navg;
      if (side === 'BUY') {
        np = Math.round((p + qty) * 100) / 100;
        if (p >= 0) navg = np ? (avg * p + px * qty) / np : 0;
        else navg = np < 0 ? avg : (np > 0 ? px : 0);
      } else {
        np = Math.round((p - qty) * 100) / 100;
        if (p <= 0) navg = np ? (avg * (-p) + px * qty) / (-np) : 0;
        else navg = np > 0 ? avg : (np < 0 ? px : 0);
      }
      var maxl = np > 0 ? np * navg : (np < 0 ? -np * (U - navg) : 0);
      // the gateway's cash check: the reservation plus the fee as if the order took (contract)
      var need = reserve + Math.max(0, Desk.fees.estimate(m.v, m.fee, side, px, qty, 'T'));
      return { reserve: reserve, fee: fee, cost: reserve + Math.max(0, fee), need: need, pos: np, avg: navg, maxl: maxl };
    },
    // the same outcome on the other venue (game pairs), or null
    pairOf: function (markets, m) {
      if (!m) return null;
      var best = null;
      markets.forEach(function (x) {
        if (best || x.v === m.v || x.og !== m.og || x.oc !== m.oc || x.status === 'SETTLED') return;
        best = x.key;
      });
      return best;
    },
    // the 15-minute series a ticker belongs to ('KXNATGAS15M'), from 'KXNATGAS15M-26OCT061930-30'
    series: function (tk) { var i = String(tk).indexOf('-'); return i > 0 ? tk.slice(0, i) : tk; },
    // a game's group is its event ('NCAAF SOUMIS@TROY', from short 'SOUMIS@TROY SOUMIS'); other
    // markets keep the contract's grp (15-minute series, city-day)
    watchGroup: function (m) {
      if (m.kind === 'GAME') { var t = String(m.short).split(' ')[0]; if (t.indexOf('@') > 0) return m.grp + ' ' + t; }
      return m.grp;
    },
    // the row label inside its group, short enough for the 288 px watchlist: the YES team, the
    // bracket without its city, the 15-minute series without its window; ' P' marks Polymarket
    watchLabel: function (m) {
      var sh = String(m.short), parts = sh.split(' '), out = sh;
      if (m.kind === 'GAME' && parts.length > 1 && parts[0].indexOf('@') > 0) out = parts.slice(1).join(' ');
      else if (m.kind === 'LIP15' && parts.length > 1) out = parts[0];
      else if (m.kind === 'WEATHER' && parts.length > 1 && String(m.grp).split(' ').indexOf(parts[0]) >= 0) out = parts.slice(1).join(' ');
      return out + (m.v === 'P' ? ' P' : '');
    },
    // the watchlist: groups in a fixed kind order (15-minute series, games, weather, other), then
    // by close time; markets inside by close time then listing order. [{grp, n, collapsed}] group
    // rows and [{m}] market rows
    watchList: function (markets, opts) {
      opts = opts || {};
      var q = (opts.q || '').toLowerCase(), venue = opts.venue || 'ALL', groups = new Map(), seq = 0;
      markets.forEach(function (m) {
        if (m.status === 'SETTLED') return;
        if (venue !== 'ALL' && m.v !== venue) return;
        if (q && (m.short + ' ' + m.title + ' ' + m.tk + ' ' + m.grp).toLowerCase().indexOf(q) < 0) return;
        var gk = logic.watchGroup(m), g = groups.get(gk);
        if (!g) { g = { grp: gk, kind: KIND_ORDER[m.kind] === undefined ? 9 : KIND_ORDER[m.kind], close: m.close_ms, list: [] }; groups.set(gk, g); }
        if (m.close_ms < g.close) g.close = m.close_ms;
        g.list.push({ m: m, i: seq++ });
      });
      var gs = Array.from(groups.values()).sort(function (a, b) { return a.kind - b.kind || a.close - b.close || (a.grp < b.grp ? -1 : a.grp > b.grp ? 1 : 0); });
      var out = [];
      gs.forEach(function (g) {
        g.list.sort(function (a, b) { return (a.m.close_ms - b.m.close_ms) || (a.i - b.i); });
        var col = !q && !!(opts.collapsed && opts.collapsed[g.grp]);
        out.push({ grp: g.grp, n: g.list.length, collapsed: col });
        if (!col) g.list.forEach(function (x) { out.push({ m: x.m }); });
      });
      return out;
    },
    // a price typed into the ticket: '0.62', '.625' (dollars) or '62', '62.5' (cents)
    parsePx: function (s) {
      s = String(s || '').trim();
      if (!s || !/^\d*\.?\d*$/.test(s) || s === '.') return null;
      var x = parseFloat(s);
      if (isNaN(x)) return null;
      var u = x < 1 ? Math.round(x * U) : Math.round(x * 100);
      return u > 0 && u < U ? u : null;
    },
    parseQty: function (s, v) {
      var x = parseFloat(String(s || '').trim());
      if (isNaN(x) || x < 0) return 0;
      return v === 'P' ? Math.floor(x) : Math.round(x * 100) / 100;
    },
    // signed cents with two decimals: fee per contract 'T1.75'
    c2: function (u) { if (u === null || u === undefined || isNaN(u)) return ''; var s = Math.abs(u / 100).toFixed(2); return (u < 0 && +s !== 0 ? MINUS : '') + s; }
  };

  // ---------------------------------------------------------------------------------------------
  // Per-viewer settings (Desk.storage: a convenience; the page works without it)
  // ---------------------------------------------------------------------------------------------
  var cfg = null;
  function defaults() {
    var lad = {};
    SLOTS.forEach(function (s) { lad[s] = { qty: 1, ttl: 15, zoom: 1 }; });
    return { cap: 1, lad: lad, tk: { qty: 1, type: 'P15', side: 'BUY' }, watch: { venue: 'ALL', collapsed: {} } };
  }
  function loadCfg() {
    var c = defaults(), s = Desk.storage ? Desk.storage.getJSON('manual', null) : null;
    if (!s || typeof s !== 'object') return c;
    if (CAPS.indexOf(s.cap) >= 0) c.cap = s.cap;
    SLOTS.forEach(function (k) {
      var x = s.lad && s.lad[k];
      if (!x) return;
      if (typeof x.qty === 'number' && x.qty >= 0 && x.qty < 1e6) c.lad[k].qty = x.qty;
      if (LAD_TTLS.indexOf(x.ttl) >= 0) c.lad[k].ttl = x.ttl;
      if (ZOOMS.indexOf(x.zoom) >= 0) c.lad[k].zoom = x.zoom;
    });
    if (s.tk) {
      if (typeof s.tk.qty === 'number' && s.tk.qty >= 0 && s.tk.qty < 1e6) c.tk.qty = s.tk.qty;
      if (TK_TYPES.some(function (t) { return t.id === s.tk.type; })) c.tk.type = s.tk.type;
      if (s.tk.side === 'BUY' || s.tk.side === 'SELL') c.tk.side = s.tk.side;
    }
    if (s.watch) {
      if (s.watch.venue === 'K' || s.watch.venue === 'P' || s.watch.venue === 'ALL') c.watch.venue = s.watch.venue;
      if (s.watch.collapsed && typeof s.watch.collapsed === 'object') Object.keys(s.watch.collapsed).forEach(function (g) { if (s.watch.collapsed[g]) c.watch.collapsed[g] = true; });
    }
    return c;
  }
  function saveCfg() {
    if (!cfg || !Desk.storage) return;
    Desk.storage.setJSON('manual', cfg);
  }

  // ---------------------------------------------------------------------------------------------
  // Runtime state
  // ---------------------------------------------------------------------------------------------
  var LAD = {};                      // slot -> ladder runtime
  var TK = { key: null, px: null, pxBad: false, arm: null, el: {} };
  var W = { cursor: null, lastFocus: null, keys: [], q: '', lastAt: 0, dirty: false, scroll: false, el: {} };
  var GR = { work: null, fills: null, pos: null, posAt: 0, posDirty: false };
  var sentAt = new Map();            // key -> our last send there (MAN shown until the mkt echo)
  var clickAt = new Map();           // cid -> the row clicked, for a pending IOC (its limit includes the cap)
  var vap = new Map();               // key -> Map px -> contracts printed since load (dim LTQ)
  var seenPrint = typeof WeakSet !== 'undefined' ? new WeakSet() : null;

  function S() { return Desk.store; }
  function $(id) { return hasDoc ? document.getElementById(id) : null; }
  function el(tag, cls, text, attrs) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined && text !== null) e.textContent = text;
    if (attrs) for (var k in attrs) e.setAttribute(k, attrs[k]);
    return e;
  }
  function btn(text, cls, attrs) { var b = el('button', cls, text, attrs); b.setAttribute('type', 'button'); return b; }
  function add(parent) { for (var i = 1; i < arguments.length; i++) if (arguments[i]) parent.appendChild(arguments[i]); return parent; }
  function mark(t) { Desk.render.mark(t); }
  function setCls(e, c) { if (e && e.className !== c) e.className = c; }
  function setDisabled(b, off) { if (b && b.disabled !== !!off) b.disabled = !!off; }
  function setVal(input, v) { if (input && document.activeElement !== input && input.value !== v) input.value = v; }

  function blockReason(m) {
    if (!m) return 'NO MARKET';
    var w = S().canTrade(m.v);
    if (w) return w;
    if (m.status !== 'OPEN') return m.status;
    return '';
  }
  function qtyReason(m, q) {
    if (!(q > 0)) return 'QTY 0';
    if (m && m.v === 'P' && q % 1 !== 0) return 'QTY WHOLE ON PM';
    return '';
  }
  function alertWarn(msg) { S().localAlert('WARN', 'MAN', msg); }
  // held by Manual: the backend's mkt.man, or our own send in the last HELD_MS
  function held(key) {
    var m = S().market(key);
    if (m && m.man) return true;
    var t = sentAt.get(key);
    return !!t && Date.now() - t < HELD_MS && !(m && m.status !== 'OPEN');
  }
  function sysThere(key) {
    var on = false;
    S().orders.forEach(function (o) { if (!on && o.key === key && !o.terminal && o.own === 'SYS') on = true; });
    var P = S().position(key);
    return on || !!(P && P.pos && (P.own === 'SYS' || P.own === 'MIX'));
  }

  // ---------------------------------------------------------------------------------------------
  // Sending (every path ends here; Desk.act paints PEND synchronously, then posts)
  // ---------------------------------------------------------------------------------------------
  function markTaken(key) {
    sentAt.set(key, Date.now()); mark('mlad');
    if (sentAt.size > 200) sentAt.delete(sentAt.keys().next().value);
  }
  function send(key, m, side, it, qty, override, ev, clickRow) {
    var cid = Desk.act.order({ v: m.v, tk: m.tk, side: side, px: it.px, qty: qty, tif: it.tif, ttl_s: it.tif === 'POST' && !it.exp_ms ? it.ttl_s : null,
                               exp_ms: it.tif === 'POST' && it.exp_ms ? it.exp_ms : null, reduce_only: !!it.reduce_only,
                               override: !!override, touch: it.touch === undefined ? null : it.touch }, ev);
    if (cid) {
      if (clickRow !== undefined && clickRow !== null && it.tif !== 'POST' && clickRow !== it.px) {
        clickAt.set(cid, clickRow);
        if (clickAt.size > 200) clickAt.delete(clickAt.keys().next().value);
      }
      markTaken(key);
    }
    return cid;
  }
  function release(key) {
    var m = S().market(key); if (!m) return;
    Desk.act.release(m.v, m.tk).then(function (r) {
      if (r && r.json && r.json.ok) { sentAt.delete(key); mark('mlad'); }
    });
  }
  function flatten(key, ev) {
    var st = S(), P = st.position(key), m = st.market(key), b = st.book(key);
    if (!P || !P.pos || !m) return null;
    var why = blockReason(m);
    if (why) { alertWarn('FLAT ' + m.short + ': ' + why); return null; }
    var side = P.pos > 0 ? 'SELL' : 'BUY';
    var it = logic.hotIntent('take', m.g, b ? b.bb : null, b ? b.ba : null, side, cfg.cap, null);
    if (it.err) { alertWarn('FLAT ' + m.short + ': ' + it.err); return null; }
    it.reduce_only = true;                   // IOC with reduce_only: it can never flip the position
    return send(key, m, side, it, Math.abs(P.pos), false, ev, it.touch);
  }

  // ---------------------------------------------------------------------------------------------
  // Ladders
  // ---------------------------------------------------------------------------------------------
  function buildLadder(slot) {
    var id = 'm-lad-' + slot.toLowerCase(), panel = $(id);
    if (!panel) return;
    var L = LAD[slot] = {
      slot: slot, el: panel, cfg: cfg.lad[slot],
      head: $(id + '-head'), cols: $(id + '-cols'), body: $(id + '-body'), foot: $(id + '-foot'),
      tb: null, h: {}, f: {}, rows: 38, top: 0, asc: null, rowsKey: null, rowsG: null,
      shownKey: undefined, needCenter: true, over: false, hold: 0, arm: null, drag: null, dragMovedAt: 0,
      wacc: 0, dirty: true
    };
    // header: three 20 px lines
    var H = L.h;
    L.head.textContent = '';
    var l1 = el('div', 'ln t'), l2 = el('div', 'ln'), l3 = el('div', 'ln');
    H.vt = el('span', 'tag vt'); H.ttl = el('span', 'ttl'); H.clk = el('span', 'clk n'); H.src = el('span', 'src');
    H.own = el('span', 'tag own'); H.rel = btn('REL', 'rel', { 'data-a': 'rel' }); H.ext = el('span', 'tag warn ext', 'EXT');
    H.blk = el('span', 'tag crit blk'); H.zm = btn('x1', 'zm', { 'data-a': 'zoom' });
    add(l1, H.vt, H.ttl, H.clk, H.src, H.own, H.rel, H.ext, H.blk, H.zm);
    H.posL = el('span', 'lb', 'POS'); H.pos = el('span', 'n v'); H.unrlL = el('span', 'lb', 'UNRL'); H.unrl = el('span', 'n v');
    H.shL = el('span', 'lb'); H.sh = el('span', 'n v'); H.feeL = el('span', 'lb', 'FEE'); H.fee = el('span', 'n v');
    add(l2, H.posL, H.pos, H.unrlL, H.unrl, H.shL, H.sh, H.feeL, H.fee);
    H.midL = el('span', 'lb', 'MID'); H.mid = el('span', 'n v'); H.mdlL = el('span', 'lb', 'MODEL'); H.mdl = el('span', 'n fair');
    H.espnL = el('span', 'lb', 'ESPN'); H.espn = el('span', 'n espn'); H.chg = el('span', 'n chg');
    add(l3, H.midL, H.mid, H.mdlL, H.mdl, H.espnL, H.espn, H.chg);
    add(L.head, l1, l2, l3);
    L.head.addEventListener('click', function (ev) {
      var b = ev.target && ev.target.closest ? ev.target.closest('button') : null;
      if (!b) return;
      var a = b.getAttribute('data-a');
      if (a === 'rel' && L.shownKey) release(L.shownKey);
      else if (a === 'zoom') {
        var i = ZOOMS.indexOf(L.cfg.zoom);
        L.cfg.zoom = ZOOMS[(i + 1) % ZOOMS.length]; L.rowsKey = null; L.needCenter = true; L.tb.textContent = ''; saveCfg(); dirtyLad(L);
      }
    });
    // column header
    var colgroup = function () {
      var cg = el('colgroup');
      ['wb', 'bq', 'rf', 'px', 'aq', 'ws', 'lt'].forEach(function (c) { cg.appendChild(el('col', c)); });
      return cg;
    };
    var ht = el('table', 'lad'), thead = el('thead'), tr = el('tr');
    [['WRK B', 'wb'], ['BID', 'bq'], ['REF', 'rf'], ['PRICE', 'px'], ['ASK', 'aq'], ['WRK S', 'ws'], ['LTQ', 'lt']].forEach(function (c) { tr.appendChild(el('th', c[1], c[0])); });
    thead.appendChild(tr); add(ht, colgroup(), thead);
    L.cols.textContent = ''; L.cols.appendChild(ht);
    // rows
    var t = el('table', 'lad'); L.tb = el('tbody');
    add(t, colgroup(), L.tb);
    L.body.textContent = ''; L.body.appendChild(t);
    // footer: presets, qty, CLR / expiry, cap, DEL ALL, HK / read-only line
    var Fo = L.f;
    L.foot.textContent = '';
    var f1 = el('div', 'ln'), f2 = el('div', 'ln');
    Fo.q = PRESETS.map(function (q) { return btn(String(q), 'q', { 'data-a': 'q', 'data-v': String(q) }); });
    Fo.qty = el('input', 'qty n', null, { type: 'text', inputmode: 'decimal', autocomplete: 'off', spellcheck: 'false' });
    Fo.clr = btn('CLR', 'clr', { 'data-a': 'clr' });
    Fo.q.forEach(function (b) { f1.appendChild(b); }); add(f1, Fo.qty, Fo.clr);
    Fo.ttl = LAD_TTLS.map(function (s) { return btn(s + 's', 'tt', { 'data-a': 'ttl', 'data-v': String(s) }); });
    Fo.capL = el('span', 'lb', 'CAP');
    Fo.cap = CAPS.map(function (c) { return btn(String(c), 'cp', { 'data-a': 'cap', 'data-v': String(c) }); });
    Fo.del = btn('DEL ALL', 'del', { 'data-a': 'del' });
    Fo.hk = btn('HK OFF', 'hk', { 'data-a': 'hk' });
    Fo.ttl.forEach(function (b) { f2.appendChild(b); }); f2.appendChild(Fo.capL); Fo.cap.forEach(function (b) { f2.appendChild(b); }); add(f2, Fo.del, Fo.hk);
    Fo.ro = el('div', 'ro-msg', 'READ-ONLY: PM key');
    add(L.foot, f1, f2, Fo.ro);
    L.foot.addEventListener('click', function (ev) { onLadFoot(L, ev); });
    Fo.qty.addEventListener('input', function () { var m = S().market(L.shownKey); L.cfg.qty = logic.parseQty(Fo.qty.value, m ? m.v : 'K'); saveCfg(); dirtyLad(L); });
    Fo.qty.addEventListener('keydown', function (ev) { if (ev.key === 'Enter') { Fo.qty.blur(); L.el.focus(); } });
    // pointer
    L.body.addEventListener('click', function (ev) { onLadClick(L, ev); });
    L.body.addEventListener('contextmenu', function (ev) { onLadContext(L, ev); });
    L.body.addEventListener('mousedown', function (ev) { onLadDown(L, ev); });
    L.body.addEventListener('mousemove', function (ev) { onLadMove(L, ev); });
    L.body.addEventListener('wheel', function (ev) { onLadWheel(L, ev); }, { passive: false });
    panel.addEventListener('mouseenter', function () { L.over = true; });
    panel.addEventListener('mouseleave', function () { L.over = false; dirtyLad(L); });
    // rows on screen follow the panel's height
    if (root.ResizeObserver) {
      try {
        new root.ResizeObserver(function (entries) {
          var h = entries && entries[0] && entries[0].contentRect ? entries[0].contentRect.height : 0;
          if (h > 0) { var n = Math.max(5, Math.ceil(h / ROW_PX)); if (n !== L.rows) { L.rows = n; dirtyLad(L); } }
        }).observe(L.body);
        L.ro = true;
      } catch (e) { L.ro = false; }
    }
  }
  function dirtyLad(L) { L.dirty = true; mark('mlad'); }
  function dirtyAllLads() { SLOTS.forEach(function (s) { if (LAD[s]) LAD[s].dirty = true; }); mark('mlad'); mark('mtk'); }
  function measureLad(L) {
    if (L.ro || !L.body) return;
    var h = L.body.clientHeight;
    if (h > 0) L.rows = Math.max(5, Math.ceil(h / ROW_PX));
  }

  function cellOf(ev) {
    var t = ev.target;
    if (!t || !t.closest) return null;
    var td = t.closest('td');
    if (!td) return null;
    var tr = td.parentNode, k = tr && tr.getAttribute ? tr.getAttribute('data-k') : null;
    if (k === null || k === undefined || k === '') return null;
    return { td: td, col: String(td.className).split(' ')[0], px: +k };
  }
  function ladMarket(L) {
    var key = L.shownKey;
    return { key: key, m: key ? S().market(key) : null, b: key ? S().book(key) : null };
  }
  function refuse(L, td, why) {
    if (td) Desk.dom.flash(td, 'warn');
    alertWarn(why);
    if (L) dirtyLad(L);
  }

  function onLadClick(L, ev) {
    if (ev.button) return;
    var c = cellOf(ev);
    if (!c) return;
    if (Date.now() - L.dragMovedAt < 100) return;      // the click that ends a drag is not an order
    if (c.col === 'px') { ticketPrice(L.shownKey, c.px); return; }
    if (c.col === 'bq' || c.col === 'wb') ladTrade(L, 'BUY', c, ev);
    else if (c.col === 'aq' || c.col === 'ws') ladTrade(L, 'SELL', c, ev);
  }
  function ladTrade(L, side, c, ev) {
    var x = ladMarket(L), m = x.m, b = x.b;
    if (!m) return null;
    var why = blockReason(m) || qtyReason(m, L.cfg.qty);
    if (why) { refuse(L, c.td, why); return null; }
    var it = logic.clickIntent(m.g, b ? b.bb : null, b ? b.ba : null, side, c.px, cfg.cap, L.cfg.ttl);
    var now = Date.now(), override = false;
    var guard = S().priceGuard(x.key, side, it.px);
    if (guard) {
      var a = L.arm;
      if (ev.shiftKey || (a && a.side === side && a.row === c.px && a.key === x.key && a.until > now)) override = true;
      else { L.arm = { side: side, row: c.px, key: x.key, until: now + ARM_MS }; refuse(L, c.td, guard + ': click again to override'); return null; }
    }
    L.arm = null;
    return send(x.key, m, side, it, L.cfg.qty, override, ev, c.px);
  }
  // cids of our orders shown on a row (manual first; a SYS-only level cancels the SYS orders)
  function levelCids(L, side, px) {
    var e = ordersAt(L.shownKey, L.asc || [])[side].get(px);
    if (!e) return [];
    return e.cids.length ? e.cids : e.sysCids;
  }
  function onLadContext(L, ev) {
    if (ev.preventDefault) ev.preventDefault();
    var c = cellOf(ev);
    if (!c) return;
    var side = (c.col === 'wb' || c.col === 'bq') ? 'BUY' : ((c.col === 'ws' || c.col === 'aq') ? 'SELL' : null);
    if (!side) return;
    var cids = levelCids(L, side, c.px);
    if (cids.length) { Desk.act.cancel(cids); dirtyLad(L); }
  }
  function onLadDown(L, ev) {
    if (ev.button !== 0) return;
    var c = cellOf(ev);
    if (!c || (c.col !== 'wb' && c.col !== 'ws')) return;
    var side = c.col === 'wb' ? 'BUY' : 'SELL';
    var e = ordersAt(L.shownKey, L.asc || [])[side].get(c.px);
    if (!e || !e.cids.length || e.pend || e.cxl) return;
    L.drag = { side: side, from: c.px, over: c.px, cids: e.cids.slice(), qty: e.man };
  }
  function onLadMove(L, ev) {
    if (!L.drag) return;
    var c = cellOf(ev);
    if (c && c.px !== L.drag.over) { L.drag.over = c.px; L.drag.moved = true; dirtyLad(L); }
  }
  function onDocUp(ev) {
    SLOTS.forEach(function (s) {
      var L = LAD[s];
      if (!L || !L.drag) return;
      var dr = L.drag;
      L.drag = null; dirtyLad(L);
      if (!L.body.contains(ev.target)) return;
      var c = cellOf(ev);
      // a drag that moved never doubles as a click, even when it comes back to its own row
      if (dr.moved) L.dragMovedAt = Date.now();
      if (!c || c.px === dr.from) return;
      reprice(L, dr, c.px, ev);
    });
  }
  // plan: no price amends in the first live weeks; a re-price is a cancel plus a new post-only
  function reprice(L, dr, px, ev) {
    var x = ladMarket(L), m = x.m, b = x.b;
    var why = blockReason(m);
    if (why) { refuse(L, null, 'REPRICE ' + why); return null; }
    if (b && ((dr.side === 'BUY' && b.ba !== null && px >= b.ba) || (dr.side === 'SELL' && b.bb !== null && px <= b.bb))) {
      refuse(L, null, 'REPRICE ' + Desk.fmt.pxg(px, m.g) + ' would cross'); return null;
    }
    Desk.act.cancel(dr.cids);
    var cid = send(x.key, m, dr.side, { tif: 'POST', px: px, ttl_s: L.cfg.ttl, touch: b ? (dr.side === 'BUY' ? b.bb : b.ba) : null }, dr.qty, false, ev);
    if (cid) S().localAlert('INFO', 'MAN', 'REPRICE ' + dr.side + ' ' + Desk.fmt.size(dr.qty) + ' ' + m.short + ' ' + Desk.fmt.pxg(dr.from, m.g) + ' to ' + Desk.fmt.pxg(px, m.g) + ': queue place lost');
    return cid;
  }
  function onLadWheel(L, ev) {
    if (ev.preventDefault) ev.preventDefault();
    var n = 0;
    var wd = ev.wheelDeltaY;
    if (typeof wd === 'number' && wd !== 0 && Math.abs(wd) % 120 === 0) n = -3 * (wd / 120);   // a mouse wheel notch: 3 rows
    else {
      var dy = ev.deltaY * (ev.deltaMode === 1 ? ROW_PX : (ev.deltaMode === 2 ? L.rows * ROW_PX : 1));
      L.wacc += dy;
      n = L.wacc > 0 ? Math.floor(L.wacc / ROW_PX) : -Math.floor(-L.wacc / ROW_PX);
      L.wacc -= n * ROW_PX;
    }
    if (n) scrollLad(L, n);
  }
  // byKey: arrows / PgUp / PgDn move the axis while the pointer may be elsewhere, so they hold
  // off the auto-recentre for a while (the wheel needs no hold: the pointer is over the ladder)
  function scrollLad(L, n, byKey) {
    if (!L.asc) return;
    var max = Math.max(0, L.asc.length - L.rows);
    var t = Math.max(0, Math.min(max, L.top + n));
    if (byKey) L.hold = Date.now() + KEY_HOLD_MS;
    if (t !== L.top) { L.top = t; dirtyLad(L); }
  }
  function centerPx(b, m) {
    if (b) {
      if (b.bb !== null && b.ba !== null) return (b.bb + b.ba) / 2;
      if (b.bb !== null) return b.bb;
      if (b.ba !== null) return b.ba;
      if (b.last) return b.last.px;
      if (b.ref && b.ref.fair !== null) return b.ref.fair;
    }
    return m && m.g.levels.length ? m.g.levels[m.g.levels.length >> 1] : U / 2;
  }
  function centerTop(L, asc, p) {
    var n = asc.length, i = logic.nearestIdx(asc, p);
    var top = (n - 1 - i) - Math.floor(L.rows / 2);
    return Math.max(0, Math.min(Math.max(0, n - L.rows), top));
  }
  function recentre(L) { L.needCenter = true; L.hold = 0; dirtyLad(L); }

  function onLadFoot(L, ev) {
    var b = ev.target && ev.target.closest ? ev.target.closest('button') : null;
    if (!b) return;
    var a = b.getAttribute('data-a'), v = +b.getAttribute('data-v');
    if (a === 'q') L.cfg.qty = ev.shiftKey ? Math.round((L.cfg.qty + v) * 100) / 100 : v;
    else if (a === 'clr') L.cfg.qty = 0;
    else if (a === 'ttl') L.cfg.ttl = v;
    else if (a === 'cap') { cfg.cap = v; dirtyAllLads(); }
    else if (a === 'del') { var m = S().market(L.shownKey); if (m) Desk.act.cancelWhere({ v: m.v, tk: m.tk, own: 'MAN' }); }
    else if (a === 'hk') Desk.keys.setHK(!Desk.keys.hk);
    else return;
    saveCfg(); dirtyLad(L);
  }

  // our orders on a market grouped into ladder rows: {BUY: Map row -> agg, SELL: Map row -> agg}
  function ordersAt(key, asc) {
    var r = { BUY: new Map(), SELL: new Map() };
    if (!key || !asc || !asc.length) return r;
    S().orders.forEach(function (o) {
      if (o.key !== key || o.terminal) return;
      var p = o.px;
      if (o.st === 'PEND' && o.tif !== 'POST' && clickAt.has(o.cid)) p = clickAt.get(o.cid);
      var row = o.side === 'BUY' ? logic.floorRow(asc, p) : logic.ceilRow(asc, p);
      if (row === null) return;
      var e = r[o.side].get(row);
      if (!e) { e = { man: 0, sys: 0, filled: 0, pend: false, part: false, nMan: 0, nCxl: 0, cxl: false, exp: null, ext: false, cids: [], sysCids: [], piq: null }; r[o.side].set(row, e); }
      if (o.own === 'SYS' && !logic.isExt(o)) { e.sys = Math.round((e.sys + o.left) * 100) / 100; e.sysCids.push(o.cid); return; }
      e.man = Math.round((e.man + o.left) * 100) / 100; e.filled = Math.round((e.filled + o.filled) * 100) / 100; e.nMan++; e.cids.push(o.cid);
      if (o.st === 'PEND') e.pend = true;
      if (o.st === 'PART') e.part = true;
      if (o.local && o.local.cxlPend) e.nCxl++;
      if (o.exp_ms && (e.exp === null || o.exp_ms < e.exp)) e.exp = o.exp_ms;
      if (o.piq !== null && o.piq !== undefined && (e.piq === null || o.piq < e.piq)) e.piq = o.piq;
      if (logic.isExt(o)) e.ext = true;
    });
    r.BUY.forEach(function (e) { e.cxl = e.nMan > 0 && e.nCxl === e.nMan; });
    r.SELL.forEach(function (e) { e.cxl = e.nMan > 0 && e.nCxl === e.nMan; });
    return r;
  }

  function renderLadders(d) {
    var vis = Desk.layout.ladders(), now = Date.now();
    for (var i = 0; i < SLOTS.length; i++) {
      var L = LAD[SLOTS[i]];
      if (!L || vis.indexOf(L.slot) < 0) continue;
      if (d.all || d.has('layout')) measureLad(L);
      renderLadder(L, d, now);
    }
  }
  function renderLadder(L, d, now) {
    var st = S(), key = Desk.focus.ladders[L.slot] || null;
    var changed = key !== L.shownKey;
    var hit = d.all || changed || L.dirty || d.has('clock') || d.has('focus') || d.has('layout') || d.has('hdr') || d.has('stale') ||
      d.has('hk') || d.has('pos') || d.has('ui') || (key !== null && (d.has('book:' + key) || d.has('ord:' + key) || d.has('mkt:' + key)));
    if (!hit) return;
    L.dirty = false;
    // a new market starts from fresh rows, so a reused price row never flashes the old sizes
    if (changed) { L.shownKey = key; L.arm = null; L.drag = null; L.needCenter = true; L.hold = 0; L.rowsKey = null; L.tb.textContent = ''; }
    var m = key ? st.market(key) : null, b = key ? st.book(key) : null;
    Desk.dom.setAttr(L.el, 'data-v', m ? m.v : null);
    Desk.dom.setAttr(L.el, 'data-ro', m && m.v === 'P' && st.hdr && st.hdr.cash.P.ro ? '1' : null);
    renderLadHead(L, key, m, b, now);
    renderLadFoot(L);
    if (!m) { L.asc = null; Desk.dom.keyedRows(L.tb, [], String, function () { return []; }); return; }
    var k = m.g.minStep < 100 ? L.cfg.zoom : 1;
    if (L.rowsKey !== key + '|' + k || L.rowsG !== m.g) {
      if (L.rowsKey !== null) L.tb.textContent = '';
      L.asc = logic.rowLevels(m.g, k); L.rowsKey = key + '|' + k; L.rowsG = m.g; L.needCenter = true;
    }
    var asc = L.asc, n = asc.length, rows = Math.min(L.rows, n);
    var bb = b ? b.bb : null, ba = b ? b.ba : null;
    if (L.needCenter && (b || changed)) { L.top = centerTop(L, asc, centerPx(b, m)); if (b) L.needCenter = false; }
    else if (!L.over && !L.drag && !(L.hold > now) && b && (bb !== null || ba !== null)) {
      var hiP = asc[Math.max(0, n - 1 - L.top)], loP = asc[Math.max(0, n - L.top - rows)];
      var out = (bb !== null && (bb < loP || bb > hiP)) || (ba !== null && (ba < loP || ba > hiP));
      if (out) L.top = centerTop(L, asc, centerPx(b, m));
    }
    L.top = Math.max(0, Math.min(Math.max(0, n - rows), L.top));
    var items = [];
    for (var i = 0; i < rows; i++) { var idx = n - 1 - (L.top + i); if (idx < 0) break; items.push(asc[idx]); }
    // what each row shows
    var bids = logic.bucket(b ? b.bids : null, asc, false), asks = logic.bucket(b ? b.asks : null, asc, true);
    var maxSz = 0;
    items.forEach(function (p) { var x = bids.get(p) || 0, y = asks.get(p) || 0; if (x > maxSz) maxSz = x; if (y > maxSz) maxSz = y; });
    var wo = ordersAt(key, asc), nowS = now + (st.srvOffset || 0);
    var bbRow = bb === null ? null : logic.floorRow(asc, bb), baRow = ba === null ? null : logic.ceilRow(asc, ba);
    var lastRow = b && b.last ? asc[logic.nearestIdx(asc, b.last.px)] : null;
    var mk = new Map();
    var addMk = function (u, ch) { if (u === null || u === undefined) return; var r = asc[logic.nearestIdx(asc, u)]; var s = mk.get(r) || ''; if (s.length < 2) mk.set(r, s + ch); };
    var fair = b ? b.ref.fair : null, P = st.position(key);
    addMk(fair, 'F'); addMk(b ? b.ref.espn : null, 'E'); if (P && P.pos && P.avg !== null) addMk(P.avg, 'A');
    var lt = new Map(), cut = now - LTQ_MS;
    if (b) b.prints.forEach(function (pr) { if (pr.at >= cut) { var r = asc[logic.nearestIdx(asc, pr.px)]; lt.set(r, pr.qty); } });
    var vp = vap.get(key), vpr = null;
    if (vp && vp.size) { vpr = new Map(); vp.forEach(function (q, p) { var r = asc[logic.nearestIdx(asc, p)]; vpr.set(r, Math.round(((vpr.get(r) || 0) + q) * 100) / 100); }); }
    var arm = L.arm && L.arm.until > now && L.arm.key === key ? L.arm : null;
    if (L.arm && !arm) L.arm = null;
    var drag = L.drag;
    var F = Desk.fmt, g = m.g;
    var woCell = function (e, side, p) {
      var base = side === 'BUY' ? 'wb' : 'ws';
      var dragCls = drag && drag.side === side && drag.over === p && drag.over !== drag.from ? ' drag' : '';
      if (e && e.nMan) {
        var secs = e.exp ? Math.max(0, Math.ceil((e.exp - nowS) / 1000)) : null;
        return { t: e.pend && !e.man ? '' : logic.woText(e.man, e.filled, e.pend ? null : secs, e.part), cls: base + ' my' + (e.pend ? ' pend' : '') + (e.cxl ? ' cxlpend' : '') + (e.ext ? ' ext' : '') + dragCls,
                 title: e.cids.join(' ') + (e.piq !== null ? ' piq ' + F.size(e.piq) : '') };
      }
      if (e && e.sys) return { t: 'S' + F.size(e.sys), cls: base + ' sys' + dragCls, title: e.sysCids.join(' ') };
      return { t: '', cls: base + dragCls, title: null };
    };
    Desk.dom.keyedRows(L.tb, items, String, function (p) {
      var bs = bids.get(p), as = asks.get(p), mks = mk.get(p) || '';
      var ltq = lt.get(p), vq = vpr ? vpr.get(p) : undefined;
      return [
        woCell(wo.BUY.get(p), 'BUY', p),
        { t: bs ? F.size(bs) : '', cls: 'bq' + (bs ? ' has' : '') + (arm && arm.side === 'BUY' && arm.row === p ? ' arm' : ''), fl: 's', style: { '--d': bs && maxSz ? (bs / maxSz).toFixed(2) : '0' } },
        { t: mks, cls: 'rf' + (mks ? (mks.charAt(0) === 'F' ? ' mk-f' : mks.charAt(0) === 'E' ? ' mk-e' : ' mk-a') : '') },
        { t: F.pxg(p, g), cls: 'px' + (p === bbRow ? ' bb' : '') + (p === baRow ? ' ba' : '') + (p === lastRow ? ' last' : '') },
        { t: as ? F.size(as) : '', cls: 'aq' + (as ? ' has' : '') + (arm && arm.side === 'SELL' && arm.row === p ? ' arm' : ''), fl: 's', style: { '--d': as && maxSz ? (as / maxSz).toFixed(2) : '0' } },
        woCell(wo.SELL.get(p), 'SELL', p),
        ltq !== undefined ? { t: F.size(ltq), cls: 'lt', fl: 's' } : { t: vq ? F.size(vq) : '', cls: 'lt dim' }
      ];
    }, function (p) {
      var inside = bbRow !== null && baRow !== null && p > bbRow && p < baRow;
      var be = logic.inBand(m.v, m.fee, fair, p);
      return (inside ? 'inside' : '') + (be ? (inside ? ' be' : 'be') : '');
    });
  }

  function renderLadHead(L, key, m, b, now) {
    var H = L.h, D = Desk.dom, F = Desk.fmt, st = S();
    if (!m) {
      D.setText(H.vt, ''); D.setText(H.ttl, key ? st.split(key).tk : 'none'); D.setAttr(H.ttl, 'title', key || null);
      [H.clk, H.src, H.own, H.blk, H.pos, H.unrl, H.shL, H.sh, H.fee, H.mid, H.mdl, H.espn, H.chg].forEach(function (x) { D.setText(x, ''); });
      D.setClass(H.rel, 'off', true); D.setClass(H.ext, 'off', true); D.setClass(H.zm, 'off', true);
      return;
    }
    var nowS = now + (st.srvOffset || 0), g = m.g, game = b && b.ref ? b.ref.game : null;
    D.setText(H.vt, m.v);
    D.setText(H.ttl, m.short); D.setAttr(H.ttl, 'title', m.title + ' ' + m.tk);
    var clk;
    if (m.status !== 'OPEN') clk = m.status + (m.result ? ' ' + m.result : '');
    else if (game) clk = game.per === 'PRE' ? 'PRE ' + game.clock : game.per + (game.per === 'FINAL' ? '' : ' ' + game.clock) + ' ' + game.score;
    else clk = F.left(m.close_ms - nowS);
    D.setText(H.clk, clk);
    D.setText(H.src, game ? game.src : '');
    var isHeld = held(key), own = isHeld ? 'MAN' : (sysThere(key) ? 'SYS' : '');
    D.setText(H.own, own); setCls(H.own, 'tag own' + (own === 'SYS' ? ' dim' : ''));
    D.setClass(H.rel, 'off', !isHeld);
    var ext = false;
    st.orders.forEach(function (o) { if (!ext && o.key === key && !o.terminal && logic.isExt(o)) ext = true; });
    D.setClass(H.ext, 'off', !ext);
    var why = blockReason(m);
    D.setText(H.blk, why && why !== m.status ? why : '');
    D.setClass(H.zm, 'off', !(g.minStep < 100));
    D.setText(H.zm, 'x' + L.cfg.zoom);
    // line 2: position, P&L, shard cash, fees per contract at the mid
    var P = st.position(key);
    D.setText(H.pos, P && P.pos ? F.ssize(P.pos) + '@' + F.pxg(P.avg, g) : '0');
    D.setText(H.unrl, P ? F.pnl(P.unrl) : ''); setCls(H.unrl, 'n v ' + (P ? F.sign(P.unrl) : ''));
    if (m.v === 'K') {
      var sh = st.hdr && m.shard ? st.hdr.cash.K.shards.filter(function (s) { return s.id === m.shard; })[0] : null;
      D.setText(H.shL, m.shard || 'K'); D.setText(H.sh, sh ? F.money(sh.free) : '');
    } else { D.setText(H.shL, 'PM'); D.setText(H.sh, st.hdr ? (st.hdr.cash.P.ro ? 'RO' : F.money(st.hdr.cash.P.free)) : ''); }
    var mid = b && b.mid !== null ? b.mid : null, at = mid !== null ? mid : (b && (b.bb !== null || b.ba !== null) ? (b.bb !== null ? b.bb : b.ba) : null);
    if (at !== null) {
      var t = Desk.fees.takerPer(m.v, m.fee, at), mk = Desk.fees.exact(m.v, m.fee, at, 1, 'M') * U;
      D.setText(H.fee, 'T' + logic.c2(t) + ' M' + logic.c2(mk));
    } else D.setText(H.fee, '');
    // line 3: mid, model, ESPN with its age, last change
    D.setText(H.mid, mid !== null ? (mid / U).toFixed(g.dp + 1) : '');
    var ref = b ? b.ref : null;
    D.setText(H.mdl, ref && ref.fair !== null ? logic.fmtRef(ref.fair, g) + (ref.sd !== null ? '\u00b1' + (ref.sd / U).toFixed(2) : '') : '');
    D.setText(H.espn, ref && ref.espn !== null ? logic.fmtRef(ref.espn, g) + (ref.espn_ts_ms ? ' ' + F.age(nowS - ref.espn_ts_ms) : '') : '');
    D.setText(H.chg, b && b.ex_ms ? 'chg ' + F.dur(Math.max(0, nowS - b.ex_ms)) : '');
    [[H.unrlL, H.unrl], [H.feeL, H.fee], [H.midL, H.mid], [H.mdlL, H.mdl], [H.espnL, H.espn]].forEach(function (x) { D.setClass(x[0], 'off', x[1].textContent === ''); });
  }
  function renderLadFoot(L) {
    var Fo = L.f, D = Desk.dom, c = L.cfg;
    Fo.q.forEach(function (b, i) { D.setClass(b, 'on', c.qty === PRESETS[i]); });
    setVal(Fo.qty, Desk.fmt.size(c.qty));
    D.setClass(Fo.qty, 'zero', !(c.qty > 0));
    Fo.ttl.forEach(function (b, i) { D.setClass(b, 'on', c.ttl === LAD_TTLS[i]); });
    Fo.cap.forEach(function (b, i) { D.setClass(b, 'on', cfg.cap === CAPS[i]); });
    D.setText(Fo.hk, Desk.keys.hk ? 'HK ON' : 'HK OFF');
    D.setClass(Fo.hk, 'hkon', Desk.keys.hk);
  }

  // ---------------------------------------------------------------------------------------------
  // Ticket
  // ---------------------------------------------------------------------------------------------
  // the ticket's order type for the market loaded: the chosen one, except that START on a market
  // with no start ahead (a 15-minute window, a game under way) acts as the 15 s default, so the
  // buttons never sit disabled on a choice made for another market (the choice is kept)
  function tkType(m) {
    var id = cfg.tk.type, t = TK_TYPES[3];
    for (var i = 0; i < TK_TYPES.length; i++) if (TK_TYPES[i].id === id) t = TK_TYPES[i];
    if (t.ttl === 'START' && m !== undefined && !typeOK(t, m)) return TK_TYPES[3];
    return t;
  }
  function typeOK(t, m) {
    if (t.tif !== 'POST') return true;
    if (t.ttl === 'START') return logic.startExp(m, Date.now() + (S().srvOffset || 0)) !== null;
    return logic.ttlOK(t.ttl);
  }
  function tkLoad(key) { TK.key = key; TK.px = null; TK.pxBad = false; TK.arm = null; }
  // the order a ticket button would send now: {px, tif, ttl_s, touch, liq}; px null = no price
  function tkOrder(side, m, b) {
    var t = tkType(m), g = m.g, bb = b ? b.bb : null, ba = b ? b.ba : null, px = TK.px, buy = side === 'BUY';
    var touch = t.tif === 'POST' ? (buy ? bb : ba) : (buy ? ba : bb);
    if (px === null) {
      if (t.tif === 'POST') px = buy ? bb : ba;
      else px = touch === null ? null : Desk.grid.step(g, touch, buy ? cfg.cap : -cfg.cap);
    }
    // a post-only order can only make; an IOC/FOK fill takes. START: the expiry is the game's start
    var start = t.ttl === 'START', exp = start ? logic.startExp(m, Date.now() + (S().srvOffset || 0)) : null;
    return { px: px, tif: t.tif, ttl_s: t.tif === 'POST' && !start ? t.ttl : null, exp_ms: exp, touch: touch, liq: t.tif === 'POST' ? 'M' : 'T', auto: TK.px === null };
  }
  function ticketPrice(key, px) {
    if (!key) return;
    if (TK.key !== key) tkLoad(key);
    TK.px = px; TK.pxBad = false; TK.arm = null;
    mark('mtk');
  }
  function tkSubmit(side, ev) {
    var st = S(), key = TK.key, m = key ? st.market(key) : null, b = key ? st.book(key) : null;
    if (!m) return null;
    var btnEl = side === 'BUY' ? TK.el.buy : TK.el.sell;
    var why = blockReason(m) || qtyReason(m, cfg.tk.qty) || (TK.pxBad ? 'PRICE OFF GRID' : '') || (typeOK(tkType(m), m) ? '' : 'EXPIRY NOT IN CONTRACT');
    if (why) { refuse(null, btnEl, why); return null; }
    var o = tkOrder(side, m, b);
    if (o.px === null) { refuse(null, btnEl, 'NO PRICE: empty book side'); return null; }
    var now = Date.now(), override = false, guard = st.priceGuard(key, side, o.px);
    if (guard) {
      var a = TK.arm;
      if (a && a.side === side && a.px === o.px && a.key === key && a.until > now) override = true;
      else { TK.arm = { side: side, px: o.px, key: key, until: now + ARM_MS }; alertWarn(guard + ': click OVERRIDE to send'); mark('mtk'); return null; }
    }
    TK.arm = null;
    var cid = send(key, m, side, { tif: o.tif, px: o.px, ttl_s: o.ttl_s, exp_ms: o.exp_ms, touch: o.touch }, cfg.tk.qty, override, ev);
    mark('mtk');
    return cid;
  }
  function buildTicket() {
    var host = $('m-ticket-body');
    if (!host) return;
    host.textContent = '';
    var E = TK.el;
    var r1 = el('div', 'tk-r'), r2 = el('div', 'tk-r'), r3 = el('div', 'tk-r'), r4 = el('div', 'tk-r');
    E.vK = btn('KAL', 'tv', { 'data-a': 'venue', 'data-v': 'K' }); E.vP = btn('PM', 'tv', { 'data-a': 'venue', 'data-v': 'P' });
    E.mkt = el('span', 'tk-mkt'); E.blk = el('span', 'tag crit tk-blk');
    E.sB = btn('BUY YES', 'ts', { 'data-a': 'side', 'data-v': 'BUY' }); E.sS = btn('SELL YES', 'ts', { 'data-a': 'side', 'data-v': 'SELL' });
    add(r1, E.vK, E.vP, E.mkt, E.blk, E.sB, E.sS);
    E.qtyL = el('span', 'lb', 'QTY');
    E.qty = el('input', 'n', null, { type: 'text', inputmode: 'decimal', autocomplete: 'off', spellcheck: 'false' });
    E.q = PRESETS.map(function (q) { return btn(String(q), 'q', { 'data-a': 'q', 'data-v': String(q) }); });
    add(r2, E.qtyL, E.qty); E.q.forEach(function (b) { r2.appendChild(b); });
    E.pxL = el('span', 'lb', 'PX');
    E.px = el('input', 'n', null, { type: 'text', inputmode: 'decimal', autocomplete: 'off', spellcheck: 'false' });
    E.dn = btn(MINUS, 'stp', { 'data-a': 'step', 'data-v': '-1' }); E.up = btn('+', 'stp', { 'data-a': 'step', 'data-v': '1' });
    E.fB = btn('BID', 'fp', { 'data-a': 'fill', 'data-v': 'BID' }); E.fM = btn('MID', 'fp', { 'data-a': 'fill', 'data-v': 'MID' }); E.fA = btn('ASK', 'fp', { 'data-a': 'fill', 'data-v': 'ASK' });
    E.auto = btn('AUTO', 'fp', { 'data-a': 'auto' });
    E.capL = el('span', 'lb gap', 'CAP');
    E.cap = CAPS.map(function (c) { return btn(String(c), 'cp', { 'data-a': 'cap', 'data-v': String(c) }); });
    add(r3, E.pxL, E.px, E.dn, E.up, E.fB, E.fM, E.fA, E.auto, E.capL); E.cap.forEach(function (b) { r3.appendChild(b); });
    E.types = TK_TYPES.map(function (t) { return btn(t.label, 'ty', { 'data-a': 'type', 'data-v': t.id }); });
    E.types.forEach(function (b) { r4.appendChild(b); });
    var l5 = el('div', 'tk-l'), l6 = el('div', 'tk-l');
    E.costL = el('span', 'lb', 'cost'); E.cost = el('span', 'n v'); E.maxlL = el('span', 'lb', 'max loss'); E.maxl = el('span', 'n v');
    E.feeL = el('span', 'lb', 'fee'); E.fee = el('span', 'n v'); E.shL = el('span', 'lb'); E.sh = el('span', 'n v'); E.shOk = el('span', 'tag');
    E.move = btn('', 'mv off', { 'data-a': 'move' });
    E.guard = el('span', 'tag warn');
    add(l5, E.costL, E.cost, E.maxlL, E.maxl, E.feeL, E.fee, E.shL, E.sh, E.shOk, E.move, E.guard);
    E.eML = el('span', 'lb', 'vs model'); E.eM = el('span', 'n fair'); E.eEL = el('span', 'lb', 'vs ESPN'); E.eE = el('span', 'n espn');
    E.eNL = el('span', 'lb', 'net'); E.eN = el('span', 'n v'); E.touch = el('span', 'n dim tch');
    add(l6, E.eML, E.eM, E.eEL, E.eE, E.eNL, E.eN, E.touch);
    var r7 = el('div', 'tk-b');
    E.buy = btn('BUY', 'buy', { 'data-a': 'send', 'data-v': 'BUY' }); E.sell = btn('SELL', 'sell', { 'data-a': 'send', 'data-v': 'SELL' });
    add(r7, E.buy, E.sell);
    add(host, r1, r2, r3, r4, l5, l6, r7);
    host.addEventListener('click', onTicketClick);
    E.qty.addEventListener('input', function () { var m = S().market(TK.key); cfg.tk.qty = logic.parseQty(E.qty.value, m ? m.v : 'K'); TK.arm = null; saveCfg(); mark('mtk'); });
    E.qty.addEventListener('keydown', function (ev) {
      if (ev.key !== 'ArrowUp' && ev.key !== 'ArrowDown') return;
      ev.preventDefault();
      cfg.tk.qty = Math.max(0, Math.round((cfg.tk.qty + (ev.key === 'ArrowUp' ? 1 : -1)) * 100) / 100);
      E.qty.value = Desk.fmt.size(cfg.tk.qty); saveCfg(); mark('mtk');
    });
    E.px.addEventListener('input', function () {
      var m = S().market(TK.key);
      var u = logic.parsePx(E.px.value);
      if (u === null || !m) { TK.pxBad = E.px.value.trim() !== ''; if (!TK.pxBad) TK.px = null; }
      else if (Desk.grid.onGrid(m.g, u)) { TK.px = u; TK.pxBad = false; }
      else TK.pxBad = true;
      TK.arm = null; mark('mtk');
    });
    E.px.addEventListener('blur', function () {
      var m = S().market(TK.key), u = logic.parsePx(E.px.value);
      if (m && u !== null && TK.pxBad) { TK.px = Desk.grid.snap(m.g, u, 0); TK.pxBad = false; }
      mark('mtk');
    });
    E.px.addEventListener('keydown', function (ev) {
      if (ev.key !== 'ArrowUp' && ev.key !== 'ArrowDown') return;
      ev.preventDefault();
      stepTicketPx(ev.key === 'ArrowUp' ? 1 : -1);
      E.px.value = TK.px === null ? '' : Desk.fmt.pxg(TK.px, S().market(TK.key).g);
    });
    // Esc in the ticket: back to AUTO price, disarm (the foundation then leaves the field)
    var tp = $('m-ticket');
    if (tp) {
      tp.setAttribute('tabindex', '0');          // a click anywhere in the ticket gives it the keys (Enter sends)
      tp.addEventListener('keydown', function (ev) { if (ev.key === 'Escape' && !ev.shiftKey) tkClear(); });
    }
  }
  function tkClear() { TK.px = null; TK.pxBad = false; TK.arm = null; mark('mtk'); }
  function stepTicketPx(n) {
    var st = S(), m = st.market(TK.key); if (!m) return;
    var base = TK.px;
    if (base === null) base = tkOrder(cfg.tk.side, m, st.book(TK.key)).px;
    if (base === null) base = centerPx(st.book(TK.key), m);
    base = Desk.grid.snap(m.g, base, 0);
    TK.px = Desk.grid.step(m.g, base, n); TK.pxBad = false; TK.arm = null;
    mark('mtk');
  }
  function onTicketClick(ev) {
    var b = ev.target && ev.target.closest ? ev.target.closest('button') : null;
    if (!b || b.disabled) return;
    var a = b.getAttribute('data-a'), v = b.getAttribute('data-v'), st = S();
    var m = TK.key ? st.market(TK.key) : null, bk = TK.key ? st.book(TK.key) : null;
    if (a === 'send') { tkSubmit(v, ev); return; }
    if (a === 'move') { tkMove(); return; }
    if (a === 'venue') { if (m && v !== m.v) { var pk = logic.pairOf(st.markets, m); if (pk) Desk.focus.set(pk, { from: 'm-ticket' }); } return; }
    if (a === 'side') cfg.tk.side = v;
    else if (a === 'q') cfg.tk.qty = ev.shiftKey ? Math.round((cfg.tk.qty + +v) * 100) / 100 : +v;
    else if (a === 'step') { stepTicketPx(+v); return; }
    else if (a === 'fill') {
      if (!m || !bk) return;
      var p = v === 'BID' ? bk.bb : v === 'ASK' ? bk.ba : (bk.mid !== null ? Desk.grid.snap(m.g, bk.mid, 0) : null);
      if (p === null) return;
      TK.px = p; TK.pxBad = false;
    }
    else if (a === 'auto') { TK.px = null; TK.pxBad = false; }
    else if (a === 'cap') { cfg.cap = +v; dirtyAllLads(); }
    else if (a === 'type') cfg.tk.type = v;
    else return;
    TK.arm = null; saveCfg(); mark('mtk');
  }
  // move the missing cash onto the market's Kalshi shard (one click; the reply updates hdr)
  function tkMove() {
    var mv = TK.move;
    if (!mv || TK.movePend) return null;
    TK.movePend = true; mark('mtk');
    return Desk.act.transfer(mv.from, mv.to, mv.amt).then(function (r) { TK.movePend = false; mark('mtk'); return r; },
                                                          function () { TK.movePend = false; mark('mtk'); });
  }
  function renderTicket(d) {
    var st = S(), key = Desk.focus.key || null;
    var loaded = key !== TK.key;
    if (loaded) tkLoad(key);
    if (!(d.all || loaded || d.has('focus') || d.has('mtk') || d.has('clock') || d.has('hdr') || d.has('stale') || d.has('pos') ||
          (key && (d.has('book:' + key) || d.has('mkt:' + key))))) return;
    var E = TK.el, D = Desk.dom, F = Desk.fmt, now = Date.now();
    if (!E.buy) return;
    if (TK.arm && TK.arm.until <= now) TK.arm = null;
    var m = key ? st.market(key) : null, b = key ? st.book(key) : null;
    D.setAttr($('m-ticket'), 'data-v', m ? m.v : null);
    D.setText($('m-ticket-tools'), m ? m.tk : '');
    // row 1
    var pair = m ? logic.pairOf(st.markets, m) : null;
    D.setClass(E.vK, 'on', !!m && m.v === 'K'); D.setClass(E.vP, 'on', !!m && m.v === 'P');
    setDisabled(E.vK, !m || (m.v !== 'K' && !pair)); setDisabled(E.vP, !m || (m.v !== 'P' && !pair));
    D.setText(E.mkt, m ? m.short : 'none'); D.setAttr(E.mkt, 'title', m ? m.title + ' ' + m.tk : null);
    var why = m ? blockReason(m) : 'NO MARKET';
    D.setText(E.blk, m && why ? why : '');
    D.setClass(E.sB, 'on-BUY', cfg.tk.side === 'BUY'); D.setClass(E.sS, 'on-SELL', cfg.tk.side === 'SELL');
    // rows 2-4
    setVal(E.qty, F.size(cfg.tk.qty));
    D.setClass(E.qty, 'zero', !(cfg.tk.qty > 0));
    E.q.forEach(function (x, i) { D.setClass(x, 'on', cfg.tk.qty === PRESETS[i]); });
    E.cap.forEach(function (x, i) { D.setClass(x, 'on', cfg.cap === CAPS[i]); });
    var t = tkType(m);
    E.types.forEach(function (x, i) {
      var tt = TK_TYPES[i], ok = typeOK(tt, m);
      D.setClass(x, 'on', tt.id === t.id); setDisabled(x, !ok);
      D.setAttr(x, 'title', ok ? (tt.ttl === 'START' && m ? 'expires at the start ' + Desk.fmt.time(m.start_ms) : null)
        : (tt.ttl === 'START' ? (m && m.start_ms ? 'started' : 'no start time') : 'not in contract ' + (Desk.contract ? Desk.contract.version : '')));
    });
    TK.move = null;
    if (!m) {
      D.setClass(E.move, 'off', true);
      setVal(E.px, ''); [E.cost, E.maxl, E.fee, E.shL, E.sh, E.shOk, E.guard, E.eM, E.eE, E.eN, E.touch].forEach(function (x) { D.setText(x, ''); });
      D.setText(E.buy, 'BUY'); D.setText(E.sell, 'SELL'); setDisabled(E.buy, true); setDisabled(E.sell, true);
      setCls(E.buy, 'buy'); setCls(E.sell, 'sell');
      return;
    }
    var g = m.g, side = cfg.tk.side;
    var ob = tkOrder('BUY', m, b), os = tkOrder('SELL', m, b), o = side === 'BUY' ? ob : os;
    if (TK.pxBad) D.setClass(E.px, 'bad', true);
    else { D.setClass(E.px, 'bad', false); setVal(E.px, o.px === null ? '' : F.pxg(o.px, g)); }
    D.setClass(E.px, 'auto', TK.px === null && !TK.pxBad);
    D.setClass(E.auto, 'on', TK.px === null);
    // check line
    var P = st.position(key);
    if (o.px !== null && cfg.tk.qty > 0) {
      var ci = logic.costInfo(m, P, side, o.px, cfg.tk.qty, o.liq);
      D.setText(E.cost, F.money(ci.cost)); D.setText(E.maxl, F.money(ci.maxl)); D.setText(E.fee, F.money(ci.fee));
      var free = null, lbl;
      if (m.v === 'K') { var sh = st.hdr && m.shard ? st.hdr.cash.K.shards.filter(function (s) { return s.id === m.shard; })[0] : null; free = sh ? sh.free : null; lbl = (m.shard || 'K') + ' free'; }
      else { free = st.hdr && !st.hdr.cash.P.ro ? st.hdr.cash.P.free : null; lbl = 'PM free'; }
      D.setText(E.shL, lbl); D.setText(E.sh, free === null ? '?' : F.money(free));
      var short = free !== null && ci.need > free;
      D.setText(E.shOk, free === null ? '' : (short ? 'SHORT ' + F.money(ci.need - free) : 'OK'));
      setCls(E.shOk, 'tag ' + (short ? 'warn' : 'ok'));
      var mv = short && m.v === 'K' && m.shard && st.hdr ? logic.moveFor(st.hdr.cash.K.shards, m.shard, ci.need - free) : null;
      if (mv) TK.move = { from: mv.from, to: m.shard, amt: mv.amt };
      D.setText(E.move, mv ? (TK.movePend ? 'MOVING' : 'MOVE ' + F.money(mv.amt, 0) + ' ' + mv.from + '>' + m.shard) : '');
      D.setClass(E.move, 'off', !mv); D.setClass(E.move, 'pend', !!TK.movePend); D.setClass(E.move, 'part', !!mv && !mv.full);
      setDisabled(E.move, !mv || !!TK.movePend || !!S().canTrade('K'));
    } else { [E.cost, E.maxl, E.fee, E.sh, E.shOk].forEach(function (x) { D.setText(x, ''); }); D.setText(E.shL, ''); D.setClass(E.move, 'off', true); }
    var guard = o.px !== null ? st.priceGuard(key, side, o.px) : '';
    D.setText(E.guard, guard ? 'GUARD ' + guard.replace(/^PRICE_BAND /, '').replace(/ \(.*$/, '') : '');
    // edge line: per contract, + in our favour
    var ref = b ? b.ref : null, sgn = side === 'BUY' ? 1 : -1, nowS = now + (st.srvOffset || 0);
    var em = ref && ref.fair !== null && o.px !== null ? (ref.fair - o.px) * sgn : null;
    var ee = ref && ref.espn !== null && o.px !== null ? (ref.espn - o.px) * sgn : null;
    D.setText(E.eM, em === null ? '' : F.cents(em) + 'c' + (ref.sd !== null ? ' \u00b1' + (ref.sd / 100).toFixed(1) : ''));
    D.setText(E.eE, ee === null ? '' : F.cents(ee) + 'c ' + (ref.espn_ts_ms ? F.age(nowS - ref.espn_ts_ms) : ''));
    var feePer = o.px !== null && cfg.tk.qty > 0 ? Desk.fees.estimate(m.v, m.fee, side, o.px, cfg.tk.qty, o.liq) / cfg.tk.qty : null;
    var net = em !== null && feePer !== null ? em - feePer : null;
    D.setText(E.eN, net === null ? '' : F.cents(net) + 'c'); setCls(E.eN, 'n v ' + (net === null ? '' : F.sign(net)));
    D.setText(E.touch, b ? 'bid ' + (b.bb === null ? '-' : F.pxg(b.bb, g)) + ' ask ' + (b.ba === null ? '-' : F.pxg(b.ba, g)) : '');
    [[E.eML, E.eM], [E.eEL, E.eE], [E.eNL, E.eN]].forEach(function (x) { D.setClass(x[0], 'off', x[1].textContent === ''); });
    // buttons
    var lab = function (sd, x) {
      var q = F.size(cfg.tk.qty);
      if (x.px === null) return sd + ' ' + q;
      if (x.tif === 'POST') return sd + ' ' + q + ' @' + F.pxg(x.px, g) + ' ' + t.label;
      return sd + ' ' + q + ' ' + (x.tif === 'FOK' ? 'FOK ' : '') + '\u2264' + F.pxg(x.px, g);
    };
    var blocked = !!(why || qtyReason(m, cfg.tk.qty) || TK.pxBad || !typeOK(t, m));
    [['BUY', E.buy, ob], ['SELL', E.sell, os]].forEach(function (x) {
      var armed = TK.arm && TK.arm.side === x[0] && TK.arm.key === key && TK.arm.px === x[2].px;
      D.setText(x[1], (armed ? 'OVERRIDE ' : '') + lab(x[0], x[2]));
      setCls(x[1], armed ? 'confirm' : (x[0] === 'BUY' ? 'buy' : 'sell'));
      setDisabled(x[1], blocked || x[2].px === null);
    });
  }

  // ---------------------------------------------------------------------------------------------
  // Watchlist
  // ---------------------------------------------------------------------------------------------
  function buildWatch() {
    var panel = $('m-watch'), tools = $('m-watch-tools'), body = $('m-watch-body');
    if (!panel || !body) return;
    panel.setAttribute('tabindex', '0');
    var E = W.el;
    if (tools) {
      tools.textContent = '';
      E.vb = ['K', 'P', 'ALL'].map(function (v) { return btn(v, 'vf', { 'data-a': 'venue', 'data-v': v }); });
      E.q = el('input', 'srch', null, { type: 'text', placeholder: '/', autocomplete: 'off', spellcheck: 'false' });
      E.vb.forEach(function (b) { tools.appendChild(b); }); tools.appendChild(E.q);
      tools.addEventListener('click', function (ev) {
        var b = ev.target && ev.target.closest ? ev.target.closest('button') : null; if (!b) return;
        cfg.watch.venue = b.getAttribute('data-v'); saveCfg(); W.dirty = true; mark('mwatch');
      });
      E.q.addEventListener('input', function () { W.q = E.q.value; W.dirty = true; mark('mwatch'); });
    }
    // 288 px: Bid/Ask fit '0.645', Fair/ESPN show cents (a model to 0.1c is false precision here),
    // T fits 'Q4 12:59'; Market gets the rest (about 60 px, 92 px in WIDE mode)
    W.tb = Desk.dom.table(body, [{ h: 'Market' }, { h: 'Bid', w: 38, cls: 'n' }, { h: 'Ask', w: 38, cls: 'n' }, { h: 'Fair', w: 32, cls: 'n' },
                                 { h: 'ESPN', w: 32, cls: 'n' }, { h: 'Pos', w: 28, cls: 'n' }, { h: 'T', w: 54 }], 'watch');
    W.tb.addEventListener('click', function (ev) {
      var tr = ev.target && ev.target.closest ? ev.target.closest('tr') : null; if (!tr) return;
      var grp = tr.getAttribute('data-grp');
      if (grp) { if (cfg.watch.collapsed[grp]) delete cfg.watch.collapsed[grp]; else cfg.watch.collapsed[grp] = true; saveCfg(); W.dirty = true; mark('mwatch'); return; }
      var k = tr.getAttribute('data-mkt'); if (k) { W.cursor = k; mark('mwatch'); }
    });
  }
  // a game group row carries the score: 'NCAAF SOUMIS@TROY 3  10-14'
  var groupGame = new Map();
  function groupInfo(grp) { var gm = groupGame.get(grp); return gm && gm.per !== 'PRE' ? '  ' + gm.score : ''; }
  function watchMove(n) {
    if (!W.keys.length) return;
    var i = W.keys.indexOf(W.cursor);
    i = i < 0 ? (n > 0 ? 0 : W.keys.length - 1) : Math.max(0, Math.min(W.keys.length - 1, i + n));
    W.cursor = W.keys[i]; W.scroll = true; W.dirty = true; mark('mwatch');
  }
  function watchLoad(other) {
    var k = W.cursor && W.keys.indexOf(W.cursor) >= 0 ? W.cursor : W.keys[0];
    if (k) Desk.focus.set(k, { other: !!other, from: 'm-watch' });
  }
  function renderWatch(d) {
    if (!W.tb) return;
    var st = S(), now = Date.now(), F = Desk.fmt;
    if (Desk.focus.key !== W.lastFocus) { W.lastFocus = Desk.focus.key; if (Desk.focus.key) W.cursor = Desk.focus.key; W.dirty = true; }
    if (d.any('book:')) W.dirty = true;
    var structural = d.all || d.has('mkt') || d.has('pos') || d.has('focus') || d.has('mwatch') || d.has('snapshot');
    if (!structural && !(W.dirty && now - W.lastAt >= 200) && !(d.has('clock') && now - W.lastAt >= 1000)) return;
    W.lastAt = now; W.dirty = false;
    if (W.el.vb) W.el.vb.forEach(function (b) { Desk.dom.setClass(b, 'on', b.getAttribute('data-v') === cfg.watch.venue); });
    groupGame.clear();
    st.markets.forEach(function (m) { if (m.kind !== 'GAME') return; var b = st.book(m.key); if (b && b.ref && b.ref.game) groupGame.set(logic.watchGroup(m), b.ref.game); });
    var list = logic.watchList(st.markets, { venue: cfg.watch.venue, q: W.q, collapsed: cfg.watch.collapsed });
    W.keys = [];
    list.forEach(function (x) { if (x.m) W.keys.push(x.m.key); });
    var nowS = now + (st.srvOffset || 0);
    if (!list.length) { Desk.dom.emptyRow(W.tb, 7); return; }
    Desk.dom.keyedRows(W.tb, list, function (x) { return x.m ? x.m.key : 'g:' + x.grp; }, function (x) {
      if (!x.m) return [{ t: (x.collapsed ? '+ ' : '') + x.grp + ' ' + x.n + groupInfo(x.grp), attr: { colspan: 7 } }];
      var m = x.m, b = st.book(m.key), g = m.g, ref = b ? b.ref : null, P = st.position(m.key);
      var amber = false;
      if (ref && ref.fair !== null && b.mid !== null) amber = Math.abs(b.mid - ref.fair) > Desk.fees.takerPer(m.v, m.fee, b.mid);
      var game = ref ? ref.game : null, T, Tc = 'tcl';
      if (m.status !== 'OPEN') { T = m.status === 'CLOSED' ? 'CLSD' : m.status; Tc = 'tcl dim'; }
      else if (game && game.per !== 'PRE') T = game.per === 'FINAL' ? 'FINAL' : game.per + ' ' + game.clock;
      else if (game) { T = game.clock; Tc = 'tcl dim'; }
      else T = F.left(m.close_ms - nowS);
      return [
        { t: logic.watchLabel(m), cls: 't', title: m.short + ' | ' + m.title + ' | ' + m.tk },
        { t: b && b.bb !== null ? F.pxg(b.bb, g) : '', cls: 'n bid', fl: 'd' },
        { t: b && b.ba !== null ? F.pxg(b.ba, g) : '', cls: 'n ask', fl: 'd' },
        { t: ref && ref.fair !== null ? logic.fmtRef(ref.fair, null) : '', cls: 'n ' + (amber ? 'warn' : 'fair'), title: ref && ref.fair !== null ? logic.fmtRef(ref.fair, g) + (ref.sd !== null ? ' \u00b1' + (ref.sd / U).toFixed(2) : '') : null },
        { t: ref && ref.espn !== null ? logic.fmtRef(ref.espn, null) : '', cls: 'n espn', title: ref && ref.espn !== null ? logic.fmtRef(ref.espn, g) + (ref.espn_ts_ms ? ' ' + F.age(nowS - ref.espn_ts_ms) : '') : null },
        { t: P && P.pos ? F.ssize(P.pos) : '', cls: 'n' },
        { t: T, cls: Tc }
      ];
    }, function (x) {
      if (!x.m) return { cls: 'grp', attr: { 'data-grp': x.grp } };
      var k = x.m.key;
      return { cls: [k === W.cursor ? 'sel' : '', x.m.status !== 'OPEN' ? 'closed' : ''].join(' ').trim(), attr: { 'data-mkt': k, 'data-v': x.m.v } };
    });
    if (W.scroll) {
      W.scroll = false;
      var ch = W.tb.children;
      for (var i = 0; i < ch.length; i++) if (ch[i].getAttribute('data-mkt') === W.cursor) {
        try { ch[i].scrollIntoView({ block: 'nearest' }); } catch (e) { try { ch[i].scrollIntoView(false); } catch (e2) { /* old engine */ } }
        break;
      }
    }
  }

  // ---------------------------------------------------------------------------------------------
  // Working (manual and foreign), fills (manual), positions
  // ---------------------------------------------------------------------------------------------
  function orderTime(o) { return o.t.click || o.t.recv || (o.local && o.local.at) || 0; }
  function buildGrids() {
    var D = Desk.dom;
    var wb = $('m-work-body'), fb = $('m-fills-body'), pb = $('m-pos-body');
    if (wb) {
      GR.work = D.table(wb, [{ h: 'Time', w: 60 }, { h: 'Mkt' }, { h: 'Side', w: 40 }, { h: 'Px', w: 44, cls: 'n' }, { h: 'Qty', w: 40, cls: 'n' },
                             { h: 'Fill', w: 40, cls: 'n' }, { h: 'Left', w: 40, cls: 'n' }, { h: 'Exp', w: 36, cls: 'n' }, { h: 'PIQ', w: 44, cls: 'n' },
                             { h: 'St', w: 52 }, { h: '', w: 20 }], 'work');
      GR.work.addEventListener('click', function (ev) {
        var td = ev.target && ev.target.closest ? ev.target.closest('td') : null;
        var cid = td ? td.getAttribute('data-cid') : null;
        if (cid) Desk.act.cancel([cid]);
      });
    }
    var wt = $('m-work-tools');
    if (wt) {
      GR.workN = el('span', 'pinfo'); GR.workDel = btn('DEL ALL', '', { 'data-a': 'delall' });
      wt.textContent = ''; add(wt, GR.workN, GR.workDel);
      GR.workDel.addEventListener('click', function () { Desk.act.cancelAll('MAN', null); });
    }
    if (fb) GR.fills = D.table(fb, [{ h: 'Time', w: 72 }, { h: 'Mkt' }, { h: 'Side', w: 40 }, { h: 'Px', w: 44, cls: 'n' }, { h: 'Qty', w: 40, cls: 'n' },
                                    { h: 'Liq', w: 28 }, { h: 'Fee', w: 40, cls: 'n' }, { h: 'MO10', w: 44, cls: 'n' }, { h: 'MO60', w: 44, cls: 'n' }], 'fills');
    var ft = $('m-fills-tools'); if (ft) { GR.fillsN = el('span', 'pinfo'); ft.textContent = ''; ft.appendChild(GR.fillsN); }
    if (pb) {
      GR.pos = D.table(pb, [{ h: 'Mkt' }, { h: 'V', w: 18 }, { h: 'Own', w: 36 }, { h: 'Pos', w: 42, cls: 'n' }, { h: 'Avg', w: 44, cls: 'n' },
                            { h: 'Bid', w: 44, cls: 'n' }, { h: 'Ask', w: 44, cls: 'n' }, { h: 'Unrl', w: 52, cls: 'n' }, { h: 'Real', w: 52, cls: 'n' },
                            { h: 'MaxL', w: 50, cls: 'n' }, { h: '', w: 36 }], 'pos');
      GR.pos.addEventListener('click', function (ev) {
        var td = ev.target && ev.target.closest ? ev.target.closest('td') : null;
        var k = td ? td.getAttribute('data-flat') : null;
        if (k) flatten(k, ev);
      });
    }
    var pt = $('m-pos-tools'); if (pt) { GR.posN = el('span', 'pinfo'); pt.textContent = ''; pt.appendChild(GR.posN); }
  }
  function renderWork(d) {
    if (!GR.work || !(d.all || d.has('ord') || d.has('focus') || d.has('clock') || d.has('mkt'))) return;
    var st = S(), F = Desk.fmt, nowS = Date.now() + (st.srvOffset || 0), fk = Desk.focus.key;
    var pred = function (o) { return o.own === 'MAN' || logic.isExt(o); };
    var live = st.working(pred), items = live.concat(st.recent(TERM_SHOW_MS, pred));
    items.sort(function (a, b) { return orderTime(b) - orderTime(a); });
    Desk.dom.setText(GR.workN, live.length ? String(live.length) : '');
    if (!items.length) { Desk.dom.emptyRow(GR.work, 11); return; }
    Desk.dom.keyedRows(GR.work, items, function (o) { return o.cid; }, function (o) {
      var m = st.market(o.key), g = m ? m.g : null, ext = logic.isExt(o);
      var mkt = (ext ? 'EXT ' : '') + (m ? m.short : o.tk), stTxt = o.st, why = '';
      if (o.st === 'CXL' && o.tif !== 'POST' && (o.why === 'IOC_REST' || o.why === 'FOK' || o.why === 'STP'))
        mkt = o.tif + ' ' + F.size(o.filled) + '/' + F.size(o.qty) + ' killed ' + (m ? m.short : o.tk);
      if (o.st === 'REJ') why = o.why || 'REJ';
      var expLeft = o.exp_ms && !o.terminal ? o.exp_ms - nowS : null;
      var exp = o.tif === 'POST' ? (expLeft === null ? (ext && !o.terminal ? 'GTC' : '') : (expLeft >= 100000 ? F.left(expLeft) : F.secs(expLeft))) : o.tif;
      var lat = o.t.click && o.t.ack ? ' ack ' + (o.t.ack - o.t.click) + 'ms' : '';
      return [
        { t: F.time(orderTime(o)), cls: 'n tm', title: o.cid + lat },
        { t: mkt, cls: o.st === 'REJ' ? 't why' : 't' + (ext ? ' ext' : ''), title: (m ? m.title + ' ' : '') + o.tk },
        { t: o.side, cls: 't side-' + o.side },
        { t: g ? F.pxg(o.px, g) : F.px(o.px), cls: 'n' },
        { t: F.size(o.qty), cls: 'n' },
        { t: o.filled ? F.size(o.filled) : '', cls: 'n' },
        { t: o.terminal ? '' : F.size(o.left), cls: 'n' },
        { t: exp, cls: 'n' + (o.tif === 'POST' ? '' : ' dim') },
        { t: o.piq !== null && o.piq !== undefined ? F.size(o.piq) : '', cls: 'n' },
        { t: why || stTxt, cls: 'st st-' + o.st + (why ? ' why' : ''), title: (o.why || '') + lat },
        o.terminal ? { t: '', cls: '', attr: { 'data-cid': null, 'data-nolink': null } } : { t: 'x', cls: 'x', attr: { 'data-cid': o.cid, 'data-nolink': '1' } }
      ];
    }, function (o) {
      return { cls: 'o-' + o.st + (o.local && o.local.cxlPend ? ' cxlpend' : '') + (o.key === fk ? ' sel' : ''), attr: { 'data-mkt': o.key, 'data-v': o.v } };
    });
  }
  function renderFills(d) {
    if (!GR.fills || !(d.all || d.has('fill') || d.has('mo') || d.has('focus') || d.has('mkt'))) return;
    var st = S(), F = Desk.fmt, fk = Desk.focus.key, items = [], n = 0;
    for (var i = 0; i < st.fills.length; i++) { var f = st.fills[i]; if (f.own !== 'MAN') continue; n++; if (items.length < 200) items.push(f); }
    Desk.dom.setText(GR.fillsN, n ? String(n) : '');
    if (!items.length) { Desk.dom.emptyRow(GR.fills, 9); return; }
    Desk.dom.keyedRows(GR.fills, items, function (f) { return f.fid; }, function (f) {
      var m = st.market(f.key), g = m ? m.g : null, m10 = f.mo['10s'], m60 = f.mo['60s'];
      return [
        { t: F.timeT(f.ts_ms), cls: 'n tm', title: f.fid + ' ' + f.cid },
        { t: m ? m.short : f.tk, cls: 't', title: f.tk },
        { t: f.side, cls: 't side-' + f.side },
        { t: g ? F.pxg(f.px, g) : F.px(f.px), cls: 'n' },
        { t: F.size(f.qty), cls: 'n' },
        { t: f.liq, cls: 't' },
        { t: F.money(f.fee), cls: 'n' },
        { t: m10 === undefined ? '' : F.cents(m10), cls: 'n ' + (m10 === undefined ? '' : F.sign(m10)) },
        { t: m60 === undefined ? '' : F.cents(m60), cls: 'n ' + (m60 === undefined ? '' : F.sign(m60)) }
      ];
    }, function (f) { return { cls: f.key === fk ? 'sel' : '', attr: { 'data-mkt': f.key, 'data-v': f.v } }; });
  }
  function renderPos(d) {
    if (!GR.pos) return;
    var now = Date.now();
    if (d.any('book:')) GR.posDirty = true;
    if (!(d.all || d.has('pos') || d.has('focus') || d.has('mkt') || d.has('stale') || (GR.posDirty && now - GR.posAt >= 500))) return;
    GR.posAt = now; GR.posDirty = false;
    var st = S(), F = Desk.fmt, fk = Desk.focus.key, open = [], flat = [];
    st.pos.forEach(function (P) { if (P.pos) open.push(P); else if (P.real) flat.push(P); });
    open.sort(function (a, b) { return b.maxl - a.maxl; });
    flat.sort(function (a, b) { return Math.abs(b.real) - Math.abs(a.real); });
    var items = open.concat(flat);
    Desk.dom.setText(GR.posN, open.length ? String(open.length) : '');
    if (!items.length) { Desk.dom.emptyRow(GR.pos, 11); return; }
    var tot = { unrl: 0, real: 0, maxl: 0 };
    items.forEach(function (P) { tot.unrl += P.unrl; tot.real += P.real; tot.maxl += P.maxl; });
    items.push({ total: true, key: '__total' });
    Desk.dom.keyedRows(GR.pos, items, function (P) { return P.key; }, function (P) {
      if (P.total) return [{ t: 'TOTAL', cls: 't' }, '', '', '', '', '', '', { t: F.pnl(tot.unrl), cls: 'n ' + F.sign(tot.unrl) },
                           { t: F.pnl(tot.real), cls: 'n ' + F.sign(tot.real) }, { t: F.money(tot.maxl), cls: 'n' }, ''];
      var m = st.market(P.key), g = m ? m.g : null, b = st.book(P.key), px = function (u) { return u === null || u === undefined ? '' : (g ? F.pxg(u, g) : F.px(u)); };
      var canFlat = P.pos && m && !blockReason(m);
      return [
        { t: m ? m.short : P.tk, cls: 't', title: P.tk },
        { t: P.v, cls: 't dim' },
        { t: P.own || '', cls: 't' },
        { t: F.ssize(P.pos), cls: 'n' },
        { t: P.pos ? px(P.avg) : '', cls: 'n' },
        { t: b ? px(b.bb) : '', cls: 'n bid' },
        { t: b ? px(b.ba) : '', cls: 'n ask' },
        { t: P.pos ? F.pnl(P.unrl) : '', cls: 'n ' + F.sign(P.unrl), fl: 'd' },
        { t: F.pnl(P.real), cls: 'n ' + F.sign(P.real) },
        { t: P.pos ? F.money(P.maxl) : '', cls: 'n' },
        canFlat ? { t: 'FLAT', cls: 'flat', attr: { 'data-flat': P.key, 'data-nolink': '1' } } : { t: '', cls: '', attr: { 'data-flat': null, 'data-nolink': null } }
      ];
    }, function (P) {
      if (P.total) return 'total';
      return { cls: [P.pos ? '' : 'flat0', P.key === fk ? 'sel' : ''].join(' ').trim(), attr: { 'data-mkt': P.key, 'data-v': P.v } };
    });
  }

  // ---------------------------------------------------------------------------------------------
  // Bookkeeping: default ladders, 15-minute rolls, takeovers, prints for the LTQ column
  // ---------------------------------------------------------------------------------------------
  function rollOf(key, m) {
    var st = S(), tk = m ? m.tk : st.split(key).tk, ser = logic.series(tk), best = null;
    if (m && m.kind !== 'LIP15') return null;
    st.markets.forEach(function (x) {
      if (x.kind !== 'LIP15' || x.status !== 'OPEN' || x.key === key || logic.series(x.tk) !== ser) return;
      if (!best || x.close_ms < best.close_ms) best = x;
    });
    return best ? best.key : null;
  }
  function defaultFor(slot, used) {
    var st = S(), open = [];
    st.markets.forEach(function (m) { if (m.status === 'OPEN' && !used[m.key]) open.push(m); });
    var pick = function (f) { for (var i = 0; i < open.length; i++) if (f(open[i])) return open[i].key; return null; };
    var inPlay = function (m) { var b = st.book(m.key), gm = b && b.ref ? b.ref.game : null; return !!gm && gm.per !== 'PRE' && gm.per !== 'FINAL'; };
    var k = null;
    if (slot === 'A') k = pick(function (m) { return m.kind === 'LIP15' && /^KXNATGAS/.test(m.tk); }) || pick(function (m) { return m.kind === 'LIP15'; });
    else if (slot === 'B') k = pick(function (m) { return m.kind === 'GAME' && m.v === 'K' && inPlay(m); }) || pick(function (m) { return m.kind === 'GAME'; });
    else if (slot === 'D') k = pick(function (m) { return m.kind === 'LIP15' && /^KXPLATINUM/.test(m.tk); }) || pick(function (m) { return m.kind === 'GAME' && m.v === 'P'; }) || pick(function (m) { return m.kind === 'LIP15'; });
    else {
      // the most likely weather bracket with a two-sided book
      var best = null;
      open.forEach(function (m) { var b = st.book(m.key); if (m.kind === 'WEATHER' && b && b.mid !== null && (!best || b.mid > best.mid)) best = { key: m.key, mid: b.mid }; });
      k = (best && best.key) || pick(function (m) { return m.kind === 'WEATHER'; }) || pick(function (m) { return m.kind === 'GAME' && m.v === 'P'; });
    }
    return k || (open.length ? open[0].key : null);
  }
  function ensureLadders() {
    var st = S(), F = Desk.focus;
    if (!st.markets.size || st.resyncing) return;
    var used = {};
    SLOTS.forEach(function (s) { var k = F.ladders[s]; if (k) used[k] = 1; });
    SLOTS.forEach(function (slot) {
      var k = F.ladders[slot], m = k ? st.market(k) : null;
      if (m && (m.status === 'OPEN' || (m.status === 'CLOSED' && m.kind !== 'LIP15'))) return;
      var next = k ? rollOf(k, m) : null;
      if (!next && (!m || m.status === 'SETTLED' || m.kind === 'LIP15')) next = defaultFor(slot, used);
      if (next && next !== k) {
        used[next] = 1;
        F.set(next, { ladder: slot, other: slot !== F.ladder, from: 'roll' });
      }
    });
  }
  function renderCore(d) {
    if (d.all || d.has('mkt') || d.has('snapshot') || d.has('layout')) ensureLadders();
  }
  function trackPrints(d) {
    if (!seenPrint) return;
    var st = S();
    var visit = function (b) {
      if (!b || !b.prints || !b.prints.length) return;
      var mp = vap.get(b.key);
      for (var i = 0; i < b.prints.length; i++) {
        var p = b.prints[i];
        if (seenPrint.has(p)) continue;
        seenPrint.add(p);
        if (!mp) { mp = new Map(); vap.set(b.key, mp); }
        mp.set(p.px, Math.round(((mp.get(p.px) || 0) + p.qty) * 100) / 100);
      }
    };
    if (d.all) { st.books.forEach(visit); return; }
    if (!d.set) return;
    d.set.forEach(function (t) { if (t.indexOf('book:') === 0) visit(st.books.get(t.slice(5))); });
  }

  // ---------------------------------------------------------------------------------------------
  // Keys
  // ---------------------------------------------------------------------------------------------
  function ladderOf(ctx) {
    var s = ctx && ctx.el && ctx.el.getAttribute ? ctx.el.getAttribute('data-ladder') : null;
    return LAD[s || Desk.focus.ladder] || null;
  }
  function onManual() { return Desk.render.tab === 'manual' || !!(Desk.layout && Desk.layout.split); }
  function hotOrder(L, kind, side, ev) {
    var x = ladMarket(L), m = x.m, b = x.b;
    if (!m) return null;
    var why = blockReason(m) || qtyReason(m, L.cfg.qty);
    if (why) { alertWarn(why); return null; }
    var it = logic.hotIntent(kind, m.g, b ? b.bb : null, b ? b.ba : null, side, cfg.cap, L.cfg.ttl);
    if (it.err) { alertWarn(kind.toUpperCase() + ' ' + side + ' ' + m.short + ': ' + it.err); return null; }
    var guard = S().priceGuard(x.key, side, it.px);
    if (guard) { alertWarn(guard); return null; }
    return send(x.key, m, side, it, L.cfg.qty, false, ev, it.take ? it.touch : undefined);
  }
  function bindKeys() {
    var K = Desk.keys;
    var hot = function (kind, side) { return function (ev, ctx) { var L = ladderOf(ctx); if (L && onManual()) hotOrder(L, kind, side, ev); }; };
    [['b', 'F1', 'take', 'BUY', 'buy Q: IOC at the ask + cap'], ['s', 'F2', 'take', 'SELL', 'sell Q: IOC at the bid ' + MINUS + ' cap'],
     ['Shift+b', 'F3', 'join', 'BUY', 'join the bid: post, ladder expiry'], ['Shift+s', 'F4', 'join', 'SELL', 'join the offer: post, ladder expiry'],
     ['Alt+b', 'F5', 'improve', 'BUY', 'bid +1 tick: post (spread 2+ ticks)'], ['Alt+s', 'F6', 'improve', 'SELL', 'offer ' + MINUS + '1 tick: post (spread 2+ ticks)']
    ].forEach(function (x) {
      K.bind({ key: x[0], scope: 'ladder', order: true, desc: x[4], fn: hot(x[2], x[3]) });
      K.bind({ key: x[1], scope: 'ladder', order: true, desc: x[4], fn: hot(x[2], x[3]) });
    });
    K.bind({ key: 'Escape', scope: 'ladder', order: true, desc: 'cancel our orders in this market', fn: function (ev, ctx) {
      var L = ladderOf(ctx), m = L && L.shownKey ? S().market(L.shownKey) : null;
      if (m && onManual()) Desk.act.cancelWhere({ v: m.v, tk: m.tk, own: 'MAN' });
    } });
    K.bind({ key: 'Shift+Escape', scope: 'manual', order: true, desc: 'cancel every manual order, both venues', fn: function () { Desk.act.cancelAll('MAN', null); } });
    PRESETS.forEach(function (q, i) {
      K.bind({ key: String(i + 1), scope: 'ladder', order: true, desc: i === 0 ? 'size presets 1 5 10 25 50 100' : null, fn: function (ev, ctx) {
        var L = ladderOf(ctx); if (!L) return; L.cfg.qty = q; saveCfg(); dirtyLad(L);
      } });
    });
    var stepQ = function (n) { return function (ev, ctx) { var L = ladderOf(ctx); if (!L) return; L.cfg.qty = Math.max(0, Math.round((L.cfg.qty + n) * 100) / 100); saveCfg(); dirtyLad(L); }; };
    K.bind({ key: '+', scope: 'ladder', order: true, desc: 'size +1', fn: stepQ(1) });
    K.bind({ key: '=', scope: 'ladder', order: true, fn: stepQ(1) });
    K.bind({ key: '-', scope: 'ladder', order: true, desc: 'size ' + MINUS + '1', fn: stepQ(-1) });
    var nav = function (f) { return function (ev, ctx) { var L = ladderOf(ctx); if (L) f(L); }; };
    K.bind({ key: 'Space', scope: 'ladder', desc: 'recentre', fn: nav(recentre) });
    K.bind({ key: 'ArrowUp', scope: 'ladder', desc: 'scroll up 1 row', fn: nav(function (L) { scrollLad(L, -1, true); }) });
    K.bind({ key: 'ArrowDown', scope: 'ladder', desc: 'scroll down 1 row', fn: nav(function (L) { scrollLad(L, 1, true); }) });
    K.bind({ key: 'PageUp', scope: 'ladder', desc: 'scroll up a page', fn: nav(function (L) { scrollLad(L, -Math.max(1, L.rows - 2), true); }) });
    K.bind({ key: 'PageDown', scope: 'ladder', desc: 'scroll down a page', fn: nav(function (L) { scrollLad(L, Math.max(1, L.rows - 2), true); }) });
    var cycle = function (n) { return function (ev, ctx) {
      var vis = Desk.layout.ladders(), L = ladderOf(ctx), i = vis.indexOf(L ? L.slot : Desk.focus.ladder);
      var s = vis[(i + n + vis.length) % vis.length];
      Desk.focus.setLadder(s);
      if (LAD[s] && LAD[s].el.focus) LAD[s].el.focus();
    }; };
    K.bind({ key: 'Tab', scope: 'ladder', desc: 'next ladder', fn: cycle(1) });
    K.bind({ key: 'Shift+Tab', scope: 'ladder', fn: cycle(-1) });
    K.bind({ key: '/', scope: 'manual', desc: 'search the watchlist', fn: function () { if (W.el.q) { W.el.q.focus(); if (W.el.q.select) W.el.q.select(); } } });
    K.bind({ key: 'ArrowUp', scope: 'watch', typing: true, desc: 'watchlist: previous market', fn: function () { watchMove(-1); } });
    K.bind({ key: 'ArrowDown', scope: 'watch', typing: true, desc: 'watchlist: next market', fn: function () { watchMove(1); } });
    K.bind({ key: 'Enter', scope: 'watch', typing: true, desc: 'watchlist: load into the focused ladder', fn: function () { watchLoad(false); } });
    K.bind({ key: 'Shift+Enter', scope: 'watch', typing: true, desc: 'watchlist: load into the other ladder', fn: function () { watchLoad(true); } });
    K.bind({ key: 'Enter', scope: 'ticket', typing: true, desc: 'ticket: send the selected side', fn: function (ev) { tkSubmit(cfg.tk.side, ev); } });
    K.bind({ key: 'Escape', scope: 'ticket', desc: 'ticket: price back to AUTO', fn: function () { tkClear(); } });
  }

  // ---------------------------------------------------------------------------------------------
  // init
  // ---------------------------------------------------------------------------------------------
  function init() {
    cfg = loadCfg();
    if (!hasDoc) return;
    SLOTS.forEach(buildLadder);
    buildWatch(); buildTicket(); buildGrids();
    bindKeys();
    document.addEventListener('mouseup', onDocUp);
    var R = Desk.render;
    R.add({ id: 'm-prints', tab: null, fn: trackPrints });
    R.add({ id: 'm-core', tab: null, fn: renderCore });
    R.add({ id: 'm-ladders', tab: 'manual', fn: renderLadders });
    R.add({ id: 'm-ticket', tab: 'manual', fn: renderTicket });
    R.add({ id: 'm-watch', tab: 'manual', fn: renderWatch });
    R.add({ id: 'm-work', tab: 'manual', fn: renderWork });
    R.add({ id: 'm-fills', tab: 'manual', fn: renderFills });
    R.add({ id: 'm-pos', tab: 'manual', fn: renderPos });
    if (root.addEventListener) root.addEventListener('resize', function () { dirtyAllLads(); mark('layout'); });
    // Safari slows hidden tabs: redraw in full when the page shows again
    document.addEventListener('visibilitychange', function () { if (!document.hidden) R.force('manual'); });
  }

  Desk.manual = {
    init: init,
    // the panels render through their own renderers (registered in init); kept for the contract
    render: function (d) { void d; },
    logic: logic,
    state: function () { return { cfg: cfg, ladders: LAD, ticket: TK, watch: W, sentAt: sentAt, held: held, clickAt: clickAt, vap: vap }; },
    // actions, for tests and the console
    flatten: flatten, release: release, ticketPrice: ticketPrice, tkSubmit: tkSubmit, recentre: function (s) { if (LAD[s]) recentre(LAD[s]); },
    hotOrder: function (s, kind, side, ev) { return LAD[s] ? hotOrder(LAD[s], kind, side, ev) : null; }
  };
})(typeof window !== 'undefined' ? window : globalThis);
