// Every <div class="scroll"> carries an inline max-height. That height only means something if
// the computed overflow clips: with overflow:visible the box is the stated height but the rows
// keep painting past it, over whatever follows in the panel. On 2026-09-10 that put the blotter's
// rows on top of the "Recent proposals" heading and table, and the second table's sticky <th>
// latched onto the page instead of its own box, so two tables interleaved on screen.
//
// getBoundingClientRect reports layout position and ignores clipping, so it cannot see this bug.
// The check is on the container: computed overflow-y, and content taller than the visible box.
//
// Run with Playwright on NODE_PATH (or installed locally):
//   NODE_PATH=/path/to/node_modules node tests/test_dashboard_scroll.cjs
// Add --break to confirm the check still fails on the old CSS.
const fs = require('node:fs'), path = require('node:path');
const assert = require('node:assert/strict');
const {chromium} = require('playwright');

const CLIPPING = ['auto', 'scroll', 'hidden'];
const file = path.join(__dirname, '../ui/index.html');
let html = fs.readFileSync(file, 'utf8');
if (process.argv.includes('--break'))
  html = html.replace(/\.scroll \{ max-height:none; overflow:auto; \}/, '.scroll { max-height:none; overflow:visible; }');

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage({viewport: {width: 1900, height: 1100}});
  await page.route('**/*', r => r.fulfill({status: 200, contentType: 'application/json', body: '{}'}));
  await page.setContent(html, {waitUntil: 'domcontentloaded'});
  const result = await page.evaluate(() => {
    // More rows than any of these boxes can show, so a box that does not clip must spill.
    document.querySelector('#blotter tbody').innerHTML =
      Array.from({length: 80}, (_, i) => `<tr><td class="l">row ${i}</td>${'<td>x</td>'.repeat(10)}</tr>`).join('');
    document.querySelector('#recent tbody').innerHTML =
      Array.from({length: 40}, (_, i) => `<tr><td class="l">t${i}</td>${'<td>y</td>'.repeat(8)}</tr>`).join('');
    const boxes = [];
    for (const d of document.querySelectorAll('.scroll')) {
      const cs = getComputedStyle(d);
      boxes.push({id: (d.querySelector('table') || {}).id || '(div)', overflowY: cs.overflowY,
                  clientH: Math.round(d.clientHeight), scrollH: Math.round(d.scrollHeight)});
    }
    const blotter = document.querySelector('#blotter').closest('.scroll');
    return {boxes, blotterOverflows: blotter.scrollHeight > blotter.clientHeight + 1,
            blotterOverflowY: getComputedStyle(blotter).overflowY};
  });
  await browser.close();

  for (const b of result.boxes)
    console.log(`  ${String(b.id).padEnd(18)} overflow-y=${b.overflowY.padEnd(8)} ` +
                `client=${String(b.clientH).padStart(5)} content=${String(b.scrollH).padStart(6)} ` +
                (CLIPPING.includes(b.overflowY) ? 'clips' : 'SPILLS'));
  const spilling = result.boxes.filter(b => !CLIPPING.includes(b.overflowY));
  assert.equal(spilling.length, 0,
    `${spilling.length} scroll box(es) do not clip, so their rows paint over what follows: ` +
    spilling.map(b => b.id).join(', '));
  // The blotter is the box the bug was reported on: it must be genuinely scrollable, not just
  // tall enough today to hide the problem.
  assert.ok(result.blotterOverflows, 'blotter box did not overflow; the fixture is not exercising the check');
  assert.ok(CLIPPING.includes(result.blotterOverflowY), 'blotter box does not clip its rows');
  console.log(`${result.boxes.length} scroll boxes, all clipping. OK`);
})().catch(e => { console.error(String(e.message || e)); process.exit(1); });
