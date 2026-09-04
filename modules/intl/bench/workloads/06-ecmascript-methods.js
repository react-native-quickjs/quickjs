/*
 * The ECMAScript-side methods that ECMA-402 redefines to route through Intl:
 * Number/BigInt/Array/Date `toLocaleString`, `String.prototype.localeCompare`,
 * `toLocaleUpperCase` / `toLocaleLowerCase`.
 *
 * WHAT THIS MODELS
 *   These are the calls an app makes WITHOUT ever naming `Intl`, and they are
 *   the highest-traffic Intl surface in practice: `price.toLocaleString()` in a
 *   list row, `a.localeCompare(b)` as a sort comparator.
 *
 *   Every one of them is specified as "construct a fresh formatter, use it,
 *   throw it away" (ECMA-402 `Number.prototype.toLocaleString` step 2 is
 *   literally `? Construct(%NumberFormat%, ...)`). A conforming engine may
 *   memoize because the construction is unobservable for a plain options
 *   argument -- V8 does exactly that, with a per-(kind, locale, options) cache.
 *   Whether we do is what these rows answer, and the gap between
 *   `toLocaleString-no-args` and `fmt-integer` in 01-numberformat.js is the
 *   size of the prize.
 *
 *   `-varying-locale` exists to keep a cache honest: a single-key cache would
 *   turn the fixed-locale rows green and leave this one exactly where it was,
 *   which is a finding rather than a failure. Do not delete it to make a table
 *   look better.
 */

setup(function () {
  (1).toLocaleString("de-DE");
});

var LOCS = ["de-DE", "en-US", "fr-FR", "ja-JP"];
var OPTS = { style: "currency", currency: "EUR" };

// ---- Number.prototype.toLocaleString ------------------------------------

bench("toLocaleString-no-args", function (i) {
  return (i + 0.5).toLocaleString().length;
}, { sinkMayDiffer: true });

bench("toLocaleString-locale", function (i) {
  return (i + 0.5).toLocaleString("de-DE").length;
});

bench("toLocaleString-locale+opts", function (i) {
  return (i + 0.5).toLocaleString("de-DE", OPTS).length;
});

bench("toLocaleString-fresh-opts-literal", function (i) {
  return (i + 0.5).toLocaleString("de-DE", {
    style: "currency",
    currency: "EUR",
  }).length;
});

bench("toLocaleString-varying-locale", function (i) {
  return (i + 0.5).toLocaleString(LOCS[i & 3]).length;
});

bench("toLocaleString-bigint", function (i) {
  return BigInt(i).toLocaleString("de-DE").length;
});

// ---- Date.prototype.toLocale*String -------------------------------------

var T0 = 1600000000000;

bench("date-toLocaleString", function (i) {
  return new Date(T0 + i * 3600000).toLocaleString("de-DE").length;
}, { sinkMayDiffer: true });

bench("date-toLocaleDateString", function (i) {
  return new Date(T0 + i * 86400000).toLocaleDateString("de-DE").length;
}, { sinkMayDiffer: true });

bench("date-toLocaleTimeString", function (i) {
  return new Date(T0 + i * 3600000).toLocaleTimeString("de-DE").length;
}, { sinkMayDiffer: true });

// ---- Array.prototype.toLocaleString --------------------------------------

var ARR = [1000.5, 2000.25, 3000.125];

bench("array-toLocaleString", function () {
  return ARR.toLocaleString("de-DE").length;
}, { sinkMayDiffer: true });

// ---- String.prototype.localeCompare --------------------------------------

var WORDS = ["apfel", "Äpfel", "zebra", "Straße", "ärger", "unter"];

bench("localeCompare-no-args", function (i) {
  return WORDS[i % 6].localeCompare(WORDS[(i + 1) % 6]) + 2;
}, { sinkMayDiffer: true });

bench("localeCompare-locale", function (i) {
  return WORDS[i % 6].localeCompare(WORDS[(i + 1) % 6], "de") + 2;
}, { sinkMayDiffer: true });

/*
 * The realistic shape: localeCompare as a sort comparator. n log n crossings
 * per sort, and the per-call construction cost (if any) multiplied by the same
 * factor.
 */
var SORT_SRC = [];
for (var si = 0; si < 200; si++) {
  SORT_SRC.push("wort" + ((si * 7919) % 1000) + (si % 3 === 0 ? "ä" : ""));
}
bench("localeCompare-sort-200", function () {
  return SORT_SRC.slice().sort(function (a, b) {
    return a.localeCompare(b, "de");
  }).length;
}, { minMs: 100 });

// ---- case mapping --------------------------------------------------------

bench("toLocaleUpperCase", function (i) {
  return WORDS[i % 6].toLocaleUpperCase("de").length;
}, { sinkMayDiffer: true });

bench("toLocaleLowerCase-tr", function (i) {
  return "ISTANBUL".toLocaleLowerCase("tr").length + (i & 0);
}, { sinkMayDiffer: true });
