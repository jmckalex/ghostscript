#!/usr/bin/env bash
# Native builds of the vendored Ghostscript, from mirrors of the source tree
# (see tools/mirror-source.sh), so they don't disturb the WASM build and pick
# up source edits.
#
#   tools/native-build.sh [debug]   debug build with AddressSanitizer, for
#                                   developing devices such as svg:
#                                   build/native/bin/gs
#   tools/native-build.sh lib       optimized shared library, e.g. as a native
#                                   reference for software embedding libgs:
#                                   build/native-lib/sobin/libgs.dylib (.so).
#                                   Built with -ffp-contract=off: no fused
#                                   multiply-adds, which WASM doesn't have, so
#                                   it computes the same coordinates as gs.wasm
#   tools/native-build.sh MODE reconfigure   re-mirror and configure afresh
#
#   build/native/bin/gs -sDEVICE=svg -o page-%d.svg input.ps
#   node tools/svg-compare.mjs --gs build/native/bin/gs input.ps
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GS_VERSION="10.08.0"
SRC="$ROOT/src/ghostscript-$GS_VERSION"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"
MODE="${1:-debug}"
[ "$MODE" = reconfigure ] && { MODE=debug; set -- debug reconfigure; }

case "$MODE" in
  debug)
    OUT="$ROOT/build/native"
    CFLAGS="${CFLAGS:--g -O1 -fsanitize=address -fno-omit-frame-pointer}"
    LDFLAGS="${LDFLAGS:--fsanitize=address}"
    TARGET=
    ;;
  lib)
    OUT="$ROOT/build/native-lib"
    CFLAGS="${CFLAGS:--O2 -ffp-contract=off}"
    LDFLAGS="${LDFLAGS:-}"
    TARGET=so
    ;;
  *)
    echo "usage: $0 [debug|lib] [reconfigure]" >&2
    exit 2
    ;;
esac

if [ "${2:-}" = reconfigure ] || [ ! -f "$OUT/Makefile" ]; then
  "$ROOT/tools/mirror-source.sh" "$SRC" "$OUT"
  (cd "$OUT" && ./configure --with-drivers=svg,png16m,pngalpha,txtwrite,bbox,pdfwrite \
      --disable-threading --disable-contrib --disable-cups --disable-dbus \
      --disable-fontconfig --disable-gtk --without-x --without-tesseract \
      --without-libidn --without-libpaper --without-pdftoraster --without-ijs \
      --without-urf CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" > configure.log)
fi
make -C "$OUT" -j"$JOBS" $TARGET > "$OUT/make.log" 2>&1 || { tail -30 "$OUT/make.log"; exit 1; }
if [ "$MODE" = lib ]; then
  ls -l "$OUT"/sobin/libgs*
else
  echo "Built $OUT/bin/gs"
fi
