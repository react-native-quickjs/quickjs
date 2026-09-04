/*
 * How often does the backend supply *real* formatToParts boundaries?
 *
 * WHY THIS EXISTS
 *   docs/intl-platform-backed.md records deviation D1: where a backend cannot
 *   supply real part boundaries it must return a single {type:"literal"} part
 *   covering the whole string, because a guessed decomposition is worse than an
 *   honest coarse one — callers index into the result and cannot tell that a
 *   guess was made.
 *
 *   That rule is only useful if the fallback rate is *known*. A backend that
 *   silently degraded to one literal part everywhere would pass every shape
 *   test in test/invariants.js and be useless. This file counts it.
 *
 * WHAT IT MEASURES
 *   Two rates over (locale x option-shape) pairs:
 *     - round-trip failures: parts do not concatenate back to format().
 *       Must be zero. Anything else is a bug, not a degradation.
 *     - coarse fallbacks: a multi-character result decomposed into exactly one
 *       literal part. Allowed, reported, and expected to be zero on both real
 *       backends.
 *
 * MEASURED, 2026-07-26, macOS 26.5 / Apple backend, 40 locales x 9 shapes =
 * 360 cases: 0 round-trip failures, 0 coarse fallbacks. The pattern-walk
 * decomposition in ios/IntlPlatform.mm is exact on every case tested, which is
 * ahead of Hermes — its Apple date implementation reconstructs parts by
 * splitting the formatted string on NSCharacterSet.alphanumericCharacterSet
 * (PlatformIntlApple.mm:1979-2018).
 *
 *   NOT YET MEASURED on Android. The same file is the instrument; running it
 *   under the Android backend on a device is the measurement, and until that
 *   happens the Android fallback rate is unknown rather than zero.
 *
 * CONTRACT
 *   Exits non-zero on any round-trip failure. A coarse fallback is reported but
 *   does not fail, because on the stub backend and on an old OS it is the
 *   correct behaviour.
 */

var LOCALES = [
  'en-US', 'en-GB', 'fr-FR', 'de-DE', 'es-ES', 'it-IT', 'pt-BR', 'nl-NL',
  'ru-RU', 'tr-TR', 'pl-PL', 'ja-JP', 'ko-KR', 'zh-Hans-CN', 'zh-Hant-TW',
  'ar-EG', 'he-IL', 'hi-IN', 'th-TH', 'vi-VN', 'cs-CZ', 'hu-HU', 'fi-FI',
  'sv-SE', 'da-DK', 'nb-NO', 'el-GR', 'uk-UA', 'id-ID', 'ms-MY', 'ro-RO',
  'bg-BG', 'hr-HR', 'sr-RS', 'sk-SK', 'sl-SI', 'et-EE', 'lv-LV', 'lt-LT',
  'fa-IR'
];

var SHAPES = [
  { year: 'numeric', month: 'numeric', day: 'numeric' },
  { year: 'numeric', month: 'long', day: 'numeric' },
  { weekday: 'long', year: 'numeric', month: 'long', day: 'numeric' },
  { hour: 'numeric', minute: '2-digit' },
  { year: 'numeric', month: 'long', day: 'numeric', hour: 'numeric',
    minute: '2-digit' },
  { dateStyle: 'full', timeStyle: 'medium' },
  { dateStyle: 'long' },
  { era: 'short', year: 'numeric' },
  { hour: 'numeric', minute: '2-digit', second: '2-digit',
    timeZoneName: 'short' }
];

var WHEN = Date.UTC(2024, 4, 17, 14, 35, 7, 250);

var total = 0;
var roundTripFailures = 0;
var coarse = 0;
var failures = [];

for (var i = 0; i < LOCALES.length; i++) {
  for (var j = 0; j < SHAPES.length; j++) {
    var opts = { timeZone: 'UTC' };
    for (var k in SHAPES[j]) opts[k] = SHAPES[j][k];

    var f = new Intl.DateTimeFormat(LOCALES[i], opts);
    var whole = f.format(WHEN);
    var parts = f.formatToParts(WHEN);
    total++;

    var joined = '';
    for (var m = 0; m < parts.length; m++) joined += parts[m].value;
    if (joined !== whole) {
      roundTripFailures++;
      if (failures.length < 10) {
        failures.push(LOCALES[i] + ' ' + JSON.stringify(SHAPES[j]) + ': ' +
                      JSON.stringify(joined) + ' != ' + JSON.stringify(whole));
      }
    }
    if (parts.length === 1 && parts[0].type === 'literal' && whole.length > 3) {
      coarse++;
    }
  }
}

print('backend             ' + Intl.__rnqjsBackend());
print('cases               ' + total);
print('round-trip failures ' + roundTripFailures);
print('coarse fallbacks    ' + coarse + '  (' +
      ((coarse / total) * 100).toFixed(1) + '%)');
for (var e = 0; e < failures.length; e++) print('  ' + failures[e]);

if (roundTripFailures !== 0) {
  throw new Error(roundTripFailures + ' formatToParts round-trip failure(s)');
}
