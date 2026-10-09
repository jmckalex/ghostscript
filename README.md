# Ghostscript → WebAssembly

[Ghostscript](https://ghostscript.com) 10.08.0 (released 2026-09-08), built to
WebAssembly with Emscripten. It runs in browsers, web workers and Node.

The unmodified upstream source is vendored in `src/ghostscript-10.08.0`
(downloaded from the [Artifex release][rel] and SHA-512 verified against its
`SHA512SUMS`). No source patches are needed. The port is entirely in
`build.sh` (configure flags and Emscripten link settings) and the JS
wrappers in `js/`.

[rel]: https://github.com/ArtifexSoftware/ghostpdl-downloads/releases/tag/gs10080

## Layout

| Path | What |
|---|---|
| `src/ghostscript-10.08.0/` | upstream source (built in-tree) |
| `build.sh` | configure + build + copy to `dist/` |
| `js/ghostscript.mjs` | library API: `runGhostscript(args, {files})` |
| `js/gs-cli.mjs` | Node CLI that behaves like the native `gs` |
| `test/` | smoke tests (`npm test`) |
| `dist/` | build output: `gs.js`, `gs.wasm` + the wrappers |

## Building

Requires an Emscripten SDK. `build.sh` uses `emcc` from `PATH` if present,
otherwise sources `~/Applications/emsdk/emsdk_env.sh` (what `emsdk-on` does);
point `EMSDK_ENV` elsewhere to use a different SDK.

```sh
./build.sh               # configure (first time) + build → dist/
./build.sh reconfigure   # after changing configure flags in build.sh
./build.sh clean
npm test
```

Knobs: `OPT` (default `-O2`), `JOBS` (default: all CPUs), `EMSDK_ENV`, `CCAUX`.

### How the cross-build works

Ghostscript's build compiles and runs helper programs (`genconf`, `echogs`,
`mkromfs`, …) that generate sources. When `--host` ≠ `--build`, configure
re-runs itself for those *auxiliary tools* with a native compiler (`CCAUX`,
`/usr/bin/clang` here). It also derives the target's `arch.h` (type sizes,
alignment, endianness) from compile-time probes against `emcc` instead of
running `genarch`. The result is a wasm32 `arch.h` (4-byte pointers,
little-endian).

The PostScript init files, ICC profiles and the 35 standard URW fonts are
compiled into the binary as a ROM filesystem (`%rom%`), so nothing needs to
be preloaded at runtime.

### Configuration choices

- **Devices:** `--with-drivers=FILES`, i.e. all file-format devices: `pdfwrite`,
  `ps2write`, `eps2write`, `txtwrite`, `docxwrite`, `png*`, `jpeg*`, `tiff*`,
  `bmp*`, `pnm`/`pam`, `pcx*`, `psd*`, `bbox`, `inkcov`, … Printer drivers are
  left out.
- **Off:** threading (single-threaded rendering), X11, CUPS, fontconfig, dbus,
  GTK, libpaper, libidn, IJS, URF, and Tesseract OCR (it would make the binary much larger and
  need tessdata at runtime).
- **Emscripten:** ES6 module factory `createGhostscript`; `INVOKE_RUN=0`;
  `EXIT_RUNTIME=1` so stdio is flushed when Ghostscript exits; growable
  memory up to 4GB; 8MB stack (Ghostscript recurses deeply); `FS`, `NODEFS`
  and `callMain` exported.

## Using it

### Library (browser or Node)

```js
import { runGhostscript } from './dist/ghostscript.mjs';

const { exitCode, files, stdout, stderr } = await runGhostscript(
  ['-sDEVICE=png16m', '-r150', '-o', 'page-%d.png', 'in.pdf'],
  { files: { 'in.pdf': pdfBytes } },   // Uint8Array | ArrayBuffer | string
);
// files: { 'page-1.png': Uint8Array, 'page-2.png': Uint8Array, … }
```

- Each call runs in a fresh instance, because Ghostscript's `main()` is not
  re-entrant. `gs.wasm` is compiled once and cached. Call
  `loadGhostscript(url?)` to compile it ahead of time, or to load it from
  somewhere other than next to `ghostscript.mjs`.
- Inputs are written to the working directory `/work`. Every file the run
  creates or modifies there comes back in `files`.
- Options: `stdin` (data for `-` / `%stdin`), `onStdout` / `onStderr`
  (streamed lines). Stdin is never interactive, so in a browser it can't
  fall through to `window.prompt()`.
- Use `-o out` (it implies `-dBATCH -dNOPAUSE`), or pass those flags
  yourself.
- In a browser, serve `gs.wasm` with `Content-Type: application/wasm` for
  streaming compilation. The wrapper falls back to a slower path if the
  type is wrong. Rendering is CPU-heavy and synchronous, so run it in a Web
  Worker to keep the page responsive.

### Command line (Node)

```sh
node dist/gs-cli.mjs -sDEVICE=pdfwrite -o out.pdf in.ps
node dist/gs-cli.mjs -q -sDEVICE=png16m -r300 -o - in.pdf > page.png
cat in.ps | node dist/gs-cli.mjs -q -sDEVICE=pdfwrite -o - - > out.pdf
```

The host filesystem is mounted, so paths work as they do with native `gs`.
Stdin and stdout pass raw bytes, so binary output to `-` is safe.

### Raw Emscripten module

`dist/gs.js` is the plain Emscripten factory if you want to work with
`FS` / `callMain` yourself:

```js
import createGhostscript from './dist/gs.js';
const gs = await createGhostscript({ print: console.log, printErr: console.error });
gs.FS.writeFile('/in.ps', '%!PS …');
const code = gs.callMain(['-sDEVICE=pdfwrite', '-o', '/out.pdf', '/in.ps']);
const pdf = gs.FS.readFile('/out.pdf');
```

## Updating Ghostscript

1. Download `ghostscript-X.Y.Z.tar.xz` and `SHA512SUMS` from the
   [releases page](https://github.com/ArtifexSoftware/ghostpdl-downloads/releases)
   and check them with `shasum -a 512 -c`.
2. Extract it into `src/`, set `GS_VERSION` in `build.sh`, and update the
   version expected in `test/smoke.test.mjs`.
3. `./build.sh && npm test`

## License

Ghostscript is © Artifex Software and licensed under the
[GNU AGPL v3](src/ghostscript-10.08.0/LICENSE) (commercial licenses are
available from Artifex). This build, and anything you distribute containing
`gs.wasm`, is subject to the AGPL.
