var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 21516;
var tot = 0;
for (var i = 0; i < N; i++) {
  var m = new Map([["a", 1], ["b", 2], ["c", i]]);
  for (var [k, v] of m) { tot += k.charCodeAt(0) + v; }
  for (var k of m.keys()) tot += k.length;
  for (var v of m.values()) tot += v;
  for (var ch of "héllo wörld") tot += ch.charCodeAt(0);
  for (var [idx, e] of ["x", "y", "z"].entries()) tot += idx + e.length;
  for (var e of new Set([1, 2, 2, 3, i % 4])) tot += e;
  for (var idx in [5, 6, 7]) tot += +idx;
  for (var p in { a: 1, b: 2 }) tot += p.length;
}
mix(tot);
print('forof-collections ' + h);
