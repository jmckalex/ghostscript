// Smoke tests for the WASM build. Run with: npm test  (or node --test test/*.test.mjs)
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, readFileSync, readdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { runGhostscript } from '../dist/ghostscript.mjs';

const CLI = fileURLToPath(new URL('../dist/gs-cli.mjs', import.meta.url));

const TWO_PAGES_PS = `%!PS
/Helvetica findfont 24 scalefont setfont
72 700 moveto (Hello from WebAssembly) show
0 0 1 setrgbcolor 72 600 200 50 rectfill
showpage
/Times-Roman findfont 24 scalefont setfont
72 700 moveto (Second page) show
showpage
`;

const PNG_SIGNATURE = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];

function pngSize(bytes) {
  assert.deepEqual([...bytes.subarray(0, 8)], PNG_SIGNATURE, 'PNG signature');
  const view = new DataView(bytes.buffer, bytes.byteOffset);
  return { width: view.getUint32(16), height: view.getUint32(20) };
}

const ascii = (bytes, n) => new TextDecoder().decode(bytes.subarray(0, n));

test('reports its version', async () => {
  const { exitCode, stdout } = await runGhostscript(['--version']);
  assert.equal(exitCode, 0);
  assert.equal(stdout.trim(), '10.08.0');
});

test('renders PostScript to one PNG per page', async () => {
  const { exitCode, files, stderr } = await runGhostscript(
    ['-sDEVICE=png16m', '-r50', '-g425x550', '-o', 'page-%d.png', 'in.ps'],
    { files: { 'in.ps': TWO_PAGES_PS } });
  assert.equal(exitCode, 0, stderr);
  assert.deepEqual(Object.keys(files).sort(), ['page-1.png', 'page-2.png']);
  assert.deepEqual(pngSize(files['page-1.png']), { width: 425, height: 550 });
});

test('PostScript -> PDF -> text round trip (pdfwrite + PDF interpreter)', async () => {
  const toPdf = await runGhostscript(['-sDEVICE=pdfwrite', '-o', 'out.pdf', 'in.ps'],
    { files: { 'in.ps': TWO_PAGES_PS } });
  assert.equal(toPdf.exitCode, 0, toPdf.stderr);
  const pdf = toPdf.files['out.pdf'];
  assert.equal(ascii(pdf, 5), '%PDF-');

  const toText = await runGhostscript(['-sDEVICE=txtwrite', '-o', '-', 'in.pdf'],
    { files: { 'in.pdf': pdf } });
  assert.equal(toText.exitCode, 0, toText.stderr);
  assert.match(toText.stdout, /Hello from WebAssembly/);
  assert.match(toText.stdout, /Second page/);
});

test('a failing run returns a non-zero exit code without touching process.exitCode', async () => {
  const before = process.exitCode;
  const { exitCode, stdout, stderr } = await runGhostscript(['-sDEVICE=nullpage', '-o', '-', 'missing.pdf']);
  assert.notEqual(exitCode, 0);
  // Ghostscript reports the error detail on stdout, only the summary on stderr.
  assert.match(stdout, /undefinedfilename in \(missing\.pdf\)/);
  assert.match(stderr, /Unrecoverable error/);
  assert.equal(process.exitCode, before);
});

test('CLI reads and writes the host filesystem', () => {
  const dir = mkdtempSync(join(tmpdir(), 'gs-wasm-'));
  writeFileSync(join(dir, 'in.ps'), TWO_PAGES_PS);
  const r = spawnSync(process.execPath,
    [CLI, '-q', '-sDEVICE=pngalpha', '-r36', '-o', join(dir, 'p%d.png'), 'in.ps'],
    { cwd: dir, encoding: 'utf8' });
  assert.equal(r.status, 0, r.stderr);
  assert.deepEqual(readdirSync(dir).sort(), ['in.ps', 'p1.png', 'p2.png']);
  pngSize(readFileSync(join(dir, 'p1.png')));
});

test('CLI pipes binary data through stdin and stdout', () => {
  const r = spawnSync(process.execPath,
    [CLI, '-q', '-sDEVICE=pdfwrite', '-o', '-', '-'],
    { input: TWO_PAGES_PS });
  assert.equal(r.status, 0, r.stderr?.toString());
  assert.equal(ascii(r.stdout, 5), '%PDF-');
  assert.match(r.stdout.subarray(-8).toString('latin1'), /%%EOF\s*$/);
});
