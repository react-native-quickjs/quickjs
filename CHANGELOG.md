# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog][keep-a-changelog],
and this project adheres to [Semantic Versioning][semver].

## [Unreleased]

### Added
- Update the embedded [quickjs-ng][ng] engine to v0.16.2.

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
