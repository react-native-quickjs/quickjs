var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 1128;
var base = [];
for (var i = 0; i < 100; i++) base.push(i);
var t = 0;
for (var r = 0; r < N; r++) {
  var a = base.map(function (x, i, arr) { return x * r + i + arr.length; });
  var s = 0;
  a.forEach(function (x) { s += x & 255; });
  t += s + a.reduceRight(function (p, c) { return (p * 3 + c) | 0; }, 1);
  t += a.filter(function (x, i) { return i % 7 == 0; }).length;
  t += a.findLastIndex(function (x) { return x % 2 == 0; });
  t += a.slice().sort(function (x, y) { return (y & 31) - (x & 31); })[0] & 31;
  t |= 0;
}
mix(t);
print('array-iteration-callbacks ' + h);
