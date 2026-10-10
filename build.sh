#!/usr/bin/env bash
# Build Ghostscript to WebAssembly with Emscripten.
#
#   ./build.sh               configure (if needed) + build + copy to dist/
#   ./build.sh clean         remove build products
#   ./build.sh reconfigure   force a fresh configure, then build
#
# Variants (VARIANT=...):
#   full (default)  all file-format output devices + svg; built in the source
#                   tree; output in dist/
#   lean            just the PostScript and PDF interpreters (no output
#                   devices beyond bbox and the null device, and no CJK
#                   fallback font), for use through the gsapi_* API, e.g. by
#                   dvisvgm; built in build/wasm-lean/; output in dist/lean/
#
# Environment overrides:
#   EMSDK_ENV   path to emsdk_env.sh (default: ~/Applications/emsdk/emsdk_env.sh,
#               only used when emcc is not already on PATH)
#   OPT         optimisation flags for compile + link (default: -O2)
#   JOBS        parallel make jobs (default: number of CPUs)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GS_VERSION="10.08.0"
SRC="$ROOT/src/ghostscript-$GS_VERSION"
VARIANT="${VARIANT:-full}"
OPT="${OPT:--O2}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"

case "$VARIANT" in
  full)
    BUILD="$SRC"
    DIST="$ROOT/dist"
    VARIANT_FLAGS=(
      --with-drivers=FILES,svg  # file-format devices (pdf/ps/png/jpeg/tiff/...) + SVG
    )
    MAKE_VARS=()
    ;;
  lean)
    BUILD="$ROOT/build/wasm-lean"
    DIST="$ROOT/dist/lean"
    VARIANT_FLAGS=(
      --with-drivers=bbox       # (the null device is always present)
      --without-libtiff
    )
    # The built-in resources (psi/psromfs.mak) without CIDFSubst/, i.e.
    # without DroidSansFallback, the 3.7MB substitute for missing CJK fonts.
    MAKE_VARS=('PS_RESOURCE_LIST=SubstCID$(D)* CIDFont$(D)* -C $(PDF_RESOURCE_LIST) ColorSpace$(D)* Decoding$(D)* Encoding$(D)* -c -C IdiomSet$(D)* ProcSet$(D)* -P $(PSRESDIR)$(D)Init$(D) -d Resource/Init/ -B $(MISC_INIT_FILES)')
    ;;
  *)
    echo "Unknown VARIANT '$VARIANT' (use full or lean)" >&2
    exit 2
    ;;
esac

# --- toolchain --------------------------------------------------------------
if ! command -v emcc >/dev/null 2>&1; then
  EMSDK_ENV="${EMSDK_ENV:-$HOME/Applications/emsdk/emsdk_env.sh}"
  # shellcheck disable=SC1090
  source "$EMSDK_ENV" >/dev/null 2>&1
fi
echo "Using $(emcc --version | head -1)"

# Native compiler for the build-time helper programs (genconf, mkromfs, ...)
# that run on this machine while generating sources. Use the /usr/bin shim on
# macOS: it locates the SDK, whereas the toolchain's clang invoked directly
# fails to link ("library 'System' not found").
CCAUX="${CCAUX:-$( [ -x /usr/bin/clang ] && echo /usr/bin/clang || command -v cc)}"

case "${1:-}" in
  clean)
    if [ "$BUILD" = "$SRC" ]; then
      (cd "$SRC" && make distclean >/dev/null 2>&1) || true
    else
      rm -rf "$BUILD"
    fi
    rm -rf "$DIST"
    exit 0
    ;;
  reconfigure)
    if [ "$BUILD" = "$SRC" ]; then
      (cd "$SRC" && make distclean >/dev/null 2>&1) || true
    else
      rm -rf "$BUILD"
    fi
    ;;
esac

# Variants other than full build in their own mirror of the source tree.
if [ "$BUILD" != "$SRC" ] && [ ! -f "$BUILD/Makefile" ]; then
  "$ROOT/tools/mirror-source.sh" "$SRC" "$BUILD"
fi
cd "$BUILD"

# --- configure --------------------------------------------------------------
CONFIGURE_FLAGS=(
  --host=wasm32-unknown-emscripten
  --build="$(./config.guess)"
  --with-exe-ext=.js
  "${VARIANT_FLAGS[@]}"
  --disable-threading           # no pthreads; single-threaded rendering
  --disable-contrib
  --disable-cups
  --disable-dbus
  --disable-fontconfig
  --disable-gtk
  --disable-sse2
  --disable-neon
  --without-x
  --without-tesseract           # OCR: large, and needs tessdata at runtime
  --without-libidn
  --without-libpaper
  --without-pdftoraster
  --without-ijs
  --without-urf
  --with-libiconv=no
)

# The Ghostscript library API, for embedding the interpreter (as dvisvgm does)
# rather than running it as a command.
GSAPI=(
  revision new_instance delete_instance set_stdio set_stdio_with_handle set_poll
  set_poll_with_handle set_display_callback register_callout deregister_callout
  set_default_device_list get_default_device_list set_arg_encoding init_with_args
  run_string_begin run_string_continue run_string_end run_string_with_length
  run_string run_file exit set_param get_param enumerate_params add_control_path
  remove_control_path purge_control_paths activate_path_control
  is_path_control_active add_fs remove_fs
)
EXPORTED_FUNCTIONS="_main,_malloc,_free$(printf ',_gsapi_%s' "${GSAPI[@]}")"

# Emscripten link settings. The module is an ES6 factory. main() is not
# re-entrant, so for command-line use (callMain) make one instance per run.
# NODEFS is linked so the Node CLI wrapper can mount the real disk; PROXYFS
# so another module (e.g. dvisvgm) can share its files with Ghostscript.
EM_LDFLAGS=(
  -sMODULARIZE=1
  -sEXPORT_ES6=1
  -sEXPORT_NAME=createGhostscript
  -sENVIRONMENT=web,worker,node
  -sINVOKE_RUN=0
  -sEXIT_RUNTIME=1
  -sFORCE_FILESYSTEM=1
  -sEXPORTED_FUNCTIONS="$EXPORTED_FUNCTIONS"
  -sEXPORTED_RUNTIME_METHODS=FS,NODEFS,PROXYFS,callMain,HEAPU8,UTF8ToString,stringToUTF8,lengthBytesUTF8,addFunction,removeFunction
  -sALLOW_TABLE_GROWTH=1
  -sALLOW_MEMORY_GROWTH=1
  -sINITIAL_MEMORY=64MB
  -sMAXIMUM_MEMORY=4GB
  -sSTACK_SIZE=8MB
  -lnodefs.js
  -lproxyfs.js
)

if [ ! -f Makefile ]; then
  emconfigure ./configure "${CONFIGURE_FLAGS[@]}" \
    CCAUX="$CCAUX" \
    CFLAGS="$OPT" \
    LDFLAGS="$OPT"
fi

# --- build ------------------------------------------------------------------
# The Emscripten settings go only to the final link (XLDFLAGS), not to
# configure, whose test programs don't define the exported functions. Relink
# every time so changes to them take effect.
rm -f bin/gs.js bin/gs.wasm
if [ "${#MAKE_VARS[@]}" -gt 0 ]; then
  rm -f obj/gsromfs1*       # the ROM file system depends on MAKE_VARS
fi
emmake make -j"$JOBS" XLDFLAGS="${EM_LDFLAGS[*]}" ${MAKE_VARS[@]+"${MAKE_VARS[@]}"}

mkdir -p "$DIST"
cp bin/gs.js bin/gs.wasm "$ROOT"/js/*.mjs "$DIST/"
chmod +x "$DIST/gs-cli.mjs"
ls -lh "$DIST"
