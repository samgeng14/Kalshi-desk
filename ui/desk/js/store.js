/* ui/desk/js/store.js
 *
 * The in-memory store. Stream handlers only write here and mark render topics dirty; renderers
 * read from here on the next animation frame. Every price and dollar amount is converted at the
 * door from the contract's strings to integers in u = $0.0001 (Desk.num), so nothing downstream
 * parses strings or compares floats.
 *
 * STATE (read-only for renderers)
 *   st.hdr                  header numbers (money fields in u) or null before the first snapshot
 *   st.markets  Map key -> {v, tk, key, ev, title, short, grp, kind, og, oc, open_ms, close_ms,
 *                           status, result, grid (contract ranges), g (Desk.grid), fee {type, mult},
 *                           shard, lip}
 *   st.books    Map key -> {v, tk, key, depth 'FULL'|'TOP', bids [[px, size]] best first,
 *                           asks [[px, size]] best first, bb, ba, bbSz, baSz, mid (u, float) | null,
 *                           last {px, qty, side, ts_ms} | null, vol, prints [{px, qty, side, ts_ms, at}]
 *                           (last 10 s, 'at' = local receive time, for the LTQ column),
 *                           ref {fair, sd, espn, espn_ts_ms, game}, ex_ms, at (local receive ms)}
 *   st.orders   Map cid -> order (contract fields, prices/money in u) plus:
 *                           key, terminal (bool), doneAt (local ms it became terminal),
 *                           local {pend: true until the backend has echoed it, cxlPend, amendPend,
 *                                  tClick (perf ms), at (local ms created)}
 *   st.fills    Array newest first; each fill has mo {'1s','10s','60s','SETTLE'} -> u per contract
 *   st.pos      Map key -> position (money/prices in u)
 *   st.lanes    Map id -> lane (money in u), in the order first seen (st.laneOrder)
 *   st.risk     {groups [{id, name, worst, best, members: [key]}], shards [{v, id, cash, free}]}
 *   st.alerts   Array newest first (server alerts plus local ones with id 'L...')
 *   st.feeds    hb.feeds; st.lastHb local ms of the last hb; st.srvOffset server - local ms
 *   st.stale    true while the stream is down, gapped, resyncing or silent > 3 s
 *   st.staleWhy 'SYNC' | 'GAP' | 'SRV DOWN' | ''
 *   st.vstale   {K: '' | 'KX DOWN' | 'KX GAP', P: '' | 'PM DOWN' | 'PM GAP'}
 *   st.mode     'demo' | 'live'
 *
 * SELECTORS
 *   st.key(v, tk), st.split(key) -> {v, tk}
 *   st.market(key), st.book(key), st.position(key)
 *   st.touch(key) -> {bb, ba, bbSz, baSz, mid} (nulls when empty)
 *   st.working(pred?)       non-terminal orders, newest first
 *   st.recent(ms, pred?)    terminal orders that ended within ms (the 3 s CXL/REJ display)
 *   st.workingAt(key)       {BUY: Map px -> {left, filled, n, pend, cxl, cids, exp}, SELL: Map}
 *   st.manWorking()         count of non-terminal MAN orders
 *   st.canTrade(v)          '' or the reason new orders are disabled (cancel and KILL stay live)
 *   st.priceGuard(key, side, px) -> '' or 'PRICE_BAND ...' (crossing more than 2 ticks)
 *   st.markouts(pred?)      {n, m1, m10, m60, mset, t60, adv, sprd} over fills (u per contract)
 *
 * WRITES (the transport and Desk.act call these; renderers never do)
 *   st.begin(), st.apply(env), st.applySnapshot(snap), st.resync(why), st.streamReset(why),
 *   st.onTransport(state), st.tick(now), st.pend(req), st.reply(cid, status, json),
 *   st.cxlPend(cids, on), st.localAlert(lvl, src, msg)
 */
(function (root) {
  'use strict';
  var Desk = root.Desk = root.Desk || {};
  var N = Desk.num, R = Desk.render;
  var px = N.px, money = N.money;
  var TERM = { FILL: 1, CXL: 1, REJ: 1, EXP: 1 };
  var KEEP_TERMINAL_MS = 120000;
  var HB_TIMEOUT_MS = 3000;

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
    alerts: [], feeds: null, subs: null,
    stale: true, staleWhy: 'SYNC', vstale: { K: '', P: '' },
    connected: false, resyncing: false, buffer: [], bufEpoch: null, inflight: false,
    lastHb: 0, srvOffset: 0,
    gaps: 0, resyncs: 0, applied: 0, invalid: 0, lastInvalid: null,
    validate: true,               // check every event against Desk.contract (cheap; catches backend drift)
    fetchSnapshot: null,          // set by main.js: () => Promise<snapshot>
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
      if (env.type === 'hb') { st.lastHb = nowMs(); st.connected = true; }
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
        st.finishResync(snap);
      }, function () {
        st.inflight = false;
        setTimeout(function () { if (st.resyncing) st.resync(st.resyncWhy); }, 5000);
      });
    },
    // the stream restarted (reconnect, new epoch): its seq starts over
    streamReset: function (why) { st.seq = null; st.buffer = []; st.bufEpoch = null; st.resyncing = false; st.resync(why); },
    finishResync: function (snap) {
      if (!snap || typeof snap.seq !== 'number') { setTimeout(function () { st.resync(st.resyncWhy); }, 1000); return; }
      if (st.validate && Desk.contract) {
        var errs = Desk.contract.validate(Desk.contract.snapshot, snap);
        if (errs.length) { st.invalid++; st.lastInvalid = 'snapshot ' + errs[0]; st.localAlert('CRIT', 'CONTRACT', 'snapshot: ' + errs[0]); }
      }
      st.applySnapshot(snap);
      var buf = st.buffer.slice().sort(function (a, b) { return a.seq - b.seq; });
      st.buffer = []; st.resyncing = false;
      for (var i = 0; i < buf.length; i++) {
        var env = buf[i];
        if (env.type === 'hb' && env.data.epoch !== st.epoch) { st.seq = null; st.resyncing = true; st.buffer = buf.slice(i); st.resync('EPOCH', 250); return; }
        if (env.seq <= st.seq) continue;
        if (env.seq !== st.seq + 1) { st.gaps++; st.resyncing = true; st.buffer = buf.slice(i); st.resync('GAP', 250); return; }
        st.seq = env.seq; st.dispatch(env);
      }
      st.recomputeStale(nowMs());
      mark('snapshot');
    },
    applySnapshot: function (s) {
      st.epoch = s.epoch; st.sid = s.sid; st.seq = s.seq;
      st.srvOffset = s.srv_ms - nowMs();
      st.lastHb = nowMs(); st.connected = true;
      H.hdr(s.hdr);
      st.markets = new Map(); s.markets.forEach(function (m) { H.mkt(m, null, true); });
      st.books = new Map(); s.books.forEach(function (b) { H.book(b, null, true); });
      // keep local orders the backend has not echoed yet (in flight)
      var keep = [];
      st.orders.forEach(function (o) { if (o.local && o.local.pend && nowMs() - o.local.at < 15000) keep.push(o); });
      var old = st.orders;
      st.orders = new Map();
      s.orders.forEach(function (d) { H.ord(d, null, true, old.get(d.cid)); });
      keep.forEach(function (o) { if (!st.orders.has(o.cid)) st.orders.set(o.cid, o); });
      st.fills = []; st.fillIdx = new Map(); st.moPending = new Map();
      s.fills.forEach(function (f) { H.fill(f, null, true); });
      s.mos.forEach(function (m) { H.mo(m, null, true); });
      st.pos = new Map(); s.pos.forEach(function (p) { H.pos(p, null, true); });
      st.lanes = new Map(); st.laneOrder = []; s.lanes.forEach(function (l) { H.lane(l, null, true); });
      H.risk(s.risk, null, true);
      var locals = st.alerts.filter(function (a) { return a.local && nowMs() - a.at < 60000; });
      st.alerts = [];
      s.alerts.forEach(function (a) { H.alert(a, null, true); });
      locals.forEach(function (a) { st.alerts.unshift(a); });
      st.alerts.sort(function (a, b) { return b.ts_ms - a.ts_ms; });
      st.feeds = s.feeds; st.subs = s.subs;
      mark('snapshot');
    },
    onTransport: function (state, reconnect) {
      if (state === 'open') {
        st.connected = true;
        if (reconnect) st.streamReset('RECONNECT');
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
    tick: function (now) {
      st.recomputeStale(now);
      // a silent stream past the timeout is a dead stream: resync when it talks again
      if (st.lastHb && now - st.lastHb > HB_TIMEOUT_MS && !st.resyncing && st.seq !== null) st.resync('GAP');
      // forget terminal orders and old prints
      var cut = now - KEEP_TERMINAL_MS;
      st.orders.forEach(function (o, cid) { if (o.terminal && o.doneAt < cut) st.orders.delete(cid); });
      st.books.forEach(function (b) { if (b.prints.length && b.prints[0].at < now - 10000) { b.prints = b.prints.filter(function (p) { return p.at >= now - 10000; }); } });
    },
    canTrade: function (v) {
      if (st.stale) return st.staleWhy;
      if (v && st.vstale[v]) return st.vstale[v];
      var h = st.hdr;
      if (h) {
        if (h.run.state === 'KILLED') return 'KILLED';
        if (v && h.cash[v] && h.cash[v].ro) return 'RO';
      }
      return '';
    },

    // ------------------------------------------------------------------------------------------
    // selectors
    // ------------------------------------------------------------------------------------------
    market: function (key) { return st.markets.get(key) || null; },
    book: function (key) { return st.books.get(key) || null; },
    position: function (key) { return st.pos.get(key) || null; },
    touch: function (key) {
      var b = st.books.get(key);
      if (!b) return { bb: null, ba: null, bbSz: null, baSz: null, mid: null };
      return { bb: b.bb, ba: b.ba, bbSz: b.bbSz, baSz: b.baSz, mid: b.mid };
    },
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
    workingAt: function (key) {
      var r = { BUY: new Map(), SELL: new Map() };
      st.orders.forEach(function (o) {
        if (o.key !== key || o.terminal) return;
        var m = r[o.side], e = m.get(o.px);
        if (!e) { e = { left: 0, filled: 0, n: 0, pend: false, cxl: false, cids: [], exp: null, own: {} }; m.set(o.px, e); }
        e.left += o.left; e.filled += o.filled; e.n++; e.cids.push(o.cid); e.own[o.own] = true;
        if (o.st === 'PEND') e.pend = true;
        if (o.local && o.local.cxlPend) e.cxl = true;
        if (o.exp_ms && (e.exp === null || o.exp_ms < e.exp)) e.exp = o.exp_ms;
      });
      return r;
    },
    manWorking: function () { var n = 0; st.orders.forEach(function (o) { if (!o.terminal && o.own === 'MAN') n++; }); return n; },
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
    // markout statistics over fills (u per contract, + = in our favour)
    markouts: function (pred) {
      var acc = { n: 0, s1: [], s10: [], s60: [], sset: [], sprd: [] };
      st.fills.forEach(function (f) {
        if (pred && !pred(f)) return;
        acc.n++;
        if (f.mo['1s'] !== undefined) acc.s1.push(f.mo['1s']);
        if (f.mo['10s'] !== undefined) acc.s10.push(f.mo['10s']);
        if (f.mo['60s'] !== undefined) acc.s60.push(f.mo['60s']);
        if (f.mo.SETTLE !== undefined) acc.sset.push(f.mo.SETTLE);
        if (f.mid !== null) acc.sprd.push((f.px - f.mid) * (f.side === 'BUY' ? 1 : -1));
      });
      var mean = function (a) { if (!a.length) return null; var s = 0; a.forEach(function (x) { s += x; }); return s / a.length; };
      var m60 = mean(acc.s60), t60 = null;
      if (acc.s60.length > 1) {
        var v = 0; acc.s60.forEach(function (x) { v += (x - m60) * (x - m60); }); v /= (acc.s60.length - 1);
        t60 = v > 0 ? m60 / Math.sqrt(v / acc.s60.length) : null;
      }
      return { n: acc.n, n60: acc.s60.length, m1: mean(acc.s1), m10: mean(acc.s10), m60: m60, mset: mean(acc.sset), t60: t60,
               adv: acc.s60.length ? acc.s60.filter(function (x) { return x < 0; }).length / acc.s60.length : null, sprd: mean(acc.sprd) };
    },

    // ------------------------------------------------------------------------------------------
    // local writes (optimistic)
    // ------------------------------------------------------------------------------------------
    // req: {cid, v, tk, side, px (u), qty, tif, ttl_s, exp_ms, touch (u|null), t_click_ms, tClick (perf)}
    pend: function (req) {
      var now = nowMs(), key = req.v + '|' + req.tk;
      var o = {
        cid: req.cid, oid: null, v: req.v, tk: req.tk, key: key, side: req.side, px: req.px, qty: req.qty, filled: 0,
        left: req.qty, avg: null, tif: req.tif, exp_ms: req.tif === 'POST' && req.ttl_s ? now + req.ttl_s * 1000 : (req.tif === 'POST' && req.exp_ms ? req.exp_ms : null),
        st: 'PEND', why: '', own: 'MAN', lane: null, piq: null, fee: 0, touch: req.touch === undefined ? null : req.touch, edge: null,
        t: { click: req.t_click_ms || null, recv: null, sent: null, ack: null, done: null },
        terminal: false, doneAt: null,
        local: { pend: true, cxlPend: false, amendPend: null, tClick: req.tClick || null, at: now, acked: false }
      };
      st.orders.set(o.cid, o);
      mark('ord'); mark('ord:' + key);
      return o;
    },
    // the HTTP answer to POST /api/order (or amend); status 0 = no answer at all
    reply: function (cid, status, json) {
      var o = st.orders.get(cid);
      if (json && json.ord) H.ord(json.ord, null, false);
      if (status === 0) {
        st.localAlert('CRIT', 'GW', 'no answer for ' + cid + ': order state unknown until the stream reports it');
        return;
      }
      if (!(json && json.ok) && o && !(json && json.ord)) {
        o.st = 'REJ'; o.why = (json && json.err && Desk.contract.enums.WHY.indexOf(json.err.code) >= 0) ? json.err.code : 'INTERNAL';
        o.left = 0; o.terminal = true; o.doneAt = nowMs(); o.local.pend = false;
        mark('ord'); mark('ord:' + o.key);
      }
      if (!(json && json.ok)) {
        var e = (json && json.err) || { code: 'INTERNAL', msg: 'HTTP ' + status };
        st.localAlert('WARN', 'GW', 'REJ ' + e.code + ' ' + (o ? o.side + ' ' + Desk.fmt.size(o.qty) + ' ' + o.tk + ' @ ' + Desk.fmt.px(o.px) : cid) + ': ' + e.msg);
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
        if (!o.local) o.local = { pend: false, cxlPend: false, amendPend: null, tClick: null, at: nowMs(), acked: true };
        o.local.cxlPend = on !== false;
        mark('ord:' + o.key);
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
    hdr: function (d) {
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
        man_working: d.man_working,
        lat: { kx_lag_ms: d.lat.kx_lag_ms, pm_lag_ms: d.lat.pm_lag_ms, gw_ack_ms: d.lat.gw_ack_ms, clk_off_ms: d.lat.clk_off_ms }
      };
      mark('hdr');
    },
    mkt: function (d) {
      var key = d.v + '|' + d.tk, old = st.markets.get(key);
      var m = {
        v: d.v, tk: d.tk, key: key, ev: d.ev, title: d.title, short: d.short, grp: d.grp, kind: d.kind, og: d.og, oc: d.oc,
        open_ms: d.open_ms, close_ms: d.close_ms, status: d.status, result: d.result, grid: d.grid,
        g: old && JSON.stringify(old.grid) === JSON.stringify(d.grid) ? old.g : Desk.grid.make(d.grid),
        fee: { type: d.fee.type, mult: d.fee.mult }, shard: d.shard, lip: d.lip, start_ms: d.start_ms, man: d.man
      };
      st.markets.set(key, m);
      mark('mkt'); mark('mkt:' + key);
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
    ord: function (d, env, quiet, prev) {
      var old = prev || st.orders.get(d.cid);
      var o = {
        cid: d.cid, oid: d.oid, v: d.v, tk: d.tk, key: d.v + '|' + d.tk, side: d.side, px: px(d.px), qty: d.qty,
        filled: d.filled, left: d.left, avg: px(d.avg), tif: d.tif, exp_ms: d.exp_ms, st: d.st, why: d.why, own: d.own,
        lane: d.lane, piq: d.piq, fee: money(d.fee), touch: px(d.touch), edge: money(d.edge),
        t: { click: d.t.click, recv: d.t.recv, sent: d.t.sent, ack: d.t.ack, done: d.t.done },
        terminal: !!TERM[d.st], doneAt: null, local: old && old.local ? old.local : null
      };
      // a terminal order never changes again; a late non-terminal echo cannot resurrect it
      if (old && old.terminal && !o.terminal) return;
      if (o.terminal) o.doneAt = old && old.terminal ? old.doneAt : nowMs();
      if (o.local) {
        o.local.pend = false;
        if (o.terminal) o.local.cxlPend = false;
        if (o.local.amendPend !== null && o.local.amendPend !== undefined && (o.px === o.local.amendPend || o.terminal)) o.local.amendPend = null;
      }
      st.orders.set(o.cid, o);
      if (o.st !== 'PEND' && !quiet) st.acked(o.cid);
      mark('ord'); mark('ord:' + o.key);
    },
    fill: function (d) {
      if (st.fillIdx.has(d.fid)) return;
      var f = {
        fid: d.fid, cid: d.cid, oid: d.oid, v: d.v, tk: d.tk, key: d.v + '|' + d.tk, side: d.side, px: px(d.px), qty: d.qty,
        liq: d.liq, fee: money(d.fee), ts_ms: d.ts_ms, own: d.own, lane: d.lane, mid: px(d.mid), fair: px(d.fair),
        real: money(d.real), mo: {}
      };
      var pend = st.moPending.get(d.fid);
      if (pend) { pend.forEach(function (m) { f.mo[m.h] = m.mo; }); st.moPending.delete(d.fid); }
      st.fills.unshift(f); st.fillIdx.set(f.fid, f);
      if (st.fills.length > 5000) { var gone = st.fills.splice(4000); gone.forEach(function (x) { st.fillIdx.delete(x.fid); }); }
      mark('fill');
    },
    pos: function (d) {
      var key = d.v + '|' + d.tk;
      st.pos.set(key, {
        v: d.v, tk: d.tk, key: key, pos: d.pos, avg: px(d.avg), own: d.own, mark: px(d.mark), unrl: money(d.unrl),
        real: money(d.real), fees: money(d.fees), maxl: money(d.maxl), if_yes: money(d.if_yes), if_no: money(d.if_no)
      });
      mark('pos');
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
      st.lastHb = nowMs();
      st.srvOffset = d.srv_ms - nowMs();
      st.feeds = d.feeds;
      st.recomputeStale(nowMs());
      mark('feeds');
    }
  };
  st._handlers = H;
})(typeof window !== 'undefined' ? window : globalThis);
