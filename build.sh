#!/usr/bin/env bash
# Build Ghostscript to WebAssembly with Emscripten.
#
#   ./build.sh            configure (if needed) + build + copy to dist/
#   ./build.sh clean      remove build products from the source tree
#   ./build.sh reconfigure  force a fresh configure, then build
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
DIST="$ROOT/dist"
OPT="${OPT:--O2}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"

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

cd "$SRC"

case "${1:-}" in
  clean)
    make distclean >/dev/null 2>&1 || true
    rm -rf "$DIST"
    exit 0
    ;;
  reconfigure)
    make distclean >/dev/null 2>&1 || true
    ;;
esac

# --- configure --------------------------------------------------------------
CONFIGURE_FLAGS=(
  --host=wasm32-unknown-emscripten
  --build="$(./config.guess)"
  --with-exe-ext=.js
  --with-drivers=FILES          # file-format devices: pdf/ps/png/jpeg/tiff/...
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

# Emscripten link settings. The module is an ES6 factory; each call creates a
# fresh Ghostscript instance (main() is not re-entrant, so do one run per
# instance). NODEFS is linked so the Node CLI wrapper can mount the real disk.
EM_LDFLAGS=(
  -sMODULARIZE=1
  -sEXPORT_ES6=1
  -sEXPORT_NAME=createGhostscript
  -sENVIRONMENT=web,worker,node
  -sINVOKE_RUN=0
  -sEXIT_RUNTIME=1
  -sFORCE_FILESYSTEM=1
  -sEXPORTED_RUNTIME_METHODS=FS,NODEFS,callMain
  -sALLOW_MEMORY_GROWTH=1
  -sINITIAL_MEMORY=64MB
  -sMAXIMUM_MEMORY=4GB
  -sSTACK_SIZE=8MB
  -lnodefs.js
)

if [ ! -f Makefile ]; then
  emconfigure ./configure "${CONFIGURE_FLAGS[@]}" \
    CCAUX="$CCAUX" \
    CFLAGS="$OPT" \
    LDFLAGS="$OPT ${EM_LDFLAGS[*]}"
fi

# --- build ------------------------------------------------------------------
emmake make -j"$JOBS"

mkdir -p "$DIST"
cp bin/gs.js bin/gs.wasm "$ROOT"/js/*.mjs "$DIST/"
chmod +x "$DIST/gs-cli.mjs"
ls -lh "$DIST"
