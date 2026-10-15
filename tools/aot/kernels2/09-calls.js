// Calls: direct, method, extra and missing arguments, recursion, returns.
function add(a, b) { return (a + b) | 0; }
function fact(n) { return n <= 1 ? 1 : (n * fact(n - 1)) | 0; }
function ret() { return; }
function many(a, b, c) { return (a | 0) + (b | 0) + (c | 0); }
function k(n) {
  var s = 0, i;
  for (i = 0; i < n; i++) {
    s = (s + add(i, 2)) | 0;
    s = (s + many(i, 1)) | 0;
    s = (s + (ret() === undefined ? 1 : 0)) | 0;
  }
  var o = { m: add };
  return [s, fact(10), o.m(4, 5), add(1), add(1, 2, 3)];
}
print(k(20000));
