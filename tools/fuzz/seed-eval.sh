#!/bin/bash
# seed-eval.sh OUTDIR [JS...] : seeds for fuzz-eval from JS files (default: the differential corpus). Each file becomes three
# inputs, by the harness's first byte: script (0), strict script (2), compile only (4: the compiler's passes without running).
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
o=$1; shift; mkdir -p "$o"
[ $# -eq 0 ] && set -- "$ROOT"/tests/differential/corpus/*.js
for f in "$@"; do
  b=$(basename "$f" .js)
  for m in 0 2 4; do { printf "\\$(printf '%03o' $m)"; cat "$f"; } > "$o/$b.m$m"; done
done
ls "$o" | wc -l
