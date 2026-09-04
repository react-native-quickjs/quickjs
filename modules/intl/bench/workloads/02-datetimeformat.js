/*
 * Intl.DateTimeFormat.
 *
 * WHAT THIS MODELS
 *   The service that costs a formatjs app the most (92% of the 415 ms and
 *   4.27 MB of the polyfill stack; see docs/intl-platform-backed.md Part 1),
 *   and the one whose backend call is heaviest -- NSDateFormatter on Apple.
 *
 *   Rows are split the same three ways as NumberFormat, plus `parts-*`, which
 *   on Apple walks the pattern NSDateFormatter itself chose and formats each
 *   field token separately. That is the module's most JS-heavy formatting path
 *   and the one most likely to be slow.
 *
 *   `fmt-*` sinks are string lengths, not text, so they compare across CLDR
 *   versions -- but a locale whose pattern length moved between ICU releases
 *   would still trip it, which is why the option-shaped rows carry
 *   sinkMayDiffer and the plain ones do not.
 */

var LOC = "de-DE";
var T0 = 1600000000000; // 2020-09-13T12:26:40Z, a fixed instant

setup(function () {
  new Intl.DateTimeFormat(LOC).format(T0);
});

// ---- constructors -------------------------------------------------------

bench("ctor-bare", function () {
  return new Intl.DateTimeFormat(LOC).format(T0).length;
});

bench("ctor-dateStyle", function () {
  return new Intl.DateTimeFormat(LOC, { dateStyle: "medium" }).format(T0).length;
}, { sinkMayDiffer: true });

bench("ctor-date+time-style", function () {
  return new Intl.DateTimeFormat(LOC, {
    dateStyle: "medium",
    timeStyle: "short",
  }).format(T0).length;
}, { sinkMayDiffer: true });

bench("ctor-component-opts", function () {
  return new Intl.DateTimeFormat(LOC, {
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
  }).format(T0).length;
}, { sinkMayDiffer: true });

bench("ctor-timeZone", function () {
  return new Intl.DateTimeFormat(LOC, { timeZone: "UTC" }).format(T0).length;
});

bench("ctor-undefined-locale", function () {
  return new Intl.DateTimeFormat().format(T0).length;
}, { sinkMayDiffer: true });

// ---- steady-state format ------------------------------------------------

var dfBare = new Intl.DateTimeFormat(LOC);
var dfStyle = new Intl.DateTimeFormat(LOC, {
  dateStyle: "medium",
  timeStyle: "short",
});
var dfUtc = new Intl.DateTimeFormat(LOC, { timeZone: "UTC" });

bench("fmt-bare", function (i) {
  return dfBare.format(T0 + i * 86400000).length;
});

bench("fmt-date+time-style", function (i) {
  return dfStyle.format(T0 + i * 3600000).length;
}, { sinkMayDiffer: true });

bench("fmt-utc", function (i) {
  return dfUtc.format(T0 + i * 86400000).length;
});

bench("fmt-Date-object", function (i) {
  return dfBare.format(new Date(T0 + i * 86400000)).length;
});

bench("fmt-no-arg-now", function () {
  return dfBare.format().length;
});

// ---- formatToParts ------------------------------------------------------

bench("parts-bare", function (i) {
  return dfBare.formatToParts(T0 + i * 86400000).length;
});

bench("parts-date+time-style", function (i) {
  return dfStyle.formatToParts(T0 + i * 3600000).length;
}, { sinkMayDiffer: true });

// ---- range --------------------------------------------------------------

bench("formatRange", function (i) {
  return dfBare.formatRange(T0, T0 + i * 86400000).length;
}, { sinkMayDiffer: true });

// ---- pure JS bookkeeping ------------------------------------------------

bench("resolved-bare", function () {
  return dfBare.resolvedOptions().timeZone.length;
});

bench("supportedLocalesOf", function () {
  return Intl.DateTimeFormat.supportedLocalesOf(["de-DE", "en-US"]).length;
});
