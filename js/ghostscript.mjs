// High-level wrapper around the Emscripten build of Ghostscript (gs.js/gs.wasm).
// Works in browsers, web workers and Node.
//
//   import { runGhostscript } from './ghostscript.mjs';
//   const { exitCode, files } = await runGhostscript(
//     ['-sDEVICE=png16m', '-r150', '-o', 'page-%d.png', 'in.pdf'],
//     { files: { 'in.pdf': pdfBytes } });
//   files['page-1.png'] // Uint8Array
//
// Ghostscript's main() is not re-entrant, so every run gets a fresh instance.
// The .wasm is compiled once and the compiled module is reused across runs.

import createGhostscript from './gs.js';

const WORKDIR = '/work';
const isNode = typeof process === 'object' && !!process.versions?.node;

let wasmModulePromise = null;

/**
 * Compile gs.wasm ahead of the first run (optional; runGhostscript calls it).
 * @param {string|URL} [wasmUrl] where to load gs.wasm from; defaults to the
 *   file next to this module.
 * @returns {Promise<WebAssembly.Module>}
 */
export function loadGhostscript(wasmUrl) {
  if (!wasmModulePromise) {
    const url = new URL(wasmUrl ?? './gs.wasm', import.meta.url);
    wasmModulePromise = compile(url).catch((e) => {
      wasmModulePromise = null;
      throw e;
    });
  }
  return wasmModulePromise;
}

async function compile(url) {
  if (isNode && url.protocol === 'file:') {
    const { readFile } = await import('node:fs/promises');
    return WebAssembly.compile(await readFile(url));
  }
  const response = fetch(url);
  try {
    return await WebAssembly.compileStreaming(response);
  } catch {
    // compileStreaming needs Content-Type: application/wasm; fall back.
    return WebAssembly.compile(await (await response).arrayBuffer());
  }
}

/**
 * Run Ghostscript once with command-line arguments.
 *
 * @param {string[]} args arguments, as for the gs command (without "gs")
 * @param {object} [options]
 * @param {Record<string, Uint8Array|ArrayBuffer|string>} [options.files]
 *   input files to place in the working directory before the run
 * @param {Uint8Array|string} [options.stdin] data readable via "-" / %stdin
 * @param {(line: string) => void} [options.onStdout] streamed stdout lines
 * @param {(line: string) => void} [options.onStderr] streamed stderr lines
 * @returns {Promise<{exitCode: number, stdout: string, stderr: string,
 *   files: Record<string, Uint8Array>}>} `files` holds every file in the
 *   working directory that the run created or modified.
 */
export async function runGhostscript(args, options = {}) {
  const wasmModule = await loadGhostscript();
  const stdout = [];
  const stderr = [];
  const stdinBytes = typeof options.stdin === 'string'
    ? new TextEncoder().encode(options.stdin)
    : options.stdin ?? new Uint8Array(0);
  let stdinPos = 0;

  const gs = await createGhostscript({
    thisProgram: 'gs',
    print: (line) => { stdout.push(line); options.onStdout?.(line); },
    printErr: (line) => { stderr.push(line); options.onStderr?.(line); },
    // Never block on, or (in browsers) window.prompt() for, input.
    stdin: () => (stdinPos < stdinBytes.length ? stdinBytes[stdinPos++] : null),
    instantiateWasm(imports, receive) {
      WebAssembly.instantiate(wasmModule, imports).then((inst) => receive(inst, wasmModule));
      return {};
    },
  });

  const { FS } = gs;
  FS.mkdir(WORKDIR);
  FS.chdir(WORKDIR);
  const inputs = new Map();
  for (const [name, data] of Object.entries(options.files ?? {})) {
    const path = `${WORKDIR}/${name}`;
    FS.mkdirTree(path.slice(0, path.lastIndexOf('/')));
    FS.writeFile(path, data instanceof ArrayBuffer ? new Uint8Array(data) : data);
    inputs.set(path, FS.stat(path).mtime.getTime());
  }

  // In Node the Emscripten runtime records main()'s status in
  // process.exitCode; don't let a Ghostscript error leak into the host.
  const savedExitCode = isNode ? process.exitCode : undefined;
  let exitCode;
  try {
    exitCode = gs.callMain([...args]);
  } finally {
    if (isNode) process.exitCode = savedExitCode;
  }

  const files = {};
  for (const path of walk(FS, WORKDIR)) {
    if (inputs.get(path) === FS.stat(path).mtime.getTime()) continue;
    files[path.slice(WORKDIR.length + 1)] = FS.readFile(path);
  }
  return { exitCode, stdout: stdout.join('\n'), stderr: stderr.join('\n'), files };
}

function* walk(FS, dir) {
  for (const name of FS.readdir(dir)) {
    if (name === '.' || name === '..') continue;
    const path = `${dir}/${name}`;
    if (FS.isDir(FS.stat(path).mode)) yield* walk(FS, path);
    else yield path;
  }
}
