// Differential corpus for the static top-of-stack cache and closures.
//
// WHY THIS FILE.  A function that names an outer variable reads it through a
// var_ref, and the cache has to leave that alone; a function that is retained
// and called later must not see a cached value from the frame that created it.
// These shapes mix captured reads with cached arithmetic, and call one function
// from inside another while a cached value is live.

function show(k, v) { print(k + '=' + v); }

function counter(start) {
  var n = start;
  return function (step) {
    n = n + step;
    return n;
  };
}
var c = counter(10);
show('c1', c(1));
show('c2', c(2));
show('c3', c(3));

function captureTwo(a, b) {
  return function () {
    var t = a + b;
    return t * 2;
  };
}
show('captureTwo', captureTwo(3, 4)());

function nestedCalls(n) {
  function inner(x) { return x * x + 1; }
  var t = 0;
  for (var i = 0; i < n; i++) {
    t = t + inner(i);
  }
  return t;
}
show('nestedCalls', nestedCalls(5));

function makeAdders() {
  var out = [];
  for (var i = 0; i < 3; i++) {
    out.push((function (k) {
      return function (x) { return x + k; };
    })(i));
  }
  return out;
}
var adders = makeAdders();
show('adder0', adders[0](10));
show('adder1', adders[1](10));
show('adder2', adders[2](10));

function recursive(n) {
  if (n <= 1) return 1;
  return n * recursive(n - 1);
}
show('recursive', recursive(6));

function mutual(a) {
  function even(n) { return n === 0 ? true : odd(n - 1); }
  function odd(n) { return n === 0 ? false : even(n - 1); }
  return even(a) + ':' + odd(a);
}
show('mutual', mutual(7));

function retained(a) {
  var f = function () { return a + 1; };
  var g = function () { return f() * 2; };
  return g;
}
var r = retained(20);
show('retained', r());

function manyLocals(a) {
  var p = a, q = a + 1, s = a + 2, t = a + 3;
  var r = p + q;
  r = r * s;
  r = r - t;
  r = r + p - q;
  return r;
}
show('manyLocals', manyLocals(2));
