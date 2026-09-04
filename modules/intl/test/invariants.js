/*
 * Self-checking invariants for react-native-quickjs-intl.
 *
 * WHY THESE ARE NOT IN tests/differential/intl/
 *   Everything in that directory is diffed byte-for-byte against node, which is
 *   the strongest kind of evidence available and is the right default. These
 *   checks cannot go there, for two different reasons:
 *
 *   - The parts-round-trip invariant is one **node itself fails**. MEASURED on
 *     node v22.20.0 / ICU 77.1: formatting 2024-05-17T14:35:07.250Z as en-US in
 *     UTC with {year,month:'long',day,hour,minute:'2-digit'}, `format()` emits
 *     U+0020 at index 20 where `formatToParts()` emits U+202F NARROW NO-BREAK
 *     SPACE, so the parts do not concatenate back to the formatted string. The
 *     two print identically and are not equal. Diffing against node would mean
 *     a corpus that fails forever against a *correct* implementation.
 *
 *   - The lazy-accessor checks are about this module's installation mechanism,
 *     which node has no equivalent of at all.
 *
 * CONTRACT
 *   Run with any binary that has the module installed:
 *
 *     build-rel/intl-cli modules/intl/test/invariants.js
 *
 *   Prints one line per check and exits non-zero if any failed. It is
 *   deliberately backend-agnostic: it asserts *invariants*, never specific
 *   formatted text, so the same file is the acceptance test for the stub, the
 *   Apple backend and the Android backend. Anything that depends on which CLDR
 *   version answered belongs in the cross-platform divergence corpus instead.
 */

var failures = 0;
var checks = 0;

function ok(label, cond, detail) {
  checks++;
  if (cond) {
    print('ok    ' + label);
  } else {
    failures++;
    print('FAIL  ' + label + (detail === undefined ? '' : '  [' + detail + ']'));
  }
}

/* ------------------------------------------------------------------------ */
/* The lazy accessor                                                         */
/* ------------------------------------------------------------------------ */

/*
 * The descriptor has two phases, and both are asserted, in order.
 *
 * `Object.getOwnPropertyDescriptor` does **not** invoke a getter, so before
 * anything reads `Intl` the descriptor is honestly an accessor pair. That is
 * deviation D8: an app that inspects the descriptor before first use sees an
 * accessor where the specification describes a data property. It is the price
 * of first-use materialization from a module, it is unobservable to anything
 * that actually *uses* Intl, and it is enumerated rather than hidden.
 *
 * The setter half is not decoration. A getter-only accessor is not writable —
 * assignment is a silent no-op in sloppy mode and a TypeError under strict —
 * and `globalThis.Intl = ...` is exactly what a feature-detecting polyfill
 * does. Without the setter, adding this module would turn "app installs a
 * polyfill" into "app throws".
 */
(function () {
  var before = Object.getOwnPropertyDescriptor(globalThis, 'Intl');
  var lazy = before && typeof before.get === 'function';
  if (lazy) {
    ok('pre-read: accessor pair (D8)', typeof before.set === 'function',
       Object.keys(before).join(','));
    ok('pre-read: non-enumerable', before.enumerable === false);
    ok('pre-read: configurable', before.configurable === true);
  } else {
    /* Already materialized — the harness read Intl before this file ran. */
    ok('pre-read: already a data property', 'value' in before);
    ok('pre-read: non-enumerable', before.enumerable === false);
    ok('pre-read: configurable', before.configurable === true);
  }

  /* Force materialization. */
  var intl = globalThis.Intl;
  ok('first read yields an object', typeof intl === 'object' && intl !== null);

  var after = Object.getOwnPropertyDescriptor(globalThis, 'Intl');
  ok('post-read: data property', after && 'value' in after,
     after ? Object.keys(after).join(',') : 'absent');
  ok('post-read: writable', after && after.writable === true);
  ok('post-read: non-enumerable', after && after.enumerable === false);
  ok('post-read: configurable', after && after.configurable === true);
  ok('post-read: getter is gone', after && after.get === undefined);
  ok('Intl identity is stable', globalThis.Intl === globalThis.Intl &&
     globalThis.Intl === intl);
  ok('Symbol.toStringTag', Object.prototype.toString.call(intl) === '[object Intl]');

  var seen = false;
  for (var k in globalThis) if (k === 'Intl') seen = true;
  ok('Intl does not appear in for-in over globalThis', !seen);
})();

/* ------------------------------------------------------------------------ */
/* Real constructors                                                         */
/* ------------------------------------------------------------------------ */

/*
 * This is the check that would fail if the class shapes had been built out of
 * host functions instead of real JavaScript. A jsi::Function from
 * createFromHostFunction is not a constructor: `new` against it succeeds and
 * produces an object with none of the prototype's methods, and `typeof` still
 * reports "function". The failure is silent, which is why it is asserted here
 * rather than assumed.
 */
(function () {
  var f = new Intl.DateTimeFormat('en-US', { timeZone: 'UTC' });
  ok('new produces an instance of the constructor',
     f instanceof Intl.DateTimeFormat);
  ok('instances inherit prototype methods',
     typeof f.formatToParts === 'function' &&
     typeof f.resolvedOptions === 'function');
  ok('prototype is the constructor prototype',
     Object.getPrototypeOf(f) === Intl.DateTimeFormat.prototype);

  function Sub() { Intl.DateTimeFormat.call(this); }
  ok('calling the constructor as a function returns an instance',
     Intl.DateTimeFormat('en-US', { timeZone: 'UTC' }) instanceof
       Intl.DateTimeFormat);
  void Sub;

  var Extended = null;
  try {
    /* eslint-disable no-eval */
    Extended = (0, eval)(
      '(class X extends Intl.DateTimeFormat { constructor(l, o) { super(l, o); this.tag = 1; } })');
  } catch (e) {
    Extended = null;
  }
  if (Extended) {
    var x = new Extended('en-US', { timeZone: 'UTC' });
    ok('subclassing works: instanceof both', x instanceof Extended &&
       x instanceof Intl.DateTimeFormat);
    ok('subclassing works: inherited method usable',
       typeof x.format(0) === 'string');
    ok('subclassing works: subclass field present', x.tag === 1);
  } else {
    ok('subclassing works', false, 'class syntax unavailable');
  }
})();

/* ------------------------------------------------------------------------ */
/* formatToParts                                                             */
/* ------------------------------------------------------------------------ */

/*
 * The invariant a caller actually depends on: the parts cover the formatted
 * string exactly, in order, with no gaps and no overlaps. A backend that
 * reconstructs part boundaries by string surgery — which is what Hermes does on
 * Apple — can drop a separator or absorb a non-breaking space and still look
 * right to the eye. This catches that.
 *
 * It is checked across a range of option shapes because the failure is
 * pattern-dependent: a bare y/M/d pattern has few literals to lose.
 */
(function () {
  var WHEN = Date.UTC(2024, 4, 17, 14, 35, 7, 250);
  var SHAPES = [
    { year: 'numeric', month: 'numeric', day: 'numeric' },
    { year: 'numeric', month: 'long', day: 'numeric' },
    { weekday: 'long', year: 'numeric', month: 'long', day: 'numeric' },
    { hour: 'numeric', minute: '2-digit' },
    { hour: 'numeric', minute: '2-digit', second: '2-digit' },
    { year: 'numeric', month: 'long', day: 'numeric', hour: 'numeric',
      minute: '2-digit' },
    { dateStyle: 'full', timeStyle: 'medium' },
    { dateStyle: 'short' },
    { era: 'short', year: 'numeric' },
    { hour: 'numeric', minute: '2-digit', timeZoneName: 'short' }
  ];
  var VALID_TYPES = {
    literal: 1, era: 1, year: 1, relatedYear: 1, yearName: 1, month: 1, day: 1,
    weekday: 1, dayPeriod: 1, hour: 1, minute: 1, second: 1,
    fractionalSecond: 1, timeZoneName: 1, unknown: 1
  };

  for (var i = 0; i < SHAPES.length; i++) {
    var opts = { timeZone: 'UTC' };
    for (var k in SHAPES[i]) opts[k] = SHAPES[i][k];
    var f = new Intl.DateTimeFormat('en-US', opts);
    var whole = f.format(WHEN);
    var parts = f.formatToParts(WHEN);
    var joined = '';
    var shapeOk = true;
    var typesOk = true;
    for (var j = 0; j < parts.length; j++) {
      joined += parts[j].value;
      if (Object.keys(parts[j]).join(',') !== 'type,value') shapeOk = false;
      if (typeof parts[j].type !== 'string' ||
          typeof parts[j].value !== 'string') shapeOk = false;
      if (!VALID_TYPES[parts[j].type]) typesOk = false;
      /* An empty part is never useful and is a symptom of a bad decomposition. */
      if (parts[j].value.length === 0) shapeOk = false;
    }
    var label = JSON.stringify(SHAPES[i]);
    ok('parts concatenate to format() ' + label, joined === whole,
       JSON.stringify(joined) + ' != ' + JSON.stringify(whole));
    ok('parts have exactly {type,value} ' + label, shapeOk);
    ok('every part type is an ECMA-402 type ' + label, typesOk);
  }
})();

/* ------------------------------------------------------------------------ */
/* Deterministic release of platform formatters                              */
/* ------------------------------------------------------------------------ */

/*
 * Each formatter owns a platform object — an NSDateFormatter, an
 * android.icu.text.DateFormat, or a map entry on the Kotlin side — released by
 * the QuickJS finalizer of the handle object. There is no way to observe the
 * release from JavaScript, so this cannot assert it directly. What it can do is
 * exercise the path hard enough that a double-free or a use-after-free shows
 * up, which under an assert-enabled engine build (-O0 -g, no -DNDEBUG) is an
 * immediate abort rather than a silent corruption.
 *
 * That combination is the point: this loop is cheap and proves nothing on a
 * release build, and is a real memory-safety test on an assert build. Run it
 * both ways.
 */
(function () {
  var acc = 0;
  for (var i = 0; i < 2000; i++) {
    var f = new Intl.DateTimeFormat('en-US', {
      timeZone: 'UTC',
      year: 'numeric',
      month: i % 2 ? 'long' : 'numeric',
      day: 'numeric'
    });
    acc += f.format(i * 86400000).length;
    if (i % 7 === 0) acc += f.formatToParts(i * 86400000).length;
  }
  ok('2000 formatter create/format/discard cycles', acc > 0, 'acc=' + acc);
})();

/* A formatter outliving the expression that made it, and a bound format
   function outliving its formatter — the two shapes that would expose a
   premature finalize. */
(function () {
  var fns = [];
  for (var i = 0; i < 200; i++) {
    fns.push(new Intl.DateTimeFormat('en-US', { timeZone: 'UTC' }).format);
  }
  var okAll = true;
  for (var j = 0; j < fns.length; j++) {
    if (typeof fns[j](0) !== 'string') okAll = false;
  }
  ok('bound format outlives the formatter expression', okAll);
})();

/* ------------------------------------------------------------------------ */
/* resolvedOptions is a faithful round trip                                  */
/* ------------------------------------------------------------------------ */

/*
 * Feeding resolvedOptions() back into the constructor must produce the same
 * resolved options again. This is the check that catches a backend reporting
 * something it will not accept — a numbering system the locale does not
 * support, a calendar name in the wrong vocabulary — which is otherwise
 * invisible until a user hits it.
 */
(function () {
  var SHAPES = [
    { year: 'numeric', month: 'long', day: 'numeric' },
    { hour: 'numeric', minute: '2-digit' },
    { dateStyle: 'medium', timeStyle: 'short' },
    { weekday: 'short', hour: 'numeric', minute: '2-digit', second: '2-digit' }
  ];
  for (var i = 0; i < SHAPES.length; i++) {
    var opts = { timeZone: 'UTC' };
    for (var k in SHAPES[i]) opts[k] = SHAPES[i][k];
    var a = new Intl.DateTimeFormat('en-US', opts).resolvedOptions();
    var b = new Intl.DateTimeFormat(a.locale, a).resolvedOptions();
    ok('resolvedOptions round trips ' + JSON.stringify(SHAPES[i]),
       JSON.stringify(a) === JSON.stringify(b),
       JSON.stringify(a) + ' vs ' + JSON.stringify(b));
  }
})();

/* ------------------------------------------------------------------------ */
/* Per-formatter state must not leak from one call into the next.            */
/* ------------------------------------------------------------------------ */

/*
 * These are regression checks for a class of bug that a single-call test can
 * never see: a formatter is reused, so anything a call leaves behind on the
 * backend object is visible to the NEXT call.
 *
 * The first one is a real bug found on 2026-07-27 while benchmarking
 * (docs/intl-vs-node.md). The Apple backend swaps its *positive* prefix for
 * the negative one to render a negative zero, and undoes it on the next call —
 * but the >15-significant-digit path returned before the undo ever ran:
 *
 *   n.format(-0);    // "-0"
 *   n.format(1e21);  // "-1,000,000,000,000,000,000,000"   <-- wrong
 *
 * Backend-agnostic by construction: the no-platform backend and Android must
 * pass it too, and it says nothing about which symbols a locale uses.
 */
(function stateDoesNotLeakBetweenCalls() {
  var n = new Intl.NumberFormat('en-US');
  var negZero = n.format(-0);
  var big = n.format(1e21);
  ok('format(-0) then format(1e21) does not leak the negative sign',
     big.charAt(0) !== '-', 'format(-0) gave ' + negZero + ', then ' + big);
  ok('format(-0) still renders its sign', negZero.charAt(0) === '-', negZero);
  ok('format(1e21) alone is positive',
     new Intl.NumberFormat('en-US').format(1e21).charAt(0) !== '-');

  /* The same shape for the memoized implicit formatters: a cached formatter is
     shared between call sites, so a leak there is shared too. */
  var a = (-0).toLocaleString('en-US');
  var b = (1e21).toLocaleString('en-US');
  ok('toLocaleString(-0) then toLocaleString(1e21) does not leak the sign',
     b.charAt(0) !== '-', a + ' then ' + b);

  /* Alternating fraction-digit counts drive the backend's digit-count memo. */
  var m = new Intl.NumberFormat('en-US');
  var seq = [m.format(1), m.format(1.25), m.format(2), m.format(2.5)];
  ok('alternating fraction counts each render their own digits',
     seq.join('|') === '1|1.25|2|2.5', seq.join('|'));
})();

/* ------------------------------------------------------------------------ */
/* The implicit-formatter memo must not change any answer.                   */
/* ------------------------------------------------------------------------ */

/*
 * The memo (js/intl.js, "the implicit-formatter memo") makes
 * `(x).toLocaleString(locale)` reuse a formatter. These checks assert that it
 * is (a) actually being taken — a fast path with zero hits has shipped in this
 * project before — and (b) invisible.
 */
(function implicitFormatterMemo() {
  var perf = Intl.__rnqjsPerf;
  ok('Intl.__rnqjsPerf exists', perf && typeof perf.stats === 'function');
  if (!perf) return;
  perf.reset();
  for (var i = 0; i < 20; i++) (i).toLocaleString('de-DE');
  var st = perf.stats();
  ok('the memo is actually reached', st.hits >= 18,
     'hits=' + st.hits + ' misses=' + st.misses);

  /* An options bag must never be cached: ECMA-402 fixes the order in which its
     properties are read and a Proxy or a getter can observe it. */
  perf.reset();
  var reads = 0;
  for (var j = 0; j < 5; j++) {
    (1).toLocaleString('de-DE', { get style() { reads++; return 'decimal'; } });
  }
  ok('an options bag bypasses the memo entirely', reads === 5, 'reads=' + reads);
  ok('an options bag is counted as a bypass', perf.stats().bypasses >= 5,
     JSON.stringify(perf.stats()));

  /* "" is an invalid locale. If it shared a cache key with `undefined` it
     would return a formatted string instead of throwing. */
  (1).toLocaleString();
  var threw = false;
  try { (1).toLocaleString(''); } catch (e) { threw = e instanceof RangeError; }
  ok('an empty locale still throws RangeError after a default-locale call',
     threw);

  /* Turning the memo off must not change any answer. */
  var withMemo = (1234.5).toLocaleString('de-DE') + '|' +
                 'a'.localeCompare('b', 'de') + '|' +
                 new Date(0).toLocaleDateString('en-US', undefined);
  perf.setEnabled(false);
  var withoutMemo = (1234.5).toLocaleString('de-DE') + '|' +
                    'a'.localeCompare('b', 'de') + '|' +
                    new Date(0).toLocaleDateString('en-US', undefined);
  perf.setEnabled(true);
  ok('the memo changes no answer', withMemo === withoutMemo,
     withMemo + ' vs ' + withoutMemo);
})();

/* ------------------------------------------------------------------------ */
/* The NumberFormat and PluralRules fast paths must agree with the slow one. */
/* ------------------------------------------------------------------------ */

/*
 * `fastRoundDecimal` short-circuits the ECMA-402 rounding pipeline when
 * String(x) is already the answer. The check that matters is not that it is
 * fast but that it is INVISIBLE, so every value here is run through a
 * formatter whose options make the fast path legal and through one whose
 * options make it illegal, and the digits must match.
 */
(function fastRoundAgreesWithSlowRound() {
  var VALUES = [0, -0, 1, -1, 0.5, -0.5, 1.25, 12345, 1e21, 1e-7, 0.1, 2.5,
                123456789.987, 1000000, 0.001, 9007199254740993, 1.005];
  /* roundingIncrement 1 vs 5 is the cheapest way to make the predicate false
     without changing the digits for values that are already exact multiples;
     minimumIntegerDigits 1 vs 2 changes the padding, so the comparison uses
     trailingZeroDisplay, which only affects all-zero fractions. */
  for (var i = 0; i < VALUES.length; i++) {
    var v = VALUES[i];
    var fast = new Intl.NumberFormat('en-US', { useGrouping: false }).format(v);
    var slow = new Intl.NumberFormat('en-US',
      { useGrouping: false, trailingZeroDisplay: 'stripIfInteger' }).format(v);
    ok('fast and slow rounding agree for ' + v, fast === slow,
       fast + ' vs ' + slow);
  }

  var perf = Intl.__rnqjsPerf;
  if (perf) {
    perf.reset();
    var nf = new Intl.NumberFormat('en-US');
    for (var k = 0; k < 20; k++) nf.format(k + 0.5);
    ok('the NumberFormat fast path is actually reached',
       perf.stats().fastRoundHits >= 20, JSON.stringify(perf.stats()));
    perf.reset();
    var pr = new Intl.PluralRules('en');
    for (var m2 = 0; m2 < 20; m2++) pr.select(m2);
    ok('the PluralRules fast path is actually reached',
       perf.stats().pluralFastHits >= 20, JSON.stringify(perf.stats()));
  }
})();

/*
 * The three fast paths added by the 2026-07-27 optimization round, each
 * asserted to be REACHED rather than merely correct.
 *
 * This project has shipped a fast path with zero hits that survived a spike, an
 * audit and a relay, and every one of these carries a measured claim in
 * docs/intl-string-seam.md, docs/intl-numberformat-double-path.md or
 * docs/intl-lazy-segmentation.md. A refactor that silently stops taking one
 * should fail a test here, not merely get slower somewhere nobody is looking.
 */
(function newFastPathsAreReached() {
  var perf = Intl.__rnqjsPerf;
  if (!perf) return;

  /* The exact-double route into the backend. 20 small values with two fraction
     digits are all inside 10^(15 - maxFrac), so every one must take it. */
  perf.reset();
  var nf = new Intl.NumberFormat('en-US');
  for (var i = 0; i < 20; i++) nf.format(i + 0.25);
  ok('the NumberFormat exact-double path is actually reached',
     perf.stats().exactDoubleHits >= 20, JSON.stringify(perf.stats()));

  /* And it must DECLINE above the bound. maximumFractionDigits 15 puts the
     bound at 1, so any value of magnitude >= 1 has to fall back. Without this
     the bound could be widened to infinity and the hit assertion above would
     still pass -- which is exactly the mutation
     tools/exact-double-differential.mjs catches with 72 failures. */
  perf.reset();
  var wide = new Intl.NumberFormat('en-US', { maximumFractionDigits: 15 });
  for (var j = 0; j < 20; j++) wide.format(1000 + j + 0.5);
  ok('the exact-double bound actually declines large values',
     perf.stats().exactDoubleHits === 0 && perf.stats().exactDoubleMisses >= 20,
     JSON.stringify(perf.stats()));

  /* The canonicalization memo. */
  perf.reset();
  for (var k = 0; k < 20; k++) Intl.getCanonicalLocales('en-US');
  ok('the canonicalization memo is actually reached',
     perf.stats().canonHits >= 19, JSON.stringify(perf.stats()));

  /* The Segmenter boundary memo, and the laziness beneath it: constructing a
     Segments object without touching it must segment NOTHING. */
  perf.reset();
  var seg = new Intl.Segmenter('en', { granularity: 'word' });
  for (var m = 0; m < 20; m++) seg.segment('the quick brown fox');
  ok('Intl.Segmenter.prototype.segment segments nothing on its own',
     perf.stats().segmentHits === 0 && perf.stats().segmentMisses === 0,
     JSON.stringify(perf.stats()));
  perf.reset();
  for (var n = 0; n < 20; n++) seg.segment('the quick brown fox').containing(4);
  ok('the Segmenter boundary memo is actually reached',
     perf.stats().segmentHits >= 19, JSON.stringify(perf.stats()));

  /* A different text must MISS, or the memo is answering with stale
     boundaries -- the failure mode that would make it wrong rather than slow. */
  perf.reset();
  var a = seg.segment('alpha beta').containing(0).segment;
  var b = seg.segment('gamma delta epsilon').containing(0).segment;
  var c = seg.segment('alpha beta').containing(0).segment;
  ok('a different text misses the Segmenter memo',
     perf.stats().segmentMisses === 3 && perf.stats().segmentHits === 0,
     JSON.stringify(perf.stats()));
  ok('the Segmenter memo returns the right boundaries per text',
     a === 'alpha' && b === 'gamma' && c === 'alpha', a + '/' + b + '/' + c);
})();

/* ------------------------------------------------------------------------ */

print('');
print(failures === 0 ? 'PASS ' + checks + ' checks'
                     : 'FAIL ' + failures + ' of ' + checks + ' checks');
if (failures !== 0) {
  throw new Error(failures + ' invariant(s) failed');
}
