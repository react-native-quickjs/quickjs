var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 3595;
var compose = function () { var fs = arguments; return function (x) { for (var i = fs.length - 1; i >= 0; i--) x = fs[i](x); return x; }; };
var curry = function (f) { return function a(x) { return function (y) { return f(x, y); }; }; };
var addc = curry(function (a, b) { return a + b; });
var memo = function (f) { var c = {}; return function (n) { return n in c ? c[n] : (c[n] = f(n)); }; };
var fibm = memo(function (n) { return n < 2 ? n : fibm(n - 1) + fibm(n - 2); });
var t = 0;
for (var r = 0; r < N; r++) {
  var f = compose(addc(r), addc(1), function (x) { return x * 2; });
  var arr = Array.from({ length: 50 }, function (_, i) { return i; });
  t = (t + arr.map(f).reduce(function (p, c) { return p + c; }, 0)) | 0;
  t += fibm(30 + r % 10) % 1000;
}
mix(t);
print('reduce-heavy-functional ' + h);
