# @react-native-quickjs/intl

ECMA-402 `Intl` for react-native-quickjs, backed by the operating system's own
CLDR database. No locale data ships in your bundle and none ships in this
package.

**This is deliberately a separate, opt-in package and will stay one.** It is
not folded into `@react-native-quickjs/quickjs` and is not enabled by default:
an app that never touches `Intl` should not carry the platform seam
(`android.icu` on Android, `NSLocale` and friends on Apple) or this package's
JavaScript.

Ported from the research repository (`modules/intl`) into the shipping one,
following the module conventions of `modules/text-encoding`. The engine API
used is public quickjs-ng throughout; the module ABI and the bytecode toolchain
are this repository's.

## What it provides

`Intl.DateTimeFormat` · `Intl.NumberFormat` · `Intl.Collator` ·
`Intl.PluralRules` · `Intl.RelativeTimeFormat` · `Intl.ListFormat` ·
`Intl.DisplayNames` · `Intl.Segmenter` · `Intl.Locale` ·
`Intl.DurationFormat` · `Intl.getCanonicalLocales` · `Intl.supportedValuesOf`,
plus the ECMAScript-side methods that must route to the same backend:
`Number.prototype.toLocaleString`, `BigInt.prototype.toLocaleString`,
`Array.prototype.toLocaleString`, `%TypedArray%.prototype.toLocaleString`,
`String.prototype.localeCompare` and its `toLocaleUpperCase` /
`toLocaleLowerCase`, and `Date.prototype.toLocaleString` /
`toLocaleDateString` / `toLocaleTimeString`.

Installing it is meant to mean an app needs no other `Intl` polyfill at all.

## How it works

ECMA-402 splits cleanly into *algorithm* and *data*. The algorithm — BCP-47
parsing, option resolution and its getter order, locale negotiation,
`resolvedOptions`, the class shapes — is written once in JavaScript
(`js/intl.js`) and is byte-identical on every platform. The data is the CLDR
database both mobile operating systems already carry, reached through a narrow
platform seam (`cpp/IntlPlatform.h`).

- **Lazy.** `install()` defines one accessor on `globalThis`. Reading `Intl`
  materializes the implementation and redefines `Intl` as a data property;
  an app that never reads it pays one property access. The setter matters: a
  polyfill assigning over `globalThis.Intl` must not throw.
- **Bytecode.** `js/intl.js` is compiled to QuickJS bytecode at build time and
  embedded, so materialization costs no parse. The compiler is `qjsc`, from the
  same engine revision as the runtime; when none is available the module embeds
  the source instead and compiles on first use — correct, still lazy, slower.
- **Real classes.** The class shapes are ordinary JavaScript classes, so
  `new`, `instanceof`, `.prototype` and subclassing all work.
- **Per-platform backend, chosen at link time.** A working en-US/UTC stub
  (`cpp/IntlPlatform.cpp`) is what a host build links; the Apple backend
  (`ios/IntlPlatform.mm`, Foundation, plus one small Swift file for the six
  things Foundation's Objective-C surface cannot answer) and the Android
  backend (`android/`, hand-written JNI into `android.icu`) register
  themselves. `--print-backend` in the host CLI says which one answered.

### Platform notes

- **Android** uses `android.icu`, public since API 24 — below React Native's
  minSdk. Individual members are newer: `ListFormatter` is API 26,
  `RelativeDateTimeFormatter.formatNumeric` API 28, `android.icu.number.NumberFormatter`
  API 30. `scripts/android-api-levels.py` checks the SDK's own
  `api-versions.xml` and exits non-zero if anything required rises above
  minSdk. The Kotlin half needs a Java-initiated call to resolve its class —
  that is what the `IntlPackage` ReactPackage is for.
- **Apple** uses `NSLocale` / `NSDateFormatter` / `NSTimeZone`; nothing links
  ICU. The six `@_cdecl` functions in `ios/IntlLikelySubtags.swift` are
  declared `__attribute__((weak))` on the Objective-C++ side, so a build where
  Swift does not link still links and degrades to "no opinion" on likely
  subtags.
- **The Android backend compiles; it has not run in this repository.** No
  device is attached here. `scripts/verify-android.sh` builds the JNI C++ for
  three ABIs, compiles the Kotlin against `android.jar`, and asserts every JNI
  signature the C++ resolves exists on the compiled class. Behavioural
  verification needs a physical device.

## Building and testing on a host

```sh
# The engine and module, plus the host CLI shells.
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DRNQJS_INTL_BUILD_CLI=ON
cmake --build build-rel --target intl_tests intl-cli intl-bench -j8

# Host tests: the module's own backend-agnostic self-checks (test/invariants.js)
# run through intl_tests, exactly as an app installs the module.
ctest --test-dir build-rel -R intl --output-on-failure

# Iterate on the JS layer, and run the invariants under either backend.
build-rel/modules/intl/intl-cli --print-backend modules/intl/test/invariants.js

# The Apple backend (macOS only; links ios/IntlPlatform.mm + the Swift object).
cmake --build build-rel --target intl-cli-apple -j8
build-rel/modules/intl/intl-cli-apple --print-backend modules/intl/test/invariants.js
```

The `intl_tests` ctest target runs on every host, including CI's Linux runners:
it links the default backend and runs `test/invariants.js` (subclassing,
formatter lifetimes, lazy-accessor behaviour, and that every fast path is
reached) plus `test/parts-coverage.js`'s `formatToParts` round-trip rate.

### Benchmarks

Two harnesses, both re-runnable:

```sh
# Never-touched install cost, in a C harness.
npm run bench                        # in modules/intl

# Per-service throughput against node (node is the honest bar for a JS layer
# over a platform backend). Three arms: warm / cold / peak RSS.
node bench/run.mjs --runs 5
```

The workloads in `bench/workloads/` are run under both node and
`intl-cli-apple`, with each sink diffed between the two so a formatting
difference is a failure rather than a silent number.

## Deviations

Enumerated deliberately rather than discovered. The implementation follows the
platform's CLDR version (as Hermes does); `formatToParts` returns one coarse
`literal` part where a backend cannot supply real boundaries rather than
guessing; rounding for `notation: "standard"` is computed in JavaScript so a
tie resolves identically on iOS and Android; Apple lacks a few
`RelativeTimeFormat` and unit forms that `android.icu` has, and Android lacks
compound `x-per-y` units that Apple derives. A deviation found later is a bug;
the list is kept current in `js/intl.js` and the platform files.

## Registering a module

Both routes exist, deduplicated by name:

- `QJS_REGISTER_MODULE("react-native-quickjs-intl", rnqjs::intl::install)`, a
  static initializer that works in a shared library, and
- `extern "C" void intl_install(facebook::jsi::Runtime&)`, the symbol a
  generated module registry calls by name and the one that survives a
  static-library link.

`package.json` declares `reactNativeQuickJSModule: { install: "intl_install",
header: "cpp/IntlModule.h" }` for autolinking.
