#!/bin/bash
#
# uncatchable-test.sh -- an uncatchable error (an InternalError such as "string too long") is not
# caught by script on the stack interpreter, the register interpreter or an ahead-of-time unit.
#
# Each script in tests/uncatchable runs a function 3000 times with a plain throw (so it is hot and
# compiled), then once with an uncatchable error inside a try/catch, a try/finally or a for-of. The
# error must end the program: no catch runs, no finally runs, no iterator is closed. The output of
# the interpreter, the stack interpreter and the unit must be identical and must not contain the
# marker lines the handlers print.
#
# Usage: tools/aot/uncatchable-test.sh [stack-bench]   (stack-bench: a qjs-bench built without QJS_REGVM)
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
CLANG=${CLANG:-clang}
STACK=${1:-build-release/qjs-bench}
LDX=; [ "$(uname)" = Darwin ] && LDX="-Wl,-undefined,dynamic_lookup"
S=engine/quickjs-ng
O=${AOT_OUT:-${TMPDIR:-$(dirname "$0")/../../.local/scratch}/qjs-aot-uncatchable}
BASE="-g -std=gnu11 -D_GNU_SOURCE -DJS_ENABLE_DEBUGGER=0 -I$S"
rm -rf "$O"; mkdir -p "$O/obj"
LIBS="$O/obj/libregexp.o $O/obj/libunicode.o $O/obj/dtoa.o -lm $LDX"

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

fail=0
for t in catch finally forof; do
  src=tests/uncatchable/$t.js
  "$O/qjsc" "$src" "$O/$t.bc" > /dev/null || { echo "FAIL $t qjsc"; fail=1; continue; }
  QJS_BENCH_MAX_SECS=0 QJS_AOT_PROFILE_OUT="$O/$t.qjsprof" "$O/run-prof" "$O/$t.bc" > /dev/null 2>&1
  "$O/qjsaot" --out "$O/$t.c" --layout-header "$O/$t-l.h" --profile "$O/$t.qjsprof" --hot-cover 0.99 "$O/$t.bc" > "$O/$t.gen" 2>&1 \
    || { echo "FAIL $t generator"; tail -5 "$O/$t.gen"; fail=1; continue; }
  "$CLANG" -O2 $BASE -I"$O" -ffp-contract=off -DQJS_REGVM=1 -DQJS_AOT=1 -w -c "$O/$t.c" -o "$O/$t.o" \
    || { echo "FAIL $t unit does not compile"; fail=1; continue; }
  "$CLANG" -o "$O/run-$t" "$O/obj/qjs-bench.o" "$O/$t.o" "$O/obj/aot.o" $LIBS || { echo "FAIL $t link"; fail=1; continue; }
  "$STACK" "$src" > "$O/$t.stack" 2>&1
  "$O/run-plain" "$O/$t.bc" > "$O/$t.reg" 2>&1
  "$O/run-$t" "$O/$t.bc" > "$O/$t.aot" 2>&1
  # the first line is "uncaught: InternalError: ..."; the trace lines differ by engine, so compare only it
  s=$(head -1 "$O/$t.stack"); r=$(head -1 "$O/$t.reg"); a=$(head -1 "$O/$t.aot")
  if [ "$s" = "uncaught: InternalError: string too long" ] && [ "$r" = "$s" ] && [ "$a" = "$s" ] \
     && ! grep -q "caught \|finally ran\|iterator closed\|reached the end" "$O/$t.stack" "$O/$t.reg" "$O/$t.aot"; then
    echo "PASS $t: uncaught on the stack interpreter, the register interpreter and the unit"
  else
    echo "FAIL $t: stack='$s' regvm='$r' aot='$a'"; fail=1
  fi
done
echo "uncatchable-test: $([ $fail = 0 ] && echo ok || echo FAILED)"
exit $fail
