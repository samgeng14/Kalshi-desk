// How much DOM work the dashboard does per second while idling on live-ish data.
// Measured 2026-09-11 on identical stubbed payloads over 12s: before 680 node creations and
// 680 removals, after 154 and 154. Run it against a backup to compare a change.
//   NODE_PATH=$(npm root -g) node tests/bench_ui_render.cjs ui/index.html 12
const fs=require('node:fs'), {chromium}=require('playwright');
const file=process.argv[2], SECONDS=Number(process.argv[3]||14);
const html=fs.readFileSync(file,'utf8');
const now=Math.floor(Date.now()/1000);
let tick=0;
function mkt(i){return {ticker:`KXT-26SEP11-B${i}`,event_ticker:`KXT-26SEP11`,series_ticker:'KXT',title:`Fixture market ${i}`,
  yes_sub_title:'sub',category:'Climate and Weather',side:'yes',p_win:.6,p_yes:.6,sigma:.03,
  bid:.59,ask:.61,yes_bid:.59,yes_ask:.61,bid_size:10,ask_size:10,spread:.02,price:.60,fee:.01,
  edge:.04,buffered_edge:.03,kelly:.1,verdict:'manual',reasons:[],warnings:[],inputs_valid:true,
  quote_ts:now,fair_ts:now,minutes_to_close:120,settlement_ts:now+7200,hours_to_settlement:2,
  fair_strategy:'weather_nws',tick:0.01,fee_type:'quadratic',fee_multiplier:1};}
function payloads(){
  const markets=Array.from({length:40},(_,i)=>mkt(i));
  markets[0].yes_bid=.58+((tick%3)/100);            // one book ticks, like a real feed
  return {
  '/api/state':{env:'prod',armed:true,now,account_ts:now,cycle:100+tick,last_cycle_ts:now,balance:132.7,
    portfolio_value:0,bankroll:100,markets_seen:400,kill:false,kill_reason:'',last_error:'',
    watch:['KXT','KXBTC','KXHIGHNY'],pending:[],recent:Array.from({length:40},(_,i)=>({id:i,ts:now-i,updated_ts:now-i,
      strategy:'systematic',ticker:`KXT-26SEP11-B${i}`,outcome_side:'yes',price:.5,count:2,net_edge:.05,status:'expired',note:'edge gone'})),
    orders:[],positions:[],fairs:Array.from({length:12},(_,i)=>({ticker:`KXT-26SEP11-B${i}`,p:.6,sigma:.03,note:'fixture'})),
    strategies:[{name:'weather_nws',category:'Climate and Weather',mode:'manual',market_weight:0,auto_rule:{}}],
    events:Array.from({length:8},(_,i)=>({ts:now-i,level:'info',msg:'fixture event '+i})),
    config:{risk:{day_tz:'America/Chicago'}},perps:{enabled:false},polymarket:{enabled:true,quotes_ts:now}},
  '/api/board':{markets,total:40},
  '/api/blotter':Array.from({length:40},(_,i)=>({created_time:'2026-09-11T03:00:0'+(i%10)+'Z',strategy:'ext',
    ticker:`KXT-26SEP11-B${i}`,outcome_side:'yes',exec_style:'taker',prop_price:.5,yes_price:.5,no_price:.5,
    fill_count:1,initial_count:1,avg_fill:.5,fees:.01,status:'executed'})),
  '/api/manual-orders':[], '/api/risk':{daily:{start_pv:100,pv:99,pnl:-1,cash:99,positions_value:0},committed:0,synced_ts:now},
  '/api/pnl':{unrealized:0,perps:{},polymarket:{synced:true,equity:12.3,today:0.4,unrealized:0.1}},
  '/api/auto':{auto_armed:false,budget:10,spent_today:0},
  '/api/ladder':{market:{ticker:'KXT-26SEP11-B0',title:'Fixture',yes_sub_title:'sub',yes_bid:.59,yes_ask:.61,last:.60,
      volume:30000,minutes_to_close:120,fee_type:'quadratic',fee_multiplier:1,tick:.01},
    fair:{p:.6,sigma:.03,strategy:'weather_nws',note:'fixture'},position:null,orders:[],pending:[],
    book:{yes_bids:Array.from({length:45},(_,i)=>[(59-i)/100, 10+((tick+i)%7)]),
          no_bids:Array.from({length:45},(_,i)=>[(39-i)/100, 12+((tick+i)%5)])}},
  '/api/systematic':{running:false,settings:{},positions:[],history:[],log:[],pending:[]},
};}
(async()=>{
  const b=await chromium.launch();
  const p=await b.newPage({viewport:{width:1900,height:1200}});
  await p.route('**/api/**', r=>{
    const u=new URL(r.request().url()); const key='/api'+u.pathname.split('/api')[1].split('?')[0];
    const all=payloads();
    const hit = all[key] !== undefined ? all[key] : (Object.entries(all).find(([k])=>key.startsWith(k))||[null,[]])[1];
    r.fulfill({status:200,contentType:'application/json',body:JSON.stringify(hit===undefined?[]:hit)});
  });
  await p.route('**/*', r=>r.request().url().includes('/api/')?r.fallback():r.fulfill({status:200,body:''}));
  p.on('pageerror',e=>console.log('PAGEERROR:',String(e).slice(0,300)));
  p.on('console',m=>{if(m.type()==='error')console.log('CONSOLE:',m.text().slice(0,200));});
  await p.route('**/__page', r=>r.fulfill({status:200,contentType:'text/html',body:html}));
  await p.goto('http://desk.local/__page',{waitUntil:'domcontentloaded'});
  await p.evaluate(()=>{ window.__m={added:0,removed:0,chars:0,attrs:0};
    new MutationObserver(rs=>{for(const r of rs){window.__m.added+=r.addedNodes.length;window.__m.removed+=r.removedNodes.length;
      if(r.type==='characterData')window.__m.chars++; if(r.type==='attributes')window.__m.attrs++;}})
      .observe(document.body,{subtree:true,childList:true,characterData:true,attributes:true}); });
  // Put a market in the ladder: it is the 99-level table that redrew every 2.5 seconds.
  await p.evaluate(()=>{ try { selectTicker('KXT-26SEP11-B0'); } catch(e){} });
  await p.waitForTimeout(1500);
  await p.evaluate(()=>{ window.__m.added=0;window.__m.removed=0;window.__m.chars=0;window.__m.attrs=0; });
  const t0=Date.now();
  const timer=setInterval(()=>{tick++;},1000);
  await p.waitForTimeout(SECONDS*1000);
  clearInterval(timer);
  const m=await p.evaluate(()=>window.__m);
  const secs=(Date.now()-t0)/1000;
  await b.close();
  console.log(`${file.split('/').pop().padEnd(26)} over ${secs.toFixed(0)}s: ` +
    `nodes created ${String(m.added).padStart(6)}  removed ${String(m.removed).padStart(6)}  ` +
    `text writes ${String(m.chars).padStart(5)}  attr writes ${String(m.attrs).padStart(5)}` +
    `   -> ${(m.added/secs).toFixed(0)} node creations/sec`);
})();
