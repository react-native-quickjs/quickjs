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
