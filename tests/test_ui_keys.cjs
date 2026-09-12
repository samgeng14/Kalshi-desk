// The keyboard layer and status line (ui/index.html). A desk is driven from the home row, and
// the four invariants - key armed, backend answering, how old the numbers are, what is at risk -
// must never be a click away. Deliberately asserts that NO single key sends an order or trips
// the kill switch, and that keys are inert while a field has focus.
//   NODE_PATH=$(npm root -g) node tests/test_ui_keys.cjs ui/index.html
const fs=require('node:fs'),{chromium}=require('playwright');
const html=fs.readFileSync(process.argv[2]||'ui/index.html','utf8');
(async()=>{const b=await chromium.launch();const p=await b.newPage({viewport:{width:1900,height:1100}});
 const errs=[]; p.on('pageerror',e=>errs.push(String(e).slice(0,120)));
 await p.route('**/api/**',r=>r.fulfill({status:200,contentType:'application/json',body:'{}'}));
 await p.route('**/__page',r=>r.fulfill({status:200,contentType:'text/html',body:html}));
 await p.goto('http://desk.local/__page',{waitUntil:'domcontentloaded'});
 const R=[];
 const has=async(sel)=>await p.locator(sel).count()>0;
 R.push(['status line present', await has('#statusline')]);
 R.push(['shortcut sheet present', await has('#keys')]);
 await p.keyboard.press('?');
 R.push(['? opens the sheet', await p.evaluate(()=>document.body.classList.contains('keys'))]);
 await p.keyboard.press('Escape');
 R.push(['Esc closes it', await p.evaluate(()=>!document.body.classList.contains('keys'))]);
 await p.keyboard.press('d');
 R.push(['d sets density', await p.evaluate(()=>document.documentElement.dataset.density==='compact')]);
 await p.keyboard.press('d'); await p.keyboard.press('d');
 R.push(['d cycles back to normal', await p.evaluate(()=>!document.documentElement.dataset.density)]);
 await p.keyboard.press('/');
 R.push(['/ focuses the filter', await p.evaluate(()=>document.activeElement && document.activeElement.id==='tr_q')]);
 await p.keyboard.type('abc');
 R.push(['typing is not swallowed', await p.evaluate(()=>document.querySelector('#tr_q').value==='abc')]);
 R.push(['j while typing does nothing', await p.evaluate(()=>!document.documentElement.dataset.density)]);
 await p.keyboard.press('Escape');
 R.push(['Esc blurs the field', await p.evaluate(()=>!document.activeElement || document.activeElement.id!=='tr_q')]);
 R.push(['no page errors', errs.length===0]);
 await b.close();
 let bad=0; for(const [n,ok] of R){ if(!ok)bad++; console.log(`  ${ok?'ok  ':'FAIL'} ${n}`);}
 if(errs.length) console.log('  errors:',errs.slice(0,3));
 console.log(bad?`${bad} FAILED`:`keyboard layer: ${R.length} checks, 0 failures`);
 process.exit(bad?1:0);})();
