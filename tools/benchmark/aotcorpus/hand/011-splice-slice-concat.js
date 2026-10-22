var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 11166;
var tot = 0;
for (var i = 0; i < N; i++) {
  var a = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
  var r = a.splice(2, 3, "a", "b");
  var c = a.concat(r, [i], 99);
  var d = c.slice(-4);
  var e = c.slice(1, -2);
  a.splice(1, 0, 0.5);
  a.splice(-1, 1);
  tot += c.length + d.length + e.length + a.length + r.length;
  mixs(a.join(",")); mixs(d);
}
mix(tot);
print('splice-slice-concat ' + h);
