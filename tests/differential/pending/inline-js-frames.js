/*
 * Inline JS->JS call frames (docs/js-call-path.md): the state an inline frame
 * has to rebind, and the state it must NOT rebind.
 *
 * The mechanism stops recursing into JS_CallInternal for a qualifying JS->JS
 * call: it pushes the callee's frame into a runtime-owned region, rebinds the
 * interpreter's own variables, and re-enters the SAME dispatch loop.  Every
 * value that the JS_CallInternal PROLOGUE computes and the interpreter LOOP
 * reads therefore has to be rebound by hand, and one of them was missed:
 *
 *   `JSObject *p`, read by OP_special_object/HOME_OBJECT, is assigned once in
 *   the prologue -- which an inline frame never re-enters.  An inline callee
 *   read its CALLER's home object.  Symptom: 83 test262 failures, every one a
 *   private-class-method, private-field-brand or `super`-property case.
 *
 * That bug survived the whole differential corpus and all 15 Octane rows,
 * because no Octane row and no corpus file at the time used `super` or
 * `#private` through an inlined call.  This file closes that hole.
 *
 * It also covers the closure/var_ref surface, where a second bug lived: the
 * local `var_refs` (the CLOSURE's captured-variable array) and `sf->var_refs`
 * (the FRAME's JSVarRef list, walked by close_var_refs) are different things,
 * and recovering one from the other produced "TypeError: not a function".
 *
 * The class and closure constructs are deliberately written INSIDE functions,
 * not at top level: a top-level <eval> frame is never an inline frame, so the
 * top-level spelling of every case below passes even on a broken build.
 *
 *   node tests/differential/run.mjs inline-js-frames
 *   node tests/differential/run.mjs inline-js-frames --qjs <assert build>
 */

var out = [];
function log(name, v) { out.push(name + ' ' + v); }

/* ---- closure capture through an inline frame ---------------------------- */

function c1() { var x = { v: 7 }; return function () { return x.v; }; }
log('closure-local', c1()());

function c2(a) { return function () { return a + 1; }; }
log('closure-arg', c2(41)());

function c3() { var y = 5; var inner = function () { return y; }; y = 9; return inner(); }
log('closure-written-after-capture', c3());

function c4() { let z = 3; { let w = z + 1; return (function () { return w; })(); } }
log('closure-block-scoped', c4());

/* the capture must survive the defining frame being popped */
function c5() { var n = 0; return function () { return ++n; }; }
var c5f = c5();
log('closure-outlives-frame', c5f() + ',' + c5f() + ',' + c5f());

/* ---- home_object: private members and super, defined inside a function --- */

function h1() { class Q { #p = 11; get() { return this.#p; } } return new Q().get(); }
log('private-field', h1());

function h2() { class Q { #p() { return 13; } get() { return this.#p(); } } return new Q().get(); }
log('private-method', h2());

function h3() {
  class Q { static #s = 17; static get() { return Q.#s; } }
  return Q.get();
}
log('private-static', h3());

function h4() {
  class Q { #p() { return 1; } static has(o) { return #p in o; } }
  return Q.has(new Q()) + ',' + Q.has({});
}
log('private-brand-in', h4());

function h5() {
  var o = { __proto__: { m() { return 'proto'; } }, m() { return super.m(); } };
  return o.m();
}
log('super-method', h5());

function h6() {
  class A { m() { return 'A'; } }
  class B extends A { m() { return super.m() + 'B'; } }
  return new B().m();
}
log('super-class', h6());

function h7() {
  class A { constructor(x) { this.x = x; } }
  class B extends A { constructor() { super(3); this.y = 4; } }
  var b = new B();
  return b.x + ',' + b.y;
}
log('super-ctor', h7());

/* ---- arguments, rest and short calls, which read argc/argv --------------- */

function a1() { return arguments.length + ':' + arguments[0] + ',' + arguments[1]; }
function a1c() { return a1(5, 6, 7); }
log('arguments', a1c());

function a2(a, b) { return a + ',' + b; }          /* called with fewer args */
function a2c() { return a2(1); }
log('short-args', a2c());

function a3(a, ...rest) { return a + ':' + rest.join('-'); }
function a3c() { return a3(1, 2, 3, 4); }
log('rest', a3c());

function a4(a) { a = a + 1; return arguments[0]; }  /* mapped arguments */
function a4c() { return a4(10); }
log('mapped-arguments', a4c());

/* ---- exceptions unwinding through inline frames -------------------------- */

function e1() { throw new Error('boom'); }
function e2() { return e1(); }
function e3() { try { return e2(); } catch (err) { return 'caught:' + err.message; } }
log('throw-through-two-frames', e3());

function e4() { try { return e1(); } finally { /* finally re-entered */ } }
function e5() { try { return e4(); } catch (err) { return 'f:' + err.message; } }
log('throw-through-finally', e5());

function e6() {
  var out = [];
  try {
    (function () { (function () { throw 'deep'; })(); })();
  } catch (err) { out.push(err); }
  return out.join('');
}
log('throw-nested-iife', e6());

/* an exception must not leave the caller's operand stack unbalanced */
function e7() {
  var s = 0;
  for (var i = 0; i < 5; i++) {
    try { s += (function (k) { if (k === 3) throw 'x'; return k; })(i); }
    catch (err) { s += 100; }
  }
  return s;
}
log('throw-in-loop', e7());

/* ---- tail-position calls, which the mechanism inlines too ---------------- */

function t1(n) { if (n === 0) return 'done'; return t1(n - 1); }
log('self-tail-recursion', t1(200));

function t2(n) { return n === 0 ? 0 : t3(n - 1); }
function t3(n) { return n === 0 ? 0 : t2(n - 1) + 1; }
log('mutual-recursion', t2(100));

/* ---- generators and async, which must NEVER take the inline path --------- */

function g1() {
  function* g() { var a = yield 1; var b = yield a + 1; return b + 1; }
  var it = g();
  return it.next().value + ',' + it.next(10).value + ',' + it.next(20).value;
}
log('generator', g1());

function g2() {
  function* g() { try { yield 1; } finally { fin = 'ran'; } }
  var fin = '';
  var it = g();
  it.next();
  it.return(9);
  return fin;
}
log('generator-return-finally', g2());

/* ---- deep recursion still reaches a RangeError --------------------------- */

function d1() { var d = 0; function f() { d++; f(); } try { f(); } catch (e) { return e instanceof RangeError; } }
log('stack-overflow-is-rangeerror', d1());

/* ---- a callee reached both inline and from C must behave identically ----- */

function s1(x) { return this === undefined || this === null ? 'noThis:' + x : this.tag + ':' + x; }
function s1c() {
  var o = { tag: 'obj', m: s1 };
  return [s1(1), o.m(2), s1.call({ tag: 'C' }, 3), [4].map(s1)[0]].join('|');
}
log('same-callee-both-paths', s1c());

print(out.join('\n'));
