# hermes-compat

A source-compatible `<hermes/hermes.h>` whose `makeHermesRuntime()` returns a
QuickJS-backed runtime, so React Native libraries that create their own Hermes
runtime build and run unmodified.

## Why

`react-native-worklets`, and so `react-native-reanimated`, does not use the
app's JavaScript runtime. It creates its own and picks the engine at compile
time from `__has_include(<hermes/hermes.h>)`, with no third branch. Expo's
`ExpoModulesJSI` calls `facebook::hermes::makeHermesRuntime` directly. Without
this shim neither can be used on QuickJS at all.

## What you get

`makeHermesRuntime()` returns a real `jsi::Runtime`. Every JSI method is
forwarded by `jsi::RuntimeDecorator`, so a change to JSI is a compile error
here rather than a silent divergence.

Everything Hermes-specific is either implemented against a genuine QuickJS
capability -- `watchTimeLimit` uses the engine's interrupt handler,
`getUniqueID` uses the object address -- or reports through
`hermes-compat/Diagnostics.h` before returning a value chosen to fail safely.
`getVMRuntimeUnsafe()` returns null rather than a `JSContext *` a caller would
cast to `hermes::vm::Runtime`; `makeThreadSafeHermesRuntime()` returns null
rather than an unlocked runtime.

Reports are one per API, to stderr or logcat. Install your own with
`qjs::hermescompat::setHandler`. Nothing aborts: strictness is yours to choose.

## Two rules when editing

**No member declaration in `include/hermes/**` may be conditional on a
preprocessor macro.** Free functions may be; virtual functions and data members
may not. Consumers are compiled separately from this shim with flags it does
not control, so a conditional virtual shifts every later vtable slot and the
failure is a silent call to the wrong function.

**A new `RuntimeConfig` field is not honoured until it is reported.**
`RuntimeConfig` and `GCConfig` carry Hermes's full field set and its defaults,
so code configuring the VM compiles. Nothing configures QuickJS, so
`makeHermesRuntime` compares what it is handed against those defaults and names
every field it cannot honour -- unsupported where the field changes what
JavaScript may do, such as `EnableEval`, ignored where nothing observable
depends on it. Add a field to `reportUnhonoured` in the same commit that adds
it to the list, or it will be dropped in silence.

## Not ABI compatibility

These headers must never be on the include path while a real `libhermes` is on
the link line.
