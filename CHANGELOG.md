# Changelog

## Unreleased

Nothing has been published yet. This section becomes the first release entry.

### Added

- QuickJS as a React Native JavaScript engine, compiled from source in the app
  build on Android and iOS. The engine is [quickjs-ng][ng], pre-patched with the
  six changes described in `engine/patches/README.md`.
- `npx react-native-quickjs install`, plus `revert` and `doctor`, and an Expo
  config plugin for `npx expo prebuild`.
- Release bundles compile to QuickJS bytecode on both platforms.
- Chrome DevTools debugging: breakpoints, stepping, the call stack, variable
  inspection and expression evaluation.
- A Hermes compatibility shim, so libraries that link against Hermes -- most
  importantly `react-native-worklets`, and through it `react-native-reanimated`
  -- run unmodified. It is built only for an app that has left Hermes.
- `TextEncoder` and `TextDecoder`, as `@react-native-quickjs/text-encoding`,
  a dependency of this package so every app has them.

### Known limitations

- iOS compiles React Native core from source: removing Hermes removes the JSI
  implementation that ships inside `hermesvm.framework`.
- Conditional breakpoints stop unconditionally; the condition is stored and
  reported but never evaluated.
- React Native's non-Hermes path is patched at `pod install`. Each workaround
  fails with a named error if React Native changes.
- Expo's `getDefaultReactHost` hardcodes Hermes, so the config plugin edits
  `node_modules`. Until [expo#PENDING][expo-pr] lands, an `npm install` reverts
  that edit and the app silently launches on Hermes.

[ng]: https://github.com/quickjs-ng/quickjs
[expo-pr]: https://github.com/expo/expo/pulls
