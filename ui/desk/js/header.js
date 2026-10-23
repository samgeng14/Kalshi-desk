/* ui/desk/js/header.js
 *
 * The 28 px header, the 20 px status bar, the tab switch, KILL, the ? key sheet, the LOG drawer
 * (and the always-open LOG panel of SYSTEMATIC from 2400 px), and the settings drawer (the desk's
 * limits read-only; the per-viewer display and alert settings editable).
 * Also the global keys: ` (tab), g s / g m, h (hotkeys), l (log), p (settings), ? (keys),
 * y (resync), d (density), c (colour-blind palette), f (FLASH off / subtle / full).
 *
 *   Desk.tabs.cur                'manual' | 'systematic'
 *   Desk.tabs.set(name), .toggle()
 *   Desk.header.init()           called once by main.js
 *   Desk.header.openLog(on?), Desk.header.openKeys(on?), Desk.header.openSet(on?)
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
      if (Desk.activity) Desk.activity.seen(name === 'systematic' ? 'SYS' : 'MAN');
      // no full redraw: the tab's renderers kept what they missed while hidden and caught up in
      // the background about once a second (Desk.render.bg), so this frame draws only the rest
      R.mark('tab');
    },
    toggle: function () { tabs.set(tabs.cur === 'manual' ? 'systematic' : 'manual'); },
    split: function () { if (Desk.layout && Desk.layout.splitOK) Desk.layout.setSplit(!Desk.layout.split); }
  };

  // ---------------------------------------------------------------------------------------------
  // overlays
  // ---------------------------------------------------------------------------------------------
  var H = Desk.header = {
    logOpen: false, keysOpen: false, setOpen: false, rearmAt: 0, rearmPend: false, dayCents: null, realCents: null,
    openLog: function (on) {
      H.logOpen = on === undefined ? !H.logOpen : !!on;
      D.setClass(document.body, 'log', H.logOpen);
      R.mark('alert');
    },
    openSet: function (on) {
      H.setOpen = on === undefined ? !H.setOpen : !!on;
      D.setClass(document.body, 'set', H.setOpen);
      R.mark('set');
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
    // letters match the physical key (the US-layout position) so Option+letter works on a Mac
    rows.push({ key: 'A-Z', scope: 'note', desc: 'letters by key position (US layout)', order: false });
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
  // KILLING (sent, not confirmed: 'KILL ...'), KILLFAIL (not confirmed, retrying; a click retries
  // now), LOCKED (the 60 s lockout, server clock), REARM, CONFIRM, REARMING, ARMED
  function killState(now) {
    var h = Desk.store.hdr, k = Desk.act.killing;
    if (k) return { st: k.failed ? 'KILLFAIL' : 'KILLING' };
    if (!h || h.run.state !== 'KILLED') return { st: 'ARMED' };
    var srv = now + (Desk.store.srvOffset || 0);
    if (h.run.rearm_ms && srv < h.run.rearm_ms) return { st: 'LOCKED', left: h.run.rearm_ms - srv };
    if (H.rearmPend) return { st: 'REARMING' };
    return { st: now - H.rearmAt < REARM_MS ? 'CONFIRM' : 'REARM' };
  }
  function onKill(ev) {
    var now = Date.now(), k = killState(now);
    if (k.st === 'ARMED' || k.st === 'KILLFAIL') { Desk.act.kill(); R.mark('hdr'); return; }
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
  // today's realized P&L, both owners, net of fees: the positions' real (the backend's own figure;
  // the header's day.total adds the marks and the LIP accrual)
  function realToday(S) { var t = 0; S.pos.forEach(function (P) { t += P.real || 0; }); return t; }
  function cell(id, text, cls) { var el = $(id); if (!el) return; D.setText(el, text); if (cls !== undefined && el.className !== cls) el.className = cls; }
  function sum(a, b) { return (a || 0) + (b || 0); }
  function health(id, label, state, valueText) {
    // state: 'ok' | 'warn' | 'crit' | 'off'
    cell(id, label + ' ' + valueText, 'hh' + (state === 'ok' ? '' : ' ' + state));
  }
  // Polymarket not connected at all (deskd has no PM venue yet): read-only, no cash, its feed
  // never up. Shown OFF and dim rather than as a failing key; a PM that was up and failed still
  // reads RO / DOWN.
  function pmOff(S) {
    var h = S.hdr, f = S.feeds;
    return !!(h && h.cash.P.ro && h.cash.P.total === 0 && (!f || (!f.PM.up && f.PM.lag_ms === null)));
  }

  // ---------------------------------------------------------------------------------------------
  // PAPER | LIVE: one toggle, two accounts. Each account is its own deskd (its own lock, ledger,
  // venue connections and port: paper, the demo account, 8788; live, production, 8789), both
  // started by `python3 tools/desk.py`; the toggle flips this tab to the other one
  // (<meta name="desk-peer">), on the same tab of the screen, asking first before LIVE.
  // ---------------------------------------------------------------------------------------------
  var desk = { peer: '', venue: '', mode: '' };
  function meta(name) {
    try { var el = document.querySelector('meta[name="' + name + '"]'); return el ? el.getAttribute('content') || '' : ''; }
    catch (e) { return ''; }
  }
  function modeCell(mode) {   // mode: the header's DEMO | LIVE ('' before the first header)
    desk.mode = mode;
    var sim = Desk.transport.mode === 'demo' || desk.venue === 'sim', live = mode === 'LIVE', can = switchable();
    var what = live ? 'LIVE: the production Kalshi account, real money' :
      sim ? 'SIM: ' + (Desk.transport.mode === 'demo' ? 'the simulated desk in this page' : 'the fake exchange on this machine') + ', nothing reaches Kalshi' :
      'PAPER: the Kalshi demo account, play money';
    if (can) {
      // the toggle: PAPER left, LIVE right, this desk's side lit
      cell('h-mode', 'PAPER', live ? 'tag sw' : 'tag warn');
      cell('h-mode-b', 'LIVE', live ? 'tag crit' : 'tag sw');
    } else {
      cell('h-mode', !mode ? '-' : live ? 'LIVE' : sim ? 'SIM' : 'PAPER', live ? 'tag crit' : 'tag warn');
      cell('h-mode-b', '', 'tag hide');
    }
    D.setAttr($('hc-mode'), 'title', what + (can ? '. Click to switch to ' + (live ? 'PAPER' : 'LIVE') : ''));
    D.setClass($('hc-mode'), 'peer', can);
  }
  // only between the two accounts' desks: the in-page simulation and the fake exchange have none
  function switchable() { return !!(desk.peer && desk.mode && Desk.transport.mode === 'live' && desk.venue !== 'sim'); }
  function switchDesk() {
    if (!switchable()) return;
    var toLive = desk.mode !== 'LIVE', name = toLive ? 'LIVE' : 'PAPER';
    // is it up? the other desk refuses this cross-port fetch (403), but a refusal still answers
    fetch(desk.peer + 'api/health', { mode: 'no-cors', cache: 'no-store', credentials: 'omit' }).then(function () {
      if (toLive && !root.confirm('Switch to LIVE?\n\nOrders there trade your real Kalshi account with real money.')) return;
      root.location.assign(desk.peer + '?tab=' + (Desk.tabs.cur || 'manual'));   // the same tab of the screen
    }, function () {
      Desk.store.localAlert('WARN', 'DESK', name + ' is not running: start both with python3 tools/desk.py' +
        (toLive ? ' (after python3 tools/desk.py setup live, once)' : ''));
    });
  }

  function renderHeader(d) {
    var S = Desk.store, h = S.hdr, now = Date.now();
    if (d.has('act') || d.has('tab') || d.all) {
      // ACT: events (by the alert filters) not seen yet; amber while any
      var A = Desk.activity, na = A ? A.total() : 0, hca = $('hc-act');
      cell('h-act', String(na));
      if (hca) {
        var ca = 'hc' + (na ? ' warn' : '');
        if (hca.className !== ca) hca.className = ca;
        D.setAttr(hca, 'title', A ? 'not seen: SYS ' + A.unseen.SYS + ', MAN ' + A.unseen.MAN + '; desktop alerts ' + (A.cfg.on ? 'on' : 'off') : null);
      }
    }
    if (d.has('hdr') || d.has('snapshot') || d.has('ord') || d.has('clock') || d.all) {
      // the tab's run state: RUN, OFF (stopped: STOP is the button's word), KILL, HALT
      var run = h ? h.run.state : null;
      cell('tab-sys-st', run === 'RUN' ? 'RUN' : run === 'KILLED' ? 'KILL' : run === 'HALTED' ? 'HALT' : run === 'STOP' ? 'OFF' : '', 'tst ' + (run === 'RUN' ? 'up' : run === 'KILLED' || run === 'HALTED' ? 'dn' : 'dim'));
      var mw = S.manWorking();
      cell('tab-man-n', String(mw), 'tst ' + (mw ? '' : 'dim'));
    }
    if (h && (d.has('hdr') || d.has('pos') || d.all || d.has('snapshot'))) {
      cell('h-kal', F.money(h.cash.K.total));
      cell('h-kal-free', 'free ' + F.money(h.cash.K.free));
      D.setAttr($('hc-kal'), 'title', 'free ' + F.money(h.cash.K.free) + '  ' + h.cash.K.shards.map(function (x) { return x.id + ' ' + F.money(x.cash) + '/' + F.money(x.free); }).join(' '));
      cell('h-pm', F.money(h.cash.P.total));
      // PM has no shards: its free cash shows only while orders hold some of it
      cell('h-pm-free', h.cash.P.ro || Math.abs(h.cash.P.total - h.cash.P.free) < 100 ? '' : 'free ' + F.money(h.cash.P.free));
      D.setAttr($('hc-pm'), 'title', pmOff(S) ? 'Polymarket: not connected on this desk' : h.cash.P.ro ? 'read-only: PM key failing' : 'free ' + F.money(h.cash.P.free));
      D.setClass($('h-pm-ro'), 'on', h.cash.P.ro && !pmOff(S));
      // the day's P&L flashes when its realized part moves by a cent (a fill that closed, an exit,
      // a settlement: green up, red down), at most once every 1.5 s; marks and the LIP accrual
      // move it every second while the lanes run, so a move of the total flashes only with FULL
      var dayCents = Math.round(h.day.total / 100), realCents = Math.round(realToday(S) / 100), hcDay = $('hc-day');
      if (H.realCents !== null && realCents !== H.realCents) D.flash(hcDay, realCents > H.realCents ? 'up' : 'dn', null, 1500);
      else if (H.dayCents !== null && dayCents !== H.dayCents) D.flash(hcDay, dayCents > H.dayCents ? 'up' : 'dn', 'full', 1500);
      H.dayCents = dayCents; H.realCents = realCents;
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
      modeCell(h.mode);
    } else if (!h && Desk.transport.mode) {
      modeCell(Desk.transport.mode === 'demo' ? 'DEMO' : '');
    }
    if (d.has('clock') || d.has('feeds') || d.has('hdr') || d.has('stale') || d.has('lat') || d.all) {
      var f = S.feeds, lat = h ? h.lat : null;
      // KX: Kalshi feed lag p50 (from sending_ts_ms), or DOWN / GAP
      if (!f) health('h-kx', 'KX', 'warn', '-');
      else if (!f.KX.up || !f.KXP.up) health('h-kx', 'KX', 'crit', 'DOWN');
      else if (f.KX.gap || f.KXP.gap) health('h-kx', 'KX', 'crit', 'GAP');
      else { var kl = lat && lat.kx_lag_ms !== null ? lat.kx_lag_ms : f.KX.lag_ms; health('h-kx', 'KX', kl !== null && kl > 1000 ? 'warn' : 'ok', kl === null ? '-' : kl + 'ms'); }
      if (h && pmOff(S)) health('h-pmh', 'PM', 'off', 'OFF');
      else if (h && h.cash.P.ro) health('h-pmh', 'PM', 'warn', 'RO');
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
        var label = k.st === 'ARMED' ? 'KILL' : k.st === 'KILLING' ? 'KILL ...' : k.st === 'KILLFAIL' ? 'KILL FAIL' :
          k.st === 'LOCKED' ? 'KILLED ' + F.secs(k.left) : k.st === 'CONFIRM' ? 'CONFIRM' : k.st === 'REARMING' ? 'RE-ARMING' : 'RE-ARM';
        D.setText(kb, label);
        var kc = k.st === 'ARMED' ? '' : k.st === 'KILLING' ? 'pend' : k.st === 'KILLFAIL' ? 'fail' : k.st === 'LOCKED' ? 'locked' : k.st === 'CONFIRM' ? 'rearm confirm' : 'rearm';
        if (kb.className !== kc) kb.className = kc;
        var kr = Desk.store.hdr ? Desk.store.hdr.run : null;
        D.setAttr(kb, 'title', kr && kr.state === 'KILLED' ? 'KILLED ' + F.time(kr.since_ms) + (kr.why ? ': ' + kr.why : '') +
          (k.st === 'LOCKED' ? '' : '. Click RE-ARM, then CONFIRM') : null);
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
      var why = S.stale ? 'STALE ' + S.staleWhy : (S.vstale.K || (pmOff(S) ? '' : S.vstale.P) || '');   // no PM: no alarm
      cell('sb-stale', why, 'sbc' + (why ? ' crit' : ' off'));
    }
    if (d.has('hk') || d.all) {
      cell('sb-hk', Desk.keys.hk ? 'HOTKEYS ON' : 'HOTKEYS OFF', 'sbc' + (Desk.keys.hk ? ' warn' : ' dim'));
      D.setAttr(document.body, 'data-hk', Desk.keys.hk ? '1' : '0');
    }
    if (d.has('lat') || d.has('clock') || d.all) {
      // click-to-paint in whole ms (Safari's clock is 1 ms coarse); amber past one frame interval
      var p = Desk.lat.paint.last, ak = Desk.lat.ack.p50(), fi = Desk.lat.fi.p50();
      cell('sb-ui', 'UI ' + (p === null ? '-' : Math.round(p) + 'ms'), 'sbc' + (p !== null && p > Desk.lat.budget() ? ' warn' : ''));
      D.setAttr($('sb-ui'), 'title', 'click to paint, last; p50 ' + (Desk.lat.paint.p50() === null ? '-' : Math.round(Desk.lat.paint.p50()) + 'ms') +
        '; frame ' + (fi === null ? '-' : fi.toFixed(1) + 'ms'));
      cell('sb-ack', 'ACK ' + (ak === null ? '-' : Math.round(ak) + 'ms'), 'sbc' + (ak !== null && ak > 250 ? ' warn' : ''));
      var errs = R.errors + S.invalid;
      cell('sb-err', errs ? 'ERR ' + errs : '', 'sbc' + (errs ? ' crit' : ' off'));
      if (errs) D.setAttr($('sb-err'), 'title', (R.lastError || '') + (S.lastInvalid ? ' | ' + S.lastInvalid : ''));
    }
  }

  // the LOG grid: the drawer (l) and SYSTEMATIC's own LOG panel from 2400 px share it
  var logTbs = {};
  function logInto(hostId, max) {
    var host = $(hostId); if (!host) return;
    var tb = logTbs[hostId];
    if (!tb || !host.contains(tb)) tb = logTbs[hostId] = D.table(host, [{ h: 'Time', w: 74 }, { h: 'Lvl', w: 46 }, { h: 'Source', w: 64 }, { h: 'Message' }], 'log');
    var rows = Desk.store.alerts.slice(0, max);
    if (!rows.length) { D.emptyRow(tb, 4); return; }
    D.keyedRows(tb, rows, function (a) { return a.id; }, function (a) {
      return [{ t: F.time(a.ts_ms), cls: 'n' }, { t: a.lvl, cls: a.lvl === 'CRIT' ? 'dn' : a.lvl === 'WARN' ? 'warn' : 'dim' }, a.src, { t: a.msg, title: a.msg }];
    });
  }
  function renderLog() { logInto('log-body', 300); }
  // SYSTEMATIC's LOG panel: shown from 2400 px (not in SPLIT), where the width is there for it
  function renderSysLog(d) {
    var L = Desk.layout;
    if (!L || !L.splitOK || L.split) return;
    if (d.has('alert') || d.has('snapshot') || d.has('layout') || d.all) logInto('s-log-body', 200);
  }

  // ---------------------------------------------------------------------------------------------
  // the settings drawer (p): read-only. What the lanes run with and what the fees are, from the
  // snapshot; the limits from the header; the page's own constants. Nothing here edits anything.
  // ---------------------------------------------------------------------------------------------
  var setTbs = {};
  function setGrid(id, cols) {
    var host = $(id); if (!host) return null;
    if (!setTbs[id] || !host.contains(setTbs[id])) setTbs[id] = D.table(host, cols, 'set');
    return setTbs[id];
  }
  function renderSet(d) {
    if (!H.setOpen || !(d.has('set') || d.has('lane') || d.has('hdr') || d.has('mkt') || d.has('snapshot') || d.all)) return;
    var S = Desk.store, h = S.hdr, sc = Desk.systematic ? Desk.systematic.cfg : null, mc = Desk.manual && Desk.manual.state ? Desk.manual.state().cfg : null;
    var tl = setGrid('set-lanes', [{ h: 'Lane', w: 40 }, { h: 'Name' }, { h: 'V', w: 28 }, { h: 'Style', w: 48 }, { h: 'Budget', w: 60, cls: 'n' }, { h: 'Thr\u00a2', w: 48, cls: 'n' }, { h: 'On', w: 36 }]);
    var lanes = S.laneOrder.map(function (id) { return S.lanes.get(id); }).filter(Boolean);
    if (tl) { if (!lanes.length) D.emptyRow(tl, 7); else D.keyedRows(tl, lanes, function (l) { return l.id; }, function (l) {
      return [l.id, { t: l.name, cls: 't' }, { t: l.venue, cls: 'dim' }, { t: l.style, cls: 'dim' }, { t: F.money(l.budget), cls: 'n' },
              { t: l.thr === null ? '' : (l.thr / 100).toFixed(1), cls: 'n' }, { t: l.on ? 'ON' : 'OFF', cls: l.on ? '' : 'dim' }];
    }); }
    // fees: one row per venue, kind and fee type, with the per-contract fee at 50c
    var fees = new Map();
    S.markets.forEach(function (m) {
      if (m.status === 'SETTLED') return;
      var k = m.v + '|' + m.kind + '|' + m.fee.type + '|' + m.fee.mult, r = fees.get(k);
      if (!r) { r = { k: k, v: m.v, kind: m.kind, fee: m.fee, n: 0 }; fees.set(k, r); }
      r.n++;
    });
    var tf = setGrid('set-fees', [{ h: 'V', w: 24 }, { h: 'Kind' }, { h: 'Fee', w: 112 }, { h: 'x', w: 32, cls: 'n' }, { h: 'T\u00a2@50', w: 58, cls: 'n' }, { h: 'M\u00a2@50', w: 58, cls: 'n' }, { h: 'Mkts', w: 40, cls: 'n' }]);
    var fl = Array.from(fees.values()).sort(function (a, b) { return a.k < b.k ? -1 : 1; });
    if (tf) { if (!fl.length) D.emptyRow(tf, 7); else D.keyedRows(tf, fl, function (r) { return r.k; }, function (r) {
      var t = Desk.fees.takerPer(r.v, r.fee, 5000), mk = Desk.fees.exact(r.v, r.fee, 5000, 1, 'M') * 10000;
      return [r.v, { t: r.kind, cls: 't' }, { t: r.fee.type, cls: 't' }, { t: String(r.fee.mult), cls: 'n' }, { t: (t / 100).toFixed(2), cls: 'n' },
              { t: (mk < 0 ? Desk.MINUS : '') + Math.abs(mk / 100).toFixed(2), cls: 'n' }, { t: String(r.n), cls: 'n' }];
    }); }
    var rows = [];
    var kv = function (k, v) { rows.push({ k: k, v: v }); };
    if (h) {
      kv('daily loss stop, Systematic', F.money(h.stop.limit));
      kv('ceiling, Systematic', (sc ? Math.round(sc.ceilPct * 100) : 70) + '%  K ' + F.money(h.sys.K.ceiling) + '  P ' + F.money(h.sys.P.ceiling));
      kv('daily cost budget, IOC lanes', F.money(h.cost.budget));
    }
    if (sc) { kv('equity floor, Systematic', F.money(sc.floorU)); kv('P(stop) daily sigma', F.money(sc.sigmaDayU)); kv('lane amber, Rlzd under Pred', (sc.edgeWarnU / 100).toFixed(1) + '\u00a2 at n ' + sc.edgeWarnN); }
    kv('price guard, Manual', '2 ticks through the touch');
    if (mc) kv('slippage cap, Manual', mc.cap + ' tick' + (mc.cap === 1 ? '' : 's'));
    kv('ladder expiries', '5 15 60 s, CL (until close)');
    kv('ticket types', 'IOC FOK 5s 15s 60s 5m 30m START CLOSE');
    kv('KILL lockout', '60 s');
    kv('contract', (Desk.contract ? Desk.contract.version : '') + ' ' + (Desk.transport.mode || ''));
    var tk = setGrid('set-limits', [{ h: 'Limit' }, { h: 'Value' }]);
    if (tk) D.keyedRows(tk, rows, function (r) { return r.k; }, function (r) { return [{ t: r.k, cls: 't lbl' }, { t: r.v, title: r.v }]; });
  }

  // ---------------------------------------------------------------------------------------------
  // the drawer's per-viewer column (saved in this browser only): FLASH and the alerts
  // ---------------------------------------------------------------------------------------------
  var SU = H.su = {};
  function mk(tag, cls, text, attrs) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text) e.textContent = text;
    if (attrs) for (var k in attrs) e.setAttribute(k, attrs[k]);
    return e;
  }
  function setHead(host, text) { host.appendChild(mk('div', 'set-h', text)); }
  // a row of choice buttons: [[value, label], ...] sending data-a=act data-v=value
  function setRow(host, label, items, act) {
    var row = mk('div', 'set-row');
    row.appendChild(mk('span', 'lb', label));
    var btns = items.map(function (it) { var b = mk('button', '', it[1], { type: 'button', 'data-a': act, 'data-v': String(it[0]) }); row.appendChild(b); return b; });
    host.appendChild(row);
    return { row: row, btns: btns };
  }
  function buildSetUi() {
    var host = $('set-ui'); if (!host) return;
    host.textContent = '';
    setHead(host, 'Display');
    SU.flash = setRow(host, 'Flash', [['off', 'OFF'], ['subtle', 'SUBTLE'], ['full', 'FULL']], 'flash');
    SU.perf = setRow(host, 'Perf', [['0', 'OFF'], ['1', 'ON']], 'perf');
    SU.perfSt = mk('span', 'st'); SU.perf.row.appendChild(SU.perfSt);
    // ALERTS: desktop notifications (Web Notifications), a short sound, when, and which events
    // per mode; at most one alert per mode every 4 s, the rest batched into it
    var T = Desk.activity ? Desk.activity.TYPES : [];
    setHead(host, 'Alerts');
    SU.desk = setRow(host, 'Desktop', [['on', 'ON'], ['off', 'OFF']], 'alerts');
    SU.deskSt = mk('span', 'st'); SU.desk.row.appendChild(SU.deskSt);
    SU.sound = setRow(host, 'Sound', [['1', 'ON'], ['0', 'OFF']], 'sound');
    SU.when = setRow(host, 'When', [['hidden', 'HIDDEN'], ['always', 'ALWAYS']], 'when');
    SU.sys = setRow(host, 'SYS', T.map(function (t) { return [t, t]; }), 'tsys');
    SU.man = setRow(host, 'MAN', T.map(function (t) { return [t, t]; }), 'tman');
    SU.soundSt = mk('span', 'st'); SU.sound.row.appendChild(SU.soundSt);
    SU.test = setRow(host, 'Test', [['SYS', 'SYS'], ['MAN', 'MAN']], 'atest');
    // what the last TEST did: 'shown', 'blocked (Safari settings)', 'off: turn Desktop or Sound on'
    SU.testSt = mk('span', 'st'); SU.test.row.appendChild(SU.testSt);
    var rateRow = mk('div', 'set-row'); rateRow.appendChild(mk('span', 'lb', 'Rate'));
    SU.rate = mk('span', 'st', 'MAN 1 per 4 s; SYS a digest per 30 s'); rateRow.appendChild(SU.rate);
    host.appendChild(rateRow);
    // choices act on pointerdown; the permission request (Desktop ON) and the sound keep the click:
    // Safari shows its prompt, and starts audio, only for a click
    D.onPress(host, onSetUi, function (ev) {
      var b = ev.target && ev.target.closest ? ev.target.closest('button') : null, a = b ? b.getAttribute('data-a') : null;
      return b && !b.disabled && a !== 'alerts' && a !== 'sound' && a !== 'atest' ? b : null;
    });
  }
  function onSetUi(ev) {
    var b = ev.target && ev.target.closest ? ev.target.closest('button') : null;
    if (!b || b.disabled) return;
    var a = b.getAttribute('data-a'), v = b.getAttribute('data-v');
    var A = Desk.activity;
    if (a === 'flash') Desk.ui.setFlash(v);
    else if (a === 'perf') Desk.perf.set(v === '1');
    else if (a === 'alerts' && A) { if (v === 'on') A.enable().then(function () { R.mark('set'); R.mark('act'); }); else A.disable(); }
    else if (a === 'sound' && A) A.setSound(v === '1');
    else if (a === 'when' && A) A.setWhen(v);
    else if (a === 'tsys' && A) A.toggleType('SYS', v);
    else if (a === 'tman' && A) A.toggleType('MAN', v);
    else if (a === 'atest' && A) SU.lastTest = A.test(v);
    R.mark('set'); R.mark('act');
  }
  function onOf(r, v) { if (r) r.btns.forEach(function (b) { D.setClass(b, 'on', b.getAttribute('data-v') === String(v)); }); }
  function renderSetUi(d) {
    if (!H.setOpen || !(d.has('set') || d.has('ui') || d.all)) return;
    onOf(SU.flash, Desk.ui.flash);
    onOf(SU.perf, Desk.perf.on ? '1' : '0');
    var A = Desk.activity;
    if (A && SU.desk) {
      var perm = A.permission(), file = !!(root.location && root.location.protocol === 'file:');
      onOf(SU.desk, A.cfg.on ? 'on' : 'off');
      // Safari keeps the permission per website: a page opened from a file may never be granted
      D.setText(SU.deskSt, perm === 'granted' ? 'allowed' : perm === 'denied' ? (file ? 'needs the served page (127.0.0.1)' : 'blocked (Safari settings)') : perm === 'default' ? 'not asked' : 'not in this browser');
      SU.deskSt.className = 'st' + (perm === 'denied' || perm === 'unsupported' ? ' warn' : (perm === 'granted' ? ' ok' : ''));
      onOf(SU.sound, A.cfg.sound ? '1' : '0');
      var as = A.audioState ? A.audioState() : 'off';
      D.setText(SU.soundSt, as === 'suspended' ? 'tap to enable' : (as === 'none' ? 'no Web Audio' : ''));
      SU.soundSt.className = 'st' + (as === 'suspended' || as === 'none' ? ' warn' : '');
      D.setText(SU.testSt, SU.lastTest || '');
      SU.testSt.className = 'st' + (!SU.lastTest ? '' : (/blocked|off:|not |needs|tap to|no sound|no Web/.test(SU.lastTest) ? ' warn' : ' ok'));
      onOf(SU.when, A.cfg.when);
      [[SU.sys, 'SYS'], [SU.man, 'MAN']].forEach(function (x) { x[0].btns.forEach(function (b) { D.setClass(b, 'on', A.cfg[x[1]].indexOf(b.getAttribute('data-v')) >= 0); }); });
    }
    // the dev check's worst control: handler / painted, ms
    var w = Desk.perf.on ? Desk.perf.report()[0] : null;
    D.setText(SU.perfSt, w ? 'worst ' + w.ctl + ' ' + w.h + '/' + w.p + 'ms' + (Desk.perf.slow ? ' SLOW ' + Desk.perf.slow : '') : '');
    if (SU.perfSt) SU.perfSt.className = 'st' + (Desk.perf.slow ? ' warn' : '');
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
    // the tab switch, the drawers and the status-bar buttons act on pointerdown (safe: they change
    // only what the screen shows); KILL keeps the click (pressed state on pointerdown)
    var now = function () { return true; }, press = D.onPress;
    press($('tab-sys'), function () { tabs.set('systematic'); }, now);
    press($('tab-man'), function () { tabs.set('manual'); }, now);
    press($('tab-split'), function () { tabs.split(); }, now);
    // ACT: show the tab with the events not seen yet, and count them seen
    press($('hc-act'), function () {
      var A = Desk.activity; if (!A || !A.total()) return;
      var mode = A.unseen.SYS >= A.unseen.MAN ? 'SYS' : 'MAN';
      tabs.set(mode === 'SYS' ? 'systematic' : 'manual'); A.seen(mode); R.mark('act');
    }, now);
    $('kill').addEventListener('click', onKill);
    desk.peer = meta('desk-peer'); desk.venue = meta('desk-venue');
    if (desk.peer && desk.peer.charAt(desk.peer.length - 1) !== '/') desk.peer += '/';
    $('hc-mode').addEventListener('click', switchDesk);
    press($('sb-alert'), function () { H.openLog(); }, now);
    press($('sb-keys'), function () { H.openKeys(); }, now);
    press($('sb-set'), function () { H.openSet(); }, now);
    press($('set-close'), function () { H.openSet(false); }, now);
    press($('sb-hk'), function () { Desk.keys.setHK(!Desk.keys.hk); }, now);
    press($('keysheet-close'), function () { H.openKeys(false); }, now);
    press($('log-close'), function () { H.openLog(false); }, now);
    buildSetUi();

    Desk.keys.overlays.push(function () { if (H.keysOpen) { H.openKeys(false); return true; } return false; });
    Desk.keys.overlays.push(function () { if (H.logOpen) { H.openLog(false); return true; } return false; });
    Desk.keys.overlays.push(function () { if (H.setOpen) { H.openSet(false); return true; } return false; });

    var K = Desk.keys;
    K.bind({ key: '`', desc: 'switch SYSTEMATIC / MANUAL', fn: function () { tabs.toggle(); } });
    K.bind({ key: '~', desc: 'SPLIT: both tabs side by side (2400 px and wider)', fn: function () { tabs.split(); } });
    K.bind({ key: 's', after: 'g', desc: 'go to SYSTEMATIC', fn: function () { tabs.set('systematic'); } });
    K.bind({ key: 'm', after: 'g', desc: 'go to MANUAL', fn: function () { tabs.set('manual'); } });
    K.bind({ key: 'h', desc: 'hotkeys on/off (order keys need ON)', fn: function () { K.setHK(!K.hk); } });
    K.bind({ key: 'l', desc: 'log drawer', fn: function () { H.openLog(); } });
    K.bind({ key: 'p', desc: 'settings: lanes, fees, limits (read-only); flash, alerts', fn: function () { H.openSet(); } });
    K.bind({ key: '?', desc: 'this sheet', fn: function () { H.openKeys(); } });
    K.bind({ key: 'y', desc: 'resync from snapshot', fn: function () { Desk.act.resync(); } });
    K.bind({ key: 'd', desc: 'density compact/normal', fn: function () { Desk.ui.cycleDensity(); } });
    K.bind({ key: 'c', desc: 'colour-blind palette', fn: function () { Desk.ui.toggleCB(); } });
    K.bind({ key: 'f', desc: 'FLASH off / subtle / full (also in settings)', fn: function () { Desk.ui.cycleFlash(); } });
    K.bind({ key: 'Escape', desc: 'close sheet/drawer, leave a field', doc: true });

    R.add({ id: 'header', tab: null, fn: renderHeader });
    R.add({ id: 'status', tab: null, fn: renderStatus });
    R.add({ id: 'set', tab: null, fn: renderSet });
    R.add({ id: 'set-ui', tab: null, fn: renderSetUi });
    R.add({ id: 's-log', tab: 'systematic', fn: renderSysLog });
  }
})(typeof window !== 'undefined' ? window : globalThis);
