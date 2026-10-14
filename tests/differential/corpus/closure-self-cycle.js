/* Self-recursive closures: `var f = function(){ ... f ... }`.
   The closure owns a var_ref whose detached value is the closure itself, a
   two-node cycle reference counting cannot break on its own.

   ## COVERAGE FIRST
   The cases below separate the two conditions any local rule that breaks that
   cycle must test, because test262 discriminates only one of them:
     - the closure must OWN the var_ref (test262 catches its removal: 1328 vs
       1066 errors);
     - the closure's count must be 1, i.e. nothing outside the pair holds it.
       test262 does NOT catch removing that one -- it reads 1066/85408 with the
       baseline md5 -- so `escaped` and `boxed` below exist to. Without the
       count test they raise "TypeError: not a function", because the escaping
       function reads its own name as undefined. */

function dies() {                 /* the pair really is garbage at teardown */
    var f = function (n) { return n <= 0 ? 0 : 1 + f(n - 1); };
    return f(4);
}
print("dies:", dies());

function escaped() {              /* closure outlives the frame: count is 2 */
    var f = function (n) { return n <= 0 ? 0 : 1 + f(n - 1); };
    return f;
}
print("escaped:", escaped()(5));

function boxed() {                /* reachable through an object, not directly */
    var h = function (n) { return n <= 0 ? "done" : h(n - 1); };
    return { fn: h };
}
print("boxed:", boxed().fn(3));

function twice() {                /* two self-recursive closures in one frame */
    var a = function (n) { return n <= 0 ? "a" : a(n - 1); };
    var b = function (n) { return n <= 0 ? "b" : b(n - 1); };
    return a(2) + b(2);
}
print("twice:", twice());

function shared() {               /* one var_ref captured by two closures */
    var t = function () { return "t"; };
    var u = function () { return t(); };
    return u();
}
print("shared:", shared());

var kept = [];
for (var i = 0; i < 3; i++) {     /* a loop of them, some kept, some dropped */
    (function (k) {
        var self = function (n) { return n <= 0 ? k : self(n - 1); };
        if (k === 1) kept.push(self);
        self(2);
    })(i);
}
print("kept:", kept[0](2));

/* ---------- under deferred reference counting ---------- */

/* called 100,000 times in a loop: the cycle must be broken each iteration */
function spin() {
    var f = function (n) { return n <= 0 ? 0 : 1 + f(n - 1); };
    var s = 0;
    for (var i = 0; i < 100000; i++) s += f(1);
    return s;
}
print("spin:", spin() === 100000);

/* the same closure returned from its frame and called later (must survive) */
function makeAdder(base) {
    var f = function (n) { return n <= 0 ? base : 1 + f(n - 1); };
    return f;
}
var survive = makeAdder(10);
print("survive:", survive(3));
print("survive-later:", survive(0));

/* stored in an object, an array and a global, and called later */
function makeStored() {
    var f = function (n) { return n <= 0 ? "f" : f(n - 1); };
    return { o: f, a: [f] };
}
var stored = makeStored();
print("stored-object:", stored.o(2));
print("stored-array:", stored.a[0](2));
function makeGlobal() {
    var g = function (n) { return n <= 0 ? "g" : g(n - 1); };
    return g;
}
var globalFn = makeGlobal();
print("stored-global:", globalFn(2));

/* captured by a second closure */
function doubleCapture() {
    var f = function (n) { return n <= 0 ? "base" : f(n - 1); };
    var outer = function () { return f(2); };
    return outer;
}
print("double-capture:", doubleCapture()());

/* inside a generator */
function* genSelf() {
    var f = function (n) { return n <= 0 ? 0 : 1 + f(n - 1); };
    yield f(3);
    yield f(5);
}
var gs = genSelf();
print("gen-self:", gs.next().value, gs.next().value);

/* inside an async function (observed after the job queue drains) */
var asyncOut = null;
async function asyncSelf() {
    var f = function (n) { return n <= 0 ? 0 : 1 + f(n - 1); };
    await null;
    return f(4);
}
asyncSelf().then(function (v) { asyncOut = v; print("async-self:", v); },
                 function (e) { print("async-self-err:", e.message); });

/* two mutually recursive closures in one frame: NOT a self-cycle each, so the
   rule must simply leave them working */
function mutual() {
    var even = function (n) { return n === 0 ? true : odd(n - 1); };
    var odd = function (n) { return n === 0 ? false : even(n - 1); };
    return [even(10), odd(7)];
}
var m = mutual();
print("mutual:", m[0], m[1]);

/* a self-recursive closure that throws: the pair must still be broken */
function throwsSelf() {
    var f = function (n) { if (n > 0) { throw new Error("boom" + n); } return f; };
    try { f(1); return "no-throw"; } catch (e) { return e.message; }
}
print("throws-self:", throwsSelf());

/* a self-recursive closure whose body throws on the recursive path */
function throwsDeep() {
    var f = function (n) { if (n === 0) throw new Error("deep"); return f(n - 1); };
    try { f(3); return "no-throw"; } catch (e) { return e.message; }
}
print("throws-deep:", throwsDeep());

/* ---------- two closures sharing the self-referencing cell ---------- */

/* `g` captures the same cell as `f` and dies first. At `f`'s frame exit the
   count on the cell is not 1 (the parked `g` still holds it), so the count test
   must not be made there -- `js_closcyc_resolve` makes it once the parked values
   are drained. Run many times so a leak would accumulate. */
function two(n) {
    var f = function (k) { return k ? f(k - 1) : 0; };
    var g = function () { return f; };
    g();
    return f(n);
}
var s = 0;
for (var i = 0; i < 20000; i++) s += two(3);
print(s);

/* the closure escapes and is called after the frame is gone (must survive) */
function esc() {
    var f = function (k) { return k ? f(k - 1) + 1 : 0; };
    return f;
}
var keep = [];
for (var i = 0; i < 2000; i++) keep.push(esc());
var t = 0;
for (var i = 0; i < keep.length; i++) t += keep[i](5);
print(t);
