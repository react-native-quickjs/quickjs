#!/bin/sh
# The register VM's phase gate, in one command.
#
#   tools/regvm/gate.sh [workdir]
#
# Every phase of the redesign ends with the same evidence, and the plan states it
# as: differential suite 103/0/0, promotion fuzzer 0 mismatches, box2d checksum
# 546.1008391043414, then the wall-time A/B against the stack VM and Hermes.
# This runs all of it and prints one PASS/FAIL summary.
#
# Environment:
#   CC          compiler                    (default clang)
#   WORK        build directory             (default /tmp/rv-gate)
#   REPS        timing repetitions          (default 4)
#   FUZZ_N      promotion-fuzzer cases      (default 800)
#   SUITE       differential suite script   (default tools/regvm/diff-suite/diff-suite.sh)
#   HERMES      hermes binary, optional; when set the A/B includes it
#   MONO        monolith register binary, optional; when set the A/B times it
#               too, which is what phase 2 is judged on: the threaded build has
#               to beat the monolith, not the stack VM
#   OBJDUMP     objdump for the static handler gates (default: the first of
#               llvm-objdump, llvm-objdump-14, objdump that exists)
#   SKIP_BUILD  1 to reuse $WORK
#
# The register binaries are built with -DQJS_REGVM_VERIFY=1 -DQJS_REGVM_KNOBS=1.
# The read-back verifier and the debug knobs are off in a shipping build (see
# QJS_REGVM_VERIFY in quickjs.c), so a gate that wants them has to ask. Timing
# is reported from the same binaries; to time the shipping configuration, build
# without those two flags and re-run with SKIP_BUILD=1.
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${WORK:-/tmp/rv-gate}
CC=${CC:-clang}
REPS=${REPS:-4}
FUZZ_N=${FUZZ_N:-800}
SUITE=${SUITE:-$ROOT/tools/regvm/diff-suite/diff-suite.sh}
SUITE_MIN=${SUITE_MIN:-300}
BOX2D=${BOX2D:-$ROOT/.local/box2d-gap/fixed.js}
CHECK=${CHECK:-$ROOT/.local/box2d-gap/check.js}
LOOP=${LOOP:-$ROOT/.local/regvm-build/bare-loop.js}
CHECKSUM_WANT=546.1008391043414

S="$ROOT/engine/quickjs-ng/quickjs.c $ROOT/engine/quickjs-ng/libregexp.c $ROOT/engine/quickjs-ng/libunicode.c $ROOT/engine/quickjs-ng/dtoa.c"
R="-O3 -DNDEBUG -std=gnu11 -D_GNU_SOURCE -I$ROOT/engine/quickjs-ng"
# EXTRA carries the configuration under test beyond the defaults: the phase-2
# threaded dispatch is gated as EXTRA=-DQJS_REGVM_THREAD=1.
REGFLAGS="-DQJS_REGVM=1 -DQJS_REGVM_VERIFY=1 -DQJS_REGVM_KNOBS=1 ${EXTRA:-}"

mkdir -p "$WORK"
fail=0
note() { printf '%s\n' "$*"; }
bad()  { printf 'FAIL: %s\n' "$*"; fail=1; }
lt()   { [ "$(awk -v a="$1" -v b="$2" 'BEGIN{print (a<b)}')" = 1 ]; }

if [ "${SKIP_BUILD:-0}" != 1 ]; then
    note "== build ($CC)"
    # shellcheck disable=SC2086
    $CC $R $REGFLAGS -o "$WORK/bref"   "$ROOT/tools/benchmark/qjs-bench.c" $S -lm
    # shellcheck disable=SC2086
    $CC $R             -o "$WORK/bstk"   "$ROOT/tools/benchmark/qjs-bench.c" $S -lm
    # shellcheck disable=SC2086
    $CC $R $REGFLAGS -o "$WORK/run1"   "$ROOT/.local/regvm-build/regvm-run.c" $S -lm
    # shellcheck disable=SC2086
    $CC $R             -o "$WORK/runstk" "$ROOT/.local/regvm-build/regvm-run.c" $S -lm
    # shellcheck disable=SC2086
    $CC $R $REGFLAGS -o "$WORK/fz1"    "$ROOT/.local/regvm-build/fuzzrun.c" $S -lm
    # shellcheck disable=SC2086
    $CC $R             -o "$WORK/fzstk"  "$ROOT/.local/regvm-build/fuzzrun.c" $S -lm
fi

# The phase-2 handler gates are static properties of the compiled handlers, and
# they only exist in a threaded build. Both are regressions this project has
# already shipped once: a call reachable from a handler puts a save/restore of
# all four arguments on every dispatch, and a select on the branch target puts
# the compare and both operand loads on the critical path before the next opcode
# can be fetched. Neither is visible in an instruction count or a wall time
# until it is large, so both are checked here.
case "$EXTRA" in
*QJS_REGVM_THREAD=1*)
    note "== static: phase-2 handler gates"
    if [ -z "${OBJDUMP:-}" ]; then
        for c in llvm-objdump llvm-objdump-14 objdump; do
            if command -v "$c" >/dev/null 2>&1; then OBJDUMP=$c; break; fi
        done
    fi
    if [ -n "${OBJDUMP:-}" ] && command -v python3 >/dev/null 2>&1; then
        # -c only, with -g so the symbols are there; a plain object keeps this
        # step a few seconds instead of a full link.
        $CC $R $REGFLAGS -c -o "$WORK/regvm.o" "$ROOT/engine/quickjs-ng/quickjs.c"
        "$OBJDUMP" -d --no-show-raw-insn "$WORK/regvm.o" > "$WORK/regvm.s"
        # The only exemptions are the record-creating halves of `call`,
        # `call_method` and the two tail calls. They allocate a frame from the
        # arena and fill it, so they are inherently calling and cannot be
        # frameless. The *handlers* that dispatch to them are frameless and are
        # NOT exempt: the frame sits off the dispatch path, which is the
        # property this check exists to keep.
        # get_global and put_field used to be named here; both now inline their
        # cache hit and tail-call the call, so they are back under the check.
        if python3 "$ROOT/tools/regvm/spillcheck.py" "$WORK/regvm.s" \
                --select regvm_t_ --no-frame --quiet \
                --no-push regvm_t_call --no-push regvm_t_call_method \
                --frame-exempt regvm_t_call_record \
                --frame-exempt regvm_t_call_method_record \
                --frame-exempt regvm_t_tail_call_record \
                --frame-exempt regvm_t_tail_call_method_record \
                --frame-exempt regvm_t_construct_record \
                --frame-exempt regvm_t_apply_arguments_record \
                --frame-exempt regvm_t_return_nested \
                --frame-exempt regvm_t_unwind; then
            note "   no framed handler fast path"
        else
            bad "a handler fast path carries a frame (a call is reachable)"
        fi
        if python3 "$ROOT/tools/regvm/spillcheck.py" "$WORK/regvm.s" \
                --select regvm_t_ --no-select --quiet; then
            note "   no select on a returned instruction pointer"
        else
            bad "a handler computes its next ip with a conditional select"
        fi
    else
        note "   no objdump/python3 (static gates skipped)"
    fi
    ;;
esac

note "== correctness: box2d checksum"
got=$("$WORK/bref" "$CHECK" 2>/dev/null | grep -o 'box2d-checksum=[0-9.]*' | cut -d= -f2)
if [ "$got" = "$CHECKSUM_WANT" ]; then
    note "   checksum $got OK"
else
    bad "box2d checksum '$got' != '$CHECKSUM_WANT'"
fi

note "== correctness: differential suite"
if [ -f "$SUITE" ]; then
    out=$(REGVM="$WORK/run1" STACK="$WORK/runstk" sh "$SUITE" 2>&1 || true)
    line=$(printf '%s\n' "$out" | grep -E '^pass=' | tail -1)
    note "   $line"
    # The requirement is that every kernel agrees and none is skipped. The pass
    # count is checked against a floor rather than an exact number so that adding
    # kernels cannot silently stop being counted: the suite used to print its
    # summary in the middle of the file, so it reported pass=103 while running
    # 286 kernels and 183 of them reached no line at all.
    npass=$(printf '%s' "$line" | sed -n 's/^pass=\([0-9]*\) fail=0 skip=0$/\1/p')
    if [ -z "$npass" ]; then
        bad "differential suite: $line"
    elif [ "$npass" -lt "$SUITE_MIN" ]; then
        bad "differential suite reported only $npass kernels, floor is $SUITE_MIN"
    fi
    f=$(printf '%s\n' "$out" | grep '^FAIL' || true)
    if [ -n "$f" ]; then
        printf '%s\n' "$f" | head -20
        bad "suite reported FAIL lines"
    fi
else
    note "   suite not found at $SUITE (skipped)"
fi

note "== correctness: promotion fuzzer ($FUZZ_N cases)"
if [ -f "$ROOT/.local/regvm-build/fuzz-promotion.py" ]; then
    fz=$(FUZZ_PRISTINE="$WORK/fzstk" FUZZ_PATCHED="$WORK/fz1" \
         python3 "$ROOT/.local/regvm-build/fuzz-promotion.py" 0 "$FUZZ_N" 2>&1 | tail -1)
    note "   $fz"
    case "$fz" in
        *"mismatches 0"*) ;;
        *) bad "promotion fuzzer: $fz" ;;
    esac
else
    note "   fuzzer not found (skipped)"
fi

# Gate 4 from the review: the two semantics a correctness suite cannot see. The
# interrupt test only means anything for the record design -- without it, an
# in-loop call loop is uninterruptible and a watchdog cannot stop a runaway
# script -- so it runs only in that configuration. The fixture's callee has no
# locals, which is what makes it fully threaded and therefore what makes these
# calls take the record path at all; with a locals-using callee the calls fall
# back to JS_CallInternal, which has always polled, and the test would pass for
# the wrong reason. Verified by removing the poll from the record halves: the
# same fixture then reports polls=1 and runs to completion.
case "${EXTRA:-}" in
*QJS_REGVM_RECORD=1*)
    note "== semantics: an in-loop call loop honours the interrupt handler"
    if $CC $R $REGFLAGS -o "$WORK/itest" \
            "$ROOT/.local/regvm-build/interrupt-test.c" $S -lm 2>/dev/null; then
        if out=$("$WORK/itest" \
                     "$ROOT/.local/regvm-build/call-loop-nolocals.js" 5 2>&1); then
            note "   $(printf '%s' "$out" | tr '\n' ' ')"
        else
            bad "an in-loop call loop did not honour JS_SetInterruptHandler"
        fi
    else
        bad "could not build the interrupt test"
    fi
    ;;
esac

note "== cost: box2d fixed work, min of $REPS"
run_ms() { "$1" "$2" 2>/dev/null | grep -o 'ms=[0-9.]*' | tail -1 | cut -d= -f2; }
reg=""; stk=""; herm=""; mono=""
i=0
while [ "$i" -lt "$REPS" ]; do
    v=$(run_ms "$WORK/bref" "$BOX2D"); if [ -z "$reg" ] || lt "$v" "$reg"; then reg=$v; fi
    v=$(run_ms "$WORK/bstk" "$BOX2D"); if [ -z "$stk" ] || lt "$v" "$stk"; then stk=$v; fi
    if [ -n "$HERMES" ]; then
        v=$(run_ms "$HERMES" "$ROOT/.local/box2d-gap/fixed.hbc")
        if [ -z "$herm" ] || lt "$v" "$herm"; then herm=$v; fi
    fi
    if [ -n "$MONO" ]; then
        v=$(run_ms "$MONO" "$BOX2D"); if [ -z "$mono" ] || lt "$v" "$mono"; then mono=$v; fi
    fi
    if [ -n "$BASE" ]; then
        v=$(run_ms "$BASE" "$BOX2D"); if [ -z "$base" ] || lt "$v" "$base"; then base=$v; fi
    fi
    i=$((i + 1))
done
note "   register ${reg} ms"
note "   stack    ${stk} ms  ($(awk -v a="$reg" -v b="$stk" 'BEGIN{printf "%.3fx", a/b}') register/stack)"
if [ -n "$BASE" ]; then
    # A wall-time *baseline*: the previous revision's binary for the same
    # configuration. "register beats the monolith" is not a regression gate --
    # R8's first batch passed that assertion while being 6.6% slower than the
    # revision it replaced, because nothing compared it to the revision it
    # replaced. Tolerance is 2%, since min-of-N on a shared machine moves by more
    # than that between runs.
    note "   baseline ${base} ms  ($(awk -v a="$reg" -v b="$base" 'BEGIN{printf "%.3fx", a/b}') register/baseline)"
    if awk -v a="$reg" -v b="$base" 'BEGIN{exit !(a > b * 1.02)}'; then
        bad "register is more than 2% slower than the baseline revision: ${reg} ms vs ${base} ms"
    else
        note "   register is not slower than the baseline revision"
    fi
fi
if [ -n "$MONO" ]; then
    note "   monolith ${mono} ms  ($(awk -v a="$reg" -v b="$mono" 'BEGIN{printf "%.3fx", a/b}') register/monolith)"
    # Fail only if the monolith is *strictly* faster. A min-of-N tie is not a
    # regression, and at 41 ms the two builds tie.
    if lt "$mono" "$reg"; then
        bad "register slower than the monolith: ${reg} ms vs ${mono} ms"
    else
        note "   register beats the monolith (or ties)"
    fi
fi
if [ -n "$HERMES" ]; then
    note "   hermes   ${herm} ms  ($(awk -v a="$reg" -v b="$herm" 'BEGIN{printf "%.3fx", a/b}') register/hermes)"
fi

note "== cost: empty loop, min of $REPS"
# The loop is timed *and* checked. Its output is the iteration count, and it is
# the one kernel whose instructions are converted by the phase-2 handlers, so a
# wrong advance in one of them shows up here as a crash rather than as a wrong
# number -- which is how the narrow `add_imm_self_branch_s` target width went
# unnoticed while the suite, the fuzzer and the checksum all passed.
got=$("$WORK/bref" "$LOOP" 2>/dev/null | grep -o 'checksum=[0-9]*' | cut -d= -f2)
if [ "$got" = "20000000" ]; then
    note "   checksum $got OK"
else
    bad "empty loop checksum '$got' != '20000000'"
fi
lr=""; lm=""
i=0
while [ "$i" -lt "$REPS" ]; do
    v=$(run_ms "$WORK/bref" "$LOOP"); if [ -z "$lr" ] || lt "$v" "$lr"; then lr=$v; fi
    if [ -n "$MONO" ]; then
        v=$(run_ms "$MONO" "$LOOP"); if [ -z "$lm" ] || lt "$v" "$lm"; then lm=$v; fi
    fi
    i=$((i + 1))
done
note "   register ${lr} ms for 20e6 iterations"
if [ -n "$MONO" ]; then
    note "   monolith ${lm} ms  ($(awk -v a="$lr" -v b="$lm" 'BEGIN{printf "%.3fx", a/b}') register/monolith)"
    if lt "$lm" "$lr"; then
        bad "register loop slower than the monolith: ${lr} ms vs ${lm} ms"
    else
        note "   register beats the monolith (or ties)"
    fi
fi

note "== coherence"
if command -v node >/dev/null 2>&1; then
    ( cd "$ROOT" && node scripts/apply-patches.js --check ) || bad "apply-patches --check"
    ( cd "$ROOT" && node scripts/sync-quickjs-rel.js --check ) || bad "sync-quickjs-rel --check"
else
    note "   node not on PATH (skipped)"
fi

printf '\n'
if [ "$fail" = 0 ]; then note "GATE PASS"; else note "GATE FAIL"; fi
exit "$fail"
