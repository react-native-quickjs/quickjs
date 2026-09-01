// Element borrow fusion (round 8): the receiver of a fused a[i] load is
// BORROWED on the fast path instead of dup'd.
//
// This file exists because patch 0023 declared exactly that unsafe, and it was
// right about the case it considered. Its counterexample is the first two tests
// below, verbatim in shape: converting a non-int key runs user `toString`,
// which can assign to the very frame slot the receiver was borrowed from and
// drop its last reference before JS_GetProperty ever touches it.
//
// The fix is not a cleverer borrow, it is a narrower one -- the borrow holds
// only on the fast path (object receiver, int key, fast array, index in range,
// not a hole), where no user code can run. Every test here that goes through a
// callback is therefore exercising the SLOW path and proving that the handler
// took a real reference before entering it.
//
// The two shapes are separate opcodes and both are covered:
//   OP_get_loc8_loc8_array_el_nr  -- receiver from var_buf (round 8, new)
//   OP_get_arg8_loc8_array_el     -- receiver from arg_buf (patch 0023, made
//                                    to borrow on its fast path by round 8)
//
// A use-after-free here does not reliably crash; it reads a freed object and
// usually returns a plausible wrong value, which is why these are diffed
// against node rather than asserted against a hand-written expectation.

function show(label, v) {
  print(label + ": " + (typeof v) + " " + String(v));
}

/* ---------------------------------------------- local receiver, slow path */

function localKilledByToString() {
  var o = [10, 20, 30];
  var k = { toString: function () { o = null; return "1"; } };
  return o[k];
}
show("local receiver cleared by toString", localKilledByToString());

function localSwappedByToString() {
  var o = [1, 2, 3];
  var k = { toString: function () { o = [7, 8, 9]; return "2"; } };
  return o[k];
}
show("local receiver swapped by toString", localSwappedByToString());

function localValueOfOnly() {
  // ToPropertyKey is ToPrimitive(hint string), which tries toString FIRST, so
  // a key carrying only valueOf keys on "[object Object]" and misses.
  var o = ["a", "b", "c"];
  var k = { valueOf: function () { o = null; return 2; } };
  return o[k];
}
show("local receiver, valueOf-only key", localValueOfOnly());

function localSymbolToPrimitive() {
  var o = ["a", "b", "c"];
  var k = {};
  k[Symbol.toPrimitive] = function () { o = null; return "2"; };
  return o[k];
}
show("local receiver, Symbol.toPrimitive", localSymbolToPrimitive());

function localThrowingKey() {
  var o = [1, 2, 3];
  var k = { toString: function () { o = null; throw new Error("boom"); } };
  try { return o[k]; } catch (e) { return e.message; }
}
show("local receiver, throwing key", localThrowingKey());

function localProtoGetter() {
  // The index is in range for the prototype but not for the array, so the fast
  // path's `idx < count` test fails and a user getter runs.
  var a = [1, 2, 3];
  var i = 5;
  Object.defineProperty(Array.prototype, "5", {
    get: function () { a = null; return 42; },
    configurable: true
  });
  var r = a[i];
  delete Array.prototype[5];
  return r;
}
show("local receiver, prototype getter", localProtoGetter());

/* -------------------------------------------- argument receiver, slow path */

function argKilledByToString(o) {
  var k = { toString: function () { o = null; return "1"; } };
  return o[k];
}
show("arg receiver cleared by toString", argKilledByToString([10, 20, 30]));

function argSwappedByToString(o) {
  var k = { toString: function () { o = [7, 8, 9]; return "2"; } };
  return o[k];
}
show("arg receiver swapped by toString", argSwappedByToString([1, 2, 3]));

function argThrowingKey(o) {
  var k = { toString: function () { o = null; throw new Error("e"); } };
  try { return o[k]; } catch (e) { return e.message; }
}
show("arg receiver, throwing key", argThrowingKey([1, 2, 3]));

/* ------------------------------------------------------------- fast paths */

function localFast() { var a = [1, 2, 3, 4, 5], i = 3; return a[i]; }
function localOob() { var a = [1, 2, 3], i = 9; return a[i]; }
function localNeg() { var a = [1, 2, 3], i = -1; return a[i]; }
function localHole() { var a = [1, , 3], i = 1; return a[i]; }
function localStr() { var s = "abc", i = 1; return s[i]; }
function localObj() { var o = { x: 1 }, k = "x"; return o[k]; }
function localLen() { var a = [1, 2, 3], i = "length"; return a[i]; }
function localTa() { var a = new Int32Array([5, 6, 7]), i = 2; return a[i]; }
function localF64() { var a = new Float64Array([1.5, 2.5]), i = 1; return a[i]; }

show("local fast int index", localFast());
show("local out of range", localOob());
show("local negative index", localNeg());
show("local hole", localHole());
show("local string index", localStr());
show("local object key", localObj());
show("local length via key", localLen());
show("local Int32Array", localTa());
show("local Float64Array", localF64());

function argFast(o) { var i = 2; return o[i]; }
function argOob(o) { var i = 99; return o[i]; }
function argHole(o) { var i = 1; return o[i]; }
function argStr(o) { var i = 0; return o[i]; }
function argTa(o) { var i = 1; return o[i]; }

show("arg fast int index", argFast([4, 5, 6]));
show("arg out of range", argOob([1, 2]));
show("arg hole", argHole([1, , 3]));
show("arg string index", argStr("xy"));
show("arg Float64Array", argTa(new Float64Array([1.5, 2.5])));

/* ------------------------------------ sustained loops, to surface a leak */

function localLoop() {
  var a = [], i, s = 0;
  for (i = 0; i < 1000; i++) a[i] = { v: i };
  for (i = 0; i < 1000; i++) s += a[i].v;
  return s;
}
show("local loop over object elements", localLoop());

function argLoop(a) {
  var i, s = 0;
  for (i = 0; i < 500; i++) s += a[i];
  return s;
}
var nums = [];
for (var j = 0; j < 500; j++) nums[j] = j;
show("arg loop sum", argLoop(nums));

function stringLoop() {
  var a = [], i, s = "";
  for (i = 0; i < 200; i++) a[i] = "s" + i;
  for (i = 0; i < 200; i++) s += a[i].length;
  return s.length;
}
show("local loop over string elements", stringLoop());

/* ----------------------------- the retired short form: set_var_ref index 3 */

// put_short_code() lost `OP_set_var_ref0 + 3` when OP_set_var_ref3 was retired
// for the new opcode; index 3 now takes the 3-byte general OP_set_var_ref.
function varRefIndexThree() {
  var a = 1, b = 2, c = 3, d = 4;
  function set() { a = 10; b = 20; c = 30; d = 40; }
  set();
  return "" + a + " " + b + " " + c + " " + d;
}
show("closure write at var_ref index 3", varRefIndexThree());

function varRefManyIndices() {
  var v0 = 0, v1 = 0, v2 = 0, v3 = 0, v4 = 0, v5 = 0;
  function set() { v0 = 1; v1 = 2; v2 = 3; v3 = 4; v4 = 5; v5 = 6; }
  set();
  return [v0, v1, v2, v3, v4, v5].join(",");
}
show("closure writes at six indices", varRefManyIndices());

/* ---------- a captured local IS reachable from user code, and is borrowed */

// The receiver lives in var_buf even when a nested closure captures it (the
// JSVarRef points into the frame), so this is the local-receiver analogue of
// patch 0023's argument case and the reason the borrow must be fast-path only.
function capturedReceiver() {
  var o = [1, 2, 3];
  var clear = function () { o = null; };
  var k = { toString: function () { clear(); return "0"; } };
  return o[k];
}
show("captured local receiver cleared", capturedReceiver());
