/*
 * ListFormat, RelativeTimeFormat, DisplayNames, Segmenter, DurationFormat.
 *
 * WHAT THIS MODELS
 *   The six services this module ships that Hermes does not, so there is no
 *   engine-to-engine comparison for them other than node. Every one of them is
 *   a thin JS shape over a single backend call, which makes them the cleanest
 *   available reading of what the seam itself costs: if `displayNames-of` is
 *   near node while `fmt-currency` is 20x off, the seam is not the problem.
 *
 *   Segmenter is the exception -- it returns a whole segmentation as one flat
 *   int[] from the backend and then materializes JS objects lazily per
 *   iteration step, so `segment-iterate` measures the JS wrapper and
 *   `segment-call` measures the crossing.
 */

setup(function () {
  new Intl.ListFormat("en").format(["a"]);
});

// ---- ListFormat ---------------------------------------------------------

bench("list-ctor", function () {
  return new Intl.ListFormat("de").format(["a", "b"]).length;
}, { sinkMayDiffer: true });

var lf = new Intl.ListFormat("de");
var lfDis = new Intl.ListFormat("de", { type: "disjunction" });
var L3 = ["Apfel", "Birne", "Kirsche"];

bench("list-format-3", function () {
  return lf.format(L3).length;
}, { sinkMayDiffer: true });

bench("list-format-disjunction", function () {
  return lfDis.format(L3).length;
}, { sinkMayDiffer: true });

bench("list-formatToParts-3", function () {
  return lf.formatToParts(L3).length;
}, { sinkMayDiffer: true });

// ---- RelativeTimeFormat -------------------------------------------------

bench("rtf-ctor", function () {
  return new Intl.RelativeTimeFormat("de").format(-1, "day").length;
}, { sinkMayDiffer: true });

var rtf = new Intl.RelativeTimeFormat("de");
var rtfNum = new Intl.RelativeTimeFormat("de", { numeric: "auto" });

bench("rtf-format", function (i) {
  return rtf.format(-(i % 30) - 1, "day").length;
}, { sinkMayDiffer: true });

bench("rtf-format-auto", function (i) {
  return rtfNum.format(-(i % 30) - 1, "day").length;
}, { sinkMayDiffer: true });

bench("rtf-formatToParts", function (i) {
  return rtf.formatToParts(-(i % 30) - 1, "day").length;
}, { sinkMayDiffer: true });

// ---- DisplayNames -------------------------------------------------------

bench("displaynames-ctor", function () {
  return new Intl.DisplayNames(["de"], { type: "region" }).of("US").length;
}, { sinkMayDiffer: true });

var dnRegion = new Intl.DisplayNames(["de"], { type: "region" });
var dnLang = new Intl.DisplayNames(["de"], { type: "language" });
var dnCur = new Intl.DisplayNames(["de"], { type: "currency" });
var REGIONS = ["US", "FR", "DE", "JP", "BR", "IN", "GB", "CN"];

bench("displaynames-of-region", function (i) {
  return dnRegion.of(REGIONS[i % REGIONS.length]).length;
}, { sinkMayDiffer: true });

bench("displaynames-of-language", function (i) {
  return dnLang.of(["en", "fr", "ja", "ar"][i % 4]).length;
}, { sinkMayDiffer: true });

bench("displaynames-of-currency", function (i) {
  return dnCur.of(["USD", "EUR", "JPY", "GBP"][i % 4]).length;
}, { sinkMayDiffer: true });

// ---- Segmenter ----------------------------------------------------------

bench("segmenter-ctor", function () {
  return new Intl.Segmenter("de", { granularity: "word" }) ? 1 : 0;
});

var sgWord = new Intl.Segmenter("de", { granularity: "word" });
var sgGrapheme = new Intl.Segmenter("de", { granularity: "grapheme" });
var TEXT = "Der schnelle braune Fuchs springt über den faulen Hund.";

bench("segmenter-segment-call", function () {
  return sgWord.segment(TEXT) ? 1 : 0;
});

bench("segmenter-iterate-word", function () {
  var n = 0;
  var it = sgWord.segment(TEXT)[Symbol.iterator]();
  for (var r = it.next(); !r.done; r = it.next()) n++;
  return n;
}, { sinkMayDiffer: true, minMs: 80 });

bench("segmenter-iterate-grapheme", function () {
  var n = 0;
  var it = sgGrapheme.segment(TEXT)[Symbol.iterator]();
  for (var r = it.next(); !r.done; r = it.next()) n++;
  return n;
}, { sinkMayDiffer: true, minMs: 80 });

bench("segmenter-containing", function (i) {
  return sgWord.segment(TEXT).containing(i % TEXT.length).index >= 0 ? 1 : 0;
});
