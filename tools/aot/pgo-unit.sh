#!/bin/bash
# pgo-unit.sh UNIT.c OUT.o: compile one generated unit, profile-guided when the unit is large.
#
#   CC          the compiler wrapper (default: ccsplit next to this script)
#   CFLAGS      flags of the plain build (-O2, -I..., -D...), as one string
#   LINK        link command for a probe binary, with @OBJ@ for the unit object and @OUT@ for the binary
#   RUN         command that runs the program the unit belongs to, with @BIN@ for the probe binary
#   PGO_MIN_C   smallest unit, in bytes of generated C, that is built profile-guided (default 4000000: about 2 MB of
#               text; every Octane row below it is within 1.5 MB of C and every row above it is at least 4.4 MB)
#   PGO_OFF=1   always the plain build
#
# A unit under the threshold is compiled once, as before. A unit at or over it is compiled instrumented, linked into a probe
# binary, run once through RUN, and compiled again with the merged profile and -fsplit-machine-functions. E_TEXT is left
# empty for both compiles: a function pinned to the `aottext` section cannot be split, and nothing reads that section. The
# parts of both compiles go in one fixed directory, so the profile's function names match.
set -u
unit=$1 out=$2
here=$(cd "$(dirname "$0")" && pwd)
CC=${CC:-$here/ccsplit}
CFLAGS=${CFLAGS:-"-O2"}
min=${PGO_MIN_C:-4000000}
size=$(stat -c %s "$unit")
if [ -n "${PGO_OFF:-}" ] || [ "$size" -lt "$min" ] || [ -z "${LINK:-}" ] || [ -z "${RUN:-}" ]; then
  exec $CC $CFLAGS -c "$unit" -o "$out"
fi
prof=${LLVM_PROFDATA:-$(dirname "$(command -v "${CCSPLIT_CC:-clang}")")/llvm-profdata}
[ -x "$prof" ] || prof=$(command -v llvm-profdata) || exec $CC $CFLAGS -c "$unit" -o "$out"
work=$(mktemp -d "${out}.pgo.XXXXXX") || exec $CC $CFLAGS -c "$unit" -o "$out"
trap 'rm -rf "$work"' EXIT
t0=$(date +%s)
ref="$work/ref"; probe="$work/probe"
if ! CCSPLIT_PGO=1 CCSPLIT_WORKDIR=$work/parts $CC $CFLAGS -DE_TEXT= -DE_LOGO_COLD -fprofile-instr-generate -c "$unit" -o "$work/i.o" > "$work/i.log" 2>&1; then
  echo "pgo-unit: instrumented compile failed, plain build" >&2; exec $CC $CFLAGS -c "$unit" -o "$out"
fi
l=${LINK//@OBJ@/$work/i.o}; l=${l//@OUT@/$probe}
if ! eval "$l -fprofile-instr-generate" > "$work/l.log" 2>&1; then
  echo "pgo-unit: instrumented link failed, plain build" >&2; exec $CC $CFLAGS -c "$unit" -o "$out"
fi
rm -f "$work/i.o"
r=${RUN//@BIN@/$probe}
LLVM_PROFILE_FILE=$work/p%p.profraw eval "$r" > "$work/run.log" 2>&1
if ! ls "$work"/p*.profraw > /dev/null 2>&1 || ! "$prof" merge "$work"/p*.profraw -o "$work/u.profdata" > "$work/m.log" 2>&1; then
  echo "pgo-unit: no profile from the probe run, plain build" >&2; exec $CC $CFLAGS -c "$unit" -o "$out"
fi
rm -f "$work"/p*.profraw "$probe"
if CCSPLIT_WORKDIR=$work/parts $CC $CFLAGS -DE_TEXT= -DE_LOGO_COLD -fprofile-instr-use="$work/u.profdata" -fsplit-machine-functions -c "$unit" -o "$out" > "$work/o.log" 2>&1; then
  echo "pgo-unit: $(basename "$unit") profile-guided ($(( $(date +%s) - t0 ))s)" >&2
  exit 0
fi
echo "pgo-unit: profile-guided compile failed, plain build" >&2
exec $CC $CFLAGS -c "$unit" -o "$out"
