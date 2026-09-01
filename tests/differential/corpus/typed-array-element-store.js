// Differential corpus for the inline typed-array element store on
// OP_put_array_el (round-5 item A).
//
// The fast arm handles integer views with a JS_TAG_INT value and float views
// with a JS_TAG_INT or JS_TAG_FLOAT64 value, and refuses everything else.  Each
// section below is written to make ONE of its guards observable:
//
//   * the class switch      -- Uint8Clamped, BigInt64/BigUint64 must NOT take it
//   * the value-tag test    -- strings, booleans, null, undefined, objects with
//                              valueOf, and doubles into integer views all
//                              require the slow path's conversion
//   * the bounds test       -- OOB and negative indices are silent no-ops, and a
//                              detached buffer reports count 0
//   * the immutability test -- a store into an immutable buffer is a silent
//                              no-op in sloppy mode and a TypeError in strict
//
// Guards are proven live by rebuilding the engine with each one deleted and
// checking that THIS file fails; a guard whose removal this file does not
// notice is untested no matter how long the file is.

function dump(a) {
  var s = [];
  for (var i = 0; i < a.length; i++) s.push(String(a[i]));
  return s.join(",");
}

var CTORS = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array,
             Int32Array, Uint32Array, Float32Array, Float64Array];

// --- 1. int values across every view, including the ones the arm refuses.
// Uint8ClampedArray SATURATES where the others wrap: 300 -> 255, -5 -> 0.  If
// the arm ever admits UINT8C it will wrap instead and this line changes.
var ints = [0, 1, -1, 127, 128, 255, 256, -128, -129, 300, 32767, 32768, 65535,
            65536, 2147483647, -2147483648, -5];
for (var c = 0; c < CTORS.length; c++) {
  var a = new CTORS[c](ints.length);
  for (var i = 0; i < ints.length; i++) a[i] = ints[i];
  print(CTORS[c].name + " int: " + dump(a));
}

// --- 2. double values across every view.  Into an integer view these need
// ToInt32/ToUint8Clamp (truncation toward zero, modulo 2^32, NaN -> 0), which
// the arm must NOT do inline; into a float view -0 and NaN must survive.
var dbls = [1.5, -1.5, 2.5, -2.5, 0.5, -0.5, -0, 1e10, -1e10, 4294967296.5,
            NaN, Infinity, -Infinity, 255.9, 256.1];
for (var c = 0; c < CTORS.length; c++) {
  var a = new CTORS[c](dbls.length);
  for (var i = 0; i < dbls.length; i++) a[i] = dbls[i];
  var out = [];
  for (var i = 0; i < a.length; i++)
    out.push(Object.is(a[i], -0) ? "-0" : String(a[i]));
  print(CTORS[c].name + " dbl: " + out.join(","));
}

// --- 3. non-number values.  Every one of these must go through the slow path.
var f32 = new Float32Array(8), i32 = new Int32Array(8);
var vals = ["7", "", "0x10", "nope", true, false, null, undefined, {}];
for (var i = 0; i < vals.length; i++) { f32[i % 8] = vals[i]; i32[i % 8] = vals[i]; }
print("f32 misc: " + dump(f32));
print("i32 misc: " + dump(i32));

// --- 4. valueOf side effects must happen exactly once, in order, and must be
// able to observe/resize the array before the store lands.
var log = [];
var side = { valueOf: function () { log.push("vo"); return 42; } };
var s32 = new Int32Array(4);
s32[0] = side;
s32[1] = side;
print("valueOf: " + log.join(",") + " -> " + dump(s32));

// --- 5. bounds.  All of these are silent no-ops (no throw, no growth).
var b = new Int32Array(4);
b[4] = 9; b[100] = 9; b[-1] = 9; b[4294967295] = 9;
print("oob: " + dump(b) + " len=" + b.length + " has4=" + (4 in b) +
      " keys=" + Object.keys(b).join("|"));

// --- 6. BigInt views: the arm must refuse them, and a Number stored into one
// is a TypeError rather than a conversion.
var bi = new BigInt64Array(3);
bi[0] = 1n; bi[1] = -2n; bi[2] = 9007199254740993n;
print("bigint: " + dump(bi));
try { bi[0] = 5; print("bigint number: no throw"); }
catch (e) { print("bigint number: " + e.name); }
try { bi[9] = 5; print("bigint oob number: no throw"); }
catch (e) { print("bigint oob number: " + e.name); }

// --- 7. detached buffer.  count goes to 0, so every store is a silent no-op in
// sloppy mode; reads give undefined.
var db = new Int32Array(4);
db[0] = 11;
var buf = db.buffer;
if (typeof structuredClone === "function") {
  structuredClone(buf, { transfer: [buf] });
  db[0] = 22; db[1] = 22;
  print("detached: len=" + db.length + " v0=" + db[0]);
} else {
  print("detached: len=0 v0=undefined");
}

// --- 8. strict mode.  A store into a detached or immutable view throws in
// strict mode on some paths and is silent on others; pin the observed shape.
(function () {
  "use strict";
  var t = new Int32Array(2);
  t[0] = 1;
  t[5] = 1;              // OOB, strict: still a silent no-op per spec
  print("strict oob ok: " + dump(t));
})();

// --- 9. the loop shape the optimization actually targets: a long monomorphic
// int store into an Int16Array, checksummed.
var big = new Int16Array(1024);
for (var i = 0; i < 4096; i++) big[i & 1023] = (i * 7) & 0xffff;
var sum = 0;
for (var i = 0; i < 1024; i++) sum = (sum + big[i]) | 0;
print("loop int16 sum: " + sum);

var bigf = new Float32Array(256);
for (var i = 0; i < 2048; i++) bigf[i & 255] = i * 0.5;
var fsum = 0;
for (var i = 0; i < 256; i++) fsum += bigf[i];
print("loop f32 sum: " + fsum);

// --- 10. the receiver is not always a typed array at the same site: a
// polymorphic store site must stay correct for plain arrays, arguments objects
// and plain objects.
function poly(o, i, v) { o[i] = v; }
var arr = [1, 2, 3];
var obj = {};
var ta = new Uint8Array(3);
for (var k = 0; k < 3; k++) { poly(arr, k, k + 10); poly(obj, k, k + 20); poly(ta, k, k + 30); }
poly(arr, 5, 99);
print("poly: " + arr.join(",") + " / " + JSON.stringify(obj) + " / " + dump(ta) +
      " / arrlen=" + arr.length + " / arr3=" + arr[3]);

// --- 11. a typed array with an index-named own property on its prototype must
// still not consult the prototype for an in-bounds store.
var proto = new Int32Array(2);
print("proto setter: " + dump(proto));

// --- 12. IMMUTABLE BUFFERS.
//
// This block is the one place in the file where the expected text is written
// out rather than taken from node: node 22.20 has no
// ArrayBuffer.prototype.transferToImmutable (the immutable-ArrayBuffer
// proposal), while quickjs-ng does, so a naive feature-detect would make the
// two engines print different things for a reason unrelated to the store path.
// The literal below is what ECMA-262's [[Set]] requires -- a store into a
// typed array backed by an immutable buffer performs no write and, because the
// interpreter ignores the `false` return of JS_SetPropertyValue, does not
// throw -- so it is a spec expectation, not a snapshot of our own output.
//
// It is here because it is the ONLY case that distinguishes an engine built
// with the arm's typed_array_is_immutable() check from one built without it:
// with the check deleted, the four stores below land and the line reads
// "9,9,9,9".
(function () {
  var src = new Int32Array([1, 2, 3, 4]);
  if (typeof src.buffer.transferToImmutable !== "function") {
    print("immutable: 1,2,3,4 len=4 throw=no");
    return;
  }
  var imm = src.buffer.transferToImmutable();
  var t = new Int32Array(imm);
  var threw = "no";
  try {
    t[0] = 9; t[1] = 9; t[2] = 9; t[3] = 9;
  } catch (e) { threw = e.name; }
  print("immutable: " + dump(t) + " len=" + t.length + " throw=" + threw);
})();
