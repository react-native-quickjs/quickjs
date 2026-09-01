// Differential corpus for the widened admission on the FUSED comparison
// family OP_cmp_br8 / OP_cmp_br (round-5 item B).
//
// WHY A SECOND FILE.  corpus/cmp-widen-fast-arms.js covers the same semantics
// but every one of its cases is in VALUE position -- `show("a<b", a < b)` --
// which the compiler emits as an unfused OP_lt/OP_strict_eq.  The fused opcode
// is only emitted when a comparison is immediately consumed by a conditional
// branch, so a corpus written entirely in value position exercises none of the
// arms attached to it.  MEASURED: navierstokes executes OP_lt 364 times against
// OP_cmp_br8 375,598,613 times, i.e. the fused form is where the population is
// and it needs its own cases.
//
// Every case below is therefore written as `if (a OP b)`, in a function so the
// operands are locals the parser cannot fold, and the branch is fused.
//
// The three things this file exists to catch:
//   * 1 === 1.0 and a rope === a flat string -- ECMAScript Type and QuickJS tag
//     are not in bijection, so an admission test that answers on tag identity
//     breaks the language and passes every benchmark.
//   * NaN, -0 and +-Infinity through all eight sub-ops in branch position.
//   * loose == must NOT reach the strict-equality arm: `null == undefined` is
//     true while `null === undefined` is false, and `"1" == 1` is true.

function br(a, b) {
  var r = "";
  if (a <  b) r += "1"; else r += "0";
  if (a <= b) r += "1"; else r += "0";
  if (a >  b) r += "1"; else r += "0";
  if (a >= b) r += "1"; else r += "0";
  if (a == b) r += "1"; else r += "0";
  if (a != b) r += "1"; else r += "0";
  if (a === b) r += "1"; else r += "0";
  if (a !== b) r += "1"; else r += "0";
  // the negated forms take the other polarity bit of the fused sub-op
  if (!(a <  b)) r += "1"; else r += "0";
  if (!(a === b)) r += "1"; else r += "0";
  return r;
}

var vals = [0, -0, 1, -1, 0.5, -0.5, 1.5, 2, 1e308, -1e308,
            Infinity, -Infinity, NaN, 2147483647, 2147483648,
            -2147483648, -2147483649, 4294967296, 1e21, 5e-324,
            "1", "", "abc", true, false, null, undefined,
            {}, [], [1]];
// BigInt is deliberately NOT in this matrix.  A relational comparison of a
// BigInt against a NEGATIVE double of a different magnitude is answered with
// the wrong sign by quickjs-ng itself -- `-0.5 < -1n` reads true -- and the
// defect is in js_bigint_float64_cmp's `if (f != e)` branch, which returns
// -1/+1 from the exponent order alone and never applies a_sign.  It reproduces
// on the unmodified patch stack with none of the round-5 arms built, so putting
// it in the matrix would make this file fail for a reason that has nothing to
// do with the arms under test.  See docs/round5-batch.md.  BigInt EQUALITY,
// which the arms must refuse, is covered in its own section below.

for (var i = 0; i < vals.length; i++)
  for (var j = 0; j < vals.length; j++)
    print("br " + i + " " + j + " " + br(vals[i], vals[j]));

// The int/float identity a tag-inequality admission gets wrong, in branch
// position.  1 is JS_TAG_INT and 1.0*1.0 is JS_TAG_FLOAT64.
var one_i = 1, one_f = 1.0 * 1.0, zero_p = 0, zero_n = -0;
if (one_i === one_f) print("1 === 1.0 -> true"); else print("1 === 1.0 -> false");
if (one_f === one_i) print("1.0 === 1 -> true"); else print("1.0 === 1 -> false");
if (zero_p === zero_n) print("0 === -0 -> true"); else print("0 === -0 -> false");
if (zero_p < zero_n) print("0 < -0 -> true"); else print("0 < -0 -> false");
if (zero_p <= zero_n) print("0 <= -0 -> true"); else print("0 <= -0 -> false");

// A REAL rope.  JS_STRING_ROPE_SHORT2_LEN is 8192, so a shorter concatenation
// is flattened eagerly and tests nothing; five characters is not a rope.
// JS_TAG_STRING and JS_TAG_STRING_ROPE are both ECMAScript String, so neither
// the same-tag arm nor the distinct-type arm may answer this pair.
var rope = "a".repeat(9000) + "b";
var flat = rope.split("").join("");
var rope2 = "a".repeat(9000) + "b";
var ropeC = "a".repeat(9000) + "c";
function ropecase(name, r, f) {
  if (r === f) print(name + " === true"); else print(name + " === false");
  if (r !== f) print(name + " !== true"); else print(name + " !== false");
  if (r == f) print(name + " == true"); else print(name + " == false");
  if (r != f) print(name + " != true"); else print(name + " != false");
  if (r < f) print(name + " < true"); else print(name + " < false");
  if (r >= f) print(name + " >= true"); else print(name + " >= false");
}
ropecase("rope/flat", rope, flat);
ropecase("flat/rope", flat, rope);
ropecase("rope/rope2", rope, rope2);
ropecase("rope/ropeC", rope, ropeC);
ropecase("rope/self", rope, rope);
ropecase("rope/undef", rope, undefined);
ropecase("undef/rope", undefined, rope);
ropecase("rope/null", rope, null);
ropecase("rope/1", rope, 1);
ropecase("rope/obj", rope, {});

// Two DISTINCT flat strings with equal contents, long enough not to be interned
// as the same atom, compared in branch position.  This is the case that
// distinguishes the arm (which must refuse same-tag strings and let
// js_strict_eq_slow compare them) from an arm that answers same-tag strings by
// pointer identity: the pointers differ and the contents do not.
var lhs = "";
for (var q = 0; q < 40; q++) lhs += "payload-" + (q % 7) + "|";
var rhs = "";
for (var q = 0; q < 40; q++) rhs += "payload-" + (q % 7) + "|";
var rhsDiff = rhs.slice(0, rhs.length - 1) + "!";
print("flat ptr distinct " + (lhs.length === rhs.length));
ropecase("flat/flat-equal", lhs, rhs);
ropecase("flat/flat-differ", lhs, rhsDiff);
ropecase("flat/self", lhs, lhs);
var fc = "abc", fc2 = String.fromCharCode(97, 98, 99);
ropecase("short/short-equal", fc, fc2);
// the same pair with one side a rope and the other a flat string of equal
// contents but a different pointer
ropecase("rope/flat-equal-2", "a".repeat(9000) + "b", flat);

// BigInt: JS_TAG_SHORT_BIG_INT and JS_TAG_BIG_INT are both BigInt.
var sb = 1n, hb = 9007199254740993n - 9007199254740992n;
if (sb === hb) print("short === heap bigint -> true");
else print("short === heap bigint -> false");
if (sb == 1) print("1n == 1 -> true"); else print("1n == 1 -> false");
if (sb === 1) print("1n === 1 -> true"); else print("1n === 1 -> false");
var bigs = [0n, 1n, -1n, 2n, 9007199254740993n, -9007199254740993n];
for (var bi = 0; bi < bigs.length; bi++) {
  for (var bj = 0; bj < bigs.length; bj++) {
    var x = bigs[bi], y = bigs[bj], r = "";
    if (x === y) r += "1"; else r += "0";
    if (x !== y) r += "1"; else r += "0";
    if (x == y) r += "1"; else r += "0";
    if (x < y) r += "1"; else r += "0";
    if (x >= y) r += "1"; else r += "0";
    print("bb " + bi + " " + bj + " " + r);
  }
  // BigInt against int32 operands only; see the note on the value matrix above.
  var z = bigs[bi], q = "";
  if (z === 1) q += "1"; else q += "0";
  if (z == 1) q += "1"; else q += "0";
  if (z == 0) q += "1"; else q += "0";
  if (z === undefined) q += "1"; else q += "0";
  if (z === null) q += "1"; else q += "0";
  if (z === "1") q += "1"; else q += "0";
  if (z == "1") q += "1"; else q += "0";
  if (z === {}) q += "1"; else q += "0";
  print("bi " + bi + " " + q);
}

// Loose == must not be answered by the strict-equality arm.
var n = null, u;
if (n == u) print("null == undefined -> true"); else print("null == undefined -> false");
if (n === u) print("null === undefined -> true"); else print("null === undefined -> false");
if (n != u) print("null != undefined -> true"); else print("null != undefined -> false");
var o = {};
if (o == null) print("obj == null -> true"); else print("obj == null -> false");
var doc = { valueOf: function () { return 0; } };
if (doc == 0) print("valueOf obj == 0 -> true"); else print("valueOf obj == 0 -> false");
if (doc === 0) print("valueOf obj === 0 -> true"); else print("valueOf obj === 0 -> false");

// ToPrimitive must still run, exactly once, on a fused relational.
var calls = 0;
var counting = { valueOf: function () { calls++; return 2.5; } };
if (counting < 3) print("counting < 3 -> true"); else print("counting < 3 -> false");
if (counting >= 2.5) print("counting >= 2.5 -> true"); else print("counting >= 2.5 -> false");
print("valueOf calls = " + calls);

// A throwing valueOf inside a fused comparison must unwind with the operands
// released and the correct exception, not leave the stack unbalanced.
var thrower = { valueOf: function () { throw new RangeError("boom"); } };
try { if (thrower < 1) print("no"); else print("no"); }
catch (e) { print("threw " + e.name + " " + e.message); }
try { if (1.5 > thrower) print("no"); else print("no"); }
catch (e) { print("threw2 " + e.name + " " + e.message); }

// A Symbol is a unique ECMAScript type but its tag is not admitted; a relational
// on one must still throw.
var sy1 = Symbol("s"), sy2 = Symbol("s");
if (sy1 === sy1) print("sym self -> true"); else print("sym self -> false");
if (sy1 === sy2) print("sym other -> true"); else print("sym other -> false");
if (sy1 === undefined) print("sym undef -> true"); else print("sym undef -> false");
try { if (sy1 < sy2) print("no"); else print("no"); }
catch (e) { print("sym relational threw " + e.name); }

// The loop shape the arm targets: a float-dense comparison in a hot branch,
// checksummed, plus a strict-equality branch against undefined at a
// polymorphic site.
var acc = 0, arrf = [];
for (var i = 0; i < 200; i++) arrf.push(i * 0.5 - 25);
for (var k = 0; k < 200; k++) {
  if (arrf[k] < 0) acc -= 1;
  else if (arrf[k] > 10.25) acc += 2;
  else acc += 1;
}
print("float branch acc = " + acc);

var mixed = [1, 1.5, "x", null, undefined, {}, true, 2n];
var seen = 0;
for (var a = 0; a < mixed.length; a++)
  for (var b = 0; b < mixed.length; b++)
    if (mixed[a] === mixed[b]) seen++;
print("strict-eq matches = " + seen);
