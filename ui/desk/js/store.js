/* ui/desk/js/store.js
 *
 * The in-memory store. Stream handlers only write here and mark render topics dirty; renderers
 * read from here on the next animation frame. Every price and dollar amount is converted at the
 * door from the contract's strings to integers in u = $0.0001 (Desk.num), so nothing downstream
 * parses strings or compares floats.
 *
 * STATE (read-only for renderers)
 *   st.hdr                  header numbers (money fields in u) or null before the first snapshot
 *   st.markets  Map key -> {v, tk, key, ev, title, short, grp, gk, kind, og, oc, open_ms, close_ms,
 *                           status, result, grid (contract ranges), g (Desk.grid), fee {type, mult},
 *                           shard, lip, cat, date, pin, live}: the LIVE SET (contract 1.4); the rest
 *                           of the day's universe is paged in by Desk.universe, never stored here.
 *                           A mkt event with live false (contract 1.5: nothing holds the market on
 *                           the backend any more) drops its book, and the market too unless a
 *                           ladder, a position row or a working order holds it here (then it
 *                           stays, live false, for its name, until the backend brings it back or
 *                           the ET day changes)
 *   st.today    hb.date / snapshot.date: today's ET date (T); st.dayOf(date) -> 'T' (today or
 *               earlier, still open) | 'T1' (tomorrow exactly) | '' (later)
 *   st.pinPend  Map key -> {on, at}: a star clicked here, until the mkt echo agrees
 *   st.books    Map key -> {v, tk, key, depth 'FULL'|'TOP', bids [[px, size]] best first,
 *                           asks [[px, size]] best first, bb, ba, bbSz, baSz, mid (u, float) | null,
 *                           last {px, qty, side, ts_ms} | null, vol, prints [{px, qty, side, ts_ms, at}]
 *                           (last 10 s, 'at' = local receive time, for the LTQ column),
 *                           ref {fair, sd, espn, espn_ts_ms, game}, ex_ms, at (local receive ms)}
 *   st.orders   Map cid -> order (contract fields, prices/money in u, rev) plus:
 *                           key, terminal (bool), doneAt (local ms it became terminal),
 *                           local {pend: true until the backend has echoed it, cxlPend,
 *                                  prov: a REJ made here from a refusal with no ord (the backend
 *                                  may still report the order; any record of it replaces this),
 *                                  at (local ms created)}
 *   st.fills    Array newest first; each fill has mo {'1s','10s','60s','SETTLE'} -> u per contract
 *   st.pos      Map key -> position (money/prices in u)
 *   st.lanes    Map id -> lane (money in u), in the order first seen (st.laneOrder)
 *   st.risk     {groups [{id, name, worst, best, members: [key]}], shards [{v, id, cash, free}]}
 *   st.alerts   Array newest first (server alerts plus local ones with id 'L...')
 *   st.feeds    hb.feeds; st.lastHb local ms of the last hb; st.srvOffset server - local ms
 *   st.stale    true while the stream is down (EventSource error), silent > 3 s, gapped,
 *               resyncing, or after a snapshot until a stream hb newer than it has been applied
 *               (a snapshot alone never proves the stream is alive)
 *   st.staleWhy 'SYNC' | 'GAP' | 'SRV DOWN' | ''
 *   st.vstale   {K: '' | 'KX DOWN' | 'KX GAP', P: '' | 'PM DOWN' | 'PM GAP'}
 *   st.mode     'demo' | 'live'
 *
 * SELECTORS
 *   st.key(v, tk), st.split(key) -> {v, tk}
 *   st.market(key), st.book(key), st.position(key)
 *   st.liveMarket(key)      the market while it is in the live set (its book streams), else null
 *   st.working(pred?)       non-terminal orders, newest first
 *   st.recent(ms, pred?)    terminal orders that ended within ms (the 3 s CXL/REJ display)
 *   st.manWorking()         count of non-terminal MAN orders
 *   st.canTrade(v)          '' or the reason new orders are disabled (cancel and KILL stay live)
 *   st.priceGuard(key, side, px) -> '' or 'PRICE_BAND ...' (crossing more than 2 ticks)
 *
 * WRITES (the transport and Desk.act call these; renderers never do)
 *   st.begin(), st.apply(env), st.applySnapshot(snap), st.resync(why), st.streamReset(why),
 *   st.onTransport(state, isReconnect), st.tick(now), st.pend(req), st.reply(cid, status, json),
 *   st.cxlPend(cids, on), st.localAlert(lvl, src, msg)
 *   st.onSnapshot           optional fn(snap) after each applied snapshot (main.js: subscriptions)
 *   st.killPend             true from the KILL click until the backend answers (Desk.act.kill)
 */
(function (root) {
  'use strict';
  var Desk = root.Desk = root.Desk || {};
  var N = Desk.num, R = Desk.render;
  var px = N.px, money = N.money;
  var TERM = { FILL: 1, CXL: 1, REJ: 1, EXP: 1 };
  var KEEP_TERMINAL_MS = 120000;
  var HB_TIMEOUT_MS = 3000;
  var SETTLED_KEEP_MS = 30 * 60000;      // the snapshot's own rule: settled markets stay 30 min
  var PRUNE_EVERY_MS = 10000;
  // refusals the gateway makes before anything reaches a venue: with no ord in the reply the order
  // was never sent, so the page may show it REJ (provisionally). Anything else (EXCHANGE, INTERNAL,
  // a 5xx, a proxy page, no answer) leaves the order's fate unknown: it stays PEND and a snapshot
  // decides.
  var PRESEND = { BAD_REQUEST: 1, OFF_GRID: 1, BAD_QTY: 1, UNKNOWN_MARKET: 1, MARKET_CLOSED: 1, PRICE_BAND: 1, REDUCE: 1,
                  FORBIDDEN: 1, KILLED: 1, STALE: 1, RO: 1, NO_CASH: 1, SHARD_CASH: 1, SYS_CEILING: 1, HALTED: 1, BAD_STATE: 1, RATE: 1 };

  function mark(t) { if (R) R.mark(t); }
  function nowMs() { return Date.now(); }

  var st = Desk.store = {
    mode: null,
    epoch: null, sid: null, seq: null,
    hdr: null,
    markets: new Map(), books: new Map(), orders: new Map(),
    fills: [], fillIdx: new Map(), moPending: new Map(),
    pos: new Map(), lanes: new Map(), laneOrder: [],
    risk: { groups: [], shards: [] },
    alerts: [], feeds: null, subs: null, today: null, pinPend: new Map(),
    stale: true, staleWhy: 'SYNC', vstale: { K: '', P: '' },
    connected: false, resyncing: false, buffer: [], bufEpoch: null, inflight: false, needHb: false,
    lastHb: 0, srvOffset: 0, snapFails: 0, lastPrune: 0, killPend: false,
    gaps: 0, resyncs: 0, applied: 0, invalid: 0, lastInvalid: null,
    validate: true,               // check every event against Desk.contract (cheap; catches backend drift)
    fetchSnapshot: null,          // set by main.js: () => Promise<snapshot>
    onSnapshot: null,
    localN: 0,

    key: function (v, tk) { return v + '|' + tk; },
    split: function (key) { var i = key.indexOf('|'); return { v: key.slice(0, i), tk: key.slice(i + 1) }; },

    // ------------------------------------------------------------------------------------------
    // stream application
    // ------------------------------------------------------------------------------------------
    begin: function () {
      st.resyncing = true; st.buffer = []; st.seq = null; st.bufEpoch = null;
      st.recomputeStale(nowMs());
    },
    apply: function (env) {
      if (!env || typeof env.seq !== 'number' || !env.type) return 'bad';
      if (st.validate && Desk.contract) {
        var errs = Desk.contract.validateEvent(env);
        if (errs.length) {
          st.invalid++; st.lastInvalid = env.type + ' ' + errs[0];
          if (st.invalid <= 5) st.localAlert('CRIT', 'CONTRACT', env.type + ' #' + env.seq + ': ' + errs[0]);
          return 'invalid';
        }
      }
      if (env.type === 'hb') st.lastHb = nowMs();       // the socket talks (connected follows the EventSource)
      if (st.resyncing) {
        if (env.type === 'hb' && st.bufEpoch !== null && env.data.epoch !== st.bufEpoch) st.buffer = [];
        if (env.type === 'hb') st.bufEpoch = env.data.epoch;
        st.buffer.push(env);
        if (st.buffer.length > 20000) st.buffer.splice(0, 10000);
        return 'buffered';
      }
      if (env.type === 'hb' && st.epoch && env.data.epoch !== st.epoch) {
        st.streamReset('EPOCH'); st.buffer.push(env); st.bufEpoch = env.data.epoch; return 'epoch';
      }
      if (st.seq !== null) {
        if (env.seq <= st.seq) return 'dup';
        if (env.seq !== st.seq + 1) {
          st.gaps++;
          st.resync('GAP');
          st.buffer.push(env);
          return 'gap';
        }
      }
      st.seq = env.seq;
      st.dispatch(env);
      return 'ok';
    },
    dispatch: function (env) {
      st.applied++;
      var fn = H[env.type];
      if (fn) fn(env.data, env);
    },
    // Ask for a snapshot; buffer stream messages meanwhile; replay those newer than it.
    // delayMs: wait before asking (a snapshot that came back older than the stream backs off,
    // so a lagging backend cannot drive a tight request loop)
    resync: function (why, delayMs) {
      if (!st.resyncing) { st.resyncing = true; st.buffer = []; st.bufEpoch = null; }
      st.resyncWhy = why || 'GAP';
      st.recomputeStale(nowMs());
      if (delayMs) { setTimeout(function () { if (st.resyncing) st.resync(st.resyncWhy); }, delayMs); return; }
      st.resyncs++;
      if (!st.fetchSnapshot || st.inflight) return;
      st.inflight = true;
      var p;
      try { p = Promise.resolve(st.fetchSnapshot()); } catch (e) { p = Promise.reject(e); }
      p.then(function (snap) {
        st.inflight = false;
        try { st.finishResync(snap); } catch (e) { st.snapFailed('resync failed: ' + (e && e.message)); }
      }, function (e) {
        st.inflight = false;
        st.snapFailed(null, e);
      });
    },
    // a snapshot that could not be fetched, was refused or failed to apply: stay STALE and ask
    // again, backing off 1, 2, 4 ... 30 s (one CRIT line for the first few, not one per retry)
    snapFailed: function (msg) {
      st.snapFails++;
      if (msg && st.snapFails <= 3) st.localAlert('CRIT', 'SYNC', msg);
      var d = Math.min(30000, 1000 * Math.pow(2, Math.min(5, st.snapFails - 1)));
      if (!st.resyncing) { st.resyncing = true; st.buffer = []; st.bufEpoch = null; }
      st.recomputeStale(nowMs());
      setTimeout(function () { if (st.resyncing) st.resync(st.resyncWhy || 'GAP'); }, d);
    },
    // the stream restarted (reconnect, new epoch): its seq starts over
    streamReset: function (why) { st.seq = null; st.buffer = []; st.bufEpoch = null; st.resyncing = false; st.resync(why); },
    finishResync: function (snap) {
      var bad = (!snap || typeof snap.seq !== 'number') ? ['$: not a snapshot'] : null;
      if (!bad && st.validate && Desk.contract) {
        var errs = Desk.contract.validate(Desk.contract.snapshot, snap);
        if (errs.length) bad = errs;
      }
      // a snapshot that breaks the contract is never applied: the screen stays STALE
      if (bad) { st.invalid++; st.lastInvalid = 'snapshot ' + bad[0]; st.snapFailed('snapshot refused (contract): ' + bad[0]); return; }
      try { st.applySnapshot(snap); } catch (e) { st.snapFailed('snapshot failed: ' + (e && e.message)); return; }
      st.snapFails = 0;
      var buf = st.buffer.slice().sort(function (a, b) { return a.seq - b.seq; });
      st.buffer = []; st.resyncing = false;
      try {
        for (var i = 0; i < buf.length; i++) {
          var env = buf[i];
          if (env.type === 'hb' && env.data.epoch !== st.epoch) { st.seq = null; st.resyncing = true; st.buffer = buf.slice(i); st.resync('EPOCH', 250); return; }
          if (env.seq <= st.seq) continue;
          if (env.seq !== st.seq + 1) { st.gaps++; st.resyncing = true; st.buffer = buf.slice(i); st.resync('GAP', 250); return; }
          st.seq = env.seq; st.dispatch(env);
        }
      } catch (e2) {
        st.localAlert('CRIT', 'SYNC', 'replay failed: ' + (e2 && e2.message));
        st.resyncing = true; st.buffer = []; st.resync('GAP', 1000);
        return;
      }
      st.recomputeStale(nowMs());
      mark('snapshot');
      if (st.onSnapshot) { try { st.onSnapshot(snap); } catch (e3) { if (root.console) console.error('[desk store] onSnapshot', e3); } }
    },
    // Replace everything from a snapshot, all or nothing: the new maps are built in place and the
    // old ones put back if any record fails. It does not touch the stream's liveness: the screen
    // stays STALE (SYNC) until a stream hb newer than the snapshot is applied.
    applySnapshot: function (s) {
      var KEYS = ['epoch', 'sid', 'seq', 'today', 'srvOffset', 'hdr', 'markets', 'books', 'orders', 'fills', 'fillIdx', 'moPending',
                  'pos', 'lanes', 'laneOrder', 'risk', 'alerts', 'feeds', 'subs', 'needHb'];
      var saved = {};
      KEYS.forEach(function (k) { saved[k] = st[k]; });
      try {
        st.epoch = s.epoch; st.sid = s.sid; st.seq = s.seq; st.today = s.date;
        st.srvOffset = s.srv_ms - nowMs();
        H.hdr(s.hdr, null, true);
        st.markets = new Map(); s.markets.forEach(function (m) { H.mkt(m, null, true, saved.markets); });
        st.books = new Map(); s.books.forEach(function (b) { H.book(b, null, true); });
        // keep local orders the backend has not echoed yet (in flight), and refusals made here
        var keep = [], now = nowMs();
        saved.orders.forEach(function (o) { if (o.local && (o.local.pend || o.local.prov) && now - o.local.at < 15000) keep.push(o); });
        st.orders = new Map();
        s.orders.forEach(function (d) { H.ord(d, null, true, saved.orders.get(d.cid), true); });
        keep.forEach(function (o) { if (!st.orders.has(o.cid)) st.orders.set(o.cid, o); });
        st.fills = []; st.fillIdx = new Map(); st.moPending = new Map();
        s.fills.forEach(function (f) { H.fill(f, null, true); });
        s.mos.forEach(function (m) { H.mo(m, null, true); });
        st.pos = new Map(); s.pos.forEach(function (p) { H.pos(p, null, true); });
        st.lanes = new Map(); st.laneOrder = []; s.lanes.forEach(function (l) { H.lane(l, null, true); });
        H.risk(s.risk, null, true);
        var locals = saved.alerts.filter(function (a) { return a.local && now - a.at < 60000; });
        st.alerts = [];
        s.alerts.forEach(function (a) { H.alert(a, null, true); });
        locals.forEach(function (a) { st.alerts.unshift(a); });
        st.alerts.sort(function (a, b) { return b.ts_ms - a.ts_ms; });
        st.feeds = s.feeds; st.subs = s.subs;
        st.needHb = true;
      } catch (e) {
        KEYS.forEach(function (k) { st[k] = saved[k]; });
        throw e;
      }
      mark('snapshot');
    },
    // the EventSource opened (the first time, or again): a stream's seq starts over, so every open
    // takes a fresh snapshot
    onTransport: function (state, reconnect) {
      if (state === 'open') {
        st.connected = true;
        st.streamReset(reconnect ? 'RECONNECT' : 'BOOT');
      } else if (state === 'error' || state === 'closed') {
        st.connected = false;
      }
      st.recomputeStale(nowMs());
    },

    // ------------------------------------------------------------------------------------------
    // the STALE rule: socket liveness plus sequence continuity, never time since the last trade
    // ------------------------------------------------------------------------------------------
    recomputeStale: function (now) {
      var why = '';
      if (!st.connected || (st.lastHb && now - st.lastHb > HB_TIMEOUT_MS)) why = 'SRV DOWN';
      else if (st.resyncing) why = (st.resyncWhy === 'GAP' || st.resyncWhy === 'EPOCH' || st.resyncWhy === 'RECONNECT') ? 'GAP' : 'SYNC';
      else if (st.needHb) why = 'SYNC';
      var f = st.feeds, vk = '', vp = '';
      if (f) {
        if (!f.KX.up || !f.KXP.up) vk = 'KX DOWN'; else if (f.KX.gap || f.KXP.gap) vk = 'KX GAP';
        if (!f.PM.up) vp = 'PM DOWN'; else if (f.PM.gap) vp = 'PM GAP';
      }
      var changed = (why !== st.staleWhy) || vk !== st.vstale.K || vp !== st.vstale.P;
      st.staleWhy = why; st.stale = !!why; st.vstale.K = vk; st.vstale.P = vp;
      if (changed) mark('stale');
      return changed;
    },
    // A silent stream past the timeout greys the screen (SRV DOWN) and blocks new orders, nothing
    // more: it is not resynced from here, because a snapshot cannot prove the stream alive. If it
    // talks again with its seq in step nothing was lost; a gap, a new epoch or a reopened stream
    // (the transport reopens one that stays silent) resyncs.
    tick: function (now) {
      st.recomputeStale(now);
      // forget terminal orders and old prints
      var cut = now - KEEP_TERMINAL_MS;
      st.orders.forEach(function (o, cid) { if (o.terminal && o.doneAt < cut) st.orders.delete(cid); });
      st.books.forEach(function (b) { if (b.prints.length && b.prints[0].at < now - 10000) { b.prints = b.prints.filter(function (p) { return p.at >= now - 10000; }); } });
      if (now - st.lastPrune >= PRUNE_EVERY_MS) { st.lastPrune = now; st.prune(now); }
    },
    // forget markets settled more than 30 min ago, with their books, unless an open position or a
    // ladder holds them (the snapshot drops them by the same rule; a flat position row, kept for
    // the day, does not hold its market)
    prune: function (now) {
      var held = st.ladderKeys(), n = 0;
      st.markets.forEach(function (m, key) {
        var P = st.pos.get(key);
        // a record kept, not live, only for a ladder, a row or an order that has since gone
        if (m.live === false && !st.heldHere(key)) { st.markets.delete(key); st.books.delete(key); n++; return; }
        if (m.status !== 'SETTLED' || !m.settledAt || now - m.settledAt < SETTLED_KEEP_MS || held[key] || (P && P.pos)) return;
        st.markets.delete(key); st.books.delete(key); n++;
      });
      if (n) mark('mkt');
      return n;
    },
    ladderKeys: function () {
      var held = {};
      if (Desk.focus && Desk.focus.ladders) Object.keys(Desk.focus.ladders).forEach(function (s2) { var k = Desk.focus.ladders[s2]; if (k) held[k] = 1; });
      return held;
    },
    // something on this page still needs a market's record: a ladder, a position row (an open one,
    // or a flat one kept for the day: the Positions grid and the tape name it), a working order
    heldHere: function (key) {
      if (st.ladderKeys()[key] || st.pos.has(key)) return true;
      var any = false;
      st.orders.forEach(function (o) { if (!any && o.key === key && !o.terminal) any = true; });
      return any;
    },
    // KILLED first: it is the one an operator must act on, and a feed gap hid it
    canTrade: function (v) {
      var h = st.hdr;
      if (st.killPend || (h && h.run.state === 'KILLED')) return 'KILLED';
      if (st.stale) return st.staleWhy;
      if (v && st.vstale[v]) return st.vstale[v];
      if (h && v && h.cash[v] && h.cash[v].ro) return 'RO';
      return '';
    },

    // ------------------------------------------------------------------------------------------
    // selectors
    // ------------------------------------------------------------------------------------------
    market: function (key) { return st.markets.get(key) || null; },
    liveMarket: function (key) { var m = st.markets.get(key); return m && m.live !== false ? m : null; },
    book: function (key) { return st.books.get(key) || null; },
    position: function (key) { return st.pos.get(key) || null; },
    working: function (pred) {
      var out = [];
      st.orders.forEach(function (o) { if (!o.terminal && (!pred || pred(o))) out.push(o); });
      return out.sort(byNewest);
    },
    recent: function (ms, pred) {
      var cut = nowMs() - ms, out = [];
      st.orders.forEach(function (o) { if (o.terminal && o.doneAt >= cut && (!pred || pred(o))) out.push(o); });
      return out.sort(byNewest);
    },
    manWorking: function () { var n = 0; st.orders.forEach(function (o) { if (!o.terminal && o.own === 'MAN') n++; }); return n; },
    // THE UNIVERSE: the day a market belongs to against hb.date (contract 1.5): 'T' today or
    // earlier (yesterday's markets still open, e.g. a weather day that closes at 01:00), 'T1'
    // tomorrow exactly, '' anything later
    dayOf: function (date) {
      if (!st.today || !date || date <= st.today) return 'T';
      return date === st.tomorrow() ? 'T1' : '';
    },
    tomorrow: function () {
      if (st._tmK !== st.today) {
        st._tmK = st.today;
        var p = String(st.today).split('-'), d = new Date(Date.UTC(+p[0], +p[1] - 1, +p[2] + 1));
        st._tm = d.getUTCFullYear() + '-' + (d.getUTCMonth() < 9 ? '0' : '') + (d.getUTCMonth() + 1) + '-' + (d.getUTCDate() < 10 ? '0' : '') + d.getUTCDate();
      }
      return st._tm;
    },
    // a new ET day (hb.date moved on): flat position rows were today's, and go (contract: the
    // backend drops them too, and its snapshot leaves them out)
    newDay: function () {
      var n = 0;
      st.pos.forEach(function (P, key) { if (!P.pos) { st.pos.delete(key); n++; } });
      // and the records kept only for those rows (markets that left the live set)
      st.markets.forEach(function (m, key) { if (m.live === false && !st.heldHere(key)) { st.markets.delete(key); n++; } });
      if (n) { mark('pos'); mark('mkt'); }
      return n;
    },
    // a market's star as shown: an optimistic toggle in flight, else the backend's (mkt.pin, or a
    // listing row's pin)
    pinned: function (key, dflt) {
      var p = st.pinPend.get(key);
      if (p && Date.now() - p.at < 10000) return p.on;
      var m = st.markets.get(key);
      return m ? !!m.pin : !!dflt;
    },
    // a crossing order more than 2 ticks through the touch is refused unless overridden
    priceGuard: function (key, side, p) {
      var m = st.markets.get(key), b = st.books.get(key);
      if (!m || !b) return '';
      if (side === 'BUY' && b.ba !== null) {
        var lim = Desk.grid.step(m.g, b.ba, 2);
        if (p > lim) return 'PRICE_BAND ' + Desk.grid.ticks(m.g, b.ba, p) + ' ticks through ask ' + Desk.fmt.pxg(b.ba, m.g) + ' (max ' + Desk.fmt.pxg(lim, m.g) + ')';
      }
      if (side === 'SELL' && b.bb !== null) {
        var lo = Desk.grid.step(m.g, b.bb, -2);
        if (p < lo) return 'PRICE_BAND ' + Desk.grid.ticks(m.g, p, b.bb) + ' ticks through bid ' + Desk.fmt.pxg(b.bb, m.g) + ' (min ' + Desk.fmt.pxg(lo, m.g) + ')';
      }
      return '';
    },
    // ------------------------------------------------------------------------------------------
    // local writes (optimistic)
    // ------------------------------------------------------------------------------------------
    // req: {cid, v, tk, side, px (u), qty, tif, ttl_s, exp_ms, touch (u|null), t_click_ms}
    pend: function (req) {
      var now = nowMs(), key = req.v + '|' + req.tk;
      var o = {
        cid: req.cid, oid: null, v: req.v, tk: req.tk, key: key, side: req.side, px: req.px, qty: req.qty, filled: 0,
        left: req.qty, avg: null, tif: req.tif, exp_ms: req.tif === 'POST' && req.ttl_s ? now + req.ttl_s * 1000 : (req.tif === 'POST' && req.exp_ms ? req.exp_ms : null),
        st: 'PEND', why: '', own: 'MAN', lane: null, piq: null, fee: 0, touch: req.touch === undefined ? null : req.touch, edge: null,
        t: { click: req.t_click_ms || null, recv: null, sent: null, ack: null, done: null }, rev: 0,
        terminal: false, doneAt: null,
        local: { pend: true, cxlPend: false, prov: false, at: now, acked: false }
      };
      st.orders.set(o.cid, o);
      mark('ord'); mark('ord:' + key); mark('mord:' + key);
      if (Desk.activity) Desk.activity.onPend(o);            // the tape's SUB, at the click
      return o;
    },
    // the HTTP answer to POST /api/order; status 0 = no answer at all
    //   ok, or a refusal carrying ord      the backend's record, applied by its rev
    //   a pre-send refusal with no ord     never sent: REJ here, provisional (any backend record of
    //                                      the cid replaces it); the line goes to the status bar
    //   no answer, 5xx, EXCHANGE/INTERNAL   fate unknown: the order stays PEND and a snapshot
    //   with no ord                        decides (the backend may have sent it to the venue)
    // A refusal that carries its ord is logged here too, and its reason kept on the order (msg):
    // the backend raises no alert for a manual order (2026-10-14: a refusal on demo showed only
    // REJ STALE on the order, and no reason anywhere).
    reply: function (cid, status, json) {
      var o = st.orders.get(cid), ok = !!(json && json.ok), hasOrd = !!(json && json.ord);
      if (hasOrd) H.ord(json.ord, null, false);
      var e = (json && json.err) || { code: 'INTERNAL', msg: 'HTTP ' + status };
      var what = o ? o.side + ' ' + Desk.fmt.size(o.qty) + ' ' + o.tk + ' @ ' + Desk.fmt.px(o.px) : cid;
      if (!ok && hasOrd) {
        var o2 = st.orders.get(cid);
        if (o2 && e.msg) { o2.msg = e.msg; mark('mord:' + o2.key); }
        st.localAlert('WARN', 'GW', (o2 && o2.st === 'REJ' ? 'REJ ' : '') + e.code + ' ' + what + ': ' + e.msg);
      }
      if (!ok && !hasOrd) {
        if (status >= 400 && status < 500 && PRESEND[e.code]) {
          if (o && !o.terminal) {
            o.st = 'REJ'; o.why = Desk.contract && Desk.contract.enums.WHY.indexOf(e.code) >= 0 ? e.code : 'INTERNAL'; o.msg = e.msg || '';
            o.left = 0; o.terminal = true; o.doneAt = nowMs();
            if (o.local) { o.local.pend = false; o.local.prov = true; }
            mark('ord'); mark('ord:' + o.key); mark('mord:' + o.key);
            if (Desk.activity) Desk.activity.onOrd(o, { st: 'PEND' }, false);
          }
          st.localAlert('WARN', 'GW', 'REJ ' + e.code + ' ' + what + ': ' + e.msg);
        } else {
          st.localAlert('CRIT', 'GW', (status === 0 ? 'no answer' : e.code) + ' for ' + what + ': state unknown until the snapshot' + (status === 0 ? '' : ' (' + e.msg + ')'));
          st.resync('GAP');
          return;
        }
      }
      st.acked(cid);
    },
    acked: function (cid) {
      var o = st.orders.get(cid);
      if (!o || !o.local || o.local.acked) return;
      o.local.acked = true;
      if (Desk.lat) { var d = Desk.lat.acked(cid); if (d !== null) mark('lat'); }
    },
    cxlPend: function (cids, on) {
      cids.forEach(function (cid) {
        var o = st.orders.get(cid);
        if (!o || o.terminal) return;
        if (!o.local) o.local = { pend: false, cxlPend: false, prov: false, at: nowMs(), acked: true };
        o.local.cxlPend = on !== false;
        mark('ord:' + o.key);
        if (o.own !== 'SYS') mark('mord:' + o.key);
      });
      mark('ord');
    },
    localAlert: function (lvl, src, msg) {
      var a = { id: 'L' + (++st.localN), lvl: lvl, src: src, msg: msg, ts_ms: nowMs(), local: true, at: nowMs() };
      st.alerts.unshift(a);
      if (st.alerts.length > 500) st.alerts.length = 400;
      mark('alert');
      return a;
    }
  };
  function byNewest(a, b) { return (b.t.recv || b.local && b.local.at || 0) - (a.t.recv || a.local && a.local.at || 0); }

  // ------------------------------------------------------------------------------------------
  // per-event handlers: contract shape in, store shape out
  // ------------------------------------------------------------------------------------------
  function mapMoney(o, keys) { var r = {}; keys.forEach(function (k) { r[k] = money(o[k]); }); return r; }
  var H = {
    hdr: function (d, env, fromSnap) {
      var shards = d.cash.K.shards.map(function (s) { return { id: s.id, cash: money(s.cash), free: money(s.free) }; });
      st.hdr = {
        mode: d.mode,
        cash: { K: { total: money(d.cash.K.total), free: money(d.cash.K.free), ro: d.cash.K.ro, shards: shards },
                P: { total: money(d.cash.P.total), free: money(d.cash.P.free), ro: d.cash.P.ro } },
        day: mapMoney(d.day, ['total', 'sys', 'man', 'lip']),
        risk: money(d.risk),
        stop: { left: money(d.stop.left), limit: money(d.stop.limit), hit: d.stop.hit },
        sys: { K: mapMoney(d.sys.K, ['used', 'ceiling', 'eff']), P: mapMoney(d.sys.P, ['used', 'ceiling', 'eff']) },
        cost: mapMoney(d.cost, ['used', 'budget']),
        run: { state: d.run.state, since_ms: d.run.since_ms, rearm_ms: d.run.rearm_ms, why: d.run.why },
        preflight: d.preflight.map(function (r) { return { id: r.id, ok: r.ok, why: r.why }; }),
        lat: { kx_lag_ms: d.lat.kx_lag_ms, pm_lag_ms: d.lat.pm_lag_ms, gw_ack_ms: d.lat.gw_ack_ms, clk_off_ms: d.lat.clk_off_ms }
      };
      if (Desk.activity && Desk.activity.onRun) Desk.activity.onRun(st.hdr.run, !!fromSnap);   // a HALT on the tape
      mark('hdr');
    },
    // prev: the market map to carry the grid cache and the settle time from (a snapshot builds a new one)
    mkt: function (d, env, fromSnap, prev) {
      var key = d.v + '|' + d.tk, old = (prev || st.markets).get(key);
      var m = {
        v: d.v, tk: d.tk, key: key, ev: d.ev, title: d.title, short: d.short, grp: d.grp, gk: d.gk, kind: d.kind, og: d.og, oc: d.oc,
        open_ms: d.open_ms, close_ms: d.close_ms, status: d.status, result: d.result, grid: d.grid,
        g: old && JSON.stringify(old.grid) === JSON.stringify(d.grid) ? old.g : Desk.grid.make(d.grid),
        fee: { type: d.fee.type, mult: d.fee.mult }, shard: d.shard, lip: d.lip, start_ms: d.start_ms, man: d.man,
        cat: d.cat, date: d.date, pin: d.pin, live: d.live !== false,
        settledAt: d.status !== 'SETTLED' ? null : (old && old.settledAt ? old.settledAt : nowMs())   // local ms first seen SETTLED
      };
      // an optimistic star gives way once the backend says the same
      if (st.pinPend.has(key) && st.pinPend.get(key).on === m.pin) st.pinPend.delete(key);
      mark('mkt'); mark('mkt:' + key);
      if (!m.live && !fromSnap) {
        // the market left the backend's live set: its book stops, so the page forgets it (the
        // watchlist's ALL scope and the browser show the listing's row again); a ladder, an open
        // position or a working order here keeps the market itself, marked not live
        st.books.delete(key); mark('book'); mark('book:' + key); mark('uni');
        if (st.heldHere(key)) st.markets.set(key, m); else st.markets.delete(key);
        return;
      }
      st.markets.set(key, m);
    },
    book: function (d, env, fromSnap) {
      var key = d.v + '|' + d.tk, now = nowMs(), old = fromSnap ? null : st.books.get(key);
      var lv = function (a) { return a.map(function (l) { return [px(l[0]), l[1]]; }); };
      var bids = lv(d.bids), asks = lv(d.asks);
      var pr = function (t) { return t ? { px: px(t.px), qty: t.qty, side: t.side, ts_ms: t.ts_ms } : null; };
      var prints = old ? old.prints : [];
      if (d.trades.length && !fromSnap) {
        prints = prints.concat(d.trades.map(function (t) { var p = pr(t); p.at = now; return p; }));
        if (prints.length > 60) prints = prints.slice(-60);
      }
      var r = d.ref;
      var b = {
        v: d.v, tk: d.tk, key: key, depth: d.depth, bids: bids, asks: asks,
        bb: bids.length ? bids[0][0] : null, ba: asks.length ? asks[0][0] : null,
        bbSz: bids.length ? bids[0][1] : null, baSz: asks.length ? asks[0][1] : null, mid: null,
        last: pr(d.last), vol: d.vol, prints: prints,
        ref: { fair: px(r.fair), sd: px(r.sd), espn: px(r.espn), espn_ts_ms: r.espn_ts_ms,
               game: r.game ? { per: r.game.per, clock: r.game.clock, score: r.game.score, src: r.game.src } : null },
        ex_ms: d.ex_ms, at: now
      };
      if (b.bb !== null && b.ba !== null) b.mid = (b.bb + b.ba) / 2;
      st.books.set(key, b);
      mark('book'); mark('book:' + key);
    },
    // An order record from the stream, an HTTP reply or a snapshot (fromSnap: authoritative, the
    // backend's word as of the snapshot). The stream and the replies travel on different
    // connections, so they can arrive out of order: rev (one more on every change of the order)
    // decides, and an older record is dropped.
    ord: function (d, env, quiet, prev, fromSnap) {
      var old = prev || st.orders.get(d.cid);
      var prov = !!(old && old.local && old.local.prov);
      if (!fromSnap && old && !prov && typeof old.rev === 'number' && d.rev < old.rev) return;
      var o = {
        cid: d.cid, oid: d.oid, v: d.v, tk: d.tk, key: d.v + '|' + d.tk, side: d.side, px: px(d.px), qty: d.qty,
        filled: d.filled, left: d.left, avg: px(d.avg), tif: d.tif, exp_ms: d.exp_ms, st: d.st, why: d.why, own: d.own,
        lane: d.lane, piq: d.piq, fee: money(d.fee), touch: px(d.touch), edge: money(d.edge),
        t: { click: d.t.click, recv: d.t.recv, sent: d.t.sent, ack: d.t.ack, done: d.t.done }, rev: d.rev,
        terminal: !!TERM[d.st], doneAt: null, local: old && old.local ? old.local : null
      };
      // a terminal order never changes again; a late non-terminal echo cannot resurrect it (a
      // provisional REJ made here is not the backend's word and gives way)
      if (!fromSnap && old && old.terminal && !o.terminal && !prov) return;
      if (o.terminal) o.doneAt = old && old.terminal ? old.doneAt : nowMs();
      if (o.local) {
        o.local.pend = false; o.local.prov = false;
        if (o.terminal) o.local.cxlPend = false;
      }
      st.orders.set(o.cid, o);
      if (Desk.activity) Desk.activity.onOrd(o, old || null, !!fromSnap);
      if (o.st !== 'PEND' && !quiet) st.acked(o.cid);
      mark('ord'); mark('ord:' + o.key);
      if (o.own !== 'SYS') mark('mord:' + o.key);      // a manual (or foreign) order: painted at once
    },
    fill: function (d, env, fromSnap) {
      if (st.fillIdx.has(d.fid)) return;
      var f = {
        fid: d.fid, cid: d.cid, oid: d.oid, v: d.v, tk: d.tk, key: d.v + '|' + d.tk, side: d.side, px: px(d.px), qty: d.qty,
        liq: d.liq, fee: money(d.fee), ts_ms: d.ts_ms, own: d.own, lane: d.lane, mid: px(d.mid), fair: px(d.fair),
        real: money(d.real), mo: {}, at: fromSnap ? 0 : nowMs()       // at: local receive time (0 from a snapshot)
      };
      var pend = st.moPending.get(d.fid);
      if (pend) { pend.forEach(function (m) { f.mo[m.h] = m.mo; }); st.moPending.delete(d.fid); }
      st.fills.unshift(f); st.fillIdx.set(f.fid, f);
      if (st.fills.length > 5000) { var gone = st.fills.splice(4000); gone.forEach(function (x) { st.fillIdx.delete(x.fid); }); }
      if (Desk.activity) Desk.activity.onFill(f, !!fromSnap);
      mark('fill');
    },
    pos: function (d, env, fromSnap) {
      var key = d.v + '|' + d.tk, old = st.pos.get(key) || null;
      var p = {
        v: d.v, tk: d.tk, key: key, pos: d.pos, avg: px(d.avg), own: d.own, mark: px(d.mark), unrl: money(d.unrl),
        real: money(d.real), fees: money(d.fees), maxl: money(d.maxl), if_yes: money(d.if_yes), if_no: money(d.if_no)
      };
      st.pos.set(key, p);
      if (Desk.activity) Desk.activity.onPos(p, old, !!fromSnap);
      mark('pos'); mark('pos:' + key);
    },
    lane: function (d) {
      if (!st.lanes.has(d.id)) st.laneOrder.push(d.id);
      var old = st.lanes.get(d.id);
      var l = {
        id: d.id, name: d.name, venue: d.venue, state: d.state, on: d.on, style: d.style,
        budget: money(d.budget), used: money(d.used), open: money(d.open), wrk: d.wrk, sent: d.sent, hit: d.hit,
        fills: d.fills, ctrs: d.ctrs, fees: money(d.fees), real: money(d.real), unrl: money(d.unrl), day: money(d.day),
        lip: money(d.lip), cap_left: money(d.cap_left), thr: money(d.thr), last_ms: d.last_ms, why: d.why,
        pendOn: null
      };
      // an optimistic toggle stays shown until the backend agrees
      if (old && old.pendOn !== null && old.pendOn !== d.on && Date.now() - (old.pendAt || 0) < 5000) { l.pendOn = old.pendOn; l.pendAt = old.pendAt; }
      st.lanes.set(d.id, l);
      mark('lane');
    },
    risk: function (d) {
      st.risk = {
        groups: d.groups.map(function (g) {
          return { id: g.id, name: g.name, worst: money(g.worst), best: money(g.best), members: g.members.map(function (m) { return m.v + '|' + m.tk; }) };
        }),
        shards: d.shards.map(function (s) { return { v: s.v, id: s.id, cash: money(s.cash), free: money(s.free) }; })
      };
      mark('risk');
    },
    mo: function (d) {
      var f = st.fillIdx.get(d.fid), v = money(d.mo);
      if (f) f.mo[d.h] = v;
      else {
        var a = st.moPending.get(d.fid) || [];
        a.push({ h: d.h, mo: v }); st.moPending.set(d.fid, a);
        if (st.moPending.size > 2000) st.moPending.delete(st.moPending.keys().next().value);
      }
      mark('mo');
    },
    alert: function (d) {
      st.alerts.unshift({ id: d.id, lvl: d.lvl, src: d.src, msg: d.msg, ts_ms: d.ts_ms, local: false });
      if (st.alerts.length > 500) st.alerts.length = 400;
      mark('alert');
    },
    hb: function (d) {
      if (d.date !== st.today) { if (st.today) st.newDay(); st.today = d.date; mark('mkt'); }
      st.lastHb = nowMs();
      st.needHb = false;                   // a stream hb newer than the snapshot: the stream is live
      st.srvOffset = d.srv_ms - nowMs();
      st.feeds = d.feeds;
      st.recomputeStale(nowMs());
      mark('feeds');
    }
  };
  st._handlers = H;
})(typeof window !== 'undefined' ? window : globalThis);
