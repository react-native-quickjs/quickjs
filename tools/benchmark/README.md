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
| `rn` | 6 | RN 0.85 native-prop payload: `diffProperties`/`flattenStyle`/`deepDiffer` at real 187/150-key scale |
| `primitives` | 17 | for-in, keyed loads into large configs, mono/poly/mega IC ladder, closure alloc |
| `data` | 20 | JSON.parse/stringify, sort, array chains, Object.assign, Map lookups |
| `strings` | 19 | template literals, string building, the RN color 9-regex ladder, charCodeAt, number→string |
| `surface` | ~30 | one-operation-per-row language/library gap sweep |
| `suspense` | 4 | throw-based control flow (Suspense, error boundary) + non-throwing control |
| `modern` | 15 | class/promise/async-await/iterator/Map/Set/module patterns |
| `arrayholes` | 5 | array holes fill/read-back |
| `octane` | 13 | stock Octane (time-budgeted `Score:` per row; zlib & codeload excluded) |
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
`calls.c`) is the JS→C call driver. `lib/harness.js` defines the `bench()` row
interface used by the workload files. `lib/hermes.mjs` provisions and caches the
Hermes VM and `hermesc`. `lib/device.mjs` cross-compiles and drives `--device`.
The octane corpus is assembled from vendor fixtures by `octane/build-octane.mjs`.

The kernel files and workload files are largely the upstream/RN-shaped sources
(ES5) so Hermes and QJS run byte-identical code.