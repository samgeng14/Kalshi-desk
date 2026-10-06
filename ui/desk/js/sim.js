/* ui/desk/js/sim.js
 *
 * THE DEMO VENUES. A simulated Kalshi + Polymarket US and a simulated desk backend that emit
 * exactly the events of Desk.contract and answer exactly its endpoints. Nothing here talks to a
 * network. The demo transport (transport.js) wires it to the store; the node test drives it with
 * a fake clock and validates every event it emits.
 *
 *   var sim = Desk.sim.create({now, seed, emit, sid, mode, pmRO, chaos, ext, port})
 *   sim.request(method, path, body, reply(status, json), headers)
 *                                                           the HTTP endpoints, with latency and
 *                                                           the web worker's guards: a POST needs
 *                                                           headers['X-Desk'] === sim.token, and a
 *                                                           Host header, when given, must pass
 *                                                           contract.hostOk(host, port)
 *   sim.token                                               the per-start token ("in the page")
 *   sim.snapshot()                                          GET /api/snapshot body
 *   sim.pump()                                              run timers and world ticks due now
 *   sim.nextDue()                                           when pump next has work (ms)
 *   sim.start() / sim.stop()                                self-driving loop (browser)
 *   test hooks: dropNext(), setFeed(name, up), freeze(key, {bids, asks}), trade(key, side, qty),
 *               external(key, side, pxU, qty) (an EXT order placed outside the desk),
 *               market(key), order(cid), position(key), cash(), state
 *
 * What is simulated:
 *  - Kalshi 15-minute LIP windows (copper, platinum, palladium, natgas, EURUSD, GBPUSD, USDJPY):
 *    fee type QUAD (takers pay, makers free), one market per series, rolled every 15 minutes,
 *    fair = Phi(ln(S/S0)/(sigma sqrt tau)).
 *  - Kalshi daily high-temperature brackets (4 cities today, 2 tomorrow), 6 brackets each.
 *  - Thin college football games on Kalshi (one market per team) and Polymarket US (one market per
 *    game), with a game clock, scores, a win-probability model, ESPN ~41 s behind, and a model
 *    fair (the friend-style overlay) with +-sigma. Plus a Polymarket-only pro game tomorrow.
 *  - Random-walk books with depth on each market's grid (1c, tapered 0.1c, Polymarket 0.1c),
 *    anonymous flow that trades against them, stale books after a score.
 *  - A matching engine for our orders: IOC/FOK never rest, POST never crosses (PostOnlyCross
 *    cancel), rests with queue position, fills when prints or the market trade through it,
 *    expires at its deadline; cancel, amend, kill; self-trade prevention.
 *  - Positions netted per market, cash per Kalshi shard (reservations by Desk.risk, the contract's
 *    rule), order revisions (rev), Kalshi and Polymarket fees by formula,
 *    markouts at 1/10/60 s and settlement, the 70% Systematic ceiling, a daily cost budget, the
 *    daily stop, lanes that trade when Start is pressed, LIP reward accrual, ~40-80 ms acks on
 *    Kalshi and ~80-150 ms on Polymarket.
 */
(function (root) {
  'use strict';
  var Desk = root.Desk = root.Desk || {};
  var U = 10000;
  var MONTHS = ['JAN', 'FEB', 'MAR', 'APR', 'MAY', 'JUN', 'JUL', 'AUG', 'SEP', 'OCT', 'NOV', 'DEC'];
  var MON3 = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
  var ET = 4 * 3600000;                 // EDT (the demo assumes UTC-4)
  var Q15 = 15 * 60000;
  var SNAP_FILLS = 5000;                // fills in a snapshot (contract: the newest 5000)

  var GRID_LINEAR = [{ start: '0.0000', end: '1.0000', step: '0.0100' }];
  var GRID_TAPERED = [{ start: '0.0000', end: '0.1000', step: '0.0010' }, { start: '0.1000', end: '0.9000', step: '0.0100' },
                      { start: '0.9000', end: '1.0000', step: '0.0010' }];
  var GRID_DECI = [{ start: '0.0000', end: '1.0000', step: '0.0010' }];

  var LIP = [
    { s: 'KXCOPPER15M', name: 'COPPER', full: 'Copper', S: 4.6125, vol: 0.24, grid: GRID_TAPERED, shard: 'S1' },
    { s: 'KXPLATINUM15M', name: 'PLAT', full: 'Platinum', S: 1012.4, vol: 0.30, grid: GRID_TAPERED, shard: 'S1' },
    { s: 'KXPALLADIUM15M', name: 'PALL', full: 'Palladium', S: 1104.0, vol: 0.34, grid: GRID_TAPERED, shard: 'S1' },
    { s: 'KXNATGAS15M', name: 'NATGAS', full: 'Natural Gas', S: 3.0811, vol: 0.60, grid: GRID_LINEAR, shard: 'S3' },
    { s: 'KXEURUSD15M', name: 'EURUSD', full: 'EUR/USD', S: 1.1042, vol: 0.08, grid: GRID_LINEAR, shard: 'S3' },
    { s: 'KXGBPUSD15M', name: 'GBPUSD', full: 'GBP/USD', S: 1.3121, vol: 0.09, grid: GRID_LINEAR, shard: 'S3' },
    { s: 'KXUSDJPY15M', name: 'USDJPY', full: 'USD/JPY', S: 148.62, vol: 0.10, grid: GRID_LINEAR, shard: 'S3' }
  ];
  var WX = [
    { s: 'KXHIGHNY', city: 'NYC', mu: 66, tomorrow: true },
    { s: 'KXHIGHCHI', city: 'CHI', mu: 61, tomorrow: true },
    { s: 'KXHIGHAUS', city: 'AUS', mu: 88, tomorrow: false },
    { s: 'KXHIGHMIA', city: 'MIA', mu: 86, tomorrow: false }
  ];
  var TEAMS = [
    ['SOUMIS', 'TROY', 'Southern Miss', 'Troy'], ['ILST', 'SIU', 'Illinois St.', 'Southern Illinois'],
    ['OHIO', 'SAC', 'Ohio', 'Sacramento St.'], ['RICE', 'UAB', 'Rice', 'UAB'], ['TULSA', 'MEM', 'Tulsa', 'Memphis'],
    ['APP', 'GASO', 'App State', 'Georgia Southern'], ['NMSU', 'UTEP', 'New Mexico St.', 'UTEP'],
    ['DUKE', 'UNC', 'Duke', 'North Carolina'], ['TXST', 'ULM', 'Texas St.', 'Louisiana-Monroe']
  ];
  var LANES = [
    { id: 'M1', name: 'LIP-15', venue: 'K', style: 'REST', budget: 50, thr: null, on: true },
    { id: 'M2', name: 'BAND 96-99', venue: 'K', style: 'REST', budget: 30, thr: null, on: true },
    { id: 'S1', name: 'ARB', venue: 'KP', style: 'IOC', budget: 25, thr: 0.02, on: true },
    { id: 'C1', name: 'COLLEGE TAKE', venue: 'KP', style: 'IOC', budget: 10, thr: 0.03, on: true },
    { id: 'X1', name: 'ESPN EXP', venue: 'K', style: 'REST', budget: 5, thr: null, on: false }
  ];
  var KIND = {
    LIP15: { spread: 150, depth: 10, sz: 60, rate: 0.25, tau: 1.5, noise: 25, upd: 1.5, holes: 0.1 },
    WEATHER: { spread: 200, depth: 8, sz: 35, rate: 0.03, tau: 6, noise: 12, upd: 0.25, holes: 0.2 },
    GAME_K: { spread: 400, depth: 6, sz: 12, rate: 0.06, tau: 3, noise: 25, upd: 0.5, holes: 0.35 },
    GAME_P: { spread: 250, depth: 8, sz: 25, rate: 0.05, tau: 3, noise: 20, upd: 0.5, holes: 0.25 }
  };

  function mulberry32(a) {
    return function () {
      a |= 0; a = a + 0x6D2B79F5 | 0;
      var t = Math.imul(a ^ a >>> 15, 1 | a);
      t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t;
      return ((t ^ t >>> 14) >>> 0) / 4294967296;
    };
  }
  function ncdf(x) { return Desk.num.ncdf(x); }
  function pad2(n) { return n < 10 ? '0' + n : '' + n; }
  function etD(ms) { return new Date(ms - ET); }
  function evDate(ms) { var d = etD(ms); return String(d.getUTCFullYear()).slice(2) + MONTHS[d.getUTCMonth()] + pad2(d.getUTCDate()); }
  function evTime(ms) { var d = etD(ms); return pad2(d.getUTCHours()) + pad2(d.getUTCMinutes()); }
  function hmET(ms) { var d = etD(ms); return pad2(d.getUTCHours()) + ':' + pad2(d.getUTCMinutes()); }
  function mdET(ms) { var d = etD(ms); return pad2(d.getUTCMonth() + 1) + '/' + pad2(d.getUTCDate()); }
  function isoET(ms) { var d = etD(ms); return d.getUTCFullYear() + '-' + pad2(d.getUTCMonth() + 1) + '-' + pad2(d.getUTCDate()); }
  function longDate(ms) { var d = etD(ms); return MON3[d.getUTCMonth()] + ' ' + d.getUTCDate() + ', ' + d.getUTCFullYear(); }
  function etMidnight(ms) { var d = etD(ms); return Date.UTC(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate()) + ET; }
  function terminal(st) { return st === 'FILL' || st === 'CXL' || st === 'REJ' || st === 'EXP'; }
  function r2(x) { return Math.round(x * 100) / 100; }

  function create(opts) {
    opts = opts || {};
    var N = Desk.num, G = Desk.grid, FEES = Desk.fees, C = Desk.contract, RISK = Desk.risk;
    var clock = opts.now || function () { return Date.now(); };
    var rnd = mulberry32((opts.seed >>> 0) || 20261006);
    var out = opts.emit || function () {};
    function R(a, b) { return a + (b - a) * rnd(); }
    function gauss() { var u = 1 - rnd(), v = rnd(); return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v); }
    function poisson(l) { var n = 0, p = Math.exp(-l), s = p, x = rnd(); while (x > s && n < 20) { n++; p *= l / n; s += p; } return n; }
    function clampPx(u) { return Math.max(0, Math.min(U, Math.round(u))); }
    function pxs(u) { return N.pxStr(clampPx(u)); }
    function ms(u) { return N.moneyStr(u); }

    var T0 = clock();
    var S = {
      epoch: 'sim-' + T0.toString(36),
      sid: opts.sid || 'demo0000',
      mode: opts.mode || 'DEMO',
      seq: 0, emitted: 0, drop: false,
      markets: new Map(), orders: new Map(), live: new Set(),
      fills: [], fillById: new Map(), mos: [], pos: new Map(), alerts: [],
      timers: [], lastTick: T0, nextTick: T0, nextHb: T0, nextLane: T0 + 500, nextMark: T0 + 1000,
      nextRisk: T0, nextDislocate: T0 + R(40000, 90000), nextChaos: T0 + R(120000, 240000),
      cash: { S0: 411000, S1: 368100, S2: 332000, S3: 312000, PM: 987000 },
      run: { state: 'STOP', since_ms: T0, rearm_ms: null, why: '' },
      feeds: { KX: { up: true, gap: false }, KXP: { up: true, gap: false }, PM: { up: true, gap: false } },
      pmRO: !!opts.pmRO, chaos: !!opts.chaos,
      subs: { ladders: [], tops: 'ALL' }, forceBook: new Set(),
      ogOutcomes: {}, games: [], wx: [], lip: {},
      gwAcks: [], lat: { kx: 34, pm: 210, clk: 80 },
      costU: 0, costBudgetU: 48000, stopLimitU: 250000,
      realBy: { MAN: 0, SYS: 0, EXT: 0 }, lipU: 0,
      port: opts.port || 8788, token: opts.token || null,
      groups: { ok: true, why: '' },
      oidN: 0, fidN: 0, alertN: 0, cidN: 0, gameN: 0,
      lanes: [], laneById: {},
      lastHdrJson: '', lastHdrEmit: 0, lastRiskJson: '', teamIdx: 0
    };
    var EPS = S.epoch.slice(-4);
    // the per-start token the backend writes into the page (never from the seeded generator, so
    // a seed replays the same market whatever the token is)
    if (!S.token) {
      var tk = '', abc = 'abcdefghijkmnpqrstuvwxyz23456789', buf = null;
      try { if (root.crypto && root.crypto.getRandomValues) { buf = new Uint8Array(24); root.crypto.getRandomValues(buf); } } catch (e) { buf = null; }
      for (var ti = 0; ti < 24; ti++) tk += abc.charAt((buf ? buf[ti] : Math.floor(Math.random() * 256)) % abc.length);
      S.token = tk;
    }
    var rollRate = 0.45;      // game clock seconds per real second

    // ------------------------------------------------------------------------------------------
    // emission
    // ------------------------------------------------------------------------------------------
    function emit(type, data) {
      S.seq++;
      if (S.drop) { S.drop = false; return; }
      S.emitted++;
      out({ type: type, seq: S.seq, ts_ms: clock(), data: data });
    }
    function alert(lvl, src, msg) {
      var a = { id: 'a-' + (++S.alertN), lvl: lvl, src: src, msg: msg, ts_ms: clock() };
      S.alerts.push(a); if (S.alerts.length > 400) S.alerts.splice(0, S.alerts.length - 200);
      emit('alert', a);
    }

    // ------------------------------------------------------------------------------------------
    // timers
    // ------------------------------------------------------------------------------------------
    // the browser loop (start) sleeps until the next timer or world tick; a timer due sooner than
    // that wakes it, so a reply due in 2 ms is not held to the next 100 ms tick
    var loopTimer = null, running = false, wakeAt = Infinity;
    function at(due, fn) {
      var t = { due: due, fn: fn }, a = S.timers, lo = 0, hi = a.length;
      while (lo < hi) { var mid = (lo + hi) >> 1; if (a[mid].due <= due) lo = mid + 1; else hi = mid; }
      a.splice(lo, 0, t);
      if (running && due < wakeAt) {
        if (loopTimer) clearTimeout(loopTimer);
        wakeAt = due;
        loopTimer = setTimeout(loop, Math.max(0, due - clock()));
      }
    }
    function latency(v) { return Math.round(v === 'P' ? R(80, 150) : R(40, 80)); }

    // ------------------------------------------------------------------------------------------
    // markets
    // ------------------------------------------------------------------------------------------
    function addMarket(spec, quiet) {
      var k = KIND[spec.bk];
      var m = {
        key: spec.v + '|' + spec.tk, v: spec.v, tk: spec.tk, ev: spec.ev, title: spec.title, short: spec.short,
        grp: spec.grp, kind: spec.kind, og: spec.og, oc: spec.oc, open_ms: spec.open_ms, close_ms: spec.close_ms,
        status: 'OPEN', result: null, settledAt: null, grid: spec.grid, g: G.make(spec.grid), fee: spec.fee,
        shard: spec.v === 'K' ? spec.shard : null, lip: !!spec.lip,
        pU: spec.p * U, mid: spec.p * U, k: k, spreadU: k.spread * R(0.8, 1.2),
        bids: new Map(), asks: new Map(), ours: [], prints: [], last: null, vol: Math.round(R(150, 3500)),
        ref: { fair: null, sd: null, espn: null, espn_ts_ms: null, game: null },
        bookDirty: true, lastBookEmit: 0, topSig: '', exMs: clock(), frozen: false, dislocate: null,
        game: spec.game || null, manOwned: false, lastRegenMid: spec.p * U, m1Flat: false, c1Shot: -1, s1Cool: 0
      };
      regen(m);
      S.markets.set(m.key, m);
      if (!quiet) emit('mkt', mktData(m));
      return m;
    }
    function mktData(m) {
      return { v: m.v, tk: m.tk, ev: m.ev, title: m.title, short: m.short, grp: m.grp, kind: m.kind, og: m.og, oc: m.oc,
               open_ms: m.open_ms, close_ms: m.close_ms, status: m.status, result: m.result, grid: m.grid,
               fee: { type: m.fee.type, mult: m.fee.mult }, shard: m.shard, lip: m.lip,
               start_ms: m.game ? m.game.start : null, man: !!m.manOwned };
    }

    // 15-minute LIP windows
    function lipWindow(spec, openMs, S0, quiet) {
      var close = openMs + Q15, ev = spec.s + '-' + evDate(close) + evTime(close);
      var st = S.lip[spec.s] || (S.lip[spec.s] = { spec: spec, S: S0, key: null });
      st.S0 = S0; st.sig = spec.vol / Math.sqrt(365 * 86400);
      S.ogOutcomes[ev] = ['UP', 'DOWN'];
      var m = addMarket({
        v: 'K', tk: ev + '-' + pad2(etD(close).getUTCMinutes()), ev: ev, bk: 'LIP15',
        title: spec.full + ' price up in next 15 mins?', short: spec.name + ' ' + hmET(close) + ' UP', grp: '15M',
        kind: 'LIP15', og: ev, oc: 'UP', open_ms: openMs, close_ms: close, grid: spec.grid,
        fee: { type: 'QUAD', mult: 1 }, shard: spec.shard, lip: true, p: lipP(st, close, clock())
      }, quiet);
      st.key = m.key;
      m.ref.fair = m.pU; m.ref.sd = 200;
      return m;
    }
    function lipP(st, close, now) {
      var tau = Math.max(1, (close - now) / 1000);
      return ncdf(Math.log(st.S / st.S0) / (st.sig * Math.sqrt(tau)));
    }

    // weather brackets
    function wxEvent(city, day, isToday, quiet) {
      var ev = city.s + '-' + evDate(day + 12 * 3600000);
      var c = Math.round(city.mu + gauss() * 2);
      var w = { city: city, ev: ev, day: day, today: isToday, mu: c + gauss() * 0.8, base: isToday ? 1.7 : 2.8, keys: [], br: [], close: day + 25 * 3600000 };
      var brs = [{ lo: -999, hi: c - 5, tk: 'T' + (c - 4), lbl: '<' + (c - 4), title: '<' + (c - 4) + '\u00b0' }];
      [c - 4, c - 2, c, c + 2].forEach(function (a) { brs.push({ lo: a, hi: a + 1, tk: 'B' + (a + 0.5), lbl: a + '-' + (a + 1), title: a + '-' + (a + 1) + '\u00b0' }); });
      brs.push({ lo: c + 4, hi: 999, tk: 'T' + (c + 3), lbl: '>' + (c + 3), title: '>' + (c + 3) + '\u00b0' });
      w.br = brs;
      S.ogOutcomes[ev] = brs.map(function (b) { return b.lbl; });
      var sig = wxSigma(w, clock());
      brs.forEach(function (b, i) {
        var m = addMarket({
          v: 'K', tk: ev + '-' + b.tk, ev: ev, bk: 'WEATHER',
          title: 'Will the maximum temperature be ' + b.title + ' on ' + longDate(day + 12 * 3600000) + '?',
          short: city.city + ' ' + b.lbl, grp: 'WX ' + city.city + ' ' + mdET(day + 12 * 3600000), kind: 'WEATHER',
          og: ev, oc: b.lbl, open_ms: day - 14 * 3600000, close_ms: w.close, grid: GRID_LINEAR,
          fee: { type: 'QUAD', mult: 1 }, shard: i % 2 ? 'S0' : 'S2', lip: false, p: bracketP(w.mu, sig, b)
        }, quiet);
        w.keys.push(m.key);
      });
      S.wx.push(w);
      return w;
    }
    function wxSigma(w, now) {
      if (!w.today) return w.base;
      var hrsTo17 = (w.day + 17 * 3600000 - now) / 3600000;
      return Math.max(0.45, w.base * Math.min(1.2, Math.max(0.25, hrsTo17 / 8)));
    }
    function bracketP(mu, sig, b) {
      var hi = b.hi >= 999 ? 1 : ncdf((b.hi + 0.5 - mu) / sig), lo = b.lo <= -999 ? 0 : ncdf((b.lo - 0.5 - mu) / sig);
      return Math.max(0.002, Math.min(0.998, hi - lo));
    }

    // games
    function newGame(pair, start, state, venues, opts2, quiet) {
      opts2 = opts2 || {};
      var lg = opts2.league || 'NCAAF';
      var g = {
        n: ++S.gameN, away: pair[0], home: pair[1], awayName: pair[2], homeName: pair[3], start: start, league: lg,
        per: 0, clock: 900, sa: 0, sh: 0, edge: R(-6, 6), bias: gauss() * 0.03, hist: [], staleUntil: 0, scoreN: 0,
        finalAt: null, keys: { K_A: null, K_H: null, P: null }
      };
      if (state) { g.per = state.per; g.clock = state.clock; g.sa = state.sa; g.sh = state.sh; }
      var evK = (lg === 'NFL' ? 'KXNFLGAME-' : 'KXNCAAFGAME-') + evDate(start) + g.away + g.home;
      g.og = evK;
      S.ogOutcomes[evK] = [g.away, g.home];
      var close = start + 6 * 3600000;
      var ph = wp(g);
      var label = g.away + '@' + g.home;
      if (venues.indexOf('K') >= 0) {
        // FBS football pays maker fees on Kalshi; FCS and smaller conferences do not
        var fee = opts2.free ? { type: 'QUAD', mult: 1 } : { type: 'QUAD_MAKER', mult: 1 };
        g.keys.K_A = addMarket({ v: 'K', tk: evK + '-' + g.away, ev: evK, bk: 'GAME_K', title: g.awayName + ' wins', short: label + ' ' + g.away,
          grp: lg, kind: 'GAME', og: evK, oc: g.away, open_ms: start - 7 * 86400000, close_ms: close, grid: GRID_LINEAR, fee: fee,
          shard: 'S2', lip: false, p: 1 - ph, game: g }, quiet).key;
        g.keys.K_H = addMarket({ v: 'K', tk: evK + '-' + g.home, ev: evK, bk: 'GAME_K', title: g.homeName + ' wins', short: label + ' ' + g.home,
          grp: lg, kind: 'GAME', og: evK, oc: g.home, open_ms: start - 7 * 86400000, close_ms: close, grid: GRID_LINEAR, fee: fee,
          shard: 'S2', lip: false, p: ph, game: g }, quiet).key;
      }
      if (venues.indexOf('P') >= 0) {
        var slug = 'aec-' + (lg === 'NFL' ? 'nfl' : 'cfb') + '-' + g.away.toLowerCase() + '-' + g.home.toLowerCase() + '-' + isoET(start);
        g.keys.P = addMarket({ v: 'P', tk: slug, ev: slug, bk: 'GAME_P', title: g.awayName + ' vs. ' + g.homeName + ': ' + g.awayName,
          short: label + ' ' + g.away, grp: lg, kind: 'GAME', og: evK, oc: g.away, open_ms: start - 7 * 86400000, close_ms: close,
          grid: opts2.deci ? GRID_DECI : GRID_LINEAR, fee: { type: 'PM', mult: 1 }, shard: null, lip: false, p: 1 - ph, game: g }, quiet).key;
      }
      S.games.push(g);
      gameRefs(g, clock());
      return g;
    }
    function remaining(g) { if (g.per === 0) return 3600; if (g.per >= 1 && g.per <= 4) return (4 - g.per) * 900 + Math.max(0, g.clock); if (g.per === 5) return Math.max(0, g.clock) * 0.5; return 0; }
    function wp(g) {
      if (g.per === 9) return g.sh > g.sa ? 1 : 0;
      var f = remaining(g) / 3600, mg = g.sh - g.sa;
      return Math.max(0.003, Math.min(0.997, ncdf((mg + g.edge * f) / (13.5 * Math.sqrt(Math.max(f, 0.004))))));
    }
    function gameRefs(g, now) {
      var ph = wp(g);
      g.hist.push({ t: now, p: ph });
      while (g.hist.length > 2 && g.hist[1].t < now - 150000) g.hist.shift();
      var lag = 41500 + 6000 * Math.sin(now / 37000);
      var e = null;
      for (var i = g.hist.length - 1; i >= 0; i--) if (g.hist[i].t <= now - lag) { e = g.hist[i]; break; }
      var per = g.per === 0 ? 'PRE' : (g.per === 9 ? 'FINAL' : (g.per === 5 ? 'OT' : 'Q' + g.per));
      var clk = g.per === 0 ? hmET(g.start) : (g.per === 9 ? '0:00' : Math.floor(Math.max(0, g.clock) / 60) + ':' + pad2(Math.floor(Math.max(0, g.clock) % 60)));
      var game = { per: per, clock: clk, score: g.sa + '-' + g.sh, src: 'LIVE' };
      [['K_A', false], ['K_H', true], ['P', false]].forEach(function (x) {
        var m = g.keys[x[0]] && S.markets.get(g.keys[x[0]]);
        if (!m || m.status === 'SETTLED') return;
        var p = x[1] ? ph : 1 - ph;
        m.pU = p * U;
        var b = x[1] ? g.bias : -g.bias;
        m.ref.fair = Math.max(10, Math.min(U - 10, (p + b) * U));
        m.ref.sd = 400;
        if (g.per >= 1 && e) { m.ref.espn = (x[1] ? e.p : 1 - e.p) * U; m.ref.espn_ts_ms = e.t; }
        else { m.ref.espn = null; m.ref.espn_ts_ms = null; }
        m.ref.game = game;
      });
    }

    // the universe at boot
    (function boot() {
      var now = clock();
      var q0 = Math.floor(now / Q15) * Q15;
      LIP.forEach(function (sp) {
        var st = S.lip[sp.s] = { spec: sp, S: sp.S, S0: sp.S, sig: sp.vol / Math.sqrt(365 * 86400), key: null };
        var S0 = sp.S * Math.exp(-st.sig * Math.sqrt(Math.max(1, (now - q0) / 1000)) * gauss());
        lipWindow(sp, q0, S0, true);
      });
      var today = etMidnight(now);
      WX.forEach(function (c) { wxEvent(c, today, true, true); });
      WX.forEach(function (c) { if (c.tomorrow) wxEvent(c, today + 86400000, false, true); });
      var t = TEAMS;
      newGame(t[0], now - 95 * 60000, { per: 2, clock: 521, sa: 10, sh: 14 }, ['K', 'P'], { deci: false }, true);
      newGame(t[1], now - 150 * 60000, { per: 3, clock: 130, sa: 21, sh: 17 }, ['K', 'P'], { deci: true }, true);
      newGame(t[2], now + 20 * 60000, null, ['K'], { free: true }, true);
      S.teamIdx = 3;
      newGame(['KC', 'BUF', 'Kansas City', 'Buffalo'], today + 86400000 + 20.25 * 3600000, null, ['P'], { league: 'NFL', deci: true }, true);
      LANES.forEach(function (l) {
        var L = { id: l.id, name: l.name, venue: l.venue, style: l.style, budgetU: l.budget * U, thrU: l.thr === null ? null : l.thr * U,
                  on: l.on, state: 'OFF', why: 'STOP', sent: 0, hit: 0, fills: 0, ctrs: 0, feesU: 0, realU: 0, lipU: 0,
                  last_ms: null, lastJson: '', lastEmit: 0 };
        S.lanes.push(L); S.laneById[L.id] = L;
      });
      S.alerts.push({ id: 'a-' + (++S.alertN), lvl: 'INFO', src: 'SIM', msg: 'demo venues up: ' + S.markets.size + ' markets', ts_ms: now });
    })();

    // ------------------------------------------------------------------------------------------
    // books
    // ------------------------------------------------------------------------------------------
    function sizeFor(m, old, i) {
      if (old && rnd() < 0.8) return Math.max(1, r2(old * R(0.9, 1.12)));
      var s = m.k.sz * Math.exp(gauss() * 0.6) * (1 + i * 0.15);
      return Math.max(1, rnd() < 0.1 ? r2(s) : Math.round(s));
    }
    function nextLevel(g, p, dir) {
      var tick = G.tickAt(g, dir > 0 ? p + 1 : p);
      var q = G.step(g, p, dir * (tick < 100 ? Math.max(1, Math.round(100 / tick)) : 1));
      return q === p ? null : q;
    }
    function regen(m) {
      var g = m.g, half = m.spreadU / 2;
      var bb = G.snap(g, m.mid - half, -1), ba = G.snap(g, m.mid + half, 1);
      if (bb !== null && ba !== null && ba <= bb) ba = G.step(g, bb, 1);
      if (bb !== null && ba !== null && ba === bb) bb = null;
      var nb = new Map(), na = new Map(), p, i;
      for (p = bb, i = 0; p !== null && i < m.k.depth; i++, p = nextLevel(g, p, -1)) if (i === 0 || rnd() > m.k.holes) nb.set(p, sizeFor(m, m.bids.get(p), i));
      for (p = ba, i = 0; p !== null && i < m.k.depth; i++, p = nextLevel(g, p, 1)) if (i === 0 || rnd() > m.k.holes) na.set(p, sizeFor(m, m.asks.get(p), i));
      m.bids = nb; m.asks = na; m.lastRegenMid = m.mid; m.bookDirty = true; m.exMs = clock() - S.lat.kx;
      m.ours.forEach(function (o) { var s = (o.side === 'BUY' ? nb : na).get(o.px) || 0; if (o.ahead > s) o.ahead = s; });
    }
    function othersBest(m, side) {
      var best = null;
      (side === 'bid' ? m.bids : m.asks).forEach(function (s, p) { if (best === null || (side === 'bid' ? p > best : p < best)) best = p; });
      return best;
    }
    // best price on one side counting our resting orders. ourSide 'BUY' = bids.
    function bestLevel(m, ourSide) {
      var best = othersBest(m, ourSide === 'BUY' ? 'bid' : 'ask');
      m.ours.forEach(function (o) { if (o.side === ourSide && (best === null || (ourSide === 'BUY' ? o.px > best : o.px < best))) best = o.px; });
      return best;
    }
    function bookLevels(m, side) {
      var mp = new Map(side === 'BUY' ? m.bids : m.asks);
      m.ours.forEach(function (o) { if (o.side === side) mp.set(o.px, r2((mp.get(o.px) || 0) + o.left)); });
      var arr = []; mp.forEach(function (s, p) { if (s > 0) arr.push([p, s]); });
      arr.sort(function (a, b) { return side === 'BUY' ? b[0] - a[0] : a[0] - b[0]; });
      return arr.slice(0, 100);
    }
    function print(m, px, q, side, now) {
      var t = { px: px, qty: r2(q), side: side, ts_ms: now };
      m.prints.push(t); if (m.prints.length > 50) m.prints.shift();
      m.last = t; m.vol = r2(m.vol + q); m.bookDirty = true; m.exMs = now - S.lat.kx;
    }
    // consume liquidity resting at one price level. Returns {q traded, stp, progress}.
    function takeLevel(m, px, ourSide, Q, incoming, now) {
      var book = ourSide === 'BUY' ? m.bids : m.asks;
      var others = book.get(px) || 0;
      var queue = m.ours.filter(function (o) { return o.side === ourSide && o.px === px && !terminal(o.st); })
        .sort(function (a, b) { return a.restAt - b.restAt || a.n - b.n; });
      var q = Q, consumed = 0, traded = 0, stp = false, progress = false;
      for (var i = 0; i < queue.length && q > 1e-9; i++) {
        var o = queue[i];
        var ahead = Math.max(0, Math.min(o.ahead, others) - consumed);
        var take = Math.min(q, ahead); consumed += take; q -= take; traded += take;
        if (q <= 1e-9) break;
        if (incoming) {
          if (incoming.own === 'MAN') { finish(o, 'CXL', 'STP', now); progress = true; continue; }
          stp = true; break;
        }
        var f = Math.min(q, o.left);
        fillOrder(o, px, f, 'M', now); q -= f; traded += f;
      }
      if (!stp && q > 1e-9) { var take2 = Math.min(q, others - consumed); consumed += take2; q -= take2; traded += take2; }
      var rest = r2(others - consumed);
      if (rest > 1e-9) book.set(px, rest); else book.delete(px);
      queue.forEach(function (o) { if (!terminal(o.st)) o.ahead = Math.max(0, r2(o.ahead - consumed)); });
      m.bookDirty = true;
      return { q: r2(traded), stp: stp, progress: progress || traded > 0 };
    }
    // anonymous flow: one taker order sweeping up to one tick through the touch
    function flowTrade(m, now, side, qty) {
      if (!side) side = rnd() < 0.5 + Math.max(-0.35, Math.min(0.35, (m.pU - m.mid) / 600)) ? 'BUY' : 'SELL';
      if (!qty) qty = Math.max(1, Math.round(m.k.sz * 0.3 * Math.exp(gauss() * 0.8)));
      var ourSide = side === 'BUY' ? 'SELL' : 'BUY';
      var best = bestLevel(m, ourSide); if (best === null) return 0;
      var limit = G.step(m.g, best, side === 'BUY' ? 1 : -1), done = 0;
      while (qty - done > 1e-9) {
        var px = bestLevel(m, ourSide);
        if (px === null || (side === 'BUY' ? px > limit : px < limit)) break;
        var res = takeLevel(m, px, ourSide, qty - done, null, now);
        if (res.q > 0) { print(m, px, res.q, side, now); done += res.q; }
        if (!res.progress) break;
      }
      return done;
    }
    // the market traded through one of our resting orders
    function crossOurs(m, now) {
      m.ours.slice().forEach(function (o) {
        if (terminal(o.st)) return;
        var opp = othersBest(m, o.side === 'BUY' ? 'ask' : 'bid');
        if (opp === null || (o.side === 'BUY' ? opp > o.px : opp < o.px)) return;
        var q = o.left;
        fillOrder(o, o.px, q, 'M', now);
        print(m, o.px, q, o.side === 'BUY' ? 'SELL' : 'BUY', now);
        var book = o.side === 'BUY' ? m.asks : m.bids;
        Array.from(book.keys()).forEach(function (p) { if (o.side === 'BUY' ? p <= o.px : p >= o.px) book.delete(p); });
      });
    }

    // ------------------------------------------------------------------------------------------
    // orders
    // ------------------------------------------------------------------------------------------
    function newOrder(req, own, lane, m, now) {
      var o = {
        n: ++S.cidN, cid: req.cid, oid: null, v: req.v, tk: req.tk, key: req.v + '|' + req.tk, side: req.side,
        px: N.px(req.px), qty: r2(req.qty), filled: 0, left: r2(req.qty), avgNum: 0, tif: req.tif,
        exp_ms: req.tif === 'POST' && req.ttl_s ? Math.ceil((now + 1 + req.ttl_s * 1000) / 1000) * 1000 : (req.exp_ms || null),
        ttl_s: req.ttl_s, st: 'PEND', why: '', own: own, lane: lane || null, ahead: 0, restAt: 0, fee: 0,
        touch: req.touch ? N.px(req.touch) : null, edge: null, acc: FEES.newAcc(),
        t: { click: req.t_click_ms || null, recv: now, sent: now + 1, ack: null, done: null },
        dirty: true, fills: [], cancelOnAck: null, cxlPending: false, hitCounted: false, at: now
      };
      if (m && m.ref.fair !== null && m.ref.fair !== undefined) o.edge = Math.round((m.ref.fair - o.px) * (o.side === 'BUY' ? 1 : -1));
      return o;
    }
    // every record of an order goes through here: rev counts the changes (the same state always
    // carries the same rev), so the page can order the stream against the HTTP replies
    function ordData(o) {
      var resting = o.st === 'WORK' || o.st === 'PART';
      var d = {
        cid: o.cid, oid: o.oid, v: o.v, tk: o.tk, side: o.side, px: pxs(o.px), qty: r2(o.qty), filled: r2(o.filled),
        left: r2(o.left), avg: o.filled > 0 ? pxs(o.avgNum / o.filled) : null, tif: o.tif, exp_ms: o.exp_ms,
        st: o.st, why: o.why, own: o.own, lane: o.lane, piq: resting ? r2(o.ahead) : null, fee: ms(o.fee),
        touch: o.touch === null ? null : pxs(o.touch), edge: o.edge === null ? null : ms(o.edge),
        t: { click: o.t.click, recv: o.t.recv, sent: o.t.sent, ack: o.t.ack, done: o.t.done }, rev: 0
      };
      var sig = [d.oid, d.px, d.qty, d.filled, d.left, d.avg, d.exp_ms, d.st, d.why, d.piq, d.fee, d.t.sent, d.t.ack, d.t.done].join('|');
      if (sig !== o.revSig) { o.revSig = sig; o.rev = (o.rev || 0) + 1; }
      d.rev = o.rev;
      return d;
    }
    function fillData(f) {
      return { fid: f.fid, cid: f.cid, oid: f.oid, v: f.v, tk: f.tk, side: f.side, px: pxs(f.px), qty: f.qty, liq: f.liq,
               fee: ms(f.fee), ts_ms: f.ts_ms, own: f.own, lane: f.lane, mid: f.mid === null ? null : pxs(f.mid),
               fair: f.fair === null ? null : pxs(f.fair), real: ms(f.real) };
    }
    function finish(o, st, why, now) {
      if (terminal(o.st)) return;
      o.st = st; o.why = why; o.left = 0; o.t.done = now; o.dirty = true;
      unrest(o); S.live.delete(o);
    }
    function unrest(o) {
      var m = S.markets.get(o.key); if (!m) return;
      var i = m.ours.indexOf(o); if (i >= 0) { m.ours.splice(i, 1); m.bookDirty = true; }
    }
    function err(code, msg) { return { code: code, msg: msg }; }
    function httpOf(code) { return C.errors[code] ? C.errors[code].http : 500; }

    function posOf(key) { var P = S.pos.get(key); return P ? P.pos : 0; }
    // reservations by the contract's rule (Desk.risk): a set of live orders, summed over markets
    function reserved(pred) {
      var list = []; S.live.forEach(function (o) { if (!pred || pred(o)) list.push(o); });
      var tot = 0; RISK.byMarket(list, posOf).forEach(function (u) { tot += u; });
      return tot;
    }
    // what one more order adds to its market's reservation, given the orders already there
    function addedReserve(key, side, px, qty, pred) {
      var list = []; S.live.forEach(function (o) { if (o.key === key && (!pred || pred(o))) list.push(o); });
      var before = RISK.market(list, posOf(key));
      return RISK.market(list.concat([{ side: side, px: px, left: qty }]), posOf(key)) - before;
    }
    function freeOf(shard) {
      return S.cash[shard] - reserved(function (o) { var m = S.markets.get(o.key); return !!m && (m.shard || 'PM') === shard; });
    }
    function checkOrder(req, own, m, now) {
      if (!m) return err('UNKNOWN_MARKET', req.v + '|' + req.tk);
      if (m.status !== 'OPEN') return err('MARKET_CLOSED', m.tk + ' is ' + m.status);
      var px = N.px(req.px);
      if (!G.onGrid(m.g, px)) return err('OFF_GRID', req.px + ' not on the grid');
      if (!(req.qty > 0) || Math.abs(req.qty * 100 - Math.round(req.qty * 100)) > 1e-6 || (m.v === 'P' && req.qty % 1 !== 0))
        return err('BAD_QTY', 'qty ' + req.qty);
      var hasTtl = req.ttl_s !== null && req.ttl_s !== undefined, hasExp = req.exp_ms !== null && req.exp_ms !== undefined;
      if (own !== 'EXT' && (req.tif === 'POST' ? hasTtl === hasExp : (hasTtl || hasExp))) return err('BAD_REQUEST', 'POST needs one of ttl_s and exp_ms; IOC and FOK neither');
      // an absolute expiry: a whole second, 5 s ahead at least, never past the market's close
      // ("until close" sends the close itself: Kalshi expiration_ts / Polymarket US GTD at the close)
      if (hasExp && (req.exp_ms % 1000 !== 0 || req.exp_ms < now + 5000 || (own !== 'EXT' && req.exp_ms > m.close_ms))) return err('BAD_REQUEST', 'exp_ms must be a whole second, 5 s ahead at least and no later than the close');
      if (req.reduce_only && req.tif !== 'IOC') return err('BAD_REQUEST', 'reduce_only is IOC only');
      if (req.reduce_only) { var rp = posOf(m.key); if (req.side === 'BUY' ? rp >= 0 : rp <= 0) return err('REDUCE', rp === 0 ? 'no position' : 'would increase the position'); }
      if (S.run.state === 'KILLED' && own !== 'EXT') return err('KILLED', 'kill lockout');
      if (own === 'SYS' && S.run.state !== 'RUN') return err('BAD_STATE', 'systematic not running');
      if (m.v === 'K' && (!S.feeds.KXP.up || !S.feeds.KX.up || S.feeds.KX.gap)) return err('STALE', 'Kalshi feed down');
      if (m.v === 'P' && !S.feeds.PM.up) return err('STALE', 'Polymarket feed down');
      if (m.v === 'P' && S.pmRO) return err('RO', 'PM key failing');
      if (own === 'MAN' && !req.override) {
        if (req.side === 'BUY') { var ba = bestLevel(m, 'SELL'); if (ba !== null && px > G.step(m.g, ba, 2)) return err('PRICE_BAND', 'BUY ' + req.px + ' is ' + G.ticks(m.g, ba, px) + ' ticks through ask ' + N.pxStr(ba) + ' (max 2)'); }
        else { var bb = bestLevel(m, 'BUY'); if (bb !== null && px < G.step(m.g, bb, -2)) return err('PRICE_BAND', 'SELL ' + req.px + ' is ' + G.ticks(m.g, px, bb) + ' ticks through bid ' + N.pxStr(bb) + ' (max 2)'); }
      }
      var shard = m.shard || 'PM';
      var need = addedReserve(m.key, req.side, px, req.qty) + Math.max(0, FEES.estimate(m.v, m.fee, req.side, px, req.qty, 'T'));
      if (need > freeOf(shard)) return err(m.v === 'K' ? 'SHARD_CASH' : 'NO_CASH', 'needs ' + N.moneyStr(need) + ', ' + shard + ' free ' + N.moneyStr(freeOf(shard)));
      if (own === 'SYS') {
        var cl = ceilings(), sysAdd = addedReserve(m.key, req.side, px, req.qty, function (o) { return o.own === 'SYS'; });
        if (cl[m.v].used + sysAdd > cl[m.v].eff) return err('SYS_CEILING', 'systematic ceiling');
      }
      return null;
    }
    function reply2(reply, status, json, delay) { if (reply) at(clock() + (delay || 2), function () { reply(status, json); }); }

    function submit(req, own, lane, reply) {
      var now = clock();
      var m = S.markets.get(req.v + '|' + req.tk);
      if (S.orders.has(req.cid)) {
        reply2(reply, 409, { ok: false, err: err('DUP_CID', 'cid ' + req.cid + ' already used'), ord: ordData(S.orders.get(req.cid)) });
        return null;
      }
      var e = checkOrder(req, own, m, now);
      if (!e && req.reduce_only) { var rq = Math.abs(posOf(m.key)); if (req.qty > rq) req = Object.assign({}, req, { qty: rq }); }
      if (e) {
        var rec = newOrder(req, own, lane, m, now);
        rec.st = 'REJ'; rec.why = C.enums.WHY.indexOf(e.code) >= 0 ? e.code : 'INTERNAL'; rec.left = 0; rec.t.sent = null; rec.t.done = now; rec.exp_ms = null;
        S.orders.set(rec.cid, rec);
        emit('ord', ordData(rec)); rec.dirty = false;
        if (own === 'MAN') alert('WARN', 'GW', 'REJ ' + e.code + ' ' + req.side + ' ' + req.qty + ' ' + req.tk + ' @ ' + req.px + ': ' + e.msg);
        reply2(reply, httpOf(e.code), { ok: false, err: e, ord: ordData(rec) });
        return null;
      }
      var o = newOrder(req, own, lane, m, now);
      S.orders.set(o.cid, o); S.live.add(o);
      if (lane) { var L = S.laneById[lane]; L.sent++; L.last_ms = now; }
      emit('ord', ordData(o)); o.dirty = false;
      if (own === 'MAN') takeover(m, now);
      at(now + latency(m.v), function () { ack(o, reply); });
      return o;
    }
    function takeover(m, now) {
      var n = 0;
      S.live.forEach(function (o) { if (o.key === m.key && o.own === 'SYS') { cancelOrder(o, 'TAKEOVER'); n++; } });
      if (!m.manOwned) { m.manOwned = true; emit('mkt', mktData(m)); if (n) alert('INFO', 'GW', 'TAKEOVER ' + m.tk + ': ' + n + ' systematic orders cancelled'); }
    }
    function ack(o, reply) {
      var now = clock();
      if (o.st !== 'PEND') { if (reply) reply(200, { ok: true, ord: ordData(o) }); return; }
      var m = S.markets.get(o.key);
      o.oid = o.v + '-' + (1000000 + (++S.oidN)); o.t.ack = now;
      S.gwAcks.push(now - o.t.recv); if (S.gwAcks.length > 20) S.gwAcks.shift();
      if (!m || m.status !== 'OPEN') finish(o, 'CXL', 'MARKET_CLOSED', now);
      else if (o.tif === 'IOC' || o.tif === 'FOK') {
        if (o.tif === 'FOK' && available(m, o) < o.qty - 1e-9) finish(o, 'CXL', 'FOK', now);
        else {
          var stp = sweep(o, m, now);
          if (o.left > 1e-9) finish(o, 'CXL', stp ? 'STP' : 'IOC_REST', now);
        }
      } else if (crosses(m, o)) finish(o, 'CXL', 'POST_CROSS', now);
      else if (o.cancelOnAck) finish(o, 'CXL', o.cancelOnAck, now);
      else {
        o.st = 'WORK'; o.restAt = now; o.ahead = (o.side === 'BUY' ? m.bids : m.asks).get(o.px) || 0;
        m.ours.push(o); m.bookDirty = true;
        scheduleExpiry(o);
      }
      o.dirty = true;
      flush(now);
      if (reply) reply(200, { ok: true, ord: ordData(o) });
    }
    function scheduleExpiry(o) { if (o.exp_ms) at(o.exp_ms, function () { expire(o); }); }
    function expire(o) {
      var now = clock();
      if ((o.st === 'WORK' || o.st === 'PART') && o.exp_ms && now >= o.exp_ms) { finish(o, 'EXP', 'EXPIRY', now); flush(now); }
    }
    function crosses(m, o) {
      var opp = bestLevel(m, o.side === 'BUY' ? 'SELL' : 'BUY');
      return opp !== null && (o.side === 'BUY' ? opp <= o.px : opp >= o.px);
    }
    function available(m, o) {
      var tot = 0, book = o.side === 'BUY' ? m.asks : m.bids;
      book.forEach(function (s, p) { if (o.side === 'BUY' ? p <= o.px : p >= o.px) tot += s; });
      return tot;
    }
    function sweep(o, m, now) {
      var ourSide = o.side === 'BUY' ? 'SELL' : 'BUY';
      while (o.left > 1e-9) {
        var px = bestLevel(m, ourSide);
        if (px === null || (o.side === 'BUY' ? px > o.px : px < o.px)) break;
        var res = takeLevel(m, px, ourSide, o.left, o, now);
        if (res.q > 0) { fillOrder(o, px, res.q, 'T', now); print(m, px, res.q, o.side, now); }
        if (res.stp) return true;
        if (!res.progress) break;
      }
      return false;
    }
    function cancelOrder(o, why) {
      if (terminal(o.st)) return false;
      if (o.st === 'PEND') { o.cancelOnAck = why; return true; }
      if (o.cxlPending) return true;
      o.cxlPending = true;
      at(clock() + latency(o.v), function () {
        o.cxlPending = false;
        if (o.st === 'WORK' || o.st === 'PART') { finish(o, 'CXL', why, clock()); flush(clock()); }
      });
      return true;
    }

    // fills, positions, cash
    function getPos(key) {
      var P = S.pos.get(key);
      if (!P) { var m = S.markets.get(key); P = { key: key, v: m.v, tk: m.tk, pos: 0, avg: 0, real: 0, fees: 0, own: {}, lane: null, dirty: true, lastJson: '' }; S.pos.set(key, P); }
      return P;
    }
    function fillOrder(o, px, q, liq, now) {
      q = r2(q); if (q <= 0) return null;
      var m = S.markets.get(o.key), P = getPos(o.key);
      var fee;
      if (m.v === 'P') fee = FEES.pmFill(o.acc, px, q, liq);
      else {
        var rev = o.side === 'BUY' ? (P.pos < 0 ? (U - px) * q : -px * q) : (P.pos > 0 ? px * q : -(U - px) * q);
        fee = FEES.kalshiFill(o.acc, rev, px, q, liq, m.fee);
      }
      o.filled = r2(o.filled + q); o.left = r2(o.left - q); o.avgNum += px * q; o.fee += fee;
      var real = applyPos(P, m, o, px, q, fee);
      var f = { n: ++S.fidN, fid: o.v + '-F-' + (100000 + S.fidN), cid: o.cid, oid: o.oid, v: o.v, tk: o.tk, key: o.key, side: o.side,
                px: px, qty: q, liq: liq, fee: fee, ts_ms: now, own: o.own, lane: o.lane, mid: Math.round(m.mid),
                fair: m.ref.fair === null ? null : Math.round(m.ref.fair), real: real, mo: {}, emitted: false };
      S.fills.push(f); S.fillById.set(f.fid, f); o.fills.push(f);
      if (S.fills.length > 20000) { var gone = S.fills.splice(0, 5000); gone.forEach(function (x) { S.fillById.delete(x.fid); }); }
      if (o.lane) {
        var L = S.laneById[o.lane];
        L.fills++; L.ctrs = r2(L.ctrs + q); L.feesU += fee; L.realU += real;
        if (!o.hitCounted) { o.hitCounted = true; L.hit++; }
        if (fee > 0) S.costU += fee;
      }
      [1000, 10000, 60000].forEach(function (h) { at(now + h, function () { markout(f, h === 1000 ? '1s' : (h === 10000 ? '10s' : '60s')); }); });
      if (o.left <= 1e-9) { o.left = 0; if (!terminal(o.st)) { o.st = 'FILL'; o.why = ''; o.t.done = now; unrest(o); S.live.delete(o); } }
      else if (o.st === 'WORK') o.st = 'PART';
      o.dirty = true;
      return f;
    }
    function applyPos(P, m, o, px, q, fee) {
      var shard = m.shard || 'PM', realized = 0, rem = q;
      if (o.side === 'BUY') {
        if (P.pos < 0) { var r = Math.min(rem, -P.pos); realized += (P.avg - px) * r; S.cash[shard] += (U - px) * r; P.pos = r2(P.pos + r); rem = r2(rem - r); if (P.pos === 0) P.avg = 0; }
        if (rem > 0) { P.avg = (P.avg * P.pos + px * rem) / (P.pos + rem); P.pos = r2(P.pos + rem); S.cash[shard] -= px * rem; }
      } else {
        if (P.pos > 0) { var r2_ = Math.min(rem, P.pos); realized += (px - P.avg) * r2_; S.cash[shard] += px * r2_; P.pos = r2(P.pos - r2_); rem = r2(rem - r2_); if (P.pos === 0) P.avg = 0; }
        if (rem > 0) { var a = -P.pos; P.avg = (P.avg * a + px * rem) / (a + rem); P.pos = r2(P.pos - rem); S.cash[shard] -= (U - px) * rem; }
      }
      S.cash[shard] -= fee;
      P.real += realized - fee; P.fees += fee;
      var ownP = o.own === 'SYS' ? 'SYS' : 'MAN';            // EXT fills count as the operator's
      S.realBy[ownP] += realized - fee;
      if (P.pos === 0) P.own = {}; else P.own[ownP] = true;
      if (o.own === 'SYS') P.lane = o.lane;
      P.dirty = true;
      return realized - fee;
    }
    function markout(f, h) {
      var m = S.markets.get(f.key); if (!m) return;
      var mid = m.status === 'SETTLED' ? (m.result === 'YES' ? U : 0) : m.mid;
      var mo = (mid - f.px) * (f.side === 'BUY' ? 1 : -1);
      f.mo[h] = mo;
      var rec = { fid: f.fid, h: h, mo: ms(mo), mid: pxs(mid) };
      S.mos.push(rec); if (S.mos.length > 60000) S.mos.splice(0, 15000);
      emit('mo', rec);
    }
    function posData(P) {
      var m = S.markets.get(P.key), x = posMath(P, m);
      var own = P.pos === 0 ? null : (P.own.MAN && P.own.SYS ? 'MIX' : (P.own.SYS ? 'SYS' : 'MAN'));
      return { v: P.v, tk: P.tk, pos: P.pos, avg: P.pos === 0 ? null : pxs(P.avg), own: own, mark: x.mark === null ? null : pxs(x.mark),
               unrl: ms(x.unrl), real: ms(P.real), fees: ms(P.fees), maxl: ms(x.maxl), if_yes: ms(x.ifYes), if_no: ms(x.ifNo) };
    }
    // a position re-emits on a mark move only when its P&L moved by a cent
    function posSig(d) { return [d.pos, d.avg, d.own, d.real, d.fees, d.maxl, Math.round(N.money(d.unrl) / 100)].join('|'); }
    function posMath(P, m) {
      var mark = m && P.pos ? (m.status === 'SETTLED' ? (m.result === 'YES' ? U : 0) : m.mid) : null;
      var a = Math.abs(P.pos), unrl = 0, ifYes = 0, ifNo = 0;
      if (P.pos > 0) { unrl = (mark - P.avg) * a; ifYes = (U - P.avg) * a; ifNo = -P.avg * a; }
      else if (P.pos < 0) { unrl = (P.avg - mark) * a; ifYes = -(U - P.avg) * a; ifNo = P.avg * a; }
      return { mark: mark === null ? null : Math.round(mark), unrl: P.pos ? unrl : 0, ifYes: ifYes, ifNo: ifNo, maxl: Math.max(0, -Math.min(ifYes, ifNo)) };
    }

    // ------------------------------------------------------------------------------------------
    // settlement and rolls
    // ------------------------------------------------------------------------------------------
    function closeMarket(m, now) {
      if (m.status !== 'OPEN') return;
      m.status = 'CLOSED';
      S.live.forEach(function (o) { if (o.key === m.key) { if (o.st === 'PEND') o.cancelOnAck = 'MARKET_CLOSED'; else finish(o, 'CXL', 'MARKET_CLOSED', now); } });
      emit('mkt', mktData(m));
    }
    function settle(m, result, now) {
      if (m.status === 'SETTLED') return;
      closeMarket(m, now);
      m.status = 'SETTLED'; m.result = result; m.settledAt = now;
      var P = S.pos.get(m.key);
      if (P && P.pos !== 0) {
        var x = posMath(P, null), shard = m.shard || 'PM', a = Math.abs(P.pos);
        var payout = P.pos > 0 ? (result === 'YES' ? U * a : 0) : (result === 'NO' ? U * a : 0);
        S.cash[shard] += payout;
        var realized = result === 'YES' ? x.ifYes : x.ifNo;
        P.real += realized;
        var owner = P.own.SYS && !P.own.MAN ? 'SYS' : 'MAN';
        S.realBy[owner] += realized;
        if (P.lane && owner === 'SYS') S.laneById[P.lane].realU += realized;
        P.pos = 0; P.avg = 0; P.own = {}; P.dirty = true;
      }
      var v = result === 'YES' ? U : 0;
      S.fills.forEach(function (f) {
        if (f.key !== m.key || f.mo.SETTLE !== undefined) return;
        var mo = (v - f.px) * (f.side === 'BUY' ? 1 : -1);
        f.mo.SETTLE = mo;
        var rec = { fid: f.fid, h: 'SETTLE', mo: ms(mo), mid: pxs(v) };
        S.mos.push(rec); emit('mo', rec);
      });
      emit('mkt', mktData(m));
      flush(now);
    }
    function rolls(now) {
      Object.keys(S.lip).forEach(function (s) {
        var st = S.lip[s], m = S.markets.get(st.key);
        if (!m || now < m.close_ms) return;
        var res = st.S >= st.S0 ? 'YES' : 'NO';
        settle(m, res, now);
        lipWindow(st.spec, m.close_ms, st.S, false);
        if (S.run.state === 'RUN' && S.laneById.M1.on) alert('INFO', 'M1', 'roll ' + st.spec.name + ' ' + hmET(m.close_ms + Q15) + ' (' + st.spec.name + ' ' + hmET(m.close_ms) + ' ' + res + ')');
      });
      S.wx.slice().forEach(function (w) {
        if (now < w.close) return;
        var x = Math.round(w.mu + gauss() * 0.5);
        w.keys.forEach(function (k, i) { var b = w.br[i], m = S.markets.get(k); if (m) settle(m, x >= b.lo && x <= b.hi ? 'YES' : 'NO', now); });
        S.wx.splice(S.wx.indexOf(w), 1);
        wxEvent(w.city, w.day + 2 * 86400000, false, false);
      });
      S.games.slice().forEach(function (g) {
        if (g.per !== 9 || !g.finalAt) return;
        var keys = [g.keys.K_A, g.keys.K_H, g.keys.P].filter(Boolean);
        if (now >= g.finalAt && now < g.finalAt + 30000) keys.forEach(function (k) { var m = S.markets.get(k); if (m) closeMarket(m, now); });
        if (now >= g.finalAt + 30000) {
          var homeWon = g.sh > g.sa;
          keys.forEach(function (k) {
            var m = S.markets.get(k); if (!m) return;
            settle(m, (m.oc === g.home) === homeWon ? 'YES' : 'NO', now);
          });
          S.games.splice(S.games.indexOf(g), 1);
          var pair = TEAMS[S.teamIdx++ % TEAMS.length];
          newGame(pair, now + 10 * 60000, null, rnd() < 0.5 ? ['K', 'P'] : ['K'], { deci: rnd() < 0.5 }, false);
        }
      });
      // forget markets settled more than 30 minutes ago
      S.markets.forEach(function (m, k) { if (m.status === 'SETTLED' && now - m.settledAt > 30 * 60000) S.markets.delete(k); });
      // forget terminal orders older than 10 minutes
      if (S.orders.size > 400) S.orders.forEach(function (o, cid) { if (terminal(o.st) && now - o.t.done > 120000) S.orders.delete(cid); });
    }

    // ------------------------------------------------------------------------------------------
    // the world tick
    // ------------------------------------------------------------------------------------------
    function stepWorld(now, dt) {
      Object.keys(S.lip).forEach(function (s) {
        var st = S.lip[s], m = S.markets.get(st.key); if (!m || m.status !== 'OPEN') return;
        st.S *= Math.exp(st.sig * Math.sqrt(dt) * gauss());
        m.pU = lipP(st, m.close_ms, now) * U;
        st.bias = (st.bias || 0) * Math.exp(-dt / 40) + gauss() * 45 * Math.sqrt(dt);
        m.ref.fair = Math.max(10, Math.min(U - 10, m.pU + st.bias));
      });
      S.wx.forEach(function (w) {
        w.mu += gauss() * 0.01 * Math.sqrt(dt);
        var sig = wxSigma(w, now);
        w.keys.forEach(function (k, i) { var m = S.markets.get(k); if (m && m.status === 'OPEN') m.pU = bracketP(w.mu, sig, w.br[i]) * U; });
      });
      S.games.forEach(function (g) {
        if (g.per === 9) return;
        if (g.per === 0 && now >= g.start) { g.per = 1; g.clock = 900; }
        if (g.per >= 1) {
          g.clock -= dt * rollRate;
          if (rnd() < 0.0016 * dt) {
            var homeScores = rnd() < 0.5 + g.edge / 40, pts = rnd() < 0.68 ? 7 : 3;
            if (homeScores) g.sh += pts; else g.sa += pts;
            g.scoreN++; g.staleUntil = now + R(3000, 8000);
          }
          if (g.clock <= 0) {
            if (g.per < 4) { g.per++; g.clock = 900; }
            else if (g.sa === g.sh) { if (g.per === 5) { if (rnd() < 0.5) g.sh += 3; else g.sa += 3; g.per = 9; g.finalAt = now; } else { g.per = 5; g.clock = 300; } }
            else { g.per = 9; g.finalAt = now; }
          }
        }
        gameRefs(g, now);
      });
      S.markets.forEach(function (m) {
        if (m.status !== 'OPEN' || m.frozen) return;
        var target = m.pU;
        if (m.dislocate && now < m.dislocate.until) target += m.dislocate.shift; else m.dislocate = null;
        var stale = m.game && m.game.staleUntil > now;
        m.spreadU = Math.max(m.k.spread * 0.5, Math.min(m.k.spread * 2, m.spreadU * Math.exp(gauss() * 0.05 * Math.sqrt(dt))));
        if (!stale) {
          m.mid += (target - m.mid) * (1 - Math.exp(-dt / m.k.tau)) + gauss() * m.k.noise * Math.sqrt(dt);
          m.mid = Math.max(30, Math.min(U - 30, m.mid));
          if (rnd() < m.k.upd * dt || Math.abs(m.mid - m.lastRegenMid) > m.g.minStep * 0.6) regen(m);
        }
        var nT = poisson(m.k.rate * dt * (stale ? 3 : 1));
        for (var i = 0; i < nT; i++) flowTrade(m, now);
        crossOurs(m, now);
      });
    }
    function dislocate(now) {
      if (now < S.nextDislocate) return;
      S.nextDislocate = now + R(60000, 150000);
      var pick = rnd(), m = null, shift = 0;
      if (pick < 0.6 && S.wx.length) {
        var w = S.wx[Math.floor(rnd() * S.wx.length)];
        w.keys.forEach(function (k) { var x = S.markets.get(k); if (x && x.status === 'OPEN' && (!m || x.pU > m.pU)) m = x; });
        if (m) shift = -Math.min(m.mid - 100, R(1800, 2600));
      } else {
        var gs = S.games.filter(function (g) { return g.per >= 1 && g.per <= 5 && g.keys.P; });
        if (gs.length) { m = S.markets.get(gs[Math.floor(rnd() * gs.length)].keys.P); shift = (rnd() < 0.5 ? -1 : 1) * R(1200, 1600); }
      }
      if (!m || m.status !== 'OPEN' || m.frozen || Math.abs(shift) < 500) return;
      m.dislocate = { until: now + R(4000, 8000), shift: shift };
      m.mid = Math.max(30, Math.min(U - 30, m.mid + shift));
      regen(m);
    }
    function accrueLip(now, dt) {
      var L = S.laneById.M1;
      Object.keys(S.lip).forEach(function (s) {
        var m = S.markets.get(S.lip[s].key); if (!m || m.status !== 'OPEN') return;
        var share = 0;
        ['BUY', 'SELL'].forEach(function (side) {
          var lv = bookLevels(m, side).slice(0, 3); if (!lv.length) return;
          var edge = lv[lv.length - 1][0], tot = 0, ours = 0;
          lv.forEach(function (l) { tot += l[1]; });
          m.ours.forEach(function (o) { if (o.side === side && (side === 'BUY' ? o.px >= edge : o.px <= edge)) ours += o.left; });
          if (tot > 0) share += ours / Math.max(tot, 300) / 2;
        });
        var u = 200000 / 900 * dt * share;
        if (u > 0) { L.lipU += u; S.lipU += u; }
      });
    }

    // ------------------------------------------------------------------------------------------
    // Systematic: ceilings, lanes
    // ------------------------------------------------------------------------------------------
    // worst-case loss (>= 0) of a set of positions, outcome group by outcome group: a basket
    // that always has one paying leg risks its net cost, not the sum of its legs
    function groupLoss(pred, byVenue) {
      var groups = {}, outv = { K: 0, P: 0, all: 0 };
      S.pos.forEach(function (P) {
        if (!P.pos || !pred(P)) return; var m = S.markets.get(P.key); if (!m) return;
        var g = groups[m.og + '|' + (byVenue ? P.v : '')] || (groups[m.og + '|' + (byVenue ? P.v : '')] = { og: m.og, v: P.v, list: [] });
        g.list.push({ m: m, x: posMath(P, m) });
      });
      Object.keys(groups).forEach(function (k) {
        var g = groups[k], worst = null;
        (S.ogOutcomes[g.og] || ['YES']).forEach(function (oc) {
          var pl = 0; g.list.forEach(function (e) { pl += e.m.oc === oc ? e.x.ifYes : e.x.ifNo; });
          if (worst === null || pl < worst) worst = pl;
        });
        var loss = Math.max(0, -(worst || 0));
        outv[g.v] += loss; outv.all += loss;
      });
      return outv;
    }
    function ceilings() {
      var out2 = { K: { used: 0, ceiling: 0, eff: 0 }, P: { used: 0, ceiling: 0, eff: 0 } };
      var open = groupLoss(function () { return true; }, true), sysOpen = groupLoss(function (P) { return !!P.own.SYS; }, true);
      var openK = open.K, openP = open.P;
      out2.K.used = sysOpen.K + reserved(function (o) { return o.own === 'SYS' && o.v === 'K'; });
      out2.P.used = sysOpen.P + reserved(function (o) { return o.own === 'SYS' && o.v === 'P'; });
      var cashK = S.cash.S0 + S.cash.S1 + S.cash.S2 + S.cash.S3;
      var freeK = freeOf('S0') + freeOf('S1') + freeOf('S2') + freeOf('S3'), freeP = freeOf('PM');
      out2.K.ceiling = 0.7 * (cashK + openK); out2.P.ceiling = 0.7 * (S.cash.PM + openP);
      out2.K.eff = Math.min(out2.K.ceiling, out2.K.used + Math.max(0, freeK));
      out2.P.eff = S.pmRO ? 0 : Math.min(out2.P.ceiling, out2.P.used + Math.max(0, freeP));
      return out2;
    }
    function laneUse(L) {
      var used = reserved(function (o) { return o.lane === L.id; }), unrl = 0, wrk = 0;
      S.live.forEach(function (o) { if (o.lane === L.id) wrk++; });
      var mine = function (P) { return P.lane === L.id && !!P.own.SYS; };
      S.pos.forEach(function (P) { if (P.pos && mine(P)) unrl += posMath(P, S.markets.get(P.key)).unrl; });
      var open = groupLoss(mine, false).all;
      return { used: used + open, open: open, unrl: unrl, wrk: wrk };
    }
    function laneLive(key, lane) {
      var r = { BUY: null, SELL: null };
      S.live.forEach(function (o) { if (o.key === key && o.lane === lane && !r[o.side]) r[o.side] = o; });
      return r;
    }
    function laneSubmit(L, m, side, pxU, qty, tif, ttl, touch) {
      if (pxU === null || !(qty > 0)) return null;
      var res = addedReserve(m.key, side, pxU, qty, function (o) { return o.lane === L.id; });
      var u = laneUse(L), cl = ceilings();
      if (u.used + res > L.budgetU) { L.capHit = 'budget ' + (u.used / U).toFixed(2) + '/' + (L.budgetU / U).toFixed(2); return null; }
      if (cl[m.v].used + res > cl[m.v].eff) { L.capHit = 'ceiling ' + m.v + ' ' + (cl[m.v].used / U).toFixed(2) + '/' + (cl[m.v].eff / U).toFixed(2); return null; }
      if (addedReserve(m.key, side, pxU, qty) + Math.max(0, FEES.estimate(m.v, m.fee, side, pxU, qty, 'T')) > freeOf(m.shard || 'PM')) { L.capHit = 'shard ' + (m.shard || 'PM') + ' cash'; return null; }
      var req = { cid: 'S' + L.id + '-' + EPS + '-' + (++S.cidN).toString(36), v: m.v, tk: m.tk, side: side, px: N.pxStr(pxU),
                  qty: r2(qty), tif: tif, ttl_s: tif === 'POST' ? ttl : null, override: false, touch: touch === null || touch === undefined ? null : N.pxStr(touch), t_click_ms: null };
      return submit(req, 'SYS', L.id, null);
    }
    function manage(L, m, side, px, o, qty, ttl, now) {
      if (px === null) { if (o && o.st !== 'PEND') cancelOrder(o, 'PULL'); return; }
      if (!o) { laneSubmit(L, m, side, px, qty, 'POST', ttl, side === 'BUY' ? othersBest(m, 'bid') : othersBest(m, 'ask')); return; }
      if (o.st === 'PEND' || o.cxlPending) return;
      if (o.px !== px) {
        var away = Math.abs(G.ticks(m.g, o.px, px));
        if ((away >= 2 && now - o.t.recv > 500) || now - o.t.recv > 5000) { cancelOrder(o, 'PULL'); laneSubmit(L, m, side, px, qty, 'POST', ttl, null); }
        return;
      }
      if (o.exp_ms && o.exp_ms - now < 3000) amendTtl(o, ttl, now);
    }
    function amendTtl(o, ttl, now) {
      if (o.amendPending) return;
      o.amendPending = true;
      at(now + latency(o.v), function () {
        o.amendPending = false;
        if (o.st !== 'WORK' && o.st !== 'PART') return;
        var t = clock();
        o.exp_ms = Math.ceil((t + ttl * 1000) / 1000) * 1000; o.ttl_s = ttl; o.dirty = true;
        scheduleExpiry(o); flush(t);
      });
    }
    function takerPer(m, px) { return FEES.takerPer(m.v, m.fee, px); }

    var laneFns = {
      M1: function (L, now) {
        Object.keys(S.lip).forEach(function (s) {
          var m = S.markets.get(S.lip[s].key);
          if (!m || m.status !== 'OPEN' || m.manOwned) return;
          var tau = m.close_ms - now, age = now - m.open_ms, q = posOf(m.key), mine = laneLive(m.key, 'M1');
          var bbO = othersBest(m, 'bid'), baO = othersBest(m, 'ask');
          if (tau < 60000) {
            if (mine.BUY) cancelOrder(mine.BUY, 'PULL'); if (mine.SELL) cancelOrder(mine.SELL, 'PULL');
            if (Math.abs(q) > 10 && !m.m1Flat && bbO !== null && baO !== null) {
              m.m1Flat = true;
              if (q > 0) laneSubmit(L, m, 'SELL', G.step(m.g, bbO, -1), q, 'IOC', null, bbO);
              else laneSubmit(L, m, 'BUY', G.step(m.g, baO, 1), -q, 'IOC', null, baO);
            }
            return;
          }
          if (bbO === null || baO === null) { manage(L, m, 'BUY', null, mine.BUY, 10, 15, now); manage(L, m, 'SELL', null, mine.SELL, 10, 15, now); return; }
          var fair = m.ref.fair, ticks = G.ticks(m.g, bbO, baO);
          var bid = ticks >= 2 ? G.step(m.g, bbO, 1) : bbO, ask = ticks >= 2 ? G.step(m.g, baO, -1) : baO;
          if (ticks === 2) { if (q > 0) bid = bbO; else ask = baO; }
          var capB = G.snap(m.g, fair - 50 - 10 * q, -1), capA = G.snap(m.g, fair + 50 - 10 * q, 1);
          if (capB === null || bid > capB) bid = capB;
          if (capA === null || ask < capA) ask = capA;
          var quoting = age < 600000, reducing = tau < 180000;
          var wantB = bid !== null && bid < baO && q < 20 && (quoting || (reducing && q < 0));
          var wantA = ask !== null && ask > bbO && q > -20 && (quoting || (reducing && q > 0));
          if (!quoting && !reducing) { wantB = wantB && !!mine.BUY && mine.BUY.px === bid; wantA = wantA && !!mine.SELL && mine.SELL.px === ask; }
          manage(L, m, 'BUY', wantB ? bid : null, mine.BUY, 5, 15, now);
          manage(L, m, 'SELL', wantA ? ask : null, mine.SELL, 5, 15, now);
        });
      },
      M2: function (L, now) {
        var n = 0;
        S.markets.forEach(function (m) {
          if (m.v !== 'K' || (m.kind !== 'WEATHER' && m.kind !== 'GAME') || m.status !== 'OPEN' || m.manOwned) return;
          var mine = laneLive(m.key, 'M2'), bbO = othersBest(m, 'bid'), baO = othersBest(m, 'ask');
          var side = null, px = null;
          if (n < 8 && bbO !== null && bbO >= 9600 && bbO <= 9900 && (baO === null || baO > bbO)) {
            side = 'BUY'; px = bbO;
            if (baO !== null && G.ticks(m.g, bbO, baO) >= 2 && G.step(m.g, bbO, 1) <= 9900) px = G.step(m.g, bbO, 1);
          } else if (n < 8 && baO !== null && baO >= 100 && baO <= 400 && (bbO === null || bbO < baO)) {
            side = 'SELL'; px = baO;
            if (bbO !== null && G.ticks(m.g, bbO, baO) >= 2 && G.step(m.g, baO, -1) >= 100) px = G.step(m.g, baO, -1);
          }
          if (side && groupWorst(m.og) - RISK.one(side, px, 5, side === 'BUY' ? -posOf(m.key) : posOf(m.key)) < -150000) side = null;
          if (side) n++;
          manage(L, m, 'BUY', side === 'BUY' ? px : null, mine.BUY, 5, 60, now);
          manage(L, m, 'SELL', side === 'SELL' ? px : null, mine.SELL, 5, 60, now);
        });
      },
      S1: function (L, now) {
        S.wx.forEach(function (w) {
          var ms_ = w.keys.map(function (k) { return S.markets.get(k); });
          if (ms_.some(function (m) { return !m || m.status !== 'OPEN' || m.manOwned; }) || now < ms_[0].s1Cool) return;
          var asks = ms_.map(function (m) { return othersBest(m, 'ask'); }), bids = ms_.map(function (m) { return othersBest(m, 'bid'); });
          var q;
          if (asks.every(function (a) { return a !== null; })) {
            var cost = 0; asks.forEach(function (a, i) { cost += a + takerPer(ms_[i], a); });
            if (U - cost >= L.thrU) {
              q = Math.min(3, Math.min.apply(null, ms_.map(function (m, i) { return m.asks.get(asks[i]); })));
              q = Math.max(1, Math.floor(q));
              ms_.forEach(function (m, i) { laneSubmit(L, m, 'BUY', asks[i], q, 'IOC', null, asks[i]); });
              ms_[0].s1Cool = now + 5000;
              alert('INFO', 'S1', 'basket ' + ms_[0].grp + ' BUY ' + ms_.length + ' legs x' + q + ' edge ' + Desk.fmt.cents(U - cost) + 'c');
              return;
            }
          }
          if (bids.every(function (b) { return b !== null; })) {
            var rev = 0; bids.forEach(function (b, i) { rev += b - takerPer(ms_[i], b); });
            if (rev - U >= L.thrU) {
              q = Math.max(1, Math.floor(Math.min(3, Math.min.apply(null, ms_.map(function (m, i) { return m.bids.get(bids[i]); })))));
              ms_.forEach(function (m, i) { laneSubmit(L, m, 'SELL', bids[i], q, 'IOC', null, bids[i]); });
              ms_[0].s1Cool = now + 5000;
              alert('INFO', 'S1', 'basket ' + ms_[0].grp + ' SELL ' + ms_.length + ' legs x' + q + ' edge ' + Desk.fmt.cents(rev - U) + 'c');
            }
          }
        });
        S.games.forEach(function (g) {
          var a = g.keys.K_A && S.markets.get(g.keys.K_A), h = g.keys.K_H && S.markets.get(g.keys.K_H), p = g.keys.P && S.markets.get(g.keys.P);
          if (a && h && a.status === 'OPEN' && h.status === 'OPEN' && now >= a.s1Cool && !a.manOwned && !h.manOwned) {
            var aa = othersBest(a, 'ask'), ha = othersBest(h, 'ask');
            if (aa !== null && ha !== null && U - (aa + ha + takerPer(a, aa) + takerPer(h, ha)) >= L.thrU) {
              laneSubmit(L, a, 'BUY', aa, 2, 'IOC', null, aa); laneSubmit(L, h, 'BUY', ha, 2, 'IOC', null, ha);
              a.s1Cool = now + 5000; alert('INFO', 'S1', 'pair ' + a.short.split(' ')[0] + ' BUY both');
            }
          }
          if (a && p && a.status === 'OPEN' && p.status === 'OPEN' && now >= p.s1Cool && !a.manOwned && !p.manOwned && !S.pmRO) {
            var ka = othersBest(a, 'ask'), kb = othersBest(a, 'bid'), pa = othersBest(p, 'ask'), pb = othersBest(p, 'bid');
            if (ka !== null && pb !== null && pb - ka - takerPer(a, ka) - takerPer(p, pb) >= L.thrU) {
              laneSubmit(L, a, 'BUY', ka, 2, 'IOC', null, ka); laneSubmit(L, p, 'SELL', pb, 2, 'IOC', null, pb);
              p.s1Cool = now + 5000; alert('INFO', 'S1', 'cross ' + a.short.split(' ')[0] + ' K BUY ' + N.pxStr(ka) + ' / P SELL ' + N.pxStr(pb));
            } else if (kb !== null && pa !== null && kb - pa - takerPer(a, kb) - takerPer(p, pa) >= L.thrU) {
              laneSubmit(L, a, 'SELL', kb, 2, 'IOC', null, kb); laneSubmit(L, p, 'BUY', pa, 2, 'IOC', null, pa);
              p.s1Cool = now + 5000; alert('INFO', 'S1', 'cross ' + a.short.split(' ')[0] + ' P BUY ' + N.pxStr(pa) + ' / K SELL ' + N.pxStr(kb));
            }
          }
        });
      },
      C1: function (L, now) {
        S.games.forEach(function (g) {
          if (g.per < 1 || g.per > 5 || g.staleUntil <= now) return;
          [g.keys.K_A, g.keys.K_H, g.keys.P].forEach(function (k) {
            var m = k && S.markets.get(k);
            if (!m || m.status !== 'OPEN' || m.manOwned || m.c1Shot === g.scoreN) return;
            if (m.v === 'P' && S.pmRO) return;
            var ba = othersBest(m, 'ask'), bb = othersBest(m, 'bid');
            if (ba !== null && m.pU - ba - takerPer(m, ba) >= L.thrU) {
              var px = G.step(m.g, ba, 1); m.c1Shot = g.scoreN;
              laneSubmit(L, m, 'BUY', px, Math.max(1, Math.floor(R(2, 5) * U / px)), 'IOC', null, ba);
            } else if (bb !== null && bb - m.pU - takerPer(m, bb) >= L.thrU) {
              var px2 = G.step(m.g, bb, -1); m.c1Shot = g.scoreN;
              laneSubmit(L, m, 'SELL', px2, Math.max(1, Math.floor(R(2, 5) * U / (U - px2))), 'IOC', null, bb);
            }
          });
        });
      },
      X1: function (L, now) {
        S.games.forEach(function (g) {
          if (g.per < 1 || g.per > 5) return;
          [g.keys.K_A, g.keys.K_H].forEach(function (k) {
            var m = k && S.markets.get(k);
            if (!m || m.status !== 'OPEN' || m.manOwned) return;
            var mine = laneLive(m.key, 'X1'), d = m.ref.espn === null ? 0 : m.ref.espn - m.mid;
            var bbO = othersBest(m, 'bid'), baO = othersBest(m, 'ask');
            manage(L, m, 'SELL', d >= 800 && baO !== null ? baO : null, mine.SELL, 2, 15, now);
            manage(L, m, 'BUY', d <= -800 && bbO !== null ? bbO : null, mine.BUY, 2, 15, now);
          });
        });
      }
    };
    function groupWorst(og) {
      var outs = S.ogOutcomes[og] || ['YES'], worst = null;
      outs.forEach(function (x) {
        var pl = 0;
        S.pos.forEach(function (P) { if (!P.pos) return; var m = S.markets.get(P.key); if (!m || m.og !== og) return; var pm = posMath(P, m); pl += m.oc === x ? pm.ifYes : pm.ifNo; });
        if (worst === null || pl < worst) worst = pl;
      });
      return worst || 0;
    }
    function laneState(L, now) {
      var u = laneUse(L), st = 'ON', why = '';
      if (!L.on) { st = 'OFF'; why = ''; }
      else if (S.run.state !== 'RUN') { st = 'OFF'; why = S.run.state; }
      else if (L.id === 'X1' && (L.fills >= 100 || L.realU + u.unrl <= -200000)) { st = 'BLOCKED'; why = 'retired: 100 trades or 20.00 lost'; }
      else if (L.venue === 'P' && S.pmRO) { st = 'BLOCKED'; why = 'PM RO'; }
      else if (!S.feeds.KX.up || !S.feeds.KXP.up) { st = 'BLOCKED'; why = 'KX feed down'; }
      else if (L.style === 'IOC' && S.costU >= S.costBudgetU) { st = 'CAPPED'; why = 'daily cost budget'; }
      else if (L.capHit) { st = 'CAPPED'; why = L.capHit; }
      else if (L.budgetU - u.used < 2000) { st = 'CAPPED'; why = 'budget ' + (u.used / U).toFixed(2) + '/' + (L.budgetU / U).toFixed(2); }
      return { st: st, why: why, u: u };
    }
    function runLanes(now) {
      S.lanes.forEach(function (L) {
        var s = laneState(L, now);
        if (s.st !== L.state && (s.st === 'BLOCKED' || L.state === 'BLOCKED') && S.run.state === 'RUN' && L.on) alert(s.st === 'BLOCKED' ? 'WARN' : 'INFO', L.id, L.state + ' -> ' + s.st + (s.why ? ' ' + s.why : ''));
        L.state = s.st; L.why = s.why; L.capHit = false;
        if (s.st === 'ON' || s.st === 'CAPPED') { try { laneFns[L.id](L, now); } catch (e) { alert('CRIT', L.id, 'lane error: ' + (e && e.message)); } }
      });
      // daily stop
      var day = sysDay();
      if (S.run.state === 'RUN' && day <= -S.stopLimitU) {
        S.run = { state: 'HALTED', since_ms: now, rearm_ms: null, why: 'daily stop' };
        cancelWhere(function (o) { return o.own === 'SYS'; }, 'HALT');
        alert('CRIT', 'SYS', 'daily stop hit ' + Desk.fmt.pnl(day) + ': systematic halted');
      }
    }
    function laneData(L) {
      var s = laneState(L, clock());
      var cl = ceilings(), effLeft = L.venue === 'P' ? cl.P.eff - cl.P.used : cl.K.eff - cl.K.used;
      var day = L.realU + s.u.unrl + L.lipU;
      return {
        id: L.id, name: L.name, venue: L.venue, state: L.state === undefined ? s.st : L.state, on: L.on, style: L.style,
        budget: ms(L.budgetU), used: ms(s.u.used), open: ms(s.u.open), wrk: s.u.wrk, sent: L.sent, hit: L.hit, fills: L.fills,
        ctrs: L.ctrs, fees: ms(L.feesU), real: ms(L.realU), unrl: ms(s.u.unrl), day: ms(day), lip: ms(L.lipU),
        cap_left: ms(Math.max(0, Math.min(L.budgetU - s.u.used, effLeft))), thr: L.thrU === null ? null : ms(L.thrU),
        last_ms: L.last_ms, why: L.why
      };
    }
    function sysDay() {
      var unrl = 0;
      S.pos.forEach(function (P) { if (P.pos && P.own.SYS && !P.own.MAN) unrl += posMath(P, S.markets.get(P.key)).unrl; });
      return S.realBy.SYS + unrl + S.lipU;
    }
    function cancelWhere(pred, why) {
      var cids = [];
      S.live.forEach(function (o) { if (pred(o) && cancelOrder(o, why)) cids.push(o.cid); });
      return cids;
    }

    // ------------------------------------------------------------------------------------------
    // header, risk, feeds
    // ------------------------------------------------------------------------------------------
    function preflight() {
      var freeK = freeOf('S0') + freeOf('S1') + freeOf('S2') + freeOf('S3');
      var low = ['S0', 'S1', 'S2', 'S3'].filter(function (s) { return freeOf(s) < 10000; });
      return [
        { id: 'KEY', ok: true, why: '' },
        { id: 'CASH', ok: freeK >= 50000, why: freeK >= 50000 ? '' : 'K free ' + (freeK / U).toFixed(2) },
        { id: 'SHARD', ok: !low.length, why: low.length ? low.join(',') + ' free under 1.00' : '' },
        { id: 'CLK', ok: Math.abs(S.lat.clk) < 500, why: '' },
        { id: 'DISK', ok: true, why: '' },
        { id: 'FEED', ok: S.feeds.KX.up && !S.feeds.KX.gap, why: !S.feeds.KX.up ? 'KX down' : (S.feeds.KX.gap ? 'KX gap' : '') },
        { id: 'PRIV', ok: S.feeds.KXP.up && !S.feeds.KXP.gap, why: !S.feeds.KXP.up ? 'KXP down' : (S.feeds.KXP.gap ? 'KXP gap' : '') },
        { id: 'GROUPS', ok: S.groups.ok, why: S.groups.why }
      ];
    }
    function hdrData() {
      var cl = ceilings(), unrlMan = 0, unrlSys = 0, risk = groupLoss(function () { return true; }, false).all + reserved(null);
      S.pos.forEach(function (P) {
        if (!P.pos) return; var x = posMath(P, S.markets.get(P.key));
        if (P.own.SYS && !P.own.MAN) unrlSys += x.unrl; else unrlMan += x.unrl;
      });
      var sys = S.realBy.SYS + unrlSys + S.lipU, man = S.realBy.MAN + unrlMan;
      var left = S.stopLimitU + Math.min(0, sys);
      var shards = ['S0', 'S1', 'S2', 'S3'].map(function (s) { return { id: s, cash: ms(S.cash[s]), free: ms(freeOf(s)) }; });
      var cashK = S.cash.S0 + S.cash.S1 + S.cash.S2 + S.cash.S3;
      var gw = S.gwAcks.length ? S.gwAcks.slice().sort(function (a, b) { return a - b; })[Math.floor(S.gwAcks.length / 2)] : null;
      return {
        mode: S.mode,
        cash: { K: { total: ms(cashK), free: ms(freeOf('S0') + freeOf('S1') + freeOf('S2') + freeOf('S3')), ro: false, shards: shards },
                P: { total: ms(S.cash.PM), free: ms(freeOf('PM')), ro: S.pmRO } },
        day: { total: ms(sys + man), sys: ms(sys), man: ms(man), lip: ms(S.lipU) },
        risk: ms(risk),
        stop: { left: ms(Math.max(0, left)), limit: ms(S.stopLimitU), hit: left <= 0 },
        sys: { K: { used: ms(cl.K.used), ceiling: ms(cl.K.ceiling), eff: ms(cl.K.eff) }, P: { used: ms(cl.P.used), ceiling: ms(cl.P.ceiling), eff: ms(cl.P.eff) } },
        cost: { used: ms(S.costU), budget: ms(S.costBudgetU) },
        run: { state: S.run.state, since_ms: S.run.since_ms, rearm_ms: S.run.rearm_ms, why: S.run.why },
        preflight: preflight(),
        lat: { kx_lag_ms: S.feeds.KX.up ? S.lat.kx : null, pm_lag_ms: S.feeds.PM.up && !S.pmRO ? S.lat.pm : null,
               gw_ack_ms: gw === null ? null : Math.round(gw), clk_off_ms: S.lat.clk }
      };
    }
    function riskData() {
      var groups = {};
      S.pos.forEach(function (P) {
        if (!P.pos) return; var m = S.markets.get(P.key); if (!m) return;
        (groups[m.og] = groups[m.og] || { id: m.og, name: m.grp + ' ' + m.og.split('-').slice(1).join('-'), members: [] }).members.push({ v: P.v, tk: P.tk });
        if (m.kind === 'LIP15') groups[m.og].name = m.short.replace(/ UP$/, '');
        else if (m.kind === 'WEATHER') groups[m.og].name = m.grp;
        else groups[m.og].name = m.grp + ' ' + m.short.split(' ')[0];
      });
      var list = Object.keys(groups).map(function (og) {
        var g = groups[og], outs = S.ogOutcomes[og] || ['YES'], worst = null, best = null;
        outs.forEach(function (x) {
          var pl = 0;
          g.members.forEach(function (mr) { var k = mr.v + '|' + mr.tk, m = S.markets.get(k), pm = posMath(S.pos.get(k), m); pl += m.oc === x ? pm.ifYes : pm.ifNo; });
          if (worst === null || pl < worst) worst = pl; if (best === null || pl > best) best = pl;
        });
        return { id: g.id, name: g.name, worst: ms(worst || 0), best: ms(best || 0), members: g.members };
      }).sort(function (a, b) { return N.money(a.worst) - N.money(b.worst); });
      var shards = ['S0', 'S1', 'S2', 'S3'].map(function (s) { return { v: 'K', id: s, cash: ms(S.cash[s]), free: ms(freeOf(s)) }; });
      shards.push({ v: 'P', id: 'PM', cash: ms(S.cash.PM), free: ms(freeOf('PM')) });
      return { groups: list, shards: shards };
    }
    function feedsData() {
      return { KX: { up: S.feeds.KX.up, gap: S.feeds.KX.gap, lag_ms: S.feeds.KX.up ? S.lat.kx : null },
               KXP: { up: S.feeds.KXP.up, gap: S.feeds.KXP.gap, lag_ms: S.feeds.KXP.up ? Math.round(S.lat.kx * 0.7) : null },
               PM: { up: S.feeds.PM.up, gap: S.feeds.PM.gap, lag_ms: S.feeds.PM.up ? S.lat.pm : null } };
    }
    function refData(m) {
      var r = m.ref;
      return { fair: r.fair === null ? null : pxs(r.fair), sd: r.sd === null ? null : pxs(r.sd), espn: r.espn === null ? null : pxs(r.espn),
               espn_ts_ms: r.espn_ts_ms, game: r.game ? { per: r.game.per, clock: r.game.clock, score: r.game.score, src: r.game.src } : null };
    }
    function bookData(m, depth, withPrints) {
      var lv = function (a) { return a.map(function (x) { return [N.pxStr(x[0]), r2(x[1])]; }); };
      var bids = bookLevels(m, 'BUY'), asks = bookLevels(m, 'SELL');
      if (depth === 'TOP') { bids = bids.slice(0, 1); asks = asks.slice(0, 1); }
      var pr = function (t) { return { px: N.pxStr(t.px), qty: t.qty, side: t.side, ts_ms: t.ts_ms }; };
      return { v: m.v, tk: m.tk, depth: depth, bids: lv(bids), asks: lv(asks), trades: withPrints ? m.prints.map(pr) : [],
               last: m.last ? pr(m.last) : null, vol: r2(m.vol), ref: refData(m), ex_ms: Math.round(m.exMs) };
    }
    function depthOf(m) {
      for (var i = 0; i < S.subs.ladders.length; i++) if (S.subs.ladders[i].v === m.v && S.subs.ladders[i].tk === m.tk) return 'FULL';
      if (S.subs.tops === 'ALL') return m.status === 'OPEN' ? 'TOP' : null;
      for (var j = 0; j < S.subs.tops.length; j++) if (S.subs.tops[j].v === m.v && S.subs.tops[j].tk === m.tk) return 'TOP';
      return null;
    }

    // ------------------------------------------------------------------------------------------
    // flushing state to the stream
    // ------------------------------------------------------------------------------------------
    function flush(now) {
      var fresh = [];
      S.orders.forEach(function (o) { if (o.dirty) { o.dirty = false; emit('ord', ordData(o)); o.fills.forEach(function (f) { if (!f.emitted) fresh.push(f); }); } });
      fresh.sort(function (a, b) { return a.n - b.n; });
      fresh.forEach(function (f) { f.emitted = true; emit('fill', fillData(f)); });
      S.pos.forEach(function (P) { if (P.dirty) { P.dirty = false; var d = posData(P); P.lastJson = posSig(d); emit('pos', d); } });
      void now;
    }
    function emitPeriodic(now) {
      // books, coalesced to 20/s per ticker; TOP only when the top or the prints changed
      S.markets.forEach(function (m) {
        if (m.v === 'K' && !S.feeds.KX.up) return;
        if (m.v === 'P' && !S.feeds.PM.up) return;
        var depth = depthOf(m); if (!depth) return;
        var force = S.forceBook.has(m.key);
        if (!force && (!m.bookDirty && !m.prints.length)) return;
        if (!force && now - m.lastBookEmit < 50) return;
        if (depth === 'TOP' && !force) {
          var b = bestLevel(m, 'BUY'), a = bestLevel(m, 'SELL');
          var sig = b + ':' + (b === null ? '' : bookLevels(m, 'BUY')[0][1]) + '/' + a + ':' + (a === null ? '' : bookLevels(m, 'SELL')[0][1]) + '/' + (m.ref.game ? m.ref.game.clock + m.ref.game.score : '') + '/' + Math.round((m.ref.fair || 0) / 50) + '/' + Math.round((m.ref.espn || 0) / 50);
          if (sig === m.topSig && !m.prints.length) { m.bookDirty = false; return; }
          m.topSig = sig;
        }
        S.forceBook.delete(m.key);
        emit('book', bookData(m, depth, true));
        m.prints = []; m.bookDirty = false; m.lastBookEmit = now;
      });
      if (now >= S.nextMark) {
        S.nextMark = now + 1000;
        S.lat.kx = Math.round(Math.max(18, Math.min(60, S.lat.kx + gauss() * 3)));
        S.lat.pm = Math.round(Math.max(120, Math.min(320, S.lat.pm + gauss() * 10)));
        S.lat.clk = Math.round(Math.max(40, Math.min(120, S.lat.clk + gauss() * 2)));
        S.pos.forEach(function (P) { var d = posData(P), j = posSig(d); if (j !== P.lastJson) { P.lastJson = j; emit('pos', d); } });
        S.lanes.forEach(function (L) { var d = laneData(L), j = JSON.stringify(d); if (j !== L.lastJson) { L.lastJson = j; L.lastEmit = now; emit('lane', d); } });
      }
      if (now >= S.nextRisk) {
        S.nextRisk = now + 1000;
        var r = riskData(), rj = JSON.stringify(r);
        if (rj !== S.lastRiskJson) { S.lastRiskJson = rj; emit('risk', r); }
      }
      if (now - S.lastHdrEmit >= 250) {
        var h = hdrData(), hj = JSON.stringify(h);
        if (hj !== S.lastHdrJson) { S.lastHdrJson = hj; S.lastHdrEmit = now; emit('hdr', h); }
      }
    }
    function tick(now) {
      var dt = Math.min(Math.max(0, now - S.lastTick), 5000) / 1000;
      S.lastTick = now;
      if (now >= S.nextHb) {
        S.nextHb = now + 1000 - ((now - S.nextHb) % 1000);
        emit('hb', { epoch: S.epoch, sid: S.sid, srv_ms: now, feeds: feedsData() });
      }
      if (dt > 0) {
        stepWorld(now, dt);
        dislocate(now);
        accrueLip(now, dt);
        if (S.run.state === 'RUN' && now >= S.nextLane) { S.nextLane = now + 500; runLanes(now); }
        else if (now >= S.nextLane) { S.nextLane = now + 500; S.lanes.forEach(function (L) { var s = laneState(L, now); L.state = s.st; L.why = s.why; }); }
        rolls(now);
        if (S.run.state === 'KILLED' && S.run.rearm_ms && now >= S.run.rearm_ms && !S.run.rearmNoted) { S.run.rearmNoted = true; alert('INFO', 'KILL', 're-arm available'); }
        if (S.chaos && now >= S.nextChaos) { S.nextChaos = now + R(120000, 240000); S.drop = true; }
      }
      flush(now);
      emitPeriodic(now);
    }

    // ------------------------------------------------------------------------------------------
    // endpoints
    // ------------------------------------------------------------------------------------------
    function snapshot() {
      var now = clock();
      var books = [];
      S.markets.forEach(function (m) { var d = depthOf(m); if (d) books.push(bookData(m, d, false)); });
      var orders = [];
      S.orders.forEach(function (o) { if (!terminal(o.st) || now - (o.t.done || 0) < 120000) orders.push(ordData(o)); });
      var mkts = [];
      S.markets.forEach(function (m) { mkts.push(mktData(m)); });
      var pos = []; S.pos.forEach(function (P) { pos.push(posData(P)); });
      // the newest 5000 fills (the page keeps no more) and only their markouts
      var fl = S.fills.filter(function (f) { return f.emitted; }).slice(-SNAP_FILLS), fids = new Set();
      fl.forEach(function (f) { fids.add(f.fid); });
      return {
        epoch: S.epoch, sid: S.sid, seq: S.seq, srv_ms: now, hdr: hdrData(), markets: mkts, books: books, orders: orders,
        fills: fl.map(fillData), mos: S.mos.filter(function (x) { return fids.has(x.fid); }), pos: pos,
        lanes: S.lanes.map(laneData), risk: riskData(), alerts: S.alerts.slice(-200), feeds: feedsData(),
        subs: { ladders: S.subs.ladders.slice(), tops: S.subs.tops === 'ALL' ? 'ALL' : S.subs.tops.slice() }
      };
    }
    function sidOf(path) { var m = /[?&]sid=([^&]*)/.exec(String(path)); return m ? decodeURIComponent(m[1]) : ''; }
    function runResp(n) { return { ok: true, run: { state: S.run.state, since_ms: S.run.since_ms, rearm_ms: S.run.rearm_ms, why: S.run.why }, n: n || 0 }; }
    function request(method, path, body, reply, hdrs) {
      reply = reply || function () {};
      var key = C.endpointFor(method, path), now = clock();
      // the web worker's guards come first: Host, then the POST token
      if (hdrs && hdrs.Host !== undefined && !C.hostOk(hdrs.Host, S.port)) { reply2(reply, 403, { ok: false, err: err('FORBIDDEN', 'Host ' + hdrs.Host) }); return; }
      if (method === 'POST' && (!hdrs || hdrs['X-Desk'] !== S.token)) { reply2(reply, 403, { ok: false, err: err('FORBIDDEN', hdrs && hdrs['X-Desk'] ? 'stale X-Desk token' : 'no X-Desk token') }); return; }
      if (!key) { reply2(reply, 404, { ok: false, err: err('NOT_FOUND', method + ' ' + path) }); return; }
      if (method === 'POST') {
        var bad = C.validateRequest(key, body);
        if (bad.length) { reply2(reply, 400, { ok: false, err: err('BAD_REQUEST', bad[0]) }); return; }
      }
      var L, n;
      switch (key) {
        case 'GET /api/snapshot':
          if (sidOf(path) !== S.sid) { reply2(reply, 409, { ok: false, err: err('BAD_STATE', 'no open stream for sid ' + sidOf(path)) }); return; }
          reply2(reply, 200, snapshot(), 3); return;
        case 'GET /api/health': reply2(reply, 200, { ok: true, epoch: S.epoch, srv_ms: now }, 1); return;
        case 'POST /api/subscribe':
          if (body.sid !== S.sid) { reply2(reply, 409, { ok: false, err: err('BAD_STATE', 'no open stream for sid ' + body.sid) }); return; }
          var before = {}; S.markets.forEach(function (m) { before[m.key] = depthOf(m); });
          S.subs = { ladders: body.ladders.slice(0, 4), tops: body.tops === 'ALL' ? 'ALL' : body.tops.slice() };
          S.markets.forEach(function (m) { var d = depthOf(m); if (d && d !== before[m.key]) S.forceBook.add(m.key); });
          reply2(reply, 200, { ok: true, subs: { ladders: S.subs.ladders.slice(), tops: S.subs.tops === 'ALL' ? 'ALL' : S.subs.tops.slice() } });
          return;
        case 'POST /api/order': submit(body, 'MAN', null, reply); return;
        case 'POST /api/cancel':
          var cids;
          if (body.cids) cids = cancelWhere(function (o) { return body.cids.indexOf(o.cid) >= 0; }, 'USER');
          else {
            var k = body.v + '|' + body.tk;
            if (!S.markets.has(k)) { reply2(reply, 404, { ok: false, err: err('NOT_FOUND', k) }); return; }
            var pxU = body.px === null ? null : N.px(body.px);
            cids = cancelWhere(function (o) {
              return o.key === k && (body.own === 'ANY' || o.own === body.own) && (!body.side || o.side === body.side) && (pxU === null || o.px === pxU);
            }, 'USER');
          }
          reply2(reply, 200, { ok: true, n: cids.length, cids: cids }, latency('K'));
          return;
        case 'POST /api/cancel_all':
          var c2 = cancelWhere(function (o) { return (body.own === 'ANY' || o.own === body.own) && (!body.v || o.v === body.v); }, 'USER');
          reply2(reply, 200, { ok: true, n: c2.length, cids: c2 }, latency('K'));
          return;
        case 'POST /api/amend': amend(body, reply); return;
        case 'POST /api/lane/{id}':
          L = S.laneById[path.split('/').pop()];
          if (!L) { reply2(reply, 404, { ok: false, err: err('NOT_FOUND', path) }); return; }
          // the strategy worker applies it a few ms later; the echo and the reply follow
          at(now + Math.round(R(8, 20)), function () {
            var now2 = clock();
            L.on = body.on;
            if (!L.on) cancelWhere(function (o) { return o.lane === L.id; }, 'LANE_OFF');
            var s = laneState(L, now2); L.state = s.st; L.why = s.why;
            var ld = laneData(L); L.lastJson = JSON.stringify(ld); emit('lane', ld);
            alert('INFO', L.id, L.on ? 'lane on' : 'lane off');
            reply(200, { ok: true, lane: ld });
          });
          return;
        case 'POST /api/start':
          if (S.run.state === 'KILLED') { reply2(reply, 409, { ok: false, err: err('KILLED', 'kill lockout; re-arm first') }); return; }
          if (S.run.state === 'HALTED') { reply2(reply, 409, { ok: false, err: err('HALTED', 'daily stop hit') }); return; }
          var pf = preflight(), failing = pf.filter(function (r) { return !r.ok && r.id !== 'DISK'; });
          if (failing.length) { reply2(reply, 409, { ok: false, err: err('PREFLIGHT', failing.map(function (r) { return r.id; }).join(',')), preflight: pf }); return; }
          if (S.run.state !== 'RUN') { S.run = { state: 'RUN', since_ms: now, rearm_ms: null, why: '' }; S.nextLane = now; alert('INFO', 'SYS', 'start'); }
          reply2(reply, 200, runResp(0), 3);
          return;
        case 'POST /api/stop':
          n = cancelWhere(function (o) { return o.own === 'SYS'; }, 'STOP').length;
          if (S.run.state === 'RUN') { S.run = { state: 'STOP', since_ms: now, rearm_ms: null, why: 'operator' }; alert('INFO', 'SYS', 'stop: ' + n + ' orders cancelled'); }
          reply2(reply, 200, runResp(n), 3);
          return;
        case 'POST /api/kill':
          n = cancelWhere(function () { return true; }, 'KILL').length;
          if (S.run.state !== 'KILLED') { S.run = { state: 'KILLED', since_ms: now, rearm_ms: now + 60000, why: 'operator' }; alert('CRIT', 'KILL', 'kill: ' + n + ' orders cancelled, systematic stopped, re-arm in 60 s'); }
          S.groups = { ok: false, why: 'triggered by KILL' };
          reply2(reply, 200, runResp(n), 3);
          return;
        case 'POST /api/rearm':
          if (S.run.state !== 'KILLED' || now < S.run.rearm_ms) { reply2(reply, 409, { ok: false, err: err('BAD_STATE', S.run.state !== 'KILLED' ? 'not killed' : 're-arm in ' + Math.ceil((S.run.rearm_ms - now) / 1000) + ' s') }); return; }
          S.run = { state: 'STOP', since_ms: now, rearm_ms: null, why: '' };
          alert('INFO', 'KILL', 're-armed');
          // the order groups reset; GROUPS passes once the exchange confirms (START waits for it)
          S.groups = { ok: false, why: 'reset pending' };
          at(now + Math.round(R(600, 1000)), function () { if (S.run.state !== 'KILLED') { S.groups = { ok: true, why: '' }; alert('INFO', 'GW', 'order groups reset'); } });
          reply2(reply, 200, runResp(0), 3);
          return;
        case 'POST /api/release':
          var mm = S.markets.get(body.v + '|' + body.tk);
          if (!mm) { reply2(reply, 404, { ok: false, err: err('UNKNOWN_MARKET', body.v + '|' + body.tk) }); return; }
          if (mm.manOwned) { mm.manOwned = false; emit('mkt', mktData(mm)); }
          reply2(reply, 200, { ok: true });
          return;
        case 'POST /api/transfer':
          var amt = N.money(body.amt), sh = ['S0', 'S1', 'S2', 'S3'];
          if (sh.indexOf(body.from) < 0 || sh.indexOf(body.to) < 0 || body.from === body.to || !(amt > 0)) { reply2(reply, 400, { ok: false, err: err('BAD_REQUEST', 'from/to must be two Kalshi shards, amt > 0') }); return; }
          if (amt > freeOf(body.from)) { reply2(reply, 409, { ok: false, err: err('NO_CASH', body.from + ' free ' + N.moneyStr(freeOf(body.from))) }); return; }
          at(now + latency('K'), function () {
            if (amt > freeOf(body.from)) { reply(409, { ok: false, err: err('NO_CASH', body.from + ' free ' + N.moneyStr(freeOf(body.from))) }); return; }
            S.cash[body.from] -= amt; S.cash[body.to] += amt;
            alert('INFO', 'GW', 'moved ' + N.moneyStr(amt).replace(/0+$/, '').replace(/\.$/, '') + ' ' + body.from + '>' + body.to);
            reply(200, { ok: true, shards: [body.from, body.to].map(function (x) { return { id: x, cash: ms(S.cash[x]), free: ms(freeOf(x)) }; }) });
          });
          return;
        default:
          reply2(reply, 404, { ok: false, err: err('NOT_FOUND', key) });
      }
    }
    function amend(body, reply) {
      var o = S.orders.get(body.cid);
      if (!o) { reply2(reply, 404, { ok: false, err: err('NOT_FOUND', body.cid) }); return; }
      if (o.st !== 'WORK' && o.st !== 'PART') { reply2(reply, 409, { ok: false, err: err('BAD_STATE', body.cid + ' is ' + o.st), ord: ordData(o) }); return; }
      var m = S.markets.get(o.key);
      if (body.px !== null && !G.onGrid(m.g, N.px(body.px))) { reply2(reply, 400, { ok: false, err: err('OFF_GRID', body.px), ord: ordData(o) }); return; }
      if (body.qty !== null && (!(body.qty > o.filled) || (m.v === 'P' && body.qty % 1 !== 0))) { reply2(reply, 400, { ok: false, err: err('BAD_QTY', 'qty ' + body.qty), ord: ordData(o) }); return; }
      if (S.run.state === 'KILLED') { reply2(reply, 409, { ok: false, err: err('KILLED', 'kill lockout'), ord: ordData(o) }); return; }
      if (m.v === 'K' ? (!S.feeds.KXP.up || !S.feeds.KX.up || S.feeds.KX.gap) : !S.feeds.PM.up) { reply2(reply, 409, { ok: false, err: err('STALE', 'feed down'), ord: ordData(o) }); return; }
      at(clock() + latency(o.v), function () {
        var now = clock();
        if (o.st !== 'WORK' && o.st !== 'PART') { reply(409, { ok: false, err: err('BAD_STATE', o.cid + ' is ' + o.st), ord: ordData(o) }); return; }
        var lose = false;
        if (body.px !== null && N.px(body.px) !== o.px) { o.px = N.px(body.px); lose = true; }
        if (body.qty !== null) { var nq = r2(body.qty); if (nq > o.qty) lose = true; o.qty = nq; o.left = r2(nq - o.filled); }
        if (body.ttl_s !== null) { o.ttl_s = body.ttl_s; o.exp_ms = Math.ceil((now + body.ttl_s * 1000) / 1000) * 1000; scheduleExpiry(o); }
        if (lose) {
          if (crosses(m, o)) finish(o, 'CXL', 'POST_CROSS', now);
          else { o.restAt = now; o.ahead = (o.side === 'BUY' ? m.bids : m.asks).get(o.px) || 0; }
        }
        m.bookDirty = true; o.dirty = true; flush(now);
        reply(200, { ok: true, ord: ordData(o) });
      });
    }

    // ------------------------------------------------------------------------------------------
    // driving
    // ------------------------------------------------------------------------------------------
    function pump() {
      var now = clock(), guard = 0;
      while (S.timers.length && S.timers[0].due <= now && guard++ < 10000) { var t = S.timers.shift(); t.fn(); }
      if (now >= S.nextTick) { S.nextTick = now + 100 - ((now - S.nextTick) % 100); tick(now); }
    }
    function nextDue() { return Math.min(S.timers.length ? S.timers[0].due : Infinity, S.nextTick); }
    function loop() {
      if (!running) return;
      loopTimer = null; wakeAt = Infinity;
      pump();
      var wait = Math.max(1, Math.min(250, nextDue() - clock()));
      wakeAt = clock() + wait;
      loopTimer = setTimeout(loop, wait);
    }

    // an order on the account that the desk did not place (the venue's website): rests with no expiry
    function external(key, side, pxU, qty) {
      var m = S.markets.get(key); if (!m) return null;
      var cid = 'X' + (++S.oidN).toString(36) + EPS + 'ext';
      var o = submit({ cid: cid, v: m.v, tk: m.tk, side: side, px: N.pxStr(pxU), qty: qty, tif: 'POST', ttl_s: null, exp_ms: null,
                       override: true, touch: null, t_click_ms: null }, 'EXT', null, null);
      return o ? o.cid : null;
    }
    // demo: one order left resting from the venue's website (a tomorrow weather bracket, 3 ticks
    // under the bid), so the screen shows how a foreign order looks
    if (opts.ext) at(T0 + 2000, function () {
      var w = S.wx.filter(function (x) { return !x.today; })[0], m = w && S.markets.get(w.keys[2]);
      var bb = m ? bestLevel(m, 'BUY') : null;
      if (m && bb !== null) external(m.key, 'BUY', Math.max(100, G.step(m.g, bb, -3)), 10);
    });

    var api = {
      state: S,
      token: S.token,
      external: external,
      request: request,
      snapshot: snapshot,
      pump: pump,
      nextDue: nextDue,
      start: function () { if (!running) { running = true; loop(); } },
      stop: function () { running = false; if (loopTimer) clearTimeout(loopTimer); loopTimer = null; wakeAt = Infinity; },
      dropNext: function () { S.drop = true; },
      setFeed: function (name, up) { if (S.feeds[name]) { S.feeds[name].up = !!up; alert(up ? 'INFO' : 'CRIT', name, up ? 'feed up' : 'feed down'); } },
      market: function (key) { return S.markets.get(key); },
      markets: function () { return Array.from(S.markets.values()); },
      order: function (cid) { return S.orders.get(cid); },
      position: function (key) { return S.pos.get(key); },
      cash: function () { return S.cash; },
      // test hooks: pin a market's book (no random walk, no flow) and trade against it
      freeze: function (key, book) {
        var m = S.markets.get(key); if (!m) return null;
        m.frozen = true;
        if (book) {
          m.bids = new Map(book.bids.map(function (l) { return [l[0], l[1]]; }));
          m.asks = new Map(book.asks.map(function (l) { return [l[0], l[1]]; }));
          m.mid = book.bids.length && book.asks.length ? (book.bids[0][0] + book.asks[0][0]) / 2 : m.mid;
          m.bookDirty = true;
        }
        return m;
      },
      trade: function (key, side, qty) { var m = S.markets.get(key); var d = flowTrade(m, clock(), side, qty); flush(clock()); return d; },
      fees: function () { return S.fills.map(function (f) { return { fid: f.fid, v: f.v, px: f.px, qty: f.qty, liq: f.liq, fee: f.fee, fee_type: S.markets.get(f.key) ? S.markets.get(f.key).fee.type : null }; }); }
    };
    return api;
  }

  Desk.sim = { create: create, LANES: LANES, LIP: LIP };
})(typeof window !== 'undefined' ? window : globalThis);
