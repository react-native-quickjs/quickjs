// Differential corpus for the joint "widen the int-only fast arms" change:
// float and mixed int/float comparison, same-tag / distinct-type strict
// equality, and the cheap-ToInt32 bitwise arm.
// See docs/comparison-slow-path-tag-census.md.
//
// Every case here exists to kill a specific way of getting the admission test
// wrong. The ones that matter most, because they are invisible to Octane and to
// every other corpus file:
//
//   * `1 === 1.0` and `1n === 1n`+heap-BigInt -- JS_TAG_INT/JS_TAG_FLOAT64 are
//     both ECMAScript Number and JS_TAG_SHORT_BIG_INT/JS_TAG_BIG_INT are both
//     BigInt, so a `tag1 != tag2 -> false` admission breaks the language.
//   * a rope compared with a flat string -- JS_TAG_STRING and
//     JS_TAG_STRING_ROPE are both String, and neither may be answered by a
//     pointer compare.
//   * `4294967296 | 0` -- (int32_t) on an out-of-range double is UB and
//     saturates on arm64, so the exponent guard is load-bearing.
//   * `null == undefined` (true) against `null === undefined` (false) -- the
//     strict-equality arm must not be reachable from loose `==`.
//
// The values are held in locals and read through functions so the parser cannot
// constant-fold the operation away; a folded case tests nothing.

function show(label, v) { print(label + " = " + String(v)); }

// ---------------------------------------------------------------- relational
// float,float and mixed int/float, all four operators, both operand orders.
var floats = [0, -0, 1, -1, 0.5, -0.5, 1.5, 1e308, -1e308,
              Infinity, -Infinity, NaN, 2147483647, 2147483648,
              -2147483648, -2147483649, 4294967296, 1e21, 5e-324];
var ints = [0, 1, -1, 2, 2147483647, -2147483648];

function rel(a, b) {
  return (a < b) + "," + (a <= b) + "," + (a > b) + "," + (a >= b) + "," +
         (a == b) + "," + (a != b) + "," + (a === b) + "," + (a !== b);
}

for (var i = 0; i < floats.length; i++) {
  for (var j = 0; j < floats.length; j++) {
    print("ff " + i + " " + j + " " + rel(floats[i], floats[j]));
  }
}
for (var i = 0; i < ints.length; i++) {
  for (var j = 0; j < floats.length; j++) {
    print("if " + i + " " + j + " " + rel(ints[i], floats[j]));
    print("fi " + i + " " + j + " " + rel(floats[j], ints[i]));
  }
}

// The int/float identity that a tag-inequality admission gets wrong.
show("1 === 1.0", 1 === 1.0);
show("1.0 === 1", 1.0 === 1);
show("1 !== 1.0", 1 !== 1.0);
var one_i = 1, one_f = 1.0 * 1.0;
show("locals 1 === 1.0", one_i === one_f);
show("0 === -0", 0 === -0);
show("-0 === 0", -0 === 0);
show("0 < -0", 0 < -0);
show("0 <= -0", 0 <= -0);
show("NaN === NaN", NaN === NaN);
show("NaN !== NaN", NaN !== NaN);
show("NaN == NaN", NaN == NaN);
show("NaN <= NaN", NaN <= NaN);
show("NaN >= NaN", NaN >= NaN);
show("1/0 > 1e308", 1 / 0 > 1e308);

// ------------------------------------------------------------------- BigInt
// Both BigInt tags are the same ECMAScript type; and BigInt must never be
// answered by a double comparison.
var bshort = 1n, bshort2 = 1n;
var bheap = 2n ** 70n;
var bheap2 = 2n ** 70n;
show("1n === 1n", bshort === bshort2);
show("bheap === bheap", bheap === bheap2);
show("1n === bheap", bshort === bheap);
show("1n === 1", bshort === 1);
show("1n == 1", bshort == 1);
show("1n < 2", bshort < 2);
show("1n < 1.5", bshort < 1.5);
show("bheap > 1e20", bheap > 1e20);
show("bheap === 1180591620717411303424", bheap === 1180591620717411303424);
show("9007199254740993n === 9007199254740992", 9007199254740993n === 9007199254740992);
show("1n === true", bshort === true);
show("1n === undefined", bshort === undefined);

// ------------------------------------------------------------------ strings
// A rope and a flat string of the same contents are the same String value.
//
// The rope must be a REAL one. QuickJS only builds a JS_TAG_STRING_ROPE when
// the left operand is longer than JS_STRING_ROPE_SHORT2_LEN (8192) or the right
// is longer than JS_STRING_ROPE_SHORT_LEN (512) -- see JS_ConcatString. An
// earlier version of this file concatenated five characters and produced a flat
// string, so the case was vacuous and two broken builds passed it. `.split("")
// .join("")` rebuilds the same characters through a string buffer, which yields
// a flat JS_TAG_STRING.
var rope = "a".repeat(9000) + "b";
var flat = rope.split("").join("");
show("rope === flat", rope === flat);
show("flat === rope", flat === rope);
show("rope !== flat", rope !== flat);
show("flat !== rope", flat !== rope);
show("rope == flat", rope == flat);
show("rope != flat", rope != flat);
show("rope < flat", rope < flat);
show("rope <= flat", rope <= flat);
show("rope === rope", rope === rope);
var rope2 = "a".repeat(9000) + "b";
show("rope === rope2", rope === rope2);
var ropeDiff = "a".repeat(9000) + "c";
show("rope === ropeDiff", rope === ropeDiff);
show("rope === flat.slice(0, 100)", rope === flat.slice(0, 100));
show("rope === undefined", rope === undefined);
show("undefined === rope", undefined === rope);
show("rope === null", rope === null);
show("rope === o1_placeholder", rope === 1);
show("rope.length", rope.length);
show("flat.length", flat.length);
var s1 = "abc", s2 = "ab" + "c", s3 = String.fromCharCode(97, 98, 99);
show("s1 === s2", s1 === s2);
show("s1 === s3", s1 === s3);
show("s1 === 'abd'", s1 === "abd");
show("s1 === undefined", s1 === undefined);
show("undefined === s1", undefined === s1);
show("s1 === null", s1 === null);
show("s1 === 1", s1 === 1);
show("'1' === 1", "1" === 1);
show("'1' == 1", "1" == 1);
show("'' == 0", "" == 0);
show("s1 === {}", s1 === {});

// ----------------------------------------------------- objects and nullish
var o1 = {}, o2 = {}, o3 = o1;
var arr = [1, 2, 3];
show("o1 === o1", o1 === o1);
show("o1 === o3", o1 === o3);
show("o1 === o2", o1 === o2);
show("o1 !== o2", o1 !== o2);
show("o1 === null", o1 === null);
show("o1 === undefined", o1 === undefined);
show("o1 == null", o1 == null);
show("o1 == undefined", o1 == undefined);
show("o1 === false", o1 === false);
show("o1 === 0", o1 === 0);
show("o1 === 0.5", o1 === 0.5);
show("arr === arr", arr === arr);
show("null === undefined", null === undefined);
show("null == undefined", null == undefined);
show("null === null", null === null);
show("undefined === undefined", undefined === undefined);
show("null == 0", null == 0);
show("undefined == 0", undefined == 0);
show("null == false", null == false);
show("true === true", true === true);
show("true === false", true === false);
show("true === 1", true === 1);
show("true == 1", true == 1);
show("false === 0", false === 0);
show("false == 0", false == 0);
show("true === 'true'", true === "true");
show("false === undefined", false === undefined);
show("null === false", null === false);
show("undefined === null", undefined === null);
show("undefined !== null", undefined !== null);

// A void 0 that arrives as a value rather than a literal, which is the shape
// box2d produces (float,undef at 69% of its strict-equality traffic).
var u;
var farr = [1.5, 2.5];
show("farr[0] === u", farr[0] === u);
show("farr[5] === u", farr[5] === u);
show("u === farr[5]", u === farr[5]);
show("farr[0] !== u", farr[0] !== u);

// Symbols are a distinct type but not admitted to the fast arm.
var sym1 = Symbol("s"), sym2 = Symbol("s");
show("sym1 === sym1", sym1 === sym1);
show("sym1 === sym2", sym1 === sym2);
show("sym1 === undefined", sym1 === undefined);
show("sym1 === o1", sym1 === o1);

// ----------------------------------------------------- ToPrimitive must run
// The fast arms must not swallow an operand with a valueOf/toString.
var box = { valueOf: function () { return 5; } };
var boxs = { toString: function () { return "7"; } };
show("box < 6", box < 6);
show("box < 6.5", box < 6.5);
show("box == 5", box == 5);
show("box === 5", box === 5);
show("boxs == 7", boxs == 7);
show("box | 0", box | 0);
var sideEffects = 0;
var counting = { valueOf: function () { sideEffects++; return 2.5; } };
show("counting < 3", counting < 3);
show("counting === 2.5", counting === 2.5);
show("sideEffects", sideEffects);

// -------------------------------------------------------------- bitwise ops
// ToInt32 over the whole exponent range, including the values where a naked
// (int32_t) cast is undefined behaviour.
var bits = [0, -0, 1, -1, 1.9, -1.9, 0.5, -0.5,
            2147483647, 2147483648, 2147483649, -2147483648, -2147483649,
            4294967295, 4294967296, 4294967297, 8589934592,
            1e15, 1e21, 1e300, -1e300, 5e-324,
            NaN, Infinity, -Infinity,
            2147483647.5, -2147483648.5, 4294967295.5];
for (var i = 0; i < bits.length; i++) {
  var b = bits[i];
  print("bit " + i + " " + (b | 0) + " " + (b & -1) + " " + (b ^ 0) + " " +
        (b << 0) + " " + (b >> 0) + " " + (b >>> 0));
}
for (var i = 0; i < bits.length; i++) {
  for (var j = 0; j < 6; j++) {
    print("shift " + i + " " + j + " " + (bits[i] << j) + " " +
          (bits[i] >> j) + " " + (bits[i] >>> j));
  }
}
// Shift counts that are themselves doubles, and out of range.
var counts = [0, 1, 31, 32, 33, 63, 64, -1, 1.5, 31.9, 32.5, NaN, Infinity];
for (var i = 0; i < counts.length; i++) {
  print("count " + i + " " + (1.5 << counts[i]) + " " + (-9.5 >> counts[i]) +
        " " + (-9.5 >>> counts[i]) + " " + (12345.6 << counts[i]));
}
// The Octane PRNG idiom: an int masked against a double that needs the
// modulo-2^32 branch of ToInt32.
var seed = 49734321;
for (var i = 0; i < 6; i++) {
  seed = ((seed + 0x7ed55d16) + (seed << 12)) & 0xffffffff;
  seed = ((seed ^ 0xc761c23c) ^ (seed >>> 19)) & 0xffffffff;
  seed = ((seed + 0x165667b1) + (seed << 5)) & 0xffffffff;
  print("prng " + i + " " + seed);
}
// Bitwise on non-numbers must keep going through the slow helper.
show("true | 0", true | 0);
show("null | 0", null | 0);
show("undefined | 0", undefined | 0);
show("'12' | 0", "12" | 0);
show("'0x10' | 0", "0x10" | 0);
show("[] | 0", [] | 0);
show("[7] | 0", [7] | 0);
show("true & 3", true & 3);
show("false | 5", false | 5);
try { print("bigint|0 " + (1n | 0)); } catch (e) { print("bigint|0 threw " + e.name); }
try { print("bigint|bigint " + String(3n | 5n)); } catch (e) { print("threw " + e.name); }
try { print("1n < sym " + (1n < sym1)); } catch (e) { print("bigint<sym threw " + e.name); }

// --------------------------------------------------- comparison in branches
// The fused OP_cmp_br path, which is a different handler from OP_lt et al and
// needs its own coverage: a comparison whose result is consumed by a branch
// rather than pushed.
function branchy(a, b) {
  var n = 0;
  if (a < b) n += 1;
  if (a <= b) n += 2;
  if (a > b) n += 4;
  if (a >= b) n += 8;
  if (a == b) n += 16;
  if (a != b) n += 32;
  if (a === b) n += 64;
  if (a !== b) n += 128;
  while (a < b) { n += 256; break; }
  return n;
}
for (var i = 0; i < floats.length; i++) {
  for (var j = 0; j < 5; j++) {
    print("br " + i + " " + j + " " + branchy(floats[i], floats[j]));
  }
}
print("br obj " + branchy(o1, o2) + " " + branchy(o1, o1) + " " +
      branchy(null, undefined) + " " + branchy(u, u) + " " +
      branchy("a", "b") + " " + branchy(rope, flat) + " " +
      branchy(true, true) + " " + branchy(true, 1));
