var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 63415;
function f(a, b = a * 2, ...rest) { return a + b + rest.length + rest.reduce(function (p, c) { return p + c; }, 0); }
function g(x = f(1), y = x + 1) { return x * y; }
var tot = 0;
for (var i = 0; i < N; i++) {
  tot = (tot + f(i) + f(i, 1) + f(i, undefined, 1, 2, 3) + g() + g(i % 7) + g(i % 3, i % 5)) | 0;
}
mix(tot);
mix(f.length); mix(g.length);
print('default-rest-params ' + h);
