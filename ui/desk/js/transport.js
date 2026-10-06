/* ui/desk/js/transport.js
 *
 * Desk.transport: one interface, two implementations.
 *   live   EventSource on GET /api/stream?sid= plus fetch() to the contract endpoints on the
 *          same origin (browsers keep the HTTP/1.1 connection alive between calls by default,
 *          so the order POST rides a warm connection; one stream leaves 5 of the 6 per-host
 *          connections free).
 *   demo   the simulated venues (sim.js) in this page. No network at all.
 * Mode: file:// or ?demo=1 -> demo; ?live=1 -> live (never falls back); otherwise a 1.5 s
 * GET /api/health probe decides, and no answer means demo (the header says DEMO in amber).
 *
 *   Desk.transport.choose() -> Promise<'demo'|'live'>
 *   Desk.transport.connect(onEnv(env), onState(state, isReconnect))
 *   Desk.transport.snapshot() -> Promise<snapshot>
 *   Desk.transport.post(path, body) -> Promise<{status, json}>   (status 0 = no answer)
 *
 * Desk.act: every operator action. Each paints its optimistic state into the store first
 * (synchronously, so the next frame shows it), then sends; nothing ever awaits the network
 * before painting.
 *   Desk.act.order({v, tk, side, px (u), qty, tif, ttl_s, override, touch (u)}, ev) -> cid | null
 *   Desk.act.cancel(cids), Desk.act.cancelWhere({v, tk, own, side, px}), Desk.act.cancelAll(own, v)
 *   Desk.act.amend(cid, {px (u), qty, ttl_s}), Desk.act.lane(id, on), Desk.act.start(), .stop(),
 *   .kill(), .rearm(), .release(v, tk), .resync()
 *
 * Desk.subs.sync(): subscribe the stream to the ladders on screen plus every market's top.
 */
(function (root) {
  'use strict';
  var Desk = root.Desk = root.Desk || {};
  var N = Desk.num;

  function randomId(n) {
    var a = 'abcdefghijkmnpqrstuvwxyz23456789', s = '';
    var buf = null;
    try { if (root.crypto && root.crypto.getRandomValues) { buf = new Uint8Array(n); root.crypto.getRandomValues(buf); } } catch (e) { buf = null; }
    for (var i = 0; i < n; i++) s += a.charAt((buf ? buf[i] : Math.floor(Math.random() * 256)) % a.length);
    return s;
  }
  var EVENT_TYPES = Desk.contract ? Object.keys(Desk.contract.events) : ['hdr', 'mkt', 'book', 'ord', 'fill', 'pos', 'lane', 'risk', 'mo', 'alert', 'hb'];

  // ---------------------------------------------------------------------------------------------
  // live
  // ---------------------------------------------------------------------------------------------
  function fetchJSON(method, path, body, timeoutMs) {
    var ctl = typeof AbortController !== 'undefined' ? new AbortController() : null;
    var timer = ctl ? setTimeout(function () { ctl.abort(); }, timeoutMs || 5000) : null;
    var init = { method: method, credentials: 'same-origin', cache: 'no-store', headers: { 'X-Desk': '1' } };
    if (body !== undefined) { init.headers['Content-Type'] = 'application/json'; init.body = JSON.stringify(body); }
    if (ctl) init.signal = ctl.signal;
    return fetch(path, init).then(function (r) {
      return r.text().then(function (t) {
        if (timer) clearTimeout(timer);
        var j = null; try { j = t ? JSON.parse(t) : null; } catch (e) { j = null; }
        if (j === null) j = { ok: false, err: { code: 'INTERNAL', msg: 'HTTP ' + r.status + ' with no JSON body' } };
        return { status: r.status, json: j };
      });
    }, function (e) {
      if (timer) clearTimeout(timer);
      return { status: 0, json: { ok: false, err: { code: 'EXCHANGE', msg: (e && e.name === 'AbortError') ? 'no answer from the desk in ' + Math.round((timeoutMs || 5000) / 1000) + ' s' : 'desk unreachable' } } };
    });
  }

  function Live(sid) { this.sid = sid; this.es = null; this.opened = false; this.retry = null; }
  Live.prototype.connect = function (onEnv, onState) {
    var self = this;
    var open = function () {
      if (self.es) { try { self.es.close(); } catch (e) { /* closed */ } }
      var es = self.es = new EventSource('/api/stream?sid=' + encodeURIComponent(self.sid));
      es.onopen = function () { onState('open', self.opened); self.opened = true; };
      es.onerror = function () {
        onState(es.readyState === 2 ? 'closed' : 'error', false);
        if (es.readyState === 2 && !self.retry) self.retry = setTimeout(function () { self.retry = null; open(); }, 2000);
      };
      EVENT_TYPES.forEach(function (t) {
        es.addEventListener(t, function (e) {
          var env; try { env = JSON.parse(e.data); } catch (x) { return; }
          onEnv(env);
        });
      });
    };
    open();
  };
  Live.prototype.snapshot = function () {
    return fetchJSON('GET', '/api/snapshot?sid=' + encodeURIComponent(this.sid), undefined, 8000).then(function (r) {
      if (r.status !== 200) throw new Error('snapshot HTTP ' + r.status);
      return r.json;
    });
  };
  Live.prototype.post = function (path, body) { return fetchJSON('POST', path, body, 5000); };
  Live.prototype.close = function () { if (this.es) this.es.close(); };

  // ---------------------------------------------------------------------------------------------
  // demo
  // ---------------------------------------------------------------------------------------------
  function Demo(sid) { this.sid = sid; this.sim = null; }
  Demo.prototype.connect = function (onEnv, onState) {
    var q = Desk.qs || {};
    this.sim = Desk.sim.create({
      sid: this.sid, mode: 'DEMO', emit: onEnv,
      seed: q.seed ? (parseInt(q.seed, 10) >>> 0) : ((Date.now() / 1000) >>> 0),
      pmRO: q.pmro === '1', chaos: q.chaos === '1'
    });
    this.sim.start();
    onState('open', false);
  };
  Demo.prototype.request = function (method, path, body) {
    var sim = this.sim;
    return new Promise(function (resolve) { sim.request(method, path, body, function (status, json) { resolve({ status: status, json: json }); }); });
  };
  Demo.prototype.snapshot = function () {
    return this.request('GET', '/api/snapshot?sid=' + this.sid).then(function (r) { return r.json; });
  };
  Demo.prototype.post = function (path, body) { return this.request('POST', path, body); };
  Demo.prototype.close = function () { if (this.sim) this.sim.stop(); };

  // ---------------------------------------------------------------------------------------------
  // the facade
  // ---------------------------------------------------------------------------------------------
  var T = Desk.transport = {
    mode: null,
    sid: randomId(8),
    impl: null,
    choose: function () {
      var q = Desk.qs || {};
      var proto = (root.location && root.location.protocol) || 'file:';
      var pick = function (m) { T.mode = m; T.impl = m === 'live' ? new Live(T.sid) : new Demo(T.sid); return m; };
      if (q.live === '1') return Promise.resolve(pick('live'));
      if (q.demo === '1' || proto === 'file:') return Promise.resolve(pick('demo'));
      return fetchJSON('GET', '/api/health', undefined, 1500).then(function (r) {
        return pick(r.status === 200 && r.json && r.json.ok === true && r.json.epoch ? 'live' : 'demo');
      });
    },
    connect: function (onEnv, onState) { T.impl.connect(onEnv, onState); },
    snapshot: function () { return T.impl.snapshot(); },
    post: function (path, body) { return T.impl.post(path, body); },
    sim: function () { return T.impl && T.impl.sim ? T.impl.sim : null; }
  };

  // ---------------------------------------------------------------------------------------------
  // subscriptions: the ladders on screen at FULL depth, every market's top
  // ---------------------------------------------------------------------------------------------
  var subTimer = null, lastSub = '';
  Desk.subs = {
    sync: function () {
      if (subTimer) return;
      subTimer = setTimeout(function () {
        subTimer = null;
        if (!T.impl) return;
        var seen = {}, ladders = [];
        Desk.layout.ladders().forEach(function (slot) {
          var k = Desk.focus.ladders[slot];
          if (k && !seen[k]) { seen[k] = 1; var s = Desk.store.split(k); ladders.push({ v: s.v, tk: s.tk }); }
        });
        var body = { sid: T.sid, ladders: ladders, tops: 'ALL' };
        var sig = JSON.stringify(body);
        if (sig === lastSub) return;
        lastSub = sig;
        T.post('/api/subscribe', body).then(function (r) { if (r.status !== 200) lastSub = ''; });
      }, 0);
    },
    reset: function () { lastSub = ''; Desk.subs.sync(); }
  };

  // ---------------------------------------------------------------------------------------------
  // actions
  // ---------------------------------------------------------------------------------------------
  var cidN = 0;
  function newCid() { return 'M' + T.sid + '-' + (++cidN).toString(36); }
  function st() { return Desk.store; }
  function failAlert(what, r) {
    var e = (r.json && r.json.err) || { code: 'INTERNAL', msg: 'HTTP ' + r.status };
    st().localAlert(r.status === 0 ? 'CRIT' : 'WARN', 'GW', what + ' ' + e.code + ': ' + e.msg);
  }
  function runReply(what) {
    return function (r) {
      if (r.json && r.json.ok && r.json.run && st().hdr) { st().hdr.run = r.json.run; Desk.render.mark('hdr'); }
      if (!(r.json && r.json.ok)) failAlert(what, r);
      return r;
    };
  }

  Desk.act = {
    // place an order. o.px and o.touch are in u. Returns the cid, or null if it was not sent.
    order: function (o, ev) {
      var S = st(), key = o.v + '|' + o.tk;
      var why = S.canTrade(o.v);
      if (why) { S.localAlert('WARN', 'GW', 'not sent: ' + why); return null; }
      var m = S.market(key);
      if (m && !Desk.grid.onGrid(m.g, o.px)) { S.localAlert('WARN', 'GW', 'not sent: ' + Desk.fmt.px(o.px) + ' is off the grid'); return null; }
      if (!(o.qty > 0)) { S.localAlert('WARN', 'GW', 'not sent: qty ' + o.qty); return null; }
      var cid = newCid();
      var ttl = o.tif === 'POST' ? (o.ttl_s || 15) : null;
      var tClickMs = Desk.lat.epoch(ev);
      Desk.lat.click(cid, ev, o.v);
      S.pend({ cid: cid, v: o.v, tk: o.tk, side: o.side, px: o.px, qty: o.qty, tif: o.tif, ttl_s: ttl,
               touch: o.touch === undefined ? null : o.touch, t_click_ms: tClickMs });
      var body = {
        cid: cid, v: o.v, tk: o.tk, side: o.side, px: N.pxStr(o.px), qty: o.qty, tif: o.tif,
        ttl_s: ttl, override: !!o.override,
        touch: o.touch === null || o.touch === undefined ? null : N.pxStr(o.touch), t_click_ms: tClickMs
      };
      T.post('/api/order', body).then(function (r) { S.reply(cid, r.status, r.json); });
      return cid;
    },
    cancel: function (cids) {
      cids = (cids || []).filter(Boolean);
      if (!cids.length) return;
      st().cxlPend(cids, true);
      T.post('/api/cancel', { cids: cids }).then(function (r) { if (!(r.json && r.json.ok)) { st().cxlPend(cids, false); failAlert('cancel', r); } });
    },
    // {v, tk, own: 'MAN'|'SYS'|'ANY', side: 'BUY'|'SELL'|null, px: u|null}
    cancelWhere: function (f) {
      var S = st(), key = f.v + '|' + f.tk, own = f.own || 'MAN';
      var cids = S.working(function (o) {
        return o.key === key && (own === 'ANY' || o.own === own) && (!f.side || o.side === f.side) && (f.px === null || f.px === undefined || o.px === f.px);
      }).map(function (o) { return o.cid; });
      S.cxlPend(cids, true);
      T.post('/api/cancel', { v: f.v, tk: f.tk, own: own, side: f.side || null, px: f.px === null || f.px === undefined ? null : N.pxStr(f.px) })
        .then(function (r) { if (!(r.json && r.json.ok)) { S.cxlPend(cids, false); failAlert('cancel', r); } });
      return cids.length;
    },
    cancelAll: function (own, v) {
      var S = st();
      var cids = S.working(function (o) { return (own === 'ANY' || o.own === own) && (!v || o.v === v); }).map(function (o) { return o.cid; });
      S.cxlPend(cids, true);
      T.post('/api/cancel_all', { own: own, v: v || null }).then(function (r) { if (!(r.json && r.json.ok)) { S.cxlPend(cids, false); failAlert('cancel all', r); } });
      return cids.length;
    },
    amend: function (cid, a) {
      var S = st(), o = S.orders.get(cid);
      if (!o || o.terminal) return false;
      if (!o.local) o.local = { pend: false, cxlPend: false, amendPend: null, tClick: null, at: Date.now(), acked: true };
      if (a.px !== null && a.px !== undefined) { o.local.amendPend = a.px; Desk.render.mark('ord'); Desk.render.mark('ord:' + o.key); }
      T.post('/api/amend', { cid: cid, px: a.px === null || a.px === undefined ? null : N.pxStr(a.px), qty: a.qty === undefined ? null : a.qty, ttl_s: a.ttl_s === undefined ? null : a.ttl_s })
        .then(function (r) {
          if (r.json && r.json.ord) S._handlers.ord(r.json.ord, null, false);
          if (!(r.json && r.json.ok)) { if (o.local) o.local.amendPend = null; Desk.render.mark('ord'); failAlert('amend', r); }
        });
      return true;
    },
    lane: function (id, on) {
      var S = st(), l = S.lanes.get(id);
      if (l) { l.pendOn = !!on; l.pendAt = Date.now(); Desk.render.mark('lane'); }
      T.post('/api/lane/' + encodeURIComponent(id), { on: !!on }).then(function (r) {
        if (r.json && r.json.ok && r.json.lane) S._handlers.lane(r.json.lane);
        else { if (l) l.pendOn = null; Desk.render.mark('lane'); failAlert('lane ' + id, r); }
      });
    },
    start: function () { return T.post('/api/start', {}).then(runReply('start')); },
    stop: function () { return T.post('/api/stop', {}).then(runReply('stop')); },
    kill: function () {
      var S = st();
      S.cxlPend(S.working().map(function (o) { return o.cid; }), true);
      if (S.hdr) { S.hdr.run = { state: 'KILLED', since_ms: Date.now(), rearm_ms: Date.now() + 60000, why: 'operator' }; Desk.render.mark('hdr'); }
      return T.post('/api/kill', {}).then(runReply('kill'));
    },
    rearm: function () { return T.post('/api/rearm', {}).then(runReply('rearm')); },
    release: function (v, tk) { return T.post('/api/release', { v: v, tk: tk }).then(function (r) { if (!(r.json && r.json.ok)) failAlert('release', r); return r; }); },
    resync: function () { st().resync('USER'); }
  };
})(typeof window !== 'undefined' ? window : globalThis);
