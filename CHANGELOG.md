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
