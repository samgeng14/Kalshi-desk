/* ui/desk/js/activity.js
 *
 * ACTIVITY: what happened to our orders and positions, as a tape per mode, a header counter of
 * the events not seen yet, and optional desktop notifications with a short sound.
 *
 * Events are derived on the page from the store's records (contract events ord, fill, pos), so
 * the backend sends nothing new:
 *   SUB   an order was sent (MAN: at the click, from the pending order; SYS: its first record)
 *   FILL  a fill that completed its order;  PART  a fill that left some of it working
 *   CXL   cancelled (the why: USER, PULL, IOC_REST, KILL, STOP ...);  EXP  the venue's expiry
 *   REJ   rejected;  EXIT  a position went flat (closed, or settled), with the market's P&L
 * MAN is the operator's (EXT orders, placed outside the desk, count as his); SYS the lanes'.
 * A snapshot never makes events (a resync is not news); it only resets what the tape compares.
 *
 *   Desk.activity.list(mode, kind)  newest first; kind 'ALL' | 'FILLS' (FILL PART EXIT) | 'ORDERS'
 *   Desk.activity.unseen            {SYS: n, MAN: n}: events that pass that mode's alert filter
 *                                   and arrived while its tab was not on screen; .seen(mode)
 *   Desk.activity.cfg               {on (desktop notifications), sound, when: 'hidden' | 'always',
 *                                   SYS: [types], MAN: [types]} saved in this browser; defaults:
 *                                   off, no sound, only while the page is hidden, FILL and EXIT
 *   Desk.activity.enable()          asks for the browser's permission (call it from a click:
 *                                   Safari shows its prompt only for a user gesture)
 * Rate limit: at most one notification (and one sound) per mode every 4 s; what arrives in
 * between is batched into the next one ('SYS: 3 fills, 1 exit').
 */
(function (root) {
  'use strict';
  var Desk = root.Desk = root.Desk || {};
  var hasDoc = typeof document !== 'undefined';
  var MAX = 2000, RATE_MS = 4000;           // events kept per mode (busy lanes write ~350 a minute)
  var TYPES = ['SUB', 'FILL', 'PART', 'CXL', 'EXP', 'REJ', 'EXIT'];
  var KIND = { FILLS: { FILL: 1, PART: 1, EXIT: 1 }, ORDERS: { SUB: 1, CXL: 1, EXP: 1, REJ: 1 } };
  var A = {
    lists: { SYS: [], MAN: [] }, unseen: { SYS: 0, MAN: 0 }, n: 0, subbed: new Set(), sent: { SYS: 0, MAN: 0 }, pend: { SYS: [], MAN: [] }, timers: {},
    cfg: { on: false, sound: false, when: 'hidden', SYS: ['FILL', 'EXIT'], MAN: ['FILL', 'EXIT'] },
    notified: 0, beeps: 0, TYPES: TYPES, RATE_MS: RATE_MS
  };

  function now() { return Date.now(); }
  function modeOf(own) { return own === 'SYS' ? 'SYS' : 'MAN'; }
  function mark() { if (Desk.render) { Desk.render.mark('act'); Desk.render.mark('hdr'); } }
  // is the mode's tab on screen (and the page visible)?
  function onScreen(mode) {
    if (hasDoc && document.hidden) return false;
    var L = Desk.layout, tab = Desk.tabs ? Desk.tabs.cur : null;
    if (L && L.split) return true;
    return tab === (mode === 'SYS' ? 'systematic' : 'manual');
  }
  function push(ev) {
    ev.id = ++A.n; ev.ts = ev.ts || now();
    var l = A.lists[ev.mode];
    l.unshift(ev);
    if (l.length > MAX) l.length = MAX - 200;
    var hot = A.cfg[ev.mode].indexOf(ev.type) >= 0;
    if (hot && !onScreen(ev.mode)) A.unseen[ev.mode]++;
    if (hot) queue(ev);
    mark();
    return ev;
  }
  function base(o, type) {
    return { mode: modeOf(o.own), type: type, key: o.key, v: o.v, tk: o.tk, side: o.side, qty: o.qty, px: o.px, lane: o.lane || null,
             why: o.why || '', cid: o.cid, tif: o.tif, filled: o.filled, ext: o.own === 'EXT' };
  }

  // ---------------------------------------------------------------------------------------------
  // the store's hooks
  // ---------------------------------------------------------------------------------------------
  // a manual order painted pending at the click
  A.onPend = function (o) {
    if (A.subbed.has(o.cid)) return;
    A.subbed.add(o.cid); trim();
    push(base(o, 'SUB'));
  };
  // an order record (stream, reply or snapshot); old = the record it replaces (or null)
  A.onOrd = function (o, old, fromSnap) {
    if (fromSnap) { A.subbed.add(o.cid); trim(); return; }
    if (!A.subbed.has(o.cid)) { A.subbed.add(o.cid); trim(); push(base(o, 'SUB')); }
    var was = old ? old.st : null;
    if (was === o.st) return;
    if (o.st === 'CXL' || o.st === 'EXP' || (o.st === 'REJ' && !(old && old.local && old.local.prov))) push(base(o, o.st));
  };
  // a fill: FILL when it completed its order, else PART
  A.onFill = function (f, fromSnap) {
    if (fromSnap) return;
    var o = Desk.store && Desk.store.orders.get(f.cid);
    var done = !o || o.st === 'FILL' || o.left <= 0;
    push({ mode: modeOf(f.own), type: done ? 'FILL' : 'PART', key: f.key, v: f.v, tk: f.tk, side: f.side, qty: f.qty, px: f.px, lane: f.lane || null,
           liq: f.liq, fee: f.fee, cid: f.cid, ts: now(), ext: f.own === 'EXT' });
  };
  // a position went flat (closed or settled): EXIT, with the market's realized P&L today
  A.onPos = function (p, old, fromSnap) {
    if (fromSnap || !old || !old.pos || p.pos) return;
    var m = Desk.store ? Desk.store.market(p.key) : null;
    push({ mode: old.own === 'SYS' ? 'SYS' : 'MAN', type: 'EXIT', key: p.key, v: p.v, tk: p.tk, side: old.pos > 0 ? 'SELL' : 'BUY', qty: Math.abs(old.pos),
           px: m && m.status === 'SETTLED' ? (m.result === 'YES' ? 10000 : 0) : p.mark, real: p.real, why: m && m.status === 'SETTLED' ? 'SETTLED ' + m.result : '',
           lane: null, ts: now() });
  };
  function trim() { if (A.subbed.size > 20000) { var it = A.subbed.values(); for (var i = 0; i < 10000; i++) A.subbed.delete(it.next().value); } }

  A.list = function (mode, kind) {
    var l = A.lists[mode] || [], k = KIND[kind];
    return k ? l.filter(function (e) { return !!k[e.type]; }) : l;
  };
  A.seen = function (mode) { if (A.unseen[mode]) { A.unseen[mode] = 0; mark(); } };
  A.total = function () { return A.unseen.SYS + A.unseen.MAN; };

  // ---------------------------------------------------------------------------------------------
  // notifications and sound, rate-limited per mode
  // ---------------------------------------------------------------------------------------------
  var F = function () { return Desk.fmt; };
  function mname(e) { var m = Desk.store ? Desk.store.market(e.key) : null; return m ? m.short : e.tk; }
  A.line = function (e) {
    var f = F(), px = e.px === null || e.px === undefined ? '' : ' @' + f.px(e.px);
    if (e.type === 'EXIT') return 'EXIT ' + f.size(e.qty) + ' ' + mname(e) + (e.why ? ' ' + e.why : '') + ' real ' + f.pnl(e.real);
    return e.type + ' ' + (e.lane ? e.lane + ' ' : '') + e.side + ' ' + f.size(e.qty) + ' ' + mname(e) + px + (e.liq ? ' ' + e.liq : '') + (e.why && e.type !== 'FILL' ? ' ' + e.why : '');
  };
  function wanted() {
    if (A.cfg.when === 'always') return true;
    if (!hasDoc) return false;
    return !!document.hidden || (typeof document.hasFocus === 'function' && !document.hasFocus());
  }
  function queue(ev) {
    if (!(A.cfg.on || A.cfg.sound)) return;
    var mode = ev.mode;
    A.pend[mode].push(ev);
    var wait = A.sent[mode] + RATE_MS - now();
    if (wait <= 0) { flush(mode); return; }
    if (!A.timers[mode]) A.timers[mode] = setTimeout(function () { A.timers[mode] = null; flush(mode); }, wait);
  }
  function flush(mode) {
    var evs = A.pend[mode]; A.pend[mode] = [];
    if (!evs.length || !wanted()) return;
    A.sent[mode] = now();
    var title, body;
    if (evs.length === 1) { title = mode + ' ' + evs[0].type + (evs[0].lane ? ' ' + evs[0].lane : ''); body = A.line(evs[0]); }
    else {
      var cnt = {};
      evs.forEach(function (e) { cnt[e.type] = (cnt[e.type] || 0) + 1; });
      title = mode + ': ' + Object.keys(cnt).map(function (k) { return cnt[k] + ' ' + k.toLowerCase() + (cnt[k] > 1 ? 's' : ''); }).join(', ');
      body = evs.slice(0, 3).map(A.line).join('\n') + (evs.length > 3 ? '\n+' + (evs.length - 3) + ' more' : '');
    }
    var N = root.Notification;
    if (A.cfg.on && N && N.permission === 'granted') {
      try {
        var n = new N(title, { body: body, tag: 'desk-' + mode, silent: true });
        A.notified++;
        var e0 = evs[0];
        n.onclick = function () {
          try { if (root.focus) root.focus(); } catch (x) { /* ignore */ }
          if (Desk.tabs) Desk.tabs.set(mode === 'SYS' ? 'systematic' : 'manual');
          if (Desk.focus && e0.key) Desk.focus.set(e0.key, { from: 'notify' });
          if (n.close) n.close();
        };
      } catch (x) { /* a browser that cannot show it now */ }
    }
    if (A.cfg.sound) beep(evs[0].type);
  }
  // a short tone (Web Audio, no files): fills high, exits middle, rejects low
  var actx = null;
  function audio() {
    if (actx) return actx;
    var AC = root.AudioContext || root.webkitAudioContext;
    if (!AC) return null;
    try { actx = new AC(); } catch (e) { actx = null; }
    return actx;
  }
  function beep(type) {
    var c = audio(); if (!c) return;
    try {
      if (c.state === 'suspended' && c.resume) c.resume();
      var o = c.createOscillator(), g = c.createGain(), t = c.currentTime;
      o.frequency.value = type === 'REJ' ? 330 : (type === 'EXIT' ? 660 : 880);
      g.gain.setValueAtTime(0.0001, t); g.gain.exponentialRampToValueAtTime(0.06, t + 0.01); g.gain.exponentialRampToValueAtTime(0.0001, t + 0.09);
      o.connect(g); g.connect(c.destination); o.start(t); o.stop(t + 0.1);
      A.beeps++;
    } catch (e) { /* no audio */ }
  }

  // ---------------------------------------------------------------------------------------------
  // settings (safe storage)
  // ---------------------------------------------------------------------------------------------
  A.save = function () { if (Desk.storage) Desk.storage.setJSON('alerts', A.cfg); if (Desk.render) Desk.render.mark('set'); };
  A.load = function () {
    var s = Desk.storage ? Desk.storage.getJSON('alerts', null) : null;
    if (!s || typeof s !== 'object') return;
    A.cfg.on = !!s.on; A.cfg.sound = !!s.sound; A.cfg.when = s.when === 'always' ? 'always' : 'hidden';
    ['SYS', 'MAN'].forEach(function (m) { if (Array.isArray(s[m])) A.cfg[m] = s[m].filter(function (t) { return TYPES.indexOf(t) >= 0; }); });
  };
  A.toggleType = function (mode, type) {
    var l = A.cfg[mode], i = l.indexOf(type);
    if (i >= 0) l.splice(i, 1); else l.push(type);
    A.save();
  };
  // the browser's permission: 'unsupported' | 'default' | 'granted' | 'denied'
  A.permission = function () { var N = root.Notification; return N ? N.permission : 'unsupported'; };
  // turn desktop notifications on: asks for the permission from the click that called it
  A.enable = function () {
    var N = root.Notification;
    if (!N) { A.cfg.on = false; A.save(); return Promise.resolve('unsupported'); }
    var done = function (p) { A.cfg.on = p === 'granted'; A.save(); return p; };
    if (N.permission === 'granted') return Promise.resolve(done('granted'));
    if (N.permission === 'denied') return Promise.resolve(done('denied'));
    try {
      var r = N.requestPermission(function (p) { done(p); });     // Safari's older callback form
      if (r && r.then) return r.then(done);
    } catch (e) { /* the callback form already ran */ }
    return Promise.resolve(N.permission);
  };
  A.disable = function () { A.cfg.on = false; A.save(); };
  A.setSound = function (on) { A.cfg.sound = !!on; if (on) { audio(); if (actx && actx.resume) try { actx.resume(); } catch (e) { /* later */ } } A.save(); };
  A.setWhen = function (w) { A.cfg.when = w === 'always' ? 'always' : 'hidden'; A.save(); };
  A.test = function (mode) { A.sent[mode] = 0; var keep = A.cfg.when; A.cfg.when = 'always'; queue({ mode: mode, type: 'FILL', side: 'BUY', qty: 1, px: 5000, tk: 'TEST', key: 'K|TEST', ts: now() }); A.cfg.when = keep; };

  // ---------------------------------------------------------------------------------------------
  // the ACTIVITY tape (one panel per tab): newest first, 200 rows, coloured by side and state;
  // [ALL] [FILLS] [ORDERS] in its title (remembered); a row links to its market (data-mkt)
  //   A.mount({mode, body, tools, panel, lane: true}) -> a renderer fn(d)
  // ---------------------------------------------------------------------------------------------
  var KINDS = [['ALL', 'ALL'], ['FILLS', 'FILLS'], ['ORDERS', 'ORDERS']];
  function info(e) {
    var f = F();
    if (e.type === 'FILL' || e.type === 'PART') return (e.liq || '') + (e.fee ? ' ' + f.money(e.fee) : '');
    if (e.type === 'EXIT') return f.pnl(e.real);
    if (e.type === 'SUB') return e.tif || '';
    return e.why || '';
  }
  A.mount = function (o) {
    if (!hasDoc || !o.body) return function () {};
    var D = Desk.dom, T = { kind: 'ALL', lastAt: 0, dirty: true };
    var saved = Desk.storage ? Desk.storage.get('act.' + o.mode) : null;
    if (saved && KIND[saved] || saved === 'ALL') T.kind = saved;
    // widths for Safari's SF Mono: 'HH:MM:SS' at 11 px is 54 px, plus padding
    var cols = [{ h: 'Time', w: 70 }];
    if (o.lane) cols.push({ h: 'Lane', w: 38 });
    cols.push({ h: 'Ev', w: 44 }, { h: 'Side', w: 44 }, { h: 'Qty', w: 42, cls: 'n' }, { h: 'Market' }, { h: 'Px', w: 50, cls: 'n' }, { h: 'Info', w: 72 });
    T.tb = D.table(o.body, cols, 'act');
    T.ncol = cols.length;
    if (o.tools) {
      o.tools.textContent = '';
      T.n = document.createElement('span'); T.n.className = 'pinfo n'; o.tools.appendChild(T.n);
      T.btns = KINDS.map(function (k) {
        var b = document.createElement('button'); b.setAttribute('type', 'button'); b.setAttribute('data-a', 'kind'); b.setAttribute('data-v', k[0]); b.textContent = k[1];
        o.tools.appendChild(b); return b;
      });
      D.onPress(o.tools, function (ev) {
        var b = ev.target && ev.target.closest ? ev.target.closest('button') : null; if (!b) return;
        T.kind = b.getAttribute('data-v'); if (Desk.storage) Desk.storage.set('act.' + o.mode, T.kind);
        T.dirty = true; Desk.render.mark('act');
      }, function (ev) { return ev.target && ev.target.closest ? ev.target.closest('button') : null; });
    }
    T.render = function (d) {
      var t = now();
      if (hasDoc && !document.hidden) A.seen(o.mode);
      if (!(d.all || T.dirty || ((d.has('act') || d.has('mkt')) && t - T.lastAt >= 250) || (T.due && t - T.lastAt >= 250))) {
        if (d.has('act')) T.due = true;
        return;
      }
      T.lastAt = t; T.dirty = false; T.due = false;
      if (T.btns) T.btns.forEach(function (b) { D.setClass(b, 'on', b.getAttribute('data-v') === T.kind); });
      var lf = o.lane && Desk.systematic ? Desk.systematic.filter : null;
      var src = A.list(o.mode, T.kind), items = [];
      for (var i = 0; i < src.length && items.length < 200; i++) { var e = src[i]; if (!lf || !e.lane || e.lane === lf) items.push(e); }
      D.setText(T.n, A.lists[o.mode].length ? String(A.lists[o.mode].length) : '');
      if (!items.length) { D.emptyRow(T.tb, T.ncol); return; }
      var fm = F();
      D.keyedRows(T.tb, items, function (e) { return 'a' + e.id; }, function (e) {
        var m = Desk.store.market(e.key), g = m ? m.g : null;
        var cells = [{ t: fm.time(e.ts), cls: 'n tm', nf: 'sz' }];
        if (o.lane) cells.push({ t: e.lane || '', cls: 't', nf: 'sz' });
        cells.push({ t: e.type, cls: 't ev', nf: 'sz' },
                   { t: e.side || '', cls: 't side-' + e.side, nf: 'sz' },
                   { t: e.qty === undefined ? '' : fm.size(e.qty), cls: 'n', nf: 'sz' },
                   { t: (e.ext ? 'EXT ' : '') + (m ? m.short : e.tk), cls: 't', title: e.tk, nf: 'sz' },
                   { t: e.px === null || e.px === undefined ? '' : (g ? fm.pxg(e.px, g) : fm.px(e.px)), cls: 'n', nf: 'sz' },
                   { t: info(e), cls: e.type === 'EXIT' ? 'n ' + fm.sign(e.real) : 't dim', title: e.why || null, nf: 'sz' });
        return cells;
      }, function (e) {
        return { cls: 'a-' + e.type + (e.ext ? ' ext' : ''), attr: { 'data-mkt': e.key || null, 'data-v': e.v || null } };
      }, { isNew: function (e) { return now() - e.ts < 3000; } });
    };
    return T.render;
  };
  if (hasDoc) document.addEventListener('visibilitychange', function () { if (!document.hidden) mark(); });

  Desk.activity = A;
})(typeof window !== 'undefined' ? window : globalThis);
