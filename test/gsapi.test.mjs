// The gsapi_* library API, used the way dvisvgm embeds Ghostscript:
// stdio callbacks, init with arguments, then code fed through
// run_string_begin / continue / end. Run with: npm test
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import createGhostscript from '../dist/gs.js';

const DVISVGM_ARGS = ['-q', '-dNODISPLAY', '-dNOPAUSE', '-dWRITESYSTEMDICT', '-dNOPROMPT',
  '-dDELAYBIND', '-dDELAYSAFER', '-dALLOWPSTRANSPARENCY'];
const gs_error_NeedInput = -106;

// 32-bit little-endian words in the module's memory (only HEAPU8 is exported).
const u32 = (gs, p) => new DataView(gs.HEAPU8.buffer).getUint32(p, true);
const setU32 = (gs, p, v) => new DataView(gs.HEAPU8.buffer).setUint32(p, v, true);

// A minimal gsapi client over one module instance.
function client(gs) {
  let out = '';
  const decoder = new TextDecoder();
  const stdin = gs.addFunction(() => 0, 'iiii');
  const stdout = gs.addFunction((handle, str, len) => {
    out += decoder.decode(gs.HEAPU8.subarray(str, str + len));
    return len;
  }, 'iiii');
  const stderr = gs.addFunction((handle, str, len) => len, 'iiii');

  const cstr = (s) => {
    const n = gs.lengthBytesUTF8(s) + 1;
    const p = gs._malloc(n);
    gs.stringToUTF8(s, p, n);
    return p;
  };

  return {
    get output() { return out; },
    // One document's worth of work: new instance, init, code chunks, exit, delete.
    run(chunks) {
      out = '';
      const pinst = gs._malloc(4);
      const pcode = gs._malloc(4);
      setU32(gs, pinst, 0);   // must be NULL: non-NULL means "share this instance"
      let code = gs._gsapi_new_instance(pinst, 0);
      assert.equal(code, 0, 'gsapi_new_instance');
      const inst = u32(gs, pinst);
      gs._gsapi_set_stdio(inst, stdin, stdout, stderr);

      const args = ['gs', ...DVISVGM_ARGS].map(cstr);
      const argv = gs._malloc(4 * args.length);
      args.forEach((p, i) => setU32(gs, argv + 4 * i, p));
      code = gs._gsapi_init_with_args(inst, args.length, argv);
      assert.equal(code, 0, 'gsapi_init_with_args');

      code = gs._gsapi_run_string_begin(inst, 0, pcode);
      assert.ok(code === 0 || code === gs_error_NeedInput, `run_string_begin ${code}`);
      for (const chunk of chunks) {
        const p = cstr(chunk);
        code = gs._gsapi_run_string_continue(inst, p, gs.lengthBytesUTF8(chunk), 0, pcode);
        gs._free(p);
        assert.ok(code === 0 || code === gs_error_NeedInput, `run_string_continue ${code}`);
      }
      code = gs._gsapi_run_string_end(inst, 0, pcode);
      assert.equal(code, 0, 'run_string_end');

      assert.equal(gs._gsapi_exit(inst), 0, 'gsapi_exit');
      gs._gsapi_delete_instance(inst);
      [...args, argv, pinst, pcode].forEach((p) => gs._free(p));
      return out;
    },
  };
}

test('gsapi: revision', async () => {
  const gs = await createGhostscript();
  const rev = gs._malloc(16);   // gsapi_revision_t: 2 pointers + 2 longs (wasm32)
  assert.equal(gs._gsapi_revision(rev, 16), 0);
  assert.equal(u32(gs, rev + 8), 10080);
  assert.match(gs.UTF8ToString(u32(gs, rev)), /Ghostscript/);
});

test('gsapi: run code in chunks, output through the stdout callback', async () => {
  const gs = await createGhostscript();
  // dvisvgm asks for the revision first, in the same module.
  const rev = gs._malloc(16);
  assert.equal(gs._gsapi_revision(rev, 16), 0);
  gs._free(rev);
  const out = client(gs).run(['/sq { dup mul } def ', '7 sq = ', '(dvi.) print 1 2 add == flush\n']);
  assert.equal(out, '49\ndvi.3\n');
});

test('gsapi: many documents in sequence in one module instance', async () => {
  const gs = await createGhostscript();
  const c = client(gs);
  const sizes = [];
  for (let i = 0; i < 30; i++) {
    assert.equal(c.run([`${i} 2 mul = flush\n`]), `${i * 2}\n`);
    sizes.push(gs.HEAPU8.length);
  }
  // The heap settles: no growth over the last 20 documents.
  assert.equal(sizes.at(-1), sizes[9]);
});

test('gsapi: files shared from another module through PROXYFS', async () => {
  const other = await createGhostscript();       // stands in for dvisvgm's module
  other.FS.mkdir('/work');
  other.FS.writeFile('/work/fig.eps', '%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: 0 0 10 10\n(eps ran) = flush\n');
  const gs = await createGhostscript();
  gs.FS.mkdir('/work');
  gs.FS.mount(gs.PROXYFS, { root: '/work', fs: other.FS }, '/work');
  gs.FS.chdir('/work');
  assert.equal(client(gs).run(['(fig.eps) run\n']), 'eps ran\n');
});

test('lean variant, if built, has the interpreters and the API', { skip: !existsSync(new URL('../dist/lean/gs.wasm', import.meta.url)) }, async () => {
  const { default: createLean } = await import('../dist/lean/gs.js');
  const gs = await createLean();
  assert.equal(client(gs).run(['3 4 add = flush\n']), '7\n');
});
