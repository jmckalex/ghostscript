#!/usr/bin/env bash
# Write the svg device as a patch against the stock Ghostscript release: what
# this repository changes in src/ghostscript-$GS_VERSION (in the working
# tree), compared with the commit that imported the release tarball
# unmodified. It applies to the release source with `patch -p1` (or `git
# apply`), after which a plain ./configure && make builds a native gs with
# -sDEVICE=svg.
#
#   tools/make-patch.sh [OUT]    default: build/ghostscript-10.08.0-svg.patch
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GS_VERSION="10.08.0"
TREE="src/ghostscript-$GS_VERSION"
OUT="${1:-$ROOT/build/ghostscript-$GS_VERSION-svg.patch}"

# The first commit to touch the tree is the import of the release tarball.
IMPORT="$(git -C "$ROOT" rev-list --reverse HEAD -- "$TREE" | head -1)"
HEAD="$(git -C "$ROOT" rev-parse --short HEAD)"
git -C "$ROOT" diff --quiet HEAD -- "$TREE" || HEAD="$HEAD-dirty"

mkdir -p "$(dirname "$OUT")"
{
  cat <<EOF
The svg output device for Ghostscript $GS_VERSION, from
https://jmckalex.org/software/ghostscript/ (commit $HEAD).

Apply it to the release source and build:

  tar xf ghostscript-$GS_VERSION.tar.xz
  cd ghostscript-$GS_VERSION
  patch -p1 < ghostscript-$GS_VERSION-svg.patch
  ./configure && make
  bin/gs -sDEVICE=svg -o page-%d.svg input.pdf

The device is in the default build whenever libpng is available (it embeds
images as PNG); --with-drivers=SVG, or svg, selects it on its own. Its
documentation is in doc/src/VectorDevices.rst. Licence: AGPL-3.0, as
Ghostscript.

EOF
  git -C "$ROOT" diff --stat=72 --relative="$TREE/" "$IMPORT" -- "$TREE"
  echo
  git -C "$ROOT" diff --relative="$TREE/" "$IMPORT" -- "$TREE"
} > "$OUT"
echo "$OUT"
