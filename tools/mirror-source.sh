#!/usr/bin/env bash
# Make DEST a mirror of the Ghostscript source tree SRC: real directories,
# with a symlink for each file. Ghostscript builds in its source tree, so this
# gives extra build configurations (native, lean WASM) their own tree while
# sharing, and picking up edits to, the sources. The in-tree WASM build's
# products are skipped.
#
#   tools/mirror-source.sh SRC DEST
set -euo pipefail

SRC="$(cd "$1" && pwd)"
DEST="$2"

prune=( \( -path ./obj -o -path ./bin -o -path ./arch-config -o -path ./tiff-config
           -o -name __pycache__ -o -name 'auxtmp*' \) -prune -o )

rm -rf "$DEST"
mkdir -p "$DEST"
(cd "$SRC" && find . "${prune[@]}" -type d -print) |
  while IFS= read -r d; do mkdir -p "$DEST/$d"; done
(cd "$SRC" && find . "${prune[@]}" -type f -print) |
  grep -v -E '^\./(Makefile|config\.(log|status)|configaux\.log|a\.wasm|auxflags\.mak)$' |
  while IFS= read -r f; do ln -s "$SRC/${f#./}" "$DEST/$f"; done
