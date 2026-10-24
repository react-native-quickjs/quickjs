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

`QJS_AOT_PROFILE` is a compile-time flag, not a runtime switch: a build without
it contains no profiling code, and a build with it and no `QJS_AOT_PROFILE_OUT`
skips every hook at run time. Add it to the dev build that records the profile
and drop it for shipping; the shipping `.text` is byte-identical with the flag
off.

`QJS_AOT_PROFILE_OUT` makes the profiler dump every function's 128-bit
bytecode digest, dynamic op count, loop trip counts, per-register and
per-instruction type sets with numeric ranges, PIC shapes, call sites, coverage,
branch bias and sampled self time at teardown. Merge several runs with
`tools/aot/profmerge.py`; a merged profile is a superset of what the selector
needs.

Two conditions keep a profile's digests identical to what ships:

- **the release JS bundle.** The profiled bundle must be the one that ships
  (Metro `--dev false`, minified, `__DEV__` false). A development bundle is
  different code and its digests match nothing.
- **no debugger attached while the code loads.** The parser emits a trap per
  statement only when a debugger is attached at parse time, and those traps
  change the register bytecode. The profiler marks such a function `dbg=1` on
  its `F` line, so it is visible rather than silent.

Either engine build works: the facts are counts, types, shapes and targets, not
timings. A release build is preferred for a long session, because the app runs
at full speed and the developer exercises more of it.

The generator reports the **bound fraction** at build time -- of the profile's
function records, how many the built bundle still has by digest. A low number
means the profile is stale or foreign (a changed bundle, another engine, another
build configuration), which would otherwise be silent.

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

### ThinLTO: recommended for release builds and CI

Add `-DQJS_AOT_LTO=ON` to put the engine and the unit in one module at link
time:

```
cmake -DQJS_REGVM=ON -DQJS_AOT=ON -DQJS_AOT_LTO=ON \
      -DQJS_AOT_UNIT=$PWD/aot_unit.c ...
```

The unit calls an engine runtime stub on every slow path, inline-cache miss and
generic operation. Compiled as separate objects those calls are opaque, so no
stub is ever inlined or specialised for the site that reaches it. With ThinLTO
the engine's stubs inline into generated code: `qjs_aot_step` and
`qjs_aot_set_reg_p` disappear entirely, and the unit's calls into the engine
drop by about two thirds. Measured on four benchmark kernels that is 6.1% fewer
instructions and 5.3% fewer cycles in geomean, with no kernel slower.

It needs clang and lld; the option refuses any other compiler rather than
silently ignoring LTO. Compile time is roughly unchanged -- the unit lowers to
bitcode faster and the link does the rest -- and the peak memory of a single
compile drops, which matters when several units build in parallel.

Two flags are added to the **link** command by the option, not just the compile
command:

- `-mbranches-within-32B-boundaries`, the JCC erratum workaround. LTO generates
  code at link time, so a flag given only at compile time never reaches the
  backend. A link without it executes 4.4% *fewer* instructions and runs 2.2%
  *slower*.
- `-ffp-contract=off`, so floating-point results are identical to the
  non-LTO build.

The measurement harness audits a built binary for the first of these and fails
rather than report a number taken from a link that lost it.

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
