// Correctness of the keyed row renderer (ui/index.html, window.__render.keyedRows).
// It reuses DOM nodes instead of rebuilding them, which is only safe if the result is identical
// to a rebuild AND surviving rows keep their identity - otherwise focus, caret, text selection
// and scroll are lost on every poll, which is the bug the renderer exists to fix.
//   NODE_PATH=$(npm root -g) node tests/test_ui_render.cjs ui/index.html
const fs=require('node:fs'), {chromium}=require('playwright');
const html=fs.readFileSync(process.argv[2],'utf8');
(async()=>{
  const b=await chromium.launch(); const p=await b.newPage();
  await p.route('**/api/**',r=>r.fulfill({status:200,contentType:'application/json',body:'{}'}));
  await p.route('**/__page',r=>r.fulfill({status:200,contentType:'text/html',body:html}));
  await p.goto('http://desk.local/__page',{waitUntil:'domcontentloaded'});
  const out=await p.evaluate(()=>{
    const t=document.createElement('table'); const tb=document.createElement('tbody');
    t.appendChild(tb); document.body.appendChild(t);
    const dump=()=>Array.from(tb.children).map(tr=>tr.dataset.k+':'+Array.from(tr.children).map(td=>td.textContent).join(',')+(tr.className?'#'+tr.className:''));
    const {keyedRows}=window.__render; const R=(items)=>keyedRows(tb, items, i=>i.k, i=>i.c.map((x,n)=>n===0?{t:x,cls:'l'}:x), i=>i.cls||'');
    const res={};
    R([{k:'a',c:['A','1']},{k:'b',c:['B','2']},{k:'c',c:['C','3']}]);
    res.initial=dump();
    const nodeB=tb.querySelector('[data-k="b"]');
    R([{k:'a',c:['A','1']},{k:'b',c:['B','9']},{k:'c',c:['C','3']}]);      // one cell changed
    res.cellChange=dump(); res.identityKept = tb.querySelector('[data-k="b"]')===nodeB;
    R([{k:'c',c:['C','3']},{k:'a',c:['A','1']},{k:'b',c:['B','9']}]);      // reordered
    res.reorder=dump(); res.identityAfterReorder = tb.querySelector('[data-k="b"]')===nodeB;
    R([{k:'c',c:['C','3']},{k:'d',c:['D','4']}]);                          // a,b gone; d new
    res.delete=dump();
    R([{k:'c',c:['C','3','extra']}]);                                      // more columns
    res.widen=dump();
    R([{k:'c',c:['C']}]);                                                  // fewer columns
    res.narrow=dump();
    R([]);                                                                 // empty
    res.empty=dump();
    // focus survival: the reason any of this matters
    R([{k:'x',c:['X','1']}]);
    const inp=document.createElement('input'); tb.querySelector('[data-k="x"]').children[1].appendChild(inp);
    inp.focus(); inp.value='typing';
    R([{k:'x',c:['X','1']},{k:'y',c:['Y','2']}]);
    res.focusKept = document.activeElement===inp && inp.value==='typing';
    // row classes track
    R([{k:'x',c:['X','1'],cls:'touch'}]); res.cls=dump();
    return res;
  });
  await b.close();
  const eq=(a,b)=>JSON.stringify(a)===JSON.stringify(b);
  const checks=[
    ['initial order + cells', eq(out.initial,['a:A,1','b:B,2','c:C,3'])],
    ['cell change applied',   eq(out.cellChange,['a:A,1','b:B,9','c:C,3'])],
    ['node identity kept',    out.identityKept===true],
    ['reorder correct',       eq(out.reorder,['c:C,3','a:A,1','b:B,9'])],
    ['identity after reorder',out.identityAfterReorder===true],
    ['delete + insert',       eq(out.delete,['c:C,3','d:D,4'])],
    ['column widen',          eq(out.widen,['c:C,3,extra'])],
    ['column narrow',         eq(out.narrow,['c:C'])],
    ['empty clears',          eq(out.empty,[])],
    ['focus + caret survive', out.focusKept===true],
    ['row class tracks',      eq(out.cls,['x:X,1#touch'])],
  ];
  let bad=0;
  for (const [n,ok] of checks){ if(!ok)bad++; console.log(`  ${ok?'ok  ':'FAIL'} ${n}`); }
  if(bad){ console.log(JSON.stringify(out,null,1)); process.exit(1); }
  console.log(`keyedRows: ${checks.length} checks, 0 failures`);
})();
