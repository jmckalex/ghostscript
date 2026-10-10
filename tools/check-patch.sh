#!/usr/bin/env bash
# Prove the patch from tools/make-patch.sh builds a native Ghostscript from the
# stock release, as a user would: extract the release source (the import
# commit is byte-identical to the tarball), apply the patch, run a plain
# ./configure && make, and convert a file with the svg device.
#
#   tools/check-patch.sh     works in build/patch-check/
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GS_VERSION="10.08.0"
TREE="src/ghostscript-$GS_VERSION"
DIR="$ROOT/build/patch-check"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"
PATCH="$DIR/ghostscript-$GS_VERSION-svg.patch"
GS="$DIR/ghostscript-$GS_VERSION"

IMPORT="$(git -C "$ROOT" rev-list --reverse HEAD -- "$TREE" | head -1)"
rm -rf "$DIR"
mkdir -p "$DIR"
git -C "$ROOT" archive "$IMPORT" "$TREE" | tar -x -C "$DIR" --strip-components=1
"$ROOT/tools/make-patch.sh" "$PATCH" >/dev/null

cd "$GS"
patch -p1 --quiet < "$PATCH"
# On a Mac with Homebrew's fontconfig, the stock build finds the library but
# compiles its build tools without the headers; Ghostscript doesn't need it.
CONFIG=()
[ "$(uname)" = Darwin ] && CONFIG=(--disable-fontconfig)
echo "Patched; configuring and building in $GS (logs: configure.log, make.log)"
./configure ${CONFIG[@]+"${CONFIG[@]}"} > configure.log 2>&1 || { tail -20 configure.log; exit 1; }
make -j"$JOBS" > make.log 2>&1 || { tail -30 make.log; exit 1; }

bin/gs -h | tr ' ' '\n' | grep -qx svg || { echo "svg is not in gs -h's device list" >&2; exit 1; }
bin/gs -q -dSAFER -dBATCH -dNOPAUSE -sDEVICE=svg -dEPSCrop -o "$DIR/tiger.svg" examples/tiger.eps
grep -q '<svg ' "$DIR/tiger.svg" || { echo "no SVG written" >&2; exit 1; }
echo "OK: $(bin/gs --version), svg device in the default build; wrote $DIR/tiger.svg ($(wc -c < "$DIR/tiger.svg" | tr -d ' ') bytes)"
