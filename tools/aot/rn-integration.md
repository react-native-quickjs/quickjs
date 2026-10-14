# AOT unit: React Native integration

The register-VM AOT unit is plain, ahead-of-time C. There is no runtime code
generation: the engine never `mmap`s executable memory and
`quickjs-aot-rt.h` includes only `quickjs.h`. That makes the unit
App-Store-safe and usable on iOS, where JIT is not allowed.

## 1. Profile in a dev build

Build the engine with the profiler on and run the app (or a representative
benchmark) on device or simulator:

```
clang ... -DQJS_REGVM=1 -DQJS_AOT_PROFILE=1 -no-pie ...
QJS_AOT_PROFILE_OUT=app.qjsprof  ./MyApp
```

`QJS_AOT_PROFILE_OUT` makes the profiler dump every function's 128-bit
bytecode digest, dynamic op count, loop trip counts, per-register type sets,
PIC shapes and call sites at teardown. Merge several runs with
`tools/aot/profmerge.py`; a merged profile is a superset of what the selector
needs.

Commit `app.qjsprof` next to the bundle. It is data, not code, and it is only
a hint: the unit is correct for any input, and a function the profile never
saw simply runs in the interpreter.

## 2. Generate and build the unit

Add a build step (this is what `cmake/quickjs.cmake` does when
`-DQJS_AOT_UNIT=<path>` is set):

```
qjsaot --out aot_unit.c --profile app.qjsprof --budget 1000000 bundle.js
clang -c aot_unit.c -o aot_unit.o        # host or target triple
```

and link `aot_unit.o` into the same binary as the engine. The unit exports
only `qjs_aot_table`, `qjs_aot_count` and `qjs_aot_abi`; the engine binds each
function by digest when it is first compiled. `QJS_AOT_ABI` must match the
engine, or the whole unit is ignored at init.

CMake:

```
cmake -DQJS_REGVM=ON -DQJS_AOT=ON -DQJS_AOT_UNIT=$PWD/aot_unit.c ...
```

## 3. Selection and budget

`qjsaot --profile` ranks functions by profiled ops, takes the prefix covering
`--hot-cover` (default 0.95) of the ops, drops startup-only entries below
`--min-ops` (default 1000), closes over compiled callees, and drops the lowest
ops/byte until the emitted body fits `--budget` bytes. `--all` disables
selection. The generator prints the selected count, op coverage and emitted
bytes so the build can log them.

## 4. Shipping safety

- The unit is keyed by the function's 128-bit register-VM bytecode digest. A
  bundle change gives the function a new digest, it finds no table entry, and
  it runs in the interpreter — no stale code can run. `tools/aot/stale-test.sh`
  exercises this.
- A different bundle with the same engine is fine. An engine ABI change
  disables the whole unit at init.
- At teardown, `QJS_AOT_STATS=1` prints `unit=1 table=N bound=B enter=E`: a
  low `bound/table` ratio in the field means the committed profile or bundle
  drifted from the shipped unit.
