#!/usr/bin/env bash
#
# compile-bench.sh -- the compile speed of qjsc, measured on its own.
#
#   compile-bench.sh [--qjsc PATH] [file.js...]
#
# Benchmarks, profiles and AOT run on qjsc bytecode, so their numbers never
# include compiling. This script measures compiling: `perf stat -e
# cycles:u,instructions:u` of one qjsc run per file, in both modes, the
# optimising compiler (qjsc's default, what an app ships) and the fast mode
# (--fast-compile, what the runtime uses for source it compiles at run time).
#
# The default set is the babylon bundle and the Octane corpus (typescript
# included). Prints one row per file and a total per mode. The bytecode is
# written under build-bench/compile-bench and deleted afterwards.
#
#   --qjsc PATH   the compiler (default: $QJSC, else build-regvm/qjsc)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
QJSC=${QJSC:-$ROOT/build-regvm/qjsc}
while [ $# -gt 0 ]; do
  case $1 in
    --qjsc) QJSC=$2; shift 2 ;;
    --) shift; break ;;
    -*) echo "compile-bench: unknown option $1" >&2; exit 2 ;;
    *) break ;;
  esac
done
[ -x "$QJSC" ] || { echo "compile-bench: no qjsc at $QJSC" >&2; exit 2; }
command -v perf >/dev/null || { echo "compile-bench: perf not found" >&2; exit 2; }

if [ $# -eq 0 ]; then
  set -- "$ROOT/tools/benchmark/jetstream3/JetStream/workloads/babylonjs/dist/bundle.es6.min.js" \
         "$ROOT"/tools/benchmark/octane/corpus/*.js
fi

OUT=$ROOT/build-bench/compile-bench
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

# measure <file> [qjsc flag]: prints "cycles instructions" of one compile.
measure() {
  local f=$1; shift
  perf stat -x, -e cycles:u,instructions:u -o "$OUT/stat" \
    "$QJSC" "$@" "$f" "$OUT/out.bc" > "$OUT/log" 2>&1 \
    || { cat "$OUT/log" >&2; echo "compile-bench: qjsc failed on $f" >&2; exit 1; }
  awk -F, '$3 ~ /^cycles/ {c=$1} $3 ~ /^instructions/ {i=$1} END {print c, i}' "$OUT/stat"
}

printf '%-28s %9s %15s %15s %15s %15s %7s\n' file KB opt-cycles opt-instr fast-cycles fast-instr opt/fast
toc=0; toi=0; tfc=0; tfi=0
for f in "$@"; do
  read -r oc oi < <(measure "$f")
  read -r fc fi_ < <(measure "$f" --fast-compile)
  toc=$((toc + oc)); toi=$((toi + oi)); tfc=$((tfc + fc)); tfi=$((tfi + fi_))
  printf '%-28s %9d %15d %15d %15d %15d %7.2f\n' "$(basename "$f")" $(( $(stat -c %s "$f") / 1024 )) \
    "$oc" "$oi" "$fc" "$fi_" "$(awk -v a="$oc" -v b="$fc" 'BEGIN{print a/b}')"
done
printf '%-28s %9s %15d %15d %15d %15d %7.2f\n' total "" "$toc" "$toi" "$tfc" "$tfi" \
  "$(awk -v a="$toc" -v b="$tfc" 'BEGIN{print a/b}')"
