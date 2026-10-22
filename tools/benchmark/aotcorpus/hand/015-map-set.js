var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 215291;
var m = new Map(), s = new Set();
for (var i = 0; i < N; i++) {
  var k = (i * 31) % 1009;
  m.set(k, (m.get(k) || 0) + i);
  s.add(k % 100);
  s.add("k" + (k % 50));
  if (i % 17 == 0) m.delete((k + 1) % 1009);
  if (i % 29 == 0) s.delete(k % 100);
}
mix(m.size); mix(s.size);
var t = 0;
m.forEach(function (v, k) { t = (t + v * 3 + k) | 0; });
mix(t);
for (var e of s) mixs(e);
m.set(NaN, 1); m.set(NaN, 2); m.set(0, 1); m.set(-0, 5);
mix(m.get(NaN)); mix(m.get(0));
print('map-set ' + h);
