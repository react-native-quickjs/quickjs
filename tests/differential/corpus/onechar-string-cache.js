/*
 * Differential corpus for the interned one-character (Latin-1) string cache.
 *
 * WHAT THE MECHANISM DOES: `js_new_string_char(ctx, c)` for `c < 0x100` returns
 * a reference to a per-runtime interned `JSString` instead of allocating a new
 * one. The table entry carries a PERMANENT reference, so its `ref_count` is
 * never 1 and it is never uniquely owned.
 *
 * Every case below is a way for that to be observable. Three families:
 *
 *   1. IN-PLACE MUTATION. Two paths in this engine extend a live JSString in
 *      place when its refcount proves sole ownership: `js_accum_append`
 *      (patch 0051) and `JS_ConcatString2`. A table entry can never pass either
 *      test, so the cache can only make them *refuse* -- but a bug that let one
 *      through would corrupt every other holder of that character, which is
 *      every `'a'` in the program. The cases build accumulators seeded FROM a
 *      one-character string, which is the only shape in which the accumulator
 *      is the table entry itself.
 *
 *   2. IDENTITY. Patch 0044 short-circuits `js_string_eq` to true on pointer
 *      identity. Interning makes far more pairs pointer-identical, so any place
 *      that (wrongly) inferred *inequality* from pointer difference, or that
 *      keyed on identity, changes answer. Map/Set keys, `Object.is`, property
 *      keys and `===` are all exercised.
 *
 *   3. REPRESENTATION. `docs/round-4-session-summary.md` sec.31 records that the
 *      safety nets do not cover the flat/rope/slice dimension: a rope-vs-flat
 *      bug survived 42,883 test262 tests (patch 0058). `JS_STRING_ROPE_SHORT2_LEN`
 *      is 8192, so `'a'.repeat(9000) + 'b'` is a ROPE, and comparing it against a
 *      flat string of the same characters must still be `true`. Every one-char
 *      operand below is additionally used against a rope.
 *
 * A one-character string also reaches the ATOM table: `__JS_NewAtom` reuses a
 * `JSString` whose `atom_type` is 0 IN PLACE, so `obj[String.fromCharCode(97)]`
 * can turn a table entry into an atom while the table still holds it. Family 4
 * exercises that ordering in both directions.
 *
 * Run with:  node tests/differential/run.mjs --qjs <binary>
 */
var out = [];
function say(label, v) { out.push(label + '=' + v); }

var CH = String.fromCharCode;

/* ---- 1. in-place mutation with a cached seed ------------------------------- */

function accFromCharCode(n) {
  var s = CH(97);                 /* the accumulator IS a table entry */
  for (var i = 0; i < n; i++) s += 'b';
  return s.length + ':' + s;
}
say('acc-seeded-1', accFromCharCode(1));
say('acc-seeded-3', accFromCharCode(3));
say('acc-seeded-100', accFromCharCode(100).slice(0, 12) + '..');

function accFromIndex(src, n) {
  var s = src[0];                 /* s[i] also routes through js_new_string_char */
  for (var i = 0; i < n; i++) s += src[1];
  return s;
}
say('acc-index', accFromIndex('xy', 5));

/* The character the accumulator was seeded from must be untouched afterwards.
 * If an in-place append ever escaped, THIS is the line that changes. */
function witness() {
  var seed = CH(97);
  var other = CH(97);
  var s = seed;
  for (var i = 0; i < 200; i++) s += 'q';
  return seed + '|' + other + '|' + CH(97) + '|' + seed.length + '|' + s.length;
}
say('witness', witness());

/* Field-store form of the accumulator (put_field), seeded from a cached char. */
function accField(n) {
  var o = { s: CH(122) };
  for (var i = 0; i < n; i++) { o.s += 'z'; }
  return o.s.length + ':' + o.s.charAt(0);
}
say('acc-field', accField(0) + ',' + accField(1) + ',' + accField(500));

/* Two accumulators seeded from the SAME character, grown independently. */
function twoAccs(n) {
  var a = CH(65), b = CH(65);
  for (var i = 0; i < n; i++) { a += '1'; b += '2'; }
  return a + '|' + b;
}
say('two-accs', twoAccs(4));

/* ---- 2. identity ----------------------------------------------------------- */

function idents() {
  var a = CH(97), b = CH(97), c = 'a';
  var d = 'ab'[0], e = 'ab'.charAt(0), f = 'ab'.at(0);
  var g = String.fromCodePoint(97);
  var h = Array.from('ab')[0];
  var parts = [a, b, c, d, e, f, g, h];
  var r = '';
  for (var i = 0; i < parts.length; i++)
    for (var j = 0; j < parts.length; j++)
      r += (parts[i] === parts[j]) ? '1' : '0';
  return r;
}
say('identity-matrix', idents());

say('object-is', Object.is(CH(97), 'a') + ',' + Object.is(CH(97), CH(97)) +
                 ',' + Object.is(CH(97), CH(98)));

function mapKeys() {
  var m = new Map(), s = new Set();
  m.set(CH(97), 1); m.set('a', 2); m.set(CH(98), 3);
  s.add(CH(97)); s.add('a'); s.add(String.fromCodePoint(97));
  return m.size + ':' + m.get('a') + ':' + m.get(CH(97)) + ':' + s.size;
}
say('map-set', mapKeys());

function propKeys() {
  var o = {};
  o[CH(97)] = 1;
  o.a = (o.a || 0) + 10;
  o[String.fromCodePoint(97)] = (o[CH(97)] || 0) + 100;
  return JSON.stringify(o) + ':' + Object.keys(o).join(',') + ':' +
         (CH(97) in o) + ':' + o[CH(97)];
}
say('prop-keys', propKeys());

/* the atom-table conversion, both orders */
function atomOrder() {
  var o1 = {}; o1[CH(200)] = 'first';           /* char -> atom */
  var o2 = { 'É': 'x' }; o2[CH(0xc9)] = 'second'; /* atom -> char */
  return o1[CH(200)] + ',' + o1['È'] + ',' + o2['É'] + ',' +
         Object.keys(o1).length + Object.keys(o2).length;
}
say('atom-order', atomOrder());

/* ---- 3. representation: flat vs rope vs slice ------------------------------ */

var ROPE = 'a'.repeat(9000) + 'b';        /* > JS_STRING_ROPE_SHORT2_LEN (8192) */
var FLAT = ('a'.repeat(9000) + 'b').split('').join('');

say('rope-len', ROPE.length + ',' + FLAT.length + ',' + (ROPE === FLAT));
say('rope-index', ROPE[0] + ROPE[8999] + ROPE[9000] + ',' +
                  (ROPE[0] === CH(97)) + ',' + (ROPE[9000] === CH(98)) + ',' +
                  (ROPE[0] === FLAT[0]));
say('rope-charat', ROPE.charAt(0) + ROPE.charAt(9000) + ',' +
                   (ROPE.charAt(0) === 'a'));
say('rope-vs-char', (ROPE.slice(9000) === CH(98)) + ',' +
                    (ROPE.slice(0, 1) === CH(97)) + ',' +
                    (CH(97) === 'a'.repeat(1)));
say('rope-concat', (CH(97) + ROPE).length + ',' + (ROPE + CH(98)).length + ',' +
                   (CH(97) + ROPE)[0] + (ROPE + CH(98))[9001]);

/* a one-char string appended to a rope, then compared with the flat form */
function ropeAccum() {
  var r = 'a'.repeat(9000);
  r += CH(98);
  return r.length + ',' + (r === FLAT) + ',' + r[9000];
}
say('rope-accum', ropeAccum());

/* a slice of length 1 vs a cached char */
function sliceOne() {
  var big = 'q'.repeat(300) + 'Z';
  var s = big.substring(300, 301);
  return s + ',' + (s === CH(90)) + ',' + (s === 'Z') + ',' + s.length;
}
say('slice-one', sliceOne());

/* ---- 4. wide characters, the boundary and mutation of the cached value ----- */

say('boundary', [0, 1, 127, 128, 255, 256, 257, 0xffff].map(function (c) {
  var s = CH(c);
  return s.length + ':' + s.charCodeAt(0) + ':' + (s === CH(c));
}).join('|'));

say('nul', CH(0).length + ',' + (CH(0) + 'x').length + ',' +
           (CH(0) + 'x').charCodeAt(0) + ',' + JSON.stringify(CH(0) + 'x'));

say('wide-mix', (CH(97) + CH(0x4e2d)).length + ',' +
                (CH(0x4e2d) + CH(97)).charCodeAt(0) + ',' +
                (CH(97) + CH(0x4e2d) === 'a中'));

/* every cached character round-trips through JSON, encodeURI-safe subset */
var acc = 0, chk = '';
for (var c = 0; c < 256; c++) {
  var s = CH(c);
  acc += s.charCodeAt(0) * (s.length === 1 ? 1 : 0);
  if (s !== CH(c) || s.charCodeAt(0) !== c) chk += 'BAD' + c;
}
say('roundtrip', acc + ',' + (chk || 'ok'));

/* string methods applied to a cached char must not mutate it */
function methodsOnCached() {
  var s = CH(65);
  var r = [s.toLowerCase(), s.toUpperCase(), s.repeat(3), s.padEnd(3, '.'),
           s.concat('B'), s.trim(), s.normalize(), s.replace('A', 'B')].join('/');
  return r + '|' + s + '|' + CH(65) + '|' + s.length;
}
say('methods', methodsOnCached());

/* the cached char used as a regexp/split/join operand */
function regexpish() {
  var s = 'a,b,c';
  return s.split(CH(44)).join('-') + ',' + s.indexOf(CH(98)) + ',' +
         s.replace(new RegExp(CH(98)), 'B') + ',' +
         (CH(44) + '').codePointAt(0);
}
say('regexpish', regexpish());

/* sorting and comparison of cached chars */
say('sort', ['c', CH(97), 'b', CH(100)].sort().join('') + ',' +
            (CH(97) < CH(98)) + ',' + (CH(98) <= 'b') + ',' +
            CH(97).localeCompare(CH(98)));

/* ---- 5. GC / lifetime: the table must survive collection ------------------- */

function churn() {
  var keep = CH(107);
  for (var i = 0; i < 20000; i++) { var t = CH(i & 255); if (t.length !== 1) return 'BAD'; }
  var o = {}; for (var j = 0; j < 5000; j++) o['k' + j] = [j];
  return keep + ',' + CH(107) + ',' + (keep === CH(107)) + ',' + keep.length;
}
say('churn', churn());

print(out.join('\n'));
