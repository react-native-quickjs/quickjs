/*
 * Rope strings vs flat strings across every operator and coercion that can
 * observe the difference.
 *
 * QuickJS stores a long concatenation as a rope (JS_TAG_STRING_ROPE) rather
 * than a flat string (JS_TAG_STRING); JS_STRING_ROPE_SHORT2_LEN is 8192, so
 * `"a".repeat(9000) + "b"` is a rope and `"ab"` is not. The two tags are the
 * same ECMAScript type, so no observable operation may distinguish them. Any
 * engine site that tests `tag == JS_TAG_STRING` instead of `tag_is_string(tag)`
 * silently drops ropes onto a different path, and that class of defect has now
 * been found twice in vendor/quickjs-ng:
 *
 *   - `js_eq_slow`: `rope == flat` answered false while `rope === flat`
 *     answered true (patch 0058-eq-slow-rope-vs-flat-string).
 *   - `js_relational_slow`: `rope < 1n` applied ToNumber to the rope instead of
 *     StringToBigInt, so `("-" + "0".repeat(9000) + "2.5") < -2n` answered true
 *     where the spec requires false (patch 0067-relational-rope-vs-bigint).
 *
 * Neither was caught by test262 (42,883 tests) or by any other corpus file.
 * The generalisation is docs/measurement-discipline.md section 31: our
 * correctness nets do not cover the string-representation dimension, because
 * the natural thing to write in a test is `"ab" + "c"`, which is flat.
 * This file exists so the next one is caught before it ships: it does not test
 * a single optimization, it tests the *representation dimension* itself.
 *
 * Every value below appears in three forms -- flat literal, rope, and a rope
 * built by a different concatenation shape (left-leaning vs right-leaning) --
 * and every form is put through the same operator table. Node has no ropes, so
 * a byte-for-byte match against node is the statement "the representation is
 * unobservable", which is exactly the invariant.
 */

var N = 9000;   /* comfortably over JS_STRING_ROPE_SHORT2_LEN (8192) */

function rope(prefix, suffix) {
  /* left-leaning: (big + small) */
  return prefix.repeat(N) + suffix;
}
function ropeRight(prefix, suffix) {
  /* right-leaning: (small + big) */
  return suffix + prefix.repeat(N);
}
function flatten(s) {
  /* forces a flat string with the same contents */
  return s.split('').join('');
}

function label(v) {
  if (typeof v === 'string') {
    if (v.length > 32) return '<str len ' + v.length + ' ' + JSON.stringify(v.slice(0, 4)) + '...' + JSON.stringify(v.slice(-4)) + '>';
    return JSON.stringify(v);
  }
  if (typeof v === 'bigint') return String(v) + 'n';
  if (Object.is(v, -0)) return '-0';
  return String(v);
}

function ops(a, b) {
  return [a < b, a <= b, a > b, a >= b, a == b, a != b, a === b, a !== b]
    .map(function (v) { return v ? 'T' : 'f'; })
    .join('');
}

/* ---- 1. rope vs flat with identical contents, every operator ---- */
print('--- rope vs flat, identical contents ---');
var pairs = [
  ['a', 'b'],
  ['0', '1'],
  ['-', '2.5'],
  ['é', 'z'],      /* forces a wide (16-bit) rope leg */
  ['😀', 'x'] /* surrogate pair in the repeated leg */
];
for (var i = 0; i < pairs.length; i++) {
  var L = rope(pairs[i][0], pairs[i][1]);
  var R = ropeRight(pairs[i][0], pairs[i][1]);
  var F = flatten(L);
  print('L/F ' + ops(L, F) + '  F/L ' + ops(F, L) + '  L/L ' + ops(L, L) + '  L/R ' + ops(L, R));
  print('  len ' + L.length + ' ' + F.length + ' charCodeAt0 ' + L.charCodeAt(0) + '/' + F.charCodeAt(0) +
        ' last ' + L.charCodeAt(L.length - 1) + '/' + F.charCodeAt(F.length - 1));
}

/* ---- 2. rope vs flat that differ, so ordering is not just equality ---- */
print('--- rope vs flat, differing contents ---');
var base = rope('a', 'b');
var lower = flatten(rope('a', 'a'));
var higher = flatten(rope('a', 'c'));
var shorter = flatten('a'.repeat(N));
var longer = flatten('a'.repeat(N) + 'bb');
print(ops(base, lower) + ' ' + ops(base, higher) + ' ' + ops(base, shorter) + ' ' + ops(base, longer));
print(ops(lower, base) + ' ' + ops(higher, base) + ' ' + ops(shorter, base) + ' ' + ops(longer, base));

/* ---- 3. rope vs BigInt: StringToBigInt, not ToNumber ---- */
print('--- rope vs bigint ---');
var digitRopes = [
  '1' + '0'.repeat(N),               /* 10^9000, an exact integer string */
  '-1' + '0'.repeat(N),
  '0'.repeat(N) + '5',               /* leading zeros, value 5 */
  '-' + '0'.repeat(N) + '5',
  '-' + '0'.repeat(N) + '2.5',       /* NOT an integer -> StringToBigInt undefined */
  '0'.repeat(N) + '2.5',
  ' '.repeat(N) + '7',               /* leading whitespace is allowed */
  '0x' + 'f'.repeat(N),              /* hex literal, accepted by StringToBigInt */
  'z'.repeat(N)                      /* not numeric at all */
];
var gs = [-(10n ** 9000n), -5n, -2n, 0n, 2n, 5n, 7n, 10n ** 9000n];
for (var i = 0; i < digitRopes.length; i++) {
  var line = '';
  for (var j = 0; j < gs.length; j++) {
    line += ops(digitRopes[i], gs[j]) + '/' + ops(gs[j], digitRopes[i]) + ' ';
  }
  print('rope#' + i + ' len ' + digitRopes[i].length + ' ' + line);
}

/* ---- 4. the same strings, flattened: the two rows must agree ---- */
print('--- flat counterpart, must match row-for-row ---');
for (var i = 0; i < digitRopes.length; i++) {
  var f = flatten(digitRopes[i]);
  var line = '';
  for (var j = 0; j < gs.length; j++) {
    line += ops(f, gs[j]) + '/' + ops(gs[j], f) + ' ';
  }
  print('flat#' + i + ' len ' + f.length + ' ' + line);
}

/* ---- 5. rope vs Number: ToNumber, and rope vs boolean/null/undefined ---- */
print('--- rope vs other primitives ---');
var numRopes = ['0'.repeat(N) + '2.5', '-' + '0'.repeat(N) + '2.5', '0'.repeat(N), ' '.repeat(N)];
var others = [2.5, -2.5, 0, -0, NaN, Infinity, true, false, null, undefined, ''];
for (var i = 0; i < numRopes.length; i++) {
  var line = '';
  for (var j = 0; j < others.length; j++) line += ops(numRopes[i], others[j]) + ' ';
  print('num-rope#' + i + ' ' + line);
}

/* ---- 6. rope as a property key, Map/Set key, and switch discriminant ---- */
print('--- rope as key ---');
var key = rope('k', '!');
var flatKey = flatten(key);
var o = {};
o[key] = 1;
print('read-back ' + o[flatKey] + ' ' + (flatKey in o) + ' ' + Object.keys(o).length +
      ' ' + (Object.keys(o)[0] === flatKey));
var m = new Map();
m.set(key, 'v');
print('map ' + m.get(flatKey) + ' ' + m.has(key));
var s = new Set([key, flatKey]);
print('set-size ' + s.size);
switch (key) {
  case flatKey: print('switch matched flat'); break;
  default: print('switch fell through'); break;
}

/* ---- 7. rope through JSON, indexOf, slice, comparison sort ---- */
print('--- rope through builtins ---');
print(JSON.stringify(rope('a', 'b')).length + ' ' + (JSON.parse(JSON.stringify(key)) === flatKey));
print(key.indexOf('k!') + ' ' + key.lastIndexOf('k') + ' ' + key.slice(-3) + ' ' + key.charAt(0));
var arr = [rope('b', 'z'), rope('a', 'z'), flatten(rope('a', 'z')), 'a', 'b'];
arr.sort();
print(arr.map(function (x) { return x.length + ':' + x.slice(0, 2); }).join(','));

/* ---- 8. strict vs loose must never disagree for two Strings ---- */
print('--- strict/loose agreement sweep ---');
var all = [];
for (var i = 0; i < pairs.length; i++) {
  all.push(rope(pairs[i][0], pairs[i][1]));
  all.push(ropeRight(pairs[i][0], pairs[i][1]));
  all.push(flatten(rope(pairs[i][0], pairs[i][1])));
}
all.push('', 'a', flatten('a'));
var disagree = 0;
for (var i = 0; i < all.length; i++) {
  for (var j = 0; j < all.length; j++) {
    if ((all[i] == all[j]) !== (all[i] === all[j])) disagree++;
    if ((all[i] != all[j]) !== (all[i] !== all[j])) disagree++;
    /* trichotomy: exactly one of <, ==, > holds for any two Strings */
    var t = (all[i] < all[j] ? 1 : 0) + (all[i] == all[j] ? 1 : 0) + (all[i] > all[j] ? 1 : 0);
    if (t !== 1) disagree++;
  }
}
print('strict/loose disagreements: ' + disagree + ' over ' + (all.length * all.length) + ' pairs');
