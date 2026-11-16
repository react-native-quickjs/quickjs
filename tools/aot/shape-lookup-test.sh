#!/bin/bash
# shape-lookup-test.sh -- the AOT shape lookups hash atoms as the shape table does.
# Builds the engine with the AOT runtime and runs tools/aot/shape-lookup-test.c.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
CLANG=${CLANG:-clang}
S=engine/quickjs-ng
O=${AOT_OUT:-${TMPDIR:-$ROOT/.local/scratch}/qjs-aot-shape-lookup}
rm -rf "$O"; mkdir -p "$O"
"$CLANG" -O1 -g -std=gnu11 -D_GNU_SOURCE -DJS_ENABLE_DEBUGGER=0 -DQJS_REGVM=1 -DQJS_AOT=1 -w -I$S \
  tools/aot/shape-lookup-test.c $S/quickjs.c $S/libregexp.c $S/libunicode.c $S/dtoa.c -lm -o "$O/t" || { echo "FAIL build"; exit 1; }
"$O/t"
