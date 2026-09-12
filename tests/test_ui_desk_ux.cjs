// The operator-facing half of the desk: what can I trade, what will the algorithm take, why is
// nothing showing, and is the engine actually running. These were the four questions the board
// could not answer, and each one is now a specific structure in ui/index.html. No browser needed:
// this reads the page and checks the structures exist and cannot silently disagree with the
// engine. The rule that matters most is the last one - the browser must never decide a route.
const fs = require('fs'), path = require('path'), assert = require('assert');
const html = fs.readFileSync(path.join(__dirname, '..', 'ui', 'index.html'), 'utf8');
let checks = 0, fails = 0;
const check = (cond, what) => { checks++; if (!cond) { fails++; console.log('FAIL: ' + what); } };

// ---- 1. the four lanes exist, and FOR ME is the default ---------------------------------
check(/\['me','FOR ME'\],\['auto','AUTO'\],\['all','ALL'\],\['research','RESEARCH'\]/.test(html),
      'the board offers exactly FOR ME / AUTO / ALL / RESEARCH');
check(/let tradesRoute = store\.get\('tradesRoute'\) \|\| 'me'/.test(html),
      "the default lane is FOR ME, not ALL");
check(/data-trroute=/.test(html), 'the lane chips are clickable targets');
check(/store\.set\('tradesRoute'/.test(html), 'the chosen lane is remembered');

// ---- 2. the lane actually filters, and a pair/perp counts as something you trade ---------
check(/const routeOf = r => r\.kind === 'kalshi' \? r\.route : 'manual'/.test(html),
      'a pair or perp row is hand-traded, so it belongs under FOR ME');
check(/rows = rows\.filter\(routeOk\)/.test(html), 'the lane filter is applied to the rows');
// the category counts must be computed AFTER the lane filter or the chips contradict the table
check(html.indexOf('rows = rows.filter(routeOk)') < html.indexOf('const counts = {all: rows.length}'),
      'category counts are computed on the rows the lane actually shows');

// ---- 3. changing lane must not leave a stale category hiding everything ------------------
check(/function setTradesRoute/.test(html), 'there is one place that changes lane');
check(/tradesCat = 'all';\s*\n\s*tradesCatPinned = false;/.test(html),
      'changing lane clears the category narrowing');

// ---- 4. an empty board explains itself, from the SERVER's numbers ------------------------
check(/function emptyBoardExplanation/.test(html), 'the empty board has an explainer');
check(/NO MANUAL TRADES RIGHT NOW/.test(html), 'the empty FOR ME board says so in words');
check(/decisions\?\.counts \|\| \{\}/.test(html), 'the breakdown comes from the server decision counts');
check(/decisions\?\.reasons \|\| \[\]/.test(html), 'the reasons come from the server reason histogram');
check(/Top reasons nothing cleared/.test(html), 'the top reasons are listed');
check(/data-trfunnel="all"[\s\S]{0,120}SHOW ALL DECISIONS/.test(html),
      'SHOW ALL DECISIONS is offered and navigates');
check(/data-trfunnel="research"[\s\S]{0,120}SHOW RESEARCH/.test(html),
      'SHOW RESEARCH is offered and navigates');
// the old bare message must be gone: it was indistinguishable from a fault
check(!/nothing actionable right now: no fresh two-sided book/.test(html),
      'the old unexplained empty message is gone');

// ---- 5. stale decisions must not look like an empty board -------------------------------
check(/DECISIONS ARE STALE/.test(html), 'stale decisions are called stale, not empty');
check(/Last good update/.test(html), 'a stale board reports its last good update');

// ---- 6. the funnel makes the router visible ----------------------------------------------
check(/id="tr_funnel"/.test(html), 'the funnel strip exists');
check(/decisions\?\.funnel/.test(html), 'the funnel comes from the server, not from JS arithmetic');
for (const step of ['MARKETS', 'PRICED', 'EDGE', 'ACTIONABLE'])
  check(new RegExp(`'${step}'`).test(html), `the funnel has a ${step} step`);

// ---- 7. halted may never read as running -------------------------------------------------
check(/ALGORITHM HALTED/.test(html), 'a halted engine says HALTED on the board header');
check(!/'RUNNING · HALTED'/.test(html), "no pill says RUNNING and HALTED at once");
check(/const algo = alg\.halted \?/.test(html),
      'the board header tests halted BEFORE it tests the running switch');
check(/The algorithm is HALTED and is sending nothing/.test(html),
      'a halted engine states plainly that nothing is being sent');
check(/Manual orders are unaffected/.test(html),
      'a halt says what it does NOT stop, so the operator knows what they can still do');

// ---- 8. the ticket's side logic ----------------------------------------------------------
check(/class="sides"/.test(html), 'the ticket shows which side is the decision\'s');
check(/data-otherside=/.test(html), 'the other side is reachable');
check(/take \$\{other\.toUpperCase\(\)\} instead/.test(html), 'the other side is offered in words');
check(/button class="alt" id="md_other"/.test(html), 'the other side is a secondary control');
// taking the other side must NOT carry the decision's backing
check(/openTrade\(tk, sd, null, \{\}\)/.test(html),
      'the other side opens a fresh hand ticket rather than mutating the decision ticket');
check(/decision_id/.test(html) && !/st\.row\.side = /.test(html),
      'the ticket never flips its own side in place, which would keep the decision_id');

// ---- 9. the edge breakdown is still whole ------------------------------------------------
for (const part of ['raw edge', 'fee', 'slippage estimate', 'uncertainty deduction', 'executable edge'])
  check(html.includes(part), `the ticket still itemises ${part}`);

// ---- 10. the browser must not decide a route --------------------------------------------
// Any expression that computes a route from edges/thresholds in JS is a second opinion, and a
// second opinion is how a screen starts disagreeing with the engine it reports on.
check(!/route\s*=\s*['"](auto|manual)['"]\s*;/.test(html.replace(/data-route=/g, '')),
      'no JS assigns a route literal to a row');
check(!/executable_edge\s*[><]=?\s*.*\?\s*['"]manual['"]/.test(html),
      'no JS derives MANUAL from an edge comparison');


// ---- 11. the selected row is explained in one panel --------------------------------------
check(/id="detailpanel"/.test(html), 'the right column has a detail panel');
check(/function renderDetail/.test(html), 'there is one renderer for the selected row');
check(/function selectedBoardRow/.test(html), 'the panel reads the board selection, not its own state');
check(/let detailKey = null/.test(html), 'the selection survives a repaint');
// the panel must follow the selection from BOTH ways of moving it
check(/detailKey = rows\[i\]\.dataset\.trade;\s*\n\s*renderDetail\(\);/.test(html),
      'j/k moves the panel');
check(/detailKey = tr\.dataset\.trade;\s*\n\s*renderDetail\(\);/.test(html),
      'clicking a row moves the panel');
check(/function keepSelection/.test(html),
      'a repaint re-applies the selection instead of losing it to innerHTML');

// ---- 12. the edge waterfall, and its self-check -------------------------------------------
check(/function edgeWaterfall/.test(html), 'the waterfall is its own function');
for (const step of ['model fair', 'raw discrepancy', 'slippage estimate', 'NET EDGE'])
  check(html.includes(step), `the waterfall shows ${step}`);
// the panel must not present a column that does not add up
check(/const resid = /.test(html), 'the waterfall computes a residual');
check(/unexplained/.test(html), 'a waterfall that does not reconcile says so');
check(/Math\.abs\(resid\) > 0\.0005/.test(html), 'the residual is only shown when it is real');

// ---- 13. the buttons ----------------------------------------------------------------------
check(/id="d_buy"/.test(html), 'the panel has a primary buy button');
check(/const tradable = r\.kind !== 'kalshi' \|\| route === 'manual'/.test(html),
      'tradable is the route, not a guess');
check(/\$\{tradable\?'':'disabled'\}/.test(html),
      'the buy button is disabled on anything that is not yours to trade');
// a disabled button must say WHY, or the operator is left guessing
check(/it is not offered to you[\s\S]{0,200}Recorded, offered to nobody/.test(html),
      'a disabled buy button explains itself per route');
check(/id="d_other"/.test(html), 'the other side is reachable from the panel');
check(/openTrade\(r\.ticker, other\.toLowerCase\(\), null, \{\}\)/.test(html),
      'the panel\'s other-side button also opens a plain hand ticket');

// ---- 14. the why-not stack ---------------------------------------------------------------
check(/Why it is on this route/.test(html), 'every selected row shows its reason stack');
check(/row\.reasons \|\| r\.reasons/.test(html), 'the reasons come from the row, not from JS');

// ---- 15. ownership is scannable ----------------------------------------------------------
check(/<th class="l" title="who owns the automatic exit/.test(html),
      'the positions table leads with an owner column');
check(/\['MIXED','man'\]/.test(html) && /\['AUTO','bot'\]/.test(html) && /\['ME','man'\]/.test(html),
      'owner is one of ME / AUTO / MIXED');
check(/never from the quantities/.test(html),
      'ownership is derived from the reported manager, not guessed from the size split');

// ---- 16. a dead backend must not look live -----------------------------------------------
check(/classList\.add\('offline'\)/.test(html), 'losing the desk marks the page offline');
check(/everything below is as of/.test(html), 'an offline page stamps its last good values');


// ---- 17. a second send queues instead of being refused ----------------------------------
// The old behaviour toasted "an order request is still pending" and dropped the send. Queuing it
// is a change to the ORDER PATH, so the properties that keep it safe are asserted here rather
// than left to reading.
check(/const MAX_QUEUED = 5/.test(html), 'the queue has a hard depth cap');
check(/let sendQueue = \[\], queueHalted = ''/.test(html), 'there is one queue and one halt reason');
check(/return st\.fromQueue \? undefined : enqueueSend\(st\)/.test(html),
      'a send while one is in flight enqueues; a drained item does not re-enqueue itself');
check(!/An order request is still pending/.test(html),
      'the old refusal message is gone');

// the queued review timestamp must be the operator's, not the departure time, or the desk's
// 90-second unsent-review expiry silently stops meaning anything
check(/reviewed_ts:Math\.floor\(Date\.now\(\)\/1000\)/.test(html), 'queueing stamps the review time');
check(/reviewed_ts:st\.reviewed_ts\|\|Math\.floor\(Date\.now\(\)\/1000\)/.test(html),
      'a queued send sends the time it was REVIEWED, not the time it left');

// a queued order must go out through the ordinary path so it keeps every check
check(/await sendKalshiModal\(st\)/.test(html), 'the drain reuses the normal send path');
check(/nothing about a queued order[\s\S]{0,80}bypasses a check/.test(html),
      'the drain documents that it bypasses no check');

// one entry decision per event: a double-press must not become two orders
check(/if\(sendQueue\.some\(q=>q\.key===key\)\)/.test(html), 'the same event cannot be queued twice');

// a failure must not blast the rest of the queue into whatever broke
check(/if\(st\.error&&sendQueue\.length\)queueHalted=st\.error/.test(html),
      'the drain halts on the first failure');
check(/if\(!st\.error\)drainSendQueue\(\)/.test(html), 'only a clean result advances the queue');
check(/data-requeue="go"/.test(html), 'a halted queue can be resumed deliberately');
check(/data-unqueue="all"/.test(html), 'a halted or long queue can be discarded');

// the queue has to be visible and cancellable, or it is a hidden order pipeline
check(/function renderSendQueue/.test(html), 'the queue renders');
check(/queued behind it/.test(html), 'the banner says how many are waiting');
check(/Closing the dialog does not cancel it/.test(html),
      'the banner states what closing the dialog does not do');
check(/only removes an item that has NOT been sent/.test(html),
      'cancel is documented as applying to unsent items only');

// ---- 18. one dialog submitting must not freeze another ----------------------------------
check(/const submittingThis = \(\) => modalState !== null && modalState === submittingState/.test(html),
      'the lock is per dialog, not global');
check(/function lockModalControls\(\) \{ if\(submittingThis\(\)\)/.test(html),
      'only the submitting dialog has its controls locked');
check(/\$\('#md_send'\).addEventListener\('click', \(\) => sendKalshiModal\(\)\)/.test(html),
      'the send listener is wrapped so the MouseEvent is not read as a queued ticket');
// opening the next ticket is how something gets queued, so it must not be refused
check(/Deliberately NOT refused while a send is in flight/.test(html),
      'a pending send no longer blocks opening the next ticket');

// ---- 19. the ladder must survive a partial book ------------------------------------------
check(/const B = L\.book \|\| \{\};/.test(html), 'the ladder guards a missing book');
check(!/L\.book\.yes_bids/.test(html), 'no unguarded read of book.yes_bids');


// ---- 20. a row with no model must not get a fabricated waterfall -------------------------
// A perps/pair row is priced by its own rule from a dollar price, with no model fair and no
// per-contract fee terms. Run through the decomposition it produced "entry at -231.8c", a zero
// NET EDGE and an "unexplained" residual invented out of absent fields.
check(/const isDecision = r\.kind === 'kalshi' && Number\.isFinite\(Number\(row\.p_win \?\? r\.fair\)\)/.test(html),
      'the waterfall only runs on a row that actually has a model probability');
check(/No model probability on this row/.test(html),
      'a row without a model says so instead of showing a decomposition');
check(/const actionLabel = side === 'YES' \|\| side === 'NO' \? `BUY \$\{side\}` : \(r\.what \|\| 'TRADE'\)/.test(html),
      'the button names the actual action rather than "BUY --" on a sideless row');
check(/r\.kind==='kalshi'&&hasSide/.test(html),
      'the other-side control is only offered on a two-sided contract');

// ---- 21. the funnel steps must be comparable --------------------------------------------
// It read "MARKETS 2124 -> PRICED 4248": the first number was markets and the second was sides,
// two per market, so the funnel grew instead of narrowing.
const eng = require('fs').readFileSync(require('path').join(__dirname,'..','src','engine.cpp'),'utf8');
check(/std::set<std::string> priced_mkts, edge_mkts, auto_mkts, manual_mkts;/.test(eng),
      'the funnel counts distinct markets, not rows');
check(/priced_mkts\.insert\(tk\)/.test(eng) && /edge_mkts\.insert\(tk\)/.test(eng),
      'priced and edge are counted per market');
check(!/\{"priced", f_priced\}/.test(eng), 'the row-counting funnel fields are gone');
check(/\{"sides", jnum\(counts, "sides", 0\)\}/.test(eng),
      'the side count is still published, separately and named');


// ---- 22. no request may hang, because a hung one wedged the whole manual lane ----------
// A pair submit to a Polymarket whose account call 401s had NO deadline. fetch without a signal
// waits forever, so its `finally` never ran, `modalSubmitting` stayed true, and every later
// manual order was refused with "an order request is still pending" until the page was reloaded.
check(/const API_TIMEOUT_MS = \d+/.test(html), 'api() has a default deadline');
check(/if \(!o\.signal\) o\.signal = AbortSignal\.timeout\(API_TIMEOUT_MS\)/.test(html),
      'every request without its own signal gets the default deadline');
check(/err\.name === 'TimeoutError' \|\| err\.name === 'AbortError'/.test(html),
      'an aborted request is reported as the desk not answering, not as a bare abort');
check(/no answer from the desk in/.test(html), 'the timeout message names the deadline');
// the caller that actually hung
check(/post\('\/api\/poly\/pair', payload, \{signal: AbortSignal\.timeout\(10000\)\}\)/.test(html),
      'the pair submit now carries a deadline');
check(/const post = \(p, body, opts\) =>/.test(html),
      'post() forwards fetch options, or the pair deadline would be silently dropped');

// ---- 23. and if the flag sticks anyway, there is a way out -----------------------------
check(/let pendingWatchdog = null, pendingTick = null, pendingSince = 0;/.test(html),
      'there is a watchdog and a clock on the in-flight flag');
check(/pendingTick=setInterval\(renderSendQueue,1000\)/.test(html),
      'the pending banner repaints on its own clock, so the escape hatch appears even when\n       a blocking request has stopped every other render');
check(/pendingWatchdog=setTimeout\(/.test(html), 'the watchdog is armed when a submission starts');
check(/never reported back and was cleared after 30s/.test(html),
      'a stuck submission is cleared and the operator is told');
check(/data-unwedge="1"/.test(html), 'the operator can stop waiting manually');
check(/It does NOT cancel the order/.test(html),
      'stopping waiting states plainly that it does not cancel the order');
check(/check the manual queue and the blotter/.test(html),
      'the operator is told where to check before re-sending');

// ---- 24. a pair is not offered without a placeable second leg --------------------------
check(/const pmLive = !!\(pnl\?\.polymarket\?\.synced\);/.test(html),
      'pair rows are gated on the Polymarket account being reachable');
check(/if \(!pmLive\) continue;/.test(html), 'an unreachable second leg removes the row');
check(/whose trade button can only fail/.test(html),
      'the gate documents why an unplaceable row must not be listed');

console.log(`${checks} checks, ${fails} failures`);
process.exit(fails ? 1 : 0);
