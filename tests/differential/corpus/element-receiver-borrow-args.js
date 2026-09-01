// UNIT 3a (patch 0106): the ARGUMENT half of the dataflow-window receiver
// borrow.
//
// Patch 0104 admitted `get_arg(a) <window> get_array_el` only when the window
// ran no user code at all, because js_u3_slot_private() refused on
// `fd->has_arguments_binding` -- a field that quickjs.c sets for every function
// that is not an arrow, whether or not `arguments` is ever mentioned.  0106
// drops that term and keeps the ones that describe a real aliasing channel.
//
// So this file is where the relaxation is either safe or wrong.  Every case is
// one of two kinds and says which:
//
//   MUST FUSE   -- an argument receiver whose slot no user code can reach.
//                  These are the population U3a buys; they are here so that a
//                  wrong VALUE (not a crash) shows up in the diff.
//   MUST REFUSE -- an argument receiver that IS reachable, through each of the
//                  four channels the predicate still tests: a mapped
//                  `arguments`, a direct eval, a `with`/var object, a closure.
//                  A relaxation that went one term too far returns the
//                  POST-window value of the slot here instead of the pre-window
//                  one, which is an observable wrong answer.
//
// The two hazards named in 0106's header, and where each is pinned:
//   H1 a mapped `arguments` object aliases arg_buf ......... a4 a5 a6 a7 a14
//   H2 direct eval can introduce a reference nothing sees .. a8 a9
//
// Run against node byte-for-byte.

function out(label, v) { print(label + " = " + String(v)); }

// ------------------------------------------------------- MUST FUSE: the shape

// a1. navierstokes' actual shape: `x[++i]` with an argument receiver.  This is
//     the 900,704,160 executions patch 0104 refused.
function a1(x, n) {
  var t = 0, i = -1;
  for (var j = 0; j < n; j++) t += x[++i];
  return t;
}
out("a1 x[++i] arg recv", a1([1, 2, 3, 4, 5], 5));

// a2. the other measured shape: `x[i + j]`, two locals and an add.
function a2(x, n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    var j = 1;
    t += x[i + j];
  }
  return t;
}
out("a2 x[i+j] arg recv", a2([1, 2, 3, 4, 5], 4));

// a3. the same in a function that ALSO has a sibling using `arguments`.  The
//     flag is per-function; a sibling must not matter.
function a3sib() { return arguments.length; }
function a3(x) { var i = 0; return x[i + 2] + a3sib(1, 2, 3); }
out("a3 sibling uses arguments", a3([7, 8, 9]));

// ------------------------------------------- MUST REFUSE: mapped `arguments`

// a4. `arguments` escapes into a local and user code inside the window writes
//     the receiver slot through it.  The load must see the ORIGINAL array.
function a4(a) {
  var ar = arguments;
  var k = { valueOf: function () { ar[0] = [40, 50, 60]; return 0; } };
  return a[k + 0];
}
out("a4 mapped write through a local alias", a4([1, 2, 3]));

// a5. the mapped object is reached through a NESTED ARROW.  An arrow has no
//     `arguments` of its own, so this resolves to a5's -- and it is resolved
//     while the arrow is compiled, which is BEFORE a5's own resolve_labels()
//     runs the scan.  If that ordering were wrong the fusion would fire here.
function a5(a) {
  var get = () => arguments;
  var k = { valueOf: function () { get()[0] = [40, 50, 60]; return 0; } };
  return a[k + 0];
}
out("a5 mapped write through a nested arrow", a5([1, 2, 3]));

// a6. the mapped object is materialised by a CALLEE: it is passed out and the
//     write happens in a function that never saw the parameter name.
function a6writer(ar) { ar[0] = [40, 50, 60]; return 0; }
function a6(a) {
  var ar = arguments;
  var k = { valueOf: function () { return a6writer(ar); } };
  return a[k + 0];
}
out("a6 mapped write from a callee", a6([1, 2, 3]));

// a7. a mapped write to a DIFFERENT index than the receiver's.  Still refused
//     -- the predicate is per-function, not per-slot -- and still correct.
function a7(a, b) {
  var ar = arguments;
  var k = { valueOf: function () { ar[1] = [40, 50, 60]; return 0; } };
  return a[k + 0] + "," + b[0];
}
out("a7 mapped write to another slot", a7([1, 2, 3], [4, 5, 6]));

// -------------------------------------------------- MUST REFUSE: direct eval

// a8. a direct eval BUILDS the closure that writes the argument.  Nothing
//     static sees a reference to `a` from user code; only has_eval_call does.
function a8(a) {
  var k = eval("({ valueOf: function () { a = [40, 50, 60]; return 0; } })");
  return a[k + 0];
}
out("a8 eval-built writer", a8([1, 2, 3]));

// a9. a direct eval that names the argument by string, in a function whose
//     window otherwise looks completely private.
function a9(a) {
  var k = { valueOf: function () { return 0; } };
  eval("a = [40, 50, 60];");
  return a[k + 0];
}
out("a9 eval writes the slot", a9([1, 2, 3]));

// ------------------------------------------ MUST REFUSE: with / closure / put

// a10. `with` puts the key behind an object binding and the var object makes
//      the frame reachable.
function a10(a) {
  var scope = { j: 1 };
  with (scope) {
    return a[j + 0];
  }
}
out("a10 with block", a10([1, 2, 3]));

// a11. a closure captures the ARGUMENT and writes it from a valueOf.
function a11(a) {
  var k = { valueOf: function () { a = [40, 50, 60]; return 0; } };
  return a[k + 0];
}
out("a11 closure writes the arg", a11([1, 2, 3]));

// a12. the window writes the argument slot directly (OP_put_arg).
function a12(a) {
  var b = [40, 50, 60], i = 0;
  return a[(a = b, i + 1)];
}
out("a12 window rewrites the arg", a12([1, 2, 3]));

// a13. the argument is captured by an arrow in STRICT mode, where there is no
//      mapped `arguments` at all -- so is_captured is the only thing refusing.
function a13(a) {
  "use strict";
  var g = () => a;
  var k = { valueOf: function () { a = [40, 50, 60]; return 0; } };
  var r = a[k + 0];
  return r + "," + g()[0];
}
out("a13 strict captured arg", a13([1, 2, 3]));

// ------------------------------- MUST FUSE: unmapped `arguments` still refused
// These functions cannot alias arg_buf at all (strict, or a non-simple
// parameter list makes `arguments` unmapped).  0106 still refuses them, because
// the predicate tests the BINDING rather than the mapping; the cases are here
// so that a later relaxation of THAT has a correctness baseline to hit.

// a14. strict mode + `arguments`: unmapped, a copy, a write is invisible.
function a14(a) {
  "use strict";
  var ar = arguments;
  var k = { valueOf: function () { ar[0] = [40, 50, 60]; return 0; } };
  return a[k + 0] + "," + String(ar[0][0]);
}
out("a14 strict arguments is a copy", a14([1, 2, 3]));

// a15. a rest parameter makes the list non-simple, so `arguments` is unmapped.
function a15(a, ...rest) {
  var ar = arguments;
  var k = { valueOf: function () { ar[0] = [40, 50, 60]; return 0; } };
  return a[k + 0] + "," + rest.length;
}
out("a15 rest parameter", a15([1, 2, 3], 9, 9));

// ------------------------------------------- MUST FUSE: non-simple parameters

// a16. a default parameter.  Parameter expressions add a scope; the receiver
//      is still an ordinary argument slot.
function a16(a, b = 1) {
  var i = 0;
  return a[i + b];
}
out("a16 default parameter", a16([1, 2, 3]) + "," + a16([1, 2, 3], 2));

// a17. a destructured parameter ahead of the receiver.
function a17([p, q], a) {
  var i = 0;
  return a[i + 1] + p + q;
}
out("a17 destructured parameter", a17([10, 20], [1, 2, 3]));

// a18. the receiver is an argument BEYOND argc -- it is undefined and the load
//      must throw, with the operand stack balanced afterwards.
function a18(a, b) {
  var i = 0;
  try { return String(b[i + 1]); }
  catch (e) { return e.constructor.name + ":" + a[i + 0]; }
}
out("a18 missing argument receiver", a18([1, 2, 3]));

// ------------------------------------------------------ control flow and refs

// a19. an exception thrown inside the window on an argument receiver: the
//      deleted push must not leave the operand stack short.
function a19(a) {
  var bad = null;
  try { return a[bad.x]; }
  catch (e) { return "caught " + (a[0] + a[1] + a[2]); }
}
out("a19 throw in an arg window", a19([1, 2, 3]));

// a20. the uniqueness oracle on an argument slot: the window appends to a
//      DIFFERENT slot holding the same string as the receiver argument.
function a20(s) {
  var u = s;
  var r = s[(u = u + "0123456789", 2)];
  return r + "|" + s + "|" + u;
}
out("a20 append into another slot", a20("abcdefghijklmnopqrstuvwxyz"));

// a21. the same, appending INTO the receiver argument slot -- refused by the
//      write test, and the value must be the pre-window string's char.
function a21(s) {
  var t = s;
  var r = s[(s = s + "0123456789", 1)];
  return r + "|" + s + "|" + t;
}
out("a21 append into the receiver arg", a21("abcdefghijklmnopqrstuvwxyz"));

// a22. a getter reached through the argument receiver's slow path that drops
//      every other reference to it.  Borrowing across that without a reference
//      is a use-after-free; the value must still be right.
function a22(a, holder) {
  var k = "bo";
  holder.a = null;
  return a[k + "om"];
}
out("a22 getter drops other refs", (function () {
  function Weird() {}
  Object.defineProperty(Weird.prototype, "boom", {
    get: function () { return 99; }
  });
  var holder = { a: new Weird() };
  return a22(holder.a, holder);
})());

// a23. a Proxy argument receiver, so every load is a trap.
function a23(p) {
  var i = 1;
  return p[i + 1];
}
out("a23 proxy arg receiver", a23(new Proxy([1, 2, 3], {
  get: function (t, k) { return k === "length" ? t.length : "P" + String(k); }
})));

// a24. deep recursion through the fused opcode on an argument receiver.
function a24(a, n) {
  if (n <= 0) return 0;
  return a[n % 3] + a24(a, n - 1);
}
out("a24 recursion", a24([1, 2, 3], 30));

// a25. holes, out of range, negative, fractional and string keys, on an
//      argument receiver reached through a user-code window.
function a25(a) {
  var r = [], i;
  for (i = -1; i < 4; i++) r.push(String(a[i + 0]));
  r.push(String(a["1" + ""]));
  r.push(String(a[0.5 + 0.5]));
  return r.join("|");
}
out("a25 holes and odd keys", a25([1, , 3]));

// a26. typed array and frozen argument receivers.
function a26(ta, fr, sh) {
  var i = 1;
  var r = [String(ta[i + 1]), String(fr[i + 0]), String(sh[i + 2])];
  sh.length = 2;
  r.push(String(sh[i + 2]));
  return r.join("|");
}
out("a26 typed / frozen / shrinking",
    a26(new Int32Array([4, 5, 6]), Object.freeze([7, 8, 9]), [1, 2, 3, 4, 5]));

// a27. `arguments.length` only.  Patch 0048 elides the reification; the
//      binding is still resolved, so the window is still refused, and the
//      answer must not change either way.
function a27(a) {
  var n = arguments.length;
  var i = 0;
  return a[i + n - 1];
}
out("a27 arguments.length only", a27([1, 2, 3], 9, 9));

// a28. `arguments[k]` -- patch 0048's ELEM kind, which capture_var()s every
//      argument on the elided path.  Both the element read and the borrow
//      candidate are in the same function.
function a28(a, b) {
  var i = 0;
  var v = arguments[i + 1];
  return a[i + 1] + "," + String(v === b);
}
out("a28 arguments[k] elision", a28([1, 2, 3], "B"));

// a29. `f.apply(null, arguments)` -- patch 0048's APPLY kind.
function a29sum() { var t = 0; for (var i = 0; i < arguments.length; i++) t += arguments[i]; return t; }
function a29(a, x, y) {
  var i = 0;
  var s = a29sum.apply(null, arguments);
  return a[i + 1] + "," + (typeof s);
}
out("a29 apply(arguments)", a29([1, 2, 3], 4, 5));

// a30. a function whose parameter is reassigned OUTSIDE any window, then read
//      through a fused window.  Nothing refuses this and the answer is the
//      current value, not the entry value.
function a30(a) {
  a = [10, 20, 30];
  var i = 0;
  return a[i + 2];
}
out("a30 arg reassigned before the window", a30([1, 2, 3]));
