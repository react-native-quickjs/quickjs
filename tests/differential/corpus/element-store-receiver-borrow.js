// UNIT 3b dataflow-window receiver borrow, the element STORE half (patch 0108).
//
// `get_loc(a) <window> put_array_el` becomes `<window> put_el_recv(a)`: the
// receiver's push is deleted and OP_put_el_recv reads the frame slot borrowed.
// The store differs from the load (patch 0104) in three ways that this file
// aims at specifically:
//
//   1. DEPTH IS 2, NOT 1.  `a[k] = v` pushes the receiver first and pops
//      three, so the key AND the value are computed inside the window.  That
//      makes the windows much longer than the load's, and it makes TWO wrong
//      anchors available instead of one: the key sits at depth 1 and the value
//      at depth 0.  Cases 1-8.
//   2. THE WINDOW MUST NOT REWRITE THE SLOT.  The original reads the receiver
//      before the key and the value; the rewrite reads it after both.  Cases
//      9-15 write the slot from inside the window by every route.
//   3. THE SLOW PATH RUNS USER CODE MORE OFTEN THAN THE LOAD'S DOES.
//      JS_SetPropertyValue can reach a setter, a Proxy `set` trap, a key's
//      toString or a valueOf, and any of those can drop references. Cases
//      16-24.  A store also FREES the value it displaces, which can drop the
//      last reference to an arbitrary object graph while the receiver is
//      borrowed: cases 25-27.
//
// Every case prints, so a wrong receiver shows up as a wrong value rather than
// as a crash.  Run against node byte-for-byte.

function out(label, v) { print(label + " = " + String(v)); }

// ---------------------------------------------------------------- 1-8 depth

// 1. plain: local receiver, computed key, computed value.  The case the patch
//    exists for.
function c1(n) {
  var a = [0, 0, 0, 0];
  for (var i = 0; i < n; i++) a[i % 4] = i * 2 + 1;
  return a.join(",");
}
out("1 local recv, computed key and value", c1(8));

// 2. argument receiver.  Every ordinary function has an `arguments` binding,
//    which is what patch 0106 stopped refusing on; without it this case is
//    silently unfused and proves nothing.
function c2(a, n) {
  for (var i = 0; i < n; i++) a[i + 0] = i + 100;
  return a.join(",");
}
out("2 arg recv", c2([0, 0, 0], 3));

// 3. the KEY is the local, not the receiver.  Depth 1 at the consumer.
function c3(o) {
  var k = 1;
  o[k] = "K";
  return o.join(",");
}
out("3 key is the local", c3([7, 8, 9]));

// 4. the VALUE is the local, not the receiver.  Depth 0 at the consumer.
function c4(o) {
  var v = "V";
  o[1] = v;
  return o.join(",");
}
out("4 value is the local", c4([7, 8, 9]));

// 5. nested: a[b[i]] = c[j].  Three candidate anchors, only the outer receiver
//    is at depth 2, and the nested element LOAD inside the window is what
//    forced the window walker to stop treating a depth mismatch as fatal.
function c5() {
  var a = [0, 0, 0, 0];
  var b = [3, 2, 1, 0];
  var c = [9, 8, 7, 6];
  var i = 1, j = 2;
  a[b[i]] = c[j];
  a[b[i + 1]] = c[j - 1];
  return a.join(",");
}
out("5 nested a[b[i]] = c[j]", c5());

// 6. the local is consumed by a field read first, so it is not the receiver of
//    the element store at all.
function c6() {
  var a = { v: [1, 2, 3] };
  var i = 2;
  a.v[i] = 30;
  return a.v.join(",");
}
out("6 a.v[i] = x", c6());

// 7. two element stores sharing one window prefix.
function c7() {
  var a = [1, 2, 3], b = [4, 5, 6], i = 0;
  a[i + 1] = 20;
  b[i + 2] = 60;
  return a.join(",") + "|" + b.join(",");
}
out("7 two stores", c7());

// 8. the store in EXPRESSION position, which emits OP_insert3 before the
//    consumer and must not be fused (insert3 is not a window opcode).
function c8() {
  var a = [1, 2, 3], i = 0;
  var r = (a[i + 1] = 99);
  return r + "|" + a.join(",");
}
out("8 store in expression position", c8());

// ------------------------------------------------- 9-15 the window writes it

// 9. the window assigns the receiver local directly, in the KEY.
function c9() {
  var a = [1, 2, 3];
  var b = [40, 50, 60];
  a[(a = b, 0)] = "X";
  return a.join(",") + "|" + b.join(",");
}
out("9 key rewrites the slot", c9());

// 10. the window assigns the receiver local in the VALUE, i.e. after the key.
function c10() {
  var a = [1, 2, 3];
  var b = [40, 50, 60];
  a[0] = (a = b, "X");
  return a.join(",") + "|" + b.join(",");
}
out("10 value rewrites the slot", c10());

// 11. same, on an argument slot.
function c11(a) {
  var b = [40, 50, 60];
  a[(a = b, 1)] = "X";
  return a.join(",") + "|" + b.join(",");
}
out("11 window rewrites the arg", c11([1, 2, 3]));

// 12. the window runs user code (valueOf) that assigns the receiver through a
//     closure.  The local is captured, so no static analysis may borrow it.
function c12() {
  var a = [1, 2, 3];
  var k = { valueOf: function () { a = [40, 50, 60]; return 0; } };
  a[k + 0] = "X";
  return a.join(",");
}
out("12 closure write from valueOf", c12());

// 13. the same through a mapped `arguments` object.  The mapped object must
//     ESCAPE to the user code and the receiver argument must NOT be captured,
//     or the closure test refuses the window first and this proves nothing
//     about the `arguments` test.
function c13(a, sink) {
  sink.args = arguments;
  var k = { valueOf: function () { sink.args[0] = [40, 50, 60]; return 0; } };
  a[k + 0] = "X";
  return a.join(",");
}
out("13 mapped arguments write", c13([1, 2, 3], {}));

// 14. a `with` block puts the slot behind an object binding.
function c14() {
  var a = [1, 2, 3];
  var scope = { j: 0 };
  with (scope) {
    a[j + 0] = "X";
  }
  return a.join(",");
}
out("14 with block", c14());

// 15. the window writes a DIFFERENT local.  This one must still be folded and
//     must still be right.
function c15() {
  var a = [1, 2, 3];
  var j;
  a[(j = 2, j - 1)] = j * 10;
  return a.join(",") + "|" + j;
}
out("15 window writes another local", c15());

// -------------------------------------- 16-24 the consumer's own slow path

// 16. a prototype SETTER that deletes every other reference to the receiver
//     while the store is in flight.  If the receiver were borrowed across
//     JS_SetPropertyValue without taking a reference this is a
//     use-after-free.
function c16() {
  var holder = { a: null };
  function Weird() {}
  Object.defineProperty(Weird.prototype, "boom", {
    set: function (v) { holder.a = null; this.seen = v; },
    get: function () { return this.seen; }
  });
  holder.a = new Weird();
  var a = holder.a;
  var k = "bo";
  a[k + "om"] = 99;
  return String(a.boom);
}
out("16 setter drops other refs", c16());

// 17. a Proxy receiver: every store goes through a trap that itself stores.
function c17() {
  var target = [1, 2, 3];
  var log = [];
  var p = new Proxy(target, {
    set: function (t, k, v) { log.push(String(k) + ":" + String(v)); t[k] = v; return true; }
  });
  var i = 1;
  p[i + 1] = "P";
  return target.join(",") + "|" + log.join(";");
}
out("17 proxy receiver", c17());

// 18. a key whose toString runs after the receiver has been read.
function c18() {
  var a = [1, 2, 3];
  var k = { toString: function () { return "zz"; } };
  a[k] = "field";
  return a.join(",") + "|" + a.zz;
}
out("18 key toString", c18());

// 19. a VALUE whose valueOf runs inside the window and reads the receiver.
function c19() {
  var a = [1, 2, 3];
  var v = { valueOf: function () { return a.length * 10; } };
  a[1] = v + 0;
  return a.join(",");
}
out("19 value valueOf reads the receiver", c19());

// 20. out of range, negative, fractional and string keys, and holes.
function c20() {
  var a = [1, , 3];
  var i;
  for (i = -1; i < 5; i++) a[i + 0] = i;
  a["1" + ""] = "s";
  a[0.5 + 0.5] = "f";
  return a.join(",") + "|" + a[-1] + "|" + a.length;
}
out("20 holes and odd keys", c20());

// 21. the receiver is not an object, in sloppy mode (silent) and in strict
//     mode (throws).
function c21() {
  var r = [];
  var n = 5, u = null, i = 0;
  n[i + 0] = 1;
  r.push(String(n[0]));
  try { u[i + 0] = 1; r.push("no throw"); } catch (e) { r.push(e.constructor.name); }
  return r.join("|");
}
out("21 non-object receivers", c21());

function c21s() {
  "use strict";
  var s = "abc", i = 0, r = [];
  try { s[i + 0] = "z"; r.push("no throw"); } catch (e) { r.push(e.constructor.name); }
  return r.join("|");
}
out("21s strict primitive receiver", c21s());

// 22. typed arrays (the R5_A inline arm), frozen arrays, sealed arrays, and an
//     array that shrinks between stores.
function c22() {
  var ta = new Int32Array([4, 5, 6]);
  var u8 = new Uint8Array([1, 2, 3]);
  var fr = Object.freeze([7, 8, 9]);
  var sh = [1, 2, 3, 4, 5];
  var i = 1;
  ta[i + 1] = 77;
  u8[i + 0] = 300;          // wraps to 44
  fr[i + 0] = 99;           // silently ignored in sloppy mode
  sh[i + 2] = 33;
  sh.length = 2;
  sh[i + 2] = 44;           // past the end: re-grows
  return ta.join(",") + "|" + u8.join(",") + "|" + fr.join(",") + "|" +
         sh.join(",") + "|" + sh.length;
}
out("22 typed / frozen / shrinking", c22());

// 23. appending exactly at the end, repeatedly: the array-growth fast arm.
function c23(n) {
  var a = [];
  for (var i = 0; i < n; i++) a[i + 0] = i * i;
  return a.length + "|" + a.join(",");
}
out("23 append arm", c23(6));

// 24. a getter on the KEY object that reassigns the local.  The local is
//     captured so the fusion is refused; the value must be the pre-window
//     receiver either way.
function c24() {
  var a = [1, 2, 3];
  var b = [7, 8, 9];
  var k = { valueOf: function () { a = b; return 0; } };
  a[k + 0] = "X";
  return a.join(",") + "|" + b.join(",");
}
out("24 key valueOf reassigns the local", c24());

// -------------------------- 25-27 the displaced value, and the throw path

// 25. the store frees the value it displaces, and that value holds the last
//     reference to a graph that points back at the receiver.  The receiver is
//     borrowed at that moment, so its own reference count is one lower than it
//     used to be; the frame slot is what keeps it alive.
function c25() {
  var a = [null, null];
  var i = 0;
  a[i + 0] = { back: a, tag: "first" };
  a[i + 0] = { back: a, tag: "second" };   // frees "first", which held `a`
  a[i + 1] = a[0].tag;
  return a[0].tag + "," + a[1] + "," + (a[0].back === a);
}
out("25 displaced value holds the receiver", c25());

// 26. an exception thrown inside the window, so the consumer never runs and
//     the deleted push must not leave the operand stack unbalanced.
function c26() {
  var a = [1, 2, 3];
  var bad = null;
  try {
    a[bad.x] = 1;
    return "no throw";
  } catch (e) {
    return "caught " + (a[0] + a[1] + a[2]);
  }
}
out("26 throw inside the window", c26());

// 27. an exception thrown by the store ITSELF, in strict mode, on a frozen
//     receiver.  The handler must not free a receiver it never owned.
function c27() {
  "use strict";
  var a = Object.freeze([1, 2, 3]);
  var i = 0;
  try {
    a[i + 0] = 9;
    return "no throw";
  } catch (e) {
    return e.constructor.name + "|" + a.join(",");
  }
}
out("27 store throws", c27());

// 28. a branch target between the receiver and the consumer: control can reach
//     the consumer without having executed the push, so it must be refused.
function c28(f) {
  var a = [1, 2, 3];
  a[f ? 0 : 2] = "X";
  return a.join(",");
}
out("28 branch in the window", c28(true) + " / " + c28(false));

// 29. the receiver is reassigned by the loop body between iterations.
function c29() {
  var a = [1, 2, 3];
  var seen = [];
  for (var i = 0; i < 3; i++) {
    a[i + 0] = i * 11;
    seen.push(a.join(","));
    a = [1, 2, 3];
  }
  return seen.join("|");
}
out("29 receiver reassigned per iteration", c29());

// 30. deep recursion through the fused opcode, so a handler that grew
//     JS_CallInternal's frame shows up as a different stack depth.
function c30(a, n) {
  if (n <= 0) return 0;
  a[n % 3] = n;
  return a[n % 3] + c30(a, n - 1);
}
out("30 recursion", c30([0, 0, 0], 30));

// 31. `arguments` itself as the receiver of a store.
function c31() {
  var i = 0;
  arguments[i + 1] = "Y";
  return String(arguments[1]) + "," + arguments.length;
}
out("31 arguments receiver", c31("x", "y", "z"));

// ------------------------------------- 32-34 the refcount uniqueness oracle

// Reference counting is a UNIQUENESS ORACLE here: `js_accum_append` mutates a
// JSString in place when its refcount equals `1 + stack_ref`.  Deleting the
// receiver's push lowers a value's refcount for the whole window, so a window
// that also appends to the SAME value could tip that test.  The
// refuse-if-the-window-writes-the-receiver-slot check is what closes this.

// 32. the window appends to the receiver local itself, in the key.
function c32() {
  var s = "abcdefghijklmnopqrstuvwxyz";
  var t = s;
  s[(s = s + "0123456789", 0)] = "Z";
  return s + "|" + t;
}
out("32 append into the receiver slot", c32());

// 33. `+=` on the receiver local, which is OP_add_loc.
function c33() {
  var s = "abcdefghijklmnopqrstuvwxyz";
  var t = s;
  s[(s += "0123456789", 1)] = "Z";
  return s + "|" + t;
}
out("33 add_loc on the receiver slot", c33());

// 34. the window appends to a DIFFERENT slot holding the same string as the
//     receiver local.  This one IS fused and must still be right.
function c34() {
  var a = ["abcdefghijklmnopqrstuvwxyz"];
  var u = a[0];
  a[(u = u + "0123456789", 0)] = u.length;
  return a.join(",") + "|" + u;
}
out("34 append into another slot", c34());
