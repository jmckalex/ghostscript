# Ghostscript → WebAssembly

[Ghostscript](https://ghostscript.com) 10.08.0 (released 2026-09-08), built to
WebAssembly with Emscripten. It runs in browsers, web workers and Node.

It also adds something upstream Ghostscript doesn't have: an **`svg` output
device** that turns PostScript, EPS and PDF into high-quality SVG (see
[SVG output](#svg-output)), and a [browser demo](#demo) of it.

The upstream source is vendored in `src/ghostscript-10.08.0` (downloaded
from the [Artifex release][rel] and SHA-512 verified against its
`SHA512SUMS`). It was committed unmodified, so `git diff` against the import
commit shows exactly what changed: one new file,
`devices/vector/gdevsvg.c`, plus its rule in `devices/devs.mak`. The rest of
the port is `build.sh` (configure flags and Emscripten link settings) and
the JS wrappers in `js/`.

[rel]: https://github.com/ArtifexSoftware/ghostpdl-downloads/releases/tag/gs10080

## Layout

| Path | What |
|---|---|
| `src/ghostscript-10.08.0/` | upstream source (built in-tree) |
| `build.sh` | configure + build + copy to `dist/` |
| `js/ghostscript.mjs` | library API: `runGhostscript(args, {files})` |
| `js/gs-cli.mjs` | Node CLI that behaves like the native `gs` |
| `demo/` | browser demo: PostScript/PDF → SVG (`npm run demo`) |
| `test/` | tests (`npm test`) |
| `tools/svg-compare.mjs` | pixel comparison of SVG output against Ghostscript's raster output |
| `tools/native-build.sh` | native debug (ASan) build, for working on the device |
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

- **Devices:** `--with-drivers=FILES,svg`, i.e. all file-format devices:
  `pdfwrite`, `ps2write`, `eps2write`, `txtwrite`, `docxwrite`, `png*`,
  `jpeg*`, `tiff*`, `bmp*`, `pnm`/`pam`, `pcx*`, `psd*`, `bbox`, `inkcov`, …,
  plus the new `svg`. Printer drivers are left out.
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

## SVG output

```sh
node dist/gs-cli.mjs -sDEVICE=svg -o page-%d.svg input.pdf
```

Ghostscript once had an experimental `svgwrite` device. It never produced
reliable output and was removed in March 2014 (9.10 was the last release
with it), with Artifex pointing users at MuPDF for SVG instead. The `svg`
device here is a new implementation, built on the graphics library's
high-level device interface:

- **Paths, strokes and clipping are native SVG at full precision.** Path
  data uses relative commands; stroke width, caps, joins, miter limit and
  dashes are carried over. A non-uniformly scaled pen (an ellipse stroked
  under `4 1 scale`) becomes a path in rescaled user space with the CTM as
  its `transform`. Clips are written from the original clip paths, intersected
  by chaining `<clipPath>`s, so they aren't rounded to pixels. Colors are
  converted to sRGB by Ghostscript's color management, so CMYK, Lab, ICC and
  spot colors come out as in its raster output.
- **Text is vector and selectable.** Each glyph outline (Type 1, CFF,
  TrueType, CID fonts) is defined once per page and drawn with `<use>`. Above
  the page sits an invisible `<text>` layer with the Unicode text at each
  glyph's position, so text can be selected, copied and found with the
  browser's find. This works for invisible text (rendering mode 3, e.g. OCR
  layers) too. Font names choose a serif, sans-serif or monospace family, a
  weight and a style for the layer, so selection boxes fit the glyphs.
- **Images are embedded at their own resolution** (capped at two samples per
  device pixel) as PNG, positioned by their image matrix. Every color space
  and Decode array is handled, stencil masks become alpha in the fill color,
  and color-key masks (ImageType 4) become transparency. Images that are
  enlarged and not interpolated stay crisp (`image-rendering`), as
  Ghostscript draws them.
- **Axial and radial shadings become `linearGradient` / `radialGradient`.**
  The shading function is sampled and converted to sRGB, and only the stops
  needed to stay within one color level are kept. `Extend` and `BBox` are
  respected (by clipping where SVG can't express them directly). Gradient
  fills of text and strokes work too, and identical definitions are shared.
- **Anything SVG can't express is still drawn exactly.** That covers mesh and
  function-based shadings, tiling patterns, ImageType 3 masks, raster
  operations, and transparency (which goes through Ghostscript's compositor).
  The graphics library rasterizes it into an RGBA canvas at the device
  resolution, and that canvas is embedded as a PNG at the right point in the
  painting order. The default resolution is 192 dpi, and `-r` changes it. Vector
  content doesn't depend on the resolution.

Output is one standalone SVG 1.1 file per page, sized in points; use `%d` in
the output file name for multi-page input. Two SVG 2 features are used:
`fr` for radial gradients whose start circle is off-center, and
`vector-effect` for zero-width (hairline) strokes.

| Parameter | Default | |
|---|---|---|
| `-dSVGTextLayer=false` | true | omit the invisible text layer |
| `-dSVGReuseGlyphs=false` | true | write each glyph as a plain path instead of `<use>` |
| `-r<dpi>` | 192 | resolution of rasterized fallback content |

**Fidelity** is checked by `tools/svg-compare.mjs`. It renders each file
with Ghostscript's own `png16m` device and as SVG through
[resvg](https://github.com/RazrFalcon/resvg), and compares the two pixel by
pixel. `npm test` runs it on the samples. On the Ghostscript example files,
typically under 1% of pixels differ noticeably, mostly antialiased edges. The
largest differences are PostScript hairlines, which raster devices widen to
a whole pixel and SVG draws at their true width.

## Demo

```sh
npm run demo        # then open http://localhost:8080/demo/
```

Edit PostScript with live re-rendering, or open/drop a PostScript, EPS or
PDF file. Ghostscript runs in a Web Worker, and each page is shown as
inline SVG that you can zoom, select text in, search and download. "Show
text layer" makes the invisible text visible.

## Developing the device

`tools/native-build.sh` makes a native debug build with AddressSanitizer in
`build/native/` (about a minute). It builds from a mirror of the source tree
made of symlinks, so edits to `gdevsvg.c` need only an incremental `make`,
and it doesn't touch the WASM build. Then:

```sh
build/native/bin/gs -sDEVICE=svg -o page-%d.svg file.ps
node tools/svg-compare.mjs --gs build/native/bin/gs --out /tmp/cmp file.ps
```

`--out` writes reference | SVG | difference images for each page.

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
