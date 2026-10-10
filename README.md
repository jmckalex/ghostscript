# Ghostscript → WebAssembly

[Ghostscript](https://ghostscript.com) 10.08.0 (released 2026-09-08), built to
WebAssembly with Emscripten. It runs in browsers, web workers and Node.

It also adds something upstream Ghostscript doesn't have: an **`svg` output
device** that turns PostScript, EPS and PDF into high-quality SVG (see
[SVG output](#svg-output)), and a [browser demo](#demo) of it.

The upstream source is vendored in `src/ghostscript-10.08.0` (downloaded
from the [Artifex release][rel] and SHA-512 verified against its
`SHA512SUMS`). It was committed unmodified, so `git diff` against the import
commit shows exactly what changed: one new file, `devices/vector/gdevsvg.c`;
its build rules (`devices/devs.mak`, and the device lists in `configure`,
`configure.ac` and `psi/msvc.mak`); its documentation
(`doc/src/VectorDevices.rst` and `Devices.rst`); and a two-line fix in
`pdf/pdf.mak`. That diff is also a patch that adds the device to a
[native Ghostscript](#native-ghostscript). The rest of the port is
`build.sh` (configure flags and Emscripten link settings) and the JS wrappers
in `js/`.

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
| `tools/make-transparency-pdf.mjs` | writes the transparency test PDF (`demo/samples/transparency.pdf`) |
| `tools/native-build.sh` | native builds: `release` `gs`, debug (ASan) `gs`, or `lib` for `libgs.dylib` |
| `tools/make-patch.sh` | the svg device as a patch against the Ghostscript release |
| `tools/check-patch.sh` | builds the release + patch natively, as a user would, and tries the device |
| `tools/mirror-source.sh` | symlink mirror of the source tree, for out-of-tree builds |
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

**Variants.** `VARIANT=lean ./build.sh` builds just the PostScript and PDF
interpreters, for embedding through the [library API](#library-api-gsapi).
It has no output devices beyond `bbox` and the null device, no libtiff, and
no DroidSansFallback (the built-in substitute for missing CJK fonts). It
builds in `build/wasm-lean/`, a mirror of the source tree, and writes to
`dist/lean/`.

| | `gs.wasm` | gzip | brotli |
|---|---|---|---|
| full | 16.5 MB | 11.3 MB | 10.7 MB |
| lean | 12.5 MB | 8.6 MB | 8.1 MB |

Most of what remains is the built-in resources (fonts, CMaps, init files) and
the two interpreters.

**Native builds.** See [Native Ghostscript](#native-ghostscript) for a native
`gs` with the device. `tools/native-build.sh lib` builds an optimized
`build/native-lib/sobin/libgs.dylib` from the same source, e.g. as a native
reference for software that embeds Ghostscript.

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

### Library API (gsapi)

Both variants export Ghostscript's C API (`gsapi_revision`,
`gsapi_new_instance`, `gsapi_set_stdio`, `gsapi_init_with_args`,
`gsapi_run_string_begin` / `_continue` / `_end`, `gsapi_run_file`,
`gsapi_exit`, `gsapi_delete_instance`, and the rest of `psi/iapi.h`), with
`_malloc`/`_free`. The runtime methods `HEAPU8`, `UTF8ToString`,
`stringToUTF8`, `lengthBytesUTF8` and `addFunction`/`removeFunction` are
exported too, so callbacks such as stdio can be passed as function pointers.
This is how dvisvgm uses Ghostscript, and `test/gsapi.test.mjs` drives it the
same way:

```js
import createGhostscript from './dist/gs.js';
const gs = await createGhostscript();
const stdout = gs.addFunction((handle, str, len) => {
  process.stdout.write(gs.HEAPU8.subarray(str, str + len));
  return len;
}, 'iiii');
// gsapi_new_instance(&inst, NULL); gsapi_set_stdio(inst, stdin, stdout, stderr);
// gsapi_init_with_args(inst, argc, argv); gsapi_run_string_*(...); gsapi_exit(inst);
// gsapi_delete_instance(inst)
```

One instance exists at a time, but a module can go through any number of
`new_instance` … `delete_instance` cycles, one per document. The test runs
30 in one module, and the heap stops growing after the first few. `PROXYFS`
is linked, so another Emscripten module's files can be mounted into
Ghostscript's file system:
`gs.FS.mount(gs.PROXYFS, { root: '/work', fs: other.FS }, '/work')`.

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
- **PDF transparency is native SVG.** The device takes the place of
  Ghostscript's transparency compositor (as pdfwrite does), so constant
  alpha becomes `fill-opacity` / `stroke-opacity`, blend modes become
  `mix-blend-mode`, and transparency groups become `<g>` elements with
  opacity and isolation. Soft masks become `<mask>` elements, luminosity or
  alpha, including a backdrop color (`/BC`), and a transfer function
  (`/TR`) becomes a filter on the mask. Images with an SMask are drawn
  through a mask too. Knockout groups are drawn as ordinary groups.
- **Anything SVG can't express is still drawn exactly.** That covers mesh and
  function-based shadings, radial shadings whose circles aren't nested
  (cones), tiling patterns, ImageType 3 masks, and raster operations.
  The graphics library rasterizes it into an RGBA canvas at the device
  resolution, and that canvas is embedded as a PNG at the right point in the
  painting order. The default resolution is 192 dpi, and `-r` changes it. Vector
  content doesn't depend on the resolution.

Output is one standalone SVG 1.1 file per page, sized in points; use `%d` in
the output file name for multi-page input. A few SVG 2 and CSS features that
browsers support are used: `mix-blend-mode`, `isolation` and `mask-type` for
transparency, `fr` for radial gradients whose start circle has a radius and
is off-center, `vector-effect` for zero-width (hairline) strokes, and
`image-rendering` for crisp pixels. The user documentation is the "SVG
output" section of `src/ghostscript-10.08.0/doc/src/VectorDevices.rst`.

| Parameter | Default | |
|---|---|---|
| `-dSVGTextLayer=false` | true | omit the invisible text layer |
| `-dSVGReuseGlyphs=false` | true | write each glyph as a plain path instead of `<use>` |
| `-dSVGTransparency=false` | true | let Ghostscript's compositor flatten transparency to images |
| `-r<dpi>` | 192 | resolution of rasterized fallback content |

**Fidelity** is checked by `tools/svg-compare.mjs`. It renders each file
with Ghostscript's own `png16m` device and as SVG through
[resvg](https://github.com/RazrFalcon/resvg), and compares the two pixel by
pixel. `npm test` runs it on the samples. On the Ghostscript example files,
typically under 1% of pixels differ noticeably, mostly antialiased edges. The
largest differences are PostScript hairlines, which raster devices widen to
a whole pixel and SVG draws at their true width.

## Native Ghostscript

The `svg` device is ordinary Ghostscript C, with nothing in it specific to
WebAssembly, so it works in a native Ghostscript too. `tools/make-patch.sh`
writes what this repository changes in the Ghostscript tree as a patch
against the 10.08.0 release (`build/ghostscript-10.08.0-svg.patch`):

```sh
tar xf ghostscript-10.08.0.tar.xz && cd ghostscript-10.08.0
patch -p1 < ghostscript-10.08.0-svg.patch
./configure && make
bin/gs -sDEVICE=svg -o page-%d.svg input.pdf
```

The patch adds the device to configure's file-format devices (`FILES`, part
of the default `ALL`) whenever libpng is available, which it always is with
the bundled copy; `--with-drivers=SVG` selects it on its own. It is in the
Windows device list (`psi/msvc.mak`) too, untested.

On a Mac with Homebrew's fontconfig installed, a stock 10.08.0 build stops
while compiling its build tools (`base/gp_unix.c`: `fontconfig/fontconfig.h`
not found). Configure with `--disable-fontconfig`: Ghostscript's own fonts
are built in.

`tools/check-patch.sh` checks the patch as a user would use it: it extracts
the release from the import commit, applies the patch, configures and builds
it, and converts `tiger.eps` (about a minute and a half on an Apple-silicon
Mac). Its SVG is byte-identical to the WebAssembly build's.
`tools/native-build.sh release` builds the working tree the same way, every
device included but fontconfig, dbus and libidn left out so that it needs
only system libraries, into `build/native-release/bin/gs`.

## Demo

```sh
npm run demo        # then open http://localhost:8080/demo/
```

Edit PostScript with live re-rendering, or open/drop a PostScript, EPS or
PDF file. Ghostscript runs in a Web Worker, and each page is shown as
inline SVG that you can zoom, select text in, search and download. "Show
text layer" makes the invisible text visible.

## Developing the device

`tools/native-build.sh` (or `tools/native-build.sh debug`) makes a native debug build with AddressSanitizer in
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
