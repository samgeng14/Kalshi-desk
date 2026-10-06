/* ui/desk/js/main.js
 *
 * Boot: restore the per-viewer settings, wire the keys and panel linking, initialise the header
 * and both tabs, pick the transport (demo from file:// or ?demo=1, live with ?live=1, otherwise
 * a health probe), connect the stream, take the snapshot, start the one 250 ms clock. Rendering
 * runs only from requestAnimationFrame when something is dirty.
 */
(function (root) {
  'use strict';
  var Desk = root.Desk;

  function safe(what, fn) {
    try { fn(); } catch (e) {
      Desk.render.errors++; Desk.render.lastError = what + ': ' + (e && e.message);
      if (root.console) console.error('[desk boot]', what, e);
    }
  }

  function boot() {
    if (Desk.booted) return;
    Desk.booted = true;
    safe('ui', function () { Desk.ui.load(); });
    safe('focus', function () { Desk.focus.load(); });
    safe('layout', function () { Desk.layout.init(); });
    Desk.keys.attach();
    Desk.focus.bindClicks();
    safe('header', function () { Desk.header.init(); });
    safe('manual', function () { if (Desk.manual && Desk.manual.init) Desk.manual.init(); });
    safe('systematic', function () { if (Desk.systematic && Desk.systematic.init) Desk.systematic.init(); });
    Desk.render.add({ id: 'manual', tab: 'manual', fn: function (d) { if (Desk.manual && Desk.manual.render) Desk.manual.render(d); } });
    Desk.render.add({ id: 'systematic', tab: 'systematic', fn: function (d) { if (Desk.systematic && Desk.systematic.render) Desk.systematic.render(d); } });
    // the stream carries the ladders on screen: follow focus and layout
    Desk.focus.on(function () { Desk.subs.sync(); });
    Desk.render.add({ id: 'subs', tab: null, fn: function (d) { if (d.has('layout')) Desk.subs.sync(); } });
    // the focused ladder carries data-focused="1" (1 px outline in desk.css)
    Desk.render.add({ id: 'ladfocus', tab: 'manual', fn: function (d) {
      if (!d.has('focus') && !d.has('layout') && !d.all) return;
      ['A', 'B', 'C'].forEach(function (s) { Desk.dom.setAttr(Desk.dom.$('m-lad-' + s.toLowerCase()), 'data-focused', Desk.focus.ladder === s ? '1' : null); });
    } });

    Desk.store.begin();
    Desk.tick.on(function (now) { Desk.store.tick(now); });
    Desk.tick.start();

    Desk.transport.choose().then(function (mode) {
      Desk.store.mode = mode;
      Desk.store.fetchSnapshot = function () { return Desk.transport.snapshot(); };
      Desk.transport.connect(
        function (env) { Desk.store.apply(env); },
        function (state, reconnect) { Desk.store.onTransport(state, reconnect); if (state === 'open' && reconnect) Desk.subs.reset(); });
      Desk.store.resync('BOOT');
      Desk.subs.reset();
      Desk.render.mark('hdr');
    }, function (e) {
      Desk.store.localAlert('CRIT', 'BOOT', 'transport: ' + (e && e.message));
    });
    Desk.render.flush();
  }

  Desk.boot = boot;
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot);
  else boot();
})(window);
