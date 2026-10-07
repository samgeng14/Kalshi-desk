// What deskd served, checked by ui/desk/js/contract.js's own validator: the contract file is
// the single statement of the API, so the backend is tested against it, not against a port.
//   node tests/deskd/contract_check.cjs <capture.json>
// capture.json: {"snapshots": [snapshot...], "events": [envelope...],
//                "responses": [{"key": "POST /api/kill", "status": 200, "json": {...}}...],
//                "requests": [{"key": "POST /api/order", "body": {...}, "accepted": true|false}...],
//                "queries": [{"key": "GET /api/universe", "path": "/api/universe?day=T", "accepted": ...}]}
// A request or query with "accepted" is one deskd was sent: deskd's verdict (false: it answered
// 400 BAD_REQUEST) must be contract.js's (validateRequest / validateQuery found no error), so the
// C++ checker (web/contract.cpp check_request) cannot drift from this file. Without "accepted" a
// request must simply be valid.
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
function verdict(what, errs, accepted) {
  if (accepted === undefined) return report(what, errs);
  const valid = errs.length === 0;
  report(what, valid === accepted ? [] : ['deskd ' + (accepted ? 'accepted' : 'refused') + ' it, contract.js says ' +
    (valid ? 'valid' : 'invalid (' + errs.slice(0, 2).join('; ') + ')')]);
}
(cap.snapshots || []).forEach((s, i) => report('snapshot #' + i, C.validate(C.snapshot, s)));
(cap.events || []).forEach((e) => report('event ' + e.type + ' #' + e.seq, C.validateEvent(e)));
(cap.responses || []).forEach((r) => {
  if (!C.endpoints[r.key]) { report(r.key, ['no such endpoint in the contract']); return; }
  report(r.key + ' ' + r.status, C.validateResponse(r.key, r.status, r.json));
});
(cap.requests || []).forEach((r) => verdict('request ' + r.key + ' ' + JSON.stringify(r.body).slice(0, 80),
  C.validateRequest(r.key, r.body), r.accepted));
(cap.queries || []).forEach((q) => verdict('query ' + q.path, C.validateQuery(q.key, C.parseQuery(q.path)), q.accepted));
console.log('contract ' + C.version + ': ' + n + ' checked, ' + bad + ' failed');
process.exit(bad ? 1 : 0);
