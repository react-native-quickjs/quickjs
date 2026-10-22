var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 572981;
var a = 0, b = 0.5, c = 1;
for (var i = 0; i < N; i++) {
  a += i % 7;
  b = b * 1.0000001 + 0.25;
  c = (c * 3) % 1000003;
  var m = (a + c) / 3;
  if (m == (m | 0)) a -= 1;
  a = a > 2147483647 ? a - 2147483647 : a;
  b = b > 1e9 ? 1 : b;
}
mix(a); mixd(b); mix(c);
print('bigloop-int-float-mix ' + h);
