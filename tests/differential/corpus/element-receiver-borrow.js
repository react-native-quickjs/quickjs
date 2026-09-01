// UNIT 3 dataflow-window receiver borrow (patch 0085).
//
// `get_loc(a) <window> get_array_el` becomes `<window> get_loc8_el_recv(a)`:
// the receiver's push is deleted and the consumer reads the frame slot
// borrowed.  Three things can break, and every case below aims at one of them.
//
//   1. DEPTH.  The simulated depth must be exactly 1 at the consumer, or the
//      slot being folded is the KEY (or something deeper) rather than the
//      receiver.  Cases 1-6, 20-22.
//   2. THE SLOT MUST NOT BE REWRITTEN BY THE WINDOW.  The original reads the
//      receiver BEFORE the key is computed; the rewrite reads it after.  Cases
//      7-13 write the slot from inside the window, directly and through every
//      indirect route (closure, mapped `arguments`, eval, `with`).
//   3. THE SLOW PATH MUST NOT USE A BORROWED RECEIVER ACROSS USER CODE.
//      Cases 14-19 make the element load itself run JavaScript -- a getter, a
//      Proxy trap, a key's toString -- and that code drops references.
//
// Every case prints, so a wrong receiver shows up as a wrong value rather than
// as a crash.  Run against node byte-for-byte.

function out(label, v) { print(label + " = " + String(v)); }

// ---------------------------------------------------------------- 1-6 depth

// 1. plain: local receiver, computed key.  The case the patch exists for.
function c1(n) {
  var a = [10, 20, 30, 40];
  var t = 0;
  for (var i = 0; i < n; i++) t += a[(i % 4)];
  return t;
}
out("1 local recv, computed key", c1(8));

// 2. argument receiver, computed key.
function c2(a, n) {
  var t = 0;
  for (var i = 0; i < n; i++) t += a[i + 1 - 1];
  return t;
}
out("2 arg recv, computed key", c2([1, 2, 3], 3));

// 3. the KEY is the local, not the receiver.  Depth 0 at the consumer; folding
//    it would read the key slot as the receiver.
function c3(o) {
  var k = 1;
  return o[k];
}
out("3 key is the local", c3([7, 8, 9]));

// 4. nested: a[b[i]].  Two candidate receivers, only the outer one matches.
function c4() {
  var a = [5, 6, 7, 8];
  var b = [3, 2, 1, 0];
  var i = 1;
  return a[b[i]] + "," + a[b[i + 1]];
}
out("4 nested a[b[i]]", c4());

// 5. the local is consumed by a field read before the element load, so it is
//    not the receiver of the element load at all.
function c5() {
  var a = { v: [1, 2, 3] };
  var i = 2;
  return a.v[i];
}
out("5 a.v[i]", c5());

// 6. two element loads sharing one window prefix.
function c6() {
  var a = [1, 2, 3], b = [4, 5, 6], i = 0;
  return a[i + 1] + b[i + 2];
}
out("6 two loads", c6());

// ------------------------------------------------- 7-13 the window writes it

// 7. the window assigns the receiver local directly, in a comma expression.
function c7() {
  var a = [1, 2, 3];
  var b = [40, 50, 60];
  return a[(a = b, 0)];
}
out("7 window rewrites the slot", c7());

// 8. same, on an argument slot.
function c8(a) {
  var b = [40, 50, 60];
  return a[(a = b, 1)];
}
out("8 window rewrites the arg", c8([1, 2, 3]));

// 9. the window runs user code (valueOf) that assigns the receiver through a
//    closure.  The local is captured, so no static analysis may borrow it.
function c9() {
  var a = [1, 2, 3];
  var k = { valueOf: function () { a = [40, 50, 60]; return 0; } };
  return a[k + 0];
}
out("9 closure write from valueOf", c9());

// 10. the same through a mapped `arguments` object on a non-strict function
//     with a simple parameter list.  NOTE THE SHAPE: the mapped object must
//     ESCAPE to the user code, and the receiver argument `a` must NOT be
//     captured -- otherwise the closure test refuses the window first and this
//     case proves nothing about the `arguments` test.  `arguments[0] = ...`
//     inside the inner function would name the INNER function's arguments and
//     could not reach `a` at all; the first version of this case made exactly
//     that mistake and the mutant survived it.
function c10(a, sink) {
  sink.args = arguments;
  var k = { valueOf: function () { sink.args[0] = [40, 50, 60]; return 0; } };
  return a[k + 0];
}
out("10 mapped arguments write", c10([1, 2, 3], {}));

// 11. a direct eval in the receiver's own function.  This is DEFENCE IN DEPTH
//     and the corpus cannot discriminate it: the window admits no call opcode,
//     so the only user code it can run is somebody else's valueOf/getter, and
//     that code can reach `a` only by capturing it -- which case 9 covers.
//     Kept because the window's opcode set is the thing that makes the
//     argument true, and that set is the part most likely to be widened.
function c11() {
  var a = [1, 2, 3];
  var b = [40, 50, 60];
  var k = { valueOf: function () { return 0; } };
  eval("void 0");
  var r = a[k + 0];
  return r + "," + b[0];
}
out("11 direct eval present", c11());

// 12. a `with` block puts the slot behind an object binding.
function c12() {
  var a = [1, 2, 3];
  var scope = { j: 0 };
  with (scope) {
    return a[j + 0];
  }
}
out("12 with block", c12());

// 13. the window writes a DIFFERENT local -- this one must still be folded and
//     must still be right.
function c13() {
  var a = [1, 2, 3];
  var t = 0;
  var j;
  return a[(j = 2, j - 1)] + (j === 2 ? 100 : 0) + t;
}
out("13 window writes another local", c13());

// -------------------------------------- 14-19 the consumer's own slow path

// 14. a prototype getter that deletes every other reference to the receiver
//     while the load is in flight.  If the receiver were borrowed across the
//     slow path without taking a reference, this is a use-after-free.
function c14() {
  var holder = { a: null };
  function Weird() {}
  Object.defineProperty(Weird.prototype, "boom", {
    get: function () { holder.a = null; return 99; }
  });
  holder.a = new Weird();
  var a = holder.a;
  var k = "bo";
  var v = a[k + "om"];
  return v;
}
out("14 getter drops other refs", c14());

// 15. a Proxy receiver: every load goes through a trap.
function c15() {
  var target = [1, 2, 3];
  var p = new Proxy(target, {
    get: function (t, k) { return k === "length" ? t.length : "P" + String(k); }
  });
  var i = 1;
  return p[i + 1];
}
out("15 proxy receiver", c15());

// 16. a key whose toString runs after the receiver has been read.
function c16() {
  var a = [1, 2, 3];
  a.zz = "field";
  var k = { toString: function () { return "zz"; } };
  return a[k];
}
out("16 key toString", c16());

// 17. holes, out of range, negative, fractional and string keys.
function c17() {
  var a = [1, , 3];
  var out2 = [];
  var i;
  for (i = -1; i < 4; i++) out2.push(String(a[i + 0]));
  out2.push(String(a["1" + ""]));
  out2.push(String(a[0.5 + 0.5]));
  return out2.join("|");
}
out("17 holes and odd keys", c17());

// 18. the receiver is not an object.
function c18() {
  var s = "abc", n = 5, u = null, i = 0;
  var r = [];
  r.push(String(s[i + 1]));
  r.push(String(n[i + 0]));
  try { r.push(String(u[i + 0])); } catch (e) { r.push(e.constructor.name); }
  return r.join("|");
}
out("18 non-object receivers", c18());

// 19. typed arrays, frozen arrays, and an array that shrinks between reads.
function c19() {
  var ta = new Int32Array([4, 5, 6]);
  var fr = Object.freeze([7, 8, 9]);
  var sh = [1, 2, 3, 4, 5];
  var i = 1;
  var r = [String(ta[i + 1]), String(fr[i + 0])];
  r.push(String(sh[i + 2]));
  sh.length = 2;
  r.push(String(sh[i + 2]));
  return r.join("|");
}
out("19 typed / frozen / shrinking", c19());

// ---------------------------------------------------- 20-22 control flow

// 20. an exception thrown inside the window, so the consumer never runs and
//     the deleted push must not leave the operand stack unbalanced.
function c20() {
  var a = [1, 2, 3];
  var bad = null;
  try {
    return a[bad.x];
  } catch (e) {
    return "caught " + (a[0] + a[1] + a[2]);
  }
}
out("20 throw inside the window", c20());

// 21. a branch target between the receiver and the consumer.  Control can
//     reach the consumer without having executed the push, so the window must
//     be refused.
function c21(f) {
  var a = [1, 2, 3];
  return a[f ? 0 : 2];
}
out("21 branch in the window", c21(true) + "," + c21(false));

// 22. the receiver is reassigned by a loop body between iterations.
function c22() {
  var a = [1, 2, 3];
  var t = 0;
  for (var i = 0; i < 3; i++) {
    t += a[i + 0];
    a = [10, 20, 30];
  }
  return t;
}
out("22 receiver reassigned per iteration", c22());

// 23. deep recursion through the fused opcode, so a handler that grew the
//     frame shows up as a different stack depth rather than as nothing.
function c23(a, n) {
  if (n <= 0) return 0;
  return a[n % 3] + c23(a, n - 1);
}
out("23 recursion", c23([1, 2, 3], 30));

// 24. `arguments` itself as the receiver.
function c24() {
  var i = 0;
  return arguments[i + 1];
}
out("24 arguments receiver", c24("x", "y", "z"));

// 25. a getter on the array's own index, reached through the slow path, that
//     reassigns the local.  The local is captured so the fusion is refused;
//     the value must be the pre-window receiver either way.
function c25() {
  var a = [1, 2, 3];
  var o = {};
  Object.defineProperty(o, "0", {
    get: function () { a = [7, 8, 9]; return 42; }
  });
  var k = { valueOf: function () { return 0; } };
  var first = a[k + 0];
  var second = o[k + 0];
  return first + "," + second + "," + a[0];
}
out("25 getter reassigns the local", c25());

// ------------------------------------- 26-28 the refcount uniqueness oracle

// Reference counting is a UNIQUENESS ORACLE here: `js_accum_append` mutates a
// JSString in place when its refcount equals `1 + stack_ref`, and
// `js_shape_prepare_update` clones a shape only when its refcount is not 1.
// Deleting the receiver's push lowers an object's refcount by one for the
// whole window, so a window that also runs a string append on the SAME value
// could tip that test.  The window's refuse-if-it-writes-the-receiver-slot
// check is what closes this, and these cases are what prove it.

// 26. the window appends to the receiver local itself.
function c26() {
  var s = "abcdefghijklmnopqrstuvwxyz";
  var t = s;
  var r = s[(s = s + "0123456789", 0)];
  return r + "|" + s + "|" + t;
}
out("26 append into the receiver slot", c26());

// 27. `+=` on the receiver local, which is OP_add_loc (stack_ref 0).
function c27() {
  var s = "abcdefghijklmnopqrstuvwxyz";
  var t = s;
  var r = s[(s += "0123456789", 1)];
  return r + "|" + s + "|" + t;
}
out("27 add_loc on the receiver slot", c27());

// 28. the window appends to a DIFFERENT slot holding the same string as the
//     receiver local.  This one is fused and must still be right.
function c28() {
  var s = "abcdefghijklmnopqrstuvwxyz";
  var u = s;
  var r = s[(u = u + "0123456789", 2)];
  return r + "|" + s + "|" + u;
}
out("28 append into another slot", c28());
