/*
 * react-native-quickjs-text-encoding
 *
 * The QuickJS module installs TextEncoder and TextDecoder into the runtime
 * before any application JavaScript runs, so by the time anything imports this
 * file the globals already exist. Importing it is about giving bundlers
 * something to resolve, and about failing with a useful message when the
 * native side is missing.
 */

'use strict';

if (typeof globalThis.TextEncoder === 'undefined') {
  throw new Error(
    'react-native-quickjs-text-encoding: QuickJS module not installed.\n' +
      'Check that react-native-quickjs is the configured JS engine and that ' +
      'the app was rebuilt after adding this package (pod install / gradle sync).'
  );
}

module.exports = {
  TextEncoder: globalThis.TextEncoder,
  TextDecoder: globalThis.TextDecoder,
};
