/*
 * Intl.Locale, Intl.getCanonicalLocales, supportedLocalesOf.
 *
 * WHAT THIS MODELS
 *   Pure algorithm with no formatting behind it: BCP-47 parsing, structural
 *   validation, alias canonicalization, and ECMA-402 locale negotiation. All of
 *   it is JavaScript in this module by design (docs/intl-platform-backed.md
 *   Part 4), and all of it runs on EVERY formatter constructor.
 *
 *   That is why these rows matter out of proportion to how often an app calls
 *   `new Intl.Locale` directly: whatever `canonicalize + negotiate` costs here
 *   is a floor under every `ctor-*` row in every other workload. If the
 *   constructors are slow, this is the first place to look, and moving this
 *   layer to native is the candidate the brief names first.
 *
 *   `maximize`/`minimize` are the rows that cross to the platform (Apple's
 *   Locale.Language.maximalIdentifier via a Swift @_cdecl shim), so they are
 *   the per-call backend cost with almost no JS around it -- a useful probe of
 *   the seam itself.
 */

setup(function () {
  new Intl.Locale("de-DE");
});

// ---- parsing / construction ---------------------------------------------

bench("locale-ctor-simple", function () {
  return new Intl.Locale("de-DE").baseName.length;
});

bench("locale-ctor-full", function () {
  return new Intl.Locale("zh-Hant-TW-u-ca-chinese-nu-hanidec-co-pinyin").baseName
    .length;
});

bench("locale-ctor-with-options", function () {
  return new Intl.Locale("de", {
    region: "AT",
    calendar: "gregory",
    numberingSystem: "latn",
  }).baseName.length;
});

var locSimple = new Intl.Locale("de-DE");
var locFull = new Intl.Locale("zh-Hant-TW-u-ca-chinese-nu-hanidec-co-pinyin");

bench("locale-getter-baseName", function () {
  return locFull.baseName.length;
});

bench("locale-getter-language", function () {
  return locFull.language.length;
});

bench("locale-toString", function () {
  return locFull.toString().length;
});

bench("locale-maximize", function () {
  return locSimple.maximize().toString().length;
}, { sinkMayDiffer: true });

bench("locale-minimize", function () {
  return locFull.minimize().toString().length;
}, { sinkMayDiffer: true });

// ---- canonicalization ---------------------------------------------------

bench("getCanonicalLocales-1", function () {
  return Intl.getCanonicalLocales("EN-us")[0].length;
});

bench("getCanonicalLocales-4", function () {
  // "zh-cmn-Hans-CN" is deliberately NOT here: extlang subtags are invalid
  // under modern UTS-35 and BOTH node and this module throw a RangeError for
  // them. An earlier draft of this row used it and the runner caught it as a
  // non-zero exit rather than reporting a plausible number.
  return Intl.getCanonicalLocales(["EN-us", "fr-FR", "iw", "cmn-Hans-CN"])
    .length;
});

bench("getCanonicalLocales-extensions", function () {
  return Intl.getCanonicalLocales("de-DE-u-nu-latn-ca-gregory-kn-true")[0]
    .length;
});

// ---- negotiation --------------------------------------------------------

bench("supportedLocalesOf-hit", function () {
  return Intl.NumberFormat.supportedLocalesOf(["de-DE"]).length;
});

bench("supportedLocalesOf-miss", function () {
  return Intl.NumberFormat.supportedLocalesOf(["xx-YY"]).length;
});

bench("supportedLocalesOf-8", function () {
  return Intl.NumberFormat.supportedLocalesOf([
    "de-DE", "fr-FR", "es-ES", "it-IT", "ja-JP", "ko-KR", "zh-CN", "pt-BR",
  ]).length;
});
