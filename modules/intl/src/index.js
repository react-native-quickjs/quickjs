/*
 * react-native-quickjs-intl
 *
 * The QuickJS module installs a lazy `Intl` accessor into the runtime before
 * any application JavaScript runs, so by the time anything imports this file
 * `globalThis.Intl` already resolves. Importing it is about giving bundlers
 * something to resolve, and about failing with a useful message when the native
 * side is missing.
 *
 * Note that reading `globalThis.Intl` here is what *materializes* the
 * implementation — the check below is not free, and that is the point of the
 * design: an app that never imports this module and never touches `Intl` pays
 * one accessor property on the global object and nothing else.
 */

'use strict';

if (typeof globalThis.Intl === 'undefined') {
  throw new Error(
    'react-native-quickjs-intl: QuickJS module not installed.\n' +
      'Check that react-native-quickjs is the configured JS engine and that ' +
      'the app was rebuilt after adding this package (pod install / gradle sync).'
  );
}

module.exports = globalThis.Intl;
