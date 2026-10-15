#!/bin/bash
#
# acceptance.sh -- write the AOT acceptance report.
#
# Builds the register-VM AOT engine, generates a STEP-ONLY unit for each kernel
# file, runs both arms (AOT and interpreter) and checks that every kernel's
# checksum is identical at both sizes. Also runs the box2d gate. With --full it
# additionally runs test262 (--all), the differential corpus and an ASan pass.
#
# Usage:
#   tools/aot/acceptance.sh            # kernels + box2d  (fast)
#   tools/aot/acceptance.sh --full     # + test262, diff, ASan
#
# The compiler_bench/jit_bench arms are run at a reduced scale (CB_REPS=1,
# CB_SCALE=1, CB_TARGET_MS=5) so the acceptance pass is minutes, not an hour;
# the two-size self-check that defines correctness is unaffected. aot_bench.js
# is already small and runs as written.
#
# Env: BOX2D (required: the box2d checksum script), AOT_OUT (scratch dir),
#      AOT_REPORT (report path, default $AOT_OUT/acceptance.md), AOT_PROFILE
#      (box2d .qjsprof for the op census), CLANG (default clang).
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
CLANG=${CLANG:-clang}
S=engine/quickjs-ng
O=${AOT_OUT:-${TMPDIR:-/tmp}/qjs-aot-accept}
MD=${AOT_REPORT:-$O/acceptance.md}
BOX2D=${BOX2D:-}
[ -n "$BOX2D" ] || { echo "acceptance.sh: set BOX2D to the box2d checksum script" >&2; exit 2; }
K=tools/benchmark/kernels
BASE="-g -std=gnu11 -D_GNU_SOURCE -I$S"
# AOT is release-only: the engine refuses JS_ENABLE_DEBUGGER with QJS_AOT.
# The unit is compiled separately from the engine, at UNIT_OPT (an -O2 unit is
# the shipping shape; -O0 is available when debugging one).
UNIT_OPT=${UNIT_OPT:--O2}
FULL=0
[ "${1:-}" = "--full" ] && FULL=1
PROFILE=${AOT_PROFILE:-}

mkdir -p "$O/obj" "$O/kfast" "$(dirname "$MD")"

log() { echo "== $* =="; }

# ---- build -----------------------------------------------------------------
log "build generator"
"$CLANG" -O2 $BASE -DQJS_REGVM=1 -DQJS_AOT_GEN=1 -DQJS_AOT_NO_MAIN \
  -o "$O/qjsaot2" tools/aot/qjsaot.c tools/aot/qjsaot2/*.c \
  $S/quickjs.c $S/libregexp.c $S/libunicode.c $S/dtoa.c -lm || exit 1

log "build engine"
"$CLANG" -O2 $BASE -DQJS_REGVM=1 -DQJS_AOT=1 -DJS_ENABLE_DEBUGGER=0 -c $S/quickjs.c -o "$O/obj/quickjs.o" || exit 1
for f in libregexp libunicode dtoa; do
  "$CLANG" -O2 $BASE -c $S/$f.c -o "$O/obj/$f.o" || exit 1
done
"$CLANG" -O2 $BASE -c tools/benchmark/qjs-bench.c -o "$O/obj/qjs-bench.o" || exit 1
"$CLANG" -o "$O/plain-run" "$O/obj/qjs-bench.o" \
  "$O/obj/quickjs.o" "$O/obj/libregexp.o" "$O/obj/libunicode.o" "$O/obj/dtoa.o" -lm || exit 1

# ---- helpers ---------------------------------------------------------------
# build_unit <tag> <js>  -> $O/<tag>.c, $O/obj/<tag>.o, $O/bench-<tag>
build_unit() {
  local tag=$1 js=$2 pargs="" sargs="--all"
  # box2d takes the profile-guided selection: when a profile is available, emit only the
  # functions covering HOT_COVER of the profiled ops instead of --all.
  if [ "$tag" = box2d ] && [ -n "$PROFILE" ]; then
    pargs="--profile $PROFILE"; sargs=""
  fi
  "$O/qjsaot2" --out "$O/$tag.c" --layout-header "$O/$tag-layout.h" $sargs $pargs "$js" 2>"$O/$tag.census"
  "$CLANG" $UNIT_OPT $BASE -I"$O" -ffp-contract=off -DQJS_REGVM=1 -DQJS_AOT=1 -c "$O/$tag.c" -o "$O/obj/$tag.o" || return 1
  "$CLANG" -o "$O/bench-$tag" "$O/obj/qjs-bench.o" "$O/obj/$tag.o" \
    "$O/obj/quickjs.o" "$O/obj/libregexp.o" "$O/obj/libunicode.o" "$O/obj/dtoa.o" -lm || return 1
}

# fast_copy <src> <dst>: shrink the benchmark scale without touching shape.
fast_copy() {
  sed -e 's/var CB_REPS = [0-9]*;/var CB_REPS = 1;/' \
      -e 's/var CB_SCALE = [0-9]*;/var CB_SCALE = 1;/' \
      -e 's/var CB_TARGET_MS = [0-9]*;/var CB_TARGET_MS = 5;/' "$1" > "$2"
}

checksums() { awk '/^##CB2##/ {print $2, $5}' "$1" > "$2"; }
cbsum()     { awk '/^##CBSUM##/ {print $3, $5, $7, $9}' "$1"; }
# Geomean-ms if printed, else recompute from the ##CB2## per-kernel ms.
geomean() {
  local g
  g=$(awk -F': ' '/^Geomean-ms:/ {print $2}' "$1")
  if [ -z "$g" ]; then
    g=$(awk '/^##CB2##/ {if ($3+0>0) {n++; s+=log($3+0)}} END {if (n) printf "%.4f", exp(s/n)}' "$1")
  fi
  echo "$g"
}
# census_field <file> <functions|profiled ops>
census_field() {
  local pat="refused functions"
  [ "$2" = "profiled ops" ] && pat="refused profiled ops"
  sed -n "s/.*$pat \\([0-9]*\\/[0-9]*\\) (\\([0-9.]*\\)%).*/\\1 (\\2%)/p" "$1" | tail -1
}

declare -a ROWS
run_kernel() {
  local tag=$1 js=$2 fast=$3 label=$4
  local jsrun=$js
  if [ "$fast" = 1 ]; then
    fast_copy "$js" "$O/kfast/$(basename "$js")"
    jsrun="$O/kfast/$(basename "$js")"
  fi
  build_unit "$tag" "$js" || { ROWS+=("$label|build failed||||"); return; }
  "$O/plain-run" "$jsrun" > "$O/$tag-plain.out" 2>&1
  "$O/bench-$tag" "$jsrun" > "$O/$tag-aot.out" 2>&1
  checksums "$O/$tag-plain.out" "$O/$tag-plain.cs"
  checksums "$O/$tag-aot.out"   "$O/$tag-aot.cs"
  local sum_aot eq ratio gp ga cfun cpop
  sum_aot=$(cbsum "$O/$tag-aot.out")
  if diff -q "$O/$tag-plain.cs" "$O/$tag-aot.cs" >/dev/null 2>&1; then eq="yes"; else eq="NO"; fi
  gp=$(geomean "$O/$tag-plain.out"); ga=$(geomean "$O/$tag-aot.out")
  ratio=$(awk -v a="$ga" -v p="$gp" 'BEGIN{ if (p>0) printf "%.2fx", a/p; else print "?" }')
  cfun=$(census_field "$O/$tag.census" functions); [ -z "$cfun" ] && cfun="?"
  cpop=$(census_field "$O/$tag.census" "profiled ops"); [ -z "$cpop" ] && cpop="n/a"
  ROWS+=("$label|$sum_aot|$eq|$ratio|$cfun|$cpop")
}

run_kernel cbf "$K/compiler_bench.js" 1 "compiler_bench"
run_kernel jbf "$K/jit_bench.js"      1 "jit_bench"
run_kernel abf "$K/aot_bench.js"      0 "aot_bench"

# ---- box2d -----------------------------------------------------------------
log "box2d"
build_unit box2d "$BOX2D"
QJS_AOT_STATS=1 "$O/bench-box2d" "$BOX2D" > "$O/box2d.out" 2>&1
BOX=$(awk -F= '/box2d-checksum=/ {print $2}' "$O/box2d.out" | tail -1)
BOXC=$(census_field "$O/box2d.census" functions); [ -z "$BOXC" ] && BOXC="?"
BOXP=$(census_field "$O/box2d.census" "profiled ops"); [ -z "$BOXP" ] && BOXP="n/a (no profile)"
SEL=$(grep -oE 'coverage [0-9.]+% \([0-9]+/[0-9]+ profiled ops\), est [0-9]+ B, actual [0-9]+ B' "$O/box2d.census" | tail -1)
[ -z "$SEL" ] && SEL="--all (no profile)"
TEXT=$(size -A "$O/obj/box2d.o" 2>/dev/null | awk '$1==".text"{print $2}')
[ -z "$TEXT" ] && TEXT="?"
BOUND=$(sed -n 's/.*bound=\([0-9]*\).*/\1/p' "$O/box2d.out" | tail -1)
TABLE=$(sed -n 's/.*table=\([0-9]*\).*/\1/p' "$O/box2d.out" | tail -1)

# ---- heavy gates (opt-in) --------------------------------------------------
T262="not run (--full to include)"
DIFF="not run (--full to include)"
ASAN="not run (--full to include)"
if [ "$FULL" = 1 ]; then
  log "test262 (--all, AOT harness unit)"
  # The harness is a glob of files, so build the unit by hand.
  "$O/qjsaot2" --out "$O/t262.c" --layout-header "$O/t262-layout.h" --all \
    $S/test262/harness/*.js "$BOX2D" 2>"$O/t262.census"
  "$CLANG" $UNIT_OPT $BASE -I"$O" -ffp-contract=off -DQJS_REGVM=1 -DQJS_AOT=1 -c "$O/t262.c" -o "$O/obj/t262.o"
  "$CLANG" -O2 $BASE -c $S/run-test262.c -o "$O/obj/run-test262.o"
  "$CLANG" -O2 $BASE -c $S/quickjs-libc.c -o "$O/obj/quickjs-libc.o"
  "$CLANG" -o "$O/aot-test262" "$O/obj/run-test262.o" "$O/obj/quickjs-libc.o" \
    "$O/obj/t262.o" "$O/obj/quickjs.o" "$O/obj/libregexp.o" "$O/obj/libunicode.o" "$O/obj/dtoa.o" -lm
  ( cd $S && "$O/aot-test262" -m -c test262.conf -c test262-fast.conf -a > "$O/test262.out" 2>&1 )
  T262=$(grep -E '^Result:' "$O/test262.out" | tail -1)

  log "diff corpus"
  "$CLANG" -O2 $BASE -o "$O/linux-run-stack" tests/differential/qjs-run.c \
    $S/quickjs.c $S/libregexp.c $S/libunicode.c $S/dtoa.c -lm
  "$CLANG" -O2 $BASE -DQJS_REGVM=1 -o "$O/linux-run-regvm" tests/differential/qjs-run.c \
    $S/quickjs.c $S/libregexp.c $S/libunicode.c $S/dtoa.c -lm
  STACK="$O/linux-run-stack" REGVM="$O/linux-run-regvm" \
    sh tools/regvm/diff-suite/diff-suite.sh > "$O/diff.out" 2>&1
  DIFF=$(grep -oE 'pass=[0-9]+ fail=[0-9]+ skip=[0-9]+' "$O/diff.out" | tail -1)

  log "asan (test262-fast)"
  for f in quickjs libregexp libunicode dtoa; do
    "$CLANG" -O1 $BASE -fsanitize=address,undefined -fno-omit-frame-pointer \
      -DQJS_REGVM=1 -DQJS_AOT=1 -c $S/$f.c -o "$O/obj/$f-asan.o"
  done
  "$CLANG" $UNIT_OPT $BASE -I"$O" -ffp-contract=off -fsanitize=address,undefined -fno-omit-frame-pointer \
    -DQJS_REGVM=1 -DQJS_AOT=1 -c "$O/t262.c" -o "$O/obj/t262-asan.o"
  "$CLANG" -O1 $BASE -fsanitize=address,undefined -fno-omit-frame-pointer \
    -c $S/run-test262.c -o "$O/obj/run-test262-asan.o"
  "$CLANG" -O1 $BASE -fsanitize=address,undefined -fno-omit-frame-pointer \
    -c $S/quickjs-libc.c -o "$O/obj/quickjs-libc-asan.o"
  "$CLANG" -fsanitize=address,undefined -o "$O/aot-test262-asan" \
    "$O/obj/run-test262-asan.o" "$O/obj/quickjs-libc-asan.o" "$O/obj/t262-asan.o" \
    "$O/obj/quickjs-asan.o" "$O/obj/libregexp-asan.o" "$O/obj/libunicode-asan.o" "$O/obj/dtoa-asan.o" -lm
  ( cd $S && "$O/aot-test262-asan" -m -c test262.conf -c test262-fast.conf -a > "$O/asan.out" 2>&1 )
  T=$(grep -E '^Result:' "$O/asan.out" | tail -1)
  U=$(grep -c 'runtime error' "$O/asan.out")
  A=$(grep -c 'ERROR: AddressSanitizer' "$O/asan.out")
  ASAN="$T; AddressSanitizer errors=$A, UBSan reports=$U"
fi

# ---- write the report ------------------------------------------------------
{
  echo "# AOT acceptance"
  echo
  echo "Generated by \`tools/aot/acceptance.sh\` on $(date -u '+%Y-%m-%d %H:%M UTC')."
  echo "Layer A: hot opcodes are inline templates that mirror the interpreter"
  echo "handler and fall through to \`qjs_aot_step\` on a miss; every other op runs"
  echo "through \`qjs_aot_step\`. Layer B adds typed numeric loop regions."
  echo "Correctness is the checksum matching the interpreter at both sizes."
  echo
  echo "## Kernel acceptance"
  echo
  echo "| corpus | CBSUM (kernels/scored/failed/warns) | checksums equal | ratio vs interp |"
  echo "|---|---|---|---|"
  for row in "${ROWS[@]}"; do
    IFS='|' read -r a b c d e f <<< "$row"
    echo "| $a | $b | $c | $d |"
  done
  echo
  echo "## Refusal census (target < 0.5% of profiled ops)"
  echo
  echo "| corpus | refused functions | refused profiled ops |"
  echo "|---|---|---|"
  for row in "${ROWS[@]}"; do
    IFS='|' read -r a b c d e f <<< "$row"
    echo "| $a | $e | $f |"
  done
  echo "| box2d | $BOXC | $BOXP |"
  echo
  echo "## Selection and size"
  echo
  echo "box2d is emitted through the profile-guided selector (HOT_COVER 0.95,"
  echo "MIN_ENTRY_OPS 1000, compiled-callee closure); the kernels stay \`--all\`."
  echo
  echo "| corpus | selection | .text bytes | bound/table |"
  echo "|---|---|---|---|"
  echo "| box2d | $SEL | $TEXT | $BOUND/$TABLE |"
  echo
  echo "## Gates"
  echo
  echo "| gate | result |"
  echo "|---|---|"
  echo "| box2d checksum | $BOX (expect 546.1008391043414) |"
  echo "| test262 --all | $T262 |"
  echo "| diff corpus | $DIFF |"
  echo "| ASan | $ASAN |"
} > "$MD"

echo "wrote $MD"
cat "$MD"
