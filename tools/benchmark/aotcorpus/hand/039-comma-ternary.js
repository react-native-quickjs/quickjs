var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 777424;
var tot = 0;
for (var i = 0, j = 10; i < N; i++, j--) {
  var a = (i & 1) ? (i & 2) ? 1 : 2 : (i & 4) ? 3 : 4;
  var b = (tot++, a * 2, a + 1);
  tot += i % 3 == 0 ? a : i % 3 == 1 ? b : j & 3;
  tot = tot > 1e9 ? tot - 1e9 : tot;
}
mix(tot);
print('comma-ternary ' + h);
