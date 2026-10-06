// What deskd served, checked by ui/desk/js/contract.js's own validator: the contract file is
// the single statement of the API, so the backend is tested against it, not against a port.
//   node tests/deskd/contract_check.cjs <capture.json>
// capture.json: {"snapshots": [snapshot...], "events": [envelope...],
//                "responses": [{"key": "POST /api/kill", "status": 200, "json": {...}}...]}
// Prints every error and exits 1 if there is any.
'use strict';
const fs = require('fs'), path = require('path'), vm = require('vm');

const ctx = { console };
ctx.window = ctx;
vm.createContext(ctx);
const src = path.join(__dirname, '..', '..', 'ui', 'desk', 'js', 'contract.js');
vm.runInContext(fs.readFileSync(src, 'utf8'), ctx, { filename: 'contract.js' });
const C = ctx.Desk.contract;

const cap = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
let n = 0, bad = 0;
function report(what, errs) {
  n++;
  if (errs.length) { bad++; console.log('FAIL ' + what + ': ' + errs.slice(0, 5).join('; ')); }
}
(cap.snapshots || []).forEach((s, i) => report('snapshot #' + i, C.validate(C.snapshot, s)));
(cap.events || []).forEach((e) => report('event ' + e.type + ' #' + e.seq, C.validateEvent(e)));
(cap.responses || []).forEach((r) => {
  if (!C.endpoints[r.key]) { report(r.key, ['no such endpoint in the contract']); return; }
  report(r.key + ' ' + r.status, C.validateResponse(r.key, r.status, r.json));
});
(cap.requests || []).forEach((r) => report('request ' + r.key, C.validateRequest(r.key, r.body)));
console.log('contract ' + C.version + ': ' + n + ' checked, ' + bad + ' failed');
process.exit(bad ? 1 : 0);
