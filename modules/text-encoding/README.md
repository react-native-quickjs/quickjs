# text-encoding

`TextEncoder` and `TextDecoder`, per [WHATWG Encoding][spec], implemented
against the engine.

Hermes provides both, so an app moving to QuickJS expects them to be there.
Without them a bundle either carries a JavaScript polyfill — the encoder is a
per-character loop over a growing array — or fails outright: Expo's development
client reads `TextEncoder` while loading, before any app code runs.

Only UTF-8 is supported, which is what the spec requires of `TextEncoder` and
what `TextDecoder` is asked for in practice. Any other label is a `RangeError`,
rather than a silent mis-decode.

## Shape

`TextEncoding.cpp` is the byte work — `encode`, `encodeInto` and `decode` — plus
a small JavaScript shim that wraps them in the classes the spec describes.

The shim exists because the class shape is the part JSI cannot express: a
`jsi::Function` is not a constructor, so `new TextEncoder()` against one would
silently write to the function object instead of a new instance, and
`class T extends TextDecoder {}` would give `undefined`. Writing the getters,
`Symbol.toStringTag` and the `new`-only guard in JavaScript is shorter than
building the same shapes through the engine's class API, and the cost is one
evaluation of a short string when the runtime is created.

It installs from the runtime factory rather than through the module registry,
because registry entries arrive from a static initializer and a static-library
link is entitled to drop an object file nothing references — which would leave
this compiled, linked and silently absent.

## Behaviour worth knowing

- An unpaired surrogate encodes as U+FFFD; it is not an error.
- A leading byte order mark is stripped unless `ignoreBOM` is set.
- `fatal: true` throws a `TypeError` on ill-formed input; otherwise ill-formed
  sequences decode to U+FFFD.
- `encodeInto` stops on a character boundary, so a destination too small for the
  next character consumes nothing rather than writing a partial sequence.

[spec]: https://encoding.spec.whatwg.org/
