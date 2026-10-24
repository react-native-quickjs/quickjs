#!/usr/bin/env bash
#
# layout-audit.sh -- the AOT layout hash must not depend on the build type.
#
# A field that exists only under -DNDEBUG changes the size of a struct the
# generated unit reads an offset of. The unit is then built against one layout
# and refused by an engine with another, which shows up as a program that runs
# in the interpreter with no diagnostic. The hash is computed twice, with and
# without -DNDEBUG, and the two must be equal.
#
# Prints one line on success; the two hashes and a diff on failure. Exits
# non-zero on a mismatch or a build failure.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
S=$ROOT/engine/quickjs-ng
CC=${CC:-clang}
W=$(mktemp -d "${TMPDIR:-/tmp}/qjs-layout-XXXXXX")
trap 'rm -rf "$W"' EXIT

hash_for() {
  local defs=$1 dir=$W/$2
  mkdir -p "$dir"
  # shellcheck disable=SC2086
  "$CC" -O1 -std=gnu11 -D_GNU_SOURCE -DJS_ENABLE_DEBUGGER=0 $defs \
      -DQJS_REGVM=1 -DQJS_AOT_GEN=1 -I"$S" \
      "$ROOT"/tools/aot/qjsaot/*.c "$S/quickjs.c" "$S/libregexp.c" \
      "$S/libunicode.c" "$S/dtoa.c" -o "$dir/gen" -lm >/dev/null 2>&1 ||
    { echo "build failed ($2)" >&2; return 1; }
  "$dir/gen" --out "$dir/u.c" --layout-header "$dir/u-l.h" \
      --report "$dir/r.txt" --hot-cover 0.99 \
      "$ROOT/tools/aot/kernels2/01-arith.js" >/dev/null 2>&1 ||
    { echo "generation failed ($2)" >&2; return 1; }
  sed -n 's/^#define QJS_AOT_LAYOUT_HASH \([0-9]*\)u$/\1/p' "$dir/u-l.h"
}

rel=$(hash_for "-DNDEBUG" ndebug)
dbg=$(hash_for "" plain)
if [ -z "$rel" ] || [ -z "$dbg" ]; then
  echo "layout-audit: could not read a layout hash (release='$rel' plain='$dbg')" >&2
  exit 1
fi
if [ "$rel" != "$dbg" ]; then
  echo "layout-audit: the AOT layout depends on -DNDEBUG" >&2
  echo "  release (-DNDEBUG): $rel" >&2
  echo "  plain            : $dbg" >&2
  echo "  A field that exists only under one of them changes a struct size the" >&2
  echo "  unit reads an offset of. Make it unconditional or outside the layout." >&2
  exit 1
fi
echo "layout hash ${rel} in both -DNDEBUG and plain builds"
