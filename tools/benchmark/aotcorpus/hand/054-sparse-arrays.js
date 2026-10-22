var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 9;
var t = 0;
for (var r = 0; r < N; r++) {
  var a = [];
  a[100000 + r] = 1;
  a[5] = 2;
  a[r] = 3;
  t += a.length;
  var ks = Object.keys(a);
  t += ks.length;
  var c = 0;
  for (var k in a) c++;
  t += c;
  a.forEach(function (v) { t += v; });
  t += a.filter(function () { return true; }).length;
  t += a.indexOf(1) > 0 ? 1 : 0;
}
mix(t);
print('sparse-arrays ' + h);
