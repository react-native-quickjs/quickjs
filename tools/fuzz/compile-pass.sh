#!/bin/bash
# compile-pass.sh QJSC [QJSC...] : compile every JS file of the JetStream 3 submodule and the differential corpus with each
# qjsc (one per VM build), without running them. A file that one build compiles and another rejects, or that a build
# rejects with an InternalError or crashes on, is listed. Exit 0 = none.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
W=$ROOT/.local/scratch/compile-pass; rm -rf "$W"; mkdir -p "$W"
mapfile -t files < <(find "$ROOT/tools/benchmark/jetstream3/JetStream" "$ROOT/tests/differential/corpus" -name '*.js' -size -40M \
  -not -path '*/node_modules/*' | sort)
bad=0; n=0
for f in "${files[@]}"; do
  n=$((n+1)); res=
  for q in "$@"; do
    out=$(timeout 120 "$q" "$f" "$W/o.bc" 2>&1); rc=$?
    cls=ok
    [ $rc -ne 0 ] && cls="err"
    [ $rc -ge 124 ] && cls="crash$rc"
    case "$out" in *InternalError*|*"stack size"*|*"buffer overflow"*|*Assertion*) cls="internal";; esac
    res="$res $cls"
    [ "$cls" = internal ] || [ "${cls#crash}" != "$cls" ] && echo "$cls $(basename "$q") ${f#$ROOT/}: $(echo "$out" | head -1 | cut -c1-160)"
  done
  first=${res# }; first=${first%% *}
  for c in $res; do [ "$c" != "$first" ] && { echo "differ [$res ] ${f#$ROOT/}"; bad=1; break; }; done
  case "$res" in *internal*|*crash*) bad=1;; esac
done
rm -rf "$W"
echo "files=$n bad=$bad"
exit $bad
