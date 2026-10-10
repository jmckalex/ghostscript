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
  // Each image's data is a definition, placed with <use>.
  assert.match(svg, /<image id="i1" width="120" height="120" preserveAspectRatio="none" xlink:href="data:image\/png;base64,/);
  assert.match(svg, /<use xlink:href="#i1" x="40" y="52" class="px" image-rendering="optimizeSpeed"\/>/);
  assert.match(svg, /<style>@supports \(image-rendering:pixelated\)\{\.px\{image-rendering:pixelated\}\}<\/style>/);
  const png = Buffer.from(svg.match(/base64,([^"]+)/)[1], 'base64');
  assert.equal(png.readUInt32BE(16), 4);   // 4x4 samples, not resampled to the page
  assert.equal(png.readUInt32BE(20), 4);
});

test('PDF transparency is native SVG: opacity, blend modes, groups and soft masks', async () => {
  const [svg] = await toSvg(SAMPLES + 'transparency.pdf');
  assert.match(svg, / fill-opacity=".5"/);
  assert.match(svg, / stroke-opacity=".5"/);
  for (const mode of ['multiply', 'screen', 'difference']) assert.match(svg, new RegExp(`mix-blend-mode:${mode}`));
  assert.match(svg, /<g opacity=".5" style="isolation:isolate">/);            // group at 50%
  assert.match(svg, /<mask id="m\d+" maskUnits="userSpaceOnUse"[^>]*>/);      // luminosity masks
  assert.match(svg, /<mask [^>]*mask-type="alpha">/);                          // alpha mask
  assert.match(svg, /<rect width="100%" height="100%" fill="#fff"\/>/);       // /BC [1]
  assert.match(svg, /<filter [^>]*><feColorMatrix type="luminanceToAlpha"\/>/); // /TR
  assert.match(svg, /<g mask="url\(#m\d+\)"/);
  // Only the image and its soft mask are raster.
  assert.equal(count(svg, /<image /g), 2);
});

test('-dSVGTransparency=false flattens transparency through the compositor', async () => {
  const [svg] = await toSvg(SAMPLES + 'transparency.pdf', ['-dSVGTransparency=false']);
  assert.equal(count(svg, /<mask /g), 0);
  assert.ok(count(svg, /<image /g) >= 1);
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

test('tiling patterns are SVG <pattern>s, colored and uncolored, at their exact place', async () => {
  const pages = await toSvg(SAMPLES + 'patterns.ps');
  assert.equal(pages.length, 2);           // page 2 draws the same patterns again
  for (const svg of pages) {
    assert.ok(count(svg, /<pattern id="p\d+" patternUnits="userSpaceOnUse"/g) >= 8);
    assert.ok(count(svg, /<g id="t\d+" transform=/g) >= 6);       // the cells' content
    assert.match(svg, /fill="url\(#p\d+\)"/);                    // filled
    assert.match(svg, /stroke="url\(#p\d+\)"/);                  // stroked
    assert.match(svg, /<use xlink:href="#t\d+" color="#[0-9a-f]{6}"\/>/);   // uncolored, in a color
    assert.match(svg, /fill="currentColor"/);                    // ... drawn in currentColor
    // Pattern space is default user space, exactly: no bending to pixels.
    assert.match(svg, /patternTransform="matrix\(1 0 0 -1 0 792\)"/);
    assert.equal(count(svg, /data:image\/png/g) <= 4, true);   // the image cell and masks only
  }
});

test('JPEG images are embedded as the original JPEG data, each once per page', async () => {
  const [svg] = await toSvg(SAMPLES + 'features.pdf');
  assert.equal(count(svg, /<image id="i\d+"[^>]* xlink:href="data:image\/jpeg;base64,/g), 3);  // two sizes, and grey
  assert.equal(count(svg, /<use xlink:href="#i\d+"/g), 4);
  const jpeg = Buffer.from(svg.match(/data:image\/jpeg;base64,([^"]+)/)[1], 'base64');
  assert.equal(jpeg.readUInt16BE(0), 0xffd8);
  const [png] = await toSvg(SAMPLES + 'features.pdf', ['-dSVGPassThroughJPEG=false']);
  assert.equal(count(png, /data:image\/jpeg/g), 0);
});

test('fill and stroke under transparency knock out, as in PDF', async () => {
  const [svg] = await toSvg(SAMPLES + 'features.pdf');
  // ca = CA: one element whose opacity covers both.
  assert.match(svg, /<path d="[^"]+" fill="#[0-9a-f]+" stroke="#[0-9a-f]+" stroke-width="14"[^>]* opacity=".5"\/>/);
  // ca != CA: the fill is masked where the stroke paints.
  assert.match(svg, /<path d="[^"]+" fill="#[0-9a-f]+" mask="url\(#m\d+\)" fill-opacity=".3"\/>/);
});

test('links, document information and layers', async () => {
  const [p1, p2] = await toSvg(SAMPLES + 'features.pdf');
  assert.match(p1, /<title>SVG device features<\/title>/);
  assert.match(p1, /<dc:creator>Ghostscript WASM tests<\/dc:creator>/);
  assert.match(p1, /<a xlink:href="https:\/\/ghostscript.com\/"><rect x="38" y="626" width="132" height="16"/);
  assert.match(p1, /<a xlink:href="page-2.svg">/);
  assert.match(p2, /<a xlink:href="page-1.svg">/);
  assert.match(p1, /<g inkscape:groupmode="layer" inkscape:label="Grid">/);
  assert.match(p1, /<g inkscape:groupmode="layer" inkscape:label="Notes">/);
  assert.doesNotMatch(p1, /Hidden"/);                         // off: not drawn at all
  const [plain] = await toSvg(SAMPLES + 'features.pdf', ['-dSVGLinks=false', '-dSVGLayers=false']);
  assert.equal(count(plain, /<a |inkscape:/g), 0);
});

test('renders like the raster devices', () => {
  // Pixel comparison against Ghostscript's own png16m rendering.
  for (const file of [SAMPLES + 'hello.ps', SAMPLES + 'shadings.ps', SAMPLES + 'images.ps',
    SAMPLES + 'strokes.ps', SAMPLES + 'transparency.pdf', SAMPLES + 'features.pdf',
    EXAMPLES + 'tiger.eps', EXAMPLES + 'text_graphic_image.pdf']) {
    // (Not patterns.ps: the raster devices bend a pattern's step to whole
    // pixels, so their tiles drift from where the svg device, exactly as
    // specified, puts them.)
    for (const row of compareFile(file, { res: 96, ss: 4 })) {
      assert.ok(!row.error, `${row.file}: ${row.error}`);
      assert.ok(row.mean < 3, `${row.file} page ${row.page}: mean difference ${row.mean.toFixed(2)}`);
    }
  }
});
