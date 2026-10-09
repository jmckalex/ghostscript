#!/usr/bin/env node
// Command-line Ghostscript on Node, backed by gs.wasm. Behaves like the native
// `gs`: the host filesystem is mounted, and stdin/stdout/stderr are raw bytes
// (so `-sOutputFile=-` and piping a file to `-` work).
//
//   node dist/gs-cli.mjs -sDEVICE=png16m -r150 -o page-%d.png input.pdf

import fs from 'node:fs';
import createGhostscript from './gs.js';

function rawWriter(fd) {
  const buf = new Uint8Array(1 << 16);
  const lineBuffered = fs.fstatSync(fd).isCharacterDevice(); // a terminal
  let n = 0;
  const flush = () => {
    for (let off = 0; off < n;) {
      try {
        off += fs.writeSync(fd, buf, off, n - off);
      } catch (e) {
        if (e.code !== 'EAGAIN') throw e;
      }
    }
    n = 0;
  };
  const put = (byte) => {
    if (byte === null) return flush();
    buf[n++] = byte;
    if (n === buf.length || (lineBuffered && byte === 10)) flush();
  };
  return { put, flush };
}

const out = rawWriter(1);
const err = rawWriter(2);

const inBuf = Buffer.alloc(1 << 16);
let inLen = 0;
let inPos = 0;
let inEOF = false;
const sleeper = new Int32Array(new SharedArrayBuffer(4));

function readStdinByte() {
  if (inPos === inLen) {
    if (inEOF) return null;
    out.flush(); // show any prompt (e.g. "GS>") before blocking
    err.flush();
    for (;;) {
      try {
        inLen = fs.readSync(0, inBuf, 0, inBuf.length, null);
        break;
      } catch (e) {
        if (e.code === 'EAGAIN') { Atomics.wait(sleeper, 0, 0, 10); continue; }
        if (e.code === 'EOF') { inLen = 0; break; }
        throw e;
      }
    }
    inPos = 0;
    if (inLen === 0) { inEOF = true; return null; }
  }
  return inBuf[inPos++];
}

const gs = await createGhostscript({
  thisProgram: 'gs',
  stdin: readStdinByte,
  stdout: out.put,
  stderr: err.put,
});

// Mount every top-level host directory at the same path in the virtual FS so
// absolute and relative paths resolve exactly as they would natively.
const { FS, NODEFS } = gs;
for (const name of fs.readdirSync('/')) {
  if (name === 'dev' || name === 'proc') continue;
  const path = `/${name}`;
  try {
    if (!fs.statSync(path).isDirectory()) continue;
  } catch {
    continue;
  }
  try { FS.mkdir(path); } catch { /* already exists, e.g. /tmp or /home */ }
  FS.mount(NODEFS, { root: path }, path);
}
FS.chdir(process.cwd());

let code;
try {
  code = gs.callMain(process.argv.slice(2));
} finally {
  out.flush();
  err.flush();
}
process.exitCode = code;
