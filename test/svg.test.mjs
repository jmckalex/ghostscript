// Tests for the svg device. Run with: npm test
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { runGhostscript } from '../dist/ghostscript.mjs';
import { compareFile } from '../tools/svg-compare.mjs';

const here = (p) => fileURLToPath(new URL(p, import.meta.url));
const EXAMPLES = here('../src/ghostscript-10.08.0/examples/');
const SAMPLES = here('../demo/samples/');

async function toSvg(file, extra = []) {
  const name = file.split('/').pop();
  const { exitCode, stderr, files } = await runGhostscript(
    ['-sDEVICE=svg', ...extra, '-o', 'page-%d.svg', name],
    { files: { [name]: readFileSync(file) } });
  assert.equal(exitCode, 0, stderr);
  const decoder = new TextDecoder();
  return Object.keys(files).sort((a, b) => parseInt(a.slice(5)) - parseInt(b.slice(5)))
    .map((f) => decoder.decode(files[f]));
}

const count = (s, re) => (s.match(re) || []).length;

test('writes a well-formed standalone SVG document', async () => {
  const [svg] = await toSvg(SAMPLES + 'hello.ps');
  assert.match(svg, /^<\?xml version="1.0" encoding="UTF-8"\?>\n<svg xmlns="http:\/\/www.w3.org\/2000\/svg"/);
  assert.match(svg, /width="612pt" height="792pt" viewBox="0 0 612 792"/);
  assert.match(svg, /<\/svg>\n$/);
  // Every referenced id is defined.
  const ids = new Set([...svg.matchAll(/ id="([^"]+)"/g)].map((m) => m[1]));
  for (const [, ref] of svg.matchAll(/(?:href="#|url\(#)([^")]+)/g)) assert.ok(ids.has(ref), `#${ref} is defined`);
});

test('glyphs are shared outlines drawn with <use>, plus a selectable text layer', async () => {
  const [svg] = await toSvg(SAMPLES + 'hello.ps');
  const defs = count(svg, /<path id="g\d+"/g);
  const uses = count(svg, /<use xlink:href="#g\d+"/g);
  assert.ok(defs > 10 && uses > 5 * defs, `${uses} uses of ${defs} glyph outlines`);
  assert.match(svg, /<text [^>]*font-family="serif" font-weight="bold">PostScript to SVG<\/text>/);
  assert.match(svg, /<g fill-opacity="0" font-family="sans-serif" xml:space="preserve">/);
});

test('TrueType text in a PDF is vector and extractable', async () => {
  const [svg] = await toSvg(EXAMPLES + 'text_graphic_image.pdf');
  assert.ok(count(svg, /<use /g) > 20);
  assert.match(svg, />Black RGB </);
  assert.match(svg, />CIELAB</);
});

test('axial and radial shadings become SVG gradients', async () => {
  const [svg] = await toSvg(SAMPLES + 'shadings.ps');
  assert.ok(count(svg, /<linearGradient /g) >= 3);
  assert.ok(count(svg, /<radialGradient /g) >= 2);
  // Gradient-filled text: glyph outlines filled with a (shared) gradient.
  assert.match(svg, /<path d="[^"]+" fill="url\(#s\d+\)"/);
});

test('images are embedded at their own resolution, unsmoothed ones kept crisp', async () => {
  const [svg] = await toSvg(SAMPLES + 'images.ps');
  assert.match(svg, /<image x="40" y="52" width="120" height="120" preserveAspectRatio="none" class="px" image-rendering="optimizeSpeed" xlink:href="data:image\/png;base64,/);
  assert.match(svg, /<style>@supports \(image-rendering:pixelated\)\{\.px\{image-rendering:pixelated\}\}<\/style>/);
  const png = Buffer.from(svg.match(/base64,([^"]+)/)[1], 'base64');
  assert.equal(png.readUInt32BE(16), 4);   // 4x4 samples, not resampled to the page
  assert.equal(png.readUInt32BE(20), 4);
});

test('one file per page', async () => {
  const pages = await toSvg(EXAMPLES + 'annots.pdf');
  assert.equal(pages.length, 6);
});

test('device parameters turn off the text layer and glyph reuse', async () => {
  const [svg] = await toSvg(SAMPLES + 'hello.ps', ['-dSVGTextLayer=false', '-dSVGReuseGlyphs=false']);
  assert.equal(count(svg, /<text /g), 0);
  assert.equal(count(svg, /<use /g), 0);
});

test('renders like the raster devices', () => {
  // Pixel comparison against Ghostscript's own png16m rendering.
  for (const file of [SAMPLES + 'hello.ps', SAMPLES + 'shadings.ps', SAMPLES + 'images.ps',
    SAMPLES + 'strokes.ps', EXAMPLES + 'tiger.eps', EXAMPLES + 'text_graphic_image.pdf']) {
    for (const row of compareFile(file, { res: 96, ss: 4 })) {
      assert.ok(!row.error, `${row.file}: ${row.error}`);
      assert.ok(row.mean < 3, `${row.file} page ${row.page}: mean difference ${row.mean.toFixed(2)}`);
    }
  }
});
