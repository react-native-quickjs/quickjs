#!/bin/bash
# difffuzz.sh WORKER NWORKERS SECONDS OUTDIR : run diff.sh over seeds WORKER, WORKER+N, ...; failing seeds are kept
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
w=$1; n=$2; secs=$3; out=$4; mkdir -p "$out/work" "$out/fail"
case $((w % 4)) in
  0) ;;
  1) export QJS_AOT_REGION_ACCUM=1 ;;
  2) export QJS_AOT_REGION_NOLEND=1 ;;
  3) export QJS_AOT_REGION_MSHAPE=1 QJS_AOT_REGION_CRECV=1 ;;
esac
end=$(( $(date +%s) + secs )); seed=$w; runs=0; fails=0
while [ $(date +%s) -lt $end ]; do
  if ! KEEP= "$ROOT/tools/fuzz/diff.sh" $seed "$out/work" > "$out/work/s$seed.log" 2>&1; then
    fails=$((fails+1)); mkdir -p "$out/fail/s$seed"; cp -r "$out/work/s$seed" "$out/fail/s$seed/" 2>/dev/null; cp "$out/work/s$seed.log" "$out/fail/s$seed/"
  fi
  rm -rf "$out/work/s$seed" "$out/work/s$seed.log"
  runs=$((runs+1)); seed=$((seed+n))
  echo "w$w runs=$runs fails=$fails seed=$seed" > "$out/progress.$w"
done
