var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 49397;
var tot = 0;
for (var i = 0; i < N; i++) {
  var a = [1, , 3];
  a[6] = 7;
  tot += a.length;
  tot += (1 in a) ? 1 : 0;
  var c = 0;
  a.forEach(function () { c++; });
  tot += c;
  tot += a.indexOf(undefined) + 1;
  tot += a.findIndex(function (x) { return x === undefined; });
  a.length = 2;
  tot += a.length + (a[2] === undefined ? 1 : 0);
  a.length = i % 5;
  tot += Object.keys(a).length;
  a[10] = 1;
  tot += a.length;
  delete a[10];
  tot += a.length;
}
mix(tot);
print('array-holes ' + h);
