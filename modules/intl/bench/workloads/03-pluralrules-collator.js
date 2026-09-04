/*
 * Intl.PluralRules and Intl.Collator.
 *
 * WHAT THIS MODELS
 *   PluralRules is the one place this module ships data -- 29,984 bytes of
 *   CLDR selectors for 224 locales -- because Foundation has no plural API in
 *   Objective-C or Swift. `select()` therefore runs a JS rule body on every
 *   call, on every platform, with no backend to hide behind. If rule
 *   evaluation is slow it is slow everywhere, and the fix would be compiling
 *   the same rules to a native evaluator (which ships no different data, so it
 *   does not violate the principle).
 *
 *   The rows deliberately cover both a trivial rule (`en`: n == 1) and a hard
 *   one (`ar`: six categories keyed on n % 100, `pl`: three keyed on
 *   n % 10 and n % 100, `cy`: six exact values). A single-locale plural
 *   benchmark would measure the cheapest rule in CLDR and call it the cost.
 *
 *   Collator.compare is included because it is Hermes's strongest area and the
 *   one operation where a backend call per comparison is potentially inside a
 *   sort's inner loop -- an n log n multiplier on our per-call overhead.
 */

setup(function () {
  new Intl.PluralRules("en").select(1);
  new Intl.Collator("de").compare("a", "b");
});

// ---- PluralRules --------------------------------------------------------

bench("plural-ctor-en", function () {
  return new Intl.PluralRules("en").select(1).length;
});

bench("plural-ctor-ordinal", function () {
  return new Intl.PluralRules("en", { type: "ordinal" }).select(2).length;
});

var prEn = new Intl.PluralRules("en");
var prAr = new Intl.PluralRules("ar");
var prPl = new Intl.PluralRules("pl");
var prCy = new Intl.PluralRules("cy");
var prOrd = new Intl.PluralRules("en", { type: "ordinal" });
var prRu = new Intl.PluralRules("ru");

bench("plural-select-en", function (i) {
  return prEn.select(i).length;
});

bench("plural-select-ar", function (i) {
  return prAr.select(i).length;
});

bench("plural-select-pl", function (i) {
  return prPl.select(i).length;
});

bench("plural-select-cy", function (i) {
  return prCy.select(i).length;
});

bench("plural-select-ru", function (i) {
  return prRu.select(i).length;
});

bench("plural-select-ordinal-en", function (i) {
  return prOrd.select(i).length;
});

bench("plural-select-fractional", function (i) {
  return prEn.select(i + 0.5).length;
});

bench("plural-resolvedOptions", function () {
  return prEn.resolvedOptions().pluralCategories.length;
});

// ---- Collator -----------------------------------------------------------

bench("collator-ctor", function () {
  return new Intl.Collator("de").compare("a", "b") + 2;
});

bench("collator-ctor-opts", function () {
  return (
    new Intl.Collator("de", {
      sensitivity: "base",
      numeric: true,
    }).compare("a", "b") + 2
  );
});

var coDe = new Intl.Collator("de");
var coNum = new Intl.Collator("de", { numeric: true });
var WORDS = [
  "apfel", "Äpfel", "zebra", "Straße", "strasse", "ärger", "arger",
  "item10", "item9", "item2", "Ünter", "unter",
];

bench("collator-compare-ascii", function (i) {
  return coDe.compare(WORDS[i % 6], WORDS[(i + 1) % 6]) + 2;
}, { sinkMayDiffer: true });

bench("collator-compare-accented", function (i) {
  return coDe.compare(WORDS[i % WORDS.length], WORDS[(i + 3) % WORDS.length]) + 2;
}, { sinkMayDiffer: true });

bench("collator-compare-numeric", function (i) {
  return coNum.compare(WORDS[6 + (i % 3)], WORDS[6 + ((i + 1) % 3)]) + 2;
}, { sinkMayDiffer: true });

/*
 * A sort is the realistic Collator workload: n log n comparisons through the
 * same bound `compare`. Each iteration re-copies the array so the sort is not
 * measuring an already-sorted array on the second pass.
 */
var SORT_SRC = [];
for (var si = 0; si < 200; si++) {
  SORT_SRC.push("item" + ((si * 7919) % 1000) + (si % 3 === 0 ? "ä" : ""));
}
bench("collator-sort-200", function () {
  return SORT_SRC.slice().sort(coDe.compare).length;
}, { minMs: 100 });

bench("collator-resolvedOptions", function () {
  return coDe.resolvedOptions().sensitivity.length;
});
