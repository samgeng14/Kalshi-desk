/* ui/desk/js/manual.js
 *
 * The MANUAL tab (watchlist, ladders A/B/C, ticket, working, fills, positions).
 * STUB: the tab builder replaces this file. Contract with the foundation:
 *   Desk.manual.init()    called once at boot, after the header; bind keys and DOM events here
 *   Desk.manual.render(d) called on animation frames while the MANUAL tab is visible and
 *                         something is dirty; d.has(topic), d.any(prefix), d.all (full redraw)
 */
(function (root) {
  'use strict';
  var Desk = root.Desk = root.Desk || {};
  Desk.manual = {
    init: function () {},
    render: function (d) { void d; }
  };
})(typeof window !== 'undefined' ? window : globalThis);
