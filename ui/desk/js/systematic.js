/* ui/desk/js/systematic.js
 *
 * The SYSTEMATIC tab: control strip, lanes grid, working orders, fills, risk book, markouts.
 * Layout (desk.css): strip 36 px / lanes 164 px / lower area; the lower area splits into
 * working + fills on the left and risk book + markouts on the right.
 *
 *   Desk.systematic.init()     once at boot, after the header: builds the strip, the panel tools,
 *                              the risk summary line, the click delegation and the tab's keys
 *   Desk.systematic.render(d)  on animation frames while the tab shows and something changed
 *   Desk.systematic.cfg        display constants (below)
 *   Desk.systematic.filter     the lane filter (lane id or null); setFilter(id)
 *
 * Reads Desk.store only (hdr, lanes, orders, fills with their markouts, positions, risk, markets,
 * books, feeds). Sends through Desk.act only (start, stop, lane on/off, cancel). Every click
 * paints its pending state on the next frame, before the network answers, and is timed by
 * Desk.lat (the status bar's UI figure).
 *
 * Linking: working, fills, risk and recent-markout rows carry data-mkt, so a click focuses that
 * market (core.js); the clicked row carries the selection bar, the market's other rows a tint.
 * A lane row (lanes grid or markouts) filters WORKING, FILLS and MARKOUTS to the lane (click
 * again: clear) and focuses the lane's newest market; Shift+click loads it into the other ladder.
 *
 * Lanes: State is green only while the lane runs (ON); a lane switched on while Systematic is
 * stopped or killed reads IDLE (the backend's OFF with the run state as the reason). The toggle is
 * neutral (white ON, grey OFF) and sits next to State; the lane KILL is grey and turns red only
 * while the lane has live orders (or under the pointer). Cent columns say so in the header (¢).
 * WORKING drops a lane's routine endings at once (a requote's CXL PULL, an expiry, a fill, a window
 * closing); rejects and killed IOCs stay 3 s. The risk book heads a group only when it has two or
 * more markets (a cross-venue pair, a basket); a one-market outcome is one row.
 * WIDE (>= 1920 px): the Market and Reason columns stop at a fixed width and a blank filler
 * takes the rest, so the numbers stay next to the names.
 *
 * Preflight tags: the plan's set KEY CASH SHARD CLK FEED PRIV GROUPS plus DISK, as contract 1.1
 * sends them. Against an older backend without PRIV and GROUPS rows, PRIV is derived from
 * hb.feeds.KXP and GROUPS shows grey (unknown, not blocking); a backend row always wins.
 *
 * Computed here from the store (the contract carries the inputs, not these figures):
 *   - Per-lane markouts and edges, P(daily stop), the equity floor line and per-market
 *     reservations are computed here from the store. All edges and markouts are contract-weighted,
 *     in cents per contract, + = in our favour:
 *       Pred  predicted net edge at the fill: (fair - px) x side - fee per contract, over fills
 *             with a model fair
 *       Rlzd  realized net edge: the 60 s markout - fee per contract, over fills whose 60 s
 *             markout has matured
 *     The lanes grid shows both over the same fills (a fair and a matured 60 s markout) once
 *     there are any; before that, Pred over every fill with a fair and Rlzd over every matured one.
 *     A lane turns amber when, over the fills that have both (at least cfg.edgeWarnN of them),
 *     Rlzd sits more than max(1c, half of |Pred|) under Pred.
 *     P(stop) = 2 x Phi(-left / (sigma_day x sqrt(share of the local day left))): the chance a
 *     driftless P&L path touches the stop before midnight (redesign v3's table: a $25 stop at a
 *     $15 SD = 10%, at $20 = 21%).
 *     Equity (floor line) = both venues' cash + open positions marked at the bid (a long YES at
 *     the YES bid, a long NO at 1 - YES ask; a settled market at its result).
 */
(function (root) {
  'use strict';
  var Desk = root.Desk = root.Desk || {};
  var D = Desk.dom, F = Desk.fmt, R = Desk.render;
  var U = 10000, MINUS = '−';
  var hasDoc = typeof document !== 'undefined';
  var $ = function (id) { return hasDoc ? document.getElementById(id) : null; };

  var CFG = {
    ceilPct: 0.70,          // Systematic's ceiling, share of each venue's cash (the label only)
    sigmaDayU: 200000,      // $20: daily SD of Systematic P&L at zero edge (redesign v3: $15-25)
    floorU: 1500000,        // $150: combined equity floor that halts Systematic (plan, risk limit 5)
    edgeWarnU: 100,         // 1c per contract: realized this far under predicted turns a lane amber
    edgeWarnN: 30,          // ...once this many fills have both a fair and a 60 s markout
    thinN: 30,              // markout figures from fewer fills than this show grey
    shardLowU: 10000,       // a shard with under $1.00 free shows amber in the cash line
    fillsMax: 200,          // rows in FILLS (the panel scrolls; a hidden tab keeps its layout)
    recentMo: 40,           // recent fills listed under MARKOUTS
    showTermMs: 3000,       // CXL / REJ / EXP rows stay this long in WORKING
    pendMs: 5000            // a START/STOP with no answer stops showing pending after this
  };
  // preflight tags in display order (plan section 6, plus the contract's DISK)
  var PF_ORDER = ['KEY', 'CASH', 'SHARD', 'CLK', 'DISK', 'FEED', 'PRIV', 'GROUPS'];

  // ---------------------------------------------------------------------------------------------
  // maths
  // ---------------------------------------------------------------------------------------------
  var ncdf = Desk.num.ncdf;
  // chance a driftless P&L path with daily SD sigma touches -left within tau days (reflection)
  function pStop(leftU, sigmaDayU, tau) {
    if (leftU === null || leftU === undefined) return null;
    if (leftU <= 0) return 1;
    var s = sigmaDayU * Math.sqrt(Math.max(0, tau));
    if (!(s > 0)) return 0;
    return Math.min(1, 2 * ncdf(-leftU / s));
  }
  // share of the local day still to run (0..1)
  function dayLeft(now) {
    var d = new Date(now);
    var end = new Date(d.getFullYear(), d.getMonth(), d.getDate() + 1).getTime();
    return Math.max(0, Math.min(1, (end - now) / 86400000));
  }
  // worst-case cash one working order holds alone (the gateway's rule: what reduces reserves $0)
  function reserve(side, px, left, pos) { pos = pos || 0; return Desk.risk.one(side, px, left, side === 'BUY' ? -pos : pos); }
  function sgn(side) { return side === 'BUY' ? 1 : -1; }
  function has(x) { return x !== null && x !== undefined; }

  // contract-weighted markout and edge statistics over a set of fills
  function newAcc() {
    return { n: 0, ctr: 0, fee: 0,
             c1: 0, w1: 0, s1: 0, c10: 0, w10: 0, s10: 0,
             c60: 0, w60: 0, s60: 0, q60: 0, ww60: 0, neg60: 0, wr: 0, sr: 0,
             cs: 0, ws: 0, ss: 0, cm: 0, wm: 0, sm: 0, cp: 0, wp: 0, sp: 0,
             pn: 0, pw: 0, pp: 0, pr: 0 };
  }
  function addFill(a, f) {
    var q = f.qty;
    if (!(q > 0)) return;
    var s = sgn(f.side), fc = (f.fee || 0) / q, mo = f.mo || {};
    a.n++; a.ctr += q; a.fee += f.fee || 0;
    if (has(mo['1s'])) { a.c1++; a.w1 += q; a.s1 += q * mo['1s']; }
    if (has(mo['10s'])) { a.c10++; a.w10 += q; a.s10 += q * mo['10s']; }
    var m60 = mo['60s'];
    if (has(m60)) {
      a.c60++; a.w60 += q; a.s60 += q * m60; a.q60 += q * m60 * m60; a.ww60 += q * q;
      if (m60 < 0) a.neg60++;
      a.wr += q; a.sr += q * (m60 - fc);
    }
    if (has(mo.SETTLE)) { a.cs++; a.ws += q; a.ss += q * mo.SETTLE; }
    if (has(f.mid)) { a.cm++; a.wm += q; a.sm += q * (f.mid - f.px) * s; }
    if (has(f.fair)) {
      var p = (f.fair - f.px) * s - fc;
      a.cp++; a.wp += q; a.sp += q * p;
      if (has(m60)) { a.pn++; a.pw += q; a.pp += q * p; a.pr += q * (m60 - fc); }
    }
  }
  function stats(a) {
    var div = function (s, w) { return w > 0 ? s / w : null; };
    var m60 = div(a.s60, a.w60), t60 = null;
    if (a.c60 > 1 && a.w60 > 0 && a.ww60 > 0) {
      // weighted t: effective n = (sum w)^2 / sum w^2
      var v = a.q60 / a.w60 - m60 * m60, neff = a.w60 * a.w60 / a.ww60;
      if (neff > 1 && v > 1e-9) { v = v * neff / (neff - 1); t60 = m60 / Math.sqrt(v / neff); }
    }
    var pp = div(a.pp, a.pw), pr = div(a.pr, a.pw);
    return {
      n: a.n, ctr: a.ctr, fee: a.fee, feePer: div(a.fee, a.ctr),
      m1: div(a.s1, a.w1), c1: a.c1, m10: div(a.s10, a.w10), c10: a.c10,
      m60: m60, c60: a.c60, t60: t60, adv: a.c60 ? a.neg60 / a.c60 : null,
      mset: div(a.ss, a.ws), cs: a.cs,
      // spread paid: mid minus our price in our direction (+ = we bought under the mid)
      sprd: div(a.sm, a.wm), cm: a.cm,
      pred: div(a.sp, a.wp), cp: a.cp, rlzd: div(a.sr, a.wr),
      pairN: a.pn, pairPred: pp, pairRlzd: pr,
      warn: a.pn >= CFG.edgeWarnN && pp !== null && pr !== null && pr < pp - Math.max(CFG.edgeWarnU, Math.abs(pp) / 2)
    };
  }
  var EMPTY = stats(newAcc());
  var agg = { lanes: new Map(), all: EMPTY };
  function recompute() {
    var by = new Map(), all = newAcc();
    var fl = Desk.store.fills;
    for (var i = 0; i < fl.length; i++) {
      var f = fl[i];
      if (f.own !== 'SYS') continue;
      var k = f.lane || '?', a = by.get(k);
      if (!a) { a = newAcc(); by.set(k, a); }
      addFill(a, f); addFill(all, f);
    }
    var out = new Map();
    by.forEach(function (a, k) { out.set(k, stats(a)); });
    agg.lanes = out; agg.all = stats(all);
  }

  // combined equity of both venues: cash plus open positions marked at the bid
  function equity() {
    var S = Desk.store, h = S.hdr;
    if (!h) return null;
    var e = h.cash.K.total + h.cash.P.total;
    S.pos.forEach(function (p) {
      if (!p.pos) return;
      var m = S.market(p.key), b = S.book(p.key), a = Math.abs(p.pos), v;
      if (m && m.status === 'SETTLED' && m.result) v = (m.result === 'YES') === (p.pos > 0) ? U : 0;
      else if (b && (m ? m.status === 'OPEN' : true)) v = p.pos > 0 ? (has(b.bb) ? b.bb : 0) : (has(b.ba) ? U - b.ba : 0);
      else if (has(p.mark)) v = p.pos > 0 ? p.mark : U - p.mark;
      else v = 0;
      e += a * v;
    });
    return Math.round(e);
  }
  // per-market reservations of working orders (both owners: it is the venue's cash they hold).
  // The position a side can reduce is shared out across that side's orders, best price first,
  // so two sells against one long cannot both count as reducing.
  function reservations() {
    var S = Desk.store, live = [];
    S.orders.forEach(function (o) { if (!o.terminal && o.left > 0) live.push(o); });
    return Desk.risk.byMarket(live, function (key) { var p = S.pos.get(key); return p ? p.pos : 0; });
  }
  // preflight rows to show: the backend's, in PF_ORDER, plus PRIV derived from the private feed
  // and GROUPS unknown when the backend does not report them (ok: true | false | null = unknown)
  function preflightRows(h) {
    var S = Desk.store, by = {}, extra = [];
    (h ? h.preflight : []).forEach(function (r) { by[r.id] = { id: r.id, ok: r.ok, why: r.why || '', src: 'srv' }; if (PF_ORDER.indexOf(r.id) < 0) extra.push(r.id); });
    if (!by.PRIV) {
      var f = S.feeds && S.feeds.KXP;
      by.PRIV = !f ? { id: 'PRIV', ok: null, why: '', src: 'feed' }
        : { id: 'PRIV', ok: !!f.up && !f.gap, why: !f.up ? 'KXP down' : (f.gap ? 'KXP gap' : ''), src: 'feed' };
    }
    if (!by.GROUPS) by.GROUPS = { id: 'GROUPS', ok: null, why: 'not reported', src: 'none' };
    var out = [];
    PF_ORDER.concat(extra).forEach(function (id) {
      if (by[id]) out.push(by[id]);
      else if (!h) out.push({ id: id, ok: null, why: '', src: 'none' });
    });
    return out;
  }

  // ---------------------------------------------------------------------------------------------
  // view state
  // ---------------------------------------------------------------------------------------------
  var filter = null;           // lane id
  var pendRun = null;          // {act: 'start'|'stop', at}
  var clickN = 0;
  var ui = {};
  var grids = {};
  var lastRisk = { maxl: 0, rsv: 0 };

  function setFilter(id) {
    id = id || null;
    if (filter === id) return;
    filter = id;
    Desk.storage.set('sys.lane', id || '');
    R.mark('sys');
  }
  // click-to-paint for a non-order click (lane toggles, START/STOP, cancels): the scheduler
  // records the time to the next painted frame in the status bar's UI figure
  function paintMeter(ev) { if (Desk.lat) Desk.lat.click('sys-' + (++clickN), ev); }
  // server time now (expiries and run times are on the server clock)
  function srvNow(now) { return now + (Desk.store.srvOffset || 0); }

  // ---------------------------------------------------------------------------------------------
  // grid plumbing: column sets per layout (d = default ~1512 px, w = WIDE >= 1920, n = < 1400).
  // A column with no m shows everywhere; m lists the layouts it shows in.
  // ---------------------------------------------------------------------------------------------
  // A column's ww is its width in WIDE mode (a cap on a column that is elastic below 1920 px).
  // When every WIDE column has a width, a blank filler column takes the slack at the right.
  function mode() { var L = Desk.layout; return L && L.sysMode ? L.sysMode() : (L && L.wide ? 'w' : (L && L.narrow ? 'n' : 'd')); }
  function Grid(hostId, cls, cols) { this.hostId = hostId; this.cls = cls; this.all = cols; this.sig = null; this.tb = null; this.cols = []; this.fill = false; }
  Grid.prototype.ensure = function () {
    var m = mode();
    var cols = this.all.filter(function (c) { return !c.m || c.m.indexOf(m) >= 0; });
    var sig = m + ':' + cols.map(function (c) { return c.id; }).join(',');
    var host = $(this.hostId);
    if (sig === this.sig && this.tb && host && host.contains(this.tb)) return false;
    this.cols = cols; this.sig = sig;
    var defs = cols.map(function (c) { return { h: c.h, w: m === 'w' && c.ww ? c.ww : c.w, cls: c.hc, title: c.title }; });
    this.fill = m === 'w' && defs.every(function (d) { return !!d.w; });
    if (this.fill) defs.push({ h: '' });
    this.tb = D.table(host, defs, this.cls);
    return true;
  };
  var FILLER = { t: '', cls: 'fill' };
  Grid.prototype.draw = function (items, keyFn, rowFn, ctx) {
    var cols = this.cols, n = cols.length + (this.fill ? 1 : 0);
    if (!items.length) { D.emptyRow(this.tb, n); return 0; }
    return D.keyedRows(this.tb, items, keyFn, function (it) {
      var out = new Array(n);
      for (var i = 0; i < cols.length; i++) out[i] = cols[i].f(it, ctx);
      if (n > cols.length) out[cols.length] = FILLER;
      return out;
    }, rowFn);
  };
  Grid.prototype.ids = function () { return this.cols.map(function (c) { return c.id; }); };

  // cells
  function num(t, cls) { return { t: has(t) ? t : '', cls: 'n' + (cls ? ' ' + cls : '') }; }
  function txt(t, cls, title) { return { t: has(t) ? t : '', cls: 't' + (cls ? ' ' + cls : ''), title: title || null }; }
  function blank() { return num(''); }
  function signOf(u, unit) { var r = Math.round(u / unit); return r > 0 ? 'up' : (r < 0 ? 'dn' : ''); }
  function pnlC(u) { return has(u) ? num(F.pnl(u), signOf(u, 100)) : blank(); }
  function moneyC(u, cls) { return has(u) ? num(F.money(u), cls) : blank(); }
  // u per contract as signed cents, coloured by direction, grey under thinN samples
  function centsC(u, n) {
    if (!has(u) || isNaN(u)) return blank();
    var cls = signOf(u, 10);
    if (n !== undefined && n < CFG.thinN) cls += ' thin';
    return num(F.cents(u), cls);
  }
  function signed(x, dp) {
    if (!has(x) || isNaN(x)) return '';
    var s = Math.abs(x).toFixed(dp);
    if (+s === 0) return (0).toFixed(dp);
    return (x < 0 ? MINUS : '+') + s;
  }
  function mkt(key) { return Desk.store.market(key); }
  function mname(key, tk) { var m = mkt(key); return m ? m.short : tk; }
  function pxOf(u, key) { if (!has(u)) return ''; var m = mkt(key); return m ? F.pxg(u, m.g) : F.px(u); }
  // the clicked row carries the selection bar; the focused market's other rows a tint
  function focusCls(panelId, rowKey, key) { return Desk.focus && Desk.focus.rowCls ? Desk.focus.rowCls(panelId, rowKey, key) : ''; }
  function laneOn(l) { return has(l.pendOn) ? l.pendOn : l.on; }

  // ---------------------------------------------------------------------------------------------
  // LANES: one row per lane plus TOTAL
  // ---------------------------------------------------------------------------------------------
  function moOf(r) { return r.mo || EMPTY; }
  var NOACT = { 'data-act': null, 'data-id': null };
  var C = '\u00a2';             // cent columns say so in the header; money columns are dollars
  var LANE_COLS = [
    { id: 'lane', h: 'Lane', w: 120, f: function (r) { return txt(r.label, 'ln' + (r.warn ? ' warn' : ''), r.title); } },
    { id: 'v', h: 'V', w: 28, m: 'w', f: function (r) { return txt(r.venue || '', 'dim'); } },
    { id: 'state', h: 'State', w: 72, f: function (r) { return txt(r.state, 'ls-' + r.state, r.stateWhy || null); } },
    // the lane's on/off toggle, next to its state: neutral (white ON, grey OFF)
    { id: 'tg', h: 'On', w: 44, f: function (r) {
      if (r.kind !== 'lane') return { t: '', cls: '', attr: NOACT };
      return { t: r.onShown ? 'ON' : 'OFF', cls: 'tg' + (r.onShown ? ' on' : '') + (r.pend ? ' pend' : ''), attr: { 'data-act': 'lane', 'data-id': r.id } };
    } },
    { id: 'style', h: 'Style', w: 56, m: 'w', f: function (r) { return txt(r.style || '', 'dim'); } },
    { id: 'budget', h: 'Budget', w: 64, hc: 'n', f: function (r) { return moneyC(r.budget); } },
    { id: 'used', h: 'Used', w: 64, hc: 'n', f: function (r) { return moneyC(r.used, r.budget > 0 && r.used >= 0.9 * r.budget ? 'warn' : ''); } },
    { id: 'open', h: 'Open', w: 64, hc: 'n', m: 'w', f: function (r) { return moneyC(r.open); } },
    { id: 'cap', h: 'CapLeft', w: 64, hc: 'n', m: 'dw', f: function (r) { return moneyC(r.cap_left, r.kind === 'lane' && r.budget > 0 && r.cap_left < 0.1 * r.budget ? 'warn' : ''); } },
    { id: 'wrk', h: 'Wrk', w: 40, hc: 'n', f: function (r) { return num(String(r.wrk), r.wrk ? '' : 'dim'); } },
    { id: 'fills', h: 'Fills', w: 48, hc: 'n', f: function (r) { return num(String(r.fills)); } },
    // contracts filled today, whole (the exact count on the title): '360.03' did not fit 56 px in SF Mono
    { id: 'ctrs', h: 'Ctrs', w: 56, hc: 'n', m: 'dw', f: function (r) { var c = num(F.whole(r.ctrs)); c.title = F.size(r.ctrs); return c; } },
    { id: 'fillp', h: 'Fill%', w: 48, hc: 'n', m: 'dw', f: function (r) { return num(r.sent ? F.pct(r.hit / r.sent) : ''); } },
    { id: 'thr', h: 'Thr' + C, w: 48, hc: 'n', m: 'w', title: 'net edge per contract an IOC lane needs to take', f: function (r) { return num(has(r.thr) ? (r.thr / 100).toFixed(1) : '', 'dim'); } },
    // Pred and Rlzd over the same fills (a fair and a matured 60 s markout) once there are any,
    // so the two columns compare like with like, as the amber rule does
    { id: 'pred', h: 'Pred' + C, w: 52, hc: 'n', title: 'predicted net edge, cents per contract', f: function (r) {
      var mo = moOf(r), c = mo.pairN ? centsC(mo.pairPred, mo.pairN) : centsC(mo.pred, mo.cp);
      if (r.warn) c.cls += ' warn'; return c;
    } },
    { id: 'rlzd', h: 'Rlzd' + C, w: 52, hc: 'n', title: 'realized net edge (60 s markout less fees), cents per contract', f: function (r) {
      var mo = moOf(r), c = mo.pairN ? centsC(mo.pairRlzd, mo.pairN) : centsC(mo.rlzd, mo.c60);
      if (r.warn) c.cls += ' warn'; return c;
    } },
    { id: 'mo10', h: 'MO10' + C, w: 52, hc: 'n', f: function (r) { return centsC(moOf(r).m10, moOf(r).c10); } },
    { id: 'mo60', h: 'MO60' + C, w: 52, hc: 'n', f: function (r) { return centsC(moOf(r).m60, moOf(r).c60); } },
    { id: 'fees', h: 'Fees', w: 52, hc: 'n', f: function (r) { return moneyC(r.fees, r.fees ? '' : 'dim'); } },
    { id: 'real', h: 'Real', w: 60, hc: 'n', m: 'dw', f: function (r) { return pnlC(r.real); } },
    { id: 'unrl', h: 'Unrl', w: 60, hc: 'n', m: 'w', f: function (r) { return pnlC(r.unrl); } },
    // trading P&L (real + unrl) with LIP in its own column, so a reward cannot hide losing fills;
    // Day = P&L + LIP (what the header's DAY S adds up)
    { id: 'pnl', h: 'P&L', w: 60, hc: 'n', f: function (r) { return pnlC(r.pnl); } },
    { id: 'lip', h: 'LIP', w: 52, hc: 'n', f: function (r) { return r.lip ? num(F.money(r.lip), signOf(r.lip, 100)) : blank(); } },
    { id: 'day', h: 'Day', w: 60, hc: 'n', m: 'dw', f: function (r) { return pnlC(r.day); } },
    { id: 'last', h: 'Last', w: 74, hc: 'n', m: 'dw', title: 'time of the lane\'s last signal', f: function (r) { return num(F.time(r.last_ms), 'dim'); } },
    { id: 'why', h: 'Reason', ww: 360, f: function (r) { return txt(r.why, 'dim', r.why || null); } },
    // the lane KILL: grey, red while the lane has live orders (and under the pointer)
    { id: 'kill', h: '', w: 40, f: function (r) {
      if (r.kind !== 'lane') return { t: '', cls: '', attr: NOACT };
      var any = r.onShown || r.sysLive > 0;
      return { t: 'KILL', cls: 'kl' + (r.sysLive > 0 ? ' live' : '') + (any ? '' : ' dis'), attr: { 'data-act': 'lkill', 'data-id': r.id } };
    } }
  ];
  var SUMS = ['budget', 'used', 'open', 'wrk', 'sent', 'hit', 'fills', 'ctrs', 'fees', 'real', 'unrl', 'day', 'lip', 'cap_left'];
  function laneRows() {
    var S = Desk.store, rows = [], h = S.hdr;
    var live = {};
    S.orders.forEach(function (o) { if (!o.terminal && o.own === 'SYS' && o.lane) live[o.lane] = (live[o.lane] || 0) + 1; });
    var runSt = h ? (h.run.state === 'STOP' ? 'OFF' : h.run.state) : '';
    var T = { kind: 'total', id: '__total', label: 'TOTAL', title: null, state: runSt, style: '', venue: '',
              thr: null, last_ms: null, why: '', mo: agg.all, warn: false };
    SUMS.forEach(function (k) { T[k] = 0; });
    var n = 0;
    S.laneOrder.forEach(function (id) {
      var l = S.lanes.get(id);
      if (!l) return;
      n++;
      var mo = agg.lanes.get(id) || EMPTY;
      // a lane switched on while Systematic does not run: IDLE (OFF is for a lane switched off)
      var idle = l.state === 'OFF' && laneOn(l) && !(h && h.run.state === 'RUN');
      rows.push({
        kind: 'lane', id: id, label: id + ' ' + l.name, title: id + ' ' + l.name + ' ' + l.venue + ' ' + l.style,
        state: idle ? 'IDLE' : l.state, stateWhy: idle ? 'on; Systematic ' + (h ? h.run.state : '') : null, style: l.style, venue: l.venue,
        budget: l.budget, used: l.used, open: l.open, wrk: l.wrk, sent: l.sent, hit: l.hit, fills: l.fills, ctrs: l.ctrs,
        fees: l.fees, real: l.real, unrl: l.unrl, pnl: l.real + l.unrl, day: l.day, lip: l.lip, cap_left: l.cap_left,
        thr: l.thr, last_ms: l.last_ms, why: l.why, mo: mo, warn: mo.warn,
        onShown: laneOn(l), pend: has(l.pendOn), sysLive: live[id] || 0
      });
      SUMS.forEach(function (k) { T[k] += l[k] || 0; });
      if (l.last_ms && (!T.last_ms || l.last_ms > T.last_ms)) T.last_ms = l.last_ms;
    });
    T.ctrs = Math.round(T.ctrs * 100) / 100;
    T.pnl = T.real + T.unrl;
    if (n) rows.push(T);
    return rows;
  }
  function renderLanes() {
    var g = grids.lanes; g.ensure();
    var rows = laneRows();
    g.draw(rows, function (r) { return r.kind === 'total' ? '__total' : 'L:' + r.id; }, function (r) {
      if (r.kind === 'total') return { cls: 'total', attr: { 'data-lane': '' } };
      return { cls: (filter === r.id ? 'sel' : '') + (r.warn ? ' lane-warn' : ''), attr: { 'data-lane': r.id } };
    });
    var on = 0, n = 0;
    Desk.store.lanes.forEach(function (l) { n++; if (laneOn(l)) on++; });
    D.setText(ui.lanesInfo, n ? 'ON ' + on + '/' + n : '');
  }

  function renderHeld() {
    var names = [];
    Desk.store.markets.forEach(function (m) { if (m.man && m.status === 'OPEN') names.push(m.short); });
    D.setText(ui.held, names.length ? 'MAN ' + names.length : '');
    D.setAttr(ui.held, 'title', names.length ? 'held by Manual (REL on its ladder): ' + names.join(', ') : null);
  }

  // ---------------------------------------------------------------------------------------------
  // WORKING (systematic orders, plus the last 3 s of finished ones)
  // ---------------------------------------------------------------------------------------------
  // the St cell: 'WORK', 'IOC 0/10' (an IOC whose rest was killed), 'CXL PULL', 'REJ PRICE_BAND'
  function stText(o) {
    if (!o.terminal) return o.st;
    if (o.st === 'CXL' && o.tif !== 'POST' && (o.why === 'IOC_REST' || o.why === 'FOK')) return o.tif + ' ' + F.size(o.filled) + '/' + F.size(o.qty);
    if (o.st === 'EXP') return 'EXP' + (o.filled > 0 ? ' ' + F.size(o.filled) + '/' + F.size(o.qty) : '');
    if (o.st === 'FILL') return 'FILL';
    return o.st + (o.why ? ' ' + o.why : '');
  }
  var NOLINK_X = { 'data-act': null, 'data-id': null, 'data-nolink': '1' };
  var WORK_COLS = [
    { id: 'time', h: 'Time', w: 72, f: function (o) { return num(F.time(o.t.recv || (o.local && o.local.at))); } },
    { id: 'lane', h: 'Lane', w: 56, f: function (o) { return txt(o.lane || ''); } },
    { id: 'v', h: 'V', w: 24, m: 'w', f: function (o) { return txt(o.v, 'dim'); } },
    { id: 'mkt', h: 'Market', ww: 280, f: function (o) { return txt(mname(o.key, o.tk), '', o.tk); } },
    { id: 'side', h: 'Side', w: 48, f: function (o) { return txt(o.side, 'side-' + o.side); } },
    { id: 'px', h: 'Px', w: 52, hc: 'n', f: function (o) { return num(pxOf(o.px, o.key)); } },
    { id: 'qty', h: 'Qty', w: 48, hc: 'n', f: function (o) { return num(F.size(o.qty)); } },
    { id: 'left', h: 'Left', w: 48, hc: 'n', f: function (o) {
      if (o.terminal) return num(F.size(o.left), 'dim');
      return num(o.filled > 0 ? F.size(o.left) + '(' + F.size(o.filled) + ')' : F.size(o.left));
    } },
    { id: 'piq', h: 'PIQ', w: 52, hc: 'n', m: 'dw', f: function (o) { return num(has(o.piq) && !o.terminal ? F.size(Math.round(o.piq)) : ''); } },
    { id: 'ttl', h: 'TTL', w: 40, hc: 'n', f: function (o, c) {
      if (o.terminal) return blank();
      if (o.tif !== 'POST') return num(o.tif, 'dim');
      return num(o.exp_ms ? F.secs(o.exp_ms - c.srv) : '');
    } },
    { id: 'edge', h: 'Edge' + C, w: 50, hc: 'n', m: 'dw', title: 'edge vs model fair at send, cents per contract', f: function (o) { return centsC(o.edge); } },
    { id: 'touch', h: 'Touch', w: 52, hc: 'n', m: 'w', f: function (o) { return num(pxOf(o.touch, o.key), 'dim'); } },
    { id: 'ack', h: 'Ack', w: 44, hc: 'n', m: 'w', f: function (o) { return num(o.t.ack && o.t.sent ? String(Math.max(0, o.t.ack - o.t.sent)) : '', 'dim'); } },
    { id: 'st', h: 'St', w: 80, f: function (o) {
      var t = stText(o);
      return { t: t, cls: 't st st-' + o.st + (o.terminal && o.why ? ' why' : ''), title: o.terminal && t !== o.st ? t : null };
    } },
    { id: 'x', h: '', w: 24, f: function (o) {
      if (o.terminal) return { t: '', cls: '', attr: NOLINK_X };
      return { t: 'x', cls: 'x', attr: { 'data-act': 'cxl', 'data-id': o.cid, 'data-nolink': '1' } };
    } }
  ];
  function byRecv(a, b) { return (b.t.recv || (b.local && b.local.at) || 0) - (a.t.recv || (a.local && a.local.at) || 0); }
  function sysPred(o) { return o.own === 'SYS' && (!filter || o.lane === filter); }
  function cxlable(o) { return !(o.local && o.local.cxlPend); }
  // a lane's routine ending: a requote's pull, an expiry, a fill (FILLS has it), a window closing.
  // These leave WORKING at once; a reject, a killed IOC or a cancel the operator or a limit caused
  // (KILL, STOP, HALT, LANE_OFF, TAKEOVER, USER, STP, POST_CROSS) stays its 3 s.
  function routineEnd(o) { return o.st === 'FILL' || o.st === 'EXP' || (o.st === 'CXL' && (o.why === 'PULL' || o.why === 'MARKET_CLOSED')); }
  function renderWorking(now) {
    var S = Desk.store, g = grids.work; g.ensure();
    var live = S.working(sysPred);
    var items = live.concat(S.recent(CFG.showTermMs, function (o) { return sysPred(o) && !routineEnd(o); })).sort(byRecv);
    g.draw(items, function (o) { return o.cid; }, function (o) {
      var cls = 'o-' + o.st + (o.local && o.local.cxlPend && !o.terminal ? ' cxlpend' : '');
      var sel = focusCls('s-work', o.cid, o.key);
      return { cls: cls + (sel ? ' ' + sel : ''), attr: { 'data-mkt': o.key, 'data-v': o.v } };
    }, { now: now, srv: srvNow(now) });
    D.setText(ui.workInfo, live.length ? String(live.length) : '');
    if (ui.cxlAll) ui.cxlAll.disabled = !live.some(cxlable);
    filterTag(ui.workTag);
  }

  // ---------------------------------------------------------------------------------------------
  // FILLS (systematic)
  // ---------------------------------------------------------------------------------------------
  function fillEdge(f) { return !has(f.fair) || !(f.qty > 0) ? null : (f.fair - f.px) * sgn(f.side) - (f.fee || 0) / f.qty; }
  var FILL_COLS = [
    { id: 'time', h: 'Time', w: 88, f: function (f) { return num(F.timeT(f.ts_ms)); } },
    { id: 'lane', h: 'Lane', w: 56, f: function (f) { return txt(f.lane || ''); } },
    { id: 'v', h: 'V', w: 24, m: 'w', f: function (f) { return txt(f.v, 'dim'); } },
    { id: 'mkt', h: 'Market', ww: 280, f: function (f) { return txt(mname(f.key, f.tk), '', f.tk); } },
    { id: 'side', h: 'Side', w: 48, f: function (f) { return txt(f.side, 'side-' + f.side); } },
    { id: 'px', h: 'Px', w: 52, hc: 'n', f: function (f) { return num(pxOf(f.px, f.key)); } },
    { id: 'qty', h: 'Qty', w: 48, hc: 'n', f: function (f) { return num(F.size(f.qty)); } },
    { id: 'liq', h: 'Liq', w: 32, m: 'dw', f: function (f) { return txt(f.liq, 'dim'); } },
    { id: 'fee', h: 'Fee', w: 48, hc: 'n', m: 'dw', f: function (f) { return moneyC(f.fee, f.fee ? '' : 'dim'); } },
    { id: 'fair', h: 'Fair', w: 52, hc: 'n', m: 'dw', f: function (f) { return num(pxOf(f.fair, f.key), 'fair'); } },
    { id: 'mid', h: 'Mid', w: 52, hc: 'n', m: 'w', f: function (f) { return num(pxOf(f.mid, f.key), 'dim'); } },
    { id: 'edge', h: 'Edge' + C, w: 54, hc: 'n', title: 'net edge vs model fair after the fee, cents per contract', f: function (f) { return centsC(fillEdge(f)); } },
    { id: 'mo1', h: 'MO1' + C, w: 52, hc: 'n', m: 'w', f: function (f) { return centsC(f.mo['1s']); } },
    { id: 'mo10', h: 'MO10' + C, w: 52, hc: 'n', f: function (f) { return centsC(f.mo['10s']); } },
    { id: 'mo60', h: 'MO60' + C, w: 52, hc: 'n', f: function (f) { return centsC(f.mo['60s']); } },
    { id: 'set', h: 'Settle' + C, w: 56, hc: 'n', m: 'w', f: function (f) { return centsC(f.mo.SETTLE); } },
    { id: 'real', h: 'Real', w: 56, hc: 'n', m: 'dw', f: function (f) { return f.real ? pnlC(f.real) : num(F.pnl(0), 'dim'); } }
  ];
  function renderFills() {
    var S = Desk.store, g = grids.fills; g.ensure();
    var items = [], fl = S.fills;
    for (var i = 0; i < fl.length && items.length < CFG.fillsMax; i++) {
      var f = fl[i];
      if (f.own === 'SYS' && (!filter || f.lane === filter)) items.push(f);
    }
    g.draw(items, function (f) { return f.fid; }, function (f) {
      return { cls: focusCls('s-fills', f.fid, f.key), attr: { 'data-mkt': f.key, 'data-v': f.v } };
    });
    var a = filter ? (agg.lanes.get(filter) || EMPTY) : agg.all;
    D.setText(ui.fillsInfo, a.n ? 'n ' + a.n + ' ctr ' + F.size(Math.round(a.ctr * 100) / 100) : '');
    filterTag(ui.fillsTag);
  }

  // ---------------------------------------------------------------------------------------------
  // RISK BOOK: grouped by real-world outcome (risk.groups), every open position, both owners.
  // A group of two or more markets gets a header row with the group's worst case and its members
  // under it; a one-market outcome is one row (the market's label; the group on its title, the
  // group's worst case in MaxL). Polymarket rows read '?'
  // while its key fails.
  // ---------------------------------------------------------------------------------------------
  function q(r, cell) { return r.unknown ? { t: '?', cls: 'n q' } : cell; }
  var RISK_COLS = [
    { id: 'name', h: 'Outcome / Market', f: function (r) {
      if (r.kind === 'grp') return txt(r.name, '', 'worst ' + F.pnl(r.worst) + ' best ' + F.pnl(r.best));
      if (r.kind === 'tot') return txt('TOTAL');
      if (r.solo) return txt(r.name, 'solo', r.gname + '  ' + r.tk + '  worst ' + F.pnl(r.worst) + ' best ' + F.pnl(r.best));
      return txt(r.m ? r.m.short : r.tk, 'mem', r.tk);
    } },
    { id: 'v', h: 'V', w: 30, f: function (r) { return txt(r.vtxt || '', 'dim'); } },
    { id: 'own', h: 'Own', w: 42, f: function (r) { return txt(r.own || ''); } },
    { id: 'pos', h: 'Pos', w: 54, hc: 'n', f: function (r) { if (r.kind !== 'mem') return blank(); var c = q(r, num(r.p ? F.pos6(r.p.pos) : '')); if (r.p && !r.unknown) c.title = F.ssize(r.p.pos); return c; } },
    { id: 'avg', h: 'Avg', w: 50, hc: 'n', m: 'dw', f: function (r) { return r.kind !== 'mem' ? blank() : q(r, num(r.p ? pxOf(r.p.avg, r.mkey) : '')); } },
    { id: 'mark', h: 'Mark', w: 50, hc: 'n', m: 'dw', f: function (r) { return r.kind !== 'mem' ? blank() : q(r, num(r.p ? pxOf(r.p.mark, r.mkey) : '')); } },
    { id: 'unrl', h: 'Unrl', w: 56, hc: 'n', f: function (r) { return q(r, pnlC(r.unrl)); } },
    { id: 'ify', h: 'IfYES', w: 56, hc: 'n', f: function (r) { return r.kind !== 'mem' ? blank() : q(r, pnlC(r.p ? r.p.if_yes : null)); } },
    { id: 'ifn', h: 'IfNO', w: 56, hc: 'n', f: function (r) { return r.kind !== 'mem' ? blank() : q(r, pnlC(r.p ? r.p.if_no : null)); } },
    { id: 'maxl', h: 'MaxL', w: 56, hc: 'n', f: function (r) { return q(r, moneyC(r.maxl, r.maxl ? '' : 'dim')); } },
    { id: 'rsv', h: 'Rsv', w: 56, hc: 'n', m: 'w', f: function (r) { return num(r.rsv ? F.money(r.rsv) : ''); } }
  ];
  function ownOf(set) { var k = Object.keys(set); if (!k.length) return ''; if (k.length > 1 || set.MIX) return 'MIX'; return k[0]; }
  function riskRows(rsv) {
    var S = Desk.store, ro = !!(S.hdr && S.hdr.cash.P.ro), rows = [];
    var T = { kind: 'tot', key: '__tot', unrl: 0, maxl: 0, rsv: 0, unknown: false };
    rsv.forEach(function (u) { T.rsv += u; });
    S.risk.groups.forEach(function (g) {
      var gr = { kind: 'grp', key: 'g:' + g.id, name: g.name, worst: g.worst, best: g.best, unrl: 0, rsv: 0, maxl: Math.max(0, -g.worst),
                 first: null, unknown: false };
      var own = {}, vs = {}, mem = [];
      g.members.forEach(function (key) {
        var p = S.position(key), m = S.market(key), sp = S.split(key);
        var r = { kind: 'mem', key: 'm:' + key, mkey: key, v: sp.v, vtxt: sp.v, tk: sp.tk, m: m, p: p, own: p && p.own ? p.own : '',
                  unrl: p ? p.unrl : null, maxl: p ? p.maxl : null, rsv: rsv.get(key) || 0, unknown: sp.v === 'P' && ro };
        gr.unrl += p ? p.unrl : 0; gr.rsv += r.rsv; vs[sp.v] = 1;
        if (p && p.own) own[p.own] = 1;
        if (!gr.first) gr.first = key;
        mem.push(r);
      });
      gr.own = ownOf(own); gr.vtxt = (vs.K ? 'K' : '') + (vs.P ? 'P' : '');
      gr.unknown = !!(vs.P && ro && !vs.K);
      T.unrl += gr.unrl; T.maxl += gr.maxl;
      if (mem.length === 1) {
        // one market: one row named after the outcome, carrying the group's worst case
        var one = mem[0];
        // (the market's own label names the outcome more exactly than the group: 'CHI <56', not
        // 'WX CHI 10/06'; the group is on the title)
        one.solo = true; one.name = one.m ? one.m.short : g.name; one.gname = g.name; one.worst = g.worst; one.best = g.best; one.maxl = gr.maxl;
        rows.push(one);
        return;
      }
      rows.push(gr);
      Array.prototype.push.apply(rows, mem);
    });
    return { rows: rows, total: T };
  }
  function renderRisk() {
    var g = grids.risk; g.ensure();
    var rsv = reservations(), rr = riskRows(rsv);
    var items = rr.rows.length ? rr.rows.concat([rr.total]) : [];
    g.draw(items, function (r) { return r.key; }, function (r) {
      if (r.kind === 'grp') return { cls: 'grp', attr: { 'data-mkt': r.first, 'data-v': null } };
      if (r.kind === 'tot') return { cls: 'total', attr: { 'data-mkt': null, 'data-v': null } };
      var sel = focusCls('s-risk', r.key, r.mkey);
      return { cls: (r.solo ? 'solo' : 'mem') + (sel ? ' ' + sel : ''), attr: { 'data-mkt': r.mkey, 'data-v': r.v } };
    });
    var n = Desk.store.risk.groups.length;
    D.setText(ui.riskInfo, n ? 'outcomes ' + n : '');
    lastRisk = rr.total;
  }
  // segmented one-line text (strip plan, risk summary, cash footer): keyed spans, no innerHTML
  function segRows(host, segs) {
    if (!host) return;
    D.keyedRows(host, segs, function (s) { return s.id; }, function (s) { return [s.t]; }, function (s) {
      return { cls: s.cls || '', attr: { title: s.title || null } };
    }, { row: 'span', cell: 'span' });
  }
  // the daily loss stop's room: 'loss' (STOP is the button)
  function stopSeg(h) {
    return { id: 'st', t: 'loss ' + F.money(h.stop.left) + '/' + F.money(h.stop.limit),
             cls: h.stop.hit ? 'crit-tx' : (h.stop.left < h.stop.limit / 2 ? 'warn' : '') };
  }
  function floorSeg(h, eq) {
    return { id: 'fl', t: 'floor ' + F.money(CFG.floorU) + ' eq ' + (eq === null ? '-' : F.money(eq)),
             cls: eq === null ? '' : (eq < CFG.floorU ? 'crit-tx' : (eq < CFG.floorU + h.stop.limit ? 'warn' : '')) };
  }
  // the risk book's totals line: risk (header figure), open worst case, reserved, stop, floor
  function riskSummary(eq) {
    var S = Desk.store, h = S.hdr, segs = [];
    if (h) {
      segs.push({ id: 'r', t: 'risk ' + F.money(h.risk), cls: 'tx0' });
      segs.push({ id: 'o', t: ' open ' + F.money(lastRisk.maxl) });
      segs.push({ id: 'v', t: ' rsv ' + F.money(lastRisk.rsv) });
      segs.push({ id: 's1', t: ' | ', cls: 'sep' });
      segs.push(stopSeg(h));
      segs.push({ id: 's2', t: ' | ', cls: 'sep' });
      segs.push(floorSeg(h, eq));
    }
    segRows(ui.riskSum, segs);
  }
  // cash footer: K S0 41.10 S1 0.00 S2 33.20 S3 31.20 | PM 98.70 RO (free amounts in WIDE)
  function cashLine() {
    var S = Desk.store, h = S.hdr, segs = [], wide = mode() === 'w';
    if (h) {
      var sh = S.risk.shards.length ? S.risk.shards : h.cash.K.shards.map(function (s) { return { v: 'K', id: s.id, cash: s.cash, free: s.free }; });
      segs.push({ id: 'K', t: 'K', cls: 'lbl' });
      sh.forEach(function (s) {
        if (s.v !== 'K') return;
        segs.push({ id: 'K' + s.id, t: ' ' + s.id + ' ' + F.money(s.cash) + (wide ? '/' + F.money(s.free) : ''),
                    cls: s.free < CFG.shardLowU ? 'warn' : 'tx0', title: s.id + ' cash ' + F.money(s.cash) + ' free ' + F.money(s.free) });
      });
      segs.push({ id: 'sep', t: ' | ', cls: 'sep' });
      var pf = null;
      sh.forEach(function (s) { if (s.v === 'P') pf = s; });
      segs.push({ id: 'P', t: 'PM ' + F.money(h.cash.P.total) + (wide ? '/' + F.money(pf ? pf.free : h.cash.P.free) : ''), cls: 'tx0' });
      segs.push({ id: 'ro', t: h.cash.P.ro ? ' RO' : '', cls: 'warn' });
    }
    segRows(ui.cashFoot, segs);
  }

  // ---------------------------------------------------------------------------------------------
  // MARKOUTS: one row per lane, ALL, then the recent fills (cents per contract, + = our way)
  // ---------------------------------------------------------------------------------------------
  function thin(n) { return n < CFG.thinN ? 'thin' : ''; }
  function isAgg(r) { return r.kind === 'lane' || r.kind === 'all'; }
  var MO_COLS = [
    { id: 'lane', h: 'Lane', f: function (r) {
      if (r.kind === 'fill') return txt((r.f.lane || '') + ' ' + mname(r.f.key, r.f.tk), '', F.time(r.f.ts_ms) + ' ' + r.f.tk);
      return txt(r.label, r.kind === 'lane' && r.mo.warn ? 'warn' : '');
    } },
    { id: 'n', h: 'n', w: 46, hc: 'n', f: function (r) {
      if (r.kind === 'grp') return blank();
      if (r.kind === 'fill') return num(F.size(r.f.qty), 'dim');
      return num(String(r.mo.n), thin(r.mo.n));
    } },
    { id: 'mo1', h: '+1s', w: 52, hc: 'n', f: function (r) { return r.kind === 'fill' ? centsC(r.f.mo['1s']) : r.kind === 'grp' ? blank() : centsC(r.mo.m1, r.mo.c1); } },
    { id: 'mo10', h: '+10s', w: 52, hc: 'n', f: function (r) { return r.kind === 'fill' ? centsC(r.f.mo['10s']) : r.kind === 'grp' ? blank() : centsC(r.mo.m10, r.mo.c10); } },
    { id: 'mo60', h: '+60s', w: 52, hc: 'n', f: function (r) { return r.kind === 'fill' ? centsC(r.f.mo['60s']) : r.kind === 'grp' ? blank() : centsC(r.mo.m60, r.mo.c60); } },
    { id: 'set', h: 'Settle', w: 52, hc: 'n', f: function (r) { return r.kind === 'fill' ? centsC(r.f.mo.SETTLE) : r.kind === 'grp' ? blank() : centsC(r.mo.mset, r.mo.cs); } },
    { id: 't60', h: 't60', w: 50, hc: 'n', f: function (r) { return isAgg(r) ? num(signed(r.mo.t60, has(r.mo.t60) && Math.abs(r.mo.t60) >= 10 ? 0 : 1), thin(r.mo.c60)) : blank(); } },
    { id: 'adv', h: 'Adv%', w: 48, hc: 'n', f: function (r) { return isAgg(r) ? num(F.pct(r.mo.adv), thin(r.mo.c60)) : blank(); } },
    { id: 'sprd', h: 'SprdPd', w: 56, hc: 'n', m: 'dw', f: function (r) {
      if (r.kind === 'fill') return has(r.f.mid) ? centsC((r.f.mid - r.f.px) * sgn(r.f.side)) : blank();
      if (r.kind === 'grp') return blank();
      return centsC(r.mo.sprd, r.mo.cm);
    } },
    { id: 'ctrs', h: 'Ctrs', w: 56, hc: 'n', m: 'w', f: function (r) { return isAgg(r) ? num(F.whole(r.mo.ctr)) : blank(); } },
    { id: 'feec', h: 'Fee/c', w: 52, hc: 'n', m: 'w', f: function (r) {
      if (r.kind === 'fill') return num(r.f.qty > 0 ? F.cents(r.f.fee / r.f.qty) : '', 'dim');
      return r.kind === 'grp' ? blank() : num(F.cents(r.mo.feePer));
    } },
    { id: 'net', h: 'Net60', w: 52, hc: 'n', m: 'w', f: function (r) {
      if (r.kind === 'fill') return has(r.f.mo['60s']) ? centsC(r.f.mo['60s'] - (r.f.fee || 0) / r.f.qty) : blank();
      return r.kind === 'grp' ? blank() : centsC(r.mo.rlzd, r.mo.c60);
    } }
  ];
  function moRows() {
    var S = Desk.store, rows = [];
    S.laneOrder.forEach(function (id) {
      var l = S.lanes.get(id); if (!l) return;
      rows.push({ kind: 'lane', key: 'L:' + id, id: id, label: id + ' ' + l.name, mo: agg.lanes.get(id) || EMPTY });
    });
    agg.lanes.forEach(function (mo, id) { if (!S.lanes.has(id)) rows.push({ kind: 'lane', key: 'L:' + id, id: id, label: id, mo: mo }); });
    if (rows.length || agg.all.n) rows.push({ kind: 'all', key: '__all', label: 'ALL', mo: agg.all });
    var recent = [], fl = S.fills;
    for (var i = 0; i < fl.length && recent.length < CFG.recentMo; i++) {
      var f = fl[i];
      if (f.own === 'SYS' && (!filter || f.lane === filter)) recent.push({ kind: 'fill', key: 'F:' + f.fid, f: f });
    }
    if (recent.length) { rows.push({ kind: 'grp', key: '__recent', label: 'RECENT' + (filter ? ' ' + filter : '') }); Array.prototype.push.apply(rows, recent); }
    return rows;
  }
  function renderMo() {
    var g = grids.mo; g.ensure();
    g.draw(moRows(), function (r) { return r.key; }, function (r) {
      if (r.kind === 'lane') return { cls: filter === r.id ? 'sel' : '', attr: { 'data-lane': r.id, 'data-mkt': null, 'data-v': null } };
      if (r.kind === 'all') return { cls: 'total', attr: { 'data-lane': '', 'data-mkt': null, 'data-v': null } };
      if (r.kind === 'grp') return { cls: 'grp', attr: { 'data-lane': null, 'data-mkt': null, 'data-v': null } };
      return { cls: focusCls('s-mo', r.key, r.f.key), attr: { 'data-lane': null, 'data-mkt': r.f.key, 'data-v': r.f.v } };
    });
    filterTag(ui.moTag);
  }

  // ---------------------------------------------------------------------------------------------
  // CONTROL STRIP: START / STOP, state, preflight tags, the failing reasons, the plan line
  // ---------------------------------------------------------------------------------------------
  function runText(h, now) {
    if (pendRun) return { t: pendRun.act === 'start' ? 'STARTING' : 'STOPPING', cls: 'pend' };
    if (!h) return { t: '', cls: 'stop' };
    var r = h.run, srv = srvNow(now);
    if (r.state === 'RUN') return { t: 'RUNNING ' + F.time(r.since_ms) + ' ' + F.dur(Math.max(0, srv - (r.since_ms || srv))), cls: 'run' };
    if (r.state === 'KILLED') return { t: r.rearm_ms && srv < r.rearm_ms ? 'KILLED re-arm ' + F.mmss(r.rearm_ms - srv) : 'KILLED RE-ARM', cls: 'killed' };
    if (r.state === 'HALTED') return { t: 'HALTED ' + (r.why || 'daily stop'), cls: 'halted' };
    return { t: 'STOPPED', cls: 'stop' };
  }
  // START is a new-order control: off while running, killed, halted, stale or any preflight
  // fails; the failing reasons sit next to the preflight tags. STOP, like cancel, stays live.
  function startGate(h) {
    var S = Desk.store;
    if (!h) return { block: true, why: S.stale ? 'STALE ' + S.staleWhy : '' };
    var why = [];
    if (S.stale) why.push('STALE ' + S.staleWhy);
    // DISK warns (red tag) but never blocks START: the recorder is not needed to trade (contract)
    preflightRows(h).forEach(function (r) { if (r.ok === false && r.id !== 'DISK') why.push(r.id + (r.why ? ' ' + r.why : '')); });
    if (!why.length && S.vstale.K) why.push(S.vstale.K);
    if (h.run.state !== 'STOP') return { block: true, why: h.run.state === 'RUN' ? '' : why.join(', ') };
    return { block: why.length > 0, why: why.join(', ') };
  }
  function setDis(btn, dis) { if (btn && btn.disabled !== !!dis) btn.disabled = !!dis; }
  function setCls(el, cls) { if (el && el.className !== cls) el.className = cls; }
  function renderStrip(now) {
    var S = Desk.store, h = S.hdr;
    if (pendRun && now - pendRun.at > CFG.pendMs) pendRun = null;
    var rv = runText(h, now);
    D.setText(ui.run, rv.t); setCls(ui.run, 's-run n ' + rv.cls);
    var gate = startGate(h);
    setDis(ui.start, gate.block || !!pendRun);
    setCls(ui.start, 'go' + (pendRun && pendRun.act === 'start' ? ' pend' : ''));
    D.setAttr(ui.start, 'title', gate.why || null);
    var sysLive = 0;
    S.orders.forEach(function (o) { if (!o.terminal && o.own === 'SYS') sysLive++; });
    var stopLive = !h || S.stale || h.run.state === 'RUN' || sysLive > 0 || (pendRun && pendRun.act === 'start');
    setDis(ui.stop, !stopLive || (pendRun && pendRun.act === 'stop'));
    setCls(ui.stop, 'stop' + (pendRun && pendRun.act === 'stop' ? ' pend' : ''));
    // preflight tags: green text when passing, red fill when failing, grey when unknown
    segRows(ui.pf, preflightRows(h).map(function (r) {
      return { id: r.id, t: r.id, cls: r.ok === null ? 'na' : (r.ok ? 'ok' : 'bad'), title: r.why || null };
    }));
    D.setText(ui.why, gate.why);
    D.setAttr(ui.why, 'title', gate.why || null);
    // plan: budget 70% K 99.62 P 69.09 RO | used 41.20 | cost 1.12/4.80 | P(stop) 12% | loss
    // (the equity floor is on the risk book's summary line; with SF Mono the whole line did not fit
    // the strip at 1512 px)
    var segs = [];
    if (h) {
      var K = h.sys.K, P = h.sys.P, ro = h.cash.P.ro;
      var kCut = K.eff < K.ceiling - 50, pCut = !ro && P.eff < P.ceiling - 50;
      segs.push({ id: 'b', t: 'budget ' + Math.round(CFG.ceilPct * 100) + '%' });
      segs.push({ id: 'bk', t: ' K ' + (kCut ? F.money(K.eff) + '/' : '') + F.money(K.ceiling), cls: kCut ? 'warn' : 'tx0',
                  title: 'K used ' + F.money(K.used) + ' eff ' + F.money(K.eff) + ' ceiling ' + F.money(K.ceiling) });
      segs.push({ id: 'bp', t: ' P ' + (pCut ? F.money(P.eff) + '/' : '') + F.money(P.ceiling), cls: pCut ? 'warn' : 'tx0',
                  title: 'P used ' + F.money(P.used) + ' eff ' + F.money(P.eff) + ' ceiling ' + F.money(P.ceiling) });
      segs.push({ id: 'ro', t: ro ? ' RO' : '', cls: 'warn' });
      segs.push({ id: 's1', t: ' | ', cls: 'sep' });
      var used = K.used + P.used, eff = K.eff + P.eff;
      segs.push({ id: 'u', t: 'used ' + F.money(used), cls: eff > 0 && used >= 0.9 * eff ? 'warn' : 'tx0' });
      segs.push({ id: 's2', t: ' | ', cls: 'sep' });
      segs.push({ id: 'c', t: 'cost ' + F.money(h.cost.used) + '/' + F.money(h.cost.budget),
                  cls: h.cost.used >= h.cost.budget ? 'crit-tx' : (h.cost.used >= 0.8 * h.cost.budget ? 'warn' : '') });
      segs.push({ id: 's3', t: ' | ', cls: 'sep' });
      var ps = h.stop.hit ? 1 : pStop(h.stop.left, CFG.sigmaDayU, dayLeft(now));
      segs.push({ id: 'p', t: 'P(stop) ' + F.pct(ps), cls: h.stop.hit ? 'crit-tx' : (ps >= 0.25 ? 'warn' : '') });
      segs.push({ id: 's4', t: ' | ', cls: 'sep' });
      segs.push(stopSeg(h));
    }
    segRows(ui.plan, segs);
  }

  // the amber filter tag in a panel title: 'M1 x' clears the lane filter
  function filterTag(el) {
    if (!el) return;
    D.setText(el, filter ? filter + ' x' : '');
    setCls(el, 'ftag' + (filter ? '' : ' off'));
  }

  // ---------------------------------------------------------------------------------------------
  // actions (each paints first, then sends)
  // ---------------------------------------------------------------------------------------------
  function runAct(act, ev) {
    if (pendRun) return;
    pendRun = { act: act, at: Date.now() };
    paintMeter(ev);
    R.mark('sys');
    var p = act === 'start' ? Desk.act.start() : Desk.act.stop();
    var done = function () { pendRun = null; R.mark('sys'); R.mark('hdr'); };
    if (p && p.then) p.then(done, done); else done();
  }
  function laneToggle(id, ev) {
    var l = Desk.store.lanes.get(id);
    if (!l) return;
    paintMeter(ev);
    Desk.act.lane(id, !laneOn(l));
  }
  // kill one lane: off, and its working orders cancelled now (CXL-PEND paints at once; the
  // backend's lane-off cancels them too)
  function laneKill(id, ev) {
    var S = Desk.store, l = S.lanes.get(id);
    if (!l) return;
    paintMeter(ev);
    if (laneOn(l)) Desk.act.lane(id, false);
    var cids = S.working(function (o) { return o.own === 'SYS' && o.lane === id && cxlable(o); }).map(function (o) { return o.cid; });
    if (cids.length) Desk.act.cancel(cids);
  }
  function cancelShown(ev) {
    var cids = Desk.store.working(function (o) { return sysPred(o) && cxlable(o); }).map(function (o) { return o.cid; });
    if (!cids.length) return;
    paintMeter(ev);
    Desk.act.cancel(cids);
  }
  // the lane's newest market: its newest working order, else its newest fill
  function laneMarket(id) {
    var S = Desk.store, w = S.working(function (o) { return o.own === 'SYS' && o.lane === id; });
    if (w.length) return w[0].key;
    for (var i = 0; i < S.fills.length; i++) if (S.fills[i].own === 'SYS' && S.fills[i].lane === id) return S.fills[i].key;
    return null;
  }
  function laneClick(lid, ev, panel) {
    if (!lid || filter === lid) { setFilter(null); return; }
    setFilter(lid);
    var key = laneMarket(lid);
    if (key && Desk.focus) Desk.focus.set(key, { other: !!(ev && ev.shiftKey), from: panel });
  }
  function onClick(ev) {
    var t = ev.target;
    if (!t || !t.closest) return;
    var a = t.closest('[data-act]');
    if (a) {
      if (a.disabled || (a.classList && a.classList.contains('dis'))) return;   // a disabled control swallows the click
      var act = a.getAttribute('data-act'), id = a.getAttribute('data-id');
      if (act === 'start' || act === 'stop') runAct(act, ev);
      else if (act === 'lane') laneToggle(id, ev);
      else if (act === 'lkill') laneKill(id, ev);
      else if (act === 'cxl') { var o = Desk.store.orders.get(id); if (o && !o.terminal && cxlable(o)) { paintMeter(ev); Desk.act.cancel([id]); } }
      else if (act === 'cxlall') cancelShown(ev);
      else if (act === 'fclear') setFilter(null);
      return;
    }
    var lr = t.closest('[data-lane]');
    if (lr) { var p = lr.closest('.panel'); laneClick(lr.getAttribute('data-lane'), ev, p ? p.id : null); }
  }

  // ---------------------------------------------------------------------------------------------
  // init and render
  // ---------------------------------------------------------------------------------------------
  function el(tag, cls, text, attrs) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text) e.textContent = text;
    if (attrs) for (var k in attrs) e.setAttribute(k, attrs[k]);
    return e;
  }
  function buildStrip() {
    var host = $('s-ctl-body');
    if (!host) return;
    host.textContent = '';
    ui.start = el('button', 'go', 'START', { id: 's-start', type: 'button', 'data-act': 'start' });
    ui.stop = el('button', 'stop', 'STOP', { id: 's-stop', type: 'button', 'data-act': 'stop' });
    ui.run = el('span', 's-run n stop', '', { id: 's-run' });
    ui.pf = el('span', 's-pf', '', { id: 's-pf' });
    ui.why = el('span', 's-why n', '', { id: 's-why' });
    ui.plan = el('span', 's-plan n seg', '', { id: 's-plan' });
    [ui.start, ui.stop, ui.run, ui.pf, ui.why, ui.plan].forEach(function (e) { host.appendChild(e); });
  }
  function buildTools() {
    var tag = function (host) { var b = el('button', 'ftag off', '', { type: 'button', 'data-act': 'fclear' }); if (host) host.appendChild(b); return b; };
    var info = function (host) { var s = el('span', 'pinfo n', ''); if (host) host.appendChild(s); return s; };
    var lt = $('s-lanes-tools'), wt = $('s-work-tools'), ft = $('s-fills-tools'), rt = $('s-risk-tools'), mt = $('s-mo-tools');
    [lt, wt, ft, rt, mt].forEach(function (h) { if (h) h.textContent = ''; });
    // markets Manual holds (mkt.man): the lanes stay out of them until REL on the ladder
    ui.held = el('span', 'pinfo n warn', '', { id: 's-held' }); if (lt) lt.appendChild(ui.held);
    ui.lanesInfo = info(lt);
    ui.workTag = tag(wt); ui.workInfo = info(wt);
    ui.cxlAll = el('button', '', 'CXL ALL', { type: 'button', 'data-act': 'cxlall', id: 's-cxlall' });
    if (wt) wt.appendChild(ui.cxlAll);
    ui.fillsTag = tag(ft); ui.fillsInfo = info(ft);
    ui.riskInfo = info(rt);
    ui.moTag = tag(mt);
    info(mt).textContent = 'c/ctr';
    // the risk book's totals line sits above the cash footer
    var foot = $('s-risk-foot');
    ui.cashFoot = foot;
    if (foot) foot.className = 'pfoot seg';
    ui.riskSum = $('s-risk-sum');
    if (!ui.riskSum && foot && foot.parentNode) { ui.riskSum = el('div', 'pfoot seg', '', { id: 's-risk-sum' }); foot.parentNode.insertBefore(ui.riskSum, foot); }
  }
  function bindKeys() {
    var K = Desk.keys;
    if (!K) return;
    K.bind({ key: 'Escape', scope: 'systematic', desc: 'clear the lane filter', fn: function () { setFilter(null); } });
    K.bind({ key: '1-9', scope: 'systematic', desc: 'filter to lane 1-9 (again: clear)', doc: true });
    for (var i = 1; i <= 9; i++) (function (n) {
      K.bind({ key: String(n), scope: 'systematic', fn: function () {
        var id = Desk.store.laneOrder[n - 1];
        if (id) setFilter(filter === id ? null : id);
      } });
    })(i);
  }

  var inited = false;
  function init() {
    if (inited || !hasDoc) return;
    inited = true;
    var saved = Desk.storage.get('sys.lane');
    filter = saved ? saved : null;
    buildStrip();
    buildTools();
    grids.lanes = new Grid('s-lanes-body', 'lanes', LANE_COLS);
    grids.work = new Grid('s-work-body', 'work', WORK_COLS);
    grids.fills = new Grid('s-fills-body', 'fills', FILL_COLS);
    grids.risk = new Grid('s-risk-body', 'risk', RISK_COLS);
    grids.mo = new Grid('s-mo-body', 'mo', MO_COLS);
    var rootEl = $('tab-systematic');
    if (rootEl) rootEl.addEventListener('click', onClick);
    bindKeys();
  }

  function render(d) {
    if (!inited) return;
    var S = Desk.store, now = Date.now();
    // a remembered filter for a lane the backend no longer has is dropped
    if (filter && S.laneOrder.length && !S.lanes.has(filter)) setFilter(null);
    var lay = d.all || d.has('layout');
    var sys = d.has('sys'), focus = d.has('focus'), clock = d.has('clock');
    var fills = d.has('fill') || d.has('mo') || d.has('snapshot');
    if (d.all || fills) recompute();
    var stripDirty = d.all || d.has('hdr') || clock || d.has('stale') || d.has('feeds') || sys || d.has('ord');
    var sumDirty = lay || clock || d.has('hdr') || d.has('pos') || d.has('risk') || d.has('ord');
    if (stripDirty) renderStrip(now);
    if (lay || d.has('lane') || fills || sys || d.has('hdr') || d.has('ord')) renderLanes();
    if (lay || d.has('mkt')) renderHeld();
    if (lay || d.has('ord') || clock || d.has('mkt') || focus || sys) renderWorking(now);
    if (lay || fills || d.has('mkt') || focus || sys) renderFills();
    if (lay || d.has('risk') || d.has('pos') || d.has('hdr') || d.has('ord') || d.has('mkt') || focus) { renderRisk(); cashLine(); }
    if (sumDirty) riskSummary(equity());
    if (lay || fills || sys || focus || d.has('lane') || d.has('mkt')) renderMo();
  }

  Desk.systematic = {
    init: init,
    render: render,
    cfg: CFG,
    get filter() { return filter; },
    setFilter: setFilter,
    // pure pieces and handles, exported for the node test
    _t: { ncdf: ncdf, pStop: pStop, dayLeft: dayLeft, reserve: reserve, newAcc: newAcc, addFill: addFill, stats: stats,
          equity: equity, reservations: reservations, preflightRows: preflightRows, agg: function () { return agg; },
          recompute: recompute, startGate: startGate, runText: runText, laneRows: laneRows, riskRows: riskRows,
          stText: stText, laneMarket: laneMarket, grids: grids, ui: ui, mode: mode }
  };
})(typeof window !== 'undefined' ? window : globalThis);
