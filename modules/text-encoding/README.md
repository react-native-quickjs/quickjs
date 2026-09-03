# @react-native-quickjs/text-encoding

[![npm](https://img.shields.io/npm/v/@react-native-quickjs/text-encoding?color=cb3837)](https://www.npmjs.com/package/@react-native-quickjs/text-encoding)

`TextEncoder` and `TextDecoder`, per [WHATWG Encoding][spec], as direct QuickJS bindings.

Hermes provides both, so an app moving to QuickJS expects them. Without them a
bundle either carries a JavaScript polyfill -- the encoder is a per-character
loop over a growing array -- or fails outright: Expo's development client reads
`TextEncoder` while loading, before any app code runs.

`@react-native-quickjs/quickjs` depends on this package, so an app gets it
without asking.

## Behaviour worth knowing

- An unpaired surrogate encodes as U+FFFD; it is not an error.
- A leading byte order mark is stripped unless `ignoreBOM` is set.
- `fatal: true` throws a `TypeError` on ill-formed input; otherwise ill-formed
  sequences decode to U+FFFD.
- `encodeInto` stops on a character boundary, so a destination too small for the
  next character consumes nothing rather than writing a partial sequence.

[spec]: https://encoding.spec.whatwg.org/
