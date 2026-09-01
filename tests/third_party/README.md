# Vendored JSI conformance suite

`jsi/test/testlib.{h,cpp}` is copied from Hermes:

    API/jsi/jsi/test/testlib.{h,cpp}
    facebook/hermes @ 10a40b5f1 (branch static_h)

## Why it is copied rather than referenced

`testlib.h` is included as `<jsi/test/testlib.h>`, so referencing it in place
means putting Hermes' `API/jsi` on the include path — which also puts Hermes'
`jsi/jsi.h` there. Hermes' `main`/`static_h` JSI has diverged from the JSI that
ships in React Native (it splits `Runtime` into `IRuntime : ICast` and adds
`TypedArray`), so `testlib.cpp` would then compile against a different, ABI
incompatible `jsi::Runtime` than the one `QuickJSRuntime` implements. That
shows up as undefined `facebook::jsi::…(facebook::jsi::IRuntime&)` symbols at
link time.

Copying only the two test files keeps `<jsi/jsi.h>` resolving to
`node_modules/react-native/ReactCommon/jsi`, which is the JSI our runtime must
actually satisfy.

## Updating

Re-copy from a Hermes checkout when React Native bumps its Hermes version
(`node_modules/react-native/sdks/.hermesversion`), and re-check that the file
still compiles against React Native's JSI.
