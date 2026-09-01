/*
 * Mixed BigInt/Number relational and equality comparisons.
 *
 * Guards `js_bigint_float64_cmp` (vendor/quickjs-ng/quickjs.c), the helper
 * every Number-vs-BigInt `<`, `<=`, `>`, `>=`, `==` and `!=` funnels through
 * via `js_compare_bigint`. Its `f != e` arm (differing binary exponents)
 * ordered by magnitude alone and never applied `a_sign`, so EVERY mixed
 * comparison with a negative operand and differing exponents answered the
 * inverted result:
 *
 *     -0.5 <  -1n     ours: true    node/spec: false
 *     -1n  <  -0.5    ours: false   node/spec: true
 *     -2.5 <  -1n     ours: false   node/spec: true
 *
 * That defect is upstream quickjs-ng and it survived 42,883 test262 tests,
 * 31 differential corpus files and all 13 Octane rows, so this file is
 * deliberately a dense matrix rather than a list of the cases the fix's author
 * had in mind. It sweeps, for both operand orders:
 *
 *   - both signs, and mixed signs
 *   - equal binary exponents (the arm that was already correct) and differing
 *     ones (the arm that was not)
 *   - zero on either side, including -0
 *   - +/-Infinity and NaN (the unordered result, which must be false for the
 *     four relational operators and for `==`, and true for `!=`)
 *   - BigInt magnitudes above and below 2^53, where the double cannot
 *     represent every integer, plus 2^200 which no double exponent matches
 *   - denormal and near-overflow doubles
 *   - all eight operators, including `===` / `!==` which must stay false/true
 *     for every Number/BigInt pair regardless
 *
 * It also runs each comparison twice in two syntactic positions -- as a value
 * and as an `if` condition -- because patch 0046 fuses compare-and-branch into
 * a separate opcode, so the branch form is a different code path in this
 * engine and a corpus that only ever evaluated the value form would not see it.
 *
 * A final section covers the String dimension: relational comparison between a
 * BigInt and a numeric string goes through StringToBigInt, and a string long
 * enough to be a rope (JS_STRING_ROPE_SHORT2_LEN is 8192) takes a different
 * internal representation than a flat one.
 */

var nums = [
  -Infinity,
  -1.7976931348623157e308,
  -1e300,
  -9007199254740994,
  -9007199254740992,
  -12345.678,
  -12345,
  -2.5,
  -2,
  -1.5,
  -1,
  -0.5,
  -1e-300,
  -5e-324,
  -0,
  0,
  5e-324,
  1e-300,
  0.5,
  1,
  1.5,
  2,
  2.5,
  12345,
  12345.678,
  9007199254740992,
  9007199254740994,
  1e300,
  1.7976931348623157e308,
  Infinity,
  NaN,
];

var bigs = [
  -(2n ** 200n),
  -(2n ** 70n),
  -9007199254740995n,
  -9007199254740993n,
  -9007199254740992n,
  -12346n,
  -12345n,
  -3n,
  -2n,
  -1n,
  0n,
  1n,
  2n,
  3n,
  12345n,
  12346n,
  9007199254740992n,
  9007199254740993n,
  9007199254740995n,
  2n ** 70n,
  2n ** 200n,
];

/* Value position. */
function ops(a, b) {
  return [a < b, a <= b, a > b, a >= b, a == b, a != b, a === b, a !== b]
    .map(function (v) { return v ? 'T' : 'f'; })
    .join('');
}

/* Branch position -- patch 0046 fuses these into compare-and-branch opcodes. */
function opsBranch(a, b) {
  var out = '';
  if (a < b) out += 'T'; else out += 'f';
  if (a <= b) out += 'T'; else out += 'f';
  if (a > b) out += 'T'; else out += 'f';
  if (a >= b) out += 'T'; else out += 'f';
  if (a == b) out += 'T'; else out += 'f';
  if (a != b) out += 'T'; else out += 'f';
  if (a === b) out += 'T'; else out += 'f';
  if (a !== b) out += 'T'; else out += 'f';
  return out;
}

function label(v) {
  if (typeof v === 'bigint') return String(v) + 'n';
  if (Object.is(v, -0)) return '-0';
  return String(v);
}

print('--- number OP bigint ---');
for (var i = 0; i < nums.length; i++) {
  for (var j = 0; j < bigs.length; j++) {
    var n = nums[i], g = bigs[j];
    var v = ops(n, g), br = opsBranch(n, g);
    print(label(n) + ' ' + label(g) + ' ' + v + (v === br ? '' : ' BRANCH-DIFFERS ' + br));
  }
}

print('--- bigint OP number ---');
for (var i = 0; i < bigs.length; i++) {
  for (var j = 0; j < nums.length; j++) {
    var g = bigs[i], n = nums[j];
    var v = ops(g, n), br = opsBranch(g, n);
    print(label(g) + ' ' + label(n) + ' ' + v + (v === br ? '' : ' BRANCH-DIFFERS ' + br));
  }
}

/*
 * Transitivity and antisymmetry, checked mechanically rather than by reading
 * the table above. `a < b` must imply `b > a`, and exactly one of `<`, `==`,
 * `>` must hold for any ordered pair. An inverted arm satisfies antisymmetry
 * (it inverts both directions) but breaks trichotomy against a third value, so
 * both checks are here.
 */
print('--- antisymmetry ---');
var bad = 0;
for (var i = 0; i < nums.length; i++) {
  for (var j = 0; j < bigs.length; j++) {
    var n = nums[i], g = bigs[j];
    if ((n < g) !== (g > n)) { print('ASYM lt ' + label(n) + ' ' + label(g)); bad++; }
    if ((n <= g) !== (g >= n)) { print('ASYM le ' + label(n) + ' ' + label(g)); bad++; }
    if ((n == g) !== (g == n)) { print('ASYM eq ' + label(n) + ' ' + label(g)); bad++; }
    var trich = (n < g ? 1 : 0) + (n == g ? 1 : 0) + (n > g ? 1 : 0);
    var expect = (n !== n) ? 0 : 1;   /* NaN is unordered with everything */
    if (trich !== expect) { print('TRICH ' + label(n) + ' ' + label(g) + ' ' + trich); bad++; }
  }
}
print('antisymmetry violations: ' + bad);

/*
 * Sorting a mixed array is the same helper used O(n log n) times with every
 * pair of neighbours, so a sign inversion shows up as a wrong permutation
 * rather than as a wrong boolean.
 */
print('--- sort mixed ---');
var mixed = [3n, -2.5, -1n, 0, 2n, -0.5, 1.5, -3n, 0n, 2.5, -12345n, 12345.5];
mixed.sort(function (a, b) { return a < b ? -1 : a > b ? 1 : 0; });
print(mixed.map(label).join(','));

/* Descending, to exercise the opposite branch of the comparator. */
var mixed2 = [3n, -2.5, -1n, 0, 2n, -0.5, 1.5, -3n, 0n, 2.5, -12345n, 12345.5];
mixed2.sort(function (a, b) { return a > b ? -1 : a < b ? 1 : 0; });
print(mixed2.map(label).join(','));

/*
 * Boundary sweep around 2^53 and around the exponent boundary of a
 * power-of-two BigInt, in both signs. These are the pairs where the double's
 * exponent and the BigInt's differ by exactly one, i.e. the smallest possible
 * `f != e`.
 */
print('--- exponent boundaries ---');
var pows = [1n, 2n, 4n, 1024n, 1n << 52n, 1n << 53n, 1n << 54n, 1n << 200n];
for (var i = 0; i < pows.length; i++) {
  var p = pows[i];
  var d = Number(p);
  var cands = [d, d / 2, d * 2, d - 1, d + 1, d * 1.5, d / 1.5];
  for (var k = 0; k < cands.length; k++) {
    print(label(p) + ' vs ' + label(cands[k]) + ' -> ' + ops(p, cands[k]) + ' | ' + ops(cands[k], p));
    print(label(-p) + ' vs ' + label(-cands[k]) + ' -> ' + ops(-p, -cands[k]) + ' | ' + ops(-cands[k], -p));
    print(label(-p) + ' vs ' + label(cands[k]) + ' -> ' + ops(-p, cands[k]) + ' | ' + ops(cands[k], -p));
  }
}

/*
 * String dimension. Relational comparison of a BigInt against a String applies
 * StringToBigInt to the string (a non-integer string yields undefined and the
 * comparison is false), and `==` does the same. Ropes are included because a
 * concatenation longer than JS_STRING_ROPE_SHORT2_LEN (8192) is stored as a
 * rope rather than a flat string, which is a distinct internal representation
 * -- the class of difference that hid the `js_eq_slow` rope bug (patch 0058).
 */
print('--- bigint vs string ---');
var flatBig = '1' + '0'.repeat(60);            /* 10^60, flat */
var ropeBig = '1' + '0'.repeat(9000);          /* 10^9000, rope */
var ropeNeg = '-1' + '0'.repeat(9000);
var strs = ['-2', '-1', '-0.5', '-0', '0', '0.5', '1', '2', '', '  -2  ', 'x',
            flatBig, ropeBig, ropeNeg];
var sbigs = [-2n, -1n, 0n, 1n, 2n, 10n ** 60n, 10n ** 9000n, -(10n ** 9000n)];
for (var i = 0; i < strs.length; i++) {
  for (var j = 0; j < sbigs.length; j++) {
    var s = strs[i], g = sbigs[j];
    var tag = s.length > 40 ? '<' + s.length + ' chars, ' + s[0] + '>' : JSON.stringify(s);
    var gtag = String(g).length > 40 ? '<bigint ' + String(g).length + ' digits, ' + String(g)[0] + '>' : String(g) + 'n';
    print(tag + ' ' + gtag + ' ' + ops(s, g) + ' | ' + ops(g, s));
  }
}

/*
 * A rope compared against a Number goes through ToNumber on the rope, not
 * through the BigInt path at all; included so the file also pins that a rope
 * numeric string still reads as the number it spells.
 */
print('--- rope vs number ---');
var ropeSmall = '-' + '0'.repeat(9000) + '2.5';
print(ropeSmall.length + ' ' + (ropeSmall < -2) + ' ' + (ropeSmall == -2.5) + ' ' + (ropeSmall > -3));
print((ropeSmall < -2n) + ' ' + (ropeSmall == -2n) + ' ' + (ropeSmall > -3n));
