#!/bin/bash
# diff.sh SEED WORK : one differential round. Modes: stack interpreter, register interpreter, the GC stress builds of both
# (when built: tools/fuzz/build-refs.sh; each with and without QJS_GC_STRESS_POLL), AOT (profile = train.js), AOT with an
# empty profile, AOT with a wrong profile (another seed's program). Exit 0 = all outputs equal.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CK=${CK:-$ROOT/.local/scratch/ck}
seed=$1; W=$2/s$seed; mkdir -p "$W"
for v in stack-rel reg-rel; do  # the two reference interpreters: Release CMake builds (reg-rel with -DQJS_REGVM=ON), target qjs-run
  [ -x "$CK/$v/tests/qjs-run" ] || { echo "diff.sh: no $CK/$v/tests/qjs-run; build it first" >&2; exit 8; }
done
export TMPDIR=$ROOT/.local/scratch/tmp; mkdir -p "$TMPDIR"
node $ROOT/tools/fuzz/gen.mjs $seed "$W/g" || exit 9
node $ROOT/tools/fuzz/gen.mjs $((seed + 1000003)) "$W/w" || exit 9
run() { # label binary args... : stdout plus the exit class
  local lab=$1; shift
  timeout ${RUN_TIMEOUT:-30} "$@" > "$W/$lab.out" 2> "$W/$lab.err"; local rc=$?
  [ $rc -ge 124 ] && echo "TIMEOUT/CRASH rc=$rc" >> "$W/$lab.out"
  [ $rc -ne 0 ] && [ $rc -lt 124 ] && echo "EXIT-NONZERO" >> "$W/$lab.out"
}
run stack $CK/stack-rel/tests/qjs-run "$W/g/run.js"
run reg   $CK/reg-rel/tests/qjs-run "$W/g/run.js"
bad=0
cmpm() { cmp -s "$W/stack.out" "$W/$1.out" || { echo "seed $seed: $1 differs from stack"; bad=1; }; }
cmpm reg
# GC stress: a missed young root aborts (an exit class the stack run does not have) instead of corrupting a later value
for v in stack-gcs reg-gcs; do
  [ -x "$CK/$v/tests/qjs-run" ] || continue
  run $v "$CK/$v/tests/qjs-run" "$W/g/run.js"; cmpm $v
  QJS_GC_STRESS_POLL=1 run ${v}p "$CK/$v/tests/qjs-run" "$W/g/run.js"; cmpm ${v}p
done
if [ -z "$NOAOT" ]; then
  $ROOT/tools/fuzz/aot-build.sh "$W/g/train.js" "$W/aot" > "$W/aot.build" 2>&1 || { echo "seed $seed: aot build: $(cat $W/aot.build | head -1)"; bad=1; }
  [ -x "$W/aot/run" ] && { run aot "$W/aot/run" "$W/g/run.js"; cmpm aot; }
  echo "" > "$W/empty.js"
  $ROOT/tools/fuzz/aot-build.sh "$W/empty.js" "$W/aote" > "$W/aote.build" 2>&1
  [ -x "$W/aote/run" ] && { run aote "$W/aote/run" "$W/g/run.js"; cmpm aote; }
  $ROOT/tools/fuzz/aot-build.sh "$W/w/train.js" "$W/aotw" > "$W/aotw.build" 2>&1
  [ -x "$W/aotw/run" ] && { run aotw "$W/aotw/run" "$W/g/run.js"; cmpm aotw; }
  # forced misses: every inline-cache site takes its exit path (the exit/gate machinery under stress)
  EXTRA_CFLAGS="-DQJS_AOT_FORCE_MISS_ENV" $ROOT/tools/fuzz/aot-build.sh "$W/g/train.js" "$W/aotf" > "$W/aotf.build" 2>&1
  [ -x "$W/aotf/run" ] && { QJS_AOT_FORCE_MISS=1 run aotf "$W/aotf/run" "$W/g/run.js"; cmpm aotf; }
fi
[ $bad = 0 ] && [ -z "$KEEP" ] && rm -rf "$W"
exit $bad
