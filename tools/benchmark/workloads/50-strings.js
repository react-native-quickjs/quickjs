/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Strings and regular expressions.
 *
 * RN is a string-heavy runtime in places people do not expect it to be. Every
 * color is parsed from a string, every route is matched with a regex, every
 * key prop is built by concatenation, and every number that reaches the UI is
 * converted to a string. Two of these were measured as large Hermes-vs-V8
 * gaps: `replace` with a global regex at 9.7x, and the color-parsing regex
 * ladder that `normalizeColor` runs on every style resolution.
 *
 * All inputs live in global variables. That is not stylistic — a string
 * operation on a literal is constant-foldable, and Hermes at -O will fold it,
 * producing a benchmark that measures nothing. A global may be reassigned by
 * anything, so the engine has to actually load and actually compute.
 *
 * Every row returns a length or a computed value rather than the string
 * itself, which forces the result to be materialized.
 */

/* ------------------------------------------------------- template literals */

/*
 * Four interpolations, which is the shape of a log line, a key prop, or an
 * accessibility label. Mixed types on purpose: two of the four go through
 * number-to-string on the way in.
 */
var T_NAME = 'ScrollView';
var T_ID = 4821;
var T_STATE = 'mounted';
var T_DEPTH = 7;

bench({
  name: 'str/template-literal-4interp',
  unit: 'string',
  run: function () {
    return `<${T_NAME} id=${T_ID} state=${T_STATE} depth=${T_DEPTH}>`.length;
  },
  expect: 42,
});

/* The `+` form of the identical string, to show whether the template desugars
   to anything different. */
bench({
  name: 'str/concat-plus-4interp',
  unit: 'string',
  run: function () {
    return ('<' + T_NAME + ' id=' + T_ID + ' state=' + T_STATE + ' depth=' + T_DEPTH + '>').length;
  },
  expect: 42,
});

/* ----------------------------------------------------------- string building */

/*
 * Two ways to build one string out of 200 pieces. `+=` in a loop is what
 * everyone writes; the array-and-join is what everyone is told to write
 * instead. Whether that advice is true is engine-specific — an engine with
 * ropes or with in-place append makes `+=` the faster one, and RN's own
 * serializers are written both ways.
 */
var PIECES = null;

bench({
  name: 'str/build-plus-equals-200',
  unit: 'string',
  setup: function () {
    PIECES = [];
    for (var i = 0; i < 200; i++) PIECES.push('item' + i + ',');
  },
  run: function () {
    var s = '';
    for (var i = 0; i < 200; i++) s += PIECES[i];
    return s.length;
  },
  expect: 1490,
});

bench({
  name: 'str/build-array-join-200',
  unit: 'string',
  setup: function () {
    PIECES = [];
    for (var i = 0; i < 200; i++) PIECES.push('item' + i + ',');
  },
  run: function () {
    var a = [];
    for (var i = 0; i < 200; i++) a.push(PIECES[i]);
    return a.join('').length;
  },
  expect: 1490,
});

/* Join over an array that already exists, isolating join from the push loop. */
bench({
  name: 'str/join-preexisting-200',
  unit: 'string',
  setup: function () {
    PIECES = [];
    for (var i = 0; i < 200; i++) PIECES.push('item' + i + ',');
  },
  run: function () {
    return PIECES.join('').length;
  },
  expect: 1490,
});

/* ------------------------------------------------------------------ replace */

/*
 * `str.replace(/\s+/g, ' ')` — whitespace normalization, which RN runs on
 * every piece of text that goes through `Text` trimming, and which every app
 * runs on user input. Hermes measured 9.7x slower than V8 here: a global
 * replace is a match loop plus a rebuild, and both halves are interpreted.
 *
 * The input has runs of 1-4 whitespace characters so the regex genuinely has
 * to backtrack over the `+`, rather than matching single spaces and exiting.
 */
var WS_RE = /\s+/g;
var WS_INPUT = '';

bench({
  name: 'str/replace-whitespace-global',
  unit: 'replace',
  setup: function () {
    var parts = [];
    for (var i = 0; i < 60; i++) {
      var gap = '   '.slice(0, 1 + (i % 3)) + (i % 4 === 0 ? '\n' : '');
      parts.push('word' + i + gap);
    }
    WS_INPUT = parts.join('');
  },
  run: function () {
    return WS_INPUT.replace(WS_RE, ' ').length;
  },
  expect: 410,
});

/* The non-global single replace, as the control: the difference is the match
   loop rather than the regex itself. */
bench({
  name: 'str/replace-whitespace-single',
  unit: 'replace',
  run: function () {
    return WS_INPUT.replace(/\s+/, ' ').length;
  },
  expect: 484,
});

/* -------------------------------------------------------------- color regex */

/*
 * RN's `normalizeColor` is a ladder: it tries a sequence of regexes — #rgb,
 * #rgba, #rrggbb, #rrggbbaa, rgb(), rgba(), hsl(), hsla(), and a named-color
 * table — and stops at the first that matches. A `#rrggbb` string, which is
 * what almost every stylesheet contains, is matched by the third. Anything
 * later in the ladder pays for every failed attempt before it.
 *
 * Two rows: the hit in isolation, and the realistic path where eight regexes
 * are tried and fail before the ninth matches. The gap between them is the
 * cost of the ladder itself, and it is the argument for reordering it.
 */
var HEX6 = /^#([0-9a-fA-F]{6})$/;
var COLOR_INPUT = '#3b82f6';

var LADDER = [
  /^#([0-9a-fA-F]{3})$/,
  /^#([0-9a-fA-F]{4})$/,
  /^rgb\(\s*([0-9.]+)\s*,\s*([0-9.]+)\s*,\s*([0-9.]+)\s*\)$/,
  /^rgba\(\s*([0-9.]+)\s*,\s*([0-9.]+)\s*,\s*([0-9.]+)\s*,\s*([0-9.]+)\s*\)$/,
  /^hsl\(\s*([0-9.]+)\s*,\s*([0-9.]+)%\s*,\s*([0-9.]+)%\s*\)$/,
  /^hsla\(\s*([0-9.]+)\s*,\s*([0-9.]+)%\s*,\s*([0-9.]+)%\s*,\s*([0-9.]+)\s*\)$/,
  /^hwb\(\s*([0-9.]+)\s+([0-9.]+)%\s+([0-9.]+)%\s*\)$/,
  /^#([0-9a-fA-F]{8})$/,
  /^#([0-9a-fA-F]{6})$/,
];

bench({
  name: 'str/regex-hex-color-hit',
  unit: 'parse',
  run: function () {
    var m = HEX6.exec(COLOR_INPUT);
    return m === null ? -1 : parseInt(m[1], 16);
  },
  expect: 3900150,
});

bench({
  name: 'str/regex-hex-color-ladder-9',
  unit: 'parse',
  run: function () {
    for (var i = 0; i < LADDER.length; i++) {
      var m = LADDER[i].exec(COLOR_INPUT);
      if (m !== null) return parseInt(m[1], 16);
    }
    return -1;
  },
  expect: 3900150,
});

/* `test` rather than `exec`, which skips building the match array. If the gap
   is large, the ladder's failing attempts should all be `test`. */
bench({
  name: 'str/regex-hex-color-test-only',
  unit: 'test',
  run: function () {
    return HEX6.test(COLOR_INPUT) ? 1 : 0;
  },
  expect: 1,
});

/* ------------------------------------------------------------- other regexes */

/*
 * Route matching, as a navigation library does on every push. Anchored with
 * two numeric captures, which is the common case.
 */
var ROUTE_RE = /^\/users\/(\d+)\/posts\/(\d+)$/;
var ROUTE_INPUT = '/users/48210/posts/993';

bench({
  name: 'str/regex-route-exec',
  unit: 'match',
  run: function () {
    var m = ROUTE_RE.exec(ROUTE_INPUT);
    return m === null ? -1 : +m[1] + +m[2];
  },
  expect: 49203,
});

/*
 * Email validation, which every form runs on every keystroke. Deliberately
 * mixed inputs: a validator that only ever sees valid input is not the one
 * that ships.
 */
var EMAIL_RE = /^[^\s@]+@[^\s@]+\.[^\s@]{2,}$/;
var EMAILS = null;

bench({
  name: 'str/regex-email-test',
  unit: '8 tests',
  setup: function () {
    EMAILS = [
      'ammar@example.com',
      'not-an-email',
      'a.b.c@sub.domain.co.uk',
      '@nope.com',
      'person+tag@example.io',
      'trailing@dot.',
      'x@y.zz',
      'spaces in@email.com',
    ];
  },
  run: function () {
    var n = 0;
    for (var i = 0; i < 8; i++) if (EMAIL_RE.test(EMAILS[i])) n++;
    return n;
  },
  expect: 4,
});

/* ------------------------------------------------------------ split / filter */

/*
 * The non-regex way to take a URL apart, which is what most routers do before
 * they reach for a regex. Allocates an array, then a second one.
 */
var PATH_INPUT = '/api/v2/users/48210/posts/993/comments/';

bench({
  name: 'str/split-filter-path',
  unit: 'parse',
  run: function () {
    var parts = PATH_INPUT.split('/').filter(function (p) {
      return p.length > 0;
    });
    return parts.length;
  },
  expect: 7,
});

/* ------------------------------------------------------------- char scanning */

/*
 * A `charCodeAt` scan over 1000 characters. This is what every hand-written
 * tokenizer, escaper and hash function in a JS codebase reduces to, and it is
 * the single tightest string loop an interpreter runs — one bytecode dispatch
 * per character.
 */
var SCAN_INPUT = '';

bench({
  name: 'str/charCodeAt-scan-1000',
  unit: 'scan',
  setup: function () {
    var s = '';
    while (s.length < 1000) s += 'The quick brown fox jumps over the lazy dog 0123456789. ';
    SCAN_INPUT = s.slice(0, 1000);
  },
  run: function () {
    var h = 0;
    for (var i = 0; i < 1000; i++) h = (h * 31 + SCAN_INPUT.charCodeAt(i)) | 0;
    return h;
  },
  expect: 1550817555,
});

/* ------------------------------------------------------- number to string */

/*
 * Integer formatting. Every number that reaches a `Text` node goes through
 * this, and lists render hundreds per frame.
 */
var INTS = null;

bench({
  name: 'str/int-toString-20',
  unit: '20 conversions',
  setup: function () {
    var rnd = 1;
    INTS = [];
    for (var i = 0; i < 20; i++) {
      rnd = (rnd * 1103515245 + 12345) & 0x7fffffff;
      INTS.push(rnd % 1000000);
    }
  },
  run: function () {
    var n = 0;
    for (var i = 0; i < 20; i++) n += INTS[i].toString().length;
    return n;
  },
  expect: 119,
});

/*
 * Fractional doubles, which is the real dtoa path: producing the shortest
 * decimal string that round-trips is a genuinely hard algorithm, and engines
 * differ by an order of magnitude in which one they picked. Layout math,
 * animation progress and any computed dimension land here.
 *
 * This row is also a differential test: if the two engines disagree on the
 * total length, they disagree on shortest-representation output, which would
 * be a spec bug in one of them.
 */
var DOUBLES = null;

bench({
  name: 'str/double-toString-20',
  unit: '20 conversions',
  setup: function () {
    DOUBLES = [];
    for (var i = 1; i <= 20; i++) DOUBLES.push(i / 7);
  },
  run: function () {
    var n = 0;
    for (var i = 0; i < 20; i++) n += String(DOUBLES[i]).length;
    return n;
  },
  expect: 326,
});

/* ------------------------------------------------------------ search */

/*
 * `indexOf` and `includes` over a URI-shaped string, which is what every
 * "is this a remote image?", "is this a data URI?" and "does this asset need
 * the bundler prefix?" check in RN's image pipeline does. The needle is near
 * the end so the scan is not short-circuited immediately.
 */
var URI = 'https://cdn.example.com/assets/v3/images/hero@3x.png?w=1080&q=80&fm=webp';

bench({
  name: 'str/indexOf-uri',
  unit: '4 searches',
  run: function () {
    var n = 0;
    n += URI.indexOf('http') >= 0 ? 1 : 0;
    n += URI.indexOf('data:') >= 0 ? 1 : 0;
    n += URI.indexOf('fm=webp') >= 0 ? 1 : 0;
    n += URI.indexOf('@3x') >= 0 ? 1 : 0;
    return n;
  },
  expect: 3,
});

bench({
  name: 'str/includes-uri',
  unit: '4 searches',
  run: function () {
    var n = 0;
    n += URI.includes('http') ? 1 : 0;
    n += URI.includes('data:') ? 1 : 0;
    n += URI.includes('fm=webp') ? 1 : 0;
    n += URI.includes('@3x') ? 1 : 0;
    return n;
  },
  expect: 3,
});

/* startsWith, which is the check that should be used for the prefix cases
   above and is often not. */
bench({
  name: 'str/startsWith-uri',
  unit: '2 checks',
  run: function () {
    var n = 0;
    n += URI.startsWith('https://') ? 1 : 0;
    n += URI.startsWith('data:') ? 1 : 0;
    return n;
  },
  expect: 1,
});
