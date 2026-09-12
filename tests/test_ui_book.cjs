// The book panel: two lanes, one switch (ui/index.html, /api/book).
// Asserts the panel tells the truth about each lane - a lane that cannot run says why rather
// than merely looking off - that lane 2's depth reaches the status line, and that the switch
// posts both directions. Starting spends money, so the confirm is part of the flow.
//   NODE_PATH=$(npm root -g) node tests/test_ui_book.cjs ui/index.html
const fs=require('node:fs'),{chromium}=require('playwright');
const html=fs.readFileSync(process.argv[2]||'ui/index.html','utf8');
const LANES=(on)=>({running:on,lanes_on:on?2:0,for_you:7,blocked:12,best_edge:.084,best_ticker:'KXNFLTOTAL-X',
  kill:false,kill_reason:'',lanes:[
   {name:'systematic',covers:'weather, crypto, sports and the locked baskets',available:true,unavailable_reason:'',running:on,mode:'live'},
   {name:'pairs',covers:'the same game on Kalshi and Polymarket',available:true,unavailable_reason:'',running:on,mode:'paper'},
   {name:'perps',covers:'the margined instance',available:false,unavailable_reason:'perps are off in config',running:false,mode:'paper'}]});
(async()=>{
 const b=await chromium.launch();const p=await b.newPage({viewport:{width:1900,height:1200}});
 let on=false, posted=null;
 p.on('dialog',d=>d.accept());
 await p.route('**/api/**',r=>r.fulfill({status:200,contentType:'application/json',body:'{}'}));
 await p.route('**/api/book',r=>{
   if(r.request().method()==='POST'){posted=JSON.parse(r.request().postData()||'{}');on=!!posted.running;
     return r.fulfill({status:200,contentType:'application/json',body:JSON.stringify({...LANES(on),changed:['systematic','pairs'],refused:[{lane:'perps',why:'perps are off in config'}]})});}
   r.fulfill({status:200,contentType:'application/json',body:JSON.stringify(LANES(on))});});
 await p.route('**/__page',r=>r.fulfill({status:200,contentType:'text/html',body:html}));
 await p.goto('http://desk.local/__page',{waitUntil:'domcontentloaded'});
 await p.waitForTimeout(1200);
 const R=[];const txt=async s=>{try{return (await p.locator(s).first().innerText({timeout:2500})).trim();}catch{return '<missing>';}};
 R.push(['panel renders', await p.locator('#bookpanel').count()===1]);
 R.push(['three lanes drawn', await p.locator('#book_lanes .lane').count()===3]);
 R.push(['unavailable lane flagged', await p.locator('#book_lanes .lane.cant').count()===1]);
 R.push(['unavailable lane says why', (await txt('#book_lanes .lane.cant')).includes('off in config')]);
 R.push(['lane 2 depth shown', await txt('#book_l2')==='7']);
 R.push(['blocked shown', await txt('#book_blocked')==='12']);
 R.push(['best edge shown', (await txt('#book_best')).startsWith('0.08')]);
 R.push(['status line shows lane 2', (await txt('#st_foryou')).includes('7 for you')]);
 R.push(['button offers start', (await txt('#book_btn')).includes('start')]);
 await p.locator('#book_btn').click(); await p.waitForTimeout(900);
 R.push(['click posts running:true', posted && posted.running===true]);
 R.push(['button flips to stop', (await txt('#book_btn')).includes('stop')]);
 R.push(['running lanes marked on', await p.locator('#book_lanes .lane.on').count()===2]);
 R.push(['paper vs live distinguished', (await txt('#book_lanes')).includes('PAPER') && (await txt('#book_lanes')).includes('LIVE')]);
 await p.locator('#book_btn').click(); await p.waitForTimeout(900);
 R.push(['stop posts running:false', posted && posted.running===false]);
 await b.close();
 let bad=0;for(const[n,ok]of R){if(!ok)bad++;console.log(`  ${ok?'ok  ':'FAIL'} ${n}`);}
 console.log(bad?`${bad} FAILED`:`book panel: ${R.length} checks, 0 failures`);process.exit(bad?1:0);
})();
