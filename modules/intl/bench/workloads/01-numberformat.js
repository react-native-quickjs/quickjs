/*
 * Intl.NumberFormat.
 *
 * WHAT THIS MODELS
 *   The most-called Intl service in real React Native code: prices in a list,
 *   percentages on a dashboard, a compact "1.2K" badge. Three question kinds,
 *   kept in separate rows because they have completely different fixes:
 *
 *     ctor-*   option resolution + locale negotiation, all in JS in this
 *              module. This is where a JS layer over a native backend is most
 *              exposed, and it is what a component that constructs a formatter
 *              inside render() pays on every frame.
 *     fmt-*    the steady-state call on an existing formatter. Should be near
 *              native if the backend is doing the work; anything above that is
 *              our own overhead.
 *     parts-*  formatToParts, which this module assembles in JS from a shared
 *              decomposition (deviation D18) rather than from each platform's
 *              field iterator.
 *
 *   `resolved-*` is pure JS bookkeeping on both engines and is here as a
 *   control: it is the one NumberFormat operation with no backend call in it
 *   at all.
 */

var LOC = "de-DE";

setup(function () {
  // Force materialization before any timed block so the 389 µs first-read cost
  // (measured; see docs/intl-vs-node.md) cannot land inside a warm row.
  new Intl.NumberFormat(LOC).format(1);
});

// ---- constructors -------------------------------------------------------

bench("ctor-bare", function () {
  return new Intl.NumberFormat(LOC).format(1).length;
});

bench("ctor-currency", function () {
  return new Intl.NumberFormat(LOC, {
    style: "currency",
    currency: "EUR",
  }).format(1).length;
});

bench("ctor-percent-2opts", function () {
  return new Intl.NumberFormat(LOC, {
    style: "percent",
    maximumFractionDigits: 1,
  }).format(0.5).length;
}, { sinkMayDiffer: true }); // Apple de-DE percent is "50%", ICU 77 is "50\u00a0%" -- deviation D4

bench("ctor-undefined-locale", function () {
  return new Intl.NumberFormat().format(1).length;
});

bench("ctor-locale-array", function () {
  return new Intl.NumberFormat(["xx-YY", "de-DE", "en"]).format(1).length;
});

bench("ctor-compact", function () {
  return new Intl.NumberFormat(LOC, {
    notation: "compact",
    compactDisplay: "short",
  }).format(1).length;
}, { sinkMayDiffer: true });

// ---- steady-state format ------------------------------------------------

var nfBare = new Intl.NumberFormat(LOC);
var nfCur = new Intl.NumberFormat(LOC, { style: "currency", currency: "EUR" });
var nfPct = new Intl.NumberFormat(LOC, { style: "percent" });
var nfGrp = new Intl.NumberFormat(LOC, { useGrouping: false });

bench("fmt-integer", function (i) {
  return nfBare.format(i).length;
});

bench("fmt-fraction", function (i) {
  return nfBare.format(i + 0.125).length;
});

bench("fmt-large-grouped", function (i) {
  return nfBare.format(i * 1234567.89).length;
});

bench("fmt-currency", function (i) {
  return nfCur.format(i + 0.5).length;
});

bench("fmt-percent", function (i) {
  return nfPct.format((i % 100) / 100).length;
}, { sinkMayDiffer: true }); // see ctor-percent-2opts

bench("fmt-nogrouping", function (i) {
  return nfGrp.format(i * 1000).length;
});

bench("fmt-string-arg", function (i) {
  return nfBare.format(String(i) + ".5").length;
});

bench("fmt-bigint", function (i) {
  return nfBare.format(BigInt(i) * 1000000000000000000n).length;
});

// ---- formatToParts ------------------------------------------------------

bench("parts-integer", function (i) {
  return nfBare.formatToParts(i).length;
});

bench("parts-currency", function (i) {
  return nfCur.formatToParts(i + 0.5).length;
});

bench("parts-large-grouped", function (i) {
  return nfBare.formatToParts(i * 1234567.89).length;
});

// ---- pure JS bookkeeping ------------------------------------------------

bench("resolved-bare", function () {
  return nfBare.resolvedOptions().maximumFractionDigits;
});

bench("resolved-currency", function () {
  return nfCur.resolvedOptions().minimumFractionDigits;
});

bench("supportedLocalesOf", function () {
  return Intl.NumberFormat.supportedLocalesOf(["de-DE", "en-US"]).length;
});
