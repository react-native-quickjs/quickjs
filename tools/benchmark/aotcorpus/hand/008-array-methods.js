var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 5050;
var tot = 0;
for (var i = 0; i < N; i++) {
  var a = [];
  for (var j = 0; j < 40; j++) a.push((j * 7 + i) % 23);
  var b = a.map(function (x) { return x * 2; }).filter(function (x) { return x % 3 != 0; });
  tot += b.reduce(function (p, c) { return p + c; }, 0);
  tot += a.indexOf(5) + (a.includes(22) ? 1 : 0);
  tot += a.some(function (x) { return x > 21; }) ? 1 : 0;
  tot += a.every(function (x) { return x >= 0; }) ? 1 : 0;
  tot += a.find(function (x) { return x > 15; }) + a.findLast(function (x) { return x < 5; }) + a.findIndex(function (x) { return x == 9; });
}
mix(tot);
print('array-methods ' + h);
