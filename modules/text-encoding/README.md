# @react-native-quickjs/text-encoding

`TextEncoder` and `TextDecoder`, per [WHATWG Encoding][spec], as direct QuickJS
bindings.

Hermes provides both, so an app moving to QuickJS expects them. Without them a
bundle either carries a JavaScript polyfill -- the encoder is a per-character
loop over a growing array -- or fails outright: Expo's development client reads
`TextEncoder` while loading, before any app code runs.

`@react-native-quickjs/quickjs` depends on this package, so an app gets it
without asking.

## Direct bindings, not JSI

Everything past the install function is the engine's own API: `JS_NewClass` and
`JS_NewCFunction2` with `JS_CFUNC_constructor` for the classes,
`JS_GetTypedArrayBuffer` to read a view, `JS_NewUint8ArrayCopy` to return one.

JSI appears once, in `install(jsi::Runtime &)`, because the runtime is how the
installer reaches the engine. That is the whole of it.

Real engine classes are what make `new TextEncoder()` construct, `instanceof`
answer correctly and `class Mine extends TextDecoder {}` work. A JSI host
function is not a constructor: `new` against one leaves `this` pointing at the
constructor itself, so an earlier version of this module needed a JavaScript
shim to define the class shapes. Direct bindings need no shim.

Only UTF-8 is supported, which is what the spec requires of `TextEncoder` and
what `TextDecoder` is asked for in practice. Any other label is a `RangeError`
rather than a silent mis-decode.

## Behaviour worth knowing

- An unpaired surrogate encodes as U+FFFD; it is not an error.
- A leading byte order mark is stripped unless `ignoreBOM` is set.
- `fatal: true` throws a `TypeError` on ill-formed input; otherwise ill-formed
  sequences decode to U+FFFD.
- `encodeInto` stops on a character boundary, so a destination too small for the
  next character consumes nothing rather than writing a partial sequence.

## Installation

Two routes, both deliberate. `QJS_REGISTER_MODULE` puts a static initializer in
this translation unit, which works for a shared library. `textEncoding_install`
is the symbol a generated module registry calls by name, and that direct
reference is what survives a static-library link -- where the linker is entitled
to drop an object file nothing references, leaving the module compiled, linked
and silently installing nothing. Registration is deduplicated, so a module
reached by both routes installs once.

[spec]: https://encoding.spec.whatwg.org/
