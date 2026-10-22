// Rasterize SVG icons with headless Chromium (Playwright). Used only by
// tools/gen_ui_icons.py; normal builds use the committed src/ui/ui_icons.h.
//   node tools/svg_render.js <outdir> <size> <file.svg>...
// Writes <outdir>/<name>-<size>.png with straight (unpremultiplied) alpha.
const fs = require('fs');
const path = require('path');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');

(async () => {
  const [outDir, sizeText, ...files] = process.argv.slice(2);
  const size = Number(sizeText);
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: size, height: size }, deviceScaleFactor: 1 });
  for (const file of files) {
    const svg = fs.readFileSync(file, 'utf8');
    await page.setContent(`<html><body style="margin:0;background:transparent">${svg}</body></html>`);
    await page.evaluate((s) => {
      const e = document.querySelector('svg');
      e.setAttribute('width', s);
      e.setAttribute('height', s);
    }, size);
    const out = path.join(outDir, `${path.basename(file, '.svg')}-${size}.png`);
    await page.screenshot({ path: out, omitBackground: true, clip: { x: 0, y: 0, width: size, height: size } });
  }
  await browser.close();
})();
