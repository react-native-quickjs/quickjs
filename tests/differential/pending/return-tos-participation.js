// Differential fixture for OP_return participating in the top-of-stack cache
// (docs/round6-codegen.md, item 1).  The mechanism makes OP_return read its
// operand out of the cache register instead of forcing a spill, so every path
// that can reach `done:` / `done_generator:` with a value on the stack has to
// keep working, and the returned reference has to be transferred exactly once.
//
// The cases below were chosen ON PURPOSE from paths the author of the change
// did NOT have in mind while writing it: generators, async resumption,
// return-through-finally (which uses OP_gosub/OP_ret), derived-constructor
// return, return out of a for-of body (iterator close), return from a getter,
// a Proxy trap and a native trampoline, and returns whose value comes from a
// slow path that had already spilled.
function p(x) { print(x); }

// 1. plain returns of every tag
function r_undef() { return undefined; }
function r_null() { return null; }
function r_int() { return 42; }
function r_dbl() { return 1.5; }
function r_str() { return "s" + 1; }
function r_obj() { return { a: 1 }; }
function r_big() { return 2n ** 70n; }
function r_sym() { return typeof Symbol("k"); }
p([r_undef(), r_null(), r_int(), r_dbl(), r_str(), JSON.stringify(r_obj()),
   String(r_big()), r_sym()].join("|"));

// 2. return through finally: the value is parked while the finally block runs
function r_finally() { try { return "try"; } finally { p("finally ran"); } }
function r_finally_override() { try { return "try"; } finally { return "finally"; } }
function r_finally_loop() {
  for (var i = 0; i < 3; i++) { try { if (i === 1) return "at" + i; } finally { p("f" + i); } }
  return "never";
}
p(r_finally() + "/" + r_finally_override() + "/" + r_finally_loop());

// 3. return out of a for-of body -- the iterator must still be closed
var closed = 0;
function iter() {
  var n = 0;
  return { [Symbol.iterator]() { return this; },
           next() { return { value: n++, done: n > 5 }; },
           return () { closed++; return { done: true }; } };
}
function r_forof() { for (var v of iter()) { if (v === 2) return "stopped at " + v; } return "ran out"; }
p(r_forof() + " closed=" + closed);

// 4. generators: OP_return reaches done_generator, not done
function* g1() { yield 1; yield 2; return "gret"; }
var it = g1(), acc = [];
for (;;) { var s = it.next(); acc.push(s.value + ":" + s.done); if (s.done) break; }
p(acc.join(","));
function* g2() { try { yield 1; } finally { p("gen finally"); } return "g2ret"; }
var it2 = g2(); it2.next(); p(JSON.stringify(it2.return("early")));

// 5. async: the return value crosses a promise resolution
async function a1() { return "a1"; }
async function a2() { var v = await a1(); return v + "+a2"; }
a2().then(function (v) { p("async " + v); });

// 6. derived constructor return (OP_check_ctor_return)
class Base { constructor() { this.b = 1; } }
class D1 extends Base { constructor() { super(); return; } }
class D2 extends Base { constructor() { super(); return { z: 9 }; } }
p(JSON.stringify(new D1()) + "/" + JSON.stringify(new D2()));

// 7. return from a getter, a setter and a Proxy trap
var got = { get v() { return "getter"; }, set v(x) { this._x = x; } };
var prox = new Proxy({}, { get: function (t, k) { return "trap:" + String(k); } });
got.v = 7;
p(got.v + "/" + got._x + "/" + prox.anything);

// 8. return of a value produced by a slow path (string rope, array method)
function r_slow(n) { var s = ""; for (var i = 0; i < n; i++) s += i; return s; }
function r_native(a) { return a.map(function (x) { return x * 2; }).join("-"); }
p(r_slow(12) + "/" + r_native([1, 2, 3]));

// 9. deep recursion: every frame returns through the cache
function rec(n) { if (n === 0) return 0; return n + rec(n - 1); }
// 400 frames: deep enough to exercise the mechanism on every frame, shallow
// enough to stay inside the engine's ~850-frame recursion limit.
p("rec=" + rec(400));

// 10. return inside with(), inside eval, and a tail position
function r_with(o) { with (o) { return a + b; } }
function r_eval() { return eval("(function(){ return 'inner'; })()"); }
function r_tail(o) { return o.m(); }
p(r_with({ a: 3, b: 4 }) + "/" + r_eval() + "/" + r_tail({ m: function () { return "tail"; } }));

// 11. exceptions unwinding past a pending return value
function r_throw() { try { return (function () { throw new Error("boom"); })(); }
                     catch (e) { return "caught " + e.message; } }
p(r_throw());
