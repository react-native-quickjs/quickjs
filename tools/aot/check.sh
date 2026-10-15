#!/usr/bin/env bash
#
# check.sh -- the AOT generator's verification loop.
#
#   tools/aot/check.sh [t0|t1|t2|t3] [extra-kernel.js ...]
#
# TIERS
#   t0  generator + kernels vs the interpreter + golden IR dumps   (seconds)
#   t1  t0 + corpora + box2d + deopt kernels + the sweep + diff-suite (~1-3 min)
#   t2  t1 + test262 with the unit + ASan + empty/wrong profiles    (long)
#   t3  t2 + acceptance.sh + unit size report (+ Octane rows with OCTANE=1)
#
# CACHING. The engines and the generator are built once under $WORK, keyed by a
# hash of their sources, so a rerun with nothing changed compiles nothing. A
# kernel's unit is cached by (kernel, generator, flags); its C is emitted once
# and syntax-checked once. The per-function cache arrives with the new
# generator's emitter; until then the unit is the cache unit.
#
# ENV
#   CC             clang
#   WORK           build and cache directory (default /tmp/qjs-check)
#   JOBS           parallel compiles (default: nproc)
#   GEN            qjsaot2 when tools/aot/qjsaot2/main.c exists, else qjsaot
#   BOX2D          box2d checksum script for t1+ (default .local/box2d-gap/check.js)
#   OCTANE=1       run the Octane rows in t3
#   UPDATE_GOLDEN=1  refresh tools/aot/kernels2/golden/*.ir in t0
#   KEEP=1         keep $WORK on failure (it is always kept; KEEP=0 deletes it)
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
S=engine/quickjs-ng
K2=tools/aot/kernels2
GOLDEN=$K2/golden
CC=${CC:-clang}
WORK=${WORK:-/tmp/qjs-check}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}
if [ -f tools/aot/qjsaot2/main.c ]; then GEN=${GEN:-qjsaot2}; else GEN=${GEN:-qjsaot}; fi
BOX2D=${BOX2D:-.local/box2d-gap/check.js}
[ -f "$BOX2D" ] || BOX2D=""
BASE="-g -std=gnu11 -D_GNU_SOURCE -I$S"

TIER=${1:-t0}
shift || true
EXTRA_KERNELS=("$@")

mkdir -p "$WORK"/{eng,gen,units,out}
fail=0
t0_time=0

log()  { printf '%s\n' "$*"; }
warn() { printf 'check: %s\n' "$*" >&2; }
die()  { printf 'check: %s\n' "$*" >&2; exit 2; }

# ---- hashing ---------------------------------------------------------------
# hash_of <files...>: a short content hash, stable across checkouts.
hash_of() { sha256sum "$@" 2>/dev/null | sha256sum | cut -c1-16; }

ENG_HASH=$(hash_of $S/quickjs.c $S/libregexp.c $S/libunicode.c $S/dtoa.c $S/*.h)
if [ "$GEN" = qjsaot2 ]; then
  GEN_SRC=(tools/aot/qjsaot2/*.c tools/aot/qjsaot2/*.h)
else
  GEN_SRC=(tools/aot/qjsaot.c)
fi
GEN_HASH=$(hash_of "${GEN_SRC[@]}" tools/benchmark/qjs-bench.c "$S/quickjs.h")
UNIT_ARGS="--all"
BIN="$WORK/gen/$GEN_HASH/$GEN"

# ---- builds (cached) -------------------------------------------------------
build_gen() {
  [ -x "$BIN" ] && return 0
  mkdir -p "$(dirname "$BIN")"
  log "check: building $GEN ($GEN_HASH)"
  local g="$WORK/gen/$GEN_HASH"
  if [ ! -f "$g/libgen.a" ]; then
    local o="$WORK/eng/$ENG_HASH/gen"
    mkdir -p "$o"
    "$CC" -O2 $BASE -DQJS_REGVM=1 -DQJS_AOT_GEN=1 -c $S/quickjs.c -o "$o/quickjs.o" || return 1
    for f in libregexp libunicode dtoa; do
      "$CC" -O2 $BASE -c $S/$f.c -o "$o/$f.o" || return 1
    done
    ar rcs "$g/libgen.a" "$o"/*.o || return 1
  fi
  "$CC" -O2 $BASE -DQJS_REGVM=1 -DQJS_AOT_GEN=1 -o "$BIN" \
    "${GEN_SRC[@]}" "$g/libgen.a" -lm || return 1
}

# The runtime engine: one build, used both as the interpreter (no unit) and as
# the unit's host, so the two arms can never drift apart.
build_rt() {
  local o="$WORK/eng/$ENG_HASH/rt"
  [ -x "$o/run" ] && return 0
  mkdir -p "$o"
  log "check: building the runtime engine ($ENG_HASH)"
  "$CC" -O2 $BASE -DQJS_REGVM=1 -DQJS_AOT=1 -DJS_ENABLE_DEBUGGER=0 \
    -c $S/quickjs.c -o "$o/quickjs.o" || return 1
  for f in libregexp libunicode dtoa; do
    "$CC" -O2 $BASE -c $S/$f.c -o "$o/$f.o" || return 1
  done
  "$CC" -O2 $BASE -c tools/benchmark/qjs-bench.c -o "$o/qjs-bench.o" || return 1
  "$CC" -o "$o/run" "$o/qjs-bench.o" "$o/quickjs.o" "$o/libregexp.o" \
    "$o/libunicode.o" "$o/dtoa.o" -lm || return 1
}

# ---- one kernel ------------------------------------------------------------
# unit_key <js> [flags...]: kernel content + generator + flags.
unit_key() {
  local js=$1; shift
  { cat "$js"; sha256sum "$BIN"; printf '%s\n' "$*"; } | sha256sum | cut -c1-24
}

# build_unit <key> <js> [flags...] -> $WORK/units/<key>.bin (cached)
build_unit() {
  local key=$1 js=$2; shift 2
  local u="$WORK/units/$key"
  [ -x "$u.bin" ] && return 0
  "$BIN" --out "$u.c" --layout-header "$u-layout.h" "$@" "$js" \
    > /dev/null 2> "$u.census" || { warn "generator failed on $js"; return 1; }
  "$CC" -fsyntax-only $BASE -DQJS_REGVM=1 -DQJS_AOT=1 "$u.c" || {
    warn "emitted C does not parse: $u.c"; return 1; }
  "$CC" -O2 $BASE -I"$WORK/units" -ffp-contract=off -DQJS_REGVM=1 -DQJS_AOT=1 \
    -c "$u.c" -o "$u.o" || return 1
  local o="$WORK/eng/$ENG_HASH/rt"
  "$CC" -o "$u.bin" "$o/qjs-bench.o" "$u.o" "$o/quickjs.o" "$o/libregexp.o" \
    "$o/libunicode.o" "$o/dtoa.o" -lm || return 1
}

# run_pair <name> <js> [flags...]: interpreter against the unit, whole output.
run_pair() {
  local name=$1 js=$2; shift 2
  local key; key=$(unit_key "$js" "$@")
  local o="$WORK/eng/$ENG_HASH/rt" out="$WORK/out"
  if ! build_unit "$key" "$js" "$@"; then
    fail=$((fail + 1)); log "FAIL $name (build)"; return 1
  fi
  "$o/run" "$js" > "$out/$name.plain" 2>&1
  "$WORK/units/$key.bin" "$js" > "$out/$name.unit" 2>&1
  if ! diff -q "$out/$name.plain" "$out/$name.unit" > /dev/null; then
    fail=$((fail + 1)); log "FAIL $name (output differs)"
    diff "$out/$name.plain" "$out/$name.unit" | head -6
    return 1
  fi
  local st
  st=$(QJS_AOT_STATS=1 "$WORK/units/$key.bin" "$js" 2>&1 > /dev/null | grep -m1 '^aot:')
  log "ok   $name ${st:+[$st]}"
}

# ---- tiers -----------------------------------------------------------------
run_kernels() { # [extra flags...]
  local k
  for k in "$K2"/*.js "${EXTRA_KERNELS[@]:-}"; do
    [ -f "$k" ] || continue
    run_pair "$(basename "$k" .js)" "$k" "$@"
  done
}

t0() {
  local start end
  start=$(date +%s)
  build_gen || die "cannot build $GEN"
  build_rt  || die "cannot build the runtime engine"

  # Golden register-program dumps: the generator's reading of the lowering is
  # part of what a review has to see, so it is diffed, not eyeballed.
  if "$BIN" --dump-ir /dev/null > /dev/null 2>&1; then
    mkdir -p "$GOLDEN"
    local k base
    for k in "$K2"/*.js; do
      base=$(basename "$k" .js)
      "$BIN" --dump-ir "$k" 2>/dev/null > "$WORK/out/$base.ir"
      if [ "${UPDATE_GOLDEN:-0}" = 1 ]; then
        cp "$WORK/out/$base.ir" "$GOLDEN/$base.ir"
      elif [ -f "$GOLDEN/$base.ir" ]; then
        if diff -q "$GOLDEN/$base.ir" "$WORK/out/$base.ir" > /dev/null; then
          log "ok   $base.ir"
        else
          fail=$((fail + 1)); log "FAIL $base.ir (golden IR drift)"
          diff "$GOLDEN/$base.ir" "$WORK/out/$base.ir" | head -6
        fi
      fi
    done
  fi

  run_kernels
  end=$(date +%s)
  t0_time=$((end - start))
  log "T0: ${t0_time}s (gate 15s)"
}

run_corpora() {
  local k rc=0
  for k in compiler_bench jit_bench aot_bench; do
    local js=tools/benchmark/kernels/$k.js fast=$WORK/out/$k.fast.js
    [ -f "$js" ] || continue
    sed -e 's/var CB_REPS = [0-9]*;/var CB_REPS = 1;/' \
        -e 's/var CB_SCALE = [0-9]*;/var CB_SCALE = 1;/' \
        -e 's/var CB_TARGET_MS = [0-9]*;/var CB_TARGET_MS = 5;/' "$js" > "$fast"
    local key; key=$(unit_key "$js" "$UNIT_ARGS")
    if ! build_unit "$key" "$js" "$UNIT_ARGS"; then
      fail=$((fail + 1)); rc=1; log "FAIL $k (build)"; continue
    fi
    local o="$WORK/eng/$ENG_HASH/rt"
    "$o/run" "$fast" > "$WORK/out/$k.plain" 2>&1
    "$WORK/units/$key.bin" "$fast" > "$WORK/out/$k.unit" 2>&1
    # The one known difference: a compiled frame carries the unit's return pc
    # rather than the bytecode pc, so `call.stack`'s checksum differs (the
    # acceptance report shows the same row as NO). Every other row must match.
    awk '/^##CB2##/ {print $2, $5}' "$WORK/out/$k.plain" |
      grep -v '^call.stack ' > "$WORK/out/$k.plain.cs"
    awk '/^##CB2##/ {print $2, $5}' "$WORK/out/$k.unit" |
      grep -v '^call.stack ' > "$WORK/out/$k.unit.cs"
    if diff -q "$WORK/out/$k.plain.cs" "$WORK/out/$k.unit.cs" > /dev/null; then
      if grep -q '^call.stack ' "$WORK/out/$k.unit" &&
         grep -q '^call.stack ' "$WORK/out/$k.plain" &&
         ! diff -q <(grep '^call.stack ' "$WORK/out/$k.unit") \
                    <(grep '^call.stack ' "$WORK/out/$k.plain") > /dev/null; then
        log "ok   $k (known: call.stack)"
      else
        log "ok   $k"
      fi
    else
      fail=$((fail + 1)); rc=1; log "FAIL $k (checksums differ)"
      diff "$WORK/out/$k.plain.cs" "$WORK/out/$k.unit.cs" | head -4
    fi
  done
  return $rc
}

run_box2d() {
  [ -n "$BOX2D" ] || { warn "BOX2D not set and $BOX2D missing; skipping box2d"; return 0; }
  # body below
  local key; key=$(unit_key "$BOX2D" "$UNIT_ARGS")
  if ! build_unit "$key" "$BOX2D" "$UNIT_ARGS"; then
    fail=$((fail + 1)); log "FAIL box2d (build)"; return 1
  fi
  local got
  got=$(QJS_AOT_STATS=1 "$WORK/units/$key.bin" "$BOX2D" 2>&1 |
        sed -n 's/.*box2d-checksum=\([0-9.]*\).*/\1/p' | tail -1)
  if [ "$got" = "546.1008391043414" ]; then
    log "ok   box2d ($got)"; return 0
  fi
  fail=$((fail + 1)); log "FAIL box2d (checksum $got)"
  return 1
}

run_deopt_kernels() {
  local k base
  for k in "$K2"/*.js; do
    base=$(basename "$k" .js)
    run_pair "$base.deopt" "$k" --deopt-everywhere
  done
}

run_sweep() {
  # The guard sweep: the unit is the deopt-everywhere build; a stride makes
  # every site fail on its n-th execution, which exercises deopt after real
  # work has happened. Every stride must produce the interpreter's output.
  local k base stride
  for k in "$K2"/0[12]-*.js; do
    base=$(basename "$k" .js)
    for stride in 1 7 64; do
      local key; key=$(unit_key "$k" --deopt-everywhere)
      build_unit "$key" "$k" --deopt-everywhere || { fail=$((fail + 1)); continue; }
      local o="$WORK/eng/$ENG_HASH/rt"
      "$o/run" "$k" > "$WORK/out/$base.plain" 2>&1
      QJS_AOT_SWEEP=all:$stride "$WORK/units/$key.bin" "$k" \
        > "$WORK/out/$base.sw$stride" 2>&1
      if diff -q "$WORK/out/$base.plain" "$WORK/out/$base.sw$stride" > /dev/null; then
        log "ok   $base sweep all:$stride"
      else
        fail=$((fail + 1)); log "FAIL $base sweep all:$stride"
        diff "$WORK/out/$base.plain" "$WORK/out/$base.sw$stride" | head -4
      fi
    done
  done
}

run_diff_suite() {
  local o="$WORK/eng/$ENG_HASH/rt2" sp=$WORK/out
  if [ ! -x "$o/stack" ]; then
    mkdir -p "$o"
    log "check: building the diff-suite runners"
    "$CC" -O2 $BASE -o "$o/stack" tests/differential/qjs-run.c \
      $S/quickjs.c $S/libregexp.c $S/libunicode.c $S/dtoa.c -lm || return 1
    "$CC" -O2 $BASE -DQJS_REGVM=1 -o "$o/regvm" tests/differential/qjs-run.c \
      $S/quickjs.c $S/libregexp.c $S/libunicode.c $S/dtoa.c -lm || return 1
  fi
  STACK="$o/stack" REGVM="$o/regvm" sh tools/regvm/diff-suite/diff-suite.sh \
    > "$sp/diff-suite.out" 2>&1
  local res
  res=$(grep -oE 'pass=[0-9]+ fail=[0-9]+ skip=[0-9]+' "$sp/diff-suite.out" | tail -1)
  case "$res" in
    *"fail=0"*) log "ok   diff-suite ($res)"; return 0 ;;
    *) fail=$((fail + 1)); log "FAIL diff-suite ($res)"; tail -5 "$sp/diff-suite.out"; return 1 ;;
  esac
}

t1() {
  local start; start=$(date +%s)
  t0
  # The corpora and the diff-suite are independent and each runs for a minute
  # or more of wall clock, so they overlap; failures are collected by exit
  # status because each runs in its own subshell.
  run_corpora & local p1=$!
  run_diff_suite & local p2=$!
  run_box2d
  wait $p1 || fail=$((fail + 1))
  wait $p2 || fail=$((fail + 1))
  run_deopt_kernels
  run_sweep
  log "T1: $(( $(date +%s) - start ))s (gate 180s)"
}

t2() {
  local start; start=$(date +%s)
  t1
  # The heavy gates are acceptance.sh's; they rebuild their own engines, so
  # this tier is minutes, not seconds.
  if [ -n "${SKIP_ACCEPT:-}" ]; then
    warn "SKIP_ACCEPT set: test262/ASan not run"
  else
    log "check: acceptance.sh --full (test262 with the unit, diff corpus, ASan)"
    BOX2D="$BOX2D" AOT_OUT="$WORK/accept" bash tools/aot/acceptance.sh --full \
      > "$WORK/out/accept-full.log" 2>&1
    local r
    for r in test262 diff ASan; do
      grep -m1 "| $r" "$WORK/accept/acceptance.md" 2>/dev/null | sed 's/^/     /'
    done
  fi
  # Empty and wrong profiles must still produce a working unit.
  local k=$K2/01-arith.js key
  : > "$WORK/out/empty.qjsprof"
  key=$(unit_key "$k" --profile "$WORK/out/empty.qjsprof")
  if build_unit "$key" "$k" --profile "$WORK/out/empty.qjsprof"; then
    if diff -q <("$WORK/eng/$ENG_HASH/rt/run" "$k" 2>&1) \
               <("$WORK/units/$key.bin" "$k" 2>&1) > /dev/null; then
      log "ok   empty profile"
    else
      fail=$((fail + 1)); log "FAIL empty profile"
    fi
  else
    fail=$((fail + 1)); log "FAIL empty profile (build)"
  fi
  log "T2: $(( $(date +%s) - start ))s"
}

t3() {
  local start; start=$(date +%s)
  t2
  log "check: acceptance.sh (kernels + box2d)"
  BOX2D="$BOX2D" AOT_OUT="$WORK/accept3" bash tools/aot/acceptance.sh \
    > "$WORK/out/accept.log" 2>&1
  grep -E '^\| (compiler_bench|jit_bench|aot_bench|box2d) ' \
    "$WORK/accept3/acceptance.md" 2>/dev/null | sed 's/^/     /'
  local u
  for u in "$WORK"/units/*.o; do
    [ -f "$u" ] || continue
    printf '     size %s: %s bytes .text\n' "$(basename "$u" .o)" \
      "$(size -A "$u" 2>/dev/null | awk '$1==".text"{print $2}')"
  done | sort -k4 -n -r | head -8
  if [ "${OCTANE:-0}" = 1 ]; then
    log "check: Octane rows (interpreter against the unit)"
    local row js
    for row in richards deltablue crypto raytrace earleyboyer regexp splay \
               navierstokes pdfjs gbemu box2d typescript mandreel; do
      js=tools/benchmark/octane/corpus/$row.js
      [ -f "$js" ] || continue
      local key; key=$(unit_key "$js" "$UNIT_ARGS")
      build_unit "$key" "$js" "$UNIT_ARGS" || { fail=$((fail + 1)); continue; }
      local p u
      p=$(timeout 900 "$WORK/eng/$ENG_HASH/rt/run" "$js" 2>/dev/null |
          awk '/^Score:/{print $2}')
      u=$(timeout 900 "$WORK/units/$key.bin" "$js" 2>/dev/null |
          awk '/^Score:/{print $2}')
      log "     $row: interpreter $p, unit $u"
    done
  fi
  log "T3: $(( $(date +%s) - start ))s"
}

case "$TIER" in
  t0) t0 ;;
  t1) t1 ;;
  t2) t2 ;;
  t3) t3 ;;
  -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
  *) die "unknown tier '$TIER' (t0|t1|t2|t3)" ;;
esac

if [ "$fail" -eq 0 ]; then
  log "check: PASS ($TIER)"
else
  log "check: FAIL ($TIER, $fail problem(s))"
fi
exit $(( fail != 0 ))
