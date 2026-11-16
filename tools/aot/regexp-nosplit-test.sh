#!/bin/bash
#
# regexp-nosplit-test.sh -- ahead-of-time regexp matchers with and without a backtrack point.
#
# A generated matcher resumes at a pushed label address after a failed attempt. A pattern with no
# split or greedy run never pushes one, and a computed goto in a function that takes no label
# address does not compile (clang). Such a matcher has to end at its failure instead. This builds a
# unit whose hot regexps include split-free patterns (sticky ones, which have no scan prefix: a literal,
# a dot, anchors, a character class without a quantifier) and patterns with backtracking (an alternation, a greedy run, a counted
# loop), compiles the unit, and requires the output to equal the interpreter's.
#
# Usage: tools/aot/regexp-nosplit-test.sh
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
CLANG=${CLANG:-clang}
LDX=; [ "$(uname)" = Darwin ] && LDX="-Wl,-undefined,dynamic_lookup"
S=engine/quickjs-ng
O=${AOT_OUT:-${TMPDIR:-$(dirname "$0")/../../.local/scratch}/qjs-aot-regexp-nosplit}
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

cat > "$O/t.js" <<'EOF'
var subjects = ["abc", "xabcx", "a.c", "abd", "12345", "12a45", "", "ab", "cab", "aaa", "ac", "abcabc"];
var pats = [
  /abc/y, /a.c/y, /^abc$/y, /^\d\d\d\d\d$/y, /[ab]c/y, /^$/y,   // sticky: no scan prefix, no split, no run
  /abc/, /a|b/y, /^\d+$/y, /(?:ab)*c/y, /a{2,3}/y, /(a)(b)?c/y     // the scan prefix, an alternation, a greedy run, a counted loop, a group
];
var out = [];
for (var rep = 0; rep < 2000; rep++)
  for (var p = 0; p < pats.length; p++)
    for (var i = 0; i < subjects.length; i++) {
      pats[p].lastIndex = 0; var m = pats[p].exec(subjects[i]);
      if (rep == 1999) out.push(p + ":" + i + ":" + (m ? m.index + "," + m[0] : "-"));
    }
print(out.join(" "));
EOF
"$O/qjsc" "$O/t.js" "$O/t.bc" > /dev/null || exit 1
QJS_BENCH_MAX_SECS=0 QJS_AOT_PROFILE_OUT="$O/p.qjsprof" "$O/run-prof" "$O/t.bc" > /dev/null 2>&1
"$O/qjsaot" --out "$O/u.c" --layout-header "$O/u-l.h" --profile "$O/p.qjsprof" --hot-cover 0.99 "$O/t.bc" > "$O/gen.out" 2>&1 \
  || { echo "FAIL generator"; tail -5 "$O/gen.out"; exit 1; }
ls "$O"/u-rx.c > /dev/null 2>&1 || { echo "FAIL no regexp matcher unit was generated"; exit 1; }
nm=$(grep -c "^static intptr_t rxf_" "$O/u-rx.c")
nolab=$(awk '/^static intptr_t rxf_/ {f=1; n=0} f && /&&L/ {n++} /^}/ && f {if (n==0) c++; f=0} END {print c+0}' "$O/u-rx.c")
"$CLANG" -O2 $BASE -I"$O" -ffp-contract=off -DQJS_REGVM=1 -DQJS_AOT=1 -w -c "$O/u.c" -o "$O/u.o" || { echo "FAIL unit does not compile"; exit 1; }
# the matchers are a second source of the unit (ccsplit compiles it with the unit's flags and joins the objects)
"$CLANG" -O2 $BASE -I"$O" -ffp-contract=off -DQJS_REGVM=1 -DQJS_AOT=1 -w -c "$O/u-rx.c" -o "$O/u-rx.o" \
  || { echo "FAIL the regexp matchers do not compile ($nm matchers, $nolab without a label address)"; exit 1; }
ld -r -o "$O/unit.o" "$O/u.o" "$O/u-rx.o" || { echo "FAIL join"; exit 1; }
"$CLANG" -o "$O/run" "$O/obj/qjs-bench.o" "$O/unit.o" "$O/obj/aot.o" $LIBS || { echo "FAIL link"; exit 1; }
"$O/run-plain" "$O/t.bc" > "$O/plain.out" 2>&1
"$O/run" "$O/t.bc" > "$O/unit.out" 2>&1
fail=0
cmp -s "$O/plain.out" "$O/unit.out" && echo "PASS output equals the interpreter's" || { echo "FAIL output differs"; fail=1; }
if [ "$nolab" -gt 0 ] && [ "$nolab" -lt "$nm" ]; then echo "PASS $nm matchers, $nolab without a label address, the rest with"
else echo "FAIL the test needs matchers of both kinds ($nm matchers, $nolab without a label address)"; fail=1; fi
echo "regexp-nosplit-test: $([ $fail = 0 ] && echo ok || echo FAILED)"
exit $fail
