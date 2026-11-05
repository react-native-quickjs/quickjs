#!/bin/bash
# aot-build.sh TRAIN.js OUTDIR : profile TRAIN.js, generate and compile its AOT unit (no LTO, -O1), link OUTDIR/run
# Needs the m1 objects (.local/m1/m1.sh build). The binary runs a script like qjs-bench/run-plain.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
M=$ROOT/.local/m1; S=$ROOT/engine/quickjs-ng
T=$1; O=$2; mkdir -p "$O"
BASE="-std=gnu11 -D_GNU_SOURCE -DJS_ENABLE_DEBUGGER=0 -I$S"
L="$M/libregexp.o $M/libunicode.o $M/dtoa.o -lm"
QJS_AOT_PROFILE_OUT=$O/p.qjsprof timeout ${PROF_TIMEOUT:-60} $M/run-prof "$T" > "$O/prof.out" 2>&1
prc=$?
if [ $prc -ge 124 ] || grep -q Assertion "$O/prof.out"; then echo "profile-run failed rc=$prc"; exit 2; fi
timeout ${GEN_TIMEOUT:-120} $M/qjsaot --out "$O/u.c" --layout-header "$O/u-l.h" --report "$O/report.txt" --facts "$O/u.facts.txt" --profile "$O/p.qjsprof" --hot-cover 0.99 "$T" > "$O/gen.log" 2>&1 || { echo "generator failed"; exit 3; }
sed -i "s|#include \"$O/u-l.h\"|#include \"u-l.h\"|" "$O/u.c"
clang -O1 $BASE ${EXTRA_CFLAGS:-} -I"$O" -ffp-contract=off -DQJS_REGVM=1 -DQJS_AOT=1 -c "$O/u.c" -o "$O/u.o" 2> "$O/cc.log" || { echo "unit compile failed"; exit 4; }
clang -o "$O/run" $M/qjs-bench.o "$O/u.o" $M/aot.o $L || { echo "link failed"; exit 5; }
