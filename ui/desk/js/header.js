/* ui/desk/js/header.js
 *
 * The 28 px header, the 20 px status bar, the tab switch, KILL, the ? key sheet and the LOG
 * drawer. Also the global keys: ` (tab), g s / g m, h (hotkeys), l (log), ? (keys), y (resync),
 * d (density), c (colour-blind palette).
 *
 *   Desk.tabs.cur                'manual' | 'systematic'
 *   Desk.tabs.set(name), .toggle()
 *   Desk.header.init()           called once by main.js
 *   Desk.header.openLog(on?), Desk.header.openKeys(on?)
 */
(function (root) {
  'use strict';
  var Desk = root.Desk = root.Desk || {};
  var D = Desk.dom, F = Desk.fmt, R = Desk.render;
  var $ = D.$;
  var REARM_MS = 3000;          // RE-ARM, then CONFIRM within this long

  // ---------------------------------------------------------------------------------------------
  // tabs: one data attribute on <body>; CSS swaps display; nothing is fetched
  // ---------------------------------------------------------------------------------------------
  // SPLIT (>= 2400 px): [SPLIT] shows both tabs side by side; picking one tab (click or `) leaves it
  var tabs = Desk.tabs = {
    cur: null,
    set: function (name) {
      if (name !== 'manual' && name !== 'systematic') return;
      var L = Desk.layout;
      if (L && L.split) { tabs.cur = name; document.body.setAttribute('data-tab', name); Desk.storage.set('tab', name); L.setSplit(false); return; }
      if (tabs.cur === name) return;
      tabs.cur = name;
      document.body.setAttribute('data-tab', name);
      R.tab = name;
      Desk.storage.set('tab', name);
      R.force(name);
      R.mark('tab');
    },
    toggle: function () { tabs.set(tabs.cur === 'manual' ? 'systematic' : 'manual'); },
    split: function () { if (Desk.layout && Desk.layout.splitOK) Desk.layout.setSplit(!Desk.layout.split); }
  };

  // ---------------------------------------------------------------------------------------------
  // overlays
  // ---------------------------------------------------------------------------------------------
  var H = Desk.header = {
    logOpen: false, keysOpen: false, rearmAt: 0, rearmPend: false,
    openLog: function (on) {
      H.logOpen = on === undefined ? !H.logOpen : !!on;
      D.setClass(document.body, 'log', H.logOpen);
      R.mark('alert');
    },
    openKeys: function (on) {
      H.keysOpen = on === undefined ? !H.keysOpen : !!on;
      D.setClass(document.body, 'keys', H.keysOpen);
      if (H.keysOpen) buildSheet();
    },
    init: init
  };

  function buildSheet() {
    var host = $('keysheet-body'); if (!host) return;
    var rows = Desk.keys.sheetRows();
    var tb = D.table(host, [{ h: 'Key', w: 120 }, { h: 'Scope', w: 96 }, { h: 'Action' }, { h: 'HK', w: 36 }], 'keys');
    D.keyedRows(tb, rows, function (r) { return r.scope + ' ' + r.key; }, function (r) {
      return [{ t: r.key, cls: 'kk' }, { t: r.scope, cls: 'dim' }, r.desc, { t: r.order ? 'HK' : '', cls: 'warn' }];
    });
  }

  // ---------------------------------------------------------------------------------------------
  // KILL: one click, no dialog; 60 s lockout; re-arm is two clicks (RE-ARM, then CONFIRM within
  // 3 s), no dialog either. After the re-arm Systematic is STOPPED and START waits for the order
  // groups to reset (the GROUPS preflight row).
  // ---------------------------------------------------------------------------------------------
  function killState(now) {
    var h = Desk.store.hdr;
    if (!h || h.run.state !== 'KILLED') return { st: 'ARMED' };
    if (h.run.rearm_ms && now < h.run.rearm_ms) return { st: 'LOCKED', left: h.run.rearm_ms - now };
    if (H.rearmPend) return { st: 'REARMING' };
    return { st: now - H.rearmAt < REARM_MS ? 'CONFIRM' : 'REARM' };
  }
  function onKill(ev) {
    var now = Date.now(), k = killState(now);
    if (k.st === 'ARMED') { Desk.act.kill(); R.mark('hdr'); return; }
    if (k.st === 'REARM') { H.rearmAt = now; R.mark('hdr'); return; }
    if (k.st === 'CONFIRM') {
      H.rearmAt = 0; H.rearmPend = true; R.mark('hdr');
      var done = function () { H.rearmPend = false; R.mark('hdr'); };
      Desk.act.rearm().then(done, done);
    }
    void ev;
  }

  // ---------------------------------------------------------------------------------------------
  // render
  // ---------------------------------------------------------------------------------------------
  function cell(id, text, cls) { var el = $(id); if (!el) return; D.setText(el, text); if (cls !== undefined && el.className !== cls) el.className = cls; }
  function sum(a, b) { return (a || 0) + (b || 0); }
  function health(id, label, state, valueText) {
    // state: 'ok' | 'warn' | 'crit'
    cell(id, label + ' ' + valueText, 'hh' + (state === 'ok' ? '' : ' ' + state));
  }

  function renderHeader(d) {
    var S = Desk.store, h = S.hdr, now = Date.now();
    if (d.has('hdr') || d.has('snapshot') || d.has('ord') || d.has('clock') || d.all) {
      var run = h ? h.run.state : null;
      cell('tab-sys-st', run === 'RUN' ? 'RUN' : run === 'KILLED' ? 'KILL' : run === 'HALTED' ? 'HALT' : run === 'STOP' ? 'STOP' : '', 'tst ' + (run === 'RUN' ? 'up' : run === 'KILLED' || run === 'HALTED' ? 'dn' : 'dim'));
      var mw = S.manWorking();
      cell('tab-man-n', String(mw), 'tst ' + (mw ? '' : 'dim'));
    }
    if (h && (d.has('hdr') || d.all || d.has('snapshot'))) {
      cell('h-kal', F.money(h.cash.K.total));
      cell('h-kal-free', 'free ' + F.money(h.cash.K.free));
      cell('h-pm', F.money(h.cash.P.total));
      // PM has no shards: its free cash shows only while orders hold some of it
      cell('h-pm-free', h.cash.P.ro || Math.abs(h.cash.P.total - h.cash.P.free) < 100 ? '' : 'free ' + F.money(h.cash.P.free));
      D.setClass($('h-pm-ro'), 'on', h.cash.P.ro);
      cell('h-day', F.pnl(h.day.total), 'hv ' + F.sign(h.day.total));
      cell('h-day-split', 'S ' + F.pnl(h.day.sys) + ' M ' + F.pnl(h.day.man));
      D.setAttr($('hc-day'), 'title', 'S ' + F.pnl(h.day.sys) + ' M ' + F.pnl(h.day.man) + ' LIP ' + F.pnl(h.day.lip));
      cell('h-risk', F.money(h.risk));
      var stopCls = h.stop.hit ? 'hc crit' : (h.stop.left < h.stop.limit / 2 ? 'hc warn' : 'hc');
      cell('h-stop', F.money(h.stop.left) + '/' + F.money(h.stop.limit));
      var hcStop = $('hc-stop'); if (hcStop && hcStop.className !== stopCls) hcStop.className = stopCls;
      var used = sum(h.sys.K.used, h.sys.P.used), eff = sum(h.sys.K.eff, h.sys.P.eff), ceil = sum(h.sys.K.ceiling, h.sys.P.ceiling);
      cell('h-sys', F.money(used, 1) + '/' + F.money(eff, 1));
      var hcSys = $('hc-sys'), sysCls = eff < ceil - 50 ? 'hc warn' : 'hc';
      if (hcSys && hcSys.className !== sysCls) hcSys.className = sysCls;
      D.setAttr($('h-sys'), 'title', 'K ' + F.money(h.sys.K.used) + '/' + F.money(h.sys.K.eff) + ' ceil ' + F.money(h.sys.K.ceiling) +
        '  P ' + F.money(h.sys.P.used) + '/' + F.money(h.sys.P.eff) + ' ceil ' + F.money(h.sys.P.ceiling));
      cell('h-mode', h.mode, 'tag ' + (h.mode === 'DEMO' ? 'warn' : 'dim'));
    } else if (!h && Desk.transport.mode) {
      cell('h-mode', Desk.transport.mode === 'demo' ? 'DEMO' : 'LIVE', 'tag ' + (Desk.transport.mode === 'demo' ? 'warn' : 'dim'));
    }
    if (d.has('clock') || d.has('feeds') || d.has('hdr') || d.has('stale') || d.has('lat') || d.all) {
      var f = S.feeds, lat = h ? h.lat : null;
      // KX: Kalshi feed lag p50 (from sending_ts_ms), or DOWN / GAP
      if (!f) health('h-kx', 'KX', 'warn', '-');
      else if (!f.KX.up || !f.KXP.up) health('h-kx', 'KX', 'crit', 'DOWN');
      else if (f.KX.gap || f.KXP.gap) health('h-kx', 'KX', 'crit', 'GAP');
      else { var kl = lat && lat.kx_lag_ms !== null ? lat.kx_lag_ms : f.KX.lag_ms; health('h-kx', 'KX', kl !== null && kl > 1000 ? 'warn' : 'ok', kl === null ? '-' : kl + 'ms'); }
      if (h && h.cash.P.ro) health('h-pmh', 'PM', 'warn', 'RO');
      else if (!f) health('h-pmh', 'PM', 'warn', '-');
      else if (!f.PM.up) health('h-pmh', 'PM', 'crit', 'DOWN');
      else { var pl = lat && lat.pm_lag_ms !== null ? lat.pm_lag_ms : f.PM.lag_ms; health('h-pmh', 'PM', pl !== null && pl > 1000 ? 'warn' : 'ok', pl === null ? '-' : pl + 'ms'); }
      // GW: click-to-ack p50 over the last 20 orders measured here; the backend's own figure before any click
      var gw = Desk.lat.ack.p50();
      if (gw === null && lat) gw = lat.gw_ack_ms;
      health('h-gw', 'GW', gw !== null && gw > 250 ? 'warn' : 'ok', gw === null ? '-' : Math.round(gw) + 'ms');
      // SRV: heartbeat age
      var age = S.lastHb ? now - S.lastHb : null;
      if (age === null || !S.connected) health('h-srv', 'SRV', 'crit', 'DOWN');
      else if (age > 3000) health('h-srv', 'SRV', 'crit', 'DOWN');
      else health('h-srv', 'SRV', age > 1500 ? 'warn' : 'ok', (age / 1000).toFixed(1) + 's');
      // CLK: our offset to exchange time, seconds
      var off = lat ? lat.clk_off_ms : null;
      health('h-clk', 'CLK', off !== null && Math.abs(off) > 500 ? 'warn' : 'ok', off === null ? '-' : (off >= 0 ? '+' : Desk.MINUS) + (Math.abs(off) / 1000).toFixed(2));
      cell('h-clock', F.time(now));
      // KILL label
      var k = killState(now), kb = $('kill');
      if (kb) {
        var label = k.st === 'ARMED' ? 'KILL' : k.st === 'LOCKED' ? 'KILLED ' + F.mmss(k.left) : k.st === 'CONFIRM' ? 'CONFIRM' : k.st === 'REARMING' ? 'RE-ARMING' : 'RE-ARM';
        D.setText(kb, label);
        var kc = k.st === 'ARMED' ? '' : k.st === 'LOCKED' ? 'locked' : k.st === 'CONFIRM' ? 'rearm confirm' : 'rearm';
        if (kb.className !== kc) kb.className = kc;
      }
    }
  }

  function renderStatus(d) {
    var S = Desk.store;
    if (d.has('alert') || d.all || d.has('snapshot')) {
      var a = S.alerts[0], el = $('sb-alert');
      if (el) {
        D.setText(el, a ? F.time(a.ts_ms) + ' ' + a.lvl + ' ' + a.src + ' ' + a.msg : '');
        var c = 'sb-alert' + (a ? (a.lvl === 'CRIT' ? ' crit' : a.lvl === 'WARN' ? ' warn' : '') : '');
        if (el.className !== c) el.className = c;
      }
      if (H.logOpen) renderLog();
    }
    if (d.has('stale') || d.has('clock') || d.all) {
      D.setClass(document.body, 'stale', S.stale);
      D.setAttr(document.body, 'data-kstale', S.vstale.K ? '1' : null);
      D.setAttr(document.body, 'data-pstale', S.vstale.P ? '1' : null);
      var why = S.stale ? 'STALE ' + S.staleWhy : (S.vstale.K || S.vstale.P || '');
      cell('sb-stale', why, 'sbc' + (why ? ' crit' : ' off'));
    }
    if (d.has('hk') || d.all) {
      cell('sb-hk', Desk.keys.hk ? 'HOTKEYS ON' : 'HOTKEYS OFF', 'sbc' + (Desk.keys.hk ? ' warn' : ' dim'));
      D.setAttr(document.body, 'data-hk', Desk.keys.hk ? '1' : '0');
    }
    if (d.has('lat') || d.has('clock') || d.all) {
      var p = Desk.lat.paint.last, ak = Desk.lat.ack.p50();
      cell('sb-ui', 'UI ' + (p === null ? '-' : (p < 10 ? p.toFixed(1) : Math.round(p)) + 'ms'), 'sbc' + (p !== null && p > 16.7 ? ' warn' : ''));
      cell('sb-ack', 'ACK ' + (ak === null ? '-' : Math.round(ak) + 'ms'), 'sbc' + (ak !== null && ak > 250 ? ' warn' : ''));
      var errs = R.errors + S.invalid;
      cell('sb-err', errs ? 'ERR ' + errs : '', 'sbc' + (errs ? ' crit' : ' off'));
      if (errs) D.setAttr($('sb-err'), 'title', (R.lastError || '') + (S.lastInvalid ? ' | ' + S.lastInvalid : ''));
    }
  }

  var logTb = null;
  function renderLog() {
    var host = $('log-body'); if (!host) return;
    if (!logTb) logTb = D.table(host, [{ h: 'Time', w: 64 }, { h: 'Lvl', w: 40 }, { h: 'Source', w: 64 }, { h: 'Message' }], 'log');
    var rows = Desk.store.alerts.slice(0, 300);
    D.keyedRows(logTb, rows, function (a) { return a.id; }, function (a) {
      return [{ t: F.time(a.ts_ms), cls: 'n' }, { t: a.lvl, cls: a.lvl === 'CRIT' ? 'dn' : a.lvl === 'WARN' ? 'warn' : 'dim' }, a.src, { t: a.msg, title: a.msg }];
    });
  }

  // ---------------------------------------------------------------------------------------------
  // init
  // ---------------------------------------------------------------------------------------------
  function init() {
    // ?tab=systematic|manual opens that tab (a link or a bookmark); otherwise the remembered one
    var q = Desk.qs && Desk.qs.tab;
    var saved = (q === 'systematic' || q === 'manual' ? q : Desk.storage.get('tab')) === 'systematic' ? 'systematic' : 'manual';
    if (Desk.layout && Desk.layout.split) { tabs.cur = saved; document.body.setAttribute('data-tab', saved); R.tab = null; R.mark('tab'); }
    else tabs.set(saved);
    $('tab-sys').addEventListener('click', function () { tabs.set('systematic'); });
    $('tab-man').addEventListener('click', function () { tabs.set('manual'); });
    var sp = $('tab-split'); if (sp) sp.addEventListener('click', function () { tabs.split(); });
    $('kill').addEventListener('click', onKill);
    $('sb-alert').addEventListener('click', function () { H.openLog(); });
    $('sb-keys').addEventListener('click', function () { H.openKeys(); });
    $('sb-hk').addEventListener('click', function () { Desk.keys.setHK(!Desk.keys.hk); });
    var closeBtn = $('keysheet-close'); if (closeBtn) closeBtn.addEventListener('click', function () { H.openKeys(false); });
    var logClose = $('log-close'); if (logClose) logClose.addEventListener('click', function () { H.openLog(false); });

    Desk.keys.overlays.push(function () { if (H.keysOpen) { H.openKeys(false); return true; } return false; });
    Desk.keys.overlays.push(function () { if (H.logOpen) { H.openLog(false); return true; } return false; });

    var K = Desk.keys;
    K.bind({ key: '`', desc: 'switch SYSTEMATIC / MANUAL', fn: function () { tabs.toggle(); } });
    K.bind({ key: '~', desc: 'SPLIT: both tabs side by side (2400 px and wider)', fn: function () { tabs.split(); } });
    K.bind({ key: 's', after: 'g', desc: 'go to SYSTEMATIC', fn: function () { tabs.set('systematic'); } });
    K.bind({ key: 'm', after: 'g', desc: 'go to MANUAL', fn: function () { tabs.set('manual'); } });
    K.bind({ key: 'h', desc: 'hotkeys on/off (order keys need ON)', fn: function () { K.setHK(!K.hk); } });
    K.bind({ key: 'l', desc: 'log drawer', fn: function () { H.openLog(); } });
    K.bind({ key: '?', desc: 'this sheet', fn: function () { H.openKeys(); } });
    K.bind({ key: 'y', desc: 'resync from snapshot', fn: function () { Desk.act.resync(); } });
    K.bind({ key: 'd', desc: 'density compact/normal', fn: function () { Desk.ui.cycleDensity(); } });
    K.bind({ key: 'c', desc: 'colour-blind palette', fn: function () { Desk.ui.toggleCB(); } });
    K.bind({ key: 'Escape', desc: 'close sheet/drawer, leave a field', doc: true });

    R.add({ id: 'header', tab: null, fn: renderHeader });
    R.add({ id: 'status', tab: null, fn: renderStatus });
  }
})(typeof window !== 'undefined' ? window : globalThis);
