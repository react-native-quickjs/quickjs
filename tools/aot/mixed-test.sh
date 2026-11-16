#!/usr/bin/env bash
#
# mixed-test.sh -- mixed programs under an AOT unit.
#
#   tools/aot/mixed-test.sh [file.js ...]     (default: tests/differential/mixed/*.js)
#
# Each file is compiled to optimised bytecode (qjsc), and its unit is
# generated from that bytecode (qjsaot --all). The unit binary runs the
# bytecode, so the host's functions bind to the unit while every payload the
# file compiles at run time (eval, new Function) is lowered by the runtime's
# default fast compile mode and runs in the interpreter beside them. The output
# must equal the plain interpreter's on the same bytecode, and the unit must be
# entered.
#
# ENV
#   CC     compiler (default tools/aot/ccsplit, as check.sh)
#   WORK   build directory (default .local/scratch/qjs-mixed); the engines are
#          rebuilt only when their sources change
#   QJSC   the bytecode compiler (default build-regvm/qjsc)
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
S=engine/quickjs-ng
CC=${CC:-$ROOT/tools/aot/ccsplit}
LDX=; [ "$(uname)" = Darwin ] && LDX="-Wl,-undefined,dynamic_lookup"
WORK=${WORK:-$ROOT/.local/scratch/qjs-mixed}
mkdir -p "$WORK" && WORK=$(cd "$WORK" && pwd)
QJSC=${QJSC:-$ROOT/build-regvm/qjsc}
BASE="-g -std=gnu11 -D_GNU_SOURCE -I$S"

log()  { printf '%s\n' "$*"; }
die()  { printf 'mixed-test: %s\n' "$*" >&2; exit 2; }

[ -x "$QJSC" ] || die "no bytecode compiler at $QJSC (build the regvm qjsc target)"

hash_of() { sha256sum -- "$@" 2>/dev/null | sha256sum | cut -c1-16; }
ENG_HASH=$(hash_of $S/quickjs.c $S/libregexp.c $S/libunicode.c $S/dtoa.c $S/*.h)
GEN_SRC=(tools/aot/qjsaot/*.c)
GEN_HASH=$(hash_of "${GEN_SRC[@]}" "$S/quickjs.h")
GEN="$WORK/gen-$GEN_HASH-$ENG_HASH/qjsaot"
RT="$WORK/rt-$ENG_HASH"

build_gen() {
  [ -x "$GEN" ] && return 0
  local o; o=$(dirname "$GEN"); mkdir -p "$o"
  log "mixed-test: building the generator"
  "$CC" -O2 $BASE -DQJS_REGVM=1 -DQJS_AOT_GEN=1 -DJS_ENABLE_DEBUGGER=0 -c $S/quickjs.c -o "$o/quickjs.o" || return 1
  for f in libregexp libunicode dtoa; do "$CC" -O2 $BASE -c $S/$f.c -o "$o/$f.o" || return 1; done
  "$CC" -O2 $BASE -DQJS_REGVM=1 -DQJS_AOT_GEN=1 -DJS_ENABLE_DEBUGGER=0 -o "$GEN" "${GEN_SRC[@]}" \
    "$o/quickjs.o" "$o/libregexp.o" "$o/libunicode.o" "$o/dtoa.o" -lm || return 1
}

# One runtime engine, the interpreter (no unit) and the unit's host alike.
build_rt() {
  [ -x "$RT/run" ] && return 0
  mkdir -p "$RT"
  log "mixed-test: building the runtime engine"
  "$CC" -O2 $BASE -DQJS_REGVM=1 -DQJS_AOT=1 -DJS_ENABLE_DEBUGGER=0 -c $S/quickjs.c -o "$RT/quickjs.o" || return 1
  for f in libregexp libunicode dtoa; do "$CC" -O2 $BASE -c $S/$f.c -o "$RT/$f.o" || return 1; done
  "$CC" -O2 $BASE -c tools/benchmark/qjs-bench.c -o "$RT/qjs-bench.o" || return 1
  "$CC" -o "$RT/run" "$RT/qjs-bench.o" "$RT/quickjs.o" "$RT/libregexp.o" "$RT/libunicode.o" "$RT/dtoa.o" -lm $LDX || return 1
}

build_gen || die "cannot build the generator"
build_rt || die "cannot build the runtime engine"

files=("$@")
[ ${#files[@]} -gt 0 ] || files=(tests/differential/mixed/*.js)
fail=0
for js in "${files[@]}"; do
  name=$(basename "$js" .js)
  u="$WORK/units/$name"; mkdir -p "$WORK/units"
  if ! "$QJSC" "$js" "$u.bc" > /dev/null 2>&1; then
    log "FAIL $name (qjsc)"; fail=$((fail + 1)); continue
  fi
  if ! "$GEN" --all --out "$u.c" --layout-header "$u-layout.h" "$u.bc" > /dev/null 2> "$u.gen.log" ||
     ! "$CC" -O2 $BASE -I"$WORK/units" -ffp-contract=off -DQJS_REGVM=1 -DQJS_AOT=1 -c "$u.c" -o "$u.o" ||
     ! "$CC" -o "$u.bin" "$RT/qjs-bench.o" "$u.o" "$RT/quickjs.o" "$RT/libregexp.o" "$RT/libunicode.o" \
          "$RT/dtoa.o" -lm $LDX; then
    log "FAIL $name (unit build, see $u.gen.log)"; fail=$((fail + 1)); continue
  fi
  "$RT/run" "$u.bc" > "$u.plain" 2>&1
  QJS_AOT_UNIT_STATS=1 "$u.bin" "$u.bc" > "$u.unit" 2> "$u.stats"
  enters=$(sed -n 's/^unit-stats enters=\([0-9]*\).*/\1/p' "$u.stats" | head -1)
  if ! diff -q "$u.plain" "$u.unit" > /dev/null; then
    log "FAIL $name (output differs from the interpreter)"; diff "$u.plain" "$u.unit" | head -6; fail=$((fail + 1))
  elif [ -z "$enters" ] || [ "$enters" -eq 0 ]; then
    log "FAIL $name (the unit was never entered: enters=${enters:-none})"; fail=$((fail + 1))
  else
    log "ok   $name (enters=$enters)"
  fi
done
[ $fail -eq 0 ] || { log "mixed-test: $fail failed"; exit 1; }
log "mixed-test: all ${#files[@]} ok"
