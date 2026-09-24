// Differential corpus for bytecode optimizer pass 2: value numbering, load
// forwarding, branch-arm merging, range narrowing and tiny-function inlining
// (patch 0113, the `js_opt_inline()` splice and the numbering pass).
//
// WHY THIS FILE.  Every case below is one the optimizer will *change* -- a
// call it will replace with the callee's body, a computation it will replace
// with a load of a local, a branch whose two arms it will merge -- and the
// question each one asks is whether the change is invisible.  A splice runs
// the callee's instructions in the caller's frame, and a numbering pass
// deletes a computation because it believes two values are the same value, so
// both are the kind of rewrite that is correct on every input the author had
// in mind and wrong on one they did not.
//
// The cases, and what each exists to catch:
//
//   A  a spliced body that throws: A1-A5 and N put the throw under a handler
//      in the same function, and A6 puts the try in a *caller* of the function
//      the splice is in, which is the case that actually runs optimized today.
//      A function that contains a `catch` is not optimized at all -- the pass
//      abandons a function for `OP_catch` the way it abandons one for `eval`,
//      because a handler can reach a local by a route the analysis does not
//      follow -- so a splice never happens in a frame that has one.  The
//      exception path is still the one a splice is most likely to get wrong:
//      a real call unwinds a frame of the callee's, and a splice has none.
//   B  the same helper called from a helper, so the inner splice lands in a
//      body that is itself about to be spliced.
//   C  argument evaluation order and count, with side effects in each
//      argument: a splice stores the arguments it finds on the stack, so an
//      order-preserving evaluation is an assumption it has to keep.
//   D  a helper that writes a caller local through its closure variable, and
//      a helper that reads a caller *argument* the same way.
//   E  a body that uses `arguments`, which must keep the call rather than be
//      spliced: `arguments` is the callee's and names its own frame.
//   F  a body that uses `this`, same reason.
//   G  a recursive helper, which must not be spliced into itself.
//   H  a property read, a call that changes that property, and the read
//      again: a numbering pass that treats a property read as a pure
//      computation of its operands would answer the second read with the
//      first, across the call that changed it.
//   I  a test that bounds a local, and a call that changes the local before
//      the load the bound would narrow: the narrowing is only valid while
//      nothing has written the local since the test.
//   J  two arms whose instructions are identical, which the merge turns into
//      one arm, and a second pair that differ by one store, which it must not.
//   K  `typeof` and the undefined tests, on a value the compiler can see the
//      type of and on one it cannot.
//   L  a loop whose counter a guard has already bounded.
//   M  minus zero, which is not an int32: `0 * -3` is -0 and `1 / -0` is
//      -Infinity, and folding it to the integer zero turns one into Infinity.
"use strict";

function show(k, v) {
  print(k + " = " + v);
}

/* ---- A: a spliced body that throws, into the caller's handler ---------- */

function caseA(o) {
  /* one local, written once, from a function literal: the callee the
     inliner is willing to name */
  var h = function h_a(v) { return v.x + 1; };
  var trace = [];
  var r = "unset";
  try {
    r = h(o);
    trace.push("no throw");
  } catch (e) {
    trace.push("caught " + (e instanceof TypeError));
    r = "threw";
  } finally {
    trace.push("finally");
  }
  return trace.join(",") + " | " + r;
}
show("A1 defined", caseA({ x: 41 }));
show("A2 undefined", caseA(undefined));
show("A3 null", caseA(null));
show("A4 number", caseA(7));

/* the finally overrides the result, and the throw is still caught */
function caseA2() {
  var h = function h_a2(v) { return v.x; };
  try {
    return "try " + h(undefined);
  } catch (e) {
    return "catch";
  } finally {
    return "finally";
  }
}
show("A5 finally wins", caseA2());

/* A6: the throw leaves the spliced function and is caught in its caller.
   `thrower` holds no handler, so it is optimized, and the call to `h` in it is
   the splice; `caseA6` holds the try, so it is not optimized. */
function thrower(o) {
  var h = function h_a6(v) { return v.x + 1; };
  return h(o) + 1;
}
function caseA6(o) {
  try {
    return "r" + thrower(o);
  } catch (e) {
    return "caught " + (e instanceof TypeError);
  }
}
show("A6 defined", caseA6({ x: 1 }));
show("A7 throw out", caseA6(undefined));
show("A8 throw out, string", caseA6("s"));

/* ---- B: a splice inside a body that is itself spliced ------------------ */

function caseB(n) {
  var inner = function inner_b(v) { return v * 2; };
  var outer = function outer_b(v) { return inner(v) + 1; };
  return outer(n);
}
show("B1 nested", caseB(20));

/* the inner one throws and the outer call is the one inside the try */
function caseB2() {
  var inner = function inner_b2(v) { return v.x; };
  var outer = function outer_b2(v) { return inner(v); };
  var r;
  try {
    r = outer(null);
  } catch (e) {
    r = "caught " + (e instanceof TypeError);
  }
  return r;
}
show("B2 nested throw", caseB2());

/* ---- C: argument order and count --------------------------------------- */

var cLog = [];
function cArg(tag, v) {
  cLog.push(tag);
  return v;
}
function caseC() {
  var h = function h_c(a, b, c) { return a * 100 + b * 10 + c; };
  cLog = [];
  var r = h(cArg("a", 1), cArg("b", 2), cArg("c", 3));
  return cLog.join(",") + " -> " + r;
}
show("C1 order", caseC());

/* fewer arguments than the body declares, and more: both must stay calls */
function caseC2() {
  var h = function h_c2(a, b) { return (a === undefined ? "u" : a) + "/" + (b === undefined ? "u" : b); };
  return h(1) + " " + h(1, 2);
}
show("C2 arity", caseC2());

/* ---- D: the body writes and reads the caller through its closure ------- */

function caseD() {
  var t = 0;
  var add = function add_d(v) { t = t + v; return t; };
  var out = [];
  for (var i = 1; i <= 4; i++) out.push(add(i));
  return out.join(",") + " t=" + t;
}
show("D1 closure write", caseD());

function caseD2() {
  var f = function f_d2(v, k) {
    var g = function g_d2(w) { k = k + w; return k; };
    return g(v);
  };
  return f(3, 10) + " " + f(3, 10);
}
show("D2 arg through closure", caseD2());

/* ---- E and F: bodies the inliner must refuse --------------------------- */

function caseE() {
  var h = function h_e(a, b) { return arguments.length + ":" + a + ":" + b; };
  return h(1) + " " + h(1, 2) + " " + h(1, 2, 3);
}
show("E1 arguments", caseE());

function caseF() {
  var h = function h_f() { return this === undefined ? "u" : this.tag; };
  var box = { tag: "box", m: h };
  return h.call(box) + " " + box.m() + " " + h.call(undefined);
}
show("F1 this", caseF());

function caseG() {
  var fact = function fact_g(n) { return n < 2 ? 1 : n * fact_g(n - 1); };
  return fact(6);
}
show("G1 recursion", caseG());

/* ---- H: a property read across a call that changes it ------------------ */

function caseH() {
  var obj = { x: 1 };
  var bump = function bump_h(o) { o.x = o.x + 1; return o.x; };
  var first = obj.x;
  var after = bump(obj);
  var second = obj.x;
  return first + "," + after + "," + second;
}
show("H1 read, call, read", caseH());

/* two reads of the same element with a call in between */
function caseH2() {
  var a = [1, 2, 3];
  var touch = function touch_h2(v) { a[0] = 9; return v; };
  var r1 = a[0];
  touch(0);
  var r2 = a[0];
  return r1 + "," + r2;
}
show("H2 element across call", caseH2());

/* ---- I: a bound is only valid until something writes the local --------- */

function caseI() {
  var k = 20;
  var reset = function reset_i() { k = 0; return 0; };
  var out = [];
  for (var i = 0; i < 2; i++) {
    if (k > 8) {
      reset();
      out.push(k - 1);
    } else {
      out.push("no");
    }
    k = 20;
  }
  return out.join(",");
}
show("I1 bound killed by a write", caseI());

/* the same test with nothing writing the local */
function caseI2() {
  var k = 20;
  var out = [];
  for (var i = 0; i < 2; i++) {
    if (k > 8) out.push(k - 1); else out.push("no");
    k = k - 4;
  }
  return out.join(",");
}
show("I2 bound holds", caseI2());

/* ---- J: arms the merge may and may not take ---------------------------- */

function caseJ(c) {
  var t = 0;
  if (c) {
    t = t + 5;
    t = t * 2;
  } else {
    t = t + 5;
    t = t * 2;
  }
  return t;
}
show("J1 identical arms", caseJ(true) + "," + caseJ(false));

function caseJ2(c) {
  var t = 0;
  if (c) {
    t = t + 5;
    t = t * 2;
  } else {
    t = t + 5;
    t = t * 3;
  }
  return t;
}
show("J2 arms differ", caseJ2(true) + "," + caseJ2(false));

/* an arm whose test has a side effect must keep the test */
var jLog = [];
function caseJ3(c) {
  jLog = [];
  var t = 0;
  if ((jLog.push("t"), c)) {
    t = 7;
  } else {
    t = 7;
  }
  return jLog.join(",") + ":" + t + "," + (function () {
    jLog = [];
    var u = 0;
    if ((jLog.push("t"), c)) { u = 7; } else { u = 7; }
    return jLog.join(",") + ":" + u;
  })();
}
show("J3 test kept", caseJ3(true) + " | " + caseJ3(false));

/* ---- K: typeof and the undefined tests --------------------------------- */

function caseK(v) {
  var out = [];
  out.push(typeof v);
  out.push(typeof v === "undefined");
  out.push(v === undefined);
  out.push(v == null);
  out.push(v === null);
  return out.join(",");
}
show("K1 undefined", caseK(undefined));
show("K2 null", caseK(null));
show("K3 string", caseK("s"));
show("K4 object", caseK({}));

function caseK2() {
  var h = function h_k(v) { return typeof v; };
  return h(undefined) + "," + h(null) + "," + h(1) + "," + h(function () {});
}
show("K5 typeof inlined body", caseK2());

/* ---- L: a loop the guard has bounded ----------------------------------- */

function caseL(n) {
  var s = 0;
  for (var i = 0; i < 5; i++) {
    if (n > 100) return "big";
    s = s + n;
  }
  return s;
}
show("L1 bounded", caseL(3) + "," + caseL(101) + "," + caseL(-2));

/* ---- M: minus zero ---------------------------------------------------- */

function neg(v) { return v * -3; }
show("M1 zero times negative", 1 / neg(0));
show("M2 positive", neg(2));
show("M3 double", 1 / (0.0 * -3));

function caseM4() {
  var h = function h_m(v) { return v * -1; };
  return 1 / h(0);
}
show("M4 inlined negate", caseM4());

/* ---- N: a throw inside a loop's try, after a splice -------------------- */

function caseN(o) {
  var h = function h_n(v) { return v.x; };
  var out = [];
  for (var i = 0; i < 3; i++) {
    try {
      out.push(h(o));
    } catch (e) {
      out.push("e" + (e instanceof TypeError));
    }
  }
  return out.join(",");
}
show("N1 loop", caseN(undefined) + " | " + caseN({ x: 5 }));

/* ---- O: source-vs-optimized agreement on the checksum this file prints -- */

var acc = 0;
function caseP() {
  var h = function h_p(a, b, c) { return (a || 0) + (b || 0) + (c || 0); };
  var s = 0;
  for (var i = 0; i < 100; i++) s = h(s, i, i % 7);
  return s;
}
acc = acc + caseP();
show("P1 loop sum", acc);
show("P2 done", "ok");
