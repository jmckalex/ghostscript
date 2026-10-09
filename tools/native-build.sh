#!/usr/bin/env bash
# Native debug build of the vendored Ghostscript (with AddressSanitizer), for
# developing devices such as svg: fast rebuilds and real stack traces.
#
# It builds in build/native/, a mirror of the source tree made of symlinks,
# so it doesn't disturb the in-tree WASM build and picks up source edits.
#
#   tools/native-build.sh               configure (first time) + build
#   tools/native-build.sh reconfigure   re-mirror (e.g. after adding files)
#   build/native/bin/gs -sDEVICE=svg -o page-%d.svg input.ps
#   node tools/svg-compare.mjs --gs build/native/bin/gs input.ps
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GS_VERSION="10.08.0"
SRC="$ROOT/src/ghostscript-$GS_VERSION"
OUT="$ROOT/build/native"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"
CFLAGS="${CFLAGS:--g -O1 -fsanitize=address -fno-omit-frame-pointer}"
LDFLAGS="${LDFLAGS:--fsanitize=address}"

# Skip the in-tree WASM build products.
prune=( \( -path ./obj -o -path ./bin -o -path ./arch-config -o -path ./tiff-config
           -o -name __pycache__ -o -name 'auxtmp*' \) -prune -o )

if [ "${1:-}" = reconfigure ] || [ ! -f "$OUT/Makefile" ]; then
  rm -rf "$OUT"
  mkdir -p "$OUT"
  (cd "$SRC" && find . "${prune[@]}" -type d -print) |
    while IFS= read -r d; do mkdir -p "$OUT/$d"; done
  (cd "$SRC" && find . "${prune[@]}" -type f -print) |
    grep -v -E '^\./(Makefile|config\.(log|status)|configaux\.log|a\.wasm|auxflags\.mak)$' |
    while IFS= read -r f; do ln -s "$SRC/${f#./}" "$OUT/$f"; done
  # pdfwrite is needed anyway: the PDF interpreter links code it brings in.
  (cd "$OUT" && ./configure --with-drivers=svg,png16m,pngalpha,txtwrite,bbox,pdfwrite \
      --disable-threading --disable-contrib --disable-cups --disable-dbus \
      --disable-fontconfig --disable-gtk --without-x --without-tesseract \
      --without-libidn --without-libpaper --without-pdftoraster --without-ijs \
      --without-urf CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" > configure.log)
fi
make -C "$OUT" -j"$JOBS" > "$OUT/make.log" 2>&1 || { tail -30 "$OUT/make.log"; exit 1; }
echo "Built $OUT/bin/gs"
