# Benchmark suite

A single entry point (`tools/benchmark/bench.mjs`) for every engine benchmark in
this repo. Runs the patched engine against Hermes (and optionally `node`), on
desktop and Android, and records/gates results against a committed baseline.

Everything is release-optimized: `bench` builds `qjs-bench` and the call driver
in a `build-release` directory (`-DCMAKE_BUILD_TYPE=Release`, `-O2`) and runs
measurements through those binaries, so the numbers reflect what an optimized
engine does, not a debug build.

## Quick start

The `bench` npm script builds the release binaries if missing, then runs every
suite against the engine:

```sh
npm run bench
```

It also auto-builds on demand: if `bench.mjs` can't find `qjs-bench` /
`native-call-bench` it compiles them in `build-release` before measuring.

List what is available, select a few rows, or run one suite:

```sh
node tools/benchmark/bench.mjs --list
node tools/benchmark/bench.mjs rn
node tools/benchmark/bench.mjs octane --rows splay,richards
node tools/benchmark/bench.mjs compiler --rows int.,fp.arith
node tools/benchmark/bench.mjs react rn --rows create-element
```

## Suites

| suite | rows | what it measures |
|---|---|---|
| `react` | 8 | React reconciler hot paths (element/fiber/hook alloc, reconcile, whole-tree render) |
| `minireact` | 1 | Meta's MiniReact benchmark, reconciler from the Hermes tree (calibration anchor) |
| `ic` | 13 | foundational and composed ICs: own/direct-prototype reads, two/four-shape polymorphism, depth-two prototypes, overwrites, transitions, megamorphic and mutation-guard sites |
| `rn` | 6 | RN 0.85 native-prop payload: `diffProperties`/`flattenStyle`/`deepDiffer` at real 187/150-key scale |
| `primitives` | 17 | for-in, keyed loads into large configs, mono/poly/mega IC ladder, closure alloc |
| `data` | 20 | JSON.parse/stringify, sort, array chains, Object.assign, Map lookups |
| `strings` | 19 | template literals, string building, the RN color 9-regex ladder, charCodeAt, number→string |
| `surface` | ~30 | one-operation-per-row language/library gap sweep |
| `suspense` | 4 | throw-based control flow (Suspense, error boundary) + non-throwing control |
| `modern` | 15 | class/promise/async-await/iterator/Map/Set/module patterns |
| `arrayholes` | 5 | array holes fill/read-back |
| `octane` | 9 (13 with `--all`) | stock Octane (time-budgeted `Score:` per row; zlib & codeload excluded). `splay`, `gbemu`, `regexp` and `pdfjs` are held out of the default run because the engine already wins or ties them, and an equal-weight geomean would count them as much as the rows being worked on |
| `jetstream3` | 62 (72 with `--all`) | JetStream 3 (WebKit, 3.0.0-alpha) JavaScript workloads, run by JetStream's own `cli.js` driver from the `jetstream3/JetStream` submodule; the score per row is the driver's (geomean of First / Worst / Average). The default run is JetStream's default set; `--all` adds the js workloads outside it. See [JetStream 3](#jetstream-3) |
| `compiler` | 94 | AOT `compiler_bench` kernels (fixed-work, per-kernel checksum) |
| `jit` | 26 | jitbench kernels (corpus shapes + defect canaries; 8-call warmup) |
| `calls` | ~50 | JavaScript → C call boundary shapes (arity, pad, closure, magic, accessor, apply/spread) |
| `startup` | 4 | cold start phases (needs a bundle/file via `--n`) |

Workload rows carry `expect` guards (and a dead-code-elimination canary) so a
benchmark that computes the wrong thing is reported as an error, not a number.

## Comparing against Hermes

Hermes runs as an extra engine when it is present:

```sh
node tools/benchmark/bench.mjs rn --engine hermes
```

hermes and hermesc are cached in `tools/benchmark/bin/` (gitignored):

- `bin/mac/hermes`, `bin/mac/hermesc` — the host (macOS) VM and the AOT
  bytecode compiler (`hermesc` is host-only; it compiles bundles to Hermes
  bytecode ahead of time).
- `bin/android/aarch64/hermes` — the Android aarch64 VM, used by `--device`.
- `bin/android/aarch64/qjs-bench`, `bin/android/aarch64/native-call-bench` —
  our cross-compiled drivers.

On first use they are pulled from an existing Hermes build or built from the
source tree, then cached in `bin/`. The default source root is
`/Users/ammarahmed/Work/hermes/hermes`, overridable with `HERMES_SRC`, and the
host binary with `HERMES_BIN` (which bypasses provisioning entirely). If the
disk is too full to hold an extra copy, a large binary is linked in place as a
symlink instead.

When more than one engine runs, the engine's post-GC memory snapshot
(`--mem`) is captured for QJS and shown as `[memory]`.

## A/B'ing two engine builds

```sh
node tools/benchmark/bench.mjs react --ab base=build-old/qjs-bench,patch=build-new/qjs-bench
```

Each `--ab label=path` adds an engine. Use it to compare an engine change
against its base binary.

## Recording and gating

```sh
node tools/benchmark/bench.mjs --save                 # record to tools/benchmark/baseline.json
node tools/benchmark/bench.mjs --against              # compare, exit 1 on regression
```

`--against` pins iteration counts from the baseline so both runs do identical
work (calibrating per-run causes false regressions of >100%). Per-kernel and
octane rows are gated on their score; workloads on ns/op with a 5% threshold.

npm aliases:

```sh
npm run bench:octane
npm run bench:workloads
npm run bench:kernels
npm run bench:calls
npm run bench:startup -- --n path/to/index.android.bundle
npm run bench:record
npm run bench:gate
npm run bench:list
```

## Row selection

`--rows`/`--skip` are substrings over the full row id; comma-separate to select
several:

```sh
node tools/benchmark/bench.mjs workloads --rows for-in,keyed-load
node tools/benchmark/bench.mjs all --skip spay,pdfjs,mandreel
```

The octane suite already skips four rows by default (`splay`, `gbemu`, `regexp`,
`pdfjs`). `--all` includes them; nothing else does, so an `--rows` naming one of
them still leaves it out:

```sh
node tools/benchmark/bench.mjs octane --all
```

## JetStream 3

`jetstream3` runs JetStream 3's plain-JavaScript workloads through the
unmodified JetStream driver and workload sources, from the submodule at
`tools/benchmark/jetstream3/JetStream` (pinned to WebKit/JetStream `5fb9b49`):

```sh
git submodule update --init tools/benchmark/jetstream3/JetStream
node tools/benchmark/bench.mjs jetstream3 --engine regvm --engine node --engine bun
node tools/benchmark/bench.mjs jetstream3 --rows /Air,/richards --js3-iterations 2 --js3-worst-case 1
```

Each row is one process: `cli.js --no-prefetch --test=<name>`, started in the
submodule with a shell shim that gives the engine what JetStream expects of a
JS shell (`print`, `printErr`, `load`, `read`, `readFile`, `arguments`, and
`runString`, a fresh global per workload):

- `qjs` / `regvm`: `qjs-bench --shell jetstream3/shell-qjs.js -- <driver args>`.
  `--shell` is a `qjs-bench` flag that adds those functions (`runString` makes a
  new context on the same runtime); without it the global object is unchanged.
  `qjs` is `build-release/qjs-bench` (stack VM), `regvm` is
  `build-regvm/qjs-bench` (register VM, or `QJS_REGVM_BENCH`).
  Rows tagged `internationalization` (the `intl` group) also get `--intl`,
  which installs `Intl` from `modules/intl` in every shell realm (CMake option
  `QJS_BENCH_INTL`, on by default). A host build links that module's en-US/UTC
  stub backend, not a locale database: every locale formats as en-US, so
  host `intl` numbers measure the module's JavaScript layer and the stub, not
  what a device's backend (`android.icu`, Foundation) does, and the workload's
  output-length checks for `DateTimeFormat` and `PluralRules` fail.
- `node` / `bun`: `jetstream3/shell-node.cjs`, scripts and realms through
  `node:vm`.
- `bun-aot`: the same shim compiled by bun's ahead-of-time mode,
  `bun build --compile --bytecode --format=cjs --target=bun shell-node.cjs`
  (bun 1.4.4 has no `--aot` flag; `--compile --bytecode` is the released
  ahead-of-time path). Only the shim is in the bytecode cache: the driver loads
  every workload from source into a fresh realm at run time, so workload code
  is compiled at run time as with plain `bun`.

- `bun-canary-aot`: bun canary's `bun build --compile --aot` (machine code at
  build time; bun 1.4.5 canary at `~/.local/bun-canary/bun-linux-x64/bun`, or
  `BUN_CANARY_BIN`). It compiles only code in the bundle, so each workload gets
  its own entry, `jetstream3/build/aot/<name>/entry.cjs`: the real driver runs
  in `node:vm` with a `runString` that records the scripts it would give the
  workload's realm (its preamble, the workload files, its own runner loop),
  and the entry runs them as sloppy-mode CommonJS functions (with `exports`,
  `module`, `define`, `require` and `process` hidden, so UMD bundles set
  globals and nothing takes a Node path) with the preloads
  inlined behind `read`; a tail scores the iteration times as the driver does. The build
  writes `explain.json` (`--explain=json`) and `build.log` beside the
  executable, and `build/aot/builds.json` keeps each build's time and size.
  Top-level names of each realm's scripts are mirrored onto `globalThis`
  (found with acorn), so eval'd code and `globalThis.X` see them as in a
  shell. Known bun failures, not the entry's: delta-blue (compiled
  executables make built-in prototypes read-only) and js-tokens (wrong
  token count under plain `bun` too; node runs the same entry correctly).

The node, bun, bun-aot and bun-canary-aot engines run this suite only. Without prefetching the
driver reads the uncompressed name of each `.z` resource; those are inflated
once into `jetstream3/build/assets` (gitignored), which the shims read when the
submodule lacks a path. `--js3-iterations N` / `--js3-worst-case N` override
the driver's counts (worst-case must stay below iterations) for a quick
correctness pass; scores from such a run are not JetStream scores.

A long comparison run can be read before it ends: each finished (row, engine)
result is appended as one JSON line to `--partial FILE`, or by default, when
`--json` output is redirected to `X.json`, to `X.partial.jsonl` beside it.

The node engine runs each workload inside `vm.createContext` (the shim's
`runString`), where V8 reaches globals through interceptors; on global-heavy
workloads (richards, earley-boyer, mandreel, delta-blue) that costs node up to
35x, so those node scores understate V8.

Excluded (no WebAssembly or Workers in a JS shell): the Wasm workloads, not
tagged `js` by JetStream -- `HashSet-wasm`, `quicksort-wasm`, `gcc-loops-wasm`,
`tsf-wasm`, `richards-wasm`, `sqlite3-wasm`, `Dart-flute-complex-wasm`,
`Dart-flute-todomvc-wasm`, `Kotlin-compose-wasm`, `transformersjs-bert-wasm`,
`transformersjs-whisper-wasm`, `tfjs-wasm`, `tfjs-wasm-simd`, `argon2-wasm`,
`8bitbench-wasm`, `zlib-wasm`, `dotnet-interp-wasm`, `dotnet-aot-wasm`,
`j2cl-box2d-wasm` -- the `WorkerTests` workloads `bomb-workers` and
`segmentation`, which JetStream itself skips in shells, and `source-map-wtb`,
which JetStream tags `js` but whose source-map library parses mappings in
WebAssembly (`EXCLUDED_NAMES` in `jetstream3/jetstream3.mjs`).

Rows that fail on the engine (2026-11-14): `intl` (outside the default set) runs
with `--intl` but fails the workload's own output-length check for
`DateTimeFormat` and `PluralRules`, because the host links the stub Intl backend
(see above). Every other row runs on both VMs.

## Android device

Run the same suites and scoring on a connected Android device. `--device`
cross-compiles `qjs-bench` / `native-call-bench` for `aarch64-linux-android`
with the NDK, pushes them and the row sources to `/data/local/tmp/rnqjs`, and
runs each measurement there (QJS only; Hermes columns stay on the host):

```sh
node tools/benchmark/bench.mjs rn octane --device
```

Requires `adb` on `PATH` (or in `$ANDROID_HOME/platform-tools`) and exactly one
connected device; `ANDROID_NDK_HOME` points at an NDK (defaults to the SDK's
latest 27.x). A missing device or more than one attached device fails fast.
Cross-compiled drivers and the Android Hermes VM are cached in
`tools/benchmark/bin/android/`.

## Controller

`bench.mjs` dispatches, selects rows, and reports. `qjs-bench.c` is the driver
(provides `print`, runs source or `.bc`). `native-call-bench.c` (from
`calls.c`) is the JS→C call driver. `jetstream3/jetstream3.mjs` lists the
JetStream workloads (by evaluating `JetStreamDriver.js`), inflates their assets
and builds the bun-aot shim. `lib/harness.js` defines the `bench()` row
interface used by the workload files. `lib/hermes.mjs` provisions and caches the
Hermes VM and `hermesc`. `lib/device.mjs` cross-compiles and drives `--device`.
The octane corpus is assembled from vendor fixtures by `octane/build-octane.mjs`.

The kernel files and workload files are largely the upstream/RN-shaped sources
(ES5) so Hermes and QJS run byte-identical code.
