#!/bin/bash
#
# stale-test.sh -- the P7 stale-unit test (plan 9.5).
#
# A unit is keyed by the 128-bit content digest of each function's register-VM
# bytecode. Ship a bundle that changes one function and the unit must stay
# correct: the changed function finds no table entry and runs in the
# interpreter, every unchanged function still binds.
#
# This builds the engine + a unit generated from bundle A, then runs that same
# binary against bundle B (A with one function's body changed) and against A.
# Both outputs must equal the plain interpreter's, and the changed function
# must be absent from the bound set.
#
# Usage: tools/aot/stale-test.sh
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
CLANG=${CLANG:-clang}
S=engine/quickjs-ng
O=${AOT_OUT:-${TMPDIR:-/tmp}/qjs-aot-stale}
BASE="-g -std=gnu11 -D_GNU_SOURCE -DJS_ENABLE_DEBUGGER=1 -I$S"
mkdir -p "$O/obj"
fail=0

log() { echo "== $* =="; }

# --- bundles ----------------------------------------------------------------
# bundle A: three functions, all called from a hot loop.
cat > "$O/bundleA.js" <<'EOF'
function f1(x) { return x + 1; }
function f2(x) { return x * 3; }
function f3(x) { return (x | 0) - 2; }
var s = 0;
for (var i = 0; i < 2000; i++) s = (s + f1(i) + f2(i) + f3(i)) | 0;
print("stale", s);
EOF
# bundle B: identical except f2's body (and therefore its digest) changed.
sed 's/return x \* 3;/return x * 3 + 100;/' "$O/bundleA.js" > "$O/bundleB.js"
if cmp -s "$O/bundleA.js" "$O/bundleB.js"; then
  echo "stale-test: bundle B did not change (sed no-op)"; exit 2
fi

# --- build ------------------------------------------------------------------
log "build generator + engine + plain interpreter"
"$CLANG" -O2 $BASE -DQJS_REGVM=1 -DQJS_AOT_GEN=1 -o "$O/qjsaot" \
  tools/aot/qjsaot.c $S/quickjs.c $S/libregexp.c $S/libunicode.c $S/dtoa.c -lm || exit 1
"$CLANG" -O2 $BASE -DQJS_REGVM=1 -DQJS_AOT=1 -c $S/quickjs.c -o "$O/obj/quickjs.o" || exit 1
for f in libregexp libunicode dtoa; do
  "$CLANG" -O2 $BASE -c $S/$f.c -o "$O/obj/$f.o" || exit 1
done
"$CLANG" -O2 $BASE -c tools/benchmark/qjs-bench.c -o "$O/obj/qjs-bench.o" || exit 1
"$CLANG" -o "$O/plain-run" "$O/obj/qjs-bench.o" \
  "$O/obj/quickjs.o" "$O/obj/libregexp.o" "$O/obj/libunicode.o" "$O/obj/dtoa.o" -lm || exit 1

log "generate unit from bundle A"
"$O/qjsaot" --out "$O/unitA.c" --all "$O/bundleA.js" 2>"$O/unitA.census" || exit 1
"$CLANG" -O0 $BASE -ffp-contract=off -DQJS_REGVM=1 -DQJS_AOT=1 -c "$O/unitA.c" -o "$O/obj/unitA.o" || exit 1
"$CLANG" -o "$O/bench-A" "$O/obj/qjs-bench.o" "$O/obj/unitA.o" \
  "$O/obj/quickjs.o" "$O/obj/libregexp.o" "$O/obj/libunicode.o" "$O/obj/dtoa.o" -lm || exit 1

# --- run --------------------------------------------------------------------
log "run plain interpreter on both bundles"
"$O/plain-run" "$O/bundleA.js" > "$O/plainA.out" 2>&1
"$O/plain-run" "$O/bundleB.js" > "$O/plainB.out" 2>&1

log "run the bundle-A unit against bundle A (all bind)"
QJS_AOT_STATS=1 "$O/bench-A" "$O/bundleA.js" > "$O/aotA.out" 2> "$O/aotA.err"
log "run the bundle-A unit against bundle B (f2 stale)"
QJS_AOT_STATS=1 "$O/bench-A" "$O/bundleB.js" > "$O/aotB.out" 2> "$O/aotB.err"

# --- check ------------------------------------------------------------------
chk() { # chk <label> <plain> <aot>
  if diff -q "$2" "$3" >/dev/null 2>&1; then
    echo "PASS $1: output identical ($(head -1 "$3"))"
  else
    echo "FAIL $1: output differs"; diff "$2" "$3" | head; fail=1
  fi
}
chk "unit-A vs A" "$O/plainA.out" "$O/aotA.out"
chk "unit-A vs B (stale)" "$O/plainB.out" "$O/aotB.out"

# The stats line (stderr) reports how many table entries bound.
boundA=$(sed -n 's/.*bound=\([0-9]*\).*/\1/p' "$O/aotA.err" | tail -1)
boundB=$(sed -n 's/.*bound=\([0-9]*\).*/\1/p' "$O/aotB.err" | tail -1)
table=$(sed -n 's/.*table=\([0-9]*\).*/\1/p' "$O/aotA.err" | tail -1)
echo "table=$table bound(A)=$boundA bound(B)=$boundB"
if [ "$boundA" = "$table" ] && [ "$boundB" -lt "$boundA" ] 2>/dev/null; then
  echo "PASS stale: changed function did not bind, the rest did"
else
  echo "FAIL stale: expected bound(B) < bound(A) == table"; fail=1
fi

echo "stale-test: $([ $fail = 0 ] && echo ok || echo FAILED)"
exit $fail
