// The nine category switches are four-way routes (auto | manual | research | off), not checkboxes,
// and nothing on the page may read a route as a boolean ('manual', 'research' and 'off' are all
// truthy strings; the explainer and the START LIVE confirmation once listed them as algo sources).
// No browser needed: this reads ui/index.html and checks the field table, the renderer and the
// read-back path agree, and it evaluates the one expression that has to migrate a stored boolean.
const fs = require('fs'), path = require('path'), assert = require('assert');
const html = fs.readFileSync(path.join(__dirname, '..', 'ui', 'index.html'), 'utf8');
let checks = 0, fails = 0;
const check = (cond, what) => { checks++; if (!cond) { fails++; console.log('FAIL: ' + what); } };

const CATEGORIES = ['weather','sports','crypto','commodities','economics','finance',
                    'tech_science','other','arbitrage'];

// 1. every category is a route field, and none is still a checkbox
for (const k of CATEGORIES) {
  const re = new RegExp(`\\['${k}','[^']*','route'`);
  check(re.test(html), `${k} is declared as a route field`);
  check(!new RegExp(`\\['${k}','[^']*','bool'`).test(html), `${k} is no longer a bool field`);
}

// 2. the manual lane has its own floor, and it is a selection-side setting
check(/\['manual_min_edge','[^']*','num',0,0\.5/.test(html), 'manual_min_edge is a selection field');

// 3. the renderer emits the three options and the read-back path accepts a route
check(/else if \(type === 'route'\)/.test(html), 'the renderer has a route branch');
check(/\['auto','auto'\],\['manual','to me'\],\['research','research'\],\['off','off'\]/.test(html),
      'a route offers exactly auto / to me / research / off');
check(/type === 'style' \|\| type === 'stopmode' \|\| type === 'route'/.test(html),
      'readSystematicSettings sends a route through as its string');

// 4. the colour classes exist for all three, so the route is legible without reading the text
for (const r of ['auto','manual','research','off'])
  check(new RegExp(`select\\.route-${r}\\s*{`).test(html), `route-${r} has a colour`);
check(/el\.className = 'route route-' \+ el\.value/.test(html), 'changing a route repaints its colour');

// 5. the renderer reads the stored value through routeOf (checked in 6), which migrates a boolean:
//    a settings row written before routes existed holds one, and true/false keep their meaning.
check(/const r = routeOf\(v\);/.test(html), 'the renderer migrates a stored value through routeOf');
check(!/\(v \|\| 'off'\)/.test(html), "no `v || 'off'`: an unknown route string must not select nothing (and so 'auto')");
check(html.indexOf('const routeOf =') < html.indexOf('function initializeSystematicControls'),
      'routeOf is defined before the controls that use it');

// 6. no route is read as a truthy value, and the one reader behaves.
check(!/filter\(\s*k\s*=>\s*set\[k\]\s*\)/.test(html), 'no category list is built from truthy route values');
check(!/\$\{set\.arbitrage\s*\?/.test(html), 'arbitrage is not tested as a boolean');
check((html.match(/routeOf\(set\.arbitrage\)\s*===\s*'auto'/g) || []).length >= 2,
      'the explainer and the START LIVE confirmation both ask routeOf(...) === auto');
const ro = html.match(/const routeOf = (v => [\s\S]*?: 'off'\));/);
check(!!ro, 'routeOf is defined');
if (ro) {
  const routeOf = eval(ro[1]);
  check(routeOf('auto') === 'auto', 'auto is auto');
  for (const v of ['manual','research','off']) check(routeOf(v) === v && routeOf(v) !== 'auto', `${v} is not auto`);
  check(routeOf(true) === 'auto' && routeOf(false) === 'manual', 'booleans keep their pre-route meaning');
  check(routeOf('sometimes') === 'off' && routeOf(undefined) === 'off', 'anything unrecognised is off, never auto');
}
// 7. the research verdict is shown, and counted, on the Systematic tab
check(/\['research', 'research', counts\.research\|\|0\]/.test(html), 'the screen counts research rows');
check(/v==='research'\?/.test(html), 'a research row says why nobody is offered it');

// 8. The Trades board reads the router's decisions and never recomputes a route (session 15).
const trades = html.slice(html.indexOf('function tradesModel()'), html.indexOf('function renderTrades()'));
check(trades.length > 0, 'the board model is found');
check(/api\('\/api\/decisions\?research=1/.test(html), 'the board fetches /api/decisions');
check(!/api\/systematic\?limit=0'\)\.catch/.test(html.slice(html.indexOf('async function refreshTrades'), html.indexOf('function tradesModel()'))),
      'the board no longer rebuilds itself from the full screen');
check(!/\.verdict/.test(trades), 'the board never reads the old verdict to decide a row');
check(!/hours_to_settlement\s*<=\s*48/.test(trades), 'the horizon is the router\'s, not the page\'s');
check(!/kelly|executable_edge\s*[<>]/.test(trades), 'the board neither re-ranks nor re-thresholds decisions');
check(/route === 'auto' && !showAuto/.test(trades) && /route === 'research' && !showResearch/.test(trades),
      'AUTO and RESEARCH are view filters on the backend route');
check(/if \(st\.decision_id\) payload\.decision_id = st\.decision_id;/.test(html), 'the order names the decision it was reviewed on');
check(/decision_id: r\.decision \? \(row\.decision_id \|\| ''\) : ''/.test(html), 'only a Trades-board row carries a decision into the dialog');
check(/r\.decision && r\.route !== 'manual'/.test(html), 'AUTO and RESEARCH rows open no order dialog');
const modal = html.slice(html.indexOf('function renderKalshiModal()'), html.indexOf('async function sendKalshiModal()'));
check(!/p\s*-\s*st\.price\s*-\s*fee/.test(modal), 'the dialog does not compute an edge of its own');
for (const k of ['model p(win)', 'σ (uncertainty)', 'market price', 'raw edge', 'slippage estimate', 'uncertainty deduction',
                 'executable edge', 'suggested size', 'max payout', 'max loss'])
  check(modal.includes(k), `the dialog shows ${k}`);

console.log(`${checks} checks, ${fails} failures`);
process.exit(fails === 0 ? 0 : 1);
