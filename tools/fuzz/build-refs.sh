#!/bin/bash
# build-refs.sh : the reference interpreters diff.sh compares, as qjs-run targets under $CK (default .local/scratch/ck):
#   stack-rel, reg-rel   Release builds of the stack and register VMs
#   stack-gcs, reg-gcs   the same with the GC stress mode (QJS_GC_STRESS=ON, assertions on); diff.sh runs these when they exist,
#                        and their ctest runs the differential corpus under both stress settings
# JOBS (default 2) sets the build parallelism.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CK=${CK:-$ROOT/.local/scratch/ck}; mkdir -p "$CK"
for v in stack-rel reg-rel stack-gcs reg-gcs; do
  case $v in reg-*) vm=ON ;; *) vm=OFF ;; esac
  case $v in *-gcs) ty=RelWithDebInfo; gs=ON; cf="-UNDEBUG -O1" ;; *) ty=Release; gs=OFF; cf= ;; esac
  cmake -S "$ROOT" -B "$CK/$v" -DCMAKE_C_COMPILER=${CC:-clang} -DCMAKE_CXX_COMPILER=${CXX:-clang++} -DCMAKE_BUILD_TYPE=$ty -DQJS_REGVM=$vm -DQJS_GC_STRESS=$gs -DCMAKE_C_FLAGS="$cf" > "$CK/$v.configure.log" 2>&1 || { echo "configure $v failed: $CK/$v.configure.log"; exit 1; }
  cmake --build "$CK/$v" --target qjs-run -j "${JOBS:-2}" > "$CK/$v.build.log" 2>&1 || { echo "build $v failed: $CK/$v.build.log"; exit 1; }
  echo "$v: $CK/$v/tests/qjs-run"
done
