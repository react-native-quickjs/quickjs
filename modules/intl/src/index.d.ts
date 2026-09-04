/**
 * ECMA-402 `Intl`, backed by the operating system's CLDR database.
 *
 * The global is installed by the QuickJS module; TypeScript's own `lib.es5` /
 * `lib.esnext.intl` declarations already describe it, so this file exists to
 * give the package an entry point to import for its side effect rather than to
 * re-declare the API.
 *
 * Stage one implements `Intl.DateTimeFormat`, `Intl.getCanonicalLocales` and
 * `Intl.supportedValuesOf`. See the module README for the enumerated list of
 * deviations from the specification.
 */
declare const Intl: typeof globalThis.Intl;
export = Intl;
