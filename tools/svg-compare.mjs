#!/usr/bin/env node
// Visual fidelity check for the svg device. Each input is rendered twice:
// by Ghostscript's own rasterizer (png16m), and as SVG (svg device, then
// rasterized by resvg). The two are compared pixel by pixel.
//
//   node tools/svg-compare.mjs [--gs PATH] [--res 96] [--ss 4] [--out DIR] file...
//
// --gs   Ghostscript to use: a native binary, or (default) dist/gs-cli.mjs
// --ss   supersampling: both renders are made at res*ss and box-filtered
//        down, so pixel snapping and antialiasing differences between the
//        two rasterizers mostly cancel out
// --out  write reference | svg | difference images here for inspection
//
// "bad" is the percentage of pixels whose largest channel difference
// exceeds 48/255; "mean" is the mean absolute channel difference.
//
// compareFile() is also used by the test suite.

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { Resvg } from '@resvg/resvg-js';
import { PNG } from 'pngjs';

const DEFAULT_GS = fileURLToPath(new URL('../dist/gs-cli.mjs', import.meta.url));

function runGs(gsPath, argv, cwd) {
  const [cmd, pre] = gsPath.endsWith('.mjs') ? [process.execPath, [gsPath]] : [gsPath, []];
  const t0 = performance.now();
  execFileSync(cmd, [...pre, '-q', '-dSAFER', '-dBATCH', '-dNOPAUSE', ...argv], {
    cwd, stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, ASAN_OPTIONS: 'detect_leaks=0' },
  });
  return performance.now() - t0;
}

function compare(ref, img) {
  let bad = 0, sum = 0;
  const diff = new PNG({ width: ref.width, height: ref.height });
  for (let i = 0; i < ref.data.length; i += 4) {
    let m = 0;
    for (let c = 0; c < 3; c++) {
      const d = Math.abs(ref.data[i + c] - img.data[i + c]);
      sum += d;
      if (d > m) m = d;
    }
    if (m > 48) bad++;
    const v = 255 - Math.min(255, m * 3);
    diff.data[i] = 255; diff.data[i + 1] = v; diff.data[i + 2] = v; diff.data[i + 3] = 255;
  }
  const n = ref.width * ref.height;
  return { bad: (100 * bad) / n, mean: sum / (3 * n), diff };
}

// Box-filter an RGBA image down by an integer factor.
function downsample(img, k) {
  if (k === 1) return img;
  const w = Math.floor(img.width / k), h = Math.floor(img.height / k);
  const out = new PNG({ width: w, height: h });
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      const acc = [0, 0, 0];
      for (let j = 0; j < k; j++)
        for (let i = 0; i < k; i++) {
          const o = ((y * k + j) * img.width + x * k + i) * 4;
          acc[0] += img.data[o]; acc[1] += img.data[o + 1]; acc[2] += img.data[o + 2];
        }
      const o = (y * w + x) * 4;
      out.data[o] = acc[0] / (k * k); out.data[o + 1] = acc[1] / (k * k);
      out.data[o + 2] = acc[2] / (k * k); out.data[o + 3] = 255;
    }
  return out;
}

function sideBySide(imgs) {
  const w = imgs[0].width, h = imgs[0].height, gap = 8;
  const out = new PNG({ width: w * imgs.length + gap * (imgs.length - 1), height: h });
  out.data.fill(128);
  imgs.forEach((im, k) => PNG.bitblt(im, out, 0, 0, w, h, k * (w + gap), 0));
  return out;
}

/**
 * Compare Ghostscript's raster output with its SVG output for one file.
 * Returns one row per page: { file, page, bad, mean, svgKB, msRef, msSvg }
 * (bad and mean as numbers), or { file, page?, error }.
 */
export function compareFile(file, { gs = DEFAULT_GS, res = 96, ss = 4, out = null } = {}) {
  const rows = [];
  const abs = path.resolve(file);
  if (gs.includes(path.sep)) gs = path.resolve(gs);   // we run it from a temporary directory
  const base = path.basename(file).replace(/\.[^.]+$/, '');
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'svgcmp-'));
  const extra = /\.eps$/i.test(file) ? ['-dEPSCrop'] : [];
  if (out) fs.mkdirSync(out, { recursive: true });
  try {
    const msRef = runGs(gs, [...extra, '-sDEVICE=png16m', `-r${res * ss}`, '-o', 'ref-%d.png', abs], tmp);
    const msSvg = runGs(gs, [...extra, '-sDEVICE=svg', '-o', 'page-%d.svg', abs], tmp);
    for (let p = 1; fs.existsSync(path.join(tmp, `ref-${p}.png`)); p++) {
      const big = PNG.sync.read(fs.readFileSync(path.join(tmp, `ref-${p}.png`)));
      const svgPath = path.join(tmp, `page-${p}.svg`);
      if (!fs.existsSync(svgPath)) {
        rows.push({ file: base, page: p, error: 'no svg' });
        continue;
      }
      const svg = fs.readFileSync(svgPath);
      const rendered = new Resvg(svg, {
        fitTo: { mode: 'width', value: big.width },
        background: 'white',
        font: { loadSystemFonts: false },
      }).render();
      if (rendered.width !== big.width || Math.abs(rendered.height - big.height) > ss) {
        rows.push({ file: base, page: p, error: `size ${rendered.width}x${rendered.height} vs ${big.width}x${big.height}` });
        continue;
      }
      const bigImg = new PNG({ width: rendered.width, height: rendered.height });
      bigImg.data = Buffer.from(rendered.pixels);
      const ref = downsample(big, ss);
      const img = downsample(bigImg, ss);
      // Heights may differ by a rounding pixel; compare the common area.
      const h = Math.min(img.height, ref.height);
      const refC = new PNG({ width: ref.width, height: h });
      const imgC = new PNG({ width: ref.width, height: h });
      PNG.bitblt(ref, refC, 0, 0, ref.width, h, 0, 0);
      PNG.bitblt(img, imgC, 0, 0, ref.width, h, 0, 0);
      const r = compare(refC, imgC);
      rows.push({ file: base, page: p, bad: r.bad, mean: r.mean, svgKB: svg.length / 1024, msRef, msSvg });
      if (out) {
        fs.writeFileSync(path.join(out, `${base}-${p}.png`), PNG.sync.write(sideBySide([refC, imgC, r.diff])));
        fs.copyFileSync(svgPath, path.join(out, `${base}-${p}.svg`));
      }
    }
  } catch (e) {
    rows.push({ file: base, error: String(e.stderr || e.message).split('\n').slice(0, 3).join(' | ') });
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
  return rows;
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  const args = process.argv.slice(2);
  const opt = {};
  const files = [];
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--gs') opt.gs = args[++i];
    else if (args[i] === '--res') opt.res = Number(args[++i]);
    else if (args[i] === '--ss') opt.ss = Number(args[++i]);
    else if (args[i] === '--out') opt.out = args[++i];
    else files.push(args[i]);
  }
  if (!files.length) {
    console.error('usage: svg-compare.mjs [--gs PATH] [--res DPI] [--ss N] [--out DIR] file...');
    process.exit(2);
  }
  const rows = files.flatMap((f) => compareFile(f, opt)).map((r) => (r.error ? r : {
    ...r, bad: r.bad.toFixed(2), mean: r.mean.toFixed(2), svgKB: r.svgKB.toFixed(1),
    msRef: Math.round(r.msRef), msSvg: Math.round(r.msSvg),
  }));
  console.table(rows);
}
