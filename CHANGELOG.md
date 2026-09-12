# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog][keep-a-changelog],
and this project adheres to [Semantic Versioning][semver].

## [Unreleased]

### Changed
- Patch `0034`: dispatches interpreter-issued calls directly to supported
  native functions while retaining the existing fallback for other callables.
- Patch `0035`: copies eligible dense array spreads directly into the
  accumulator while preserving iterator semantics for unsupported arrays.
- Patch `0036`: forwards unchanged dense mapped arguments directly from their
  live parameter slots during apply, with the existing generic fallback.
- Patch `0037`: avoids copying complete argument arrays for callees proven not
  to write, capture, expose, or alias their argument slots.
- Patch `0038`: avoids reifying frame-visible arguments for safe length, indexed
  access, and builtin apply patterns while preserving observable slow paths.
- Patch `0039`: keeps a pending exception when a formal parameter fails to
  parse, instead of replacing a stack overflow, interrupt or allocation failure
  with a syntax error.
- Patch `0040`: assigns the C-function frame argument buffer, which was
  previously left uninitialized.
- Patch `0041`: appends `Array.prototype.map` and `filter` results directly to
  the dense result array instead of walking the generic define path per
  element. On-device, `map` identity is 25.2% faster, `filter` 19.9% faster and
  mapping to objects 11.7% faster. The sampled Octane rows are neutral overall
  (geomean -0.49%), the largest moves being deltablue -1.76% and richards
  -1.29%, which measure as code layout rather than the added check.
- Patch `0042`: gives fast arrays a hole representation instead of demoting the
  object to a property table when an index is skipped or deleted. On-device the
  crypto row of Octane is 23.4% faster and pdfjs 20.4% faster, the two rows
  whose element accesses previously landed on a demoted array; the other
  sampled rows are flat (8-row geomean +5.3%). Filling from index 2 is 52.8%
  faster and deleting every other element 17.7%; a reverse fill is unchanged.
- Patch `0043`: steps array iterators from the dense storage instead of
  re-reading `length` and the element with two property gets, and stores the
  iterator record inline instead of in its own allocation. On-device `for...of`
  over an array is 33.3% faster, array destructuring 11.1%, `.values()` 8.1%,
  `.entries()` 6.9% and `.keys()` 4.9%, with an indexed loop unchanged; each
  live iterator also costs one fewer malloc and 32 bytes. The sampled Octane
  rows are flat (10-row geomean -0.27%), because those rows iterate with
  indexed loops rather than `for...of`.
- Patch `0044`: fills `Array.prototype.slice` and `splice` results directly into
  the dense result array instead of defining every index through the generic
  path. On-device, slicing a 200-element packed array is 74% faster, a
  50-element window of it 70%, and a 100,000-element array 70%, with an
  array-like source and an empty result unchanged. The sampled Octane rows are
  flat (8-row geomean -0.04%).
- Patch `0045`: copies `Array.prototype.concat` inputs that are already dense
  fast arrays straight into the result instead of reading and defining every
  element. On-device, concatenating two 200-element arrays is 85% faster, four
  100-element arrays 80%, a scalar 81% and two 20,000-element arrays 74%, with
  an input pair that keeps the generic path unchanged. The sampled Octane rows
  are flat (8-row geomean +0.21%).
- Patch `0046`: reserves capacity for the `Array.prototype.filter` result from
  the input length, capped at 4096 slots, before the callback loop. On-device,
  filtering a 200-element array is 8.3% faster and a 100,000-element array
  7.6%; a filter that keeps nothing pays about 1%, and a live result can hold
  up to 32 KB of reserved storage that its `length` does not expose. The
  sampled Octane rows are flat (8-row geomean -0.32%).
- Patch `0047`: `OP_define_field` overwrites an object literal's existing slot
  inline instead of re-defining it through the generic path. On-device, a
  1-field literal is 12.1% faster, 3 fields 24.4%, 9 fields 44.7% and a literal
  whose values are three call results 16.3%, with a constructor using `this.x =`
  unchanged; a literal site wider than the 16-property template limit is 2.4%
  slower, because those defines are inserts and the one extra lookup is not
  amortised. On the sampled Octane rows the mechanism alone moves the 8-row
  geomean +0.90% (splay +4.7%, navierstokes +2.2%) and the shipped binary
  +1.4% to +1.6% across two runs.
- Patch `0048`: initializes a templated object literal's slots where they live,
  instead of staging a 16-slot array on the stack for the allocator to copy in.
  Measured same-binary (both paths in one build, so no layout component), a
  3-field literal is 3.9% faster, 9 fields 2.7% and 1 field 1.9%, and a 20-field
  literal -- past the template limit, so it never stages -- is unchanged. The
  sampled Octane rows are flat (8-row geomean +0.01%); one cross-binary run read
  -0.72% and did not reproduce.
- Patch `0049`: `js_closure()` builds a function object from a per-realm shape
  template in one allocation, writing `length`, `name` and `prototype` straight
  into the property array instead of adding them one shape transition at a time.
  Measured same-binary, creating a closure is 40% to 50% cheaper -- a named or
  anonymous function expression 50.4%, an arrow 44.6%, a generator 43.4%, a
  capturing closure 40.7% -- with `new` and plain calls unchanged. The sampled
  Octane rows move +0.30% geomean same-binary and +0.60% shipped (pdfjs +2.7%).
- Patch `0050`: an object whose shape needs at most four property slots -- an
  object literal, a closure, a for-in iterator -- keeps its property array
  inside its own allocation, so birth and death lose a malloc, a free and a
  second block header. Measured same-binary, an empty object is 9.8% cheaper to
  create, a one-property object 11.3%, a two-property object 11.3%, a capturing
  closure 6.1%, an arrow function 11.8% and a for-in loop 2.3%, with a
  five-property object and both array shapes (ineligible) unchanged. On a
  260,000-object workload total malloc falls 3.4% and the allocation count 46%
  (521,128 -> 281,102). The inline area is sized to the shape: a fixed four-slot
  area measured +11.8% total heap for the same speed. The sampled Octane rows
  move +1.54% geomean same-binary (splay +10.7%) and +0.24% shipped (splay
  +9.7%).
- Patch `0051`: a shape keeps a strong reference to the shape it was cloned
  from, so a construction path that was already walked once is resolved from the
  transition table instead of being cloned again. Shape clones per Octane run:
  typescript 7,017,436 -> 29,514 (238x), with the other seven rows unchanged.
  Score, same-binary over six runs: typescript +4.55% (7,971 -> 8,334, the arms
  not overlapping) and 8-row geomean +0.40%; shipped, typescript +3.70% and
  geomean +1.21%. Unlike the patches above it, this one costs memory where it
  gains: retained predecessors stay allocated, so typescript's shape bytes go
  295,024 -> 2,005,472 (+1.7 MB, +580%) and its total malloc 4.93 MB -> 6.66 MB
  (+35%). A workload that creates and drops shapes measures +0.2%, and the other
  seven rows clone nothing extra, so the cost is confined to the clone-heavy
  workloads that also gain.
- Patch `0052`: an eligible object literal's field stores are fused at compile
  time into one `OP_object_fill`, deleting N-1 `OP_define_field` dispatches from
  the emitted bytecode and keeping the values on the operand stack until the
  fill. Measured same-binary, a 3-property literal is 8.0% faster, a 9-property
  literal 17.4% and a literal whose values are variables 7.0%, while unfused
  controls (a 20-property literal past the template cap, a constructor, an array
  literal) are unchanged; a literal whose values are call results is 2.8%
  slower, because those values stay live on the stack. The three shipped builtins
  shrink 952 -> 921, 2755 -> 2720 and 2704 -> 2669 bytes. The sampled Octane rows
  are neutral (8-row geomean -0.25%) because they barely use literals, so what
  this patch buys is smaller precompiled bundles and fewer ops at startup --
  `9999` still raises 27 -> 28 and now carries these builtins regenerated by this
  compiler.
- Host-function calls now build only the arguments actually passed, instead of
  materialising all eight inline slots, and the QuickJS value conversion is
  inlined into the argument loop. Measured against the same branch without this
  change, on-device release builds are 22–44% cheaper across arities 0, 1, 4 and
  8 and for string- and object-backed arguments, with direct native and
  JavaScript call controls unchanged.

### Added
- A JSI host-call benchmark in the example app, and a class-call row (the shape
  a host function presents to the engine) in the `calls` benchmark suite.

## [v1.0.0-alpha.3] — 2026-09-10

### Added
- Update the embedded [quickjs-ng][ng] engine to v0.16.2.

### Changed
- Patch `0008`: faster eager `JSON.parse` through direct ASCII key-to-atom
  parsing, optimized numeric arrays, integer-aware number parsing, and owned
  dense-array construction.
- Patch `0009`: faster `JSON.stringify` through direct atom enumeration,
  efficient key/string quoting, and direct primitive formatting.
- Patch `0010`: faster JSON tokenization with a dedicated common-punctuation
  fast path for brackets, braces, commas and colons.
- Patch `0011`: opt-in lazy `JSON.parse` for documents at least 4 KiB, using a
  shared source document, structural tape, direct tape-node markers and
  repeated-layout materialization. Application builds remain eager by default;
  opt in from the existing `package.json` with
  `react-native-quickjs.lazyJson: true`. Reviver calls remain eager, and
  embedders can opt out per runtime with `JS_SetJSONLazyEnabled()`.
- Patch `0012`: caches shapes for eligible repeated static object literals.
  Representative object-literal workloads are approximately 12–52% faster.
- Patch `0013`: accelerates forward `String.prototype.indexOf` and `includes`
  searches with `memchr` and same-width `memcmp`; representative mixed
  short/long searches are approximately 8–9% faster.
- Patch `0014`: reclaims an opcode slot with a compact escape prefix for
  debugger traps, preserving malformed-bytecode rejection.
- Patch `0015`: adds foundational monomorphic inline caches for property reads,
  writable overwrites, and transition stores, with lazy tables and a cheap
  megamorphic call-site guard. In local paired release measurements,
  constructor stores were 1.36× faster, React fiber allocation 1.92× faster,
  tree rendering 1.29× faster, and RN commit performance was neutral.
- Patch `0015` hardens serialized-bytecode and IC metadata validation, moves
  prototype-watchpoint invalidation after successful mutations, and preserves
  non-throwing lazy allocation and correct transition OOM unwinding.
- Patch `0016`: composes explicitly enabled four-entry polymorphic sites,
  guarded depth-two prototype reads, and a weak direct-mapped megamorphic
  fallback cache. Width 1 remains the shipping default; the width-4 default
  change is reserved for a later patch.
- Patch `0016` validation covers weak fallback shape reuse, prototype mutation
  invalidation, runtime destruction, width-4 ASan/OOM/differential paths, and
  malformed serialized bytecode. Release CTest passed 15/15; host timings are
  smoke results, with device performance based on the selected 1095 evidence.
- Patch `0016` paired host Octane smoke results improved the geomean by 7.3%
  over the IC-disabled baseline across three runs. The largest gains were
  DeltaBlue (+34.1%), Richards (+24.3%), Raytrace (+19.7%), and Box2D
  (+14.5%); Crypto, RegExp, Splay, Navier-Stokes, PDF.js, and Mandreel were
  within neutral-to-small-regression noise. The full reference data is in
  `tools/benchmark/octane/octane-results.json`.
- Patch `0017`: makes four-entry inline-cache sites the shipping default after
 the width-4 validation of the composed cache.
- Patch `0018`: stores bounded literal, prefix, and first-set metadata
  alongside compiled regexp bytecode.
- Patch `0019`: adds the runtime-width backtracking matcher with computed-goto
  dispatch and register-based execution.
- Patch `0020`: analyzes patterns for literal, prefix, and first-set candidate
  searches before entering the full matcher.
- Patch `0021`: connects compiled search metadata to RegExp compilation and
  execution.
- Patch `0022`: adds direct candidate scans and stack storage for small capture
  sets, reducing the work needed for common matches.
- Patch `0023`: keeps `RegExp.prototype.test` result-free and streamlines
  successful match handling.
- Patch `0024`: accelerates split scanning and improves first-set handling for
  whitespace-led patterns.
- Patch `0025`: interns RegExp flag names as atoms for consistent reuse during
  compilation.
- Patch `0026`: serves eligible `.flags` reads from validated compiled flags
  while preserving the generic observable path.
- Patch `0027`: keeps the regexp fixture and bounds-accounting coverage aligned
  with the matcher.
- Patch `0028`: hardens serialized RegExp validation with safe opcode, operand,
  target, metadata, allocator, and interrupt checks.
- Patch `0029`: adds table-driven malformed-bytecode coverage and preserves
  runtime serialization round-trip coverage through the embedding allocator.
- Patch `0030`: replaces the serialized-bytecode checksum with portable
  CRC-32C, using arm64 and x86-64 hardware instructions when available while
  retaining a software fallback and corruption detection. This reduces the
  checksum portion of bytecode loading without changing the application-facing
  bytecode API. Hardware availability is probed per call, avoiding shared
  runtime state, and bytecode tests cover the standard CRC-32C vector plus
  empty, short, unaligned-length, and larger inputs.
- Patch `0031`: frames serialized function bodies and adds copied lazy
  function-body loading. The reader validates and owns the copied payload;
  body parsing and reconstruction move to first use.
- Patch `0032`: adds explicit borrowed lazy-bytecode input for embedders that
  can keep their immutable payload alive for the required lifetime. It is not
  used by the React Native runtime.
- Patch `0033`: defers interning non-constant bytecode atoms until they are
  first referenced, while preserving eager validation of their serialized
  lengths and boundaries.
- The React Native runtime integration enables copied lazy loading; it is an
  integration change layered on top of patches `0031`–`0033`, not part of
  patch `0031` itself.
- In the isolated atom comparison, deferred interning saved 0.330 ms in the
  host load-only median. The Android `bundleLoaded` median was 0.736 ms slower
  with deferred atoms; the observed `appLoaded` difference is not conclusive
  without variance measurements, so no device loading improvement is claimed.
- JSON benchmark reporting now separates checksum-free parse-only timing from
  traversal and parse-plus-consume timing. The lazy path reduces the
  repeated-object parse-only workload from about 199 µs eager to about 43 µs.

## [v1.0.0-alpha.2] — 2026-09-05

### Changed
- Updated the example app and tooling to React Native 0.85.3.
- Bumped the `@react-native-quickjs/text-encoding` dependency to v1.0.1.

## [v1.0.0-alpha.1] — 2026-09-05

### Added
- Native modules now fold into the engine's single `.so` on Android and iOS:
  - `Intl` and `TextEncoding` native code compile into `libquickjsinstancejni.so`.
  - A generated module registry is compiled into the app target on iOS, so
    autolinked modules register without a separate registration step.
- `@react-native-quickjs/intl` and `@react-native-quickjs/text-encoding` now
  install as a dependency and ship their bytecode embedded in the engine binary.
- New `scripts/release-module.js` and per-module CI workflows for publishing
  the module workspace.
- The engine now stringifies a bytecode function whose source was stripped as
  `[stripped source]` instead of `[native code]` (new engine patch `0007`).
- Release bundles compile to QuickJS bytecode on both platforms.

### Changed
- Prefer the root project's NDK version on Android.
- `Intl`: memoized `supportedLocalesOf`, skip `String()` conversion, and an LRU
  cache for `NumberFormatter`.
- `text-encoding` module bumped to v1.0.1 (see below).

## [intl-v1.0.0] — 2026-09-05

### Added
- `@react-native-quickjs/intl` 1.0.0:
  - ECMA-402 `Intl` backed by the operating system's CLDR data (like Hermes),
    with no locale data in your bundle.
  - Lazy, bytecode-embedded implementation; materializes on first read.
  - Covers `Intl.DateTimeFormat`, `NumberFormat`, `Collator`, `PluralRules`,
    `RelativeTimeFormat`, `ListFormat`, `DisplayNames`, `Segmenter`, `Locale`,
    `DurationFormat`, `getCanonicalLocales`, `supportedValuesOf`, and the
    ECMAScript-side `toLocaleString` family.
  - Platform backends for Android (`android.icu`) and Apple (`NSLocale`/friends),
    plus a host stub for tests and benchmarks.

## [text-encoding-v1.0.1] — 2026-09-05

### Changed
- `@react-native-quickjs/text-encoding` 1.0.1:
  - Folded the Android module into the engine's single `.so`, so the package
    ships no native code of its own on Android.
  - Added `<jsi/jsi.h>` linkage to the CMake config so it builds against
    `ReactAndroid::jsi` (Android) or `jsi` (host).
  - Fixed `JNI_OnLoad` handling when folded (one `JNI_OnLoad` per `.so`).

## [v1.0.0-alpha.0] — 2026-09-03

### Added
- QuickJS as a React Native JavaScript engine, compiled from source in the app
  build on Android and iOS. The engine is [quickjs-ng][ng], pre-patched with the
  changes described in `engine/patches/README.md`.
- `npx react-native-quickjs install`, plus `revert` and `doctor`, and an Expo
  config plugin for `npx expo prebuild`.
- Release bundles compile to QuickJS bytecode on both platforms.
- Chrome DevTools debugging: breakpoints, stepping, the call stack, variable
  inspection and expression evaluation.
- A Hermes compatibility shim, so libraries that link against Hermes — most
  importantly `react-native-worklets`, and through it `react-native-reanimated`
  — run unmodified. It is built only for an app that has left Hermes.
- `TextEncoder` and `TextDecoder`, as `@react-native-quickjs/text-encoding`,
  a dependency of this package so every app has them.
- A JSI implementation covering host objects, functions, arrays, arraybuffers,
  strings, symbols, bigints, weak references, native state, microtasks,
  deferred GC scheduling and safepoints, exceptions, and bytecode compilation
  (`qjsc`).

## [text-encoding-v1.0.0] — 2026-09-03

### Added
- `@react-native-quickjs/text-encoding` 1.0.0:
  - `TextEncoder` and `TextDecoder` per the WHATWG Encoding spec, as direct
    QuickJS bindings — the package `@react-native-quickjs/quickjs` depends on.
  - Behaviour: unpaired surrogates encode as U+FFFD; a leading BOM is stripped
    unless `ignoreBOM` is set; `fatal: true` throws on ill-formed input;
    `encodeInto` stops on character boundaries.

[keep-a-changelog]: https://keepachangelog.com/en/1.1.0/
[semver]: https://semver.org/
[ng]: https://github.com/quickjs-ng/quickjs
[expo-pr]: https://github.com/expo/expo/pull/49686
