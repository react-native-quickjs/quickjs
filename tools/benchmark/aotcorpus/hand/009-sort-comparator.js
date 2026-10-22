var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 1201;
var seed = 7;
function rnd() { seed = (seed * 69069 + 1) >>> 0; return seed >>> 8; }
for (var i = 0; i < N; i++) {
  var a = [];
  for (var j = 0; j < 100; j++) a.push(rnd() % 1000);
  a.sort(function (x, y) { return y - x; });
  mix(a[0]); mix(a[50]); mix(a[99]);
  var b = a.map(String); b.sort();
  mixs(b[0] + b[99]);
}
print('sort-comparator ' + h);
