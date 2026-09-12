// Browser regression checks against fixture APIs only. No server, account, or API key needed.
// Run with Playwright on NODE_PATH (or installed locally):
//   NODE_PATH=/path/to/node_modules node tests/test_dashboard.cjs
// Uses installed Chrome. Every browser request is intercepted; unexpected requests fail closed.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {chromium} = require('playwright');

const html = fs.readFileSync(path.join(__dirname, '../ui/index.html'), 'utf8');
const now = Math.floor(Date.now() / 1000);
function candidate(ticker, overrides = {}) {
  return {
    ticker, event_ticker: ticker.split('-').slice(0, -1).join('-'), series_ticker: ticker.split('-')[0],
    title: 'Fixture market, no real order', category: 'weather', side: 'yes',
    p_win: .72, p_raw_win: .74, p_yes: .72, sigma: .03, bid: .59, ask: .62,
    bid_size: 10, ask_size: 10, spread: .03, fee_type: 'quadratic', fee_multiplier: 1,
    tick: .01, style: 'maker', price: .60, fee: 0, edge: .12, buffered_edge: .105,
    kelly: .26, model_gap: .10, minutes_to_close: 120, settlement_ts: now + 7200,
    hours_to_settlement: 2, fair_strategy: 'fixture_model', verdict: 'manual',
    reasons: ['fixture estimate'], warnings: [], inputs_valid: true, quote_ts: now, fair_ts: now,
    ...overrides,
  };
}
// A decision row as GET /api/decisions returns it: the screen's fields plus the router's.
let nextDecision = 0;
function decision(ticker, overrides = {}) {
  const id = 'd' + (0xa000 + (++nextDecision)).toString(16).padStart(16, '0');
  return candidate(ticker, {route: 'manual', decision_id: id, rank: nextDecision, model: 'fixture_model', model_version: 'v1',
    model_key: 'fixture_model_v1', raw_edge: .12, executable_edge: .105, slippage: 0, uncertainty_buffer: .015,
    ev_per_contract: .12, event_rows: 1, settlement_verified: true, auto_eligible: true, ...overrides});
}
function fixtures(rows) {
  return {
    decisions: {generated_ts: now, rows, algorithm: {running: false, mode: 'paper', auto_armed: false, auto_off_reason: 'the algorithm is stopped: offered to you instead'},
      counts: {manual: rows.filter(r => r.route === 'manual').length, algo: rows.filter(r => r.route === 'auto').length},
      floors: {min_edge: .03, manual_min_edge: .05, uncertainty_buffer: .5, slippage_estimate: 0}, market_data_max_age_seconds: 25},
    state: {
      env: 'demo', armed: true, account_ts: now, now, balance: 80, cycle: 1, last_cycle_ts: now,
      watch: [], pending: [], recent: [], orders: [], positions: [], fairs: [], strategies: [], events: [],
      config: {risk: {day_tz: 'America/Chicago'}}, perps: {enabled: false},
    },
    systematic: {
      settings: {
        weather: true, sports: true, crypto: true, other: false, max_model_gap: .15,
        min_win_probability: .60, min_edge: .03, max_edge: .15, uncertainty_buffer: .5,
        max_spread: .05, max_horizon_hours: 48, daily_loss_stop_dollars: 5,
        max_open_dollars: 20, max_positions: 5, daily_budget_dollars: 10,
        model_routes: {weather_nws_v1: 'manual', sports_inplay_v1: 'research', ladder_arb: 'auto'}, graduated_models: [],
      },
      mode: 'paper', running: false, live_possible: false, settings_version: 1,
      screen: {generated_ts: now, rows, counts: {algo: 0, manual: rows.length, weak: 0, blocked: 0, sides: rows.length}},
      performance: {paper: {}, live: {}}, positions: [], history: [], log: [],
      execution_stats: {paper: {attempted: 3, filled: 1, unfilled: 2, cancel_reasons: {'entry TTL expired': 2}}},
    },
    perps: {
      enabled: true, armed: true, account_ts: now, public_ts: now, balance: {equity: 20, settled_funds: 20},
      positions: [], orders: [], markets: [], fills: [],
      rails: {max_order_notional: 50, max_total_notional: 100, max_leverage: 3, price_band_pct: 2},
      fees: {maker_fee_rates: {KXTESTPERP: .0005}, taker_fee_rates: {KXTESTPERP: .0012}},
      algo: {running: false, paper: true, settings: {markets: ['KXTESTPERP'], entry_bps: 25, stop_bps: 100, exit_bps: 5, contracts: 1}, positions: [], history: []},
    },
    'account-summary': {
      events: {cash: 80, equity: 82}, perps: {cash: 20, equity: 19},
      combined: {cash: 100, equity: 101, pnl_today: -.78, pnl_total: null, pnl_recorded: -.90},
      as_of: now, complete: false, warnings: ['Fixture: funding history is incomplete'],
    },
    risk: {events: [], clusters: [], daily: {}, open_exposure: 0, open_cap: 50},
    'manual-orders': [],
    'sports-models': {candidates:[],live_enabled:false},
    'pair-models': {running:false,per_pair_dollars:6,min_edge:.01,hold_seconds:300,positions:[],pending:[],candidates:[],history:[],pnl:0},
    'model-lab': {schema:'model-lab-v1',generated_ts:now,version:'fixture',models:[{name:'weather_calibration',status:'insufficient_independent_data',objective:'Fixture research model',groups:5,rows:20,days:2,live_enabled:false}],live_enabled:false},
    'risk-controls': {account_stop:{enabled:true,loss_fraction:.1},daily:{pv:80,stop_pv:72},kill:false,kill_reason:''},
    pnl: {positions: [], total: 0, settled_today: 0, unrealized: 0},
    auto: {auto_armed: false, instant_edge: 0, spent_today: 0, budget: 10},
    scorecard: {}, blotter: [], brackets: [], board: {markets: [], total: 0}, 'poly/pairs': [],
    book: {running: false, lanes_on: 0, lanes: [], for_you: rows.filter(r => r.route === 'manual').length, blocked: 0, research: 0,
           best_edge: 0, best_ticker: '', screen_ts: now, kill: false, kill_reason: ''},
    crossvenue: {
      settings: {enabled: true, total_budget: 100, kalshi_budget: 0, polymarket_budget: 0, min_net_edge: .02, slippage_per_leg: .01, max_quote_age_seconds: 15, max_quote_skew_seconds: 5, max_horizon_hours: 48},
      status: {}, polymarket: {connected: true, credentials_configured: false, markets: [], account: {}},
      pairs: [], candidates: [], observations: {samples: 0}, now,
    },
  };
}

async function openFixture(browser, data) {
  const context = await browser.newContext({viewport: {width: 1440, height: 1100}, serviceWorkers: 'block'});
  const page = await context.newPage(), errors = [], requests = [], writes = [], unexpected = [];
  page.on('pageerror', e => errors.push(e.message));
  await context.route('**/*', async route => {
    const req = route.request(), url = new URL(req.url());
    if (url.origin !== 'http://localhost') { unexpected.push(req.url()); return route.abort('blockedbyclient'); }
    if (url.pathname === '/') return route.fulfill({contentType: 'text/html', body: html});
    if (!url.pathname.startsWith('/api/')) return route.fulfill({status: 404, body: ''});
    const endpoint = url.pathname.slice(5), method = req.method();
    requests.push({endpoint, method, search: url.search});
    if (method !== 'GET') {
      const body = req.postDataJSON(); writes.push({endpoint, body});
      // Keep the request open long enough for the second click to hit the duplicate guard.
      await new Promise(resolve => setTimeout(resolve, 180));
      if(endpoint==='manual-orders'&&body.decision_id&&(data.refuse||{})[body.decision_id])return route.fulfill({status:400,contentType:'application/json',body:JSON.stringify({error:data.refuse[body.decision_id]})});
      if(endpoint==='manual-orders'){const r={id:body.client_request_id,ticker:body.ticker,event_ticker:body.ticker.split('-').slice(0,-1).join('-'),status:'queued',payload:JSON.stringify(body),result:'{}',error:'',created_ts:now,expires_ts:now+90};data['manual-orders'].unshift(r);return route.fulfill({contentType:'application/json',body:JSON.stringify(r)});}
      return route.fulfill({contentType: 'application/json', body: JSON.stringify({status: 'submitted', order_id: 'fixture-only', fill_count: 0, remaining_count: 1})});
    }
    let result = data[endpoint];
    if (endpoint === 'size') {
      const price = Number(url.searchParams.get('price')), style = url.searchParams.get('style'), did = url.searchParams.get('decision_id');
      const row = did ? data.decisions.rows.find(r => r.decision_id === did) : null;
      const edge = row ? {fair_probability: row.p_win, raw_fair_probability: row.p_raw_win, entry_price: price, entry_fee: 0, estimated_slippage: 0,
                          uncertainty_buffer: row.uncertainty_buffer, raw_edge: row.p_win - price, net_edge: row.p_win - price,
                          executable_edge: row.p_win - price - row.uncertainty_buffer, ev_per_contract: row.p_win - price, executable: true,
                          style, bid: row.bid, ask: row.ask, sigma: row.sigma, model: row.model_key} : null;
      result = {count: 3, fee_per: 0, cost_per: price, basis: 'risk caps', cash: 80, cap_event: 10, cap_cluster: 25, price, style, edge,
                decision: did ? (row ? {decision_id: did, found: true, route: row.route, reasons: row.reasons, manual_min_edge: .05,
                                        clears_floor: edge.executable_edge >= .05} : {decision_id: did, found: false, reason: 'this row is no longer on the board'}) : null};
    }
    if (endpoint === 'hedge') result = {options: [], note: 'No fixture hedge', have_structure: false};
    if (endpoint.startsWith('ladder/')) {
      const ticker = decodeURIComponent(endpoint.slice('ladder/'.length));
      const row = data.systematic.screen.rows.find(r => r.ticker === ticker) || candidate(ticker);
      result = {market: {...row, yes_bid: .59, yes_ask: .62}, book: {yes_bids: [[.59, 10]], no_bids: [[.38, 10]]}, orders: [], pending: [], position: null};
    }
    if (endpoint.startsWith('perps/book/')) result = {best_bid: 10.04, best_ask: 10.06, bids: [[10.04, 10]], asks: [[10.06, 10]], bid_size: 10, ask_size: 10};
    if (endpoint === 'markets') result = data.systematic.screen.rows.map(r => ({...r, yes_bid: .59, yes_ask: .62}));
    if (result === undefined) { unexpected.push(endpoint); result = []; }
    return route.fulfill({contentType: 'application/json', body: JSON.stringify(result)});
  });
  await page.goto('http://localhost/');
  await page.waitForFunction(() => document.querySelector('#balance').textContent === '$100.00');
  await page.waitForFunction(() => document.querySelector('#trades_count').textContent.includes('actionable'));
  if(errors.length)throw Error(errors.join(';')); 
  return {context, page, errors, writes, requests, unexpected};
}

async function run() {
  const browser = await chromium.launch({headless: true, channel: 'chrome'});
  try {
    // The desk's decisions, as the router published them. The page shows exactly these: the ranks,
    // the routes and the one-row-per-event folding are the backend's, and so is ownership (the held,
    // queued and resting events are simply not in the list). The addition on the event you hold is
    // listed because the router offered it; the page does not second-guess that from its own state.
    const auto = decision('AUTOFIX-26SEP09-A', {route: 'auto', verdict: 'algo', category: 'crypto', kelly: .40, reasons: []});
    const eligible = decision('UIFIX-26SEP09-A', {event_rows: 2, reasons: ['weather is set to manual: priced for you, never sent']});
    const addition = decision('KXNCAAFGAME-26SEP09WISND-WIS', {category: 'sports', title: 'Wisconsin', verdict: 'algo',
      reasons: ['an addition to the position you hold here (qualified): it adds concentration, so it is yours to review']});
    const research = decision('RESFIX-26SEP09-A', {route: 'research', verdict: 'research', rank: null,
      reasons: ['settlement not verified: what this contract settles on has not been checked against fixture_model_v1']});
    const data = fixtures([auto, eligible, addition, research]);
    data.state.positions=[{ticker:'KXNCAAFGAME-26SEP09WISND-WIS',position:2}];
    data.state.orders=[{ticker:'REST-26SEP09-HELD',outcome_side:'yes',remaining:1,filled:0,yes_price:.5,order_id:'fixture-rest'}];
    data.state.pending=[{id:10,ticker:'QUEUE-26SEP09-HELD',event_ticker:'QUEUE-26SEP09',outcome_side:'yes',strategy:'fixture',exec_style:'maker',price:.5,count:1,fee_per:0,net_edge:.05,mode:'manual'}];
    const t=await openFixture(browser,data),page=t.page;
    const rows=page.locator('#trades tbody tr[data-trade]');
    const tickers=async()=>(await rows.evaluateAll(trs=>trs.map(tr=>tr.dataset.trade.split('|')[0])));
    assert.deepEqual(await tickers(),['AUTOFIX-26SEP09-A','UIFIX-26SEP09-A','KXNCAAFGAME-26SEP09WISND-WIS'],'the desk\'s rows in the desk\'s order; RESEARCH behind its filter');
    assert.deepEqual(await rows.evaluateAll(trs=>trs.map(tr=>tr.cells[0].textContent.trim())),['1','2','3'],'the rank is the desk\'s');
    assert.deepEqual(await rows.evaluateAll(trs=>trs.map(tr=>tr.dataset.route)),['auto','manual','manual'],'the route is the desk\'s, whatever the row\'s old verdict says');
    assert.match(await rows.nth(0).innerText(),/AUTO/);assert.equal(await rows.nth(0).locator('button').count(),0,'an AUTO row offers no trade button');
    assert.match(await rows.nth(1).innerText(),/MANUAL/);assert.equal(await rows.nth(1).locator('button[data-tradebtn]').count(),1);
    assert.match(await rows.nth(1).innerText(),/best of 2 in this event/);
    for (const h of ['#','market','side','price','fair','σ','raw edge','net edge','model','route','time left'])
      assert.ok((await page.locator('#trades thead th').allTextContents()).includes(h),'column '+h);
    assert.ok(t.requests.some(r=>r.endpoint==='decisions'&&r.search==='?research=1&limit=200'),'the board reads the desk\'s decisions');
    assert.ok(!t.requests.some(r=>r.endpoint==='systematic'),'the board no longer rebuilds itself from the full screen');
    await page.locator('#tr_research').check();
    assert.deepEqual(await tickers(),['AUTOFIX-26SEP09-A','UIFIX-26SEP09-A','KXNCAAFGAME-26SEP09WISND-WIS','RESFIX-26SEP09-A'],'RESEARCH shows behind its filter');
    assert.match(await rows.nth(3).innerText(),/RESEARCH/);assert.equal(await rows.nth(3).locator('button').count(),0);
    await rows.nth(3).dblclick();assert.equal(await page.locator('#modal').count(),0,'a RESEARCH row opens no order dialog');
    await page.locator('#tr_research').uncheck();await page.locator('#tr_algo').uncheck();
    assert.deepEqual(await tickers(),['UIFIX-26SEP09-A','KXNCAAFGAME-26SEP09WISND-WIS'],'AUTO rows can be hidden');
    await page.locator('#tr_algo').check();
    await rows.nth(0).dblclick();assert.equal(await page.locator('#modal').count(),0,'an AUTO row opens no order dialog');
    await page.locator('#tr_minwin').fill('80');await page.locator('#tr_minwin').dispatchEvent('change');
    assert.equal(await rows.count(),0,'p(win) filter is a view filter');
    await page.locator('#tr_minwin').fill('0');await page.locator('#tr_minwin').dispatchEvent('change');
    assert.equal(t.writes.length,0);
    assert.equal(await page.locator('#combined_equity').textContent(),'$101.00');
    assert.equal(await page.locator('#balance').textContent(),'$100.00');
    assert.match(await page.locator('#pnl_total_label').innerText(),/recorded/i);
    assert.match(await page.locator('#manual_ownership').innerText(),/manually managed/);
    await rows.nth(1).dblclick();await page.waitForSelector('#md_send');
    await page.waitForFunction(()=>document.querySelector('#modal_body').textContent.includes('executable edge'));
    assert.equal(t.writes.length,0,'double click only opens review');
    const modalText=await page.locator('#modal').innerText();
    assert.match(modalText,/manual order/);
    assert.match(modalText,/scenario only/);
    // The breakdown is the desk's, for the decision on the row (p(win), sigma, price, raw edge, fee,
    // slippage, the uncertainty deduction, executable edge, size and the route's reason).
    for (const k of [/model p\(win\)\s*72\.0%/i,/σ \(uncertainty\)\s*0\.030/i,/market price\s*0\.60/i,/raw edge\s*\+0\.120/i,/fee\s*-?0\.000/i,
                     /slippage estimate/i,/uncertainty deduction\s*-0\.015/i,/executable edge\s*\+0\.105/i,/manual floor 0\.05: clears it/i,
                     /suggested size\s*3 contracts/i,/route\s*MANUAL/i,/weather is set to manual/i,/max payout/i,/max loss/i])
      assert.match(modalText,k,'dialog shows '+k);
    const sizeReq=t.requests.filter(r=>r.endpoint==='size').pop();
    assert.match(sizeReq.search,new RegExp('decision_id='+eligible.decision_id),'the dialog asks the desk to price this decision');
    await page.locator('#md_send').evaluate(button=>{button.click();button.click();});
    await page.waitForSelector('#modal',{state:'detached',timeout:5000}).catch(async e=>{throw Error(e.message+' MODAL='+await page.locator('#modal').innerText()+' ERRORS='+t.errors.join(';'));});
    const sent=t.writes.filter(w=>w.endpoint==='manual-orders');
    assert.equal(sent.length,1,'exactly one submission');
    assert.equal(sent[0].body.decision_id,eligible.decision_id,'the order names the decision it was reviewed on');
    assert.equal(sent[0].body.ticker,'UIFIX-26SEP09-A');
    assert.deepEqual(await tickers(),['AUTOFIX-26SEP09-A','KXNCAAFGAME-26SEP09WISND-WIS'],'submitted event immediately suppressed');
    await page.locator('[data-tab="systematic"]').click();await page.waitForSelector('#sys_patient',{state:'visible'});
    await page.locator('#sys_patient').click();
    assert.equal(await page.locator('#sys_setting_capital_budget_dollars').inputValue(),'40');
    assert.equal(await page.locator('#sys_setting_stop_loss_fraction').inputValue(),'70');
    assert.equal(await page.locator('#sys_setting_model_value_exit').isChecked(),false);
    assert.equal(await page.locator('#sys_setting_thesis_stop_enabled').isChecked(),false);
    assert.equal(await page.locator('#sys_setting_daily_loss_stop_enabled').isChecked(),false);
    assert.equal(t.writes.length,1,'preset preview changes no server setting and starts no algo');
    const capBefore=await page.locator('#sys_setting_capital_budget_dollars').inputValue();await page.locator('#sys_value').click();assert.equal(await page.locator('#sys_setting_per_trade_dollars').inputValue(),'6');assert.equal(await page.locator('#sys_setting_sports_per_trade_dollars').inputValue(),'2');assert.equal(await page.locator('#sys_setting_min_win_probability').inputValue(),'0.6','the 60% preset is a probability, not a percent');assert.equal(await page.locator('#sys_setting_capital_budget_dollars').inputValue(),capBefore);
    // Model routes are edited here, inside their category's route; graduating a model asks first,
    // and a dismissed question applies nothing.
    assert.equal(await page.locator('#sys_models [data-modelroute="weather_nws_v1"]').inputValue(),'manual');
    assert.equal(await page.locator('#sys_models [data-modelroute="sports_inplay_v1"]').inputValue(),'research');
    assert.match(await page.locator('#sys_line').innerText(),/routed auto and graduated \(none yet\)/);
    await page.locator('#sys_models [data-graduated="weather_nws_v1"]').check();
    // The fixture carries only the settings the other checks read; give the rest a valid value so
    // Apply reaches the question instead of stopping at an empty field.
    await page.evaluate(()=>{for(const el of document.querySelectorAll('input[data-syssetting][type=number]'))if(el.value===''||!el.checkValidity())el.value=el.min||'1';});
    let graduationQuestion='';
    page.once('dialog',d=>{graduationQuestion=d.message();d.dismiss();});
    await page.locator('#sys_apply').click();
    await page.waitForTimeout(200);
    assert.match(graduationQuestion,/Graduate weather_nws_v1 to unattended execution/);
    assert.equal(t.writes.length,1,'a graduation you decline applies nothing');
    // The Systematic tab's Review opens the same decision dialog as the Trades board, and only on a
    // MANUAL row: an AUTO or RESEARCH row is not offered to you from here either.
    assert.equal(await page.locator('#sys_screen button[data-review="AUTOFIX-26SEP09-A"]').isDisabled(),true);
    await page.locator('#sys_screen button[data-review="KXNCAAFGAME-26SEP09WISND-WIS"]').click();
    await page.waitForSelector('#md_send');
    assert.match(await page.locator('#modal h3').innerText(),/MANUAL DECISION/i);
    assert.match(t.requests.filter(r=>r.endpoint==='size').pop().search,new RegExp('decision_id='+addition.decision_id));
    await page.keyboard.press('Escape');await page.waitForSelector('#modal',{state:'detached'});
    assert.match(await page.locator('#manual_queue').innerText(),/queued/);
    await page.locator('[data-tab="models"]').click();await page.waitForFunction(()=>document.querySelector('#models_cards').textContent.includes('weather_calibration'));
    assert.match(await page.locator('#models_cards').innerText(),/insufficient_independent_data/i);
    assert.equal(t.writes.length,1,'model lab never deploys or starts trading');
    // A ticker with no model still uses the same guarded review, without inventing a fair.
    // The $0.37 / 1.5-contract ticket values must survive the asynchronous size suggestion of 3.
    await page.locator('#t_ticker').fill('UNKNOWN-26SEP09-A');
    await page.locator('#t_side').selectOption('no');
    await page.locator('#t_style').selectOption('maker');
    await page.locator('#t_price').fill('0.37');
    await page.locator('#t_count').fill('1.5');
    await page.locator('#t_note').fill('fixture-only discretionary thesis');
    await page.locator('#t_submit').click();
    await page.waitForSelector('#md_send');
    await page.waitForFunction(()=>document.querySelector('#modal_body').textContent.includes('3 allowed'));
    assert.equal(t.writes.length,1,'Ticket button reviews without submitting');
    assert.equal(await page.locator('#md_price').inputValue(),'0.37');
    assert.equal(await page.locator('#md_count').inputValue(),'1.5');
    assert.equal(await page.locator('#md_style').inputValue(),'maker');
    assert.match(await page.locator('#modal h3').innerText(),/^NO UNKNOWN-/);
    assert.match(await page.locator('#modal .pl > div').nth(3).innerText(),/expected at p\(win\) -/i);
    assert.match(await page.locator('#modal').innerText(),/no model covers this contract/i,'a hand ticket without a model shows no fair');
    assert.equal(await page.locator('#modal .pl > div').nth(3).locator('b').innerText(),'-','unknown fair has no estimated EV');
    await page.screenshot({path:'/tmp/tradeit-ticket-preview.png',fullPage:false});
    await page.locator('#md_send').evaluate(button=>{
      button.click();
      // Escape dismisses a pending request without mutating or resubmitting the order.
      const qty=document.querySelector('#md_count');
      if(!qty.disabled)throw Error('quantity remained editable while sending');
      qty.value='99';qty.dispatchEvent(new Event('change',{bubbles:true}));
      document.dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true}));
      if(document.querySelector('#modal'))throw Error('Escape did not dismiss the pending dialog');
      if(document.querySelector('#order_pending').classList.contains('hidden'))throw Error('pending request was no longer tracked');
      button.click();
    });
    await page.waitForSelector('#modal',{state:'detached'});
    await page.waitForFunction(()=>document.querySelector('#order_pending').classList.contains('hidden'));
    const orders=t.writes.filter(w=>w.endpoint==='manual-orders');
    assert.equal(orders.length,2,'one POST for each separately reviewed order despite repeated clicks');
    const {client_request_id,reviewed_ts,...queuedBody}=orders[1].body;assert.match(client_request_id,/^[0-9a-f-]{36}$/);assert.equal(typeof reviewed_ts,'number');
    assert.deepEqual(queuedBody,{ticker:'UNKNOWN-26SEP09-A',outcome_side:'no',price:.37,count:1.5,post_only:true,note:'fixture-only discretionary thesis'});
    assert.deepEqual(t.errors,[]);assert.deepEqual(t.unexpected,[]);
    await page.screenshot({path:'/tmp/tradeit-sep9-dashboard.png',fullPage:false});
    await t.context.close();
    // A decision the desk refuses at the click: the dialog says why, nothing is queued, and the
    // event is not left locked on the board.
    const refused=fixtures([decision('UIFIX-26SEP09-A')]);
    const rid=refused.decisions.rows[0].decision_id;
    refused.refuse={[rid]:'at 0.60 (maker) the edge after the fee, slippage and the uncertainty buffer is 0.03, under the 0.05 manual floor'};
    const f=await openFixture(browser,refused);
    await f.page.locator('#trades tbody tr[data-trade]').first().dblclick();await f.page.waitForSelector('#md_send');
    await f.page.waitForFunction(()=>document.querySelector('#modal_body').textContent.includes('executable edge'));
    await f.page.locator('#md_send').click();
    await f.page.waitForFunction(()=>document.querySelector('#modal_body').textContent.includes('under the 0.05 manual floor'));
    assert.match(await f.page.locator('#modal .dn').last().innerText(),/under the 0\.05 manual floor/,'the backend\'s refusal is shown');
    assert.equal(f.writes.filter(w=>w.endpoint==='manual-orders').length,1);
    await f.page.keyboard.press('Escape');await f.page.waitForSelector('#modal',{state:'detached'});
    assert.equal(await f.page.locator('#trades tbody tr[data-trade]').count(),1,'a refused request leaves the row on the board');
    assert.deepEqual(f.errors,[]);assert.deepEqual(f.unexpected,[]);await f.context.close();
    const killed=fixtures([decision('UIFIX-26SEP09-A')]);
    killed.state.kill=true;killed.state.kill_reason='Daily loss stop: previous floor';
    killed['risk-controls'].kill=true;killed['risk-controls'].kill_reason=killed.state.kill_reason;
    const k=await openFixture(browser,killed);
    await k.page.locator('#killbtn').click();
    assert.equal(await k.page.locator('#account_stop_controls').evaluate(e=>e.open),true);
    assert.equal(k.writes.length,0,'reviewing a daily halt never re-arms or starts trading');
    await k.page.locator('[data-tab="trades"]').count().then(async n=>{if(n)await k.page.locator('[data-tab="trades"]').click();});
    await k.page.locator('#t_ticker').fill('UNKNOWN-26SEP09-A');
    await k.page.locator('#t_price').fill('0.37');await k.page.locator('#t_count').fill('1');
    await k.page.locator('#t_submit').click();await k.page.waitForSelector('#md_send');
    await k.page.locator('#md_send').click();
    await k.page.waitForFunction(()=>document.querySelector('#actionlog').textContent.includes('Desk halted'));
    assert.equal(k.writes.length,0,'known halt refuses before any order POST');
    await k.page.keyboard.press('Escape');await k.page.waitForSelector('#modal',{state:'detached'});
    assert.deepEqual(k.errors,[]);assert.deepEqual(k.unexpected,[]);await k.context.close();
    // A 15-minute commodity row. Its probability is a statement about where one reference value
    // lands at the window's close, so the board and the dialog must show the numbers it was
    // computed from: the reference, the strike, the distance in sigma and the real clock. The
    // clock matters twice over - the exchange's expected_expiration_time on these series is hours
    // past the window, so a row that reads its own diagnostics says "4m" where the generic column
    // would have said "5.8h".
    const gold=decision('KXGOLD15M-26SEP09T2300-T3652',{
      category:'commodities',title:'Gold at 23:00',model:'commodity_terminal_gold',model_version:'v1',
      model_key:'commodity_terminal_gold_v1',p_win:.95,p_yes:.95,sigma:.04,hours_to_settlement:5.81,
      // The estimate is 40 seconds old. The board must age its clocks by that, not print the
      // numbers the model computed as though they were current.
      fair_ts:now-40,close_ts:now+221,
      quote:{ticker:'KXGOLD15M-26SEP09T2300-T3652',model_name:'commodity_terminal_gold',model_version:'v1',
             model_key:'commodity_terminal_gold_v1',p_yes:.95,sigma:.04,model_ts:now-40,underlying_ts:now-42,
             inputs_valid:true,settlement_verified:true,auto_eligible:false,research_only:false,
             diagnostics:{inputs:{valid:true,reasons:[],auto_eligible:false,source:'Kalshi pyth_value (Metal.Index.GOLD/USD)',
               underlying_ts:now-42,underlying:'gold',symbol:'Metal.Index.GOLD/USD',reference_price:3654.21,
               reference_age_seconds:2,strike:3652.0,seconds_to_close:261,distance:2.21,distance_in_sigma:1.61,
               payoff:{type:'above',strike_type:'greater_or_equal',lower:3652.0,lower_inclusive:true},
               // 3654.21 against a 3652.00 strike is ln(1.000605) = 6.05e-4, which over an
               // sd_log of 3.75e-4 is 1.61 sigma and P(YES) = 0.946. The fixture states the
               // numbers a real estimate would carry, so a units or precision error in the row
               // shows up as an inconsistency rather than as a string that still matches.
               terminal_distribution:{spot:3654.21,sigma_per_sqrt_second:0.0000232,sd_log:0.000375,tau_seconds:263,mu_per_second:0},
               volatility:{sigma_per_sqrt_second:0.0000232,rv_1m:0.000021,rv_5m:0.0000225,warm:true,samples:720},
               probability_band:0.06,
               settlement:{rules_name_this_feed:true,replicated_strike:true,replication_error:0.0,tolerance:0.02},
               external:{symbol:'GC=F',enabled:true,price:3655.10,basis:0.89}}}}});
    const g=await openFixture(browser,fixtures([gold]));
    const grow=g.page.locator('#trades tbody tr[data-trade]').first();
    const gtext=await grow.innerText();
    assert.match(gtext,/ref 3654\.21/,'the board row names the reference value it was priced from');
    assert.match(gtext,/strike 3652\.00/);
    assert.match(gtext,/1\.61σ/,'and how far the strike is in sigma');
    // close_ts is now+221, so the window has 3m41s left - not the 4m21s the model computed forty
    // seconds ago, and not the 5.8h the exchange's expiration field claims.
    assert.match(gtext,/3m4[01]s left/,'the clock runs on, it is not frozen at the model timestamp');
    assert.doesNotMatch(gtext,/5\.8h/,'the misleading expected_expiration_time is not shown for these');
    assert.match(gtext,/reference 4[12]s old/,'and the reference age includes the age of the estimate itself');
    await grow.dblclick();await g.page.waitForSelector('#md_send');
    await g.page.waitForFunction(()=>document.querySelector('#modal_body').textContent.includes('executable edge'));
    const gmodal=await g.page.locator('#modal').innerText();
    for (const k of [/reference \(Metal\.Index\.GOLD\/USD\)\s*3654\.21/i,/4[12]s old/,/estimate itself 40s old/,
                     /strike\s*3652\.00/i,/at or above/i,
                     /distance\s*\+2\.21/i,/1\.61σ/,/time left\s*3m4[01]s/i,/σ used\s*0\.018% per minute/i,
                     /model p\(YES\)\s*95\.0%/i,/settlement feed\s*confirmed/i,/GC=F cross-check/i,/diagnostic only, never priced from/i])
      assert.match(gmodal,k,'the commodity dialog shows '+k);
    assert.deepEqual(g.errors,[]);assert.deepEqual(g.writes,[]);await g.context.close();
    const stale=fixtures([decision('UIFIX-26SEP09-A')]);stale.decisions.generated_ts=now-120;
    stale['account-summary'].combined={cash:100,equity:null,pnl_today:null,pnl_total:null,pnl_recorded:null};
    const x=await openFixture(browser,stale);
    assert.equal(await x.page.locator('#trades tbody tr[data-trade]').count(),0);
    assert.equal(await x.page.locator('#combined_equity').textContent(),'-');
    assert.equal(await x.page.locator('#pnl_today').textContent(),'-');
    assert.match(await x.page.locator('#trades_note').innerText(),/stale/);
    assert.deepEqual(x.errors,[]);assert.deepEqual(x.writes,[]);await x.context.close();
    console.log('PASS: the Trades board shows the desk\'s decisions (rank, route, AUTO marked, RESEARCH behind its filter, no route recomputed), the dialog\'s breakdown is the desk\'s and the order names its decision, a commodity row shows its reference, strike, sigma distance and real clock, a backend refusal is shown, modal and Ticket duplicate guards, immutable explicit ticket values, unknown fair, preset preview, account totals and stale-data checks. All requests intercepted.');
  } finally {await browser.close();}
}
run().catch(error=>{console.error(error);process.exitCode=1;});
