/* ui/desk/js/transport.js
 *
 * Desk.transport: one interface, two implementations.
 *   live   EventSource on GET /api/stream?sid= plus fetch() to the contract endpoints on the
 *          same origin (browsers keep the HTTP/1.1 connection alive between calls by default,
 *          so the order POST rides a warm connection; one stream leaves 5 of the 6 per-host
 *          connections free).
 *   demo   the simulated venues (sim.js) in this page. No network at all.
 * Mode, never switched once chosen: a page the backend served (a non-empty desk-token meta) or
 * ?live=1 is live; with no token, file:// or ?demo=1 is demo; anything else is live. A live
 * page with no backend shows SRV DOWN and keeps trying; it never falls back to the simulation.
 * Live keeps its stream alive: no message for 5 s closes and reopens the EventSource (as does a
 * page restored from Safari's back-forward cache), and while the stream is down it probes
 * GET /api/health every 5 s and reopens the stream as soon as the backend answers.
 *
 *   Desk.transport.choose() -> Promise<'demo'|'live'>
 *   Desk.transport.connect(onEnv(env), onState(state, isReconnect))   ('open' on every open)
 *   Desk.transport.snapshot() -> Promise<snapshot>
 *   Desk.transport.post(path, body) -> Promise<{status, json}>   (status 0 = no answer)
 *   Desk.transport.get(path) -> Promise<{status, json}>          (GET /api/universe)
 *
 * Desk.token: the per-start X-Desk token the backend writes into the page
 * (<meta name="desk-token">). Every POST carries it. Live mode re-reads it from the page after
 * a backend restart (hb.epoch changes) and retries a POST refused FORBIDDEN once after that;
 * demo mode takes the simulated backend's token.
 *   Desk.token.value, .read(), .refresh() -> Promise<token>
 *
 * Desk.act: every operator action. Each paints its optimistic state into the store first
 * (synchronously, so the next frame shows it), then sends; nothing ever awaits the network
 * before painting.
 *   Desk.act.order({v, tk, side, px (u), qty, tif, ttl_s, exp_ms, reduce_only, override, touch (u),
 *                   onReply(r, cid)}, ev) -> cid | null
 *   Desk.act.cancel(cids), Desk.act.cancelWhere({v, tk, own, side, px}), Desk.act.cancelAll(own, v)
 *   Desk.act.lane(id, on), Desk.act.start(), .stop(), .kill(), .rearm(), .release(v, tk), .watch(v, tk, on),
 *   .transfer(from, to, amt (u)), .resync()
 *   Desk.act.killing          null, or {at, tries, failed} from the KILL click until the backend
 *                             confirms it. KILL retries by itself (1, 2 ... 5 s apart) until it
 *                             lands; new orders stay refused meanwhile (store.killPend).
 *
 * Desk.subs.sync(): subscribe the stream to the ladders on screen plus every market's top;
 * Desk.subs.reset() sends it again (every stream open); Desk.subs.check(snap.subs) re-sends it
 * when a snapshot shows the backend holding something else. A refused subscribe retries in 2 s.
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
  // the X-Desk token
  // ---------------------------------------------------------------------------------------------
  var META = (Desk.contract && Desk.contract.TOKEN_META) || 'desk-token';
  var RE_META = new RegExp('<meta\\s+name=["\']' + META + '["\']\\s+content=["\']([^"\']*)["\']', 'i');
  var tokenRefresh = null;
  var token = Desk.token = {
    value: '',
    read: function () {
      try {
        var el = root.document && root.document.querySelector ? root.document.querySelector('meta[name="' + META + '"]') : null;
        token.value = el ? (el.getAttribute('content') || '') : '';
      } catch (e) { token.value = ''; }
      return token.value;
    },
    // fetch this page again (never cached) and take the token the backend wrote into it
    refresh: function () {
      if (tokenRefresh) return tokenRefresh;
      var path = (root.location && root.location.pathname) || '/';
      tokenRefresh = fetch(path, { method: 'GET', credentials: 'same-origin', cache: 'no-store' }).then(function (r) { return r.text(); })
        .then(function (t) { var m = RE_META.exec(t); if (m) token.value = m[1]; tokenRefresh = null; return token.value; },
              function () { tokenRefresh = null; return token.value; });
      return tokenRefresh;
    }
  };

  // ---------------------------------------------------------------------------------------------
  // live
  // ---------------------------------------------------------------------------------------------
  function fetchJSON(method, path, body, timeoutMs) {
    var ctl = typeof AbortController !== 'undefined' ? new AbortController() : null;
    var timer = ctl ? setTimeout(function () { ctl.abort(); }, timeoutMs || 5000) : null;
    var init = { method: method, credentials: 'same-origin', cache: 'no-store', headers: {} };
    if (method === 'POST') init.headers['X-Desk'] = token.value;
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

  var SILENT_MS = 5000, PROBE_MS = 5000;
  function Live(sid) {
    this.sid = sid; this.es = null; this.opened = false; this.retry = null; this.epoch = null;
    this.lastMsg = 0; this.wd = null; this.probing = false; this.lastProbe = 0; this.reopens = 0;
    token.read();
  }
  Live.prototype.connect = function (onEnv, onState) {
    var self = this;
    var inner = onEnv;
    // a new hb.epoch is a backend restart: its token is new too
    onEnv = function (env) {
      if (env && env.type === 'hb' && env.data && env.data.epoch) {
        if (self.epoch !== null && env.data.epoch !== self.epoch) token.refresh();
        self.epoch = env.data.epoch;
      }
      inner(env);
    };
    var open = function () {
      if (self.es) { try { self.es.close(); } catch (e) { /* closed */ } }
      self.lastMsg = Date.now();                      // the new socket gets SILENT_MS to say something
      var es = self.es = new EventSource('/api/stream?sid=' + encodeURIComponent(self.sid));
      es.onopen = function () { if (es !== self.es) return; self.lastMsg = Date.now(); onState('open', self.opened); self.opened = true; };
      es.onerror = function () {
        if (es !== self.es) return;
        onState(es.readyState === 2 ? 'closed' : 'error', false);
        if (es.readyState === 2 && !self.retry) self.retry = setTimeout(function () { self.retry = null; open(); }, 2000);
      };
      EVENT_TYPES.forEach(function (t) {
        es.addEventListener(t, function (e) {
          if (es !== self.es) return;
          self.lastMsg = Date.now();
          var env; try { env = JSON.parse(e.data); } catch (x) { return; }
          onEnv(env);
        });
      });
    };
    // close and open again: a stream that stopped talking without an error event (a wedged
    // backend stream, sleep/wake, a page back from the back-forward cache)
    self.reopen = function () {
      self.reopens++;
      if (self.retry) { clearTimeout(self.retry); self.retry = null; }
      onState('error', false);
      open();
    };
    if (!self.wd) self.wd = setInterval(function () {
      var now = Date.now(), es = self.es, up = !!es && es.readyState === 1;
      if (up && now - self.lastMsg > SILENT_MS) { self.reopen(); return; }
      if (!up && !self.probing && now - self.lastProbe >= PROBE_MS) {
        self.lastProbe = now; self.probing = true;
        fetchJSON('GET', '/api/health', undefined, 2000).then(function (r) {
          self.probing = false;
          if (r.status === 200 && r.json && r.json.ok === true && !(self.es && self.es.readyState === 1)) self.reopen();
        });
      }
    }, 1000);
    if (root.addEventListener) root.addEventListener('pageshow', function (e) { if (e && e.persisted) self.reopen(); });
    open();
  };
  Live.prototype.snapshot = function () {
    return fetchJSON('GET', '/api/snapshot?sid=' + encodeURIComponent(this.sid), undefined, 8000).then(function (r) {
      if (r.status !== 200) throw new Error('snapshot HTTP ' + r.status);
      return r.json;
    });
  };
  Live.prototype.get = function (path) { return fetchJSON('GET', path, undefined, 5000); };
  Live.prototype.post = function (path, body) {
    return fetchJSON('POST', path, body, 5000).then(function (r) {
      if (r.status !== 403 || !(r.json && r.json.err && r.json.err.code === 'FORBIDDEN')) return r;
      return token.refresh().then(function () { return fetchJSON('POST', path, body, 5000); });
    });
  };
  Live.prototype.close = function () { if (this.wd) clearInterval(this.wd); this.wd = null; if (this.es) this.es.close(); };

  // ---------------------------------------------------------------------------------------------
  // demo
  // ---------------------------------------------------------------------------------------------
  function Demo(sid) { this.sid = sid; this.sim = null; }
  Demo.prototype.connect = function (onEnv, onState) {
    var q = Desk.qs || {};
    this.sim = Desk.sim.create({
      sid: this.sid, mode: 'DEMO', emit: onEnv,
      seed: q.seed ? (parseInt(q.seed, 10) >>> 0) : ((Date.now() / 1000) >>> 0),
      pmRO: q.pmro === '1', chaos: q.chaos === '1', ext: q.ext !== '0'
    });
    token.value = this.sim.token;           // what the backend would have written into the page
    this.sim.start();
    onState('open', false);
  };
  // The simulated backend runs in this page: its work is deferred until the frame the click caused
  // has been painted (Desk.later), as a real request would leave the page at once and do its work
  // elsewhere. A handler never runs the venue's matching itself.
  Demo.prototype.request = function (method, path, body) {
    var sim = this.sim;
    var hdrs = method === 'POST' ? { 'Content-Type': 'application/json', 'X-Desk': token.value } : {};
    return new Promise(function (resolve) {
      var go = function () { sim.request(method, path, body, function (status, json) { resolve({ status: status, json: json }); }, hdrs); };
      if (Desk.later) Desk.later(go); else go();
    });
  };
  Demo.prototype.snapshot = function () {
    return this.request('GET', '/api/snapshot?sid=' + this.sid).then(function (r) { return r.json; });
  };
  Demo.prototype.post = function (path, body) { return this.request('POST', path, body); };
  Demo.prototype.get = function (path) { return this.request('GET', path); };
  Demo.prototype.close = function () { if (this.sim) this.sim.stop(); };

  // ---------------------------------------------------------------------------------------------
  // the facade
  // ---------------------------------------------------------------------------------------------
  var T = Desk.transport = {
    mode: null,
    sid: randomId(8),
    impl: null,
    // the simulation runs only where no backend can be: a page the backend served is always live
    choose: function () {
      var q = Desk.qs || {};
      var proto = (root.location && root.location.protocol) || 'file:';
      var pick = function (m) { T.mode = m; T.impl = m === 'live' ? new Live(T.sid) : new Demo(T.sid); return m; };
      if (q.live === '1' || token.read()) return Promise.resolve(pick('live'));
      if (q.demo === '1' || proto === 'file:') return Promise.resolve(pick('demo'));
      return Promise.resolve(pick('live'));
    },
    connect: function (onEnv, onState) { T.impl.connect(onEnv, onState); },
    snapshot: function () { return T.impl.snapshot(); },
    post: function (path, body) { return T.impl.post(path, body); },
    get: function (path) { return T.impl ? T.impl.get(path) : Promise.resolve({ status: 0, json: null }); },
    sim: function () { return T.impl && T.impl.sim ? T.impl.sim : null; }
  };

  // ---------------------------------------------------------------------------------------------
  // subscriptions: the ladders on screen at FULL depth, every market's top
  // ---------------------------------------------------------------------------------------------
  var subTimer = null, subRetry = null, lastSub = '';
  function wantSubs() {
    var seen = {}, ladders = [];
    Desk.layout.ladders().forEach(function (slot) {
      var k = Desk.focus.ladders[slot];
      if (k && !seen[k]) { seen[k] = 1; var s = Desk.store.split(k); ladders.push({ v: s.v, tk: s.tk }); }
    });
    return { sid: T.sid, ladders: ladders, tops: 'ALL' };
  }
  function subKeys(a) { return (a || []).map(function (x) { return x.v + '|' + x.tk; }).sort().join(','); }
  function sendSubs() {
    if (!T.impl) return;
    var body = wantSubs(), sig = JSON.stringify(body);
    if (sig === lastSub) return;
    lastSub = sig;
    T.post('/api/subscribe', body).then(function (r) {
      if (r.status === 200 && r.json && r.json.ok) return;
      if (lastSub === sig) lastSub = '';
      if (!subRetry) subRetry = setTimeout(function () { subRetry = null; Desk.subs.sync(); }, 2000);
    });
  }
  Desk.subs = {
    sync: function () {
      if (subTimer) return;
      subTimer = setTimeout(function () { subTimer = null; sendSubs(); }, 0);
    },
    reset: function () { lastSub = ''; Desk.subs.sync(); },
    // a snapshot's subs: what the backend holds for this sid; re-send when it is not what we show
    check: function (subs) {
      if (!T.impl || !subs) return false;
      var w = wantSubs();
      var same = subKeys(subs.ladders) === subKeys(w.ladders) && (subs.tops === 'ALL') === (w.tops === 'ALL');
      if (!same) Desk.subs.reset();
      return !same;
    }
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

  function killSend() {
    var k = Desk.act.killing;
    if (!k) return Promise.resolve(null);
    if (k.timer) { clearTimeout(k.timer); k.timer = null; }
    k.tries++;
    return T.post('/api/kill', {}).then(function (r) {
      if (Desk.act.killing !== k) return r;
      if (r.json && r.json.ok) {
        runReply('kill')(r);
        Desk.act.killing = null; st().killPend = false;
        Desk.render.mark('hdr'); Desk.render.mark('stale');
        return r;
      }
      var e = (r.json && r.json.err) || { code: 'INTERNAL', msg: 'HTTP ' + r.status };
      st().localAlert('CRIT', 'KILL', 'KILL not confirmed (' + (r.status === 0 ? 'no answer' : e.code) + ': ' + e.msg + '), retrying');
      if (!k.failed) { k.failed = true; st().cxlPend(k.cids, false); }
      Desk.render.mark('hdr');
      k.timer = setTimeout(killSend, Math.min(5000, 1000 * k.tries));
      return r;
    });
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
      var expMs = o.tif === 'POST' && o.exp_ms ? o.exp_ms : null;
      var ttl = o.tif === 'POST' && !expMs ? (o.ttl_s || 15) : null;
      var tClickMs = Desk.lat.epoch(ev);
      Desk.lat.click(cid, ev);
      S.pend({ cid: cid, v: o.v, tk: o.tk, side: o.side, px: o.px, qty: o.qty, tif: o.tif, ttl_s: ttl, exp_ms: expMs,
               touch: o.touch === undefined ? null : o.touch, t_click_ms: tClickMs });
      var body = {
        cid: cid, v: o.v, tk: o.tk, side: o.side, px: N.pxStr(o.px), qty: o.qty, tif: o.tif,
        ttl_s: ttl, override: !!o.override,
        touch: o.touch === null || o.touch === undefined ? null : N.pxStr(o.touch), t_click_ms: tClickMs
      };
      if (expMs) body.exp_ms = expMs;
      if (o.reduce_only && o.tif === 'IOC') body.reduce_only = true;
      T.post('/api/order', body).then(function (r) {
        S.reply(cid, r.status, r.json);
        if (o.onReply) { try { o.onReply(r, cid); } catch (e) { if (root.console) console.error('[desk order reply]', e); } }
      });
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
    // KILL paints at once (KILL ... on the button, every working order struck through, new orders
    // refused) and shows KILLED only when the backend confirms it. A failure says KILL FAIL,
    // un-strikes the orders it did not cancel, and KILL retries by itself until it lands (it is
    // idempotent); a click on KILL FAIL retries at once.
    killing: null,
    kill: function () {
      var S = st(), k = Desk.act.killing;
      if (k) { if (k.failed) return killSend(); return Promise.resolve(null); }
      var cids = S.working().map(function (o) { return o.cid; });
      S.cxlPend(cids, true);
      Desk.act.killing = { at: Date.now(), tries: 0, failed: false, cids: cids, timer: null };
      S.killPend = true;
      Desk.render.mark('hdr'); Desk.render.mark('stale');
      return killSend();
    },
    rearm: function () { return T.post('/api/rearm', {}).then(runReply('rearm')); },
    // the watchlist star: shown at once (store.pinPend), the backend's mkt echo confirms it; a
    // refusal puts it back
    watch: function (v, tk, on) {
      var S = st(), key = v + '|' + tk;
      S.pinPend.set(key, { on: !!on, at: Date.now() });
      Desk.render.mark('mkt'); Desk.render.mark('uni');
      return T.post('/api/watch', { v: v, tk: tk, on: !!on }).then(function (r) {
        if (!(r.json && r.json.ok)) { S.pinPend.delete(key); Desk.render.mark('mkt'); Desk.render.mark('uni'); failAlert('watch ' + tk, r); }
        else if (S.markets.has(key) && !!S.markets.get(key).pin === !!on) { S.pinPend.delete(key); }
        return r;
      });
    },
    release: function (v, tk) { return T.post('/api/release', { v: v, tk: tk }).then(function (r) { if (!(r.json && r.json.ok)) failAlert('release', r); return r; }); },
    // move free cash between Kalshi shards; amt in u
    transfer: function (from, to, amt) {
      return T.post('/api/transfer', { from: from, to: to, amt: N.moneyStr(amt) }).then(function (r) { if (!(r.json && r.json.ok)) failAlert('move', r); return r; });
    },
    resync: function () { st().resync('USER'); }
  };
})(typeof window !== 'undefined' ? window : globalThis);
