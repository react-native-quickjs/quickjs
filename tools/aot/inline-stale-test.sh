#!/bin/bash
#
# inline-stale-test.sh -- a unit generated from bundle v1, run on bundle v2.
#
# A hot call site of v1 inlines its callee. v2 changes the callee, or a fact the unit was specialised
# on, while the caller's register program (and so its digest) stays the same, so the caller still
# binds. The unit must not run v1's inlined copy: the call site tests the callee's bound entry, a
# callee that did not bind (or is another function or closure) takes the generic call, and a changed
# fact is caught at the entry or value check. Every case runs one binary on v1 and on v2 and the
# output must equal the interpreter's on the same bytecode. On v1 the unit must be entered, and for
# the cases that inline, inlined code must run (otherwise the case tests nothing).
#
# Usage: tools/aot/inline-stale-test.sh
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
CLANG=${CLANG:-clang}
LDX=; [ "$(uname)" = Darwin ] && LDX="-Wl,-undefined,dynamic_lookup"
S=engine/quickjs-ng
O=${AOT_OUT:-${TMPDIR:-$(dirname "$0")/../../.local/scratch}/qjs-aot-inline-stale}
BASE="-g -std=gnu11 -D_GNU_SOURCE -DJS_ENABLE_DEBUGGER=0 -I$S"
rm -rf "$O"; mkdir -p "$O/obj"
fail=0
LIBS="$O/obj/libregexp.o $O/obj/libunicode.o $O/obj/dtoa.o -lm $LDX"

echo "== build generator, profiling engine, plain engine, unit engine =="
"$CLANG" -O2 $BASE -DQJS_REGVM=1 -DQJS_AOT_GEN=1 -o "$O/qjsaot" \
  tools/aot/qjsaot/*.c $S/quickjs.c $S/libregexp.c $S/libunicode.c $S/dtoa.c -lm || exit 1
"$CLANG" -O2 $BASE -DQJS_REGVM=1 -DQJS_AOT_PROFILE=1 -c $S/quickjs.c -o "$O/obj/prof.o" || exit 1
"$CLANG" -O2 $BASE -DQJS_REGVM=1 -c $S/quickjs.c -o "$O/obj/plain.o" || exit 1
"$CLANG" -O2 $BASE -DQJS_REGVM=1 -DQJS_AOT=1 -c $S/quickjs.c -o "$O/obj/aot.o" || exit 1
for f in libregexp libunicode dtoa; do "$CLANG" -O2 $BASE -c $S/$f.c -o "$O/obj/$f.o" || exit 1; done
"$CLANG" -O2 $BASE -c tools/benchmark/qjs-bench.c -o "$O/obj/qjs-bench.o" || exit 1
"$CLANG" -O2 $BASE -c tools/bytecode/qjsc.c -o "$O/obj/qjsc.o" || exit 1
"$CLANG" -o "$O/run-prof" "$O/obj/qjs-bench.o" "$O/obj/prof.o" $LIBS || exit 1
"$CLANG" -o "$O/run-plain" "$O/obj/qjs-bench.o" "$O/obj/plain.o" $LIBS || exit 1
"$CLANG" -o "$O/qjsc" "$O/obj/qjsc.o" "$O/obj/plain.o" $LIBS || exit 1

# mk <name> <inlines: 1|0> <v1 source on stdin> <sed script turning v1 into v2>
mk() { mkdir -p "$O/$1"; echo "$2" > "$O/$1/inl"; cat > "$O/$1/v1.js"; sed "$3" "$O/$1/v1.js" > "$O/$1/v2.js"
  cmp -s "$O/$1/v1.js" "$O/$1/v2.js" && { echo "inline-stale-test: $1: v2 equals v1"; exit 2; }; }

# c1: B is a global function; v2 changes only B's body.
mk c1 1 's/return x \* 3;/return x * 3 + 100;/' <<'EOF'
function B(x) { return x * 3; }
function A(x) { return B(x) + 1; }
var s = 0;
for (var i = 0; i < 30000; i++) s = (s + A(i)) | 0;
print("c1", s);
EOF
# c2: B is replaced at run time, in the same bundle.
mk c2 1 's/^print("c2", s);/print("c2", s, 1);/' <<'EOF'
function B(x) { return x * 3; }
function A(x) { return B(x) + 1; }
var s = 0;
for (var i = 0; i < 30000; i++) s = (s + A(i)) | 0;
B = function (x) { return x * 5; };
for (var i = 0; i < 30000; i++) s = (s + A(i)) | 0;
print("c2", s);
EOF
# c3: B reads a closure variable that changes at run time; v2 changes its initial value.
mk c3 1 's/var k = 1;/var k = 2;/' <<'EOF'
(function () {
  var k = 1;
  function B(x) { return x + k; }
  function A(x) { return B(x) + 1; }
  var s = 0;
  for (var i = 0; i < 30000; i++) s = (s + A(i)) | 0;
  k = 7;
  for (var i = 0; i < 30000; i++) s = (s + A(i)) | 0;
  print("c3", s);
})();
EOF
# c4: v2's B reads another closure variable.
mk c4 1 's/return x + k;/return x + j;/' <<'EOF'
(function () {
  var k = 1, j = 10;
  function B(x) { return x + k; }
  function A(x) { return B(x) + 1; }
  var s = 0;
  for (var i = 0; i < 30000; i++) s = (s + A(i)) | 0;
  print("c4", s);
})();
EOF
# c5: two closures of the same code with different captured values reach one call site.
mk c5 0 's/mk(1)/mk(2)/' <<'EOF'
function mk(k) { return function B(x) { return x + k; }; }
function A(B, x) { return B(x) + 1; }
var b1 = mk(1), b2 = mk(100), s = 0;
for (var i = 0; i < 30000; i++) s = (s + A(b1, i)) | 0;
for (var i = 0; i < 30000; i++) s = (s + A(b2, i)) | 0;
print("c5", s);
EOF
# c6: A inlines B and B inlines C; v2 changes only C. The inner site's test fails.
mk c6 1 's/return x \* 3;/return x * 3 + 100;/' <<'EOF'
function C(x) { return x * 3; }
function B(x) { return C(x) + 1; }
function A(x) { return B(x) + 2; }
var s = 0;
for (var i = 0; i < 30000; i++) s = (s + A(i)) | 0;
print("c6", s);
EOF
# c7a: a global B reads changes from an int to a double; B's code is the same.
mk c7a 0 's/var K = 3;/var K = 3.5;/' <<'EOF'
var K = 3;
function B(x) { return x + K; }
function A(x) { return B(x) + 1; }
var s = 0;
for (var i = 0; i < 30000; i++) s = s + A(i);
print("c7a", s);
EOF
# c7b: v2's caller passes B a string where the profile saw only ints.
mk c7b 1 's/s = (s + A(i)) | 0;/s = (s + A(i)) | 0; if (i == 29999) t = A("5");/; s/print("c7b", s);/print("c7b", s, t);/' <<'EOF'
function B(x) { return x + 1; }
function A(x) { return B(x) + 1; }
var s = 0, t = 0;
for (var i = 0; i < 30000; i++) { s = (s + A(i)) | 0; }
print("c7b", s);
EOF

stat() { sed -n "/^unit-stats enters=/{s/.* $2=\([0-9]*\).*/\1/p;q}" "$1"; }
for c in c1 c2 c3 c4 c5 c6 c7a c7b; do
  W=$O/$c
  for v in v1 v2; do "$O/qjsc" "$W/$v.js" "$W/$v.bc" > /dev/null || { echo "FAIL $c: qjsc"; fail=1; continue 2; }; done
  QJS_BENCH_MAX_SECS=0 QJS_AOT_PROFILE_OUT="$W/p.qjsprof" "$O/run-prof" "$W/v1.bc" > /dev/null 2>&1
  "$O/qjsaot" --out "$W/u.c" --layout-header "$W/u-l.h" --profile "$W/p.qjsprof" --hot-cover 0.99 "$W/v1.bc" > "$W/gen.out" 2>&1 \
    || { echo "FAIL $c: generator"; fail=1; continue; }
  "$CLANG" -O2 $BASE -I"$W" -ffp-contract=off -DQJS_REGVM=1 -DQJS_AOT=1 -w -c "$W/u.c" -o "$W/u.o" || { echo "FAIL $c: unit"; fail=1; continue; }
  "$CLANG" -o "$W/run" "$O/obj/qjs-bench.o" "$W/u.o" "$O/obj/aot.o" $LIBS || { echo "FAIL $c: link"; fail=1; continue; }
  ok=1; why=
  for v in v1 v2; do
    "$O/run-plain" "$W/$v.bc" > "$W/plain-$v.out" 2>&1
    QJS_AOT_UNIT_STATS=1 "$W/run" "$W/$v.bc" > "$W/unit-$v.out" 2> "$W/unit-$v.err"
    cmp -s "$W/plain-$v.out" "$W/unit-$v.out" || { ok=0; why="$why $v output differs from the interpreter;"; }
  done
  en=$(stat "$W/unit-v1.err" enters); inl=$(stat "$W/unit-v1.err" inlined)
  [ "${en:-0}" -gt 0 ] || { ok=0; why="$why the v1 unit was never entered;"; }
  if [ "$(cat "$W/inl")" = 1 ] && [ "${inl:-0}" -eq 0 ]; then ok=0; why="$why no inlined code ran on v1;"; fi
  if [ $ok = 1 ]; then echo "PASS $c: v1 and v2 equal the interpreter (v1 enters=$en inlined=$inl)"
  else echo "FAIL $c:$why"; fail=1; fi
done
echo "inline-stale-test: $([ $fail = 0 ] && echo ok || echo FAILED)"
exit $fail
